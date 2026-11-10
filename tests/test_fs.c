/* Host-side test of the INPS filesystem against a RAM disk. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "fs.h"

static uint8_t* ram;
static uint32_t ram_sectors = FS_DATA_LIMIT + 8;

static bool rd(void* c, uint32_t lba, uint32_t n, void* b) { (void)c; if (lba + n > ram_sectors) return false; memcpy(b, ram + (size_t)lba * 512, (size_t)n * 512); return true; }
static bool wr(void* c, uint32_t lba, uint32_t n, const void* b) { (void)c; if (lba + n > ram_sectors) return false; memcpy(ram + (size_t)lba * 512, b, (size_t)n * 512); return true; }

static int failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

static char* rd_all(const char* n, size_t* sz) { return fs_read(n, sz); }

int main(void) {
    ram = calloc(ram_sectors, 512);
    BlockDev d = { NULL, rd, wr, NULL };

    CHECK(!fs_mount(&d));                                   /* unformatted disk is rejected */
    memcpy(ram + (size_t)FS_DIR_SECTOR * 512 + FS_SIGNATURE_OFF, FS_SIGNATURE, 4);
    CHECK(fs_mount(&d));
    CHECK(fs_count() == 0);

    CHECK(fs_write("a.txt", "hello", 5));
    size_t n; char* s = rd_all("a.txt", &n);
    CHECK(s && n == 5 && strcmp(s, "hello") == 0); free(s);

    /* replacing must not create a second entry (original bug) */
    CHECK(fs_write("a.txt", "world!!", 7));
    CHECK(fs_count() == 1);
    s = rd_all("a.txt", &n); CHECK(s && n == 7 && strcmp(s, "world!!") == 0); free(s);

    /* append */
    CHECK(fs_append("a.txt", "XY", 2));
    s = rd_all("a.txt", &n); CHECK(s && n == 9 && strcmp(s, "world!!XY") == 0); free(s);
    CHECK(fs_append("new.txt", "abc", 3));
    s = rd_all("new.txt", &n); CHECK(s && strcmp(s, "abc") == 0); free(s);

    /* growing across sector boundaries relocates the file; content stays intact */
    char* big = malloc(5000); for (int i = 0; i < 5000; i++) big[i] = (char)('a' + i % 26);
    CHECK(fs_write("a.txt", big, 5000));
    s = rd_all("a.txt", &n); CHECK(s && n == 5000 && memcmp(s, big, 5000) == 0); free(s);
    s = rd_all("new.txt", &n); CHECK(s && strcmp(s, "abc") == 0); free(s);  /* neighbour untouched */

    /* deleted space is reused (original never reclaimed it) */
    CHECK(fs_delete("new.txt"));
    CHECK(!fs_exists("new.txt"));
    CHECK(fs_write("b.txt", "bbb", 3));
    s = rd_all("b.txt", &n); CHECK(s && strcmp(s, "bbb") == 0); free(s);

    /* exactly one sector, zero bytes, and exact multiples */
    char* sec = malloc(1024); memset(sec, 'q', 1024);
    CHECK(fs_write("s512", sec, 512));  s = rd_all("s512", &n); CHECK(s && n == 512 && memcmp(s, sec, 512) == 0); free(s);
    CHECK(fs_write("s1024", sec, 1024)); s = rd_all("s1024", &n); CHECK(s && n == 1024 && memcmp(s, sec, 1024) == 0); free(s);
    CHECK(fs_write("empty", "", 0));    s = rd_all("empty", &n); CHECK(s && n == 0); free(s);

    /* name validation */
    CHECK(!fs_write("", "x", 1));
    CHECK(!fs_write("this_name_is_way_too_long_for_the_32_byte_field", "x", 1));
    CHECK(!fs_write("has space", "x", 1));

    /* directory capacity: 10 entries */
    int made = 0; char name[16];
    for (int i = 0; i < 20; i++) { snprintf(name, sizeof name, "f%d", i); if (fs_write(name, "z", 1)) made++; }
    CHECK(fs_count() == FS_MAX_FILES);
    CHECK(made + 5 /* a.txt b.txt s512 s1024 empty */ == FS_MAX_FILES);

    /* persistence: remount from the raw bytes */
    CHECK(fs_mount(&d));
    s = rd_all("a.txt", &n); CHECK(s && n == 5000 && memcmp(s, big, 5000) == 0); free(s);

    free(big); free(sec); free(ram);
    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("test_fs: all passed\n");
    return 0;
}
