#ifndef EASEC_INTERNAL_H
#define EASEC_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdarg.h>
#include "easec.h"
#include "easec_rt.h"

/* ------------------------------------------------------------------ values & objects */
typedef struct Obj Obj;

typedef enum { VAL_NULL, VAL_BOOL, VAL_INT, VAL_DECIMAL, VAL_OBJ } ValType;
typedef struct {
    ValType type;
    union { bool boolean; int64_t integer; double decimal; Obj* obj; } as;
} Value;

#define NULL_VAL        ((Value){ VAL_NULL,    { .integer = 0 } })
#define BOOL_VAL(b)     ((Value){ VAL_BOOL,    { .boolean = (b) } })
#define INT_VAL(i)      ((Value){ VAL_INT,     { .integer = (i) } })
#define DEC_VAL(d)      ((Value){ VAL_DECIMAL, { .decimal = (d) } })
#define OBJ_VAL(o)      ((Value){ VAL_OBJ,     { .obj = (Obj*)(o) } })
#define IS_NULL(v)      ((v).type == VAL_NULL)
#define IS_OBJ(v)       ((v).type == VAL_OBJ)
#define IS_NUMBER(v)    ((v).type == VAL_INT || (v).type == VAL_DECIMAL)
#define AS_NUMBER(v)    ((v).type == VAL_INT ? (double)(v).as.integer : (v).as.decimal)

typedef enum {
    OBJ_STRING, OBJ_ARRAY, OBJ_DICT, OBJ_FUNCTION, OBJ_CLOSURE, OBJ_NATIVE, OBJ_MODULE, OBJ_ENV
} ObjType;

struct Obj { ObjType type; bool marked; Obj* next; };

typedef struct { Obj obj; uint32_t hash; int length; char chars[]; } ObjString;

typedef struct { ObjString* key; Value value; } Entry;
typedef struct { int count; int capacity; Entry* entries; } Table;   /* open addressing, power-of-two capacity */

typedef struct { Obj obj; Value* items; int count; int capacity; } ObjArray;
typedef struct { Obj obj; Table table; } ObjDict;

typedef struct {
    uint8_t* code; int* lines; int count; int capacity;
    Value* constants; int const_count; int const_capacity;
} Chunk;

typedef struct { Obj obj; ObjString* name; int arity; ObjString** params; Chunk chunk; } ObjFunction;

typedef struct Env { Obj obj; Table vars; struct Env* parent; } Env;
/* A closure is an (immutable, shared) function plus the environment it was created in.
 * The original copied the Chunk struct by value into every closure, so the same code/constant
 * arrays were freed once per copy by the GC (double free). */
typedef struct { Obj obj; ObjFunction* fn; Env* env; } ObjClosure;

typedef bool (*NativeFn)(EasecVM* vm, int argc, Value* argv, Value* result);
typedef struct { Obj obj; NativeFn fn; const char* name; int arity; } ObjNative;   /* arity < 0: variadic */
typedef struct { Obj obj; ObjString* name; Env* env; } ObjModule;

#define AS_STRING(v)   ((ObjString*)(v).as.obj)
#define AS_ARRAY(v)    ((ObjArray*)(v).as.obj)
#define AS_DICT(v)     ((ObjDict*)(v).as.obj)
#define AS_CLOSURE(v)  ((ObjClosure*)(v).as.obj)
#define AS_NATIVE(v)   ((ObjNative*)(v).as.obj)
#define AS_MODULE(v)   ((ObjModule*)(v).as.obj)
static inline bool is_obj_type(Value v, ObjType t) { return v.type == VAL_OBJ && v.as.obj->type == t; }
#define IS_STRING(v)   is_obj_type((v), OBJ_STRING)
#define IS_ARRAY(v)    is_obj_type((v), OBJ_ARRAY)
#define IS_DICT(v)     is_obj_type((v), OBJ_DICT)

/* ------------------------------------------------------------------ bytecode */
typedef enum {
    OP_CONST,            /* u16 constant */
    OP_NULL, OP_TRUE, OP_FALSE, OP_POP, OP_DUP, OP_DUP2,
    OP_GET_VAR, OP_DEF_VAR, OP_SET_VAR,                /* u16 name constant; SET pops its value */
    OP_GET_PROP, OP_SET_PROP,                          /* u16 name constant */
    OP_GET_INDEX, OP_SET_INDEX,
    OP_EQ, OP_GT, OP_LT, OP_GE, OP_LE,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_NOT, OP_NEG,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_LOOP,                /* u16 offset */
    OP_CALL,                                           /* u8 argc */
    OP_RETURN,
    OP_CLOSURE,                                        /* u16 function constant */
    OP_ARRAY, OP_DICT,                                 /* u16 element / pair count */
    OP_SAY,
    OP_IMPORT,                                         /* u16 path constant, u16 alias constant */
    OP_CHECK_TYPE                                      /* u8 EaType */
} OpCode;

typedef enum { TY_TEXT, TY_NUMBER, TY_DECIMAL, TY_BOOLEAN, TY_LIST, TY_DICT } EaType;

/* ------------------------------------------------------------------ VM */
#define STACK_MAX   1024
#define FRAMES_MAX  128
#define GC_MIN_NEXT (256u * 1024u)

typedef struct { ObjClosure* closure; uint8_t* ip; Value* slots; Env* env; } CallFrame;

struct EasecVM {
    EasecHost host;
    CallFrame frames[FRAMES_MAX];
    int       frame_count;
    Value     stack[STACK_MAX];
    Value*    stack_top;

    Table     strings;            /* weak intern table */
    Table     modules;            /* path -> module */
    Obj*      objects;
    size_t    bytes_allocated, next_gc, max_bytes;
    int       gc_paused;
    bool      out_of_memory;

    Env*      builtins;
    Env*      globals;
    Obj**     gray;
    int       gray_count, gray_cap;
    int       import_depth;
};

/* ------------------------------------------------------------------ object.c */
void*       ea_realloc(EasecVM* vm, void* p, size_t old_size, size_t new_size);
#define EA_ARRAY(vm, T, n)         ((T*)ea_realloc((vm), NULL, 0, sizeof(T) * (size_t)(n)))
#define EA_GROW(vm, T, p, old, n)  ((T*)ea_realloc((vm), (p), sizeof(T) * (size_t)(old), sizeof(T) * (size_t)(n)))
#define EA_FREE_ARRAY(vm, T, p, n) ea_realloc((vm), (p), sizeof(T) * (size_t)(n), 0)

void        ea_printf(EasecVM* vm, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void        ea_write(EasecVM* vm, const char* s, size_t n);

Obj*        alloc_object(EasecVM* vm, size_t size, ObjType type);
ObjString*  string_intern(EasecVM* vm, const char* chars, int length);
ObjString*  string_cstr(EasecVM* vm, const char* s);
ObjArray*   array_new(EasecVM* vm);
void        array_push(EasecVM* vm, ObjArray* a, Value v);
ObjDict*    dict_new(EasecVM* vm);
ObjFunction* function_new(EasecVM* vm);
ObjClosure* closure_new(EasecVM* vm, ObjFunction* fn, Env* env);
ObjNative*  native_new(EasecVM* vm, const char* name, NativeFn fn, int arity);
ObjModule*  module_new(EasecVM* vm, ObjString* name, Env* env);
Env*        env_new(EasecVM* vm, Env* parent);

void        table_init(Table* t);
void        table_free(EasecVM* vm, Table* t);
bool        table_get(const Table* t, ObjString* key, Value* out);
Value*      table_slot(const Table* t, ObjString* key);
bool        table_set(EasecVM* vm, Table* t, ObjString* key, Value v);   /* true if key is new */
bool        table_delete(Table* t, ObjString* key);
int         table_live_count(const Table* t);

bool        env_lookup(const Env* env, ObjString* key, Value* out);
bool        env_assign(Env* env, ObjString* key, Value v);
void        env_define(EasecVM* vm, Env* env, ObjString* key, Value v);

void        chunk_init(Chunk* c);
void        chunk_free(EasecVM* vm, Chunk* c);
void        chunk_write(EasecVM* vm, Chunk* c, uint8_t byte, int line);
int         chunk_add_constant(EasecVM* vm, Chunk* c, Value v);

typedef struct { char* data; size_t len; size_t cap; } StrBuf;
void        sb_init(StrBuf* sb);
void        sb_append(EasecVM* vm, StrBuf* sb, const char* s, size_t n);
void        sb_free(EasecVM* vm, StrBuf* sb);
size_t      ea_format_double(double v, char out[48]);
void        value_format(EasecVM* vm, StrBuf* sb, Value v, int depth, bool quote);
ObjString*  value_to_string(EasecVM* vm, Value v);
bool        values_equal(Value a, Value b);
bool        is_truthy(Value v);
const char* value_type_name(Value v);

void        gc_collect(EasecVM* vm);
void        vm_free_all(EasecVM* vm);

/* ------------------------------------------------------------------ compiler.c / lexer.c */
typedef enum {
    T_EOF, T_ERROR, T_NEWLINE,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_COLON, T_DOT,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_EQEQ, T_BANGEQ, T_LT, T_LE, T_GT, T_GE,
    T_PLUSEQ, T_MINUSEQ, T_STAREQ, T_SLASHEQ,
    T_STRING, T_NUMBER, T_DECIMAL, T_IDENT,
    T_SAY, T_VAR, T_JOB, T_OUT, T_IF, T_ELSE, T_WHILE, T_REPEAT, T_FOREVER,
    T_STOP, T_SKIP, T_IMPORT, T_AS, T_TRUE, T_FALSE, T_NULL, T_AND, T_OR, T_NOT,
    T_COUNT
} TokenType;

typedef struct { TokenType type; const char* start; int length; int line; } Token;
typedef struct { const char* start; const char* current; int line; } Lexer;

void  lexer_init(Lexer* lx, const char* source);
Token lexer_next(Lexer* lx);

ObjFunction* compile(EasecVM* vm, const char* source, const char* name);

/* ------------------------------------------------------------------ vm.c / natives.c */
void vm_runtime_error(EasecVM* vm, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
bool vm_execute(EasecVM* vm, ObjClosure* closure);   /* runs a script closure to completion */
void natives_install(EasecVM* vm);

#endif
