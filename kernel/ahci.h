#ifndef AHCI_H
#define AHCI_H

#include <stdint.h>
#include "blockdev.h"

#define AHCI_MAX_DISKS 4

/* Probes PCI for an AHCI controller and brings up every attached SATA disk.
 * Returns the number of disks found (0 if none / no controller). */
int ahci_init(void);
int ahci_disk_count(void);
/* The returned BlockDev stays valid for the lifetime of the kernel. */
const BlockDev* ahci_disk(int index);

#endif
