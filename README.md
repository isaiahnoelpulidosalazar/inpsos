# inpsos

**inpsos** is a lightweight, custom 32-bit x86 operating system featuring a modular architecture, an embedded standard library, a custom storage driver, and its own proprietary compiled scripting language called **Easec**. 

The operating system boots via a custom Master Boot Record (MBR) bootloader and runs a single-process user interface shell that allows you to manage files and execute Easec programs.

---

## Architecture Overview

The operating system is built on x86-compatible PC hardware and implements a minimal, self-contained architecture:

```
┌─────────────────────────────────────────────────────────────────┐
│                     Easec Interpreter / VM                      │
│     (AST-Compiler, Stack-VM, Arena Allocator, Mark-Sweep GC)    │
├─────────────────────────────────────────────────────────────────┤
│                            OS Shell                             │
├───────────────────────────────┬─────────────────────────────────┤
│   Custom Filesystem (INPS)    │      Custom libc Standard       │
│   (Sector 256+ Allocation,    │   (First-Fit Heap Allocator,    │
│        10-File Limit)         │   KFILE, timing, string API)    │
├───────────────────────────────┴─────────────────────────────────┤
│                    PCI AHCI SATA Disk Driver                    │
├─────────────────────────────────────────────────────────────────┤
│                       x86 Kernel Core                           │
│   (VGA Text Mode, GDT, keyboard controller, port-mapped I/O)    │
└─────────────────────────────────────────────────────────────────┘
```

### Key Components

1. **Kernel (`kernel.c`)**: Initializes the system, parses hardware drivers, manages the 2-second boot timeout, hosts the interactive installer and installed shell prompts, and binds the Easec VM interface.
2. **PCI & AHCI SATA Driver (`ahci.c` / `ahci.h`)**: Scans the PCI bus for AHCI controllers, initializes disk ports, and uses Direct Memory Access (DMA) to read and write sectors of the virtual hard drive.
3. **Custom libc (`libc.c` / `libc.h`)**: Since the system operates on bare metal, it implements its own standard C functions. It features a custom first-fit memory allocator (for `malloc`, `free`, and `realloc`), basic string libraries, formatting buffers (`printf`, `snprintf`), timing logic, and a `KFILE` stream implementation mapping files to disk sectors.
4. **Easec Compiler & VM (`easec.c`)**: A fully realized language runtime. It includes a lexical analyzer (lexer), a recursive-descent compiler parsing to a stack-oriented bytecode system, an AST arena allocator to prevent leaks, a VM executor, and a mark-and-sweep garbage collector.
5. **Packer Tool (`pack_fs.py`)**: A Python utility that compiles user scripts located in the `programs/` directory into a binary file-system image (`fs.bin`) and auto-generates C header files to embed them into the initial installation media.

---

## Boot & Installation Flow

### 1. Interactive Boot Options
When the system starts, it displays "Starting system..." and begins a **2-second countdown**. 
* **Forcing Installer Mode**: Press the **`i`** key (scancode `0x17`) during this countdown.
* **Booting Installed System**: If you do not press `i`, the system searches your SATA drive for the filesystem signature `"INPS"`. If found, it skips the installer and loads directly into your installed OS shell.

### 2. The Installer Command
If no installation is detected, or if you forced the installer, you will reach the `installer>` prompt. Write the OS to your hard drive by entering:
```bash
installer> install
```
The installer will:
1. Format sectors 0 to 320 of your target SATA drive.
2. Install the custom MBR bootloader to sector 0.
3. Write the compiled kernel binary to sectors 1-240.
4. Deploy the packed filesystem containing your `.easec` programs to Sector 256+.
5. Flush the drive's physical write cache.

Once complete, remove your installation CD-ROM (or kill the virtual ISO drive) and restart.

---

## The INPS Filesystem

The custom file system (signature `"INPS"`) is mapped directly to **Sector 256** of your storage drive.
* Up to **10 files** can be cached and indexed in the directory table at any time.
* Each directory entry contains a 32-character name, a starting sector index, sector span, and exact file size.
* The system shell natively auto-completes run requests to `.easec` scripts. To launch a script named `test.easec`, simply type:
  ```bash
  inpsos> test
  ```

---

## Easec Language Reference

Easec is an embedded, compiled, lexically scoped language featuring implicit method calling, dynamic structures, and low-level system integrations.

### Variables & Stdin Input
Unlike many languages, **variable declarations in Easec do not use `=`**. Initializations use space separators. Assignment statements to existing variables, however, do require `=`.

```
// Declaration (No "=")
var text msg "Hello, inpsos!"
var number x 42
var decimal pi 3.14159
var boolean flag true

// Variable Assignment (Uses "=")
x = 100

// Capture user input from stdin console
var text user_input get
say "You entered: " + user_input
```

### Arrays
Arrays are dynamic collections of values.
```
// Declare an array
array text my_arr "Red", "Green", "Blue"

// Read an element
var text element array get my_arr 1
say element // Output: Green

// Mutate an element
array set my_arr 1 "Yellow"
```

### Dictionaries
Dictionaries store key-value pairs where keys are identifiers.
```
// Declare a dictionary
dictionary text config port: 8080, host: "localhost"

// Retrieve a value
var text host_val dictionary get config host
say host_val // Output: localhost

// Mutate or write a key
dictionary set config host "127.0.0.1"
```

### Jobs (Functions)
Functions in Easec are called **Jobs**. Return values are outputted using the `out` keyword.
```
job add a, b [
    out a + b
]

var number total add 15, 25
say total // Output: 40
```

### Control Flow
```
// Conditional checks
if x > 50 [
    say "x is large"
] else [
    say "x is small"
]

// Fixed repeats
repeat 5 [
    say "Repeating..."
]

// Infinite loops
repeat forever [
    say "Infinite"
]
```

### System & Filesystem Bindings
Easec interacts directly with the custom storage driver via file statements.
```
// Create or overwrite a file
file create "notes.txt" "Log data inside inpsos"

// Append to a file
file update "notes.txt" "\nAdditional log data"

// Remove a file
file delete "notes.txt"

// Fetch list of files pre-registered by OS environment
// 'files' is a global array mapped to the filesystem directory table
var number total_files file_count
say "Total files on disk: " + total_files
```

### Time and Modules
```
// Timing routines
var number start time get
time sleep 1000 // Sleep for 1000ms
var number duration (time get) - start

// Modular importing
import "math.easec" as m
var number result m.calculate 5
```