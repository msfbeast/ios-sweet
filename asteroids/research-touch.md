# Phone (3a) touch: FocalTech over GENI SPI, research for a polled XNU driver

Date: 2026-10-20. Read-only research. Nothing in the tree was changed except this file.

## Sources used

- **Nothing kernel source**: `NothingOSS/android_kernel_msm-6.1_nothing_sm7635`, branch `sm7635/v/mr`, commit `d5f5f965fd0b478eaf4eb63fd984c5a4a92135a7`. Below, `NK:` means `https://github.com/NothingOSS/android_kernel_msm-6.1_nothing_sm7635/blob/d5f5f965fd0b478eaf4eb63fd984c5a4a92135a7/`.
  - The touch driver is in the Qualcomm modules tree under `vendor/qcom/opensource/touch-drivers/focaltech/`, shortened below to `FT/`. There are no focaltech files in `drivers/input/touchscreen/`.
  - `FT/` is a different directory from `focaltech_touch/`, which is an older I2C copy that is not built.
- **Stock DT**: `firmware/Asteroids_V3.1-250302-1856/dt/asteroids.dts`, shortened below to `DTS:<line>`.
- **Stock kernel logs**: `asteroids/backup/oem-dmesg-clean.txt` and the matching `oem-dmesg-probe*` logs.
- **sweet driver**: `research/bt-fdad0de/xnu/bsd/dev/sweet_touch.c` and `sweet_rpmh.c`.
- **Panel touch firmware blobs**: `focaltech_ts_fw_vxn.bin` and `focaltech_ts_fw_boe.bin`, both 139264 bytes, from `AKoskovich/proprietary_vendor_nothing_asteroids` at `proprietary/vendor/firmware/`. A recovery tree (`Ubuntuify/device_nothing_asteroids-recovery`) ships the same two files.

**Proof the source matches the phone:** every stock dmesg capture shows `[FTS_TS]fts_ts_spi_init: Enter` followed by `Exit(612)`. Line 612 of `FT/focaltech_spi_v2.c` is the `FTS_FUNC_EXIT()` in `fts_ts_spi_init`. Driver version string: `FocalTech V4.2 20240407` (`FT/focaltech_common.h:39`).

There is one build-file contradiction:
- `touch_driver_board.mk:43-45` lists `goodix_ts.ko` and `focaltech_fts.ko` for volcano.
- `target.bzl:98-111` (DDK, volcano) lists only `goodix_ts`.

The dmesg evidence above shows the focaltech SPI v2 module does run on this phone.

---

## 1. Which IC, and the SPI protocol

### 1.1 IC identity

The driver is built for two discrete FocalTech AMOLED touch controllers and picks between them at runtime from the chip ID. It checks only `id_l`:

| IC | `FTS_CHIP_TYPE` | Reg 0xA3 (id_h) | Reg 0x9F (id_l) | Boot / ROM id | Source |
|---|---|---|---|---|---|
| **FT3683G** (FocalTech internal name "ft5672") | `0x56720090` (type 0x90) | 0x56 | **0x72** | 0x36 0xB3 | `FT/focaltech_config.h:204`, `FT/focaltech_common.h:63`, `FT/focaltech_core.c:340-364` |
| **FT3681** ("ft5662") | `0x3681008A` (type 0x8A) | 0x56 | **0x62** | pramboot 0x56 0x62, BL 0x56 0xE2 | `FT/focaltech_config.h:187`, `FT/focaltech_common.h:64`, `FT/focaltech_core.c:365-390` |

- The compile-time default is FT3683G: `long int FTS_CHIP_TYPE = _FT3683G;` (`FT/focaltech_core.c:2346`). Only the FT3683G and FT3681 flash back-ends are linked (`touch_modules.bzl:52-53`, `Kbuild:111-112`).
- Which of the two this unit has is **not confirmed**. The stock dmesg captures stop before the probe log line. Accept either at runtime: id_h must be 0x56, and id_l must be 0x72, 0xB3 or 0x62.
- Panel module names (`FT/focaltech_config.h:337-338`):
  - `FTS_MODULE_NAME "vxn"` (module id 0x0044, Visionox);
  - `FTS_MODULE2_NAME "boe"` (0x00DA, BOE).
  - These match the two `focaltech_ts_fw_*.bin` blobs.
- **It is not TDDI.** The `panel = <...>` list on `focaltech@0` (`DTS:11816`) resolves to the `rm69220` AMOLED panels (BOE `DTS:19779`, Visionox `DTS:19889`) and the `vtdr6130` AMOLED nodes. The `ft8726` LCD/TDDI panels (`DTS:21940` and following) are not in it. The panel DDIC is a Raydium RM69220, and the touch IC is a separate FocalTech part on its own SPI bus.

### 1.2 SPI electrical settings

| Item | Value | Source |
|---|---|---|
| Mode | `SPI_MODE_0` (CPOL=0, CPHA=0) | `FT/focaltech_spi_v2.c:476` |
| Bits per word | 8, MSB first | `FT/focaltech_spi_v2.c:477` |
| Max speed | 12 MHz (`spi-max-frequency = <0xb71b00>`) | `DTS:11826` |
| Controller max | 50 MHz (`0x2faf080`) | `DTS:11810` |
| Chip select | CS0 (`reg = <0x00>`), active low, no `spi-cs-high` | `DTS:11827` |
| Transfer type | Always one full-duplex `spi_transfer` (both `tx_buf` and `rx_buf` set), so CS stays low for the whole frame | `FT/focaltech_spi_v2.c:71-92` |
| CS high gap | `udelay(150)` (`CS_HIGH_DELAY`) after **every** frame, and between retries | `FT/focaltech_spi_v2.c:43, 213, 234, 344` |
| Retries | up to 20 (`SPI_RETRY_NUMBER`) | `FT/focaltech_spi_v2.c:42` |
| Per-device CS/clock delays | none: no `controller_data`, so `SE_SPI_DELAY_COUNTERS` = 0 | DT node has no delay properties |

### 1.3 Frame format ("SPI v2", `bus_ver = BUS_VER_V2`, `dummy_byte = 3`)

Constants (`FT/focaltech_spi_v2.c:45-50`): `WRITE_CMD 0x00`, `DATA_CRC_EN 0x20`, `READ_CMD 0xA0` (0x80 read bit plus 0x20 CRC enable), `SPI_DUMMY_BYTE 3`.

**Write** of `writebuf[0]=addr`, `writebuf[1..n-1]=data` (`fts_write`, `FT/focaltech_spi_v2.c:157-237`):
```
MOSI: addr, 0x00, (n-1)>>8, (n-1)&0xFF, [0x00 0x00 0x00, data[0..n-2]]   (the dummy and data part only if n-1 > 0)
```
- Single-register write (`fts_write_reg(reg, val)`): `reg, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, val`, which is 8 bytes.
- No CRC is sent on writes.

**Read** of `len` bytes from register `addr` (`fts_read`, `FT/focaltech_spi_v2.c:248-347`):
```
MOSI: addr, 0xA0, len>>8, len&0xFF, 0x00 x3 (dummy), 0x00 x len, 0x00 x2      total = 7 + len + 2
MISO: data starts at byte 7; CRC16 is the last 2 bytes, little endian (low byte first)
```
- Only `cmd[0]` is used, even when `cmdlen > 1` (`FT/focaltech_spi_v2.c:287`).

**Status check, for both read and write:** the byte on MISO during the 4th header byte, `rx[3]`, must satisfy `(rx[3] & 0xA0) == 0`. Otherwise the frame is retried (`FT/focaltech_spi_v2.c:207, 299`).

**CRC** (`crckermit`, `FT/focaltech_spi_v2.c:123-140`; check in `rdata_check`, `:142-155`):
- Reflected poly 0x8408, init 0xFFFF, no final XOR (this is CRC-16/MCRF4XX; the driver calls it "kermit" but the init value differs).
- Computed over the `len` data bytes only. Compare it with `rx[7+len] | rx[8+len] << 8`.
- A CRC mismatch is retried.

### 1.4 Chip ID

- **Normal path** (`fts_get_ic_information`, `FT/focaltech_core.c:442-505`):
  - read reg **0xA3** for id_h and reg **0x9F** for id_l (`FTS_REG_CHIP_ID` and `FTS_REG_CHIP_ID2`, `FT/focaltech_common.h:88-89`);
  - retry every 200 ms for up to 1 s (`INTERVAL_READ_REG`/`TIMEOUT_READ_REG`, `FT/focaltech_common.h:51-52`).
- **Readiness poll** after reset (`fts_wait_tp_to_valid`, `FT/focaltech_core.c:117-143`): poll 0xA3 until it equals id_h, up to 10 tries with growing delays.
- **Boot-mode fallback, only when the firmware is invalid** (`fts_read_bootid`, `FT/focaltech_core.c:399-428`):
  - write `0x55 0xAA` (`FTS_CMD_START1`/`2`), wait 12 ms, then read 2 bytes with command 0x90.
  - A polled driver should not need this. If 0xA3 and 0x9F read 0x00 or 0xFF, the panel firmware is broken; the driver should not try to fix that.
- Other useful registers (`FT/focaltech_common.h:84-118`):
  - firmware version `0xA6`, vendor id `0xA8`, panel id `0xE3`;
  - power mode `0xA5` (writing 0x03 puts the IC to sleep, so never write it);
  - gesture enable `0xD0`, FOD enable `0xCF`, work mode `0x00` (0x00 = work, 0x40 = factory).

### 1.5 Touch report

Read **84 bytes from register 0x01** on SPI (`fts_read_touchdata_spi`, `FT/focaltech_core.c:1180-1194`).
- The size is `FTS_TOUCH_DATA_LEN_V2 = 10*8 + 4` (`FT/focaltech_core.h:85`, set in `fts_buffer_init` at `FT/focaltech_core.c:1553-1554`).
- The whole frame is 7 + 84 + 2 = **93 bytes** on the wire.
- In the 84-byte buffer `d[]`, `d[0]` is register 0x01 (unused), and `d[1]` is `(event_type << 4) | count`.

Frame handling (`fts_read_parse_touchdata`, `FT/focaltech_core.c:1241-1304`):
- `d[1..4] == FF FF FF FF` means the firmware just (re)initialized (`TOUCH_FW_INIT`). Release all fingers and re-run state recovery (`:1270-1274`, `:1325-1328`).
- The event type `d[1] >> 4` is one of (`FT/focaltech_core.h:327-334`):
  - **0x0 `TOUCH_DEFAULT`**: classic 6-byte records (`fts_input_report_touch`, `FT/focaltech_core.c:993-1067`):
    - `count = d[1] & 0x0F` is the number of fingers currently down (at most 10).
    - Records are at `d[2 + 6*i]` for i = 0..9. Stop when `id = rec[2] >> 4` is 0x0A or more (0x0F means an unused slot).
    - `flag = rec[0] >> 6`: 0 = down, 1 = up, 2 = contact (move) (`FT/focaltech_core.h:113-117`).
    - `x = ((rec[0] & 0x0F) << 8) | rec[1]`, `y = ((rec[2] & 0x0F) << 8) | rec[3]`, `rec[4]` = pressure, `rec[5]` = area. These are display pixels.
  - **0x2 `TOUCH_PROTOCOL_v2`**: 8-byte records, likely what the SPI parts send (`fts_input_report_touch_pv2`, `FT/focaltech_core.c:1069-1129`):
    - `count = d[1] & 0x0F` is the number of **records present** (1..10, including UP records).
    - Records are at `d[4 + 8*i]` for i < count.
    - `flag = rec[0] >> 6`, `id = rec[2] >> 4`.
    - `x16 = ((rec[0] & 0x0F) << 12) | (rec[1] << 4) | (rec[4] >> 4)`, `y16 = ((rec[2] & 0x0F) << 12) | (rec[3] << 4) | (rec[4] & 0x0F)`.
    - Because `FTS_TOUCH_HIRES_EN = 0` (`FT/focaltech_core.h:148`), the driver uses **x = x16 / 16, y = y16 / 16**, in display pixels.
    - `rec[5]` = major, `rec[6]` = minor.
  - **0xB**: pen (`FTS_PEN_EN = 0`, ignored). **0xE**: firmware debug. Anything else is ignored.
- Reporting is MT protocol B (`FTS_MT_PROTOCOL_B_EN 1`, `FT/focaltech_config.h:238`; `fts_input_report_b`, `FT/focaltech_core.c:794-860`). Any slot that was down and is not reported down in this frame is released. That is the same model as sweet's `touch_report(mask, ...)`.
- Limits:
  - max points 10 (`FTS_MAX_POINTS_SUPPORT`, `FT/focaltech_core.h:79`; DT `focaltech,max-touch-number = <0x0a>`);
  - coordinates 0..1079 by 0..2391 (DT `focaltech,display-coords = <0 0 0x438 0x958>`, `DTS:11820`). sweet's y limit of 2399 must become 2391.

---

## 2. Power-on and reset sequence; firmware

The ordered calls, with `FTS_POWER_SOURCE_CUST_EN = 1` and `FTS_PINCTRL_EN = 0` (`FT/focaltech_config.h:274, 289`). See `fts_ts_probe_entry` at `FT/focaltech_core.c:2348-2459`.

1. **GPIO setup**: IRQ (TLMM 19) set as input; reset (TLMM 20) requested (`fts_gpio_configure`, `:1845-1883`).
2. **Power on** (`fts_power_init` calls `fts_power_source_init`, which calls `fts_power_source_ctrl(ENABLE)`, `:1616-1667`, `:1680-1722`, `:1744-1779`):
   1. Reset low (`fts_set_reset(0)`), then 2 ms.
   2. Enable **vci**, set to 3.3 V (`FTS_VTG_MIN/MAX_UV 3300000`, `:61-62`).
   3. Enable **vddio**, set to 1.8 V (`FTS_IOVCC_VTG_*_UV 1800000`, `:63-64`). There is no delay between vci and vddio.
   4. Wait 2 ms, then reset high.
   5. Then `fts_msleep(200)` (`:1777`) before the first bus access.
3. **Read the chip ID** (`:2402`, see 1.4).
4. Input, sysfs, gesture, ESD (`FTS_ESDCHECK_EN 0`, so off), and IRQ registration: `IRQF_TRIGGER_FALLING | IRQF_ONESHOT` (`:1398-1411`).
5. Firmware upgrade work and the panel notifier.
6. Read the firmware version register 0xA6 (`:2455-2457`).

**Reset helper**: `fts_reset_proc` drives reset low, waits 2 ms, then drives it high, plus an optional delay (`:198-210`).
- Before driving reset low on type 0x90/0x92/0x93 (FT3683G), the driver writes reg `0xB6 = 1` and waits 20 ms, unless power is already off (`fts_reset_pre`, `:161-174`).
- After a reset, `fts_tp_state_recovery` waits for 0xA3 and re-applies modes (`:145-159`).

**Suspend** (panel off): writes `0xA5 = 0x03` (sleep), then powers off: reset low, 2 ms, vci off, 200 us, vddio off (`:1781-1812`, `:1647-1664`). **Resume**: the power-on sequence above, then 50 ms (`FTS_DELAY_RESUME_RESET`, `FT/focaltech_common.h:54`).

**Firmware: no host download is required.** Both ICs are flash parts that run their own firmware out of internal flash:
- The upgrade code erases and programs flash. FT3683G uses `fts_ft5672_flash_write_buf` with `pramboot_supported = false` (`FT/focaltech_flash/focaltech_upgrade_ft3683g.c:373-382`). FT3681 loads a pramboot only as a flashing helper (`FT/focaltech_flash/focaltech_upgrade_ft3681.c:388, 432-445`).
- Auto-upgrade runs in a work item after probe. It only reflashes if the firmware is invalid or the version in 0xA6 differs from the host file (`fts_fwupg_need_upgrade`, `FT/focaltech_flash.c:1800-1829`).
- The host file comes from `request_firmware("focaltech_ts_fw_<vxn|boe>.bin")` (`FT/focaltech_flash.c:41-43, 2039-2060`). The built-in arrays are empty (`:48-61`).
- `FTS_MULTI_FW_NUM 0` with its comment "for none flash project" (`FT/focaltech_config.h:306`) confirms these are flash projects.
- The touch firmware is not loaded by the panel either, since this is not TDDI. **XNU can skip the upgrade path entirely.**

---

## 3. Regulators

| Supply | Phandle | Regulator node | Rail | Voltage | Type | Source |
|---|---|---|---|---|---|---|
| `vddio-supply` | `<0x27c>` | `pm7550_l11` / `L11B` | PM7550 LDO 11 | **1.8 V** (`0x1b7740`, min = max = init) | RPMh VRM, `qcom,resource-name = "ldob11"`, `pmic5-ldo`, modes `<2 4>`, LPM below 30 mA | `DTS:1682-1698`, alias `DTS:26633` |
| `vci-supply` | `<0x286>` | `pm7550_l22` / `L22B` | PM7550 LDO 22 | **3.3 V** (`0x325aa0`) | RPMh VRM, `qcom,resource-name = "ldob22"`, `pmic5-ldo`, LPM below 10 mA | `DTS:1880-1896`, alias `DTS:26655` |

- Both nodes live under `/soc/rsc@17a00000/drv@2/` (`apps_rsc`, DRV 2), with `qcom,set = <3>` (active and sleep sets).
- **Not shared with the display.**
  - The panel and DSI use `vddio-supply = <0x28>` (L9B 1.8 V) and `vci-supply = <0x29>` (L19B 3.0 V) (`DTS:931-932`, `DTS:1643-1661`, `DTS:1822-1841`). Those two have `qcom,proxy-consumer-enable`, meaning "the bootloader left this on; keep it on until a consumer claims it".
  - L11B and L22B have **no** proxy-consumer and no other consumer anywhere in the DT. Only `focaltech@0` references `0x27c` and `0x286`.
- **Assume both touch rails are off at XNU entry** and vote them on yourself.
- RPMh VRM command layout (`NK:drivers/regulator/rpmh-regulator.c:92-98`, address step of 4 at `:829, 868`; `qcom,init-mode = <4>` in the DT):

  | Offset from the cmd-db address | Field | Value to write |
  |---|---|---|
  | +0 | voltage | in mV |
  | +4 | enable | 1 |
  | +8 | mode | PMIC5 LDO: RM=3, LPM=4, **HPM=7** (`:296-298`) |

  sweet's order (voltage, mode, enable last) carries over unchanged.

### Porting `sweet_rpmh.c` to the 3a

It is written for SM7150 and needs these changes:
- **Addresses:**
  - DRV 2 base = **0x17a20000** (`reg-names "drv-2"`, `DTS:1313-1314`);
  - TCS base = DRV base + `qcom,tcs-offset` 0xd00 = **0x17a20d00** (`DTS:1321-1322`);
  - read the RSC id at 0x17a20000 + 0.
- **ACTIVE TCS count = 3.** `qcom,tcs-config = <2 3  0 2  1 2  3 0  4 1>` (`DTS:1325`) lists (type, count) pairs.
  - Linux assigns TCS indices in list order (`NK:drivers/soc/qcom/rpmh-rsc.c:1541-1561`).
  - With ACTIVE=2, SLEEP=0, WAKE=1, CONTROL=3, FAST_PATH=4, that gives ACTIVE TCS 0..2, SLEEP 3..4, WAKE 5..6, FAST_PATH 7. sweet's `RH_ACTIVE_N` is 2.
- **cmd-db**: the region is `aop_cmd_db_region@81c60000`, size 0x20000, `compatible = "qcom,cmd-db"` (`DTS:189-193`). Map **0x81c60000** directly; it holds the cmd-db header. sweet instead reads an AOP-RAM locator at 0x0c3f000c. Resource names are `"ldob11"` and `"ldob22"`.
- **Guard**: `sweet_rpmh_vreg_enable` only accepts `"ldoc11"` at 2950..3400 mV. It needs a small allow-list: `ldob11` at 1800 mV and `ldob22` at 3300 mV.
- **RSC register layout depends on the hardware revision.** Linux picks v3.0 offsets when the major version (bits 23:16 of the RSC id) is 3 or more (`NK:drivers/soc/qcom/rpmh-rsc.c:1720-1728`). sweet refuses anything other than major 1 or 2. Volcano's revision is not known offline, so log it and support both:

  | Register (per TCS, stride 0x2a0 in both) | v2.7 | v3.0 |
  |---|---|---|
  | IRQ_ENABLE / IRQ_STATUS / IRQ_CLEAR (TCS base) | 0x00 / 0x04 / 0x08 | 0x00 / 0x04 / 0x08 |
  | CMD_WAIT_FOR_CMPL | 0x10 | 0x20 |
  | CONTROL | 0x14 | 0x24 |
  | STATUS (idle) | 0x18 | 0x28 |
  | CMD_ENABLE | 0x1C | 0x2C |
  | per-CMD stride | 20 (0x14) | 24 (0x18) |
  | CMD MSGID / ADDR / DATA / STATUS | 0x30 / 0x34 / 0x38 / 0x3C | 0x34 / 0x38 / 0x3C / 0x40 |

  - Source: `NK:drivers/soc/qcom/rpmh-rsc.c:196-246`.
  - The active-mode trigger is the same on both: clear the trigger, clear AMC, set AMC (bit 16), then AMC plus trigger (bit 24) (`__tcs_set_trigger`, `:500-522`).
  - The v3 `CHN_*` registers (0x464 to 0x4A0) are only touched for sleep/wake channel switching (`:1293-1347`), not for an ACTIVE-only vote.

---

## 4. GENI SPI in polled FIFO mode

### 4.1 Where things are

| Block | Address | Source |
|---|---|---|
| SE0 (`qupv3_se0_spi`, `compatible "qcom,spi-geni"`, so the downstream `spi-msm-geni.c` binds) | 0xa80000, size 0x4000 | `DTS:11795-11813`; `NK:drivers/spi/spi-msm-geni.c:3207` |
| QUP wrapper 0 (`qcom,geni-se-qup`) | 0xac0000; `QUP_HW_VER` at **0xac0004** | `DTS:11760-11766`; `NK:drivers/soc/qcom/qcom-geni-se.c:102, 184-189` |
| GCC | 0x100000, size 0x1f4200 | `DTS:5465-5467` |
| TLMM | DT base 0xf000000, **GPIO n at 0xf100000 + 0x1000*n** (see below) | `DTS:2408-2418`; `NK:drivers/pinctrl/qcom/pinctrl-volcano.c:21-28` |
| SE0 pins | GPIO0 MISO (`qup0_se0_l0`), GPIO1 MOSI (`l1`), GPIO2 CLK (`l2`), GPIO3 CS0 (`l3`); function index **1**; 6 mA; bias-disable | `DTS:qupv3_se0_spi_pins`; `NK:.../pinctrl-volcano.c:1781-1788` |

**TLMM address warning.** `pinctrl-volcano.c` defines `REG_BASE 0x100000`, and `ctl_reg = REG_BASE + 0x1000*id`, `io_reg = ctl_reg + 4`, `intr_cfg = +8`, `intr_status = +0xc` (`:21-31`).
- `pinctrl-msm.c` accesses `pctrl->regs[g->tile] + g->ctl_reg` (`NK:drivers/pinctrl/qcom/pinctrl-msm.c:131-139`), where `regs[0]` is the DT base 0xf000000.
- So:
  - GPIO 19 (IRQ): cfg **0xf113000**, io **0xf113004**;
  - GPIO 20 (reset): cfg **0xf114000**, io **0xf114004**;
  - GPIO 0..3: cfg 0xf100000..0xf103000.
- The brief's "0xf000000, stride 0x1000" leaves out the 0x100000 offset.
- The same offset is missing in patch 0103: `KEYS_ESS_PA = 0x0f000000 + 71*0x1000` (`xnu-patches/0103-asteroids-keys.patch:40`). The Essential Key IN_OUT register should be **0xf147004**, not 0xf047004. Check that on hardware; the patch was not modified here.
- The cfg bit layout matches sweet: pull [1:0], func [5:2], drive [8:6] (mA/2 - 1), OE bit 9; io: in bit 0, out bit 1 (`pinctrl-volcano.c` `PINGROUP` fields).

**SE0 clocks.** `clocks = <0x32 0x50>` (`DTS:11802`) resolves as follows:
- `0x32` is `gcc`, and 0x50 = 80 = `GCC_QUPV3_WRAP0_S0_CLK` (`NK:include/dt-bindings/clock/qcom,gcc-volcano.h:90`).
- The wrapper clocks are `<0x32 0x70 0x32 0x71>` (m-ahb, s-ahb, `DTS:11766`).
- Register details, all from `NK:drivers/clk/qcom/gcc-volcano.c`:

| Clock | Vote register (GCC +) | Bit | Halt register | Source |
|---|---|---|---|---|
| `gcc_qupv3_wrap0_core_2x_clk` | 0x52008 | 18 | 0x23018 | `:2363-2374` |
| `gcc_qupv3_wrap0_core_clk` | 0x52008 | 19 | 0x23008 | `:2376-2387` |
| `gcc_qupv3_wrap_0_m_ahb_clk` | 0x52008 | 20 | 0x23000 | `:2703-2714` |
| `gcc_qupv3_wrap_0_s_ahb_clk` | 0x52008 | 21 | 0x23004 | `:2716-2728` |
| `gcc_qupv3_wrap0_s0_clk` | 0x52008 | 22 | 0x18004 | `:2407-2421` |
| RCG `gcc_qupv3_wrap0_s0_clk_src` | CMD_RCGR **0x18010**, CFG 0x18014 | in `gcc_dfs_clocks` (DFS) | | `:805-821`, `:3392-3394` |

- With DFS, the SE's `SE_GENI_CLK_SEL` picks the performance level:
  - DFSR = RCG + 0x14 = **0x18024** (bit 0 = DFS enabled);
  - level n config = RCG + 0x1c + 4n (**0x1802c** for level 0).
  - This is the same scheme sweet uses.
- The frequency table includes **19.2 MHz from XO (parent 0, div 1)** (`:778-797`).
- Is the SE0 clock left on by ABL? **Unknown.** ABL has no use for touch, so assume it is off and vote it, exactly as sweet does. The m/s AHB clocks **must** be on before touching any SE or wrapper register, or the access will fault.
- The QUP core also needs the RPMh BCM "QUP0" (interconnect `qup-core`, `clk_virt` MASTER 32 to SLAVE 568). Volcano marks it `keepalive = true` (`NK:drivers/interconnect/qcom/volcano.c:1743-1750`), so it should stay voted from boot, as it did on sweet where ICC was ignored. This is unverified on the 3a.

### 4.2 Register map (SE-relative)

Generic GENI registers (`NK:include/linux/soc/qcom/geni-se.h:78-112`, `NK:drivers/soc/qcom/qcom-geni-se.c:105-139`):

| Offset | Name | Use |
|---|---|---|
| 0x20 | GENI_FORCE_DEFAULT_REG | write 1 |
| 0x24 | GENI_OUTPUT_CTRL | 0x7f |
| 0x28 | GENI_CGC_CTRL | OR in 0x7f |
| 0x40 | SE_GENI_STATUS | bit 0 = M command active |
| 0x48 | GENI_SER_M_CLK_CFG | `(div << 4) \| 1` |
| 0x64 | GENI_IF_DISABLE_RO | bit 0 = FIFO interface disabled (GSI-only SE). **Must be 0.** |
| 0x68 | GENI_FW_REVISION_RO | protocol = bits [15:8]; **SPI = 1** (`GENI_SE_SPI`, `geni-se.h:33-41`; I2C = 3, UART = 2) |
| 0x7c | SE_GENI_CLK_SEL | 0 (DFS level 0) |
| 0x254 | SE_GENI_BYTE_GRAN | 0 |
| 0x258 | SE_GENI_DMA_MODE_EN | 0 (FIFO) |
| 0x260 / 0x264 | TX_PACKING_CFG0/1 | 0x0007f8fe / 0x000ffefe |
| 0x284 / 0x288 | RX_PACKING_CFG0/1 | 0x0007f8fe / 0x000ffefe |
| 0x600 | SE_GENI_M_CMD0 | `(opcode << 27) \| params` |
| 0x604 | SE_GENI_M_CMD_CTRL_REG | bit 1 abort, bit 2 cancel |
| 0x610 / 0x614 / 0x618 | M_IRQ_STATUS / EN / CLEAR | see bits below |
| 0x640 / 0x644 / 0x648 | S_IRQ_STATUS / EN / CLEAR | clear, leave unused |
| 0x700 / 0x780 | TX_FIFOn / RX_FIFOn | 32-bit words, first byte in bits [7:0] |
| 0x800 | TX_FIFO_STATUS | word count [27:0] |
| 0x804 | RX_FIFO_STATUS | word count [24:0], RX_LAST bit 31, last-word valid bytes [30:28] |
| 0x80c / 0x810 / 0x814 | TX_WM / RX_WM / RX_RFR_WM | |
| 0xe18 | SE_GSI_EVENT_EN | 0 |
| 0xe1c | SE_IRQ_EN | routing to the GIC; 0 when polling (as sweet does) |
| 0xe24 | SE_HW_PARAM_0 | TX FIFO width [29:24] (expect 32); TX depth [21:16], or **[23:16] if QUP_HW_VER major > 3, or major == 3 and minor >= 10** (`geni-se.h:414-430`) |
| 0xe28 | SE_HW_PARAM_1 | RX FIFO, same fields |
| 0xe30 | SE_DMA_GENERAL_CFG | OR in 0xf |

SPI-specific registers (`NK:drivers/spi/spi-msm-geni.c:30-89`; mainline `spi-geni-qcom.c:20-67`):

| Offset | Name | Value for FocalTech |
|---|---|---|
| 0x224 | SE_SPI_CPHA (bit 0) | 0 |
| 0x22c | SE_SPI_LOOPBACK | 0 |
| 0x230 | SE_SPI_CPOL (bit 2) | 0 |
| 0x24c | SE_SPI_DEMUX_OUTPUT_INV | 0 (CS active low) |
| 0x250 | SE_SPI_DEMUX_SEL | 0 (CS0) |
| 0x25c | SE_SPI_TRANS_CFG | clear CS_TOGGLE (see caveat) |
| 0x268 | SE_SPI_WORD_LEN | bits_per_word - 4 = **4** |
| 0x26c | SE_SPI_TX_TRANS_LEN | number of words = bytes (24-bit) |
| 0x270 | SE_SPI_RX_TRANS_LEN | number of words = bytes |
| 0x274 | SE_SPI_PRE_POST_CMD_DLY | 0 |
| 0x278 | SE_SPI_DELAY_COUNTERS | 0. On I2C this offset is SCL timing; **do not copy sweet's 100 kHz value.** |

SPI M_CMD opcodes: 1 = TX only, 2 = RX only, **3 = full duplex** (`SPI_FULL_DUPLEX`, used by the downstream driver whenever both buffers are set, `spi-msm-geni.c:81, 1846-1851`), 8 = CS assert, 9 = CS deassert.
- Param bit 2 = `FRAGMENTATION`: keep CS asserted after this command.
- With CS_TOGGLE cleared and no FRAGMENTATION, CS asserts at the start of the command and deasserts at the end (`spi-msm-geni.c:1854-1869`). So one command per FocalTech frame, with no explicit CS_ASSERT or CS_DEASSERT.

M_IRQ bits (`geni-se.h:161-185`):
- bit 0 CMD_DONE; bit 1 CMD_OVERRUN; bit 2 ILLEGAL_CMD; bit 3 CMD_FAILURE; bit 4 CANCEL; bit 5 ABORT;
- bit 24 RX_FIFO_RD_ERR; bit 25 RX_FIFO_WR_ERR; bit 26 RX_WATERMARK; bit 27 RX_LAST; bit 28 TX_FIFO_RD_ERR; bit 29 TX_FIFO_WR_ERR; bit 30 TX_WATERMARK.
- **SPI error mask = 0x3300000E.** Drop sweet's I2C GP-IRQ bits 9, 10, 12 and 13 (NACK and similar); they mean nothing for SPI.
- IRQ enable value, as Linux sets it (`M_COMMON_GENI_M_IRQ_EN` plus DONE and watermarks, `qcom-geni-se.c:244-293`): **0x7F00007F**.

**Oversampling**: if `QUP_HW_VER` major == 1 and minor == 0, SCLK = clk / (2 * div); otherwise SCLK = clk / div (`spi-msm-geni.c:1625-1636`; mainline `spi-geni-qcom.c:578-584`). Volcano is almost certainly not v1.0, but read it anyway.

### 4.3 Comparison with sweet_touch.c (I2C)

**Reusable unchanged:**
- `touch_rd`/`touch_wr` with barriers, `touch_wait`, `touch_wait_clear`;
- the GCC vote, RCG/DFS check and branch halt polling (`sweet_touch.c:137-172`), with new offsets;
- `touch_gpio` (`:121-134`), with a new base;
- the abort path (`:176-201`);
- the FIFO and IRQ register offsets;
- the packing constants, which match `geni_se_config_packing(8, 4, msb_first)` for both I2C and SPI (worked from `qcom-geni-se.c:439-490`: vectors 0xFE, 0x1FE, 0x2FE, 0x3FF give 0x7f8fe / 0xffefe, BYTE_GRAN 0);
- `touch_report` (`:368-384`).

**Must change:**
- Protocol check: `(fw >> 8) & 0xff` must be **1**, not 3 (`:498`).
- `touch_se_init`:
  - drop the SCL write (0x278 becomes 0) and the I2C-specific `0x254/0x260...` assumptions only where they differ (they do not);
  - add the SPI registers above;
  - set `0x7c = 0`, `0x48 = (div << 4) | 1`.
- `touch_msg_run` (`:213-289`) handles at most 3 TX bytes, and either TX or RX. SPI needs **full duplex, 8 to 93 bytes**:
  - Write TX_TRANS_LEN = RX_TRANS_LEN = n, then `M_CMD0 = 3 << 27`.
  - Then loop:
    - while TX remains and `TX_FIFO_STATUS & 0x0fffffff` is below the depth, push words;
    - drain `RX_FIFO_STATUS` words, using `RX_LAST` and the valid-bytes field for the final word;
    - stop when `CMD_DONE` is set, then drain once more.
  - The FIFO depth is probably below 93 bytes, so use one of these two approaches:
    - (a) keep the TX and RX loop tight, or
    - (b) **split the frame into chunks no larger than `min(tx_depth, rx_depth) * 4` bytes**, setting `FRAGMENTATION` (param bit 2) on every chunk except the last, so CS stays low across chunks. (b) can never overflow a FIFO and is the safer choice for a first try.
- The error mask (above) and the post-frame 150 us CS-high gap.

---

## 5. Other things that matter for a minimal polled driver

- **IRQ pin**:
  - The DT says `interrupts = <0x13 0x2008>` (level-low plus IRQF_ONESHOT), but the driver overrides it with **`IRQF_TRIGGER_FALLING`** (`FT/focaltech_core.c:1404`). The IC pulses INT low once per report frame, so reading GPIO 19 as a level can miss frames.
  - Either poll the report every 10 ms unconditionally, or (optional, unverified) program the TLMM interrupt logic without routing to the GIC: intr_cfg 0xf113008 with falling-edge detection, RAW_STATUS_EN bit 4 set and ENABLE bit 0 clear; then poll intr_status 0xf11300c bit 0 and clear it by writing 0.
  - pinctrl `ts_active` puts GPIO 19 and 20 in function gpio, 8 mA, pull-up (`DTS:4642-4655`).
- **No heartbeat is needed.** ESD check is compiled out (`FTS_ESDCHECK_EN 0`, `FT/focaltech_config.h:268`). The Linux driver sends nothing periodic in normal operation, and idle low-power scanning is handled inside the IC's firmware.
  - The only things that put the IC to sleep are the host writing `0xA5 = 0x03` on suspend, or the rails going off (`FTS_POWER_SUSPEND_OFF_EN 1`).
  - The panel notifier (`panel = <...>` list, `fts_panel_notifier_callback`) suspends touch when the panel blanks. XNU has no such notifier, so touch just stays awake.
- **Gesture and FOD**:
  - `FTS_FOD_EN 1` (`FT/focaltech_config.h:262`), but FOD mode is only enabled through the `fts_fod_enable` sysfs path (writes reg `0xCF = 0x02`).
  - Gesture mode (`0xD0` plus 0xD1..0xD8) is only used in suspend, and only when `gesture_support` is set through sysfs.
  - After a hardware reset both are at the firmware defaults. A minimal driver needs nothing here; writing `0xCF = 0` and `0xD0 = 0` once after the ID check is harmless if desired.
- **Firmware-init frames**: after any IC-internal reset, the report reads as `FF FF FF FF` at `d[1..4]`. Release all contacts and continue.
- **Reset before access**: on FT3683G the driver writes `0xB6 = 1` before pulling reset low (section 2). Not needed for a cold power-on, where the rails start off.
- **Nothing else shares the SE.** `spi@a80000` has only `focaltech@0` as a child. It has no `qcom,shared`, `qcom,le-vm` or trusted-touch properties. `qcom,gpios-reserved = <0x0d 0x3f 0x40>` (`DTS:2417`) does not include 0..3, 19 or 20.

---

## Minimal bring-up recipe

Physical addresses:
- GCC = 0x100000
- TLMM_GPIO(n) = 0xf100000 + 0x1000*n
- SE = 0xa80000
- QUPW = 0xac0000
- RSC2 = 0x17a20000
- TCS = 0x17a20d00
- CMDDB = 0x81c60000

1. **Start late.** Use a boot-arg delay, like `sweettouchdelay`, and map GCC (0x200000), the TLMM range for GPIOs 0..20, SE (0x4000) and QUPW (0x2000).
2. **Clocks.**
   1. `GCC[0x52008] |= 0x007C0000` (bits 18..22).
   2. Poll until bit 31 is clear in `GCC[0x23018]`, `[0x23008]`, `[0x23000]`, `[0x23004]` and `[0x18004]`.
   3. Read `GCC[0x18024]`:
      - If bit 0 is set (DFS): require `GCC[0x1802c] & 0x371f` == 0x0001 (XO, div 1); otherwise stop and log.
      - If bit 0 is clear: write `GCC[0x18014] = (old & ~(0x371f | 1<<20)) | 1`, then `GCC[0x18010] |= 1`, and poll until bit 0 clears.
3. **Check the SE.**
   1. `ver = QUPW[0x4]`.
   2. Require `(SE[0x68] >> 8) & 0xff == 1` (SPI firmware loaded) and `SE[0x64] & 1 == 0` (FIFO allowed).
   3. Require `(SE[0xe24] >> 24) & 0x3f == 32`.
   4. Depth = `(SE[0xe24] >> 16) & (major > 3 || (major == 3 && minor >= 10) ? 0xff : 0x3f)`.
   5. Require `SE[0x40] & 1 == 0`. Log all of these.
4. **Initialize the SE.** Write in this order:
   1. `0xe18 = 0`, `0xe1c = 0`
   2. `0x618 = 0xffffffff`, `0x648 = 0xffffffff`, `0x644 = 0`, `0xc44 = 0xffffffff`, `0xd44 = 0xffffffff`
   3. `0x28 |= 0x7f`, `0xe30 |= 0xf`, `0x24 = 0x7f`, `0x20 = 1`
   4. `0x258 = 0`
   5. `0x810 = depth - 3`, `0x814 = depth - 2`, `0x80c = 0`
   6. `0x614 = 0x7F00007F`
   7. `0x254 = 0`, `0x260 = 0x7f8fe`, `0x264 = 0xffefe`, `0x284 = 0x7f8fe`, `0x288 = 0xffefe`
   8. `0x268 = 4`, `0x22c = 0`, `0x224 = 0`, `0x230 = 0`, `0x24c = 0`, `0x250 = 0`, `0x274 = 0`, `0x278 = 0`
   9. `0x25c &= ~1` (downstream CS_TOGGLE; see the unverified list)
   10. `0x7c = 0`, `0x48 = (4 << 4) | 1`. This gives 4.8 MHz from 19.2 MHz. Move to div 2 (9.6 MHz) once it works.
   11. Read back the packing registers and check that both FIFO counts are 0.
5. **Pins.**
   - GPIO 0..3 cfg = **0x84** (func 1, 6 mA, no pull).
   - GPIO 19 cfg = **0xC3** (gpio, 8 mA, pull-up, input).
   - GPIO 20: io bit 1 = 0 first, then cfg = **0x2C3** (output low, 8 mA, pull-up).
6. **Power** (the driver's order).
   1. Reset is already low; wait 2 ms.
   2. RPMh ACTIVE vote `ldob22`: 3300 mV, mode 7, enable 1.
   3. RPMh ACTIVE vote `ldob11`: 1800 mV, mode 7, enable 1.
   4. Wait 2 ms (10 ms is fine), then GPIO 20 io bit 1 = 1 (reset high).
   5. Wait **200 ms**.
7. **Identify.**
   1. Read 0xA3 and 0x9F with the 1-byte read frame `[reg, 0xA0, 0x00, 0x01, 0, 0, 0, 0, 0, 0]` (10 bytes). The data is rx[7], the CRC is rx[8..9], and the status rx[3] & 0xA0 must be 0.
   2. Accept id_h 0x56 with id_l 0x72 or 0xB3 (FT3683G) or 0x62 (FT3681). Retry every 200 ms for up to 1 s.
   3. Log 0xA6 (firmware version). Never write 0xA5.
8. **Poll every 10 ms.**
   1. Send a 93-byte full-duplex frame: TX = `[0x01, 0xA0, 0x00, 0x54, 0, 0, 0, then 86 zeros]`.
   2. Check `(rx[3] & 0xA0) == 0` and CRC16(0x8408, init 0xFFFF) over `rx[7..90]` equals `rx[91] | rx[92] << 8`. On failure, keep the current contacts. After 10 consecutive failures, release all.
   3. Take `d = rx + 7`. If `d[1..4]` are all 0xFF, release all. Otherwise parse by `d[1] >> 4` (0 or 2) as in section 1.5.
   4. Build the down mask, call `touch_report(mask, x, y)`, then wait 150 us of CS-high (the 10 ms poll covers it).

---

## Unverified or guessed

1. **The exact IC on this unit**, FT3683G or FT3681. The dmesg captures end before the probe log line. Read 0xA3 and 0x9F on hardware.
2. **Whether L11B and L22B are on at XNU entry.** The missing proxy-consumer suggests they are off. Vote them anyway.
3. **The RPMh RSC hardware revision on volcano** (v2.7 or v3.0 layout). Read the id at 0x17a20000 at runtime. Also check that the cmd-db header at 0x81c60000 passes sweet's version 1 and flags checks.
4. **The TCS type numbering** (SLEEP 0, WAKE 1, ACTIVE 2, CONTROL 3, FAST_PATH 4), and therefore "ACTIVE TCS = 0..2". It is inferred from `rpmh-rsc.c` ordering and the standard binding; the binding header was not read.
5. **Whether SE0 SPI firmware is loaded and the FIFO interface enabled** at XNU entry. This is expected, since XBL loads QUP firmware per SE config and stock Linux probes `a80000.spi` fine. Gate on 0x68 and 0x64.
6. **The SE0 clock state after ABL**, assumed off, and **the QUP0 and CNOC interconnect BCMs** staying voted (keepalive).
7. **The DFS level-0 contents for SE0**, presumably XO at div 1.
8. **The CS_TOGGLE bit position in SE_SPI_TRANS_CFG**: bit 0 in downstream `spi-msm-geni.c:64`, bit 1 in mainline `spi-geni-qcom.c:38`. The recipe follows downstream because it is what this phone runs. If CS toggles per word, also clear bit 1.
9. **Whether a slow poll loop can overflow the RX FIFO** during a 93-byte frame. The RFR watermark should stall SCLK in master mode, but that is not confirmed. Chunking with FRAGMENTATION avoids the question.
10. **Whether the SPI parts report event type 0x2** (v2, 8-byte records, coordinates divided by 16) or 0x0. Handle both.
11. **Whether the IC tolerates SCLK gaps** inside a CS-low frame when chunking. This is normal for SPI slaves but not confirmed for FocalTech.
12. **TLMM direct-interrupt polling** (intr_cfg / intr_status without GIC routing) under Gunyah. It is optional.
13. **The 0x100000 TLMM offset**: confirmed from source, but not yet checked on hardware. The Essential Key address in patch 0103 (0xf047004) is probably wrong for the same reason; the expected address is 0xf147004.
14. **The volcano DDK build list** (`target.bzl`) names only `goodix_ts`, which contradicts the dmesg evidence. The source match is inferred from the `Exit(612)` line number.
15. **Firmware versions in the blobs** (not decoded). They are not needed unless reflashing, which XNU should never do.
