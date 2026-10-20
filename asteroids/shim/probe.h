#pragma once
#include <stdint.h>

/* Shared by probe.c and touch.c. */
extern const char *stage;
/* Guarded MMIO: a data abort is recorded and skipped; returns its ESR or 0. */
uint64_t mmio_rd32(uintptr_t a, uint32_t *v);
uint64_t mmio_wr32(uintptr_t a, uint32_t v);
uint64_t now(void);
void delay_us(unsigned us);
void delay_ms(unsigned ms);
void __attribute__((noreturn)) park(void);

/* touch.c: staged Phone (3a) touch bring-up (build with TOUCH=1). */
void touch_probe(void);
