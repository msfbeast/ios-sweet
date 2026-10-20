#!/usr/bin/env python3
"""Phone (3a) changes to the iOS root, on top of upstream tools/sweet-root-tweaks.py.

  sudo asteroids-root-tweaks.py MOUNTED_ROOT

Removes launchd jobs that only crash-loop on this hardware, the same way
upstream removes CommCenter. Measured on the 3a (2026-10-20, oem dmesg):
bluetoothd and fairplayd.H2 were relaunched 92 and 52 times in one boot
(each through xpcproxy), with no Bluetooth driver and no SEP to talk to.
The first run keeps the original as launchd.plist.asteroids-orig; the
script is idempotent.
"""
import os, plistlib, shutil, sys

REMOVE = {
    'com.apple.bluetoothd': 'no Bluetooth driver: exits, KeepAlive relaunches it',
    'com.apple.fairplayd.H2': 'no SEP: exits, KeepAlive relaunches it',
}


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    path = os.path.join(sys.argv[1], 'System/Library/xpc/launchd.plist')
    backup = path + '.asteroids-orig'
    st = os.stat(path)
    if not os.path.exists(backup):
        shutil.copy2(path, backup)
        os.chown(backup, st.st_uid, st.st_gid)
        print('backup', backup)
    with open(path, 'rb') as f:
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    for key in [k for k, j in jobs.items() if j.get('Label') in REMOVE]:
        label = jobs[key]['Label']
        del jobs[key]
        print(f'removed {label}: {REMOVE[label]}')
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {len(jobs)} jobs')


if __name__ == '__main__':
    main()
