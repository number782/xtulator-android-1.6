XTulator Android Assets Directory
==================================

This directory is for storing BIOS files and disk images that the emulator needs.

Required files (place here or they will be copied from external storage on first run):
- bios.bin     - PC BIOS ROM (e.g., from Bochs or similar)
- disk.img     - Hard disk image (FAT12/16 formatted)

File locations at runtime:
- Internal storage: /data/data/com.xtulator.android/files/
- External storage: /sdcard/xtulator/

The emulator will look for files in this order:
1. Internal app files directory
2. External storage /sdcard/xtulator/
3. Assets directory (this folder)

Note: For Android 1.6, external storage path may be /sdcard/ or /mnt/sdcard/