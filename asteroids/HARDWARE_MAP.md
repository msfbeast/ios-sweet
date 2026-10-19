# Nothing Phone (3a) vs Redmi Note 10 Pro (sweet): hardware map

What ios-sweet hardcodes for `sweet`, and what the Phone (3a) (`Asteroids`, A059) has in the same place.

**Sources (2026-10-19)**
- Phone (3a) side: the stock device tree the phone actually boots. That is `vendor_boot` base tree 9 ("Volcano SoC", msm-id 0x27c) plus `dtbo` overlay 64 ("Volcano QRD", board-id 0xb), from firmware `Asteroids_V3.1-250302-1856`. That firmware is the build on Sajid's phone (`ro.boot.dtb_idx=9`, `ro.boot.dtbo_idx=64`), and the images came from [Nothing Archive](https://github.com/spike0en/nothing_archive) with matching SHA-256.
- Read-only `adb` on the phone: build, CPU parts, display modes, lock state.
- sweet side: this repo at `fdad0de` (upstream ios-sweet) (2026-10-18), mostly `tools/mkadt.py`, `NOTES.md` and the driver patches.
- Merged tree: `firmware/Asteroids_V3.1-250302-1856/dt/asteroids.dts`. Rebuild with `asteroids/tools/extract_dtbs.py` plus `fdtoverlay`.

The latest firmware (`B4.1-260928-1204`) has the same addresses for everything below, but its tree indices differ. Use the indices only together with the build they came from.

## The phone right now

| Item | Value |
| --- | --- |
| Build | `V3.1-250302-1856`, Android 15, kernel 6.1.84 (launch firmware, never updated) |
| Variant | `AsteroidsIND`, 8 GB RAM (`MemTotal` 7.3 GiB) |
| Bootloader | `01955-LANAI-1`, **locked** (`verifiedbootstate=green`), OEM unlocking **allowed** in settings |
| Slot | `_a` (A/B device) |
| CPU parts | 4x `0xd80` Cortex-A520, 4x `0xd81` Cortex-A720 |

## Side by side

| Area | sweet (SM7150), as ios-sweet uses it | Phone (3a) (SM7635 "Volcano") | Port impact |
| --- | --- | --- | --- |
| Boot image | boot.img v2, DTB inside the boot image | boot v4 + `vendor_boot` v4 + `init_boot` + `dtbo`, A/B | Shim must be repacked as a v4 boot image. ABL takes the DTB from `vendor_boot`+`dtbo` on flash, so the stock tree still arrives in `x0`. With v4, ABL concatenates the vendor ramdisk fragments, then boot.img's ramdisk, then bootconfig. The shim's `loader.c` reads the payload from `linux,initrd-start` and rejects a start that isn't 8-aligned. The vendor ramdisk is 8,822,676 bytes (4 mod 8), so the payload may land 4-aligned behind it. The loader must search for it and accept that alignment (M1 probe measures it) |
| `fastboot boot` | Works when unlocked | ABL has the string "Fastboot boot command is not available in locked device", so it is gated on unlock, not removed | Likely works after unlocking. Not tested yet |
| ABL display hand-off | Splash left on at 0x9c000000 | ABL looks up `/reserved-memory/splash_region` in the DT it passes on. If missing: "powering down the display and controller" | Keep the stock DT intact up to the shim. The shim then reads the region from it |
| Framebuffer | 0x9c000000, 1080x2400, stride 4320, a8r8g8b8, DSC video-mode panel | Static DT value `splash_region` 0xe3940000, size 0x2b00000 (43 MiB, about 4x one 1080x2392x4 frame). ABL rewrites it at boot ("Splash memory region before/after updating"). Panel 1080x2392, RM69220 AMOLED (Visionox or BOE, ABL picks), **DSC video mode** | Same display model as sweet: no kick needed. The mainline 3a tree (Koskovich, LKML 2026-03) drives the panel as `simple-framebuffer` at **0xe3940000, 1080x2392, stride 4320, a8r8g8b8**, so that layout works in practice. The shim should still check `splash_region` in the FDT from `x0` |
| Screen size presented to iOS | 1125x2436 (iPhone X) on a 1080x2400 panel | 1080x2392 panel, 120/90/60/30 Hz modes, 126 px punch-hole cutout at top | Same trick works. Scaling is 2392 vs 2400 rows, so 8 rows less |
| CPU | 6x A55 + 2x A76, MPIDR Aff1 = core | cpu0-3 A520 (cluster0), cpu4-6 A720 (cluster1), cpu7 A720 prime (cluster2). MPIDR `0x000`..`0x700`, Aff1 = core | Same MPIDR scheme. ARMv9.2 cores run the non-PAC arm64 kernel. Patch 0078 (8 cores) is the model |
| PSCI | `smc` | `arm,psci-1.0`, `smc` | Same |
| Hypervisor | QHEE | **Gunyah** (carve-out at 0x80000000, `HypInfo` calls in ABL) | XNU stays an EL1 guest as on sweet. Stage-2 limits are unknown: first boot will show what faults |
| GIC | GICv3, GICD 0x17a00000, GICR 0x17a60000, **stride 0x20000** | GICv3, GICD **0x17100000**, GICR **0x17180000** (size 0x200000), **stride 0x40000**, ITS at 0x17140000 | New addresses. sweet hardcodes `GICR_PE_SIZE 0x20000` in patch 0004. **It must become 0x40000** (or come from the ADT), or every core after cpu0 hits the wrong frame |
| Timer | 19.2 MHz, virtual timer INTID 19 | 19.2 MHz (`0x124f800`), PPIs 13/14/11/12, so the **virtual timer is PPI 11 = INTID 27** | Change `virtual-timer-intid` in the ADT |
| RAM | Upper bank 0x1_00000000 + 0xfcc00000 | 8 GB. Fixed carve-outs end at 0xfc800000 (OEM VM). The `memory` node is filled in by ABL | Same "use the upper bank" approach is likely. The exact bank list has to come from the live FDT the shim receives |
| Persistent log | 4 MiB ring at the end of `userdata`, read from OrangeFox | Channels in bring-up order: (1) text on the ABL framebuffer, filmed (how sweet got its first boots; only needs the live splash buffer); (2) `ramoops` at **0x81f20000** (4 MiB, no-map), read back from a rooted Android or a custom recovery; (3) `userdata` ring as on sweet, if an OrangeFox build exists; (4) USB live log (sweet 0077), last, because it needs the new eUSB2 PHY first. ABL also has `fastboot oem dmesg` (unknown output) | Framebuffer text first. Everything else comes after M1 |
| USB | DWC3 0x0a600000, QUSB2 PHY 0x088e2000, GCC GDSC | DWC3 at **0xa600000** (`qcom,dwc-usb3-msm`), **eUSB2 PHY + PMIC eUSB2 repeater** (pmiv0104 `@fd00`) | Controller address matches. PHY setup is new. If ABL leaves the USB link up after fastboot, sweet's ECM gadget (0072-0077, 0081) may need only the PHY part |
| Debug UART | Wired as SPI, unusable | `qcom,geni-debug-uart` at **0xa94000** (QUP0 SE5), 115200, `status = "ok"`. ABL has `console=ttyMSM0` and `NTUartStatus` | The mainline tree says RX/TX go to **test points on the board**, not USB. Usable only with the back off and wires soldered on. Optional |
| Storage | UFS host 0x01d84000 (`ufs_mem`), shim + XNU drivers | UFS host **0x1d84000** (`qcom,ufshc`), 2 lanes, ICE at 0x1d88000, PHY `qcom,ufs-phy-qmp-v4-crow`, UFS 2.2 | **Same host address.** ABL leaves UFS running, so sweet's driver may work with small register checks. Best early win |
| Touch | Goodix GT9896, I²C over GENI, SE at 0x00a84000, RPMh rail vote | **FocalTech `fts_ts` over SPI**, GENI SE0 at **0xa80000**, IRQ TLMM 19, reset TLMM 20, 10 points, coords 1080x2392 | New chip protocol **and** GENI SPI instead of I²C. Rails: `vddio` and `vci` supplies |
| Power key | PMIC PON over SPMI | `pmk8350-pwrkey` in `pon_hlos@1300` on PMK8550 (SPMI sid 0) | Same idea |
| Volume down | PMIC RESIN | `pmk8350-resin`, same PON block | Same idea |
| Volume up | PMIC GPIO | PMXR2230 (SPMI sid 1) GPIO 6 | New PMIC, still SPMI |
| Essential Key | (none) | `ai_key`, TLMM GPIO 71, code 0xfa | Bonus button. Could map to Siri or AssistiveTouch |
| SPMI arbiter | cnfg 0x0c40a000, core 0x0c440000, obsrvr 0x0e600000 | cnfg **0xc42d000**, core 0xc400000, chnls 0xc500000, obsrvr 0xc440000, intr 0xc4c0000 | Newer arbiter generation, so the register layout differs. Key driver needs a rework |
| RPMh / cmd-db | RSC 0x18220000, cmd-db dict 0x0c3f000c | `apps_rsc` at **0x17a00000** (drv-0/1/2), cmd-db at **0x81c60000** | New addresses, same mechanism (needed for touch power) |
| TLMM | 0x03900000 (north tile) | **0xf000000** (`qcom,volcano-pinctrl`, single block) | New base, simpler layout |
| Clocks | GCC 0x00100000 | GCC 0x100000 (`qcom,volcano-gcc`) | Same base. Register offsets differ |
| Battery | Static 100 % | `pmic_glink` + `battery-charger` (lives in the ADSP firmware) | A real reading needs the ADSP booted plus GLINK. That's a big job, not a quick win |
| GPU | Adreno 618, unused | `qcom,adreno-gpu-gen8-3-0` at 0x3d00000 (consistent with Adreno 810) | Unused, same as sweet. CPU rendering stays |

## Corrections to the pasted plan

1. **"Use the 3a's debug UART":** it works, but only through board test points (needs opening the phone). Plan on filmed framebuffer text first, then `ramoops` and the `userdata` ring, with the USB live log (sweet patches 0072-0077, 0081) last. The UART is a bonus if pads turn up.
2. **"Generated from Nothing's published DTS":** we already have something better: the exact compiled tree this phone boots. Kernel sources are still useful for driver behaviour (FocalTech SPI protocol, eUSB2), not for addresses.
3. **"UFS host driver retargeted":** the host is at the same address as on sweet. Treat it as a check-and-adjust job, not a rewrite.
4. **"Mainline PMIC-GLINK for a real battery reading":** correct path, but it needs the ADSP firmware running first. Leave it after touch, buttons and USB.
5. **GPU "Adreno 810":** the tree's `gen8_3_0` is consistent with it.
6. **New item missing from the plan:** the GIC redistributor stride (0x40000) and the virtual timer INTID (27) have to change before anything boots.

## Still unknown (needs the unlocked phone)

- That the splash buffer is still at 0xe3940000 on this unit and build (mainline says yes).
- Whether `fastboot boot` of a v4 image works on this ABL (`fastboot boot` of the **stock** `boot.img` from this build is the harmless test).
- The live FDT ABL passes: memory banks, chosen splash info, the pixel format and stride of the splash buffer.
- What `fastboot oem dmesg` returns.
- Whether ABL leaves the USB PHY and UFS link up when it jumps to the kernel.
- Which panel variant this unit has (Visionox or BOE): ABL picks it at boot and puts it on the kernel command line, which isn't readable without root.

## Other sources checked

- Nothing kernel + DT: [NothingOSS/android_kernel_msm-6.1_nothing_sm7635](https://github.com/NothingOSS/android_kernel_msm-6.1_nothing_sm7635) (default branch `sm7635/v/mr`, covers 3a, 3a Pro and 4a).
- Mainline DT: Alexander Koskovich, [v1](https://lkml.iu.edu/2603.2/12985.html) and [v2](https://lkml.iu.edu/hypermail/linux/kernel/2603.2/15183.html), `milos-nothing-asteroids.dts`. Source of the framebuffer and UART notes above.
- OrangeFox: [Ubuntuify/device_nothing_asteroids-recovery](https://github.com/Ubuntuify/device_nothing_asteroids-recovery), release 2026-09-25, `6.1` image for stock Nothing OS. **Its notes say to flash it to `recovery`, not `fastboot boot` it.** It has adb, backup, slot switch and decryption.

## Files

| Path | What |
| --- | --- |
| `asteroids/tools/extract_dtbs.py` | Splits `vendor_boot` v3/v4 and `dtbo` into individual trees with their msm-id and board-id |
| `firmware/Asteroids_V3.1-250302-1856/` | Phone's build: `dl/` archives + hash lists, `img/` boot images, `dt/` split trees, `asteroids.dtb/.dts` merged tree |
| `firmware/Asteroids_B4.1-260928-1204/` | Latest stable build, the same plus `abl.img`, `xbl.img` and other firmware images |
| `shim/`, `tools/`, `xnu/patches/` (repo root) | Upstream sweet code at `fdad0de`, the reference |

`firmware/` stays out of git: it holds Nothing's firmware images.
