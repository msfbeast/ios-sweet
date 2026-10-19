#pragma once
#include <stdint.h>

/* The SWEETXNU payload appended to this image (tools/mkpayload.py), or 0. */
uintptr_t loader_find(void);

/* Act as iBoot for xnu; returns only on error. fb = live splash base. */
const char *loader_boot(uintptr_t payload, uintptr_t fb);
