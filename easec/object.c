/* Memory accounting, interned strings, hash tables, environments, mark & sweep GC, value formatting. */
#include "easec_internal.h"

/* ---------------------------------------------------------------- memory */
void* ea_realloc(EasecVM* vm, void* p, size_t old_size, size_t new_size) {
    vm->bytes_allocated = vm->bytes_allocated - old_size + new_size;
    if (new_size == 0) { free(p); return NULL; }
    void* r = realloc(p, new_size);
    if (!r) {
        ea_write(vm, "Fatal: out of memory\n", 21);
        abort();
    }
    return r;
}

void ea_write(EasecVM* vm, const char* s, size_t n) { vm->host.write(vm->host.ctx, s, n); }

void ea_printf(EasecVM* vm, const char* fmt, ...) {
    char buf[320];
    va_list ap; va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    ea_write(vm, buf, (size_t)n < sizeof buf ? (size_t)n : sizeof buf - 1);
}

Obj* alloc_object(EasecVM* vm, size_t size, ObjType type) {
    if (!vm->gc_paused && vm->bytes_allocated + size > vm->next_gc) gc_collect(vm);
    if (!vm->gc_paused && vm->bytes_allocated + size > vm->max_bytes) vm->out_of_memory = true;
    Obj* o = ea_realloc(vm, NULL, 0, size);
    memset(o, 0, size);
    o->type = type;
    o->next = vm->objects;
    vm->objects = o;
    return o;
}

/* ---------------------------------------------------------------- tables */
void table_init(Table* t) { t->count = 0; t->capacity = 0; t->entries = NULL; }
void table_free(EasecVM* vm, Table* t) { EA_FREE_ARRAY(vm, Entry, t->entries, t->capacity); table_init(t); }

/* Empty slot: key==NULL && value==null.  Tombstone: key==NULL && value==true. */
static Entry* find_entry(Entry* entries, int capacity, ObjString* key) {
    uint32_t i = key->hash & (uint32_t)(capacity - 1);
    Entry* tomb = NULL;
    for (;;) {
        Entry* e = &entries[i];
        if (!e->key) {
            if (IS_NULL(e->value)) return tomb ? tomb : e;
            if (!tomb) tomb = e;
        } else if (e->key == key) return e;      /* strings are interned: pointer equality */
        i = (i + 1) & (uint32_t)(capacity - 1);
    }
}

Value* table_slot(const Table* t, ObjString* key) {
    if (t->count == 0) return NULL;
    Entry* e = find_entry(t->entries, t->capacity, key);
    return e->key ? &e->value : NULL;
}

bool table_get(const Table* t, ObjString* key, Value* out) {
    Value* v = table_slot(t, key);
    if (!v) return false;
    *out = *v;
    return true;
}

static void grow(EasecVM* vm, Table* t, int capacity) {
    Entry* entries = EA_ARRAY(vm, Entry, capacity);
    for (int i = 0; i < capacity; i++) { entries[i].key = NULL; entries[i].value = NULL_VAL; }
    int count = 0;
    for (int i = 0; i < t->capacity; i++) {
        Entry* e = &t->entries[i];
        if (!e->key) continue;
        Entry* d = find_entry(entries, capacity, e->key);
        *d = *e; count++;
    }
    EA_FREE_ARRAY(vm, Entry, t->entries, t->capacity);
    t->entries = entries; t->capacity = capacity; t->count = count;   /* tombstones are dropped */
}

bool table_set(EasecVM* vm, Table* t, ObjString* key, Value v) {
    if ((t->count + 1) * 4 > t->capacity * 3) grow(vm, t, t->capacity < 8 ? 8 : t->capacity * 2);
    Entry* e = find_entry(t->entries, t->capacity, key);
    bool is_new = e->key == NULL;
    if (is_new && IS_NULL(e->value)) t->count++;          /* reusing a tombstone does not change count */
    e->key = key; e->value = v;
    return is_new;
}

bool table_delete(Table* t, ObjString* key) {
    if (t->count == 0) return false;
    Entry* e = find_entry(t->entries, t->capacity, key);
    if (!e->key) return false;
    e->key = NULL; e->value = BOOL_VAL(true);
    return true;
}

int table_live_count(const Table* t) {
    int n = 0;
    for (int i = 0; i < t->capacity; i++) if (t->entries[i].key) n++;
    return n;
}

/* ---------------------------------------------------------------- strings */
static uint32_t hash_bytes(const char* s, int n) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static ObjString* find_interned(Table* t, const char* chars, int len, uint32_t hash) {
    if (t->count == 0) return NULL;
    uint32_t i = hash & (uint32_t)(t->capacity - 1);
    for (;;) {
        Entry* e = &t->entries[i];
        if (!e->key) { if (IS_NULL(e->value)) return NULL; }
        /* The original compared strlen(key) on every probe; the length is stored now. */
        else if (e->key->length == len && e->key->hash == hash && memcmp(e->key->chars, chars, (size_t)len) == 0) return e->key;
        i = (i + 1) & (uint32_t)(t->capacity - 1);
    }
}

ObjString* string_intern(EasecVM* vm, const char* chars, int length) {
    uint32_t hash = hash_bytes(chars, length);
    ObjString* s = find_interned(&vm->strings, chars, length, hash);
    if (s) return s;
    s = (ObjString*)alloc_object(vm, sizeof(ObjString) + (size_t)length + 1, OBJ_STRING);
    s->length = length; s->hash = hash;
    memcpy(s->chars, chars, (size_t)length);
    s->chars[length] = '\0';
    table_set(vm, &vm->strings, s, NULL_VAL);
    return s;
}

ObjString* string_cstr(EasecVM* vm, const char* s) { return string_intern(vm, s, (int)strlen(s)); }

/* ---------------------------------------------------------------- objects */
ObjArray* array_new(EasecVM* vm) { return (ObjArray*)alloc_object(vm, sizeof(ObjArray), OBJ_ARRAY); }

void array_push(EasecVM* vm, ObjArray* a, Value v) {
    if (a->count + 1 > a->capacity) {
        int cap = a->capacity < 8 ? 8 : a->capacity * 2;
        a->items = EA_GROW(vm, Value, a->items, a->capacity, cap);
        a->capacity = cap;
    }
    a->items[a->count++] = v;
}

ObjDict* dict_new(EasecVM* vm) { return (ObjDict*)alloc_object(vm, sizeof(ObjDict), OBJ_DICT); }
ObjFunction* function_new(EasecVM* vm) {
    ObjFunction* f = (ObjFunction*)alloc_object(vm, sizeof(ObjFunction), OBJ_FUNCTION);
    chunk_init(&f->chunk);
    return f;
}
ObjClosure* closure_new(EasecVM* vm, ObjFunction* fn, Env* env) {
    ObjClosure* c = (ObjClosure*)alloc_object(vm, sizeof(ObjClosure), OBJ_CLOSURE);
    c->fn = fn; c->env = env;
    return c;
}
ObjNative* native_new(EasecVM* vm, const char* name, NativeFn fn, int arity) {
    ObjNative* n = (ObjNative*)alloc_object(vm, sizeof(ObjNative), OBJ_NATIVE);
    n->fn = fn; n->name = name; n->arity = arity;
    return n;
}
ObjModule* module_new(EasecVM* vm, ObjString* name, Env* env) {
    ObjModule* m = (ObjModule*)alloc_object(vm, sizeof(ObjModule), OBJ_MODULE);
    m->name = name; m->env = env;
    return m;
}
Env* env_new(EasecVM* vm, Env* parent) {
    Env* e = (Env*)alloc_object(vm, sizeof(Env), OBJ_ENV);
    e->parent = parent;
    return e;
}

bool env_lookup(const Env* env, ObjString* key, Value* out) {
    for (const Env* e = env; e; e = e->parent)
        if (table_get(&e->vars, key, out)) return true;
    return false;
}
bool env_assign(Env* env, ObjString* key, Value v) {
    for (Env* e = env; e; e = e->parent) {
        Value* slot = table_slot(&e->vars, key);
        if (slot) { *slot = v; return true; }
    }
    return false;
}
void env_define(EasecVM* vm, Env* env, ObjString* key, Value v) { table_set(vm, &env->vars, key, v); }

/* ---------------------------------------------------------------- chunks */
void chunk_init(Chunk* c) { memset(c, 0, sizeof *c); }
void chunk_free(EasecVM* vm, Chunk* c) {
    EA_FREE_ARRAY(vm, uint8_t, c->code, c->capacity);
    EA_FREE_ARRAY(vm, int, c->lines, c->capacity);
    EA_FREE_ARRAY(vm, Value, c->constants, c->const_capacity);
    chunk_init(c);
}
void chunk_write(EasecVM* vm, Chunk* c, uint8_t byte, int line) {
    if (c->count + 1 > c->capacity) {
        int cap = c->capacity < 16 ? 16 : c->capacity * 2;
        c->code  = EA_GROW(vm, uint8_t, c->code,  c->capacity, cap);
        c->lines = EA_GROW(vm, int,     c->lines, c->capacity, cap);
        c->capacity = cap;
    }
    c->code[c->count] = byte; c->lines[c->count] = line; c->count++;
}
int chunk_add_constant(EasecVM* vm, Chunk* c, Value v) {
    for (int i = 0; i < c->const_count; i++) {            /* de-duplicate strings and small scalars */
        Value o = c->constants[i];
        if (o.type == v.type && (v.type == VAL_OBJ ? o.as.obj == v.as.obj
                               : v.type == VAL_INT ? o.as.integer == v.as.integer : false)) return i;
    }
    if (c->const_count + 1 > c->const_capacity) {
        int cap = c->const_capacity < 8 ? 8 : c->const_capacity * 2;
        c->constants = EA_GROW(vm, Value, c->constants, c->const_capacity, cap);
        c->const_capacity = cap;
    }
    c->constants[c->const_count] = v;
    return c->const_count++;
}

/* ---------------------------------------------------------------- garbage collector */
static void gray_push(EasecVM* vm, Obj* o) {
    if (vm->gray_count + 1 > vm->gray_cap) {
        vm->gray_cap = vm->gray_cap < 64 ? 64 : vm->gray_cap * 2;
        vm->gray = realloc(vm->gray, sizeof(Obj*) * (size_t)vm->gray_cap);
        if (!vm->gray) { ea_write(vm, "Fatal: out of memory\n", 21); abort(); }
    }
    vm->gray[vm->gray_count++] = o;
}

static void mark_object(EasecVM* vm, Obj* o) {
    if (!o || o->marked) return;
    o->marked = true;
    if (o->type != OBJ_STRING && o->type != OBJ_NATIVE) gray_push(vm, o);   /* leaves have nothing to trace */
}
static void mark_value(EasecVM* vm, Value v) { if (IS_OBJ(v)) mark_object(vm, v.as.obj); }
static void mark_table(EasecVM* vm, const Table* t) {
    for (int i = 0; i < t->capacity; i++)
        if (t->entries[i].key) { mark_object(vm, (Obj*)t->entries[i].key); mark_value(vm, t->entries[i].value); }
}

/* Iterative tracing: the original's recursive mark_object/mark_env could overflow the kernel
 * stack on deeply nested data. */
static void blacken(EasecVM* vm, Obj* o) {
    switch (o->type) {
    case OBJ_ARRAY: { ObjArray* a = (ObjArray*)o; for (int i = 0; i < a->count; i++) mark_value(vm, a->items[i]); break; }
    case OBJ_DICT:  mark_table(vm, &((ObjDict*)o)->table); break;
    case OBJ_ENV:   { Env* e = (Env*)o; mark_table(vm, &e->vars); mark_object(vm, (Obj*)e->parent); break; }
    case OBJ_MODULE:{ ObjModule* m = (ObjModule*)o; mark_object(vm, (Obj*)m->name); mark_object(vm, (Obj*)m->env); break; }
    case OBJ_CLOSURE:{ ObjClosure* c = (ObjClosure*)o; mark_object(vm, (Obj*)c->fn); mark_object(vm, (Obj*)c->env); break; }
    case OBJ_FUNCTION: {
        ObjFunction* f = (ObjFunction*)o;
        mark_object(vm, (Obj*)f->name);
        for (int i = 0; i < f->arity; i++) mark_object(vm, (Obj*)f->params[i]);
        for (int i = 0; i < f->chunk.const_count; i++) mark_value(vm, f->chunk.constants[i]);
        break;
    }
    default: break;
    }
}

static void free_object(EasecVM* vm, Obj* o) {
    switch (o->type) {
    case OBJ_STRING:  ea_realloc(vm, o, sizeof(ObjString) + (size_t)((ObjString*)o)->length + 1, 0); return;
    case OBJ_ARRAY:   EA_FREE_ARRAY(vm, Value, ((ObjArray*)o)->items, ((ObjArray*)o)->capacity); ea_realloc(vm, o, sizeof(ObjArray), 0); return;
    case OBJ_DICT:    table_free(vm, &((ObjDict*)o)->table); ea_realloc(vm, o, sizeof(ObjDict), 0); return;
    case OBJ_ENV:     table_free(vm, &((Env*)o)->vars); ea_realloc(vm, o, sizeof(Env), 0); return;
    case OBJ_FUNCTION: {
        ObjFunction* f = (ObjFunction*)o;
        chunk_free(vm, &f->chunk);
        EA_FREE_ARRAY(vm, ObjString*, f->params, f->arity);
        ea_realloc(vm, o, sizeof(ObjFunction), 0); return;
    }
    case OBJ_CLOSURE: ea_realloc(vm, o, sizeof(ObjClosure), 0); return;
    case OBJ_NATIVE:  ea_realloc(vm, o, sizeof(ObjNative), 0); return;
    case OBJ_MODULE:  ea_realloc(vm, o, sizeof(ObjModule), 0); return;
    }
}

void gc_collect(EasecVM* vm) {
    if (vm->gc_paused) return;

    for (Value* s = vm->stack; s < vm->stack_top; s++) mark_value(vm, *s);
    for (int i = 0; i < vm->frame_count; i++) {
        mark_object(vm, (Obj*)vm->frames[i].closure);
        mark_object(vm, (Obj*)vm->frames[i].env);
    }
    mark_object(vm, (Obj*)vm->builtins);
    mark_object(vm, (Obj*)vm->globals);
    mark_table(vm, &vm->modules);
    while (vm->gray_count > 0) blacken(vm, vm->gray[--vm->gray_count]);

    /* the intern table is weak: drop strings nobody else references */
    for (int i = 0; i < vm->strings.capacity; i++) {
        Entry* e = &vm->strings.entries[i];
        if (e->key && !e->key->obj.marked) table_delete(&vm->strings, e->key);
    }

    Obj** link = &vm->objects;
    while (*link) {
        Obj* o = *link;
        if (o->marked) { o->marked = false; link = &o->next; }
        else { *link = o->next; free_object(vm, o); }
    }
    vm->next_gc = vm->bytes_allocated * 2;
    if (vm->next_gc < GC_MIN_NEXT) vm->next_gc = GC_MIN_NEXT;
}

void vm_free_all(EasecVM* vm) {
    Obj* o = vm->objects;
    while (o) { Obj* n = o->next; free_object(vm, o); o = n; }
    vm->objects = NULL;
    table_free(vm, &vm->strings);
    table_free(vm, &vm->modules);
    free(vm->gray);
    vm->gray = NULL; vm->gray_count = vm->gray_cap = 0;
}

/* ---------------------------------------------------------------- strings & values */
void sb_init(StrBuf* sb) { sb->data = NULL; sb->len = sb->cap = 0; }
void sb_free(EasecVM* vm, StrBuf* sb) { ea_realloc(vm, sb->data, sb->cap, 0); sb_init(sb); }
void sb_append(EasecVM* vm, StrBuf* sb, const char* s, size_t n) {
    if (sb->len + n + 1 > sb->cap) {
        size_t cap = sb->cap < 32 ? 32 : sb->cap;
        while (cap < sb->len + n + 1) cap *= 2;
        sb->data = ea_realloc(vm, sb->data, sb->cap, cap);
        sb->cap = cap;
    }
    memcpy(sb->data + sb->len, s, n);
    sb->len += n;
    sb->data[sb->len] = '\0';
}

/* Shortest fixed notation with <= 6 decimals; identical on the kernel and on the host,
 * so scripts print the same everywhere (libc's %g differs between implementations). */
size_t ea_format_double(double v, char out[48]) {
    size_t n = 0;
    if (v != v) { memcpy(out, "nan", 4); return 3; }
    if (v < 0) { out[n++] = '-'; v = -v; }
    if (v >= 9.2e18) { memcpy(out + n, "overflow", 9); return n + 8; }
    uint64_t ip = (uint64_t)v;
    uint32_t frac = (uint32_t)((v - (double)ip) * 1000000.0 + 0.5);
    if (frac >= 1000000u) { ip++; frac -= 1000000u; }
    char tmp[24]; size_t d = 0;
    if (ip == 0) tmp[d++] = '0';
    while (ip) { tmp[d++] = (char)('0' + ip % 10); ip /= 10; }
    while (d) out[n++] = tmp[--d];
    if (frac) {
        char fd[6];
        for (int i = 5; i >= 0; i--) { fd[i] = (char)('0' + frac % 10); frac /= 10; }
        int keep = 6;
        while (keep > 0 && fd[keep - 1] == '0') keep--;
        out[n++] = '.';
        memcpy(out + n, fd, (size_t)keep); n += (size_t)keep;
    }
    out[n] = '\0';
    return n;
}

void value_format(EasecVM* vm, StrBuf* sb, Value v, int depth, bool quote) {
    char tmp[64];
    switch (v.type) {
    case VAL_NULL: sb_append(vm, sb, "null", 4); return;
    case VAL_BOOL: if (v.as.boolean) sb_append(vm, sb, "true", 4); else sb_append(vm, sb, "false", 5); return;
    case VAL_INT:  sb_append(vm, sb, tmp, (size_t)snprintf(tmp, sizeof tmp, "%lld", (long long)v.as.integer)); return;
    case VAL_DECIMAL: { char b[48]; sb_append(vm, sb, b, ea_format_double(v.as.decimal, b)); return; }
    case VAL_OBJ: break;
    }
    Obj* o = v.as.obj;
    switch (o->type) {
    case OBJ_STRING:
        if (quote) sb_append(vm, sb, "\"", 1);
        sb_append(vm, sb, ((ObjString*)o)->chars, (size_t)((ObjString*)o)->length);
        if (quote) sb_append(vm, sb, "\"", 1);
        return;
    case OBJ_ARRAY: {
        ObjArray* a = (ObjArray*)o;
        if (depth > 6) { sb_append(vm, sb, "[...]", 5); return; }       /* also stops cyclic structures */
        sb_append(vm, sb, "[", 1);
        for (int i = 0; i < a->count; i++) {
            if (i) sb_append(vm, sb, ", ", 2);
            value_format(vm, sb, a->items[i], depth + 1, true);
        }
        sb_append(vm, sb, "]", 1);
        return;
    }
    case OBJ_DICT: {
        ObjDict* d = (ObjDict*)o;
        if (depth > 6) { sb_append(vm, sb, "{...}", 5); return; }
        sb_append(vm, sb, "{", 1);
        bool first = true;
        for (int i = 0; i < d->table.capacity; i++) {
            Entry* e = &d->table.entries[i];
            if (!e->key) continue;
            if (!first) sb_append(vm, sb, ", ", 2);
            first = false;
            sb_append(vm, sb, e->key->chars, (size_t)e->key->length);
            sb_append(vm, sb, ": ", 2);
            value_format(vm, sb, e->value, depth + 1, true);
        }
        sb_append(vm, sb, "}", 1);
        return;
    }
    case OBJ_CLOSURE: {
        ObjClosure* c = (ObjClosure*)o;
        sb_append(vm, sb, "<job ", 5);
        sb_append(vm, sb, c->fn->name ? c->fn->name->chars : "?", c->fn->name ? (size_t)c->fn->name->length : 1);
        sb_append(vm, sb, ">", 1);
        return;
    }
    case OBJ_NATIVE: sb_append(vm, sb, "<native ", 8); sb_append(vm, sb, ((ObjNative*)o)->name, strlen(((ObjNative*)o)->name)); sb_append(vm, sb, ">", 1); return;
    case OBJ_MODULE: sb_append(vm, sb, "<module ", 8); sb_append(vm, sb, ((ObjModule*)o)->name->chars, (size_t)((ObjModule*)o)->name->length); sb_append(vm, sb, ">", 1); return;
    default: sb_append(vm, sb, "<object>", 8); return;
    }
}

ObjString* value_to_string(EasecVM* vm, Value v) {
    if (IS_STRING(v)) return AS_STRING(v);
    StrBuf sb; sb_init(&sb);
    value_format(vm, &sb, v, 0, false);
    ObjString* s = string_intern(vm, sb.data ? sb.data : "", (int)sb.len);
    sb_free(vm, &sb);
    return s;
}

bool is_truthy(Value v) {
    switch (v.type) {
    case VAL_NULL: return false;
    case VAL_BOOL: return v.as.boolean;
    case VAL_INT:  return v.as.integer != 0;
    case VAL_DECIMAL: return v.as.decimal != 0.0;
    default: return true;
    }
}

bool values_equal(Value a, Value b) {
    if (IS_NUMBER(a) && IS_NUMBER(b)) {
        if (a.type == VAL_INT && b.type == VAL_INT) return a.as.integer == b.as.integer;
        return AS_NUMBER(a) == AS_NUMBER(b);
    }
    if (a.type != b.type) return false;
    switch (a.type) {
    case VAL_NULL: return true;
    case VAL_BOOL: return a.as.boolean == b.as.boolean;
    case VAL_OBJ:  return a.as.obj == b.as.obj;           /* interned strings: identity == equality */
    default: return false;
    }
}

const char* value_type_name(Value v) {
    switch (v.type) {
    case VAL_NULL: return "null";
    case VAL_BOOL: return "boolean";
    case VAL_INT:  return "number";
    case VAL_DECIMAL: return "decimal";
    case VAL_OBJ:
        switch (v.as.obj->type) {
        case OBJ_STRING: return "text";
        case OBJ_ARRAY:  return "list";
        case OBJ_DICT:   return "dict";
        case OBJ_CLOSURE: case OBJ_NATIVE: case OBJ_FUNCTION: return "job";
        case OBJ_MODULE: return "module";
        default: return "object";
        }
    }
    return "?";
}
