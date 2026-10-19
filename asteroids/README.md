# asteroids: ios-sweet on the Nothing Phone (3a)

Experimental port of [ios-sweet](https://github.com/dipa-studio/ios-sweet) (XNU + the iPhone X iOS 16 userland) from the Redmi Note 10 Pro to the Nothing Phone (3a) (`Asteroids`, SM7635). Nothing boots yet: we're at M1, the first probe boot.

| File | What |
| --- | --- |
| `HARDWARE_MAP.md` | sweet vs Phone (3a): every address and part the port depends on, with sources |
| `RUNBOOK-M0-M1.md` | Unlock, backup and first boots, with the risk of each command |
| `NOTES.md` | Engineering log: every boot, crash and decision |
| `shim/` | M1 probe shim: boot image v4, reports CPU, FDT, SMP, GIC and UFS facts on screen |
| `tools/extract_dtbs.py` | Split `vendor_boot` / `dtbo` images into device trees |
| `tools/backup-partitions.sh` | Pull and hash-check partition images from OrangeFox |
| `tools/aosp/` | AOSP `mkbootimg.py` / `unpack_bootimg.py` (Apache 2.0) |

The repo root (`shim/`, `tools/`, `xnu/`) is upstream sweet, kept unchanged as the reference.

Running iOS on non-Apple hardware breaks Apple's licence: this is a personal experiment. No Apple files belong in this repository.
