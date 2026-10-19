#pragma once
#include <stdint.h>

#define FDT_MAGIC	0xd00dfeed

uint32_t fdt_be32(uintptr_t a);
uint64_t fdt_be64(uintptr_t a);

/*
 * Find property `prop` of the node at `path` ("/chosen",
 * "/reserved-memory/splash_region"). A path component also matches a node
 * named "<component>@<unit>". Returns the value address and sets *len,
 * or 0 if the node or property is absent.
 */
uintptr_t fdt_prop(uintptr_t fdt, const char *path, const char *prop, uint32_t *len);

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
