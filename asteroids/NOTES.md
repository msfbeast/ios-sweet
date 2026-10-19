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

## 2026-10-19: M0.1 bootloader unlocked

- **`fastboot getvar all` (locked)** saved to `asteroids/backup/getvar-all-locked.txt` (git-ignored, holds the serial). Bootloader `01955-LANAI-1`, baseband `00560.1-MILOS_GEN_PACK-1`, `product:pineapple`, `kernel:uefi`, `hw-revision:10000`, `max-download-size` 768 MiB.
- **Slots:** `current-slot:a`; slot b is `slot-unbootable:b:yes`, so it never held a working system. Slot a is the only real one.
- **Partitions worth knowing:** `userdata` 0x3835db3000 (about 225 GiB, f2fs, so the 256 GB model), `persist` 32 MiB, `super` 9 GiB, `recovery_a/b` 100 MiB each, `boot_a/b` and `vendor_boot_a/b` 96 MiB, `init_boot_a/b` 8 MiB. Nothing-specific raw partitions: **`nt_kmsg` 64 MiB, `nt_log` 256 MiB**, `nt_uefi`, `nt_reserve1/2` 64 MiB each, `binfo`, plus `rawdump` 256 MiB and `logfs` 8 MiB.
- **Log channel candidate:** `nt_kmsg` / `nt_log` look like Nothing's own kernel-log partitions. A persistent XNU log ring there would leave `userdata` (the iOS root) untouched. Check from OrangeFox what Android writes into them before using one.
- **Unlock:** `fastboot flashing unlock` returned OKAY, confirmed on the phone. After the wipe and offline setup: `ro.boot.vbmeta.device_state=unlocked`, `verifiedbootstate=orange`, `flash.locked=0`, build still `V3.1-250302-1856`, slot `_a`, no active network.

## 2026-10-19: M0.2 OrangeFox in recovery_a, partitions backed up

- **OrangeFox:** `OrangeFox-R12.0_6.1-Unofficial-Asteroids.img` (Ubuntuify release 2026-09-25), SHA-256 `0c676769...dbae9e`, which matches GitHub's asset digest. Same shape as stock recovery: boot image v4, ramdisk only (no kernel; it runs on the boot partition's 6.1 kernel), exactly 104,857,600 bytes, AVB footer. `fastboot flash recovery_a` OK. `fastboot reboot recovery` came up with adb as root (`u:r:su:s0`), `/data` decrypted (f2fs on dm-14), 120 by-name entries.
- **Only `recovery_a` was written.** Stock `recovery.img` (V3.1, hash-checked) restores it.
- **Backup:** `asteroids/backup/20261019-192842` (723 MiB, git-ignored), 22 partitions, each matched to a SHA-256 computed on the phone. Contents: `persist`, `modemst1/2`, `fsg`, `fsc`, `devinfo`, plus both slots of `boot`, `init_boot`, `vendor_boot`, `dtbo`, `vbmeta`, `vbmeta_system`, `vbmeta_vendor` and `recovery`.
- **Slot a = stock V3.1 byte for byte** (`boot`, `init_boot`, `vendor_boot`, `dtbo`, all three `vbmeta`; only zero padding past the image end). So the Nothing Archive V3.1 images are this phone's images.
- **Slot b = factory build `2502172235`** (vbmeta `init_boot` fingerprint `.../UKQ1.241011.001/2502172235`, patch 2024-11-05), marked unbootable after the first OTA to slot a (`2503021856`).
- **Tool fix:** `backup-partitions.sh` now uses `adb -d` unless `ANDROID_SERIAL` is set. With the Android emulator running, plain `adb` stopped with "more than one device/emulator".
