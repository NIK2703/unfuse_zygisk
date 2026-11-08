Restores pre-scoped storage behavior for internal storage: each application gets direct access to /storage/emulated, Android/data, and Android/obb, bypassing FUSE and MediaProvider in the I/O path. This drastically increases access speeds for a large number of small files in storage, which can be highly useful, for example, when running PC games via emulators or building large projects.

The module ships a `sepolicy.rule` that lets app domains execute files and create symlinks or hardlinks on `media_rw_data_file`. Stock Android grants read and write there but not execute, symlink or hardlink. The installer asks whether to enable these rules: Volume Up enables them, Volume Down disables them. If no key is pressed within 30 seconds, they stay enabled. In the sdcardfs build, mount flags such as `noexec` may still apply.

ADB shell, `adb push` and `adb pull` keep access to `/storage/emulated/0`: the raw tree carries a `sdcard_rw` (gid 1015) ACL alongside the app-wide 9997 grant. Without it, adbd cannot traverse the raw `/data/media` root, which is owned by `media_rw` and mode 0550.

sdcardfs is a legacy storage access method that predates Android 10. A module relying on it uses fewer hooks and is potentially more reliable. Kernel support for sdcardfs is required (it was dropped from Android Linux kernels after version 4.19, meaning later versions require a support patch).

Benchmark results for various operations on 2000 4 KiB files. For large files, the difference is negligible.
<img width="925" height="512" alt="3" src="https://github.com/user-attachments/assets/8131bc39-cf71-4d1a-94b9-b52dabbb70a0" />
