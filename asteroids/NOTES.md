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

## 2026-10-19: M1.1 `fastboot boot` works; `oem dmesg` is a log channel

- **`fastboot boot` of stock V3.1 `boot.img`:** sent 96 MiB, `Booting OKAY` after 10.4 s, Android up with kernel `6.1.84-android14-11-gf4b0d69fdf34`, slot `_a`, `sys.boot_completed=1` at 23 s uptime. **Temporary boot of a v4 image works on this ABL.**
- **`fastboot oem dmesg`** (after `adb reboot bootloader`, a warm reset) returned 300 KB: the full kernel log of that boot. It starts with `DBGC`, Linux pstore's `PERSISTENT_RAM_SIG`, so ABL dumps the **ramoops console zone at 0x81f20000** raw (header u32 sig, start, size, then text). ABL loses a few bytes at each 64-byte INFO chunk boundary. `tools/oem-dmesg.py` joins the chunks; raw and clean copies are in `asteroids/backup/oem-dmesg*.txt` (git-ignored).
- **Facts from that log:**
  - RAM: low bank 0x80e00000..0xffffffff in 9 pieces. **High bank 0x8_80000000..0x9_ffffffff** with holes; largest run 0x8_bf800000..0x9_ffffefff. Total 7,839,672 KiB.
  - CPU0-3 `MIDR 0x410fd801` (A520), CPU4-7 `0x410fd811` (A720). PAC with QARMA3. A520 and A720 differ in ID_AA64MMFR1.
  - PSCI 1.1, SMCCC 1.3, OSI mode. Gunyah carve-out 0x80000000..0x80dfffff. `kvm-arm.mode=protected` on the command line.
  - GICv3: 988 SPIs, 16 PPIs, DirectLPI, CPU1's redistributor at 0x171c0000 (**stride 0x40000 confirmed**), ITS at 0x17140000. Arch timer 19.2 MHz, virtual.
  - Linux warns "Kernel image misaligned at boot": ABL doesn't place the image at a 2 MiB boundary. The shim is position independent, so this doesn't affect it.
- **Probe update:** the console now mirrors every line into the ramoops zone in pstore format (`con_log_init`), so after the probe a warm reset plus `oem dmesg` returns its text. That also answers whether RAM survives Power + Vol-. Host-tested the writer. New image SHA-256 `91c807250588683445ddeddb6c471de2cf1f48a60d922ad10cc9d508585dbc4d`.

## 2026-10-19: M1.2 probe run 1: our code runs; GICD read aborts under Gunyah

- **Image** `91c80725...dbc4d` via `fastboot boot`: `Sending 88 KB OKAY`, `Booting OKAY` (10.5 s). Evidence: Sajid's photo of the blue panic screen.
- **Result:** ABL ran the shim, the framebuffer at 0xe3940000 works (full-screen blue panic drawn by our handler), and the stages before the GIC ran: CPU regs, FDT, initrd, PSCI, CPU_ON x7. Panic: `SYNC`, slot 4 (current EL, SPx), cpu 0, stage `gic dist`, **ESR 0x96000010** (EC 0x25 data abort at EL1, ISV 0, DFSC 0x10 *synchronous external abort*), **FAR 0x17100000** (GICD_CTLR, the first distributor read), ELR 0xA80021CC (ABL loaded the image near 0xA8000000), SPSR 0x820003C5.
- **Reading:** Linux in the Android VM reads the same GICD (988 SPIs), so Gunyah emulates or passes it for Linux but returned an external abort to our access. Open causes: Gunyah's vGIC state after our PSCI CPU_ON/CPU_OFF of the 7 secondaries (they ran first in run 1), or something about how ABL hands over a non-Linux image.
- **ramoops check failed to test:** the panic screen cleared the earlier lines, and the following `oem dmesg` returned a *new Android* log (process IDs differ from the first dump). Android booted after the forced restart and pstore reset the zone, so whether RAM survives the forced restart is still unknown.
- **Probe v2** (`d6dddbcb2b2c7118078ef07125e8ce17787077bf59c72cb6592ab12b20b71398`):
  - GIC and UFS accesses go through `mmio_rd32/rd64/wr32`: a data abort on the boot CPU while `probing` is set records ESR and skips the instruction.
  - Order is now FDT, SMCCC vendor-hypervisor UID, GIC (ICC regs, GICD CTLR/TYPER/PIDR2, every GICR frame's TYPER, then the timer and SGI tests if the own frame is reachable), UFS, and PSCI SMP last.
  - The panic screen keeps the earlier lines.

## 2026-10-19: M1.2 probe run 2: memory map, Gunyah, GIC rules, and a working log

- **Image** `d6dddbcb...71398`. Screen photo, plus the same text read back with `fastboot oem dmesg` (`backup/oem-dmesg-probe2*.txt`).
- **CPU:** EL1, boot core A520 (`MIDR 0x410fd801`), `MPIDR 0x81000000` (MT=1, core in Aff1), CNTFRQ 19.2 MHz, **ID_AA64MMFR0 TGran16 = 1 (16K pages supported)**, PARange 2 (40-bit), PFR0 GIC=3, EL2=1.
- **Live FDT:** DTB at 0xb738f000. `/memory` (first 8 entries, 4K pages): 0x80e00000+0xa00, 0x81cf5000+0xa, 0x81f20000+0x580, 0xeea00000+0x11600, 0x8_80000000+0x2fb00, 0x8_b0000000+0x9700, 0x9_00000000+0x100000, 0x8_bf800000+0x40800. So **0x8_bf800000..0xa_00000000 is contiguous (5 GiB)**. Splash 0xe3940000 / 0x2b00000. Panel **`rm69220_fhd_plus_120hz_vid_vxn`** (Visionox).
- **Initrd** 0xb7590000..0xb7fff4a8 = 10,941,608 bytes = vendor ramdisk 8,822,676 + **`init_boot` ramdisk 2,113,583** + 5,349 (bootconfig). Our marker is absent: **ABL ignores boot.img's ramdisk when `init_boot` exists.** The XNU payload must ride inside the kernel image instead (append after the shim, find it via `_end`).
- **Hypervisor:** SMCCC vendor-hyp UID w0 = `0xcd8fd5c1`, the first word of Gunyah's UUID `c1d58fcd-a453-5fdb-9265-ce36673d5f14`. **Gunyah confirmed.** PSCI 1.1, SMCCC 1.3.
- **GIC:** ICC_SRE=1, ICC_CTLR 0x88400. **GICD reads work this time** (CTLR 0xd0, TYPER 0x016b001f = 1024 lines, PIDR2 0x30 = GICv3). GICR frame 0 at 0x17180000 has affinity 0 (CPU0), WAKER 0. Then a **synchronous external abort at 0x17190400 (`GICR_IPRIORITYR`)** on the probe's *byte* writes, while the 32-bit `IGROUPR0` write just before worked. **Rule: under Gunyah, touch the GIC with 32-bit accesses only.** Sweet's XNU GIC code already writes priorities as 32-bit words.
- **Run 1 vs run 2:** the only order difference before the GICD access was PSCI CPU_ON/CPU_OFF of the 7 secondaries (run 1). Hypothesis: after a vCPU is powered off, Gunyah refuses distributor access from the remaining vCPU. Run 3 re-reads GICD after the SMP test.
- **Log channel proven:** RAM survived Power + Vol- straight to fastboot, and `oem dmesg` printed the probe's lines from 0x81f20000. ABL ignores the pstore size field (old Android text followed ours) and lost the first ~50 bytes, so the probe now clears 64 KiB and writes 4 padding lines first.
- **Probe v3** `72cc8b7f09c438d8d1206d8ea011cc7b0e6511d9c5ce1774b013b1b14cace358`: 32-bit priority and enable writes (guarded), GICD re-read after SMP, cleaner log.

## 2026-10-19: M1 done: probe run 3 completes on the Phone (3a)

- **Image** `72cc8b7f...ce358`, read back in full with `fastboot oem dmesg` after Power + Vol- (`backup/oem-dmesg-probe3*.txt`). The padding worked: ABL dropped part of the first lines only.
- **GIC under Gunyah, 32-bit accesses only:** priorities + ISENABLER0 written as words without aborts. **Virtual timer INTID 27: 30 IRQs in 300 ms (0x1e)**, pending bit 27 seen before unmasking, last INTID 0x1b. **SGI 1: 10 of 10.** Group 1 IRQs through ICC_IAR1/EOIR1 work.
- **UFS host (read only):** VER 0x300 (UFSHCI 3.0), CAP 0x1587031f, HCS 0x10f (device present, UTRL/UTMRL ready, UIC ready), HCE 1. **ABL leaves the UFS host enabled**, as sweet's driver expects.
- **PSCI:** FEATURES(CPU_ON) = 0, FEATURES(SYSTEM_RESET2) = 0 (both supported). **All 7 secondaries came up and turned off:** CPU1-3 MPIDR 0x81000100..300 MIDR 0x410fd801 (A520), CPU4-7 0x81000400..700 MIDR 0x410fd811 (A720).
- **GICD after SMP:** CTLR 0xd3 (our Group 1 enable), TYPER unchanged. So CPU_ON/CPU_OFF did *not* break distributor access. **Run 1's GICD abort stays unexplained** (same 32-bit read of 0x17100000). Treat GICD access as possibly flaky under Gunyah: XNU's GIC code should survive or retry an external abort there, and the first XNU boots should log every GICD access.
- **M1 is complete.** Inputs for M2 (ADT + XNU early console):
  - phys window inside 0x8_c0000000..0x9_c0000000;
  - GICD 0x17100000 / GICR 0x17180000, stride 0x40000, 32-bit access only;
  - vtimer INTID 27;
  - payload appended to the shim image, because ABL ignores boot.img's ramdisk;
  - framebuffer 0xe3940000 1080x2392 stride 4320;
  - console mirrored into ramoops 0x81f20000 for `oem dmesg`;
  - UFS host at 0x1d84000 already enabled;
  - Gunyah, PSCI 1.1.

## 2026-10-20: M2 groundwork: payload in the shim image, 3a ADT, patch 0100, CI

- **Loader:** ABL ignores boot.img's ramdisk, so `shim/build.sh PAYLOAD` appends the SWEETXNU payload at `_end` (16-aligned) and grows the Image header's `image_size` to cover it. `loader_find()` checks the magic there. With a payload, `shim_main` prints CPU/FDT and calls `loader_boot()` (sweet's loader with the live splash base, 1080x2392). Without one it runs the probe. Dummy-payload test: magic lands at exactly the aligned `_end`.
- **ADT:** `tools/mkadt.py` (copy of upstream's, uses upstream `tools/adt.py`):
  - dram 0x8_c0000000 + 4 GiB;
  - GICD 0x17100000 / GICR 0x17180000 size 0x200000, `virtual-timer-intid` 27;
  - `sweet-fbcon` 0xe3940000 1080x2392 stride 4320;
  - `sweet-ufs` 0x1d84000;
  - informational `asteroids-ramoops` node and `redistributor-stride`;
  - root compatible adds `nothing,asteroids` + `qcom,sm7635` and keeps `xiaomi,sweet`, `qcom,kryo470` and `sweet,display` so the drivers match.
  
  `/product` is missing until the D22AP device tree is extracted from the iPhone X IPSW.
- **Patch inventory** (subagent over all 81 upstream patches): Tier A (early console) needs the framebuffer PA, height, two-block mapping and alias offset; Tier B (fbcon) and the GIC addresses are ADT-driven already; `GICR_PE_SIZE` is hardcoded. Keys, touch and USB start by default and poke sweet MMIO: boot with `sweetkeys=0 sweettouch=0 sweetusb=0`. The persistent log writes the last 4 MiB of `ufsroot=` (default userdata) when `ufsrw=1`, so M2 boots must not set `ufsrw`/`rootrw`/`rd=ufsroot0`.
- **`asteroids/xnu-patches/0100-asteroids-board.patch`** (made against upstream CI's `xnu-buildtree` of `fdad0de`, `git apply --check` clean):
  - `SWEET.h`: `SWEET_ASTEROIDS`, `SWEET_EARLY_FB_*` (PA 0xe3940000, 1080x2392, stride 4320, 2 x 32 MiB blocks from 0xe2000000), default vtimer 27, `GICR_PE_SIZE 0x40000`;
  - `start.s`: stage-0 band from the macros; V=P framebuffer mapping loops over 2 blocks (x9 = block size, x14 = count; both set before use in `common_start`);
  - `sweet_early.c`: geometry from the macros; relocated pointer = alias + (PA - block PA);
  - `arm_vm_init.c`: kernel-VA alias fills 2 L2 slots.
  
  **Not changed: `pe_fiq.c`'s 64-bit `GICR_TYPER` read.** Probe runs 2 and 3 made that exact 64-bit read under Gunyah without an abort.
- **CI:** `.github/workflows/xnu-asteroids.yml` = upstream workflow + `asteroids/xnu-patches/*.patch` + a step that fails unless patch 0100 is visible in the built tree (`build.sh` only warns when a patch doesn't apply). Artifacts `xnu-kernel-asteroids`, `xnu-buildtree-asteroids` (the file inside is still `kernel.development.sweet`).
- **ABL kernel-size cap:** ABL has "Kernel size is over the limit" and "BootImage: Size is greater than max download size" (768 MiB). The first limit's value is unknown, so test a large image before the first XNU boot.

## 2026-10-20: kernel build 1, size test, patch 0101

- **Size test:** probe + 64 MiB padding (`asteroids-sizetest-boot.img`, 65,628 KB) `Booting OKAY`. ABL's kernel-size cap is above 64 MiB. Whether the probe then ran couldn't be told from `oem dmesg` (same text as run 3).
- **CI run 39313723440** (`xnu-asteroids`, patch 0100): success, including the applied-patch check. `kernel.development.sweet` 17,206,056 bytes, SHA256SUMS OK. Kept in `asteroids/kernels/` (git-ignored).
- **Payload** (`tools/mkxnuboot.sh`): virt_base 0xfffffe0006000000, entry 0xfffffe000748b488, kernel_end 0xfffffe00084c8000, payload 16,610,992 bytes, boot image 16.7 MB (`159bb995...597a`). Boot-args: `debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress wdt=-1 sweetkeys=0 sweettouch=0 sweetusb=0`. No root device, so it is expected to stop at root mount at best.
- **Kernel VA room:** the framebuffer alias starts at VM_MIN_KERNEL_ADDRESS + 32 MiB = 0xfffffe0002000000, and two blocks end exactly at virt_base 0xfffffe0006000000. There is **no room for a third alias block**, so the first 0101 draft (log as a third block) was replaced.
- **Patch 0101 (ramoops console):**
  - `start.s` maps the 0x80000000 block VA=PA as Device memory;
  - `sweet_early.c` mirrors early putc / early panic text, and `_cnputs` mirrors all console text, into 0x81f20000 in pstore layout, continuing after the shim's lines;
  - at `sweet_early_relocate` (TTBR0 teardown) text is held in a 64 KiB RAM buffer;
  - `pe_serial.c` calls `sweet_ramoops_map()` (`ml_io_map`, Device) before the fbcon mapping, which flushes the held text and logs `[asteroids: ramoops remapped]`.
  
  Applies after 0100 on the clean tree.

## 2026-10-20: M2 done: XNU boots on the Phone (3a) to the root-device wait

- **Boot:** `fastboot boot asteroids-xnu2-boot.img` (CI run 39315293493, patches 0100 + 0101, `700e8a16...6de9`). Log via Power + Vol- to fastboot, then `fastboot oem dmesg` (`backup/oem-dmesg-xnu2*.txt`). ABL drops a few bytes per 64-byte chunk, so the quotes below have gaps.
- **Shim:** payload at 0xa8095000, PHYS 0x8_c0000000 / 4 GiB, 10 segments, end 0x8_c24d8000, boot_args at 0x8_c24dc000, jump to 0x8_c148c488.
- **Early stages 1-20 all passed:** arm_init with MMU on, PE_init_platform, ml_parse_cpu_topology, cpu_bootp, kernel_startup_bootstrap, cpu_init, arm_vm_init, leaving bootstrap TTBR0 (two-block framebuffer alias + relocation, patch 0100), physmap, pmap_bootstrap, early I/O page tables, serial_init. **`[asteroids: ramoops remapped]`** is printed right before fbcon (patch 0101 hold buffer + `ml_io_map`); fbcon mapping and kprintf follow.
- **Kernel:** Darwin banner; vm_page_bootstrap 258,756 free pages; zone_init over the window; firehose/oslog; AMFI stand-in, trust cache unavailable (expected), device-tree platform expert, AppleKeyStore stub, IOWatchdog stub, IOSurface root, HID buttons + touch services (drivers off by boot-arg), static battery 100%, **`AppleCLCD 1080x2392 stride 4320 at 0xe3940000 ... presented as 1125x2436`**, **`SWEET CPU: cpu=0 mpidr=0x0 came alive; SGI1 OK`** (GIC + vtimer under Gunyah, `GICR_PE_SIZE 0x40000`), MAC framework, permissive sandbox, HFS+, mbuf, dlil (lo0, gif0, stf0).
- **Stops at:** `Waiting on <dict ... IOMedia ... Apple_HFS ...>`, i.e. no root device. Expected, because the M2 boot-args set no `rd=`. Also `debug_log_init: gPanicBase not initialized` and `Failed to write boot-picker-bringup-reason to NVRAM` (harmless, as on sweet).
- **Observation:** the old Android text left in the zone after our log now shows bit flips ("iniT: pbocessing"). Possibly DDR retention across the forced resets; our own freshly written text is clean apart from ABL's chunk drops. Keep an eye on it.
- **Next (M3):** root on UFS. Sweet's UFS driver at the same host address (UFSHCI 3.0, enabled by ABL, CAP.64AS set) with `rd=ufsroot0 ufsroot=<partition>`, plus the iPhone X 16.7.16 HFS+ root image (`tools/mkiosroot.sh`, needs Linux) and the D22AP `/product` node for the ADT. Decide which partition holds the iOS root: `userdata` (225 GiB, Android data already wiped) as on sweet.
