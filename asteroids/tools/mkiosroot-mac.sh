#!/bin/bash
# macOS port of tools/mkiosroot.sh: build the SWEET iOS root image from an
# iPhone X IPSW, then apply tools/sweet-root-tweaks.py.
#
#   asteroids/tools/mkiosroot-mac.sh IPSW OUT.img [SIZE]     (SIZE default 16G)
#
# OUT is a raw case-sensitive journaled HFS+ volume with no partition map,
# for `fastboot flash userdata OUT` (rd=ufsroot0 mounts the partition itself).
# Same layout as upstream: OS root at /, SystemOS and AppOS cryptexes in
# /private/preboot/Cryptexes/{OS,App}, dyld cache and cryptex-only files hard
# linked into the root, _logd stores, plain-plist system keybag.
#
# Differences from the Linux script: macOS reads the IPSW's APFS images
# natively (hdiutil, transparent decompression) instead of apfs-fuse, and
# newfs_hfs/mount_hfs replace hfsprogs, so no name normalisation fix-ups.
# Uses sudo for the copy (root-owned files); run it from a terminal.
# The output is Apple's software: keep it on this Mac and the phone.
set -euo pipefail

[[ $# -ge 2 ]] || { sed -n '2,16p' "$0"; exit 2; }
ipsw=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
out=$2 size=${3:-16G}
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
RSYNC=/opt/homebrew/bin/rsync
[[ -x $RSYNC ]] || { echo "needs Homebrew rsync 3 (brew install rsync)" >&2; exit 1; }
[[ ! -e $out ]] || { echo "refusing to overwrite $out" >&2; exit 1; }

sudo -v
work=$(mktemp -d /tmp/mkiosroot.XXXXXX)
devs=()
mnts=()
cleanup() {
	for m in "${mnts[@]}"; do sudo umount "$m" 2>/dev/null || true; done
	for d in "${devs[@]}"; do hdiutil detach -force "$d" >/dev/null 2>&1 || true; done
	rm -rf "$work"/*.dmg "$work"/extract
	rmdir "$work"/* "$work" 2>/dev/null || echo "left $work in place" >&2
}
trap cleanup EXIT

# 1. Image names from BuildManifest.plist (the iPhone10,3 / d22ap erase identity).
read -r os sysos appos < <(unzip -p "$ipsw" BuildManifest.plist | python3 -I -c '
import plistlib, sys
m = plistlib.load(sys.stdin.buffer)
for b in m["BuildIdentities"]:
    info = b["Info"]
    if info.get("DeviceClass") == "d22ap" and info.get("RestoreBehavior") == "Erase":
        bm = b["Manifest"]
        print(bm["OS"]["Info"]["Path"], bm["Cryptex1,SystemOS"]["Info"]["Path"], bm["Cryptex1,AppOS"]["Info"]["Path"])
        break
else:
    sys.exit("no d22ap Erase identity in BuildManifest.plist")
')
echo "OS=$os SystemOS=$sysos AppOS=$appos"
mkdir "$work/extract"
unzip -q -o "$ipsw" "$os" "$sysos" "$appos" -d "$work/extract"

# 2. Attach them read-only; the cryptexes are raw APFS.
attach_ro() {
	local plist
	plist=$(hdiutil attach -readonly -nobrowse -noverify -owners on -plist "$@")
	python3 -I -c '
import plistlib, sys
ents = plistlib.loads(sys.stdin.buffer.read())["system-entities"]
mp = [e["mount-point"] for e in ents if "mount-point" in e]
print(mp[0])' <<<"$plist"
}
# (detached by mount point: hdiutil then ejects the whole image)
osmnt=$(attach_ro "$work/extract/$os")
devs+=("$osmnt")
sysmnt=$(attach_ro -imagekey diskimage-class=CRawDiskImage "$work/extract/$sysos")
devs+=("$sysmnt")
appmnt=$(attach_ro -imagekey diskimage-class=CRawDiskImage "$work/extract/$appos")
devs+=("$appmnt")
echo "mounted: $osmnt $sysmnt $appmnt"

# 3. The raw HFS+ volume (sparse file; no partition map).
mkfile -n "$size" "$out"
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$out" | awk 'NR==1{print $1}')
devs=("$dev" "${devs[@]}")
newfs_hfs -s -J -v iOSRoot "$dev"
root="$work/root"
mkdir "$root"
sudo mount -t hfs -o nobrowse "$dev" "$root"   # as root: owners honoured
mnts=("$root")
sudo mdutil -i off "$root" >/dev/null 2>&1 || true

copy() {
	echo "copying $1 -> ${2#"$root"}"
	sudo mkdir -p "$2"
	sudo "$RSYNC" -aH --numeric-ids "$1/" "$2/"
}
copy "$osmnt" "$root"
copy "$sysmnt" "$root/private/preboot/Cryptexes/OS"
copy "$appmnt" "$root/private/preboot/Cryptexes/App"

# 4. Shared cache where dyld looks at ignition level 1: hard links.
dsc=private/preboot/Cryptexes/OS/System/Library/Caches/com.apple.dyld
sudo mkdir -p "$root/System/Library/Caches/com.apple.dyld"
for f in "$root/$dsc"/*; do
	sudo ln "$f" "$root/System/Library/Caches/com.apple.dyld/${f##*/}"
done

# Cryptex files with no counterpart in the OS root, linked to their root path.
for c in OS App; do
	(cd "$root/private/preboot/Cryptexes/$c" && sudo find . ! -type d ! -path './System/Library/Caches/com.apple.dyld/*') |
	while IFS= read -r f; do
		f=${f#./}
		if ! sudo test -e "$root/$f" && ! sudo test -L "$root/$f"; then
			sudo mkdir -p "$root/$(dirname "$f")"
			src="$root/private/preboot/Cryptexes/$c/$f"
			if sudo test -L "$src"; then sudo cp -P "$src" "$root/$f"; else sudo ln "$src" "$root/$f"; fi
			echo "linked $c:/$f"
		fi
	done
done

# 5. logd stores (owner _logd 272) and the plain-plist system keybag.
db="$root/private/var/db"
sudo install -d -o 272 -g 272 -m 0751 "$db/diagnostics"
sudo install -d -o 272 -g 272 -m 0755 "$db/uuidtext" "$db/diagnostics/timesync"
for d in diagnostics uuidtext diagnostics/timesync; do
	sudo touch "$db/$d/.migrated"
	sudo chown 272:272 "$db/$d/.migrated"
	sudo chmod 0644 "$db/$d/.migrated"
done
sudo install -d -m 0700 -o 0 -g 0 "$root/private/var/keybags"
printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' \
	'<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' \
	'<plist version="1.0"><dict><key>KeyBagKeys</key><data>U1dFRVQ=</data></dict></plist>' |
	sudo tee "$root/private/var/keybags/systembag.kb" >/dev/null
sudo chown 0:0 "$root/private/var/keybags/systembag.kb"
sudo chmod 0600 "$root/private/var/keybags/systembag.kb"

# 6. Bring-up tweaks (Setup skipped, activation, watchdogs, CommCenter...).
sudo python3 -I "$repo/tools/sweet-root-tweaks.py" "$root"

# macOS bookkeeping must not end up in the iOS root.
sudo rm -rf "$root/.Spotlight-V100" "$root/.fseventsd" "$root/.Trashes" "$root/.TemporaryItems"
df -h "$root"
sudo umount "$root"
mnts=()
sudo fsck_hfs -fn "$dev"
hdiutil detach "$dev" >/dev/null
devs=("${devs[@]:1}")
ls -l "$out"
echo "done: $out"
