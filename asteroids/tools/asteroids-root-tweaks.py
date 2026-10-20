#!/usr/bin/env python3
"""Phone (3a) changes to the iOS root, on top of upstream tools/sweet-root-tweaks.py.

  sudo asteroids-root-tweaks.py MOUNTED_ROOT

Stops launchd jobs that only crash-loop on this hardware from relaunching.
Measured on the 3a (2026-10-20, oem dmesg): bluetoothd and fairplayd.H2
were relaunched 92 and 52 times in one boot (each through xpcproxy), with
no Bluetooth driver and no SEP to talk to.

Removing the jobs (first version) hung the boot on the Apple logo:
SpringBoard's Control Center setup waited forever on the LaunchServices
database lock (LSContextInitCommon -> os_unfair_lock, sampled 240 s) while
the post-flash database rebuild depended on a removed service. So the jobs
stay, with KeepAlive and RunAtLoad off: they no longer start or restart on
their own, but a client message still launches them on demand.
The script always starts from launchd.plist.asteroids-orig (made on the
first run), so it is idempotent and undoes the removal.
"""
import os, plistlib, shutil, sys

ON_DEMAND = {
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
    with open(backup, 'rb') as f:		# always from the original
        plist = plistlib.load(f)
    jobs = plist['LaunchDaemons']
    for job in jobs.values():
        label = job.get('Label')
        if label in ON_DEMAND:
            job['KeepAlive'] = False
            job['RunAtLoad'] = False
            print(f'on demand only: {label} ({ON_DEMAND[label]})')
    with open(path, 'wb') as f:
        plistlib.dump(plist, f, fmt=plistlib.FMT_BINARY)
    os.chown(path, st.st_uid, st.st_gid)
    os.chmod(path, st.st_mode & 0o7777)
    print(f'launchd.plist: {len(jobs)} jobs')


if __name__ == '__main__':
    main()
