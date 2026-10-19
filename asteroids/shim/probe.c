/*
 * asteroids M1: one read-only boot answers what the XNU port needs to know
 * about the Phone (3a), on the ABL framebuffer (film it):
 *   - CPU feature registers, exception level, timer frequency
 *   - the live FDT from ABL: memory banks, splash region, initrd, panel
 *   - where our marker landed in the v4 initrd (vendor ramdisk comes first)
 *   - PSCI/SMCCC, and PSCI CPU_ON for the 7 secondaries (MPIDR + MIDR each)
 *   - GICv3 at 0x17100000 / 0x17180000 (stride 0x40000), virtual timer
 *     INTID 27 and SGI 1 delivered as Group 1 IRQs
 *   - UFS host controller version (read only), last because a dead clock
 *     would hang the bus
 * Nothing is written to storage or PMIC. The screen stays up until
 * Power + Volume Down. Any unexpected exception on the boot CPU paints a
 * blue panic screen; a secondary's fault is recorded and that core parks.
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"

#define GICD_BASE		0x17100000UL
#define GICR_BASE		0x17180000UL
#define GICR_SIZE		0x200000UL
#define GICR_PE_SIZE		0x40000UL	/* GICv4 frames; sweet uses 0x20000 */
#define GICD_TYPER		0x0004
#define GICD_IIDR		0x0008
#define GICD_PIDR2		0xffe8
#define GICR_TYPER		0x08
#define GICR_WAKER		0x14
#define GICR_SGI		0x10000
#define GICR_IGROUPR0		(GICR_SGI + 0x080)
#define GICR_ISENABLER0		(GICR_SGI + 0x100)
#define GICR_ICENABLER0		(GICR_SGI + 0x180)
#define GICR_IPRIORITYR		(GICR_SGI + 0x400)
#define GICR_ISPENDR0		(GICR_SGI + 0x200)
#define GICR_ISACTIVER0		(GICR_SGI + 0x300)
#define VTIMER_INTID		27	/* DT: <GIC_PPI 11 ...> */
#define SGI_INTID		1

#define UFS_BASE		0x01d84000UL
#define UFS_CAP			0x00
#define UFS_VER			0x08
#define UFS_HCS			0x30
#define UFS_HCE			0x34

#define PSCI_VERSION		0x84000000UL
#define PSCI_CPU_OFF		0x84000002UL
#define PSCI_CPU_ON		0xc4000003UL
#define PSCI_FEATURES		0x8400000aUL
#define PSCI_SYSTEM_RESET2	0xc4000012UL
#define SMCCC_VERSION		0x80000000UL

#define NR_CPUS			8
#define PROBE_MAGIC		0x424f525033545341ULL	/* "AST3PROB" little-endian */

#define rsr(r) ({ uint64_t _v; __asm__ volatile("mrs %0, " r : "=r"(_v)); _v; })
#define wsr(r, v) __asm__ volatile("msr " r ", %0" : : "r"((uint64_t)(v)))
#define isb() __asm__ volatile("isb" ::: "memory")

static inline uint32_t rd32(uintptr_t a) { return *(volatile uint32_t *)a; }
static inline uint64_t rd64(uintptr_t a) { return *(volatile uint64_t *)a; }
static inline void wr32(uintptr_t a, uint32_t v) { *(volatile uint32_t *)a = v; }
static inline void wr8(uintptr_t a, uint8_t v) { *(volatile uint8_t *)a = v; }

extern char secondary_entry[];

static const char *stage;	/* set at runtime: no absolute pointers in data */
static uint64_t boot_mpidr;
static volatile uint64_t sec_mpidr[NR_CPUS], sec_midr[NR_CPUS], sec_fault[NR_CPUS];
static volatile uint32_t irq_count, irq_other, irq_last;
static volatile int irq_src;

static uint64_t smc(uint64_t fn, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fn;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;
	__asm__ volatile("smc #0"
			 : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3)
			 :
			 : "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11",
			   "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static uint64_t now(void)
{
	isb();
	return rsr("cntpct_el0");
}

static void delay_ms(unsigned ms)
{
	uint64_t end = now() + rsr("cntfrq_el0") / 1000 * ms;
	while (now() < end)
		;
}

static void __attribute__((noreturn)) park(void)
{
	for (;;)
		__asm__ volatile("wfe");
}

/* ICC_SGI1R_EL1 value targeting the calling core only. */
static uint64_t sgi_self(unsigned id)
{
	uint64_t m = rsr("mpidr_el1");
	return ((m >> 16) & 0xff) << 32 |	/* Aff2 */
	       ((m >> 8) & 0xff) << 16 |	/* Aff1 */
	       (uint64_t)id << 24 |
	       (1U << (m & 0xf));		/* target list: Aff0 */
}

/* ---- exceptions ---- */

static void irq_handler(void)
{
	uint64_t iar = rsr("icc_iar1_el1");
	uint32_t id = iar & 0xffffff;

	if (id >= 1020 && id <= 1023)
		return;	/* spurious */
	irq_last = id;
	if (id == (uint32_t)irq_src)
		irq_count++;
	else
		irq_other++;
	if (id == VTIMER_INTID)
		wsr("cntv_tval_el0", rsr("cntfrq_el0") / 100);	/* 10 ms */
	wsr("icc_eoir1_el1", iar);
	isb();
}

void exc_handler(uint64_t slot, uint64_t *regs)
{
	uint64_t me = rsr("mpidr_el1") & 0xffffff;

	if (slot == 5) {	/* IRQ from current EL, SPx */
		irq_handler();
		return;
	}
	if (me != boot_mpidr) {
		unsigned idx = (me >> 8) & 7;
		sec_fault[idx] = rsr("esr_el1") | (1ULL << 63);
		park();
	}
	con_clear(BLUE);
	con_skip();
	con_skip();
	switch (slot & 3) {
	case 0: con_str("PANIC", "SYNC"); break;
	case 1: con_str("PANIC", "IRQ"); break;
	case 2: con_str("PANIC", "FIQ"); break;
	default: con_str("PANIC", "SERROR"); break;
	}
	con_hex2("SLOT", "n=", slot, "cpu=", me);
	con_str("STAGE", stage);
	con_hex("ESR", rsr("esr_el1"));
	con_hex("ELR", rsr("elr_el1"));
	con_hex("FAR", rsr("far_el1"));
	con_hex("SPSR", rsr("spsr_el1"));
	con_hex("X0", regs[0]);
	con_hex("X1", regs[1]);
	con_skip();
	con_str("HOLD", "Power + Vol- to leave");
	park();
}

/* ---- CPU and FDT ---- */

static void show_cpu(void)
{
	stage = "cpu regs";
	con_hex2("EL/MIDR", "el=", (rsr("currentel") >> 2) & 3, "midr=", rsr("midr_el1"));
	con_hex2("MPIDR", "lo=", rsr("mpidr_el1"), "cntfrq=", rsr("cntfrq_el0"));
	uint64_t mmfr0 = rsr("id_aa64mmfr0_el1"), pfr0 = rsr("id_aa64pfr0_el1");
	con_hex2("MMFR0", "pa=", mmfr0 & 0xf, "tg16=", (mmfr0 >> 20) & 0xf);
	con_hex2("PFR0", "gic=", (pfr0 >> 24) & 0xf, "el2=", (pfr0 >> 8) & 0xf);
}

/* Print up to `max` chars of a string at `s` that stop at `end` or 0. */
static void show_text(const char *label, const char *s, char end, int max)
{
	char buf[40];
	int i = 0;

	if (max > (int)sizeof(buf) - 1)
		max = sizeof(buf) - 1;
	for (; i < max && s[i] && s[i] != end; i++)
		buf[i] = s[i];
	buf[i] = 0;
	con_str(label, buf);
}

static const char *find_str(const char *hay, uint32_t len, const char *needle)
{
	for (uint32_t i = 0; i < len; i++) {
		uint32_t j = 0;
		while (needle[j] && i + j < len && hay[i + j] == needle[j])
			j++;
		if (!needle[j])
			return hay + i + j;
	}
	return 0;
}

static void show_fdt(uintptr_t dtb)
{
	uint32_t len;
	uintptr_t v;

	stage = "fdt";
	if (!dtb || fdt_be32(dtb) != FDT_MAGIC) {
		con_hex("DTB BAD", dtb);
		return;
	}
	con_hex2("DTB", "hi=", dtb >> 32, "lo=", dtb);
	if ((v = fdt_prop(dtb, "/", "model", &len)))
		show_text("MODEL", (const char *)v, 0, 35);

	v = fdt_prop(dtb, "/memory", "reg", &len);
	for (uint32_t i = 0; v && i < len / 16 && i < 8; i++) {
		char label[] = "MEM0";
		label[3] = '0' + i;
		uint64_t base = fdt_be64(v + i * 16), size = fdt_be64(v + i * 16 + 8);
		con_hex2(label, "at=", base >> 12, "pg=", size >> 12);	/* 4K pages */
	}
	if (!v)
		con_str("MEM", "no /memory reg");

	if ((v = fdt_prop(dtb, "/reserved-memory/splash_region", "reg", &len)) && len == 16) {
		con_hex2("SPLASH", "at=", fdt_be64(v), "size=", fdt_be64(v + 8));
		con_fb = (volatile uint32_t *)(uintptr_t)fdt_be64(v);
	} else {
		con_str("SPLASH", "not in DT, using 0xe3940000");
	}

	if ((v = fdt_prop(dtb, "/chosen", "bootargs", &len))) {
		const char *panel = find_str((const char *)v, len, "msm_drm.dsi_display0=");
		const char *name = panel ? find_str(panel, 16, "qcom,mdss_dsi_") : 0;
		if (panel)
			show_text("PANEL", name ? name : panel, ' ', 35);	/* rm69220_..._vxn / _boe */
		else
			con_str("PANEL", "no dsi_display0 in bootargs");
	}
}

static void show_initrd(uintptr_t dtb)
{
	uint64_t lo, hi;

	stage = "initrd";
	if (!fdt_chosen_u64(dtb, "linux,initrd-start", &lo) ||
	    !fdt_chosen_u64(dtb, "linux,initrd-end", &hi)) {
		con_str("INITRD", "none in /chosen");
		return;
	}
	con_hex2("INITRD", "lo=", lo, "hi=", hi);
	/* 4-byte steps and 32-bit reads: the vendor ramdisk is 4 mod 8 bytes long,
	 * so the marker may sit at a 4-aligned address (no unaligned 64-bit loads
	 * with the MMU off). */
	for (uint64_t a = (lo + 3) & ~3ULL; a + 8 <= hi; a += 4)
		if (rd32(a) == (uint32_t)PROBE_MAGIC && rd32(a + 4) == (uint32_t)(PROBE_MAGIC >> 32)) {
			con_hex2("MARKER", "off=", a - lo, "of=", hi - lo);
			return;
		}
	con_str("MARKER", "not found in initrd");
}

/* ---- SMP ---- */

void secondary_main(uint64_t idx)
{
	sec_midr[idx & 7] = rsr("midr_el1");
	sec_mpidr[idx & 7] = rsr("mpidr_el1") | (1ULL << 63);
	__asm__ volatile("dsb sy" ::: "memory");
	smc(PSCI_CPU_OFF, 0, 0, 0);
	park();
}

static void test_smp(void)
{
	int64_t ret[NR_CPUS];

	stage = "smp";
	for (int i = 1; i < NR_CPUS; i++)
		ret[i] = smc(PSCI_CPU_ON, (uint64_t)i << 8, (uintptr_t)secondary_entry, i);
	delay_ms(500);
	for (int i = 1; i < NR_CPUS; i++) {
		char label[] = "CPU0";
		label[3] = '0' + i;
		if (sec_fault[i])
			con_hex2(label, "FAULT esr=", (uint32_t)sec_fault[i], 0, 0);
		else if (sec_mpidr[i])
			con_hex2(label, "mpidr=", (uint32_t)sec_mpidr[i], "midr=", (uint32_t)sec_midr[i]);
		else
			con_hex2(label, "ret=", (uint32_t)ret[i], "no answer", 0);
	}
}

/* ---- GIC + timer ---- */

static uintptr_t find_gicr(void)
{
	uint64_t aff = rsr("mpidr_el1");
	uint32_t want = (aff & 0xffffff) | ((aff >> 8) & 0xff000000);

	for (uintptr_t off = 0; off < GICR_SIZE; off += GICR_PE_SIZE) {
		uint64_t typer = rd64(GICR_BASE + off + GICR_TYPER);
		if ((uint32_t)(typer >> 32) == want)
			return GICR_BASE + off;
		if (typer & (1 << 4))	/* Last */
			break;
	}
	return 0;
}

static void gicd_wait_rwp(void)
{
	for (int i = 0; i < 1000000 && (rd32(GICD_BASE) & (1U << 31)); i++)
		;
}

/* Run one interrupt source for ~300 ms and report what the GIC saw. */
static void irq_window(const char *label, uintptr_t r, int src)
{
	uint64_t f = rsr("cntfrq_el0");

	irq_count = irq_other = 0;
	irq_last = 0x3ff;
	irq_src = src;
	if (src == VTIMER_INTID) {
		wsr("cntv_tval_el0", f / 100);
		wsr("cntv_ctl_el0", 1);
	}
	isb();
	uint64_t t = now() + f / 50;
	while (now() < t)
		;
	if (src == SGI_INTID)
		wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
	isb();
	uint32_t pend = rd32(r + GICR_ISPENDR0);

	__asm__ volatile("msr daifclr, #2" ::: "memory");
	t = now() + f * 3 / 10;
	/* SGI: one already sent above, send 9 more 10 ms apart -> expect 10 */
	uint64_t next = now() + f / 100;
	for (int sent = 1; now() < t;)
		if (src == SGI_INTID && sent < 10 && now() >= next) {
			wsr("icc_sgi1r_el1", sgi_self(SGI_INTID));
			isb();
			sent++;
			next += f / 100;
		}
	__asm__ volatile("msr daifset, #2" ::: "memory");
	wsr("cntv_ctl_el0", 0);
	isb();

	con_hex2(label, "irqs=", irq_count, "other=", irq_other);
	con_hex2(" pre", "pend=", pend, "last=", irq_last);
}

static void test_gic(void)
{
	stage = "gic sre";
	uint64_t sre = rsr("icc_sre_el1");
	if (!(sre & 1)) {
		wsr("icc_sre_el1", sre | 1);
		isb();
	}
	if (!(rsr("icc_sre_el1") & 1)) {
		con_str("GIC", "no sysreg iface, skip");
		return;
	}

	stage = "gic dist";
	uint32_t dctlr = rd32(GICD_BASE);
	con_hex2("GICD", "typer=", rd32(GICD_BASE + GICD_TYPER), "iidr=", rd32(GICD_BASE + GICD_IIDR));
	con_hex2(" ", "pidr2=", rd32(GICD_BASE + GICD_PIDR2), "ctlr=", dctlr);
	/* Non-secure view (DS=0): bit0 EnableGrp1, bit1 EnableGrp1A, bit4 ARE_NS; Linux writes 0x13 */
	wr32(GICD_BASE, dctlr | 0x13);
	gicd_wait_rwp();

	stage = "gic redist";
	uintptr_t r = find_gicr();
	if (!r) {
		con_str("GICR", "not found at stride 0x40000");
		return;
	}
	uint32_t waker = rd32(r + GICR_WAKER);
	if (waker & 2) {
		wr32(r + GICR_WAKER, waker & ~2U);
		for (int i = 0; i < 100000 && (rd32(r + GICR_WAKER) & 4); i++)
			;
	}
	con_hex2("GICR", "base=", r, "waker=", rd32(r + GICR_WAKER));

	stage = "gic config";
	wr32(r + GICR_IGROUPR0, 0xffffffff);	/* NS view: secure bits are WI */
	for (int id = 0; id < 32; id++)
		wr8(r + GICR_IPRIORITYR + id, 0xa0);
	wr32(r + GICR_ISENABLER0, (1U << VTIMER_INTID) | (1U << SGI_INTID));
	wsr("icc_ctlr_el1", rsr("icc_ctlr_el1") & ~2ULL);	/* EOImode 0 */
	wsr("icc_pmr_el1", 0xf0);
	wsr("icc_bpr1_el1", 0);
	wsr("icc_igrpen1_el1", 1);
	isb();

	stage = "gic vtimer";
	irq_window("VTIMER27", r, VTIMER_INTID);
	stage = "gic sgi";
	irq_window("SGI1", r, SGI_INTID);
	wr32(r + GICR_ICENABLER0, 0xffffffff);
	isb();
}

static void test_ufs(void)
{
	stage = "ufs read";
	con_hex2("UFS", "ver=", rd32(UFS_BASE + UFS_VER), "cap=", rd32(UFS_BASE + UFS_CAP));
	con_hex2(" ", "hcs=", rd32(UFS_BASE + UFS_HCS), "hce=", rd32(UFS_BASE + UFS_HCE));
}

/* ---- main ---- */

void shim_main(uint64_t dtb)
{
	boot_mpidr = rsr("mpidr_el1") & 0xffffff;

	/* Find the live splash buffer before painting anything. */
	uint32_t len;
	uintptr_t v = fdt_prop(dtb, "/reserved-memory/splash_region", "reg", &len);
	if (v && len == 16)
		con_fb = (volatile uint32_t *)(uintptr_t)fdt_be64(v);

	stage = "start";
	con_clear(TEAL);
	con_skip();	/* rows 0-1 sit under the punch hole and rounded corners */
	con_skip();
	con_str("ASTEROID", "M1 probe, read-only");
	show_cpu();
	show_fdt(dtb);
	show_initrd(dtb);
	con_skip();

	stage = "psci";
	con_hex2("PSCI", "ver=", smc(PSCI_VERSION, 0, 0, 0), "smccc=", smc(SMCCC_VERSION, 0, 0, 0));
	con_hex2(" feat", "on=", smc(PSCI_FEATURES, PSCI_CPU_ON, 0, 0),
		 "rst2=", smc(PSCI_FEATURES, PSCI_SYSTEM_RESET2, 0, 0));
	test_smp();
	con_skip();
	test_gic();
	test_ufs();
	con_skip();

	stage = "done";
	con_str("DONE", "film this, then Power + Vol-");
	park();
}
