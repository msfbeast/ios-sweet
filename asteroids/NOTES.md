# asteroids engineering log

Port of ios-sweet to the Nothing Phone (3a) (`Asteroids`, A059, SM7635 "Volcano"/"Milos"). Every boot, crash and decision goes here, newest last, like the upstream `NOTES.md`. Hardware facts live in `HARDWARE_MAP.md`; device steps in `RUNBOOK-M0-M1.md`.

## 2026-10-19: groundwork (no device boots yet)

- **Base:** fork `msfbeast/ios-sweet` of `dipa-studio/ios-sweet` at `fdad0de` (upstream patch 0081, USB ECM live log + 8-core SMP). 3a work stays in `asteroids/` (its own `shim/`, `tools/`, later `xnu-patches/`); upstream files stay untouched so upstream merges stay clean.
- **Phone state (read-only adb):** `AsteroidsIND`, 8 GB, build `V3.1-250302-1856` (Android 15, kernel 6.1.84), slot `_a`, bootloader `01955-LANAI-1` **locked**, OEM unlocking allowed. CPU parts 4x `d80` (A520), 4x `d81` (A720). Display 1080x2392, modes 120/90/60/30 Hz. Booted trees: `ro.boot.dtb_idx=9`, `ro.boot.dtbo_idx=64`.
- **Firmware:** stock images from Nothing Archive, SHA-256 checked against their lists: `Asteroids_V3.1-250302-1856` (the phone's build, boot images) and `Asteroids_B4.1-260928-1204` (latest, all firmware images incl. ABL). Kept in `firmware/` (git-ignored).
- **Device tree:** the phone boots `vendor_boot` base 9 ("Volcano SoC", msm-id 0x27c) + `dtbo` 64 ("Volcano QRD", board-id 0xb). Indices differ between builds (in B4.1 the same pair is 12 + 65). Merged with `fdtoverlay` into `asteroids.dts`. `tools/extract_dtbs.py` splits the images.
- **ABL findings (B4.1 `abl.img`, LZMA FV at 0x1078):** "Fastboot boot command is not available in locked device" (so `fastboot boot` is gated on unlock, not removed); `oem dmesg`, `oem select-display-panel`, a `NTUartStatus` string; it looks up `/reserved-memory/splash_region` and powers the display down if the node is missing; Gunyah `HypInfo` overlays.
- **Mainline cross-check:** Koskovich's `milos-nothing-asteroids.dts` (LKML 2026-03-21/23) runs simple-framebuffer at 0xe3940000 1080x2392 a8r8g8b8 stride 4320, and says the debug UART RX/TX go to board test points only.
- **Decisions:**
  - First log channel is text on the ABL framebuffer, filmed (same as sweet's first boots). ramoops (0x81f20000) and the `userdata` ring come next. The USB live log comes last because it needs the new eUSB2 PHY.
  - GIC redistributor stride 0x40000 (upstream hardcodes `GICR_PE_SIZE 0x20000` in patch 0004); virtual timer INTID 27 (upstream 19).
  - With boot image v4, ABL puts vendor_boot's 8.4 MiB ramdisk first. So the XNU loader has to search the initrd for its payload instead of assuming offset 0. M1 measures where it lands.
- **M1 probe shim built** (`asteroids/shim`, `aarch64-elf-gcc` 16.2, AOSP `mkbootimg.py` v4): `out/asteroids-probe-boot.img`, SHA-256 `114da9b16640722c37f328307df0853c60f07c476df0a73fd35523b73666e585`. Read-only probe: CPU ID registers, live FDT (memory banks, splash, panel name, initrd), marker offset in the initrd, PSCI + CPU_ON x7, GICv3 vtimer/SGI test at the 3a addresses, UFS VER/CAP/HCS/HCE. The new FDT path reader was tested on the Mac against the real merged tree. **Not booted yet.**
- **Probe fixes before first boot:** the vendor ramdisk is 8,822,676 bytes (4 mod 8), so the marker may sit at a 4-aligned address. The search steps 4 bytes with two 32-bit reads (no unaligned 64-bit loads with the MMU off). Upstream `loader.c` rejects `rd_start & 7`, so the 3a loader must accept a 4-aligned payload or pad it. Text starts at row 2 to clear the punch hole (x 454-540, y 0-126) and the 100 px rounded corners.
- **Housekeeping:** `gh` was active on the OspreySK account, so the first fork landed at `OspreySK/ios-sweet` by mistake. Sajid decides whether to delete it.
