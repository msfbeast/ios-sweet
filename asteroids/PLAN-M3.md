# M3 plan: iOS root on UFS

Goal: XNU on the Phone (3a) mounts the iPhone X iOS 16.7.16 root from UFS and starts launchd. M4 (SpringBoard on screen) follows from the same image.

## What M2 and the hardware already give us

- **Kernel:** reaches `Waiting on ... IOMedia ... Apple_HFS` with patches 0100 + 0101 (NOTES, 2026-10-20). No new XNU patch is expected for M3.
- **UFS host:** 0x1d84000, the same address as sweet. UFSHCI 3.0, left enabled by ABL, `CAP.64AS` set, so DMA buffers at 0x8_c0000000 are fine.
- **Partition:** `userdata` is `sda13` on **LUN 0** with **4096-byte blocks** (Android log + OrangeFox `by-name`). LUN 0 also holds `persist` (sda2), `super` (sda6) and `metadata` (sda11).
- **sweet's UFS driver** (`bsd/dev/sweet_ufs.c`):
  - LUN 0 only, GPT at LBA 1, 4 KiB blocks: matches the 3a.
  - Finds the partition by GPT name (`ufsroot=`, default `userdata`).
  - **Refuses every write unless `ufsrw=1`**, and checks each write against that partition's bounds.
  - With `ufsrw=1` it also keeps its 4 MiB persistent log in the last blocks of the same partition.

## Steps

### Step 1: UFS read-only smoke test (no download, nothing flashed)

Boot the existing kernel 2 with `rd=ufsroot0` and **no** `ufsrw`/`rootrw`. Expected `oem dmesg` lines:
- the driver's partition line (`userdata`, LUN 0, first block, size about 225 GiB);
- then a failed HFS+ mount, because `userdata` still holds Android's wiped f2fs.

This proves the 3a UFS read path before any write path is enabled. Risk: none on storage; the driver cannot write.

### Step 2: build the iOS root image on this Mac

`tools/mkiosroot.sh` is Linux-only (`apfs-fuse`, `mkfs.hfsplus`, `mount -t hfsplus`). A macOS version, `asteroids/tools/mkiosroot-mac.sh`, does the same with native tools:

1. Get `iPhone10,3,iPhone10,6_16.7.16_20H392_Restore.ipsw` from Apple's CDN (`updates.cdn-apple.com`, 5.75 GiB, still signed).
2. Read `BuildManifest.plist` for the OS, SystemOS and AppOS cryptex image names. Attach them read-only with `hdiutil` (macOS reads APFS natively).
3. Create a 16 GiB **case-sensitive journaled HFS+ raw image without a partition map**, so the volume starts at block 0 of `userdata`. Attach it with owners enabled and Spotlight off.
4. Copy as root with Homebrew `rsync -aH --numeric-ids`. The OS root goes to `/`, the cryptexes to `/private/preboot/Cryptexes/{OS,App}`. Then make the same SWEET additions: dyld cache hard links, cryptex-only files linked into the root, `_logd` log stores, the plain-plist system keybag.
5. Run upstream `tools/sweet-root-tweaks.py` on the mounted image (Setup skipped, activation seeded, watchdogs off, CommCenter removed).
6. Detach, then `fsck_hfs -fn`.
7. Extract `DeviceTree.d22ap.im4p` from the same IPSW into `research/ipsw-x/d22ap.adt`, so `mkadt.py` adds the iPhone X `/product` node that MobileGestalt needs.

Copying needs `sudo` (root-owned files), so you run that one script yourself with your password. Output: `asteroids/root/ios16-root.img`, git-ignored and never uploaded.

### Step 3: flash the root image to `userdata`

```
fastboot -S 512M flash userdata asteroids/root/ios16-root.img
```

This writes **only `userdata`** (Android's user-data partition, already wiped by the unlock). Bootloader, modem, persist, `super` and `boot` are untouched.

### Step 4: boot XNU from UFS

Kernel 2 plus the full ADT (with `/product`). The boot-args are upstream's working set without the sweet hardware drivers:

```
debug=0xc serial=3 cpus=1 keepsyms=1 -enable_kprintf_spam -noprogress
rd=ufsroot0 ufsrw=1 rootrw=1 unentitled_ios_sim_launch=1 ignition_level=1
launchd_unsecure_cache=1 wdt=-1 wdt_test=no_user_checkin
sweetkeys=0 sweettouch=0 sweetusb=0
```

- `cpus=1` stays until SMP is tested under Gunyah.
- No touch or keys yet (M5), so success means the lock screen drawn on the panel, not interaction.
- Sweet's first boot took 4 to 5 minutes on the Apple logo.

Read the log with `fastboot oem dmesg`. Once `ufsrw=1` is set, sweet's persistent ring at the end of `userdata` also works (readable from OrangeFox with upstream `tools/sweet-log-pull.sh`).

## What I need from you before anything is flashed

1. **OK to download the IPSW** (5.75 GiB from Apple's CDN) and to `brew install rsync`.
2. **You run the root-image build** (`sudo`, your password). It only touches files on this Mac.
3. **Confirm `userdata` as the iOS root partition.** After step 3, Android won't start: its data partition holds HFS+. Never let the phone boot Android normally while iOS is on `userdata`, because Android may reformat it. Always use `fastboot boot`, or Power + Vol- into fastboot. To go back to Android, run `fastboot -w` (or format data in OrangeFox) and reboot.
4. **Apple licence:** the image is built from Apple's IPSW for this personal experiment only. It stays on this Mac and the phone, and never goes into git or the GitHub fork.

Step 1 needs none of this and can run now.
