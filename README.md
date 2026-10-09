Restores pre-scoped storage behavior for internal storage: each application gets direct access to /storage/emulated, Android/data, and Android/obb, bypassing FUSE and MediaProvider in the I/O path. This drastically increases access speeds for a large number of small files in storage, which can be highly useful, for example, when running PC games via emulators or building large projects.

Benchmark results for various operations on 2000 4 KiB files. For large files, the difference is negligible.
<img width="925" height="512" alt="3" src="https://github.com/user-attachments/assets/8131bc39-cf71-4d1a-94b9-b52dabbb70a0" />
<img width="924" height="412" alt="1" src="https://github.com/user-attachments/assets/1219d3c5-3c83-4890-b28b-cb99f436eb16" />
<img width="934" height="415" alt="2" src="https://github.com/user-attachments/assets/52611c31-2ce3-4c28-a525-cadf16e77a04" />
