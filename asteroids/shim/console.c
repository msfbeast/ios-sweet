/*
 * Minimal text console on the ABL splash framebuffer: sweet's shim/console.c
 * with the base set at run time. With the MMU off every access is Device
 * memory, so stay aligned (-mstrict-align).
 */
#include "console.h"
#include "font.h"

#define SCALE		3
#define CH_W		(8 * SCALE)
#define CH_H		(16 * SCALE)
#define MARGIN		24
#define LABEL_COLS	8

volatile uint32_t *con_fb = (volatile uint32_t *)FB_DEFAULT;
static uint32_t con_bg;
static int row;

void con_clear(uint32_t bg)
{
	for (int y = 0; y < FB_H; y++) {
		volatile uint32_t *line = con_fb + y * (FB_STRIDE / 4);
		for (int x = 0; x < FB_W; x++)
			line[x] = bg;
	}
	con_bg = bg;
	row = 0;
}

static void put_char(int col, char ch, uint32_t fg)
{
	if (ch < 0x20 || ch > 0x7e)
		ch = '?';
	const unsigned char *g = font8x16[ch - 0x20];
	int x0 = MARGIN + col * CH_W, y0 = MARGIN + row * CH_H;
	if (x0 + CH_W > FB_W || y0 + CH_H > FB_H)
		return;
	for (int gy = 0; gy < 16; gy++)
		for (int gx = 0; gx < 8; gx++) {
			uint32_t c = (g[gy] & (0x80 >> gx)) ? fg : con_bg;
			for (int sy = 0; sy < SCALE; sy++) {
				volatile uint32_t *p = con_fb + (y0 + gy * SCALE + sy) * (FB_STRIDE / 4)
						       + x0 + gx * SCALE;
				for (int sx = 0; sx < SCALE; sx++)
					p[sx] = c;
			}
		}
}

/*
 * Every console line is also kept as text in the ramoops console zone
 * (0x81f20000, Linux pstore layout: u32 sig "DBGC", u32 start, u32 size,
 * then the text). After a warm reset ABL prints that zone with
 * `fastboot oem dmesg`. Byte stores only: the MMU is off.
 */
#ifndef LOG_ZONE	/* host_test.c points it at a buffer */
#define LOG_ZONE	0x81f20000UL
#endif
#define LOG_SIG		0x43474244U	/* "DBGC", PERSISTENT_RAM_SIG */
#define LOG_CAP		(0x200000U - 12)

static int log_on;
static uint32_t log_len;
static int log_col;

static void log_putc(char c)
{
	if (!log_on || log_len >= LOG_CAP)
		return;
	*(volatile char *)(LOG_ZONE + 12 + log_len++) = c;
	log_col = c == '\n' ? 0 : log_col + 1;
}

/* Publish the length so far; done once per line, not per byte. */
static void log_newline(void)
{
	log_putc('\n');
	if (!log_on)
		return;
	*(volatile uint32_t *)(LOG_ZONE + 4) = log_len;	/* start: next write offset */
	*(volatile uint32_t *)(LOG_ZONE + 8) = log_len;	/* size: bytes valid */
	__asm__ volatile("dsb sy" ::: "memory");
}

/*
 * ABL's `oem dmesg` ignores the size field and prints the whole zone, and
 * it lost about the first 50 bytes in run 2. So clear the first 64 KiB
 * (old Android text would follow ours) and start with a sacrificial line.
 */
#define LOG_CLEAR	0x10000U

void con_log_init(void)
{
	for (uint32_t i = 0; i < LOG_CLEAR; i += 4)	/* data starts at +12: 4-aligned only */
		*(volatile uint32_t *)(LOG_ZONE + 12 + i) = 0;
	*(volatile uint32_t *)(LOG_ZONE + 0) = LOG_SIG;
	*(volatile uint32_t *)(LOG_ZONE + 4) = 0;
	*(volatile uint32_t *)(LOG_ZONE + 8) = 0;
	log_len = 0;
	log_col = 0;
	log_on = 1;
	for (int i = 0; i < 4; i++) {
		static const char pad[] = "........ oem dmesg drops the start of the zone ........";
		for (const char *p = pad; *p; p++)
			log_putc(*p);
		log_newline();
	}
}

static int put_str(int col, const char *s, uint32_t fg)
{
	while (log_col < col)
		log_putc(' ');
	while (*s) {
		log_putc(*s);
		put_char(col++, *s++, fg);
	}
	return col;
}

static void fmt_hex(char *buf, uint64_t v, int digits)
{
	static const char hex[] = "0123456789ABCDEF";
	for (int i = 0; i < digits; i++)
		buf[i] = hex[(v >> (4 * (digits - 1 - i))) & 0xf];
	buf[digits] = 0;
}

void con_skip(void)
{
	log_newline();
	row++;
}

void con_str(const char *label, const char *s)
{
	put_str(0, label, YELLOW);
	put_str(LABEL_COLS, s, WHITE);
	log_newline();
	row++;
}

void con_hex(const char *label, uint64_t v)
{
	char buf[17];
	fmt_hex(buf, v, 16);
	con_str(label, buf);
}

void con_hex2(const char *label, const char *a, uint32_t va, const char *b, uint32_t vb)
{
	char buf[9];
	int col = put_str(0, label, YELLOW);
	col = col < LABEL_COLS ? LABEL_COLS : col + 1;
	col = put_str(col, a, YELLOW);
	fmt_hex(buf, va, 8);
	col = put_str(col, buf, WHITE);
	if (b) {
		col = put_str(col + 1, b, YELLOW);
		fmt_hex(buf, vb, 8);
		put_str(col, buf, WHITE);
	}
	log_newline();
	row++;
}
