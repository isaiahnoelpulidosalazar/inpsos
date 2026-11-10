#include "easec_internal.h"

/* Tokens point INTO the source (no per-token allocation).  The original allocated a copy of
 * every token in the AST arena and called strlen(source) inside lexer_peek_next(), i.e. on
 * every number/operator: O(n^2) lexing. */

void lexer_init(Lexer* lx, const char* source) { lx->start = lx->current = source; lx->line = 1; }

static bool is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static Token make(Lexer* lx, TokenType t) {
    return (Token){ t, lx->start, (int)(lx->current - lx->start), lx->line };
}
static Token error_token(Lexer* lx, const char* msg) {
    return (Token){ T_ERROR, msg, (int)strlen(msg), lx->line };
}
static bool match(Lexer* lx, char c) {
    if (*lx->current != c) return false;
    lx->current++;
    return true;
}

static const struct { const char* word; int len; TokenType type; } keywords[] = {
    {"say", 3, T_SAY}, {"var", 3, T_VAR}, {"job", 3, T_JOB}, {"out", 3, T_OUT},
    {"if", 2, T_IF}, {"else", 4, T_ELSE}, {"while", 5, T_WHILE}, {"repeat", 6, T_REPEAT},
    {"forever", 7, T_FOREVER}, {"stop", 4, T_STOP}, {"skip", 4, T_SKIP},
    {"import", 6, T_IMPORT}, {"as", 2, T_AS}, {"true", 4, T_TRUE}, {"false", 5, T_FALSE},
    {"null", 4, T_NULL}, {"and", 3, T_AND}, {"or", 2, T_OR}, {"not", 3, T_NOT},
};

static TokenType ident_type(const char* s, int len) {
    for (size_t i = 0; i < sizeof keywords / sizeof keywords[0]; i++)
        if (keywords[i].len == len && memcmp(keywords[i].word, s, (size_t)len) == 0) return keywords[i].type;
    return T_IDENT;
}

/* `note` comments.  The original treated ANY identifier starting with "note" (notes, note_count,
 * notebook ...) as a comment.  Now `note` must be a whole word, and is a reserved word.
 *   note this is a line comment
 *   note { block comment, nests { like this } }                                           */
static bool at_note(const Lexer* lx) {
    const char* p = lx->current;
    return p[0] == 'n' && p[1] == 'o' && p[2] == 't' && p[3] == 'e' && !is_alpha(p[4]) && !is_digit(p[4]);
}

/* Returns an error message, or NULL. */
static const char* skip_trivia(Lexer* lx) {
    for (;;) {
        char c = *lx->current;
        if (c == ' ' || c == '\t' || c == '\r') { lx->current++; }
        else if (at_note(lx)) {
            lx->current += 4;
            const char* p = lx->current;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '{') {
                int depth = 0;
                lx->current = p;
                do {
                    char d = *lx->current;
                    if (d == '\0') return "Unterminated block comment";
                    if (d == '{') depth++;
                    else if (d == '}') depth--;
                    else if (d == '\n') lx->line++;
                    lx->current++;
                } while (depth > 0);
            } else {
                while (*lx->current && *lx->current != '\n') lx->current++;
            }
        } else return NULL;
    }
}

Token lexer_next(Lexer* lx) {
    const char* err = skip_trivia(lx);
    lx->start = lx->current;
    if (err) return error_token(lx, err);
    char c = *lx->current;
    if (c == '\0') return make(lx, T_EOF);
    lx->current++;

    if (c == '\n') { Token t = make(lx, T_NEWLINE); lx->line++; return t; }
    if (is_alpha(c)) {
        while (is_alpha(*lx->current) || is_digit(*lx->current)) lx->current++;
        return make(lx, ident_type(lx->start, (int)(lx->current - lx->start)));
    }
    if (is_digit(c)) {
        while (is_digit(*lx->current)) lx->current++;
        if (*lx->current == '.' && is_digit(lx->current[1])) {
            lx->current++;
            while (is_digit(*lx->current)) lx->current++;
            return make(lx, T_DECIMAL);
        }
        return make(lx, T_NUMBER);
    }
    switch (c) {
    case '(': return make(lx, T_LPAREN);   case ')': return make(lx, T_RPAREN);
    case '{': return make(lx, T_LBRACE);   case '}': return make(lx, T_RBRACE);
    case '[': return make(lx, T_LBRACKET); case ']': return make(lx, T_RBRACKET);
    case ',': return make(lx, T_COMMA);    case ':': return make(lx, T_COLON);
    case '.': return make(lx, T_DOT);      case '%': return make(lx, T_PERCENT);
    case '+': return make(lx, match(lx, '=') ? T_PLUSEQ  : T_PLUS);
    case '-': return make(lx, match(lx, '=') ? T_MINUSEQ : T_MINUS);
    case '*': return make(lx, match(lx, '=') ? T_STAREQ  : T_STAR);
    case '/': return make(lx, match(lx, '=') ? T_SLASHEQ : T_SLASH);
    case '=': return make(lx, match(lx, '=') ? T_EQEQ : T_EQ);
    case '<': return make(lx, match(lx, '=') ? T_LE : T_LT);
    case '>': return make(lx, match(lx, '=') ? T_GE : T_GT);
    case '!': if (match(lx, '=')) return make(lx, T_BANGEQ); return error_token(lx, "Unexpected '!' (use 'not' or '!=')");
    case '"': {
        while (*lx->current != '"' && *lx->current != '\0') {
            if (*lx->current == '\\' && lx->current[1] != '\0') lx->current++;   /* skip escaped char */
            if (*lx->current == '\n') lx->line++;
            lx->current++;
        }
        if (*lx->current == '\0') return error_token(lx, "Unterminated string");
        lx->current++;
        return make(lx, T_STRING);
    }
    }
    return error_token(lx, "Unexpected character");
}
