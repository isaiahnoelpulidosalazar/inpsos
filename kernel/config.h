/* Layout and limits shared by the whole kernel.
 * KERNEL_SECTORS and FS_DIR_SECTOR are injected by the Makefile (-D) so the
 * bootloader, installer and pack_fs.py always agree. */
#ifndef CONFIG_H
#define CONFIG_H

#define SECTOR_SIZE        512u

#ifndef KERNEL_SECTORS
#define KERNEL_SECTORS     240u            /* sectors 1..KERNEL_SECTORS hold the kernel */
#endif
#ifndef FS_DIR_SECTOR
#define FS_DIR_SECTOR      256u            /* INPS directory sector */
#endif

#define KERNEL_LOAD_ADDR   0x10000u
#define FS_DATA_START      (FS_DIR_SECTOR + 1u)
#define FS_DATA_LIMIT      (FS_DATA_START + 16384u)   /* 8 MiB of file data */
#define FS_MAX_FILES       10
#define FS_NAME_MAX        32              /* including the NUL */
#define FS_SIGNATURE       "INPS"
#define FS_SIGNATURE_OFF   508

#define WIPE_SECTORS       (FS_DIR_SECTOR + 65u)       /* installer wipes sectors [0, WIPE_SECTORS) */

#define HEAP_START         0x100000u
#define HEAP_SIZE          (4u * 1024u * 1024u)
#define EASEC_MEM_LIMIT    (1024u * 1024u)             /* per-program memory budget */

#endif
