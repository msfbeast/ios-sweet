#!/bin/bash
# Apply asteroids-root-tweaks.py to the built iOS root image and make the
# sparse image for flashing. Only touches files on this Mac.
#
#   asteroids/tools/update-root.sh [IMG]      (default asteroids/root/ios16-root.img)
#
# Then: fastboot -S 512M flash userdata asteroids/root/ios16-root.simg
# (rewrites userdata from scratch: anything iOS wrote there is replaced).
set -euo pipefail
cd "$(dirname "$0")/../.."
img=${1:-asteroids/root/ios16-root.img}
[[ -f $img ]] || { echo "no $img" >&2; exit 1; }
sudo -v
dev=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$img" | awk 'NR==1{print $1}')
mnt=$(mktemp -d /tmp/iosroot.XXXXXX)
cleanup() {
	sudo umount "$mnt" 2>/dev/null || true
	hdiutil detach "$dev" >/dev/null 2>&1 || true
	rmdir "$mnt" 2>/dev/null || true
}
trap cleanup EXIT
sudo mount -t hfs -o nobrowse "$dev" "$mnt"
sudo mdutil -i off "$mnt" >/dev/null 2>&1 || true
sudo python3 -I asteroids/tools/asteroids-root-tweaks.py "$mnt"
sudo rm -rf "$mnt/.Spotlight-V100" "$mnt/.fseventsd" "$mnt/.Trashes" "$mnt/.TemporaryItems"
sudo umount "$mnt"
sudo fsck_hfs -fn "$dev"
hdiutil detach "$dev" >/dev/null
rm -f "${img%.img}.simg"
img2simg "$img" "${img%.img}.simg" 4096
ls -lh "${img%.img}.simg"
echo "done: ${img%.img}.simg"
echo "first boot after flashing: use a CPUS=1 image (8 cores hang in the post-flash LaunchServices rebuild), then the normal 8-core image"
