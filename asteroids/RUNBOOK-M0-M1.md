# Phone (3a) runbook: M0 (unlock + backup) and M1 (first probe boot)

Every command runs on the Mac, from the repo root (`~/Nothing x iOS`). Each step says what it risks. Stop at any step that doesn't go as written and send me the output.

Rules for every step:
- Nothing here writes to the bootloader, modem or persist partitions.
- The phone's serial number and IMEI show up in `getvar all` and in the backups. Keep those files out of chats, git and cloud folders.

## Before you start

- **Unlocking wipes the phone.** Everything in Android user storage is erased, including photos, apps and eSIM profiles: move any eSIM off first. Copy anything you need now.
- **Warranty:** Nothing lets you unlock officially, but I couldn't find its warranty terms for unlocked phones in India. Ask Nothing support before unlocking if the warranty matters to you.
- **While unlocked, on Android:** the boot screen shows an "unlocked" warning, and apps that check device integrity (some UPI and bank apps, Google Wallet) may refuse to run. Relocking later needs fully stock images and wipes again.
- **Apple licensing:** iOS's licence only allows it on Apple hardware. Building and booting it here is a personal experiment. Never publish Apple files (IPSW contents, root images, the shared cache, `d22ap.adt`), and never put them in the GitHub fork. That fork is public, like every fork of a public repo. XNU itself is Apple open source (APSL 2.0), so the patches are fine to publish.
- **Charge the phone above 60 %** and use a known-good USB-C data cable.

## M0.1: Unlock the bootloader (wipes the phone)

1. On the phone: Settings > System > Developer options > **OEM unlocking** is already allowed (`sys.oem_unlock_allowed=1`). Leave **USB debugging** on.
2. Reboot to the bootloader:
   ```bash
   adb -d reboot bootloader
   ```
   *Risk: none.* (`-d` picks the USB phone, not the Android emulator that is also running.)
3. Check fastboot sees it and record the current state:
   ```bash
   mkdir -p asteroids/backup && fastboot getvar all 2>&1 | tee asteroids/backup/getvar-all-locked.txt
   ```
   *Risk: none (read-only).* The file holds the serial number; it stays in `asteroids/backup/`, which git ignores.
4. Unlock:
   ```bash
   fastboot flashing unlock
   ```
   On the phone, use the volume keys to pick **UNLOCK THE BOOTLOADER**, then press Power.
   *Risk: **erases all user data.** Changes only the unlock flag (`devinfo`), no partitions are flashed.*
5. The phone reboots and wipes itself. Let Android start once and **do setup offline** (skip Wi-Fi and SIM, no Google account). Then Settings > System > System update: turn off automatic updates. An update to B4.1 now would stop our V3.1 images, tree indices and `recovery.img` restore path from matching the phone. Then turn on Developer options and USB debugging again, then:
   ```bash
   adb shell getprop ro.boot.vbmeta.device_state
   ```
   It should print `unlocked`.

Send me: the output of steps 3 and 5.

## M0.2: Back up the device partitions

Our stock images (`firmware/Asteroids_V3.1-250302-1856/img/`) are hash-checked against Nothing Archive's list for this build; the backup script checks whether the phone's own partitions match them. The irreplaceable parts are per device: `persist`, `modemst1`, `modemst2`, `fsg`, `fsc`, `devinfo`. They can only be read as root, and stock Nothing OS has no root. **This needs your decision first:**

- **Option A (recommended): OrangeFox in `recovery`.** [Ubuntuify's OrangeFox](https://github.com/Ubuntuify/device_nothing_asteroids-recovery/releases/tag/2026-09-25), image `OrangeFox-R12.0_6.1-Unofficial-Asteroids.img`, the stock-ROM build. Its README says to **flash** it, not `fastboot boot` it. That flashes the `recovery` partition only (not boot, bootloader, modem or persist). Our verified stock `recovery.img` puts it back at any time. It also gives us sweet's way of reading the kernel log later.
- **Option B: try `fastboot boot` of OrangeFox anyway.** Nothing is written, but the maintainer says it doesn't work properly that way; worst case it hangs and you force-restart.
- **Option C: Magisk** (flash a patched `init_boot`). Gives root in Android, but changes the boot chain. Not recommended here.

Once in OrangeFox (Option A or B) with the phone on USB:

```bash
asteroids/tools/backup-partitions.sh
```
*Risk: none on the phone (only `dd if=` and `sha256sum`).* It saves `persist`, `modemst1/2`, `fsg`, `fsc`, `devinfo` and both A/B slots of `boot`, `init_boot`, `vendor_boot`, `dtbo`, `vbmeta*` and `recovery`. Each file is checked against a hash computed on the phone. Copy the folder to an offline drive afterwards.

Restoring stock recovery after Option A, if you ever want to:
```bash
fastboot flash recovery firmware/Asteroids_V3.1-250302-1856/img/recovery.img
```

## M1.1: Prove that `fastboot boot` works with a known-good image

From Android (`adb reboot bootloader`) or the bootloader screen:
```bash
fastboot boot firmware/Asteroids_V3.1-250302-1856/img/boot.img
```
*Risk: none. Temporary boot of the exact stock kernel the phone already runs; nothing is written.* Expected: Android starts normally. If ABL answers "Fastboot boot command is not available", the unlock didn't take.

Also send me what this prints (unknown command on this bootloader, read-only):
```bash
fastboot oem dmesg 2>&1 | tee asteroids/backup/oem-dmesg.txt
```

## M1.2: Boot the probe shim

Build it (already built at `asteroids/shim/out/asteroids-probe-boot.img`, SHA-256 `72cc8b7f...ce358`):
```bash
asteroids/shim/build.sh
```
Boot it:
```bash
fastboot boot asteroids/shim/out/asteroids-probe-boot.img
```
*Risk: low. Nothing is written to storage, PMIC or fuses.* The probe:
- reads CPU registers and the device tree the bootloader hands over
- searches the initrd for its marker
- starts the 7 other cores through PSCI and turns them off again
- enables two interrupts on the GIC for about 0.6 s
- reads 4 UFS host registers

Then it stops with the text on screen. At worst the phone freezes on a teal or blue screen, which a forced restart clears.

What you should see: a **teal** screen with yellow labels and white values, ending in `DONE`. A **blue** screen means it hit an exception: that is a result too.

**Film or photograph the whole screen sharply**, then leave it:
- Force restart: hold **Power + Volume Down** until the screen goes off, and keep holding Volume Down to land in the bootloader. I haven't confirmed the 3a's exact combo: if nothing happens after 20 s, hold Power alone for 20 s.

Once it's back in fastboot, pull the probe's own text log. **Risk: none.** The probe writes every line into the ramoops zone that ABL prints:
```bash
fastboot oem dmesg 2>&1 | tee asteroids/backup/oem-dmesg-probe.txt
```
If RAM survived the forced restart, this shows the probe's lines instead of Android's log. Either answer is useful.

`oem dmesg` ends with "Memory dump complete." but ABL never sends the final OKAY, so `fastboot` keeps waiting. Press **Ctrl+C** once that line appears: the file is already complete, and the next fastboot command needs the USB connection free.

Send me: the photo or video, whether the screen stayed teal or turned blue, and tell me when the `oem dmesg` file is saved.

What I'm reading from it:

| Line | Question it answers |
| --- | --- |
| `EL/MIDR`, `MPIDR`, `MMFR0`, `PFR0` | Exception level ABL leaves us in under Gunyah, 16K page support, GIC system registers |
| `DTB`, `MEM0..7`, `SPLASH` | Where ABL put the device tree, the real RAM banks (we can't see them without it), the live framebuffer |
| `PANEL` | Visionox or BOE panel on this unit |
| `INITRD`, `MARKER` | Where our ramdisk lands behind the 8.4 MiB vendor ramdisk (the XNU loader must find its payload there). `not found` with an INITRD size well above 8.4 MiB means ABL used `init_boot`'s ramdisk instead of ours, so the XNU payload would have to ride inside the kernel image |
| `PSCI`, `CPU1..7` | Whether all 8 cores start, and which MPIDR is an A520 (`d80`) or A720 (`d81`) |
| `GICD`, `GICR`, `VTIMER27`, `SGI1` | Whether the new GIC addresses, the 0x40000 stride and timer INTID 27 work (expect `irqs=1E` for the timer, `A` for SGI) |
| `UFS` | Whether the UFS host is up and its version (sweet's driver expects the same register block) |
