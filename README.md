Restores pre-scoped storage behavior for internal storage: each application gets direct access to /storage/emulated, Android/data, and Android/obb, bypassing FUSE and MediaProvider in the I/O path. This drastically increases access speeds for a large number of small files in storage, which can be highly useful, for example, when running PC games via emulators or building large projects.

sdcardfs is a legacy storage access method that predates Android 10. A module relying on it uses fewer hooks and is potentially more reliable. Kernel support for sdcardfs is required (it was dropped from Android Linux kernels after version 4.19, meaning later versions require a support patch).

Benchmark results for various operations on 2000 4 KiB files. For large files, the difference is negligible.
<img width="925" height="512" alt="3" src="https://github.com/user-attachments/assets/8131bc39-cf71-4d1a-94b9-b52dabbb70a0" />
<img width="924" height="412" alt="1" src="https://github.com/user-attachments/assets/1219d3c5-3c83-4890-b28b-cb99f436eb16" />
<img width="934" height="415" alt="2" src="https://github.com/user-attachments/assets/52611c31-2ce3-4c28-a525-cadf16e77a04" />

## Two builds, one repository

The same goal is reached by two independent mechanisms, and this repository builds
both from one tree:

| build | archive | how |
| --- | --- | --- |
| `unfuse` | `out/unfuse-<version>.zip` | patches vold so that its FUSE mount for emulated storage becomes a bind of the raw `/data/media`, shapes the tree with ACLs, and patches bionic entry points inside each app |
| `unfuse-sdcardfs` | `out/unfuse-sdcardfs-<version>.zip` | mounts sdcardfs on `/mnt/runtime/*/emulated` and binds it into each app's private mount namespace |

They are alternatives, not companions: on Android 11 they set `persist.sys.fuse`
to opposite values, and both install under the same module id, so installing one
replaces the other. `unfuse-sdcardfs` needs a kernel with the `sdcardfs` driver;
`unfuse` does not.

```sh
./build.sh                  # both modules, arm64-v8a + armeabi-v7a
./build.sh unfuse           # one of them
./build.sh unfuse-sdcardfs
```

Both are built from one tree by `./build.sh`; what they share (the installer stub and
the shell primitives both modules source) lives in `module/common/`.
