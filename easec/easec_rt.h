/* The few C library functions Easec needs. In the kernel they come from kernel/libc.h,
 * in the host test build from the system headers. */
#ifndef EASEC_RT_H
#define EASEC_RT_H

#ifdef EASEC_HOSTED
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#else
#include "libc.h"
#endif

#endif
