/*
 * Just enough flattened-device-tree parsing to read what ABL hands over:
 * /chosen, /memory and /reserved-memory children. Big-endian, 4-byte
 * aligned tokens. Extends sweet's shim/fdt.c from /chosen to any path.
 */
#include "fdt.h"

#define FDT_BEGIN_NODE	1
#define FDT_END_NODE	2
#define FDT_PROP	3
#define FDT_NOP		4
#define FDT_END		9
#define MAX_DEPTH	8

uint32_t fdt_be32(uintptr_t a)
{
	return __builtin_bswap32(*(volatile uint32_t *)a);
}

uint64_t fdt_be64(uintptr_t a)
{
	return (uint64_t)fdt_be32(a) << 32 | fdt_be32(a + 4);
}

static int streq(const char *a, const char *b)
{
	while (*a && *a == *b)
		a++, b++;
	return *a == *b;
}

static uintptr_t align4(uintptr_t a)
{
	return (a + 3) & ~3UL;
}

static uintptr_t skip_str(uintptr_t p)
{
	/* byte loads are fine on Device memory as long as they are aligned to 1 */
	while (*(volatile char *)p)
		p++;
	return p + 1;
}

/* Does node `name` match path component path[0..n)? "memory" matches "memory@80000000". */
static int comp_match(const char *name, const char *comp, int n)
{
	int i = 0;
	for (; i < n; i++)
		if (name[i] != comp[i])
			return 0;
	return name[i] == 0 || name[i] == '@';
}

uintptr_t fdt_prop(uintptr_t fdt, const char *path, const char *prop, uint32_t *len)
{
	const char *comp[MAX_DEPTH];
	int clen[MAX_DEPTH], ncomp = 0;

	if (!fdt || fdt_be32(fdt) != FDT_MAGIC)
		return 0;
	for (const char *s = path; *s;) {
		while (*s == '/')
			s++;
		if (!*s || ncomp == MAX_DEPTH)
			break;
		comp[ncomp] = s;
		while (*s && *s != '/')
			s++;
		clen[ncomp] = s - comp[ncomp];
		ncomp++;
	}

	uintptr_t p = fdt + fdt_be32(fdt + 8);		/* off_dt_struct */
	uintptr_t strings = fdt + fdt_be32(fdt + 12);	/* off_dt_strings */
	int depth = 0, matched = 0;	/* root is depth 1; comp[i] is at depth i + 2 */

	for (;;) {
		uint32_t tok = fdt_be32(p);
		p += 4;
		switch (tok) {
		case FDT_BEGIN_NODE: {
			const char *name = (const char *)p;
			depth++;
			if (depth >= 2 && matched == depth - 2 && matched < ncomp &&
			    comp_match(name, comp[matched], clen[matched]))
				matched++;
			p = align4(skip_str(p));
			break;
		}
		case FDT_END_NODE:
			if (depth >= 2 && matched >= depth - 1)
				matched = depth - 2;
			if (--depth == 0)
				return 0;
			break;
		case FDT_PROP: {
			uint32_t plen = fdt_be32(p), nameoff = fdt_be32(p + 4);
			uintptr_t val = p + 8;
			if (matched == ncomp && depth == ncomp + 1 &&
			    streq((const char *)(strings + nameoff), prop)) {
				*len = plen;
				return val;
			}
			p = align4(val + plen);
			break;
		}
		case FDT_NOP:
			break;
		default:	/* FDT_END or garbage */
			return 0;
		}
	}
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
	uint32_t len;
	uintptr_t v = fdt_prop(fdt, "/chosen", prop, &len);

	if (v && len == 4)
		*out = fdt_be32(v);
	else if (v && len == 8)
		*out = fdt_be64(v);
	else
		return 0;
	return 1;
}
