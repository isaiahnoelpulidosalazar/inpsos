/* Host-side test runner for the Easec interpreter.
 *   test_easec <tests-dir>
 * For every tests/easec/NAME.easec it runs the script with a fake host and compares the
 * output with NAME.expected (optional NAME.input provides stdin lines).
 * Built with ASan/UBSan, so leaks, double frees and overflows fail the test. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <time.h>
#include "easec.h"

typedef struct {
    char out[1 << 16]; size_t out_len;
    char* input; size_t input_pos;
    struct { char name[64]; char* data; size_t len; } files[16]; int nfiles;
    const char* dir;
} Fake;

static void h_write(void* c, const char* s, size_t n) { Fake* f = c; if (f->out_len + n < sizeof f->out) { memcpy(f->out + f->out_len, s, n); f->out_len += n; f->out[f->out_len] = 0; } }
static bool h_read_line(void* c, char* buf, size_t cap) {
    Fake* f = c;
    if (!f->input || !f->input[f->input_pos]) return false;
    size_t i = 0;
    while (f->input[f->input_pos] && f->input[f->input_pos] != '\n' && i + 1 < cap) buf[i++] = f->input[f->input_pos++];
    if (f->input[f->input_pos] == '\n') f->input_pos++;
    buf[i] = 0; return true;
}
static char* slurp(const char* path, size_t* len) {
    FILE* fp = fopen(path, "rb"); if (!fp) return NULL;
    fseek(fp, 0, SEEK_END); long n = ftell(fp); fseek(fp, 0, SEEK_SET);
    char* b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, fp); b[r] = 0; fclose(fp);
    if (len) *len = r;
    return b;
}
static int find_file(Fake* f, const char* n) { for (int i = 0; i < f->nfiles; i++) if (!strcmp(f->files[i].name, n)) return i; return -1; }
static char* h_read_file(void* c, const char* path, size_t* len) {
    Fake* f = c; int i = find_file(f, path);
    if (i >= 0) { char* b = malloc(f->files[i].len + 1); memcpy(b, f->files[i].data, f->files[i].len); b[f->files[i].len] = 0; *len = f->files[i].len; return b; }
    char p[512];
    snprintf(p, sizeof p, "programs/%s", path); char* b = slurp(p, len); if (b) return b;   /* real shipped modules */
    snprintf(p, sizeof p, "%s/easec/%s", f->dir, path); return slurp(p, len);
}
static bool h_write_file(void* c, const char* path, const char* d, size_t n, bool app) {
    Fake* f = c; int i = find_file(f, path);
    if (i < 0) { if (f->nfiles == 16) return false; i = f->nfiles++; snprintf(f->files[i].name, 64, "%s", path); f->files[i].data = NULL; f->files[i].len = 0; }
    if (!app) f->files[i].len = 0;
    f->files[i].data = realloc(f->files[i].data, f->files[i].len + n + 1);
    memcpy(f->files[i].data + f->files[i].len, d, n); f->files[i].len += n; return true;
}
static bool h_delete(void* c, const char* path) {
    Fake* f = c; int i = find_file(f, path); if (i < 0) return false;
    free(f->files[i].data); f->files[i] = f->files[--f->nfiles]; return true;
}
static int h_count(void* c) { return ((Fake*)c)->nfiles; }
static const char* h_name(void* c, int i) { return ((Fake*)c)->files[i].name; }
static int64_t h_now(void* c) { (void)c; return 1700000000123LL; }                 /* fixed: 2023-11-14 22:13:20 UTC */
static int64_t h_up(void* c) { (void)c; return (int64_t)clock() * 1000 / CLOCKS_PER_SEC; }
static void h_sleep(void* c, int64_t ms) { (void)c; (void)ms; }

static int run_one(const char* dir, const char* name) {
    char p[512], base[256]; snprintf(base, sizeof base, "%.*s", (int)(strlen(name) - 6), name);
    snprintf(p, sizeof p, "%s/easec/%s", dir, name);
    char* src = slurp(p, NULL);
    snprintf(p, sizeof p, "%s/easec/%s.expected", dir, base); char* expected = slurp(p, NULL);
    snprintf(p, sizeof p, "%s/easec/%s.input", dir, base);    char* input = slurp(p, NULL);
    if (!src || !expected) { printf("SKIP %s (missing files)\n", name); free(src); free(expected); free(input); return 0; }

    static Fake f; memset(&f, 0, sizeof f); f.dir = dir; f.input = input;
    EasecHost h = { &f, h_write, h_read_line, h_read_file, h_write_file, h_delete, h_count, h_name, h_now, h_up, h_sleep, 256 * 1024 };
    EasecVM* vm = easec_create(&h);
    easec_run(vm, name, src);
    easec_destroy(vm);
    for (int i = 0; i < f.nfiles; i++) free(f.files[i].data);

    int bad = strcmp(f.out, expected) != 0;
    printf("%s %s\n", bad ? "FAIL" : "ok  ", name);
    if (bad) printf("--- expected ---\n%s--- got ---\n%s----------------\n", expected, f.out);
    free(src); free(expected); free(input);
    return bad;
}

int main(int argc, char** argv) {
    const char* dir = argc > 1 ? argv[1] : "tests";
    char p[512]; snprintf(p, sizeof p, "%s/easec", dir);
    DIR* d = opendir(p); if (!d) { printf("cannot open %s\n", p); return 2; }
    char* names[256]; int n = 0; struct dirent* e;
    while ((e = readdir(d)) && n < 256) { size_t l = strlen(e->d_name); if (l > 6 && !strcmp(e->d_name + l - 6, ".easec") && strncmp(e->d_name, "mod_", 4)) names[n++] = strdup(e->d_name); }
    closedir(d);
    int bad = 0;
    for (int i = 0; i < n; i++) bad += run_one(dir, names[i]);
    for (int i = 0; i < n; i++) free(names[i]);
    printf(bad ? "%d test(s) FAILED\n" : "all %d test files passed\n", bad ? bad : n);
    return bad != 0;
}
