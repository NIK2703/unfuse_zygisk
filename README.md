Restores pre-scoped storage behavior for internal storage: each application gets direct access to /storage/emulated, Android/data, and Android/obb, bypassing FUSE and MediaProvider in the I/O path. This drastically increases access speeds for a large number of small files in storage, which can be highly useful, for example, when running PC games via emulators or building large projects.

sdcardfs is a legacy storage access method that predates Android 10. A module relying on it uses fewer hooks and is potentially more reliable. Kernel support for sdcardfs is required (it was dropped from Android Linux kernels after version 4.19, meaning later versions require a support patch).

Benchmark results for various operations on 2000 4 KiB files. For large files, the difference is negligible.
<img width="925" height="512" alt="3" src="https://github.com/user-attachments/assets/8131bc39-cf71-4d1a-94b9-b52dabbb70a0" />
