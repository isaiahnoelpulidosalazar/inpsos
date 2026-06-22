#include "libc.h"
#include "ahci.h"

#include "boot_bin.h"
#include "fs_bin.h"

extern void init_allocator();
extern void init_ahci();
extern void init_vm();
extern void* create_env(void* parent);
extern void run_script(const char* source, void* env);

extern HBA_Port* active_ports[32];
extern int active_port_count;

#define DIRECTORY_SECTOR 256
struct InpsFileEntry dir_cache[10];
bool is_installed_mode = false;

#define VGA_WIDTH  80
#define VGA_HEIGHT 25
#define VGA_MEM    ((volatile uint16_t*)0xB8000)

static int cursor_x = 0;
static int cursor_y = 0;
static uint8_t term_color = 0x07;

void clear_screen() {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_MEM[i] = (term_color << 8) | ' ';
    }
    cursor_x = 0; cursor_y = 0;
}

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    __asm__ volatile ("inb %1, %0" : "=a"(ret) : "Nd"(port));
    return ret;
}

void update_cursor() {
    uint16_t pos = cursor_y * VGA_WIDTH + cursor_x;
    outb(0x3D4, 0x0F); outb(0x3D5, (uint8_t)(pos & 0xFF));
    outb(0x3D4, 0x0E); outb(0x3D5, (uint8_t)((pos >> 8) & 0xFF));
}

void scroll() {
    if (cursor_y >= VGA_HEIGHT) {
        int diff = cursor_y - VGA_HEIGHT + 1;
        for (int i = 0; i < (VGA_HEIGHT - diff) * VGA_WIDTH; i++) {
            VGA_MEM[i] = VGA_MEM[i + diff * VGA_WIDTH];
        }
        for (int i = (VGA_HEIGHT - diff) * VGA_WIDTH; i < VGA_HEIGHT * VGA_WIDTH; i++) {
            VGA_MEM[i] = (term_color << 8) | ' ';
        }
        cursor_y = VGA_HEIGHT - 1;
    }
}

void kputc(char c) {
    if (cursor_y >= VGA_HEIGHT) {
        scroll();
    }
    
    if (c == '\n') { 
        cursor_x = 0; 
        cursor_y++; 
    }
    else if (c == '\r') { 
        cursor_x = 0; 
    }
    else if (c == '\b') {
        if (cursor_x > 0) {
            cursor_x--;
            VGA_MEM[cursor_y * VGA_WIDTH + cursor_x] = (term_color << 8) | ' ';
        }
    } else {
        VGA_MEM[cursor_y * VGA_WIDTH + cursor_x] = (term_color << 8) | c;
        cursor_x++;
    }
    
    if (cursor_x >= VGA_WIDTH) { 
        cursor_x = 0; 
        cursor_y++; 
    }
    
    if (cursor_y >= VGA_HEIGHT) {
        scroll();
    }
    update_cursor();
}

void kputs(const char* s) {
    while (*s) kputc(*s++);
}

char kget_char() {
    static const char layout[128] = {
        0, 0, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '=', '\b',
        '\t', 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', '[', ']', '\n',
        0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', ';', '\'', '`', 0, '\\',
        'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', '.', '/', 0, '*', 0, ' '
    };
    static const char shift_layout[128] = {
        0, 0, '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '_', '+', '\b',
        '\t', 'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '{', '}', '\n',
        0, 'A', 'S', 'D', 'F', 'G', 'H', 'J', 'K', 'L', ':', '"', '~', 0, '|',
        'Z', 'X', 'C', 'V', 'B', 'N', 'M', '<', '>', '?', 0, '*', 0, ' '
    };
    static int shift_pressed = 0;

    while (1) {
        while ((inb(0x64) & 1) == 0);
        uint8_t code = inb(0x60);
        
        if (code == 0x2A || code == 0x36) {
            shift_pressed = 1;
        } else if (code == 0xAA || code == 0xB6) {
            shift_pressed = 0;
        } else if (!(code & 0x80)) {
            char c = shift_pressed ? shift_layout[code] : layout[code];
            if (c) return c;
        }
    }
}

void read_directory(HBA_Port* disk) {
    uint32_t sector_buffer[128];
    memset(sector_buffer, 0, sizeof(sector_buffer));
    if (disk) {
        ahci_read(disk, DIRECTORY_SECTOR, 0, 1, (uint16_t*)sector_buffer);
    }
    memcpy(dir_cache, sector_buffer, sizeof(dir_cache));
}

void write_directory(HBA_Port* disk) {
    uint32_t sector_buffer[128];
    memset(sector_buffer, 0, sizeof(sector_buffer));
    memcpy(sector_buffer, dir_cache, sizeof(dir_cache));
    memcpy(&((uint8_t*)sector_buffer)[508], "INPS", 4);
    ahci_write(disk, DIRECTORY_SECTOR, 0, 1, (uint16_t*)sector_buffer);
}

bool create_file(HBA_Port* disk, const char* name, const char* content, uint32_t size) {
    read_directory(disk);
    int slot = -1;
    for (int i = 0; i < 10; i++) {
        if (!dir_cache[i].used) { slot = i; break; }
    }
    if (slot == -1) return false;
    
    uint32_t start_sec = 257;
    for (int i = 0; i < 10; i++) {
        if (dir_cache[i].used) {
            if (dir_cache[i].start_sector + dir_cache[i].num_sectors > start_sec) {
                start_sec = dir_cache[i].start_sector + dir_cache[i].num_sectors;
            }
        }
    }
    
    uint32_t num_secs = (size + 511) / 512;
    if (num_secs == 0) num_secs = 1;
    
    uint8_t* buf = malloc(num_secs * 512);
    memset(buf, 0, num_secs * 512);
    memcpy(buf, content, size);
    
    if (!ahci_write(disk, start_sec, 0, num_secs, (uint16_t*)buf)) {
        free(buf);
        return false;
    }
    free(buf);
    
    strcpy(dir_cache[slot].filename, name);
    dir_cache[slot].start_sector = start_sec;
    dir_cache[slot].num_sectors = num_secs;
    dir_cache[slot].size = size;
    dir_cache[slot].used = 1;
    
    write_directory(disk);
    return true;
}

bool delete_file(HBA_Port* disk, const char* name) {
    read_directory(disk);
    for (int i = 0; i < 10; i++) {
        if (dir_cache[i].used && strcmp(dir_cache[i].filename, name) == 0) {
            dir_cache[i].used = 0;
            write_directory(disk);
            return true;
        }
    }
    return false;
}

char* read_file(HBA_Port* disk, const char* name, uint32_t* out_size) {
    read_directory(disk);
    for (int i = 0; i < 10; i++) {
        if (dir_cache[i].used && strcmp(dir_cache[i].filename, name) == 0) {
            uint32_t num_secs = dir_cache[i].num_sectors;
            uint8_t* buf = malloc(num_secs * 512);
            if (!ahci_read(disk, dir_cache[i].start_sector, 0, num_secs, (uint16_t*)buf)) {
                free(buf); return NULL;
            }
            *out_size = dir_cache[i].size;
            char* out = malloc(dir_cache[i].size + 1);
            memcpy(out, buf, dir_cache[i].size);
            out[dir_cache[i].size] = '\0';
            free(buf);
            return out;
        }
    }
    return NULL;
}

typedef enum { VAL_NULL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_OBJ } ValType;
typedef struct {
    ValType type;
    union {
        int boolean;
        long long integer;
        double floating;
        void* obj;
    } as;
} Value;

typedef struct { void* obj; Value* items; int capacity; int count; } ObjArray;

#define OBJ_VAL(o) ((Value){VAL_OBJ, {.obj = (void*)(o)}})

extern Value make_array();
extern void env_define(void* env, const char* name, Value val);
extern void* allocate_string(const char* chars, int length);
extern void* safe_alloc(size_t size);
extern Value make_int(long long i);

void register_filesystem_env(void* env) {
    read_directory(active_ports[1]);
    int count = 0;
    for (int i = 0; i < 10; i++) {
        if (dir_cache[i].used) count++;
    }
    
    env_define(env, "file_count", make_int(count));
    
    Value arr_val = make_array();
    ObjArray* arr = (ObjArray*)arr_val.as.obj;
    arr->count = count;
    arr->capacity = count;
    arr->items = (Value*)safe_alloc(sizeof(Value) * count);
    
    int idx = 0;
    for (int i = 0; i < 10; i++) {
        if (dir_cache[i].used) {
            void* name = allocate_string(dir_cache[i].filename, strlen(dir_cache[i].filename));
            arr->items[idx++] = OBJ_VAL(name);
        }
    }
    env_define(env, "files", arr_val);
}

void run_installer() {
    kputs("\nStarting bare-metal installation of inpsos...\n");
    if (active_port_count < 1) {
        kputs("Error: No SATA hard drive detected! Ensure SATA is enabled in BIOS.\n");
        return;
    }
    HBA_Port* dest = active_ports[1];
    
    kputs("Installing custom MBR bootloader to Sector 0... ");
    uint32_t sector_buffer[128];
    memset(sector_buffer, 0, 512);
    memcpy(sector_buffer, boot_bin, boot_bin_len > 512 ? 512 : boot_bin_len);
    if (!ahci_write(dest, 0, 0, 1, (uint16_t*)sector_buffer)) {
        kputs("Failed!\n"); return;
    }
    kputs("Done.\n");
    
    kputs("Deploying kernel image (Sectors 1 to 240) directly from physical memory... ");
    uint8_t* ram_kernel = (uint8_t*)0x10000;
    for (int s = 1; s <= 240; s++) {
        uint8_t* ram_offset = ram_kernel + (s - 1) * 512;
        if (!ahci_write(dest, s, 0, 1, (uint16_t*)ram_offset)) {
            kputs("Failed!\n"); return;
        }
    }
    kputs("Done.\n");
    
    kputs("Deploying packed filesystem files directly from RAM (Sectors 256 to 320)... ");
    int fs_sectors = fs_bin_len / 512;
    if (fs_sectors == 0) fs_sectors = 1;
    
    for (int s = 0; s < fs_sectors; s++) {
        memset(sector_buffer, 0, 512);
        int bytes_to_copy = fs_bin_len - (s * 512);
        if (bytes_to_copy > 512) bytes_to_copy = 512;
        memcpy(sector_buffer, fs_bin + (s * 512), bytes_to_copy);
        
        if (s == 0) {
            memcpy(&((uint8_t*)sector_buffer)[508], "INPS", 4);
        }
        
        if (!ahci_write(dest, 256 + s, 0, 1, (uint16_t*)sector_buffer)) {
            kputs("Failed!\n"); return;
        }
    }
    kputs("Done.\n");
    
    kputs("\nSUCCESS! inpsos has been physically installed to your hard drive.\n");
    kputs("Please remove your USB installation drive and restart your computer!\n");
}

void k_main() {
    clear_screen();
    kputs("==================================================\n");
    kputs("          INPSOS OPERATING SYSTEM                  \n");
    kputs("==================================================\n");
    
    init_allocator();
    init_ahci();
    
    bool force_installer = false;
    
    kputs("Press 'i' to enter Installer Mode (Booting in 2s)... ");
    long long start_wait = get_time_ms();
    int last_seconds_left = 2;
    printf("%d ", last_seconds_left);
    
    while (1) {
        long long elapsed = get_time_ms() - start_wait;
        if (elapsed >= 2000) break;
        
        int seconds_left = 2 - (int)(elapsed / 1000);
        if (seconds_left < last_seconds_left && seconds_left > 0) {
            printf("%d ", seconds_left);
            last_seconds_left = seconds_left;
        }
        
        if (inb(0x64) & 1) {
            uint8_t scancode = inb(0x60);
            if (scancode == 0x17) {
                force_installer = true;
                break;
            }
        }
    }
    kputs("\n\n");
    
    HBA_Port* installed_port = NULL;
    uint32_t sector_buffer[128];
    
    for (int i = 0; i < active_port_count; i++) {
        HBA_Port* port = active_ports[i];
        if (ahci_read(port, DIRECTORY_SECTOR, 0, 1, (uint16_t*)sector_buffer)) {
            uint8_t* byte_ptr = (uint8_t*)sector_buffer;
            if (memcmp(&byte_ptr[508], "INPS", 4) == 0) {
                installed_port = port;
                break;
            }
        }
    }
    
    if (force_installer) {
        is_installed_mode = false;
        if (active_port_count == 1) {
            active_ports[1] = active_ports[0];
        }
    } else {
        if (installed_port) {
            is_installed_mode = true;
            active_ports[1] = installed_port;
        } else {
            is_installed_mode = false;
            if (active_port_count == 1) {
                active_ports[1] = active_ports[0];
            }
        }
    }
    
    if (is_installed_mode) {
        kputs("Booted from primary hard drive. inpsos is active.\n");
        kputs("Type a program name to execute (e.g. list.easec)\n\n");
    } else {
        kputs("Booted from installation USB medium.\n");
        kputs("Type 'install' to physically write inpsos to your hard drive.\n\n");
    }
    
    init_vm();
    void* global_env = create_env(NULL);
    char input_buffer[256];
    
    while (1) {
        if (is_installed_mode) kputs("inpsos$ "); else kputs("installer$ ");
        fgets(input_buffer, sizeof(input_buffer), stdin);
        
        int len = strlen(input_buffer);
        while (len > 0 && (input_buffer[len-1] == '\n' || input_buffer[len-1] == '\r')) {
            input_buffer[len-1] = '\0';
            len--;
        }
        if (strlen(input_buffer) == 0) continue;
        
        if (!is_installed_mode) {
            if (strcmp(input_buffer, "install") == 0) {
                run_installer();
            } else {
                kputs("Unsupported command. The ISO environment only supports 'install'.\n");
            }
        } else {
            uint32_t fsize = 0;
            
            if (strcmp(input_buffer, "help") != 0 && strcmp(input_buffer, "install") != 0) {
                printf("[DIAGNOSTIC] 1. Searching for '%s' on disk...\n", input_buffer);
                
                char* script_src = read_file(active_ports[1], input_buffer, &fsize);
                
                if (script_src) {
                    printf("[DIAGNOSTIC] 2. Found! Loaded %d bytes from sectors.\n", fsize);
                    
                    printf("[DIAGNOSTIC] 3. Registering filesystem variables to VM...\n");
                    register_filesystem_env(global_env);
                    
                    printf("[DIAGNOSTIC] 4. Parsing and compiling script...\n");
                    extern int had_runtime_error;
                    had_runtime_error = 0;
                    
                    run_script(script_src, global_env);
                    
                    printf("[DIAGNOSTIC] 5. Script finished execution.\n");
                    free(script_src);
                } else {
                    printf("[DIAGNOSTIC] File '%s' not found on storage.\n", input_buffer);
                }
            } else if (strcmp(input_buffer, "help") == 0) {
                kputs("Available programs on storage:\n");
                read_directory(active_ports[1]);
                for (int i = 0; i < 10; i++) {
                    if (dir_cache[i].used) printf("  - %s (%d bytes)\n", dir_cache[i].filename, dir_cache[i].size);
                }
            } else if (strcmp(input_buffer, "install") == 0) {
                run_installer();
            }
        }
    }
}