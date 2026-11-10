#include "easec_internal.h"

/* Differences from the original VM:
 *  - Variable names are interned constants, so a lookup is a pointer-keyed hash probe.  The
 *    original called allocate_string(name->chars, strlen(...)) -- hash + intern -- on EVERY
 *    variable read/write.
 *  - Every frame carries its own Env; nothing is "restored" through a global vm.env.
 *  - Frame depth and value-stack depth are checked (the original overran its arrays on
 *    recursion deeper than 64).
 *  - No global error flag polled each instruction: errors return false up the C stack.
 *  - Reading a variable that holds null is legal (the original reported "undefined").
 */

static void push(EasecVM* vm, Value v) { *vm->stack_top++ = v; }
static Value pop(EasecVM* vm) { return *--vm->stack_top; }
static Value peek(EasecVM* vm, int d) { return vm->stack_top[-1 - d]; }

void vm_runtime_error(EasecVM* vm, const char* fmt, ...) {
    char msg[200];
    va_list ap; va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    ea_printf(vm, "Runtime error: %s\n", msg);
    for (int i = vm->frame_count - 1; i >= 0; i--) {
        CallFrame* f = &vm->frames[i];
        Chunk* c = &f->closure->fn->chunk;
        int off = (int)(f->ip - c->code) - 1;
        if (off < 0) off = 0;
        ea_printf(vm, "  [line %d] in %s\n", c->lines[off], f->closure->fn->name ? f->closure->fn->name->chars : "?");
    }
}

static bool call_closure(EasecVM* vm, ObjClosure* c, int argc, bool is_script) {
    if (vm->frame_count == FRAMES_MAX) { vm_runtime_error(vm, "Stack overflow (recursion too deep)"); return false; }
    Env* env;
    if (is_script) env = c->env;
    else {
        env = env_new(vm, c->env);
        for (int i = 0; i < argc; i++) env_define(vm, env, c->fn->params[i], vm->stack_top[-argc + i]);
    }
    CallFrame* f = &vm->frames[vm->frame_count++];
    f->closure = c; f->ip = c->fn->chunk.code; f->env = env;
    f->slots = vm->stack_top - argc - 1;
    return true;
}

static const char* const type_names[] = { "text", "number", "decimal", "boolean", "list", "dict" };

static bool type_matches(EasecVM* vm, Value* v, EaType t) {
    switch (t) {
    case TY_TEXT:    return IS_STRING(*v);
    case TY_NUMBER:  return v->type == VAL_INT;
    case TY_DECIMAL: if (v->type == VAL_INT) { *v = DEC_VAL((double)v->as.integer); return true; }
                     return v->type == VAL_DECIMAL;
    case TY_BOOLEAN: return v->type == VAL_BOOL;
    case TY_LIST:    return IS_ARRAY(*v);
    case TY_DICT:    return IS_DICT(*v);
    }
    (void)vm;
    return false;
}

static bool run(EasecVM* vm, int base_frames);

/* Runs a script closure that is ALREADY on top of the stack. On success the closure's slot holds
 * the script's return value. */
static bool execute_pushed(EasecVM* vm, ObjClosure* c) {
    int base = vm->frame_count;
    if (!call_closure(vm, c, 0, true)) return false;
    return run(vm, base);
}

bool vm_execute(EasecVM* vm, ObjClosure* c) {
    push(vm, OBJ_VAL(c));
    return execute_pushed(vm, c);
}

/* Loads (or fetches from cache) a module; returns NULL after reporting an error. */
static ObjModule* load_module(EasecVM* vm, ObjString* path) {
    Value cached;
    if (table_get(&vm->modules, path, &cached)) return AS_MODULE(cached);   /* cycles see the partial module */
    if (vm->import_depth >= 16) { vm_runtime_error(vm, "Imports nested too deeply"); return NULL; }

    size_t len = 0;
    char* src = vm->host.read_file(vm->host.ctx, path->chars, &len);
    if (!src) { vm_runtime_error(vm, "Cannot open module '%s'", path->chars); return NULL; }

    vm->gc_paused++;
    ObjFunction* fn = compile(vm, src, path->chars);
    free(src);
    if (!fn) { vm->gc_paused--; vm_runtime_error(vm, "Module '%s' has compile errors", path->chars); return NULL; }
    Env* env = env_new(vm, vm->builtins);
    ObjModule* mod = module_new(vm, path, env);
    table_set(vm, &vm->modules, path, OBJ_VAL(mod));
    ObjClosure* cl = closure_new(vm, fn, env);
    push(vm, OBJ_VAL(cl));                               /* rooted from here on */
    vm->gc_paused--;

    vm->import_depth++;
    bool ok = execute_pushed(vm, cl);
    vm->import_depth--;
    if (!ok) { table_delete(&vm->modules, path); return NULL; }
    pop(vm);                                             /* the module script's return value */
    return mod;
}

static bool concat(EasecVM* vm, Value a, Value b) {
    vm->gc_paused++;                                     /* temporaries below are not rooted */
    ObjString* sa = value_to_string(vm, a);
    ObjString* sb = value_to_string(vm, b);
    int64_t total = (int64_t)sa->length + sb->length;
    if (total > (1 << 24)) { vm->gc_paused--; vm_runtime_error(vm, "Text too long"); return false; }
    char* tmp = malloc((size_t)total + 1);
    memcpy(tmp, sa->chars, (size_t)sa->length);
    memcpy(tmp + sa->length, sb->chars, (size_t)sb->length);
    ObjString* r = string_intern(vm, tmp, (int)total);
    free(tmp);
    vm->gc_paused--;
    vm->stack_top -= 2;
    push(vm, OBJ_VAL(r));
    return true;
}

#define READ_BYTE()  (*frame->ip++)
#define READ_U16()   (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define CONSTANT()   (frame->closure->fn->chunk.constants[READ_U16()])
#define NAME()       AS_STRING(CONSTANT())
#define ERR(...)     do { vm_runtime_error(vm, __VA_ARGS__); return false; } while (0)

#define ARITH(sym, C_OP, D_OP) do { \
        Value b = peek(vm, 0), a = peek(vm, 1); \
        if (!IS_NUMBER(a) || !IS_NUMBER(b)) ERR("Operands of '%s' must be numbers (got %s and %s)", sym, value_type_name(a), value_type_name(b)); \
        vm->stack_top -= 2; \
        if (a.type == VAL_INT && b.type == VAL_INT) push(vm, INT_VAL((int64_t)((uint64_t)a.as.integer C_OP (uint64_t)b.as.integer))); \
        else push(vm, DEC_VAL(AS_NUMBER(a) D_OP AS_NUMBER(b))); \
    } while (0)

#define COMPARE(sym, OP) do { \
        Value b = peek(vm, 0), a = peek(vm, 1); \
        if (!IS_NUMBER(a) || !IS_NUMBER(b)) ERR("Operands of '%s' must be numbers (got %s and %s)", sym, value_type_name(a), value_type_name(b)); \
        vm->stack_top -= 2; \
        if (a.type == VAL_INT && b.type == VAL_INT) push(vm, BOOL_VAL(a.as.integer OP b.as.integer)); \
        else push(vm, BOOL_VAL(AS_NUMBER(a) OP AS_NUMBER(b))); \
    } while (0)

static bool run(EasecVM* vm, int base_frames) {
    CallFrame* frame = &vm->frames[vm->frame_count - 1];
    for (;;) {
        if (vm->out_of_memory) ERR("Out of memory (limit %u KiB)", (unsigned)(vm->max_bytes / 1024));
        if (vm->stack_top > vm->stack + STACK_MAX - 4) ERR("Stack overflow (expression too large)");

        switch (READ_BYTE()) {
        case OP_CONST: push(vm, CONSTANT()); break;
        case OP_NULL:  push(vm, NULL_VAL); break;
        case OP_TRUE:  push(vm, BOOL_VAL(true)); break;
        case OP_FALSE: push(vm, BOOL_VAL(false)); break;
        case OP_POP:   pop(vm); break;
        case OP_DUP:   push(vm, peek(vm, 0)); break;
        case OP_DUP2:  push(vm, peek(vm, 1)); push(vm, peek(vm, 1)); break;

        case OP_GET_VAR: {
            ObjString* name = NAME(); Value v;
            if (!env_lookup(frame->env, name, &v)) ERR("Undefined variable '%s'", name->chars);
            push(vm, v); break;
        }
        case OP_DEF_VAR: { ObjString* name = NAME(); env_define(vm, frame->env, name, peek(vm, 0)); pop(vm); break; }
        case OP_SET_VAR: {
            ObjString* name = NAME();
            if (!env_assign(frame->env, name, peek(vm, 0))) ERR("Undefined variable '%s' (declare it with 'var' first)", name->chars);
            pop(vm); break;
        }

        case OP_GET_PROP: {
            ObjString* name = NAME(); Value o = peek(vm, 0), out;
            if (is_obj_type(o, OBJ_MODULE)) {
                if (!table_get(&AS_MODULE(o)->env->vars, name, &out)) ERR("Module '%s' has no member '%s'", AS_MODULE(o)->name->chars, name->chars);
            } else if (IS_DICT(o)) {
                if (!table_get(&AS_DICT(o)->table, name, &out)) out = NULL_VAL;
            } else ERR("Only modules and dictionaries have properties (got %s)", value_type_name(o));
            vm->stack_top[-1] = out; break;
        }
        case OP_SET_PROP: {
            ObjString* name = NAME(); Value o = peek(vm, 1);
            if (!IS_DICT(o)) ERR("Can only assign properties of a dictionary (got %s)", value_type_name(o));
            table_set(vm, &AS_DICT(o)->table, name, peek(vm, 0));
            vm->stack_top -= 2; break;
        }
        case OP_GET_INDEX: {
            Value idx = peek(vm, 0), o = peek(vm, 1), out;
            if (IS_ARRAY(o) || IS_STRING(o)) {
                if (idx.type != VAL_INT) ERR("Index must be a number (got %s)", value_type_name(idx));
                int64_t n = IS_ARRAY(o) ? AS_ARRAY(o)->count : AS_STRING(o)->length;
                if (idx.as.integer < 0 || idx.as.integer >= n) ERR("Index %lld out of range (length %lld)", (long long)idx.as.integer, (long long)n);
                out = IS_ARRAY(o) ? AS_ARRAY(o)->items[idx.as.integer]
                                  : OBJ_VAL(string_intern(vm, AS_STRING(o)->chars + idx.as.integer, 1));
            } else if (IS_DICT(o)) {
                if (!IS_STRING(idx)) ERR("Dictionary keys must be text (got %s)", value_type_name(idx));
                if (!table_get(&AS_DICT(o)->table, AS_STRING(idx), &out)) out = NULL_VAL;
            } else ERR("Can't index a %s", value_type_name(o));
            vm->stack_top -= 2; push(vm, out); break;
        }
        case OP_SET_INDEX: {
            Value val = peek(vm, 0), idx = peek(vm, 1), o = peek(vm, 2);
            if (IS_ARRAY(o)) {
                if (idx.type != VAL_INT || idx.as.integer < 0 || idx.as.integer >= AS_ARRAY(o)->count) ERR("Invalid list index (use push() to grow a list)");
                AS_ARRAY(o)->items[idx.as.integer] = val;
            } else if (IS_DICT(o)) {
                if (!IS_STRING(idx)) ERR("Dictionary keys must be text (got %s)", value_type_name(idx));
                table_set(vm, &AS_DICT(o)->table, AS_STRING(idx), val);
            } else ERR("Can't assign into a %s", value_type_name(o));
            vm->stack_top -= 3; break;
        }

        case OP_EQ: { Value b = pop(vm), a = pop(vm); push(vm, BOOL_VAL(values_equal(a, b))); break; }
        case OP_GT: COMPARE(">", >); break;
        case OP_LT: COMPARE("<", <); break;
        case OP_GE: COMPARE(">=", >=); break;
        case OP_LE: COMPARE("<=", <=); break;

        case OP_ADD:
            if (IS_STRING(peek(vm, 0)) || IS_STRING(peek(vm, 1))) { if (!concat(vm, peek(vm, 1), peek(vm, 0))) return false; }
            else ARITH("+", +, +);
            break;
        case OP_SUB: ARITH("-", -, -); break;
        case OP_MUL: ARITH("*", *, *); break;
        case OP_DIV: {
            Value b = peek(vm, 0), a = peek(vm, 1);
            if (!IS_NUMBER(a) || !IS_NUMBER(b)) ERR("Operands of '/' must be numbers (got %s and %s)", value_type_name(a), value_type_name(b));
            vm->stack_top -= 2;
            if (a.type == VAL_INT && b.type == VAL_INT) {            /* integer division, as before */
                if (b.as.integer == 0) ERR("Division by zero");
                push(vm, INT_VAL(b.as.integer == -1 ? (int64_t)(0 - (uint64_t)a.as.integer) : a.as.integer / b.as.integer));
            } else {
                if (AS_NUMBER(b) == 0.0) ERR("Division by zero");
                push(vm, DEC_VAL(AS_NUMBER(a) / AS_NUMBER(b)));
            }
            break;
        }
        case OP_MOD: {
            Value b = peek(vm, 0), a = peek(vm, 1);
            if (a.type != VAL_INT || b.type != VAL_INT) ERR("Operands of '%%' must be whole numbers");
            if (b.as.integer == 0) ERR("Division by zero");
            vm->stack_top -= 2;
            push(vm, INT_VAL(b.as.integer == -1 ? 0 : a.as.integer % b.as.integer));
            break;
        }
        case OP_NOT: vm->stack_top[-1] = BOOL_VAL(!is_truthy(peek(vm, 0))); break;
        case OP_NEG: {
            Value v = peek(vm, 0);
            if (v.type == VAL_INT) vm->stack_top[-1] = INT_VAL((int64_t)(0 - (uint64_t)v.as.integer));
            else if (v.type == VAL_DECIMAL) vm->stack_top[-1] = DEC_VAL(-v.as.decimal);
            else ERR("Operand of '-' must be a number (got %s)", value_type_name(v));
            break;
        }

        case OP_JUMP:          { uint16_t off = READ_U16(); frame->ip += off; break; }
        case OP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!is_truthy(peek(vm, 0))) frame->ip += off; break; }
        case OP_LOOP:          { uint16_t off = READ_U16(); frame->ip -= off; break; }

        case OP_CALL: {
            int argc = READ_BYTE();
            Value callee = peek(vm, argc);
            if (is_obj_type(callee, OBJ_CLOSURE)) {
                ObjClosure* c = AS_CLOSURE(callee);
                if (argc != c->fn->arity) ERR("'%s' expects %d argument(s) but got %d", c->fn->name->chars, c->fn->arity, argc);
                if (!call_closure(vm, c, argc, false)) return false;
                frame = &vm->frames[vm->frame_count - 1];
            } else if (is_obj_type(callee, OBJ_NATIVE)) {
                ObjNative* n = AS_NATIVE(callee);
                if (n->arity >= 0 && argc != n->arity) ERR("'%s' expects %d argument(s) but got %d", n->name, n->arity, argc);
                Value result = NULL_VAL;
                vm->gc_paused++;                                   /* natives allocate several linked objects */
                bool ok = n->fn(vm, argc, vm->stack_top - argc, &result);
                vm->gc_paused--;
                if (!ok) return false;
                vm->stack_top -= argc + 1;
                push(vm, result);
            } else ERR("Can only call jobs (got %s)", value_type_name(callee));
            break;
        }
        case OP_RETURN: {
            Value result = pop(vm);
            vm->frame_count--;
            vm->stack_top = frame->slots;
            push(vm, result);
            if (vm->frame_count == base_frames) return true;
            frame = &vm->frames[vm->frame_count - 1];
            break;
        }
        case OP_CLOSURE: {
            ObjFunction* fn = (ObjFunction*)CONSTANT().as.obj;
            push(vm, OBJ_VAL(closure_new(vm, fn, frame->env)));
            break;
        }

        case OP_ARRAY: {
            int n = READ_U16();
            ObjArray* a = array_new(vm);                           /* elements are still on the stack: GC-safe */
            if (n) { a->items = EA_ARRAY(vm, Value, n); memcpy(a->items, vm->stack_top - n, sizeof(Value) * (size_t)n); }
            a->count = a->capacity = n;
            vm->stack_top -= n;
            push(vm, OBJ_VAL(a));
            break;
        }
        case OP_DICT: {
            int n = READ_U16();
            ObjDict* d = dict_new(vm);
            Value* base = vm->stack_top - 2 * n;
            for (int i = 0; i < n; i++) table_set(vm, &d->table, AS_STRING(base[2 * i]), base[2 * i + 1]);
            vm->stack_top = base;
            push(vm, OBJ_VAL(d));
            break;
        }

        case OP_SAY: {
            Value v = peek(vm, 0);
            if (IS_STRING(v)) ea_write(vm, AS_STRING(v)->chars, (size_t)AS_STRING(v)->length);
            else {
                StrBuf sb; sb_init(&sb);
                value_format(vm, &sb, v, 0, false);
                if (sb.len) ea_write(vm, sb.data, sb.len);
                sb_free(vm, &sb);
            }
            ea_write(vm, "\n", 1);
            pop(vm); break;
        }
        case OP_IMPORT: {
            ObjString* path = NAME();
            ObjString* alias = NAME();
            ObjModule* mod = load_module(vm, path);
            if (!mod) return false;
            env_define(vm, frame->env, alias, OBJ_VAL(mod));
            break;
        }
        case OP_CHECK_TYPE: {
            int t = READ_BYTE();
            if (!type_matches(vm, &vm->stack_top[-1], (EaType)t)) ERR("Expected %s but got %s", type_names[t], value_type_name(peek(vm, 0)));
            break;
        }
        default: ERR("Corrupt bytecode (opcode %d)", frame->ip[-1]);
        }
    }
}

/* ------------------------------------------------------------------ embedding API */
EasecVM* easec_create(const EasecHost* host) {
    EasecVM* vm = calloc(1, sizeof *vm);
    if (!vm) return NULL;
    vm->host = *host;
    vm->max_bytes = host->max_bytes ? host->max_bytes : 1024 * 1024;
    vm->stack_top = vm->stack;
    vm->next_gc = GC_MIN_NEXT;
    vm->gc_paused = 1;
    vm->builtins = env_new(vm, NULL);
    natives_install(vm);
    vm->globals = env_new(vm, vm->builtins);
    vm->gc_paused = 0;
    return vm;
}

void easec_destroy(EasecVM* vm) {
    if (!vm) return;
    vm_free_all(vm);
    free(vm);
}

size_t easec_bytes_allocated(const EasecVM* vm) { return vm->bytes_allocated; }

bool easec_run(EasecVM* vm, const char* name, const char* source) {
    vm->gc_paused++;
    ObjFunction* fn = compile(vm, source, name);
    if (!fn) { vm->gc_paused--; return false; }
    ObjClosure* c = closure_new(vm, fn, vm->globals);
    vm->gc_paused--;
    vm->out_of_memory = false;
    bool ok = vm_execute(vm, c);
    vm->frame_count = 0;
    vm->stack_top = vm->stack;
    return ok;
}
