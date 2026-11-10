/* Single-pass compiler: Pratt parser that emits bytecode directly.
 *
 * The original built a full AST (with a 64 KiB arena, realloc-per-element arrays and a
 * 'peek_next_token_parser' that copied the whole lexer state) and then walked it again to emit
 * code, and its grammar needed global flags (allow_implicit_call) to disambiguate
 * `f x, y` calls.  Explicit call syntax `f(x, y)` makes the grammar context-free and lets
 * parsing + code generation happen in one pass. */
#include "easec_internal.h"

#define MAX_JUMPS 32

typedef struct LoopCtx {
    struct LoopCtx* enclosing;
    int breaks[MAX_JUMPS], nbreaks;
    int conts[MAX_JUMPS],  nconts;
} LoopCtx;

typedef struct Compiler {
    struct Compiler* enclosing;
    ObjFunction* function;
    LoopCtx* loop;
} Compiler;

typedef struct {
    EasecVM* vm;
    const char* name;
    Lexer lexer;
    Token current, previous;
    bool had_error, panic, assigned;
    int errors;
} Parser;

static Parser P;
static Compiler* C;

typedef enum {
    PREC_NONE, PREC_ASSIGN, PREC_OR, PREC_AND, PREC_EQUALITY, PREC_COMPARISON,
    PREC_TERM, PREC_FACTOR, PREC_UNARY, PREC_CALL
} Prec;

typedef void (*ParseFn)(bool can_assign);
typedef struct { ParseFn prefix, infix; Prec prec; } Rule;

static Rule* rule_for(TokenType t);
static void expression(void);
static void declaration(void);
static void block(void);
static void parse_precedence(Prec p);

/* ------------------------------------------------------------------ errors */
static void error_at(Token* t, const char* msg) {
    if (P.panic) return;
    P.panic = true; P.had_error = true; P.errors++;
    if (t->type == T_ERROR) ea_printf(P.vm, "%s:%d: Error: %s\n", P.name, t->line, t->start);
    else if (t->type == T_EOF) ea_printf(P.vm, "%s:%d: Error at end: %s\n", P.name, t->line, msg);
    else if (t->type == T_NEWLINE) ea_printf(P.vm, "%s:%d: Error at end of line: %s\n", P.name, t->line, msg);
    else ea_printf(P.vm, "%s:%d: Error at '%.*s': %s\n", P.name, t->line, t->length, t->start, msg);
}
static void error(const char* msg) { error_at(&P.previous, msg); }
static void error_cur(const char* msg) { error_at(&P.current, msg); }

/* ------------------------------------------------------------------ token plumbing */
static void advance(void) {
    P.previous = P.current;
    for (;;) {
        P.current = lexer_next(&P.lexer);
        if (P.current.type != T_ERROR) break;
        error_cur(P.current.start);
    }
}
static bool check(TokenType t) { return P.current.type == t; }
static bool match_tok(TokenType t) { if (!check(t)) return false; advance(); return true; }
static void consume(TokenType t, const char* msg) { if (check(t)) advance(); else error_cur(msg); }
static void skip_newlines(void) { while (check(T_NEWLINE)) advance(); }
static bool at_statement_end(void) { return check(T_NEWLINE) || check(T_EOF) || check(T_RBRACE); }
static void end_statement(void) {
    if (check(T_NEWLINE)) { advance(); return; }
    if (check(T_EOF) || check(T_RBRACE)) return;
    error_cur("Expect end of statement (newline)");
}

/* ------------------------------------------------------------------ emitting */
static Chunk* chunk(void) { return &C->function->chunk; }
static void emit(uint8_t b) { chunk_write(P.vm, chunk(), b, P.previous.line); }
static void emit_u16(uint16_t v) { emit((uint8_t)(v >> 8)); emit((uint8_t)v); }
static void emit_op_u16(OpCode op, uint16_t v) { emit(op); emit_u16(v); }

static uint16_t make_constant(Value v) {
    int i = chunk_add_constant(P.vm, chunk(), v);
    if (i > 0xFFFF) { error("Too many constants in one job"); return 0; }
    return (uint16_t)i;
}
static uint16_t ident_constant(const Token* t) { return make_constant(OBJ_VAL(string_intern(P.vm, t->start, t->length))); }

static int emit_jump(OpCode op) { emit(op); emit(0xFF); emit(0xFF); return chunk()->count - 2; }
static void patch_jump(int at) {
    int jump = chunk()->count - at - 2;
    if (jump > 0xFFFF) { error("Too much code to jump over"); return; }
    chunk()->code[at] = (uint8_t)(jump >> 8);
    chunk()->code[at + 1] = (uint8_t)jump;
}
static void emit_loop(int start) {
    emit(OP_LOOP);
    int off = chunk()->count - start + 2;
    if (off > 0xFFFF) error("Loop body too large");
    emit_u16((uint16_t)off);
}

/* ------------------------------------------------------------------ expressions */
static void number(bool ca) {
    (void)ca;
    if (P.previous.type == T_NUMBER) {
        uint64_t v = 0;
        for (int i = 0; i < P.previous.length; i++) {
            unsigned d = (unsigned)(P.previous.start[i] - '0');
            if (v > (UINT64_MAX - d) / 10 || v * 10 + d > (uint64_t)INT64_MAX) { error("Number too large"); return; }
            v = v * 10 + d;
        }
        emit_op_u16(OP_CONST, make_constant(INT_VAL((int64_t)v)));
    } else {
        double v = 0, scale = 1; bool frac = false;
        for (int i = 0; i < P.previous.length; i++) {
            char c = P.previous.start[i];
            if (c == '.') { frac = true; continue; }
            v = v * 10 + (c - '0');
            if (frac) scale *= 10;
        }
        emit_op_u16(OP_CONST, make_constant(DEC_VAL(v / scale)));
    }
}

static ObjString* unescape(const Token* t) {
    const char* s = t->start + 1; int n = t->length - 2;
    char* buf = malloc((size_t)n + 1);
    int len = 0;
    for (int i = 0; i < n; i++) {
        char c = s[i];
        if (c == '\\' && i + 1 < n) {
            char e = s[++i];
            switch (e) {
            case 'n': c = '\n'; break; case 't': c = '\t'; break; case 'r': c = '\r'; break;
            case '"': c = '"'; break;  case '\\': c = '\\'; break; case '0': c = '\0'; break;
            default: error("Unknown escape sequence (use \\n \\t \\r \\\" \\\\ \\0)"); c = e;
            }
        }
        buf[len++] = c;
    }
    ObjString* r = string_intern(P.vm, buf, len);
    free(buf);
    return r;
}

static void string(bool ca) { (void)ca; emit_op_u16(OP_CONST, make_constant(OBJ_VAL(unescape(&P.previous)))); }
static void literal(bool ca) {
    (void)ca;
    emit(P.previous.type == T_TRUE ? OP_TRUE : P.previous.type == T_FALSE ? OP_FALSE : OP_NULL);
}
static void grouping(bool ca) { (void)ca; skip_newlines(); expression(); skip_newlines(); consume(T_RPAREN, "Expect ')' after expression"); }

static void array_literal(bool ca) {
    (void)ca;
    int n = 0;
    skip_newlines();
    while (!check(T_RBRACKET) && !check(T_EOF)) {
        expression(); n++;
        skip_newlines();
        if (!match_tok(T_COMMA)) break;
        skip_newlines();
    }
    consume(T_RBRACKET, "Expect ']' after list elements");
    if (n > 0xFFFF) error("Too many list elements");
    emit_op_u16(OP_ARRAY, (uint16_t)n);
}

static void dict_literal(bool ca) {
    (void)ca;
    int n = 0;
    skip_newlines();
    while (!check(T_RBRACE) && !check(T_EOF)) {
        if (match_tok(T_IDENT)) emit_op_u16(OP_CONST, ident_constant(&P.previous));
        else if (match_tok(T_STRING)) string(false);
        else { error_cur("Expect a key (name or text)"); break; }
        consume(T_COLON, "Expect ':' after key");
        skip_newlines();
        expression(); n++;
        skip_newlines();
        if (!match_tok(T_COMMA)) break;
        skip_newlines();
    }
    consume(T_RBRACE, "Expect '}' after dictionary entries");
    if (n > 0xFFFF) error("Too many dictionary entries");
    emit_op_u16(OP_DICT, (uint16_t)n);
}

static bool is_assign_op(TokenType t) { return t == T_EQ || t == T_PLUSEQ || t == T_MINUSEQ || t == T_STAREQ || t == T_SLASHEQ; }

static void emit_compound_op(TokenType t) {
    emit(t == T_PLUSEQ ? OP_ADD : t == T_MINUSEQ ? OP_SUB : t == T_STAREQ ? OP_MUL : OP_DIV);
}

/* After the target has been compiled: handles `= expr` / `+= expr` ... Returns true if consumed.
 * `load_old` emits the code that pushes the current value of the target (for compound ops). */
static bool assignment(void (*load_old)(void*), void* arg, OpCode store, bool has_u16, uint16_t operand) {
    if (!is_assign_op(P.current.type)) return false;
    advance();
    TokenType op = P.previous.type;
    skip_newlines();
    if (op != T_EQ) load_old(arg);
    expression();
    if (op != T_EQ) emit_compound_op(op);
    if (has_u16) emit_op_u16(store, operand); else emit(store);
    P.assigned = true;
    return true;
}

static void load_var(void* a) { emit_op_u16(OP_GET_VAR, *(uint16_t*)a); }
static void load_prop(void* a) { emit(OP_DUP); emit_op_u16(OP_GET_PROP, *(uint16_t*)a); }
static void load_index(void* a) { (void)a; emit(OP_DUP2); emit(OP_GET_INDEX); }

static void variable(bool can_assign) {
    uint16_t name = ident_constant(&P.previous);
    if (can_assign && assignment(load_var, &name, OP_SET_VAR, true, name)) return;
    emit_op_u16(OP_GET_VAR, name);
}

static void dot(bool can_assign) {
    consume(T_IDENT, "Expect property name after '.'");
    uint16_t name = ident_constant(&P.previous);
    if (can_assign && assignment(load_prop, &name, OP_SET_PROP, true, name)) return;
    emit_op_u16(OP_GET_PROP, name);
}

static void index_expr(bool can_assign) {
    skip_newlines(); expression(); skip_newlines();
    consume(T_RBRACKET, "Expect ']' after index");
    if (can_assign && assignment(load_index, NULL, OP_SET_INDEX, false, 0)) return;
    emit(OP_GET_INDEX);
}

static void call(bool can_assign) {
    (void)can_assign;
    int argc = 0;
    skip_newlines();
    while (!check(T_RPAREN) && !check(T_EOF)) {
        expression(); argc++;
        skip_newlines();
        if (!match_tok(T_COMMA)) break;
        skip_newlines();
    }
    consume(T_RPAREN, "Expect ')' after arguments");
    if (argc > 255) error("Can't pass more than 255 arguments");
    emit(OP_CALL); emit((uint8_t)argc);
}

static void unary(bool ca) {
    (void)ca;
    TokenType op = P.previous.type;
    if (op == T_NOT) parse_precedence(PREC_EQUALITY);   /* `not a == b` means not (a == b) */
    else parse_precedence(PREC_UNARY);
    emit(op == T_NOT ? OP_NOT : OP_NEG);
}

static void binary(bool ca) {
    (void)ca;
    TokenType op = P.previous.type;
    skip_newlines();                                    /* allow a line break after an operator */
    parse_precedence((Prec)(rule_for(op)->prec + 1));
    switch (op) {
    case T_PLUS: emit(OP_ADD); break;     case T_MINUS: emit(OP_SUB); break;
    case T_STAR: emit(OP_MUL); break;     case T_SLASH: emit(OP_DIV); break;
    case T_PERCENT: emit(OP_MOD); break;  case T_EQEQ: emit(OP_EQ); break;
    case T_BANGEQ: emit(OP_EQ); emit(OP_NOT); break;
    case T_LT: emit(OP_LT); break;        case T_LE: emit(OP_LE); break;
    case T_GT: emit(OP_GT); break;        case T_GE: emit(OP_GE); break;
    default: break;
    }
}

/* short-circuit: result is the deciding operand, like Python/Lua */
static void and_expr(bool ca) {
    (void)ca;
    int end = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    skip_newlines();
    parse_precedence(PREC_AND + 1);
    patch_jump(end);
}
static void or_expr(bool ca) {
    (void)ca;
    int else_j = emit_jump(OP_JUMP_IF_FALSE);
    int end = emit_jump(OP_JUMP);
    patch_jump(else_j);
    emit(OP_POP);
    skip_newlines();
    parse_precedence(PREC_OR + 1);
    patch_jump(end);
}

static Rule rules[T_COUNT] = {
    [T_LPAREN]   = { grouping, call, PREC_CALL },
    [T_LBRACKET] = { array_literal, index_expr, PREC_CALL },
    [T_LBRACE]   = { dict_literal, NULL, PREC_NONE },
    [T_DOT]      = { NULL, dot, PREC_CALL },
    [T_MINUS]    = { unary, binary, PREC_TERM },
    [T_PLUS]     = { NULL, binary, PREC_TERM },
    [T_SLASH]    = { NULL, binary, PREC_FACTOR },
    [T_STAR]     = { NULL, binary, PREC_FACTOR },
    [T_PERCENT]  = { NULL, binary, PREC_FACTOR },
    [T_EQEQ]     = { NULL, binary, PREC_EQUALITY },
    [T_BANGEQ]   = { NULL, binary, PREC_EQUALITY },
    [T_LT] = { NULL, binary, PREC_COMPARISON }, [T_LE] = { NULL, binary, PREC_COMPARISON },
    [T_GT] = { NULL, binary, PREC_COMPARISON }, [T_GE] = { NULL, binary, PREC_COMPARISON },
    [T_AND]      = { NULL, and_expr, PREC_AND },
    [T_OR]       = { NULL, or_expr, PREC_OR },
    [T_NOT]      = { unary, NULL, PREC_NONE },
    [T_NUMBER]   = { number, NULL, PREC_NONE },
    [T_DECIMAL]  = { number, NULL, PREC_NONE },
    [T_STRING]   = { string, NULL, PREC_NONE },
    [T_TRUE] = { literal, NULL, PREC_NONE }, [T_FALSE] = { literal, NULL, PREC_NONE }, [T_NULL] = { literal, NULL, PREC_NONE },
    [T_IDENT]    = { variable, NULL, PREC_NONE },
};
static Rule* rule_for(TokenType t) { return &rules[t]; }

static void parse_precedence(Prec prec) {
    advance();
    ParseFn prefix = rule_for(P.previous.type)->prefix;
    if (!prefix) { error("Expect expression"); return; }
    bool can_assign = prec <= PREC_ASSIGN;
    prefix(can_assign);
    while (prec <= rule_for(P.current.type)->prec) {
        advance();
        rule_for(P.previous.type)->infix(can_assign);
    }
    if (is_assign_op(P.current.type)) {
        advance();
        error(can_assign ? "Invalid assignment target"
                         : "Assignment is not allowed here (it is a statement, not an expression)");
    }
}

/* Plain expressions bind tighter than assignment, so `if x = 1 {` and `say a = b` are errors
 * instead of silent assignments.  Only an expression STATEMENT may assign. */
static void expression(void) { parse_precedence(PREC_OR); }

/* ------------------------------------------------------------------ statements */
static void synchronize(void) {
    P.panic = false;
    while (!check(T_EOF)) {
        if (P.previous.type == T_NEWLINE) return;
        switch (P.current.type) {
        case T_VAR: case T_JOB: case T_SAY: case T_IF: case T_WHILE: case T_REPEAT:
        case T_OUT: case T_IMPORT: return;
        default: break;
        }
        advance();
    }
}

static void block(void) {
    consume(T_LBRACE, "Expect '{' to begin block");
    skip_newlines();
    while (!check(T_RBRACE) && !check(T_EOF)) { declaration(); skip_newlines(); }
    consume(T_RBRACE, "Expect '}' to end block");
}

static int parse_type(void) {
    consume(T_IDENT, "Expect a type name after ':'");
    static const struct { const char* n; EaType t; } types[] = {
        {"text", TY_TEXT}, {"number", TY_NUMBER}, {"decimal", TY_DECIMAL},
        {"boolean", TY_BOOLEAN}, {"list", TY_LIST}, {"dict", TY_DICT} };
    for (size_t i = 0; i < sizeof types / sizeof types[0]; i++)
        if ((int)strlen(types[i].n) == P.previous.length && memcmp(types[i].n, P.previous.start, (size_t)P.previous.length) == 0)
            return types[i].t;
    error("Unknown type (use text, number, decimal, boolean, list or dict)");
    return -1;
}

static void var_declaration(void) {
    consume(T_IDENT, "Expect variable name");
    uint16_t name = ident_constant(&P.previous);
    int type = -1;
    if (match_tok(T_COLON)) type = parse_type();
    if (match_tok(T_EQ)) { skip_newlines(); expression(); if (type >= 0) { emit(OP_CHECK_TYPE); emit((uint8_t)type); } }
    else emit(OP_NULL);                                   /* `var x` declares x = null */
    emit_op_u16(OP_DEF_VAR, name);
    end_statement();
}

static void job_declaration(void) {
    consume(T_IDENT, "Expect job name");
    Token name_tok = P.previous;
    uint16_t name = ident_constant(&name_tok);

    ObjFunction* fn = function_new(P.vm);
    fn->name = string_intern(P.vm, name_tok.start, name_tok.length);
    Compiler comp = { C, fn, NULL };
    C = &comp;

    consume(T_LPAREN, "Expect '(' after job name");
    ObjString* params[255]; int n = 0;
    skip_newlines();
    while (!check(T_RPAREN) && !check(T_EOF)) {
        consume(T_IDENT, "Expect parameter name");
        if (n == 255) { error("Can't have more than 255 parameters"); break; }
        params[n++] = string_intern(P.vm, P.previous.start, P.previous.length);
        skip_newlines();
        if (!match_tok(T_COMMA)) break;
        skip_newlines();
    }
    consume(T_RPAREN, "Expect ')' after parameters");
    fn->params = EA_ARRAY(P.vm, ObjString*, n ? n : 1);
    for (int i = 0; i < n; i++) fn->params[i] = params[i];
    fn->arity = n;
    if (!n) { EA_FREE_ARRAY(P.vm, ObjString*, fn->params, 1); fn->params = NULL; }

    skip_newlines();
    block();
    emit(OP_NULL); emit(OP_RETURN);
    C = comp.enclosing;

    emit_op_u16(OP_CLOSURE, make_constant(OBJ_VAL(fn)));
    emit_op_u16(OP_DEF_VAR, name);
    end_statement();
}

static void if_statement(void) {                         /* shared by `if` and `else if`; no end_statement here */
    expression();
    skip_newlines();
    int then_j = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    block();
    int else_j = emit_jump(OP_JUMP);
    patch_jump(then_j);
    emit(OP_POP);
    if (match_tok(T_ELSE)) {
        if (match_tok(T_IF)) if_statement();
        else { skip_newlines(); block(); }
    }
    patch_jump(else_j);
}

static void begin_loop(LoopCtx* l) { l->enclosing = C->loop; l->nbreaks = l->nconts = 0; C->loop = l; }
static void patch_list(const int* list, int n) { for (int i = 0; i < n; i++) patch_jump(list[i]); }

static void while_statement(void) {
    int start = chunk()->count;
    expression();
    skip_newlines();
    int exit_j = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    LoopCtx l; begin_loop(&l);
    block();
    patch_list(l.conts, l.nconts);                        /* `skip` lands right before the back-jump */
    emit_loop(start);
    patch_jump(exit_j);
    emit(OP_POP);                                         /* the false condition */
    patch_list(l.breaks, l.nbreaks);                      /* `stop` jumps here: condition already popped */
    C->loop = l.enclosing;
}

static void repeat_statement(void) {
    LoopCtx l;
    if (match_tok(T_FOREVER)) {
        skip_newlines();
        int start = chunk()->count;
        begin_loop(&l);
        block();
        patch_list(l.conts, l.nconts);
        emit_loop(start);
        patch_list(l.breaks, l.nbreaks);
        C->loop = l.enclosing;
        return;
    }
    expression();                                         /* counter stays on the stack */
    skip_newlines();
    int start = chunk()->count;
    emit(OP_DUP); emit_op_u16(OP_CONST, make_constant(INT_VAL(0))); emit(OP_GT);
    int exit_j = emit_jump(OP_JUMP_IF_FALSE);
    emit(OP_POP);
    begin_loop(&l);
    block();
    patch_list(l.conts, l.nconts);
    emit_op_u16(OP_CONST, make_constant(INT_VAL(1))); emit(OP_SUB);
    emit_loop(start);
    patch_jump(exit_j);
    emit(OP_POP);                                         /* condition */
    patch_list(l.breaks, l.nbreaks);                      /* `stop`: only the counter is left */
    emit(OP_POP);                                         /* counter */
    C->loop = l.enclosing;
}

static void jump_statement(bool is_stop) {
    if (!C->loop) { error(is_stop ? "'stop' outside of a loop" : "'skip' outside of a loop"); return; }
    int* list = is_stop ? C->loop->breaks : C->loop->conts;
    int* n = is_stop ? &C->loop->nbreaks : &C->loop->nconts;
    if (*n >= MAX_JUMPS) { error("Too many 'stop'/'skip' statements in one loop"); return; }
    list[(*n)++] = emit_jump(OP_JUMP);
}

static ObjString* module_alias(const Token* path_tok) {
    const char* s = path_tok->start + 1; int n = path_tok->length - 2;
    int begin = 0, end = n;
    for (int i = 0; i < n; i++) if (s[i] == '/') begin = i + 1;
    for (int i = n - 1; i >= begin; i--) if (s[i] == '.') { end = i; break; }
    return string_intern(P.vm, s + begin, end - begin);
}

static void import_statement(void) {
    consume(T_STRING, "Expect a module path (text) after 'import'");
    Token path_tok = P.previous;
    uint16_t path = make_constant(OBJ_VAL(unescape(&path_tok)));
    ObjString* alias;
    if (match_tok(T_AS)) { consume(T_IDENT, "Expect module alias"); alias = string_intern(P.vm, P.previous.start, P.previous.length); }
    else alias = module_alias(&path_tok);
    emit(OP_IMPORT); emit_u16(path); emit_u16(make_constant(OBJ_VAL(alias)));
    end_statement();
}

static void statement(void) {
    if (match_tok(T_SAY)) {
        if (at_statement_end()) emit_op_u16(OP_CONST, make_constant(OBJ_VAL(string_intern(P.vm, "", 0))));
        else expression();
        emit(OP_SAY); end_statement();
    } else if (match_tok(T_IF))     { if_statement(); end_statement(); }
    else if (match_tok(T_WHILE))    { while_statement(); end_statement(); }
    else if (match_tok(T_REPEAT))   { repeat_statement(); end_statement(); }
    else if (match_tok(T_OUT)) {
        if (at_statement_end()) emit(OP_NULL); else expression();
        emit(OP_RETURN); end_statement();
    } else if (match_tok(T_STOP))   { jump_statement(true); end_statement(); }
    else if (match_tok(T_SKIP))     { jump_statement(false); end_statement(); }
    else if (match_tok(T_IMPORT))   import_statement();
    else {
        P.assigned = false;
        parse_precedence(PREC_ASSIGN);
        if (!P.assigned) emit(OP_POP);                    /* expression statement: discard the value */
        end_statement();
    }
}

static void declaration(void) {
    if (match_tok(T_VAR)) var_declaration();
    else if (match_tok(T_JOB)) job_declaration();
    else statement();
    if (P.panic) synchronize();
}

ObjFunction* compile(EasecVM* vm, const char* source, const char* name) {
    Parser saved_p = P; Compiler* saved_c = C;           /* compile() is re-entrant for `import` */
    memset(&P, 0, sizeof P);
    P.vm = vm; P.name = name;
    lexer_init(&P.lexer, source);

    ObjFunction* fn = function_new(vm);
    fn->name = string_cstr(vm, "<script>");
    Compiler comp = { NULL, fn, NULL };
    C = &comp;

    advance();
    skip_newlines();
    while (!check(T_EOF) && P.errors < 20) { declaration(); skip_newlines(); }
    emit(OP_NULL); emit(OP_RETURN);

    bool ok = !P.had_error;
    P = saved_p; C = saved_c;
    return ok ? fn : NULL;
}
