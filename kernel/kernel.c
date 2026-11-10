#include "libc.h"
#include "console.h"
#include "keyboard.h"
#include "timer.h"
#include "ahci.h"
#include "fs.h"
#include "shell.h"
#include "installer.h"

/* Waits up to timeout_ms for the given key press. */
static bool wait_for_key(uint8_t scancode, int timeout_ms) {
    int64_t end = timer_uptime_ms() + timeout_ms;
    while (timer_uptime_ms() < end)
        if (kbd_poll_scancode() == scancode) return true;
    return false;
}

void k_main(void) {
    console_init();
    heap_init();                     /* before anything that mallocs */
    timer_init();                    /* before AHCI: its timeouts use the clock */

    console_puts("Starting system...\n");
    int disks = ahci_init();
    printf("Found %d SATA disk(s). Press 'i' within 2 seconds for the installer.\n", disks);

    bool force_installer = wait_for_key(SC_KEY_I, 2000);

    if (!force_installer) {
        for (int i = 0; i < disks; i++)
            if (fs_mount(ahci_disk(i))) shell_run();        /* never returns */
    }
    installer_shell();                                       /* never returns */
}
