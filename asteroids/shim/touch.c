/*
 * Phone (3a) touch bring-up in the shim, before porting to XNU. Stages, each
 * gated on the previous one and logged (screen + ramoops for oem dmesg):
 *   A  read-only: GCC QUP0 clocks, RPMh DRV2 RSC id/TCS state, cmd-db
 *      entries for ldob11/ldob22, TLMM pins, SE0 firmware/FIFO (if clocked)
 *   B  clocks on (GCC vote), SE0 pins + touch GPIOs as Android sets them,
 *      SE0 initialised for SPI mode 0, 8 bit, FIFO, polled
 *   C  reset low, RPMh ACTIVE votes ldob22 3.3 V and ldob11 1.8 V, reset
 *      high, 200 ms
 *   D  FocalTech chip id (0xA3/0x9F) and firmware version (0xA6): reads only
 *   E  60 s of 10 ms report polling: a dot under every finger, log lines
 * Facts and sources: asteroids/research-touch.md. The touch IC is never
 * written to (no 0xA5 sleep, no firmware upgrade).
 */
#include <stdint.h>
#include "console.h"
#include "probe.h"

#define GCC		0x00100000UL
#define GCC_VOTE	(GCC + 0x52008)		/* QUPv3 wrap0 core2x/core/m/s ahb/s0 = bits 18..22 */
#define GCC_VOTE_BITS	0x007c0000U
#define GCC_DFSR	(GCC + 0x18024)		/* s0 RCG DFS enable (bit 0) */
#define GCC_DFS_L0	(GCC + 0x1802c)		/* DFS level 0: src [10:8], div [4:0] */
static const uint32_t gcc_halt[5] = { 0x23018, 0x23008, 0x23000, 0x23004, 0x18004 };

#define QUPW		0x00ac0000UL
#define SE		0x00a80000UL
#define TLMM_GPIO(n)	(0x0f100000UL + 0x1000UL * (n))	/* + REG_BASE 0x100000 */

#define RSC		0x17a20000UL		/* apps RSC, DRV 2 */
#define TCS		0x17a20d00UL
#define TCS_STRIDE	0x2a0
#define TCS_ACTIVE_N	3			/* qcom,tcs-config: ACTIVE count 3 */
#define CMDDB		0x81c60000UL
#define CMDDB_SIZE	0x20000U
#define CMDDB_MAGIC	0x0c0330dbU
#define CMDDB_HEADER	144U
#define CMDDB_ENTRY	24U

/* SE registers */
#define SE_FORCE_DEF	0x20
#define SE_OUT_CTRL	0x24
#define SE_CGC_CTRL	0x28
#define SE_STATUS	0x40
#define SE_M_CLK_CFG	0x48
#define SE_IF_DIS	0x64
#define SE_FW_REV	0x68
#define SE_CLK_SEL	0x7c
#define SPI_CPHA	0x224
#define SPI_LOOPBACK	0x22c
#define SPI_CPOL	0x230
#define SPI_DEMUX_INV	0x24c
#define SPI_DEMUX_SEL	0x250
#define SE_BYTE_GRAN	0x254
#define SE_DMA_MODE	0x258
#define SPI_TRANS_CFG	0x25c
#define SE_TX_PACK0	0x260
#define SE_TX_PACK1	0x264
#define SPI_WORD_LEN	0x268
#define SPI_TX_LEN	0x26c
#define SPI_RX_LEN	0x270
#define SPI_PRE_POST	0x274
#define SPI_DELAY	0x278
#define SE_RX_PACK0	0x284
#define SE_RX_PACK1	0x288
#define M_CMD0		0x600
#define M_IRQ_STATUS	0x610
#define M_IRQ_EN	0x614
#define M_IRQ_CLEAR	0x618
#define S_IRQ_EN	0x644
#define S_IRQ_CLEAR	0x648
#define TX_FIFO		0x700
#define RX_FIFO		0x780
#define TX_FIFO_STATUS	0x800
#define RX_FIFO_STATUS	0x804
#define TX_WM		0x80c
#define RX_WM		0x810
#define RX_RFR_WM	0x814
#define DMA_TX_IRQ_CLR	0xc44
#define DMA_RX_IRQ_CLR	0xd44
#define SE_GSI_EVT_EN	0xe18
#define SE_IRQ_EN	0xe1c
#define SE_HW_PARAM0	0xe24
#define SE_HW_PARAM1	0xe28
#define SE_DMA_GEN_CFG	0xe30
#define M_CMD_DONE	(1U << 0)
#define M_SPI_ERRORS	0x3300000eU

static uint32_t tcs_wait, tcs_control, tcs_status, tcs_enable, tcs_cmd_stride;
static uint32_t tcs_msgid, tcs_addr, tcs_data, tcs_cmd_status, rsc_ncpt;
static uint32_t fifo_depth;

/* ---- guarded MMIO with logging of the first abort ---- */

static int aborts;

static uint32_t rd(uintptr_t a)
{
	uint32_t v;
	uint64_t esr = mmio_rd32(a, &v);
	if (esr && aborts++ < 6)
		con_hex2("ABORT rd", "at=", (uint32_t)a, "esr=", (uint32_t)esr);
	return esr ? 0xdeadbeefU : v;
}

static int wr(uintptr_t a, uint32_t v)
{
	uint64_t esr = mmio_wr32(a, v);
	if (esr && aborts++ < 6)
		con_hex2("ABORT wr", "at=", (uint32_t)a, "esr=", (uint32_t)esr);
	return esr ? -1 : 0;
}

/* ---- stage A: read-only state ---- */

static int clocks_on(void)
{
	for (int i = 0; i < 5; i++)
		if (rd(GCC + gcc_halt[i]) & (1U << 31))	/* CLK_OFF */
			return 0;
	return 1;
}

static void show_clocks(const char *label)
{
	uint32_t off = 0;
	for (int i = 0; i < 5; i++)
		off |= (rd(GCC + gcc_halt[i]) >> 31) << i;
	con_hex2(label, "vote=", rd(GCC_VOTE), "offmask=", off);
	con_hex2(" dfs", "en=", rd(GCC_DFSR), "l0=", rd(GCC_DFS_L0));
}

static int rsc_setup(void)
{
	uint32_t id = rd(RSC), config = rd(RSC + 0x0c);
	uint32_t major = (id >> 16) & 0xff, count = (config >> 12) & 0x3f;

	rsc_ncpt = (config >> 27) & 0x1f;
	con_hex2("RSC", "id=", id, "config=", config);
	con_hex2(" drv2", "tcs=", count, "ncpt=", rsc_ncpt);
	if (major >= 3) {	/* Linux rpmh-rsc.c v3.0 offsets */
		tcs_wait = 0x20; tcs_control = 0x24; tcs_status = 0x28; tcs_enable = 0x2c;
		tcs_cmd_stride = 0x18; tcs_msgid = 0x34; tcs_addr = 0x38; tcs_data = 0x3c; tcs_cmd_status = 0x40;
	} else {		/* v2.x, as sweet */
		tcs_wait = 0x10; tcs_control = 0x14; tcs_status = 0x18; tcs_enable = 0x1c;
		tcs_cmd_stride = 0x14; tcs_msgid = 0x30; tcs_addr = 0x34; tcs_data = 0x38; tcs_cmd_status = 0x3c;
	}
	if (id == 0xdeadbeefU || major < 1 || major > 4 || count < TCS_ACTIVE_N ||
	    count > 0x3000 / TCS_STRIDE || rsc_ncpt < 3 || rsc_ncpt > 16) {
		con_str("RSC", "unexpected geometry, stop");
		return -1;
	}
	for (int t = 0; t < TCS_ACTIVE_N; t++) {
		uintptr_t b = TCS + t * TCS_STRIDE;
		char l[] = " tcs0";
		l[4] = '0' + t;
		con_hex2(l, "idle=", rd(b + tcs_status), "en=", rd(b + tcs_enable));
	}
	return 0;
}

static uint32_t db16(uint32_t off)
{
	return (rd(CMDDB + (off & ~3U)) >> (8 * (off & 3))) & 0xffff;
}

/* VRM address of an RPMh resource (sweet_rpmh.c rh_lookup), 0 if not found. */
static uint32_t cmddb_lookup(const char *res)
{
	uint32_t key[2] = { 0, 0 }, found = 0, matches = 0;

	for (int i = 0; i < 8 && res[i]; i++)
		key[i / 4] |= (uint32_t)(uint8_t)res[i] << (8 * (i % 4));
	for (int i = 0; i < 8; i++) {
		uint32_t h = 8 + 16 * i, slave = db16(h);
		if (!slave)
			break;
		uint32_t entries = CMDDB_HEADER + db16(h + 2), count = db16(h + 6);
		if ((entries & 3) || entries + count * CMDDB_ENTRY > CMDDB_SIZE)
			return 0;
		for (uint32_t j = 0; j < count; j++) {
			uint32_t e = entries + CMDDB_ENTRY * j;
			if (rd(CMDDB + e) != key[0] || rd(CMDDB + e + 4) != key[1])
				continue;
			found = rd(CMDDB + e + 16);
			matches++;
		}
	}
	if (matches != 1 || (found >> 16) != 4 || (found & 15))	/* VRM slave 4 */
		return 0;
	return found;
}

static void show_pins(void)
{
	static const uint8_t pins[6] = { 0, 1, 2, 3, 19, 20 };
	for (int i = 0; i < 6; i += 2) {
		char l[] = "GPIO a,b";
		l[4] = pins[i] < 10 ? '0' + pins[i] : 'x';
		con_hex2(l, "cfg=", rd(TLMM_GPIO(pins[i])) | rd(TLMM_GPIO(pins[i]) + 4) << 16,
			 "cfg=", rd(TLMM_GPIO(pins[i + 1])) | rd(TLMM_GPIO(pins[i + 1]) + 4) << 16);
	}
}

/* ---- stage C: RPMh ACTIVE vote (sweet_rpmh.c rh_send, v2/v3 layouts) ---- */

static int sync(uintptr_t a, uint32_t v)
{
	if (wr(a, v))
		return -1;
	for (int i = 0; i < 1000; i++) {
		if (rd(a) == v)
			return 0;
		delay_us(1);
	}
	con_hex2("RPMH sync", "at=", (uint32_t)a, "want=", v);
	return -1;
}

static int tcs_free(int t)
{
	uintptr_t b = TCS + t * TCS_STRIDE;
	uint32_t idle = rd(b + tcs_status);
	return idle != 0 && idle != 0xdeadbeefU && rd(b + tcs_control) == 0 &&
	       rd(b + tcs_enable) == 0 && rd(b + tcs_wait) == 0 &&
	       !(rd(TCS + 0x04) & (1U << t));
}

static int rpmh_vote(const char *res, uint32_t mv)
{
	uint32_t addr = cmddb_lookup(res);
	int t;

	con_hex2(res, "addr=", addr, "mv=", mv);
	if (!addr)
		return -1;
	for (t = 0; t < TCS_ACTIVE_N && !tcs_free(t); t++)
		;
	if (t == TCS_ACTIVE_N) {
		con_str("RPMH", "no free ACTIVE TCS");
		return -1;
	}
	uintptr_t b = TCS + t * TCS_STRIDE;
	uint32_t bit = 1U << t, irq_en = rd(TCS);
	const uint32_t offs[3] = { 0, 8, 4 }, vals[3] = { mv, 7, 1 };	/* voltage, HPM, enable last */

	if (sync(TCS, irq_en & ~bit))
		return -1;
	for (int c = 0; c < 3; c++) {
		uintptr_t cmd = b + c * tcs_cmd_stride;
		if (sync(cmd + tcs_msgid, 8 | (1U << 8) | (1U << 16)) ||	/* len 8, resp, write */
		    sync(cmd + tcs_addr, addr + offs[c]) || sync(cmd + tcs_data, vals[c]))
			return -1;
	}
	if (sync(b + tcs_wait, 7) || sync(b + tcs_enable, 7) ||
	    sync(b + tcs_control, 0) || sync(b + tcs_control, 1U << 16))
		return -1;
	wr(b + tcs_control, (1U << 16) | (1U << 24));	/* AMC + trigger */

	uint32_t st[3] = { 0, 0, 0 }, idle = 0;
	int done = 0, progress = 0;
	for (int us = 0; us < 100000 && !done; us++) {
		int complete = 1;
		idle = rd(b + tcs_status);
		for (int c = 0; c < 3; c++) {
			st[c] = rd(b + tcs_cmd_status + c * tcs_cmd_stride);
			complete &= (st[c] & ((1U << 8) | (1U << 16))) == ((1U << 8) | (1U << 16));
		}
		/* As sweet: a fresh IRQ or an observed busy phase rules out stale status. */
		progress |= idle == 0 || !complete;
		done = idle && idle != 0xdeadbeefU && complete && ((rd(TCS + 0x04) & bit) || progress);
		if (!done)
			delay_us(1);
	}
	con_hex2(done ? " vote ok" : " vote TIMEOUT", "tcs=", t, "st0=", st[0]);
	if (!done)
		return -1;
	sync(b + tcs_control, 1U << 16);
	sync(b + tcs_control, 0);
	sync(b + tcs_enable, 0);
	sync(b + tcs_wait, 0);
	wr(TCS + 0x08, bit);		/* W1C our completion only */
	sync(TCS, (rd(TCS) & ~bit) | (irq_en & bit));
	return 0;
}

/* ---- stage B: SE0 as SPI ---- */

static int se_init(void)
{
	uint32_t hw = rd(QUPW + 4), fw = rd(SE + SE_FW_REV), dis = rd(SE + SE_IF_DIS);
	uint32_t p0 = rd(SE + SE_HW_PARAM0), p1 = rd(SE + SE_HW_PARAM1);
	uint32_t major = hw >> 28, minor = (hw >> 16) & 0xfff;
	uint32_t mask = (major > 3 || (major == 3 && minor >= 10)) ? 0xff : 0x3f;
	uint32_t txd = (p0 >> 16) & mask, rxd = (p1 >> 16) & mask;

	con_hex2("QUP", "hwver=", hw, "status=", rd(SE + SE_STATUS));
	con_hex2("SE0", "fw=", fw, "ifdis=", dis);
	con_hex2(" fifo", "tx=", txd, "rx=", rxd);
	if (((fw >> 8) & 0xff) != 1 || (dis & 1) || ((p0 >> 24) & 0x3f) != 32 || !txd || !rxd) {
		con_str("SE0", "not an SPI FIFO SE, stop");
		return -1;
	}
	fifo_depth = txd < rxd ? txd : rxd;

	wr(SE + SE_GSI_EVT_EN, 0);
	wr(SE + SE_IRQ_EN, 0);
	wr(SE + M_IRQ_CLEAR, 0xffffffff);
	wr(SE + S_IRQ_CLEAR, 0xffffffff);
	wr(SE + S_IRQ_EN, 0);
	wr(SE + DMA_TX_IRQ_CLR, 0xffffffff);
	wr(SE + DMA_RX_IRQ_CLR, 0xffffffff);
	wr(SE + SE_CGC_CTRL, rd(SE + SE_CGC_CTRL) | 0x7f);
	wr(SE + SE_DMA_GEN_CFG, rd(SE + SE_DMA_GEN_CFG) | 0xf);
	wr(SE + SE_OUT_CTRL, 0x7f);
	wr(SE + SE_FORCE_DEF, 1);
	wr(SE + SE_DMA_MODE, 0);
	wr(SE + RX_WM, rxd - 3);
	wr(SE + RX_RFR_WM, rxd - 2);
	wr(SE + TX_WM, 0);
	wr(SE + M_IRQ_EN, 0x7f00007f);
	wr(SE + SE_BYTE_GRAN, 0);
	wr(SE + SE_TX_PACK0, 0x0007f8fe);
	wr(SE + SE_TX_PACK1, 0x000ffefe);
	wr(SE + SE_RX_PACK0, 0x0007f8fe);
	wr(SE + SE_RX_PACK1, 0x000ffefe);
	wr(SE + SPI_WORD_LEN, 8 - 4);
	wr(SE + SPI_LOOPBACK, 0);
	wr(SE + SPI_CPHA, 0);
	wr(SE + SPI_CPOL, 0);
	wr(SE + SPI_DEMUX_INV, 0);
	wr(SE + SPI_DEMUX_SEL, 0);
	wr(SE + SPI_PRE_POST, 0);
	wr(SE + SPI_DELAY, 0);
	uint32_t tc = rd(SE + SPI_TRANS_CFG);
	wr(SE + SPI_TRANS_CFG, tc & ~1U);	/* downstream CS_TOGGLE = bit 0 */
	wr(SE + SE_CLK_SEL, 0);
	wr(SE + SE_M_CLK_CFG, (4 << 4) | 1);	/* 19.2 MHz / 4 = 4.8 MHz */
	con_hex2(" init", "transcfg=", tc, "pack=", rd(SE + SE_TX_PACK0));
	con_hex2(" fifos", "txst=", rd(SE + TX_FIFO_STATUS), "rxst=", rd(SE + RX_FIFO_STATUS));
	return 0;
}

/* One full-duplex frame; CS low for the whole command (no CS_TOGGLE). */
static int spi_xfer(const uint8_t *tx, uint8_t *rx, uint32_t n)
{
	uint32_t ti = 0, ri = 0;
	uint64_t t_end = now() + 192000;	/* 10 ms at 19.2 MHz */

	wr(SE + M_IRQ_CLEAR, 0xffffffff);
	wr(SE + SPI_TX_LEN, n);
	wr(SE + SPI_RX_LEN, n);
	wr(SE + M_CMD0, 3U << 27);	/* SPI_FULL_DUPLEX */
	while (ri < n) {
		while (ti < n && (rd(SE + TX_FIFO_STATUS) & 0x0fffffff) < fifo_depth) {
			uint32_t w = 0;
			for (int k = 0; k < 4 && ti + k < n; k++)
				w |= (uint32_t)tx[ti + k] << (8 * k);
			wr(SE + TX_FIFO, w);
			ti += 4;
		}
		uint32_t words = rd(SE + RX_FIFO_STATUS) & 0x1ffffff;
		for (uint32_t k = 0; k < words && ri < n; k++) {
			uint32_t w = rd(SE + RX_FIFO);
			for (int b = 0; b < 4 && ri < n; b++)
				rx[ri++] = (uint8_t)(w >> (8 * b));
		}
		uint32_t irq = rd(SE + M_IRQ_STATUS);
		if (irq & M_SPI_ERRORS) {
			con_hex2("SPI err", "irq=", irq, "ri=", ri);
			wr(SE + M_IRQ_CLEAR, 0xffffffff);
			return -1;
		}
		if (now() > t_end) {
			con_hex2("SPI tmo", "irq=", irq, "ri=", ri);
			wr(SE + 0x604, 1U << 2);	/* M_CMD_CTRL cancel */
			return -1;
		}
	}
	for (int i = 0; i < 1000 && !(rd(SE + M_IRQ_STATUS) & M_CMD_DONE); i++)
		delay_us(1);
	wr(SE + M_IRQ_CLEAR, 0xffffffff);
	delay_us(150);				/* CS-high gap, as the Linux driver */
	return 0;
}

/* ---- FocalTech SPI v2 ---- */

static uint16_t crc16(const uint8_t *p, uint32_t n)
{
	uint16_t crc = 0xffff;
	while (n--) {
		crc ^= *p++;
		for (int b = 0; b < 8; b++)
			crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
	}
	return crc;
}

static uint8_t ft_tx[128], ft_rx[128];
static int ft_crc_logged;

static int ft_read(uint8_t reg, uint8_t *out, uint32_t len)
{
	uint32_t n = 7 + len + 2;
	for (uint32_t i = 0; i < n; i++)
		ft_tx[i] = 0;
	ft_tx[0] = reg;
	ft_tx[1] = 0xa0;			/* read + CRC */
	ft_tx[2] = (uint8_t)(len >> 8);
	ft_tx[3] = (uint8_t)len;
	for (int tries = 0; tries < 5; tries++) {
		if (spi_xfer(ft_tx, ft_rx, n))
			return -1;
		uint16_t want = ft_rx[7 + len] | ft_rx[8 + len] << 8, got = crc16(&ft_rx[7], len);
		if (ft_crc_logged < 3) {
			ft_crc_logged++;
			con_hex2(" ft", "rx0-3=", ft_rx[0] | ft_rx[1] << 8 | ft_rx[2] << 16 | (uint32_t)ft_rx[3] << 24,
				 "crc=", (uint32_t)want << 16 | got);
		}
		if ((ft_rx[3] & 0xa0) == 0 && want == got) {
			for (uint32_t i = 0; i < len; i++)
				out[i] = ft_rx[7 + i];
			return 0;
		}
	}
	return -1;
}

/* ---- stage E: live touch ---- */

static void dot(int x, int y, uint32_t c)
{
	for (int dy = -12; dy < 12; dy++)
		for (int dx = -12; dx < 12; dx++) {
			int px = x + dx, py = y + dy;
			if (px >= 0 && px < FB_W && py >= 0 && py < FB_H)
				con_fb[py * (FB_STRIDE / 4) + px] = c;
		}
}

static void touch_live(void)
{
	static const uint32_t colors[10] = {
		0xffff4040, 0xff40ff40, 0xff4080ff, 0xffffff40, 0xffff40ff,
		0xff40ffff, 0xffffffff, 0xffff8000, 0xff8000ff, 0xff00ff80,
	};
	uint8_t d[84];
	int logged = 0, fails = 0, reports = 0;
	uint8_t last_n = 0xff;

	stage = "touch live";
	con_str("TOUCH", "live 60 s: draw on the screen");
	for (int tick = 0; tick < 6000; tick++) {
		delay_ms(10);
		if (ft_read(0x01, d, 84)) {
			if (fails++ == 20)
				con_str("TOUCH", "20 report reads failed");
			continue;
		}
		if (d[1] == 0xff && d[2] == 0xff && d[3] == 0xff && d[4] == 0xff)
			continue;			/* firmware (re)init frame */
		uint8_t type = d[1] >> 4, n = d[1] & 0x0f;
		if (type != 0 && type != 2)
			continue;
		reports++;
		for (int i = 0; i < 10 && (type == 2 ? i < n : 1); i++) {
			const uint8_t *r = type == 2 ? &d[4 + 8 * i] : &d[2 + 6 * i];
			uint32_t id = r[2] >> 4, flag = r[0] >> 6, x, y;
			if (id >= 10)
				break;
			if (type == 2) {
				x = (((r[0] & 0x0f) << 12) | (r[1] << 4) | (r[4] >> 4)) / 16;
				y = (((r[2] & 0x0f) << 12) | (r[3] << 4) | (r[4] & 0x0f)) / 16;
			} else {
				x = ((r[0] & 0x0f) << 8) | r[1];
				y = ((r[2] & 0x0f) << 8) | r[3];
			}
			if (flag != 1)			/* down or move */
				dot((int)x, (int)y, colors[id]);
			if (logged < 60 && (n != last_n || flag != 2)) {
				logged++;
				con_hex2("T", "id/flag=", id << 8 | flag, "x/y=", x << 16 | y);
			}
		}
		last_n = n;
	}
	con_hex2("TOUCH", "reports=", reports, "fails=", fails);
}

/* ---- the stages ---- */

void touch_probe(void)
{
	stage = "touch A";
	con_skip();
	show_clocks("GCC");
	if (rsc_setup())
		return;
	con_hex2("CMDDB", "ver=", rd(CMDDB), "magic=", rd(CMDDB + 4));
	if (rd(CMDDB + 4) != CMDDB_MAGIC) {
		con_str("CMDDB", "bad magic, stop");
		return;
	}
	con_hex2(" ldob11", "addr=", cmddb_lookup("ldob11"), "", 0);
	con_hex2(" ldob22", "addr=", cmddb_lookup("ldob22"), "", 0);
	show_pins();

	stage = "touch B";
	wr(GCC_VOTE, rd(GCC_VOTE) | GCC_VOTE_BITS);
	for (int i = 0; i < 100 && !clocks_on(); i++)
		delay_us(100);
	show_clocks("GCC on");
	if (!clocks_on() || !(rd(GCC_DFSR) & 1) || (rd(GCC_DFS_L0) & 0x371f) != 0x0001) {
		con_str("GCC", "QUP0 SE0 clock not XO/1 on, stop");
		return;
	}
	if (se_init())
		return;
	for (int p = 0; p < 4; p++)
		wr(TLMM_GPIO(p), 0x84);			/* func 1 (QUP), 6 mA, no pull */
	wr(TLMM_GPIO(19), 0xc3);			/* gpio, 8 mA, pull-up, input */
	wr(TLMM_GPIO(20) + 4, 0);			/* reset low ... */
	wr(TLMM_GPIO(20), 0x2c3);			/* ... then output, 8 mA, pull-up */
	show_pins();

	stage = "touch C";
	delay_ms(2);
	if (rpmh_vote("ldob22", 3300) || rpmh_vote("ldob11", 1800))
		return;
	delay_ms(10);
	wr(TLMM_GPIO(20) + 4, 2);			/* reset high */
	delay_ms(200);

	stage = "touch D";
	uint8_t idh = 0, idl = 0, fwv = 0;
	int ok = 0;
	for (int i = 0; i < 5 && !ok; i++) {
		ok = !ft_read(0xa3, &idh, 1) && !ft_read(0x9f, &idl, 1) && idh == 0x56;
		if (!ok)
			delay_ms(200);
	}
	ft_read(0xa6, &fwv, 1);
	con_hex2("FT id", "h/l=", idh << 8 | idl, "fw=", fwv);
	if (!ok || (idl != 0x72 && idl != 0xb3 && idl != 0x62)) {
		con_str("FT", "no FT3683G/FT3681 id, stop");
		return;
	}
	touch_live();
}
