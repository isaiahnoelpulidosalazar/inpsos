/* Built-in jobs and the `time` / `file` modules.
 *
 * The original had a dedicated opcode, statement grammar and parser branch for each of
 * `time get`, `time sleep`, `file create/update/delete`, `array get/set` ... Adding a system
 * call meant touching the lexer, parser, compiler and VM.  Now it is one table entry here. */
#include "easec_internal.h"

#define FAIL(...) do { vm_runtime_error(vm, __VA_ARGS__); return false; } while (0)
#define NEED_TEXT(i, what) do { if (!IS_STRING(argv[i])) FAIL("%s must be text (got %s)", what, value_type_name(argv[i])); } while (0)

static bool parse_number(const char* s, int len, Value* out) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    while (len > i && (s[len - 1] == ' ' || s[len - 1] == '\t')) len--;
    bool neg = false;
    if (i < len && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i >= len) return false;
    uint64_t iv = 0; double dv = 0, scale = 1; bool frac = false, any = false, overflow = false;
    for (; i < len; i++) {
        char c = s[i];
        if (c == '.' && !frac) { frac = true; continue; }
        if (c < '0' || c > '9') return false;
        any = true;
        if (!frac) { if (iv > (UINT64_MAX - (uint64_t)(c - '0')) / 10 || iv * 10 + (uint64_t)(c - '0') > (uint64_t)INT64_MAX) overflow = true; else iv = iv * 10 + (uint64_t)(c - '0'); }
        dv = dv * 10 + (c - '0');
        if (frac) scale *= 10;
    }
    if (!any) return false;
    if (frac || overflow) *out = DEC_VAL((neg ? -dv : dv) / scale);
    else *out = INT_VAL(neg ? (int64_t)(0 - iv) : (int64_t)iv);
    return true;
}

/* ---- conversion & inspection ------------------------------------------------------------ */
static bool n_text(EasecVM* vm, int argc, Value* argv, Value* r) { *r = OBJ_VAL(value_to_string(vm, argv[0])); return true; }

static bool n_number(EasecVM* vm, int argc, Value* argv, Value* r) {
    Value v = argv[0];
    if (v.type == VAL_INT) *r = v;
    else if (v.type == VAL_DECIMAL) {
        if (!(v.as.decimal > -9.2e18 && v.as.decimal < 9.2e18)) FAIL("Decimal out of range for number()");
        *r = INT_VAL((int64_t)v.as.decimal);
    } else if (IS_STRING(v)) {
        Value p;
        if (parse_number(AS_STRING(v)->chars, AS_STRING(v)->length, &p)) *r = p.type == VAL_INT ? p : INT_VAL((int64_t)p.as.decimal);
        else *r = NULL_VAL;                                            /* unparsable text -> null */
    } else if (v.type == VAL_BOOL) *r = INT_VAL(v.as.boolean ? 1 : 0);
    else *r = NULL_VAL;
    return true;
}

static bool n_decimal(EasecVM* vm, int argc, Value* argv, Value* r) {
    Value v = argv[0], p;
    if (IS_NUMBER(v)) *r = DEC_VAL(AS_NUMBER(v));
    else if (IS_STRING(v) && parse_number(AS_STRING(v)->chars, AS_STRING(v)->length, &p)) *r = DEC_VAL(AS_NUMBER(p));
    else *r = NULL_VAL;
    return true;
}

static bool n_type(EasecVM* vm, int argc, Value* argv, Value* r) { *r = OBJ_VAL(string_cstr(vm, value_type_name(argv[0]))); return true; }

static bool n_len(EasecVM* vm, int argc, Value* argv, Value* r) {
    Value v = argv[0];
    if (IS_STRING(v)) *r = INT_VAL(AS_STRING(v)->length);
    else if (IS_ARRAY(v)) *r = INT_VAL(AS_ARRAY(v)->count);
    else if (IS_DICT(v)) *r = INT_VAL(table_live_count(&AS_DICT(v)->table));
    else FAIL("len() needs text, a list or a dictionary (got %s)", value_type_name(v));
    return true;
}

static bool n_push(EasecVM* vm, int argc, Value* argv, Value* r) {
    if (!IS_ARRAY(argv[0])) FAIL("push() needs a list (got %s)", value_type_name(argv[0]));
    array_push(vm, AS_ARRAY(argv[0]), argv[1]);
    *r = INT_VAL(AS_ARRAY(argv[0])->count);
    return true;
}

static bool n_pop(EasecVM* vm, int argc, Value* argv, Value* r) {
    if (!IS_ARRAY(argv[0])) FAIL("pop() needs a list (got %s)", value_type_name(argv[0]));
    ObjArray* a = AS_ARRAY(argv[0]);
    *r = a->count ? a->items[--a->count] : NULL_VAL;
    return true;
}

static bool n_keys(EasecVM* vm, int argc, Value* argv, Value* r) {
    if (!IS_DICT(argv[0])) FAIL("keys() needs a dictionary (got %s)", value_type_name(argv[0]));
    ObjArray* a = array_new(vm);
    Table* t = &AS_DICT(argv[0])->table;
    for (int i = 0; i < t->capacity; i++) if (t->entries[i].key) array_push(vm, a, OBJ_VAL(t->entries[i].key));
    *r = OBJ_VAL(a);
    return true;
}

static bool n_input(EasecVM* vm, int argc, Value* argv, Value* r) {
    if (argc > 1) FAIL("input() takes at most one argument");
    if (argc == 1) { ObjString* p = value_to_string(vm, argv[0]); ea_write(vm, p->chars, (size_t)p->length); }
    char buf[256];
    if (!vm->host.read_line(vm->host.ctx, buf, sizeof buf)) *r = NULL_VAL;
    else *r = OBJ_VAL(string_cstr(vm, buf));
    return true;
}

/* ---- time ---------------------------------------------------------------------------------- */
static bool t_now(EasecVM* vm, int argc, Value* argv, Value* r)    { *r = INT_VAL(vm->host.now_ms(vm->host.ctx)); return true; }
static bool t_uptime(EasecVM* vm, int argc, Value* argv, Value* r) { *r = INT_VAL(vm->host.uptime_ms(vm->host.ctx)); return true; }
static bool t_sleep(EasecVM* vm, int argc, Value* argv, Value* r) {
    if (argv[0].type != VAL_INT) FAIL("time.sleep() needs a whole number of milliseconds");
    if (argv[0].as.integer > 0) vm->host.sleep_ms(vm->host.ctx, argv[0].as.integer);
    return true;
}

/* ---- file ---------------------------------------------------------------------------------- */
static bool file_write_common(EasecVM* vm, Value* argv, Value* r, bool append) {
    NEED_TEXT(0, "file name");
    ObjString* data = value_to_string(vm, argv[1]);
    *r = BOOL_VAL(vm->host.write_file(vm->host.ctx, AS_STRING(argv[0])->chars, data->chars, (size_t)data->length, append));
    return true;
}
static bool f_create(EasecVM* vm, int argc, Value* argv, Value* r) { return file_write_common(vm, argv, r, false); }
static bool f_append(EasecVM* vm, int argc, Value* argv, Value* r) { return file_write_common(vm, argv, r, true); }
static bool f_delete(EasecVM* vm, int argc, Value* argv, Value* r) {
    NEED_TEXT(0, "file name");
    *r = BOOL_VAL(vm->host.delete_file(vm->host.ctx, AS_STRING(argv[0])->chars));
    return true;
}
static bool f_read(EasecVM* vm, int argc, Value* argv, Value* r) {
    NEED_TEXT(0, "file name");
    size_t len = 0;
    char* data = vm->host.read_file(vm->host.ctx, AS_STRING(argv[0])->chars, &len);
    if (!data) { *r = NULL_VAL; return true; }
    *r = OBJ_VAL(string_intern(vm, data, (int)len));
    free(data);
    return true;
}
static bool f_exists(EasecVM* vm, int argc, Value* argv, Value* r) {
    NEED_TEXT(0, "file name");
    int n = vm->host.file_count(vm->host.ctx);
    *r = BOOL_VAL(false);
    for (int i = 0; i < n; i++)
        if (strcmp(vm->host.file_name(vm->host.ctx, i), AS_STRING(argv[0])->chars) == 0) { *r = BOOL_VAL(true); break; }
    return true;
}
static bool f_list(EasecVM* vm, int argc, Value* argv, Value* r) {
    ObjArray* a = array_new(vm);
    *r = OBJ_VAL(a);
    int n = vm->host.file_count(vm->host.ctx);
    for (int i = 0; i < n; i++) array_push(vm, a, OBJ_VAL(string_cstr(vm, vm->host.file_name(vm->host.ctx, i))));
    return true;
}

/* ---- registration -------------------------------------------------------------------------- */
typedef struct { const char* name; NativeFn fn; int arity; } NativeDef;

static void define(EasecVM* vm, Env* env, const NativeDef* d) {
    env_define(vm, env, string_cstr(vm, d->name), OBJ_VAL(native_new(vm, d->name, d->fn, d->arity)));
}

static void define_module(EasecVM* vm, const char* name, const NativeDef* defs, size_t n) {
    Env* env = env_new(vm, NULL);
    ObjString* mname = string_cstr(vm, name);
    env_define(vm, vm->builtins, mname, OBJ_VAL(module_new(vm, mname, env)));
    for (size_t i = 0; i < n; i++) define(vm, env, &defs[i]);
}

void natives_install(EasecVM* vm) {        /* called with the GC paused */
    static const NativeDef globals[] = {
        {"input", n_input, -1}, {"len", n_len, 1}, {"text", n_text, 1}, {"number", n_number, 1},
        {"decimal", n_decimal, 1}, {"type", n_type, 1}, {"push", n_push, 2}, {"pop", n_pop, 1}, {"keys", n_keys, 1},
    };
    static const NativeDef time_fns[] = { {"now", t_now, 0}, {"uptime", t_uptime, 0}, {"sleep", t_sleep, 1} };
    static const NativeDef file_fns[] = {
        {"create", f_create, 2}, {"append", f_append, 2}, {"delete", f_delete, 1},
        {"read", f_read, 1}, {"exists", f_exists, 1}, {"list", f_list, 0},
    };
    for (size_t i = 0; i < sizeof globals / sizeof globals[0]; i++) define(vm, vm->builtins, &globals[i]);
    define_module(vm, "time", time_fns, sizeof time_fns / sizeof time_fns[0]);
    define_module(vm, "file", file_fns, sizeof file_fns / sizeof file_fns[0]);
}
