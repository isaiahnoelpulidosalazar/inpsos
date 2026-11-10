#include "shell.h"
#include "libc.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "fs.h"
#include "easec.h"

/* ---- the kernel's Easec host: this is the ONLY place where the interpreter touches the OS ---- */
static void h_write(void* c, const char* s, size_t n) { (void)c; console_write(s, n); }
static bool h_read_line(void* c, char* buf, size_t cap) { (void)c; console_readline(buf, cap); return true; }
static char* h_read_file(void* c, const char* path, size_t* len) { (void)c; return fs_read(path, len); }
static bool h_write_file(void* c, const char* path, const char* d, size_t n, bool append) {
    (void)c;
    return append ? fs_append(path, d, n) : fs_write(path, d, n);
}
static bool h_delete_file(void* c, const char* path) { (void)c; return fs_delete(path); }
static int h_file_count(void* c) { (void)c; return fs_count(); }
static const char* h_file_name(void* c, int i) { (void)c; return fs_name(i); }
static int64_t h_now(void* c) { (void)c; return timer_unix_ms(); }
static int64_t h_uptime(void* c) { (void)c; return timer_uptime_ms(); }
static void h_sleep(void* c, int64_t ms) { (void)c; timer_sleep_ms(ms); }

static const EasecHost kernel_host = {
    NULL, h_write, h_read_line, h_read_file, h_write_file, h_delete_file,
    h_file_count, h_file_name, h_now, h_uptime, h_sleep, EASEC_MEM_LIMIT,
};

size_t shell_read_command(const char* prompt, char* buf, size_t cap) {
    console_puts(prompt);
    console_readline(buf, cap);
    size_t start = 0, end = strlen(buf);
    while (start < end && buf[start] == ' ') start++;
    while (end > start && buf[end - 1] == ' ') end--;
    memmove(buf, buf + start, end - start);
    buf[end - start] = '\0';
    return end - start;
}

/* A fresh VM per program: all memory is reclaimed when it exits, and one program's variables
 * can no longer leak into the next (the original reused one global environment forever). */
static void run_program(const char* name) {
    char file[FS_NAME_MAX];
    if (strlen(name) + 7 > sizeof file || snprintf(file, sizeof file, "%s.easec", name) < 0 || !fs_valid_name(file)) {
        printf("Invalid program name '%s'.\n", name);
        return;
    }
    size_t len;
    char* src = fs_read(file, &len);
    if (!src) { printf("Program '%s' not found. Type 'help'.\n", name); return; }

    EasecVM* vm = easec_create(&kernel_host);
    if (!vm) { printf("Out of memory.\n"); free(src); return; }
    if (!easec_run(vm, file, src)) printf("[%s exited with an error]\n", name);
    easec_destroy(vm);
    free(src);
}

void shell_run(void) {
    char line[128];
    console_puts("inpsos booted.\n\n");
    for (;;) {
        if (shell_read_command("inpsos> ", line, sizeof line) == 0) continue;
        if (strcmp(line, "clear") == 0) console_clear();
        else if (strcmp(line, "reboot") == 0) kbd_reboot();
        else run_program(line);
    }
}
