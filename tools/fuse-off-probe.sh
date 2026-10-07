#!/system/bin/sh
# Hypothesis-C verification on the live device.
#
# !!! WARNING — READ THIS BEFORE RUNNING !!!
# On the device this script must run under `unshare -m`, AND `mount --make-rprivate /`
# must succeed BEFORE touching /mnt/user/0/emulated. On Android 16 the FIRST
# `unshare -m sh -c` from the su domain did not make the tree private in time, and
# the umount+bind leaked into the GLOBAL namespace: /dev/fuse left the mount table
# for the whole system until reboot. The system survived it (that is itself useful
# evidence — see docs/fuse-root-patch-design.md), but do not treat this script as
# side-effect-free. Prefer a disposable device or be ready to reboot.
#
# To leak-proof it, verify propagation right after unshare:
#     unshare -m sh -c 'mount --make-rprivate / && cat /proc/self/mountinfo | grep -c shared'
# and only proceed if the count is 0.
set -u

echo "=== 1. current state ==="
echo -n "/mnt/user/0/emulated : "; stat -f -c "%t %T" /mnt/user/0/emulated
echo -n "/data/media          : "; stat -f -c "%t %T" /data/media
echo -n "/mnt/pass_through/0/emulated : "; stat -f -c "%t %T" /mnt/pass_through/0/emulated

echo
echo "=== 2. sample app identity ==="
PID=$(pidof com.android.settings)
if [ -n "$PID" ]; then
    echo "settings pid=$PID"
    grep -E '^(Uid|Gid|Groups):' /proc/$PID/status
else
    echo "settings not running"
fi

echo
echo "=== 3. can a plain app read the raw tree today? ==="
# 10398 = the app uid sampled by aclprobe (typical 3rd-party app)
if command -v setpriv >/dev/null 2>&1; then
    setpriv --reuid=10398 --regid=10398 --clear-groups ls /data/media/0/ >/dev/null 2>&1 \
        && echo "raw /data/media/0 : readable" || echo "raw /data/media/0 : DENIED (expected without 9997)"
else
    echo "setpriv absent (toybox), skipping"
fi

echo
echo "=== 4. hypothesis C: substitute raw tree for FUSE in a private ns ==="
unshare -m sh -c '
    mount --make-rprivate / 2>/dev/null
    umount -l /mnt/user/0/emulated 2>/dev/null && echo "  umount FUSE point: ok"
    mount --bind /data/media /mnt/user/0/emulated 2>/dev/null && echo "  bind /data/media  : ok"
    echo -n "  after  : "; stat -f -c "%t %T" /mnt/user/0/emulated
    echo -n "  entries: "; ls /mnt/user/0/emulated/ | tr "\n" " "; echo
    echo -n "  0/     : "; ls /mnt/user/0/emulated/0/ 2>/dev/null | head -5 | tr "\n" " "; echo
'

echo
echo "=== 5. do ACLs (group 9997) still stand on the raw tree? ==="
echo -n "/data/media/0 mode: "; stat -c "%a %U:%G" /data/media/0
echo "xattr (toybox lacks getfattr; reading via python if present):"
if command -v python3 >/dev/null 2>&1; then
    python3 - <<'EOF'
import os
for p in ("/data/media/0", "/data/media"):
    for name in ("system.posix_acl_access", "system.posix_acl_default"):
        try:
            v = os.getxattr(p, name)
            # parse 4-byte version + 8-byte entries (tag,perm,id)
            import struct
            ver = struct.unpack_from("<I", v, 0)[0]
            n = (len(v) - 4) // 8
            out = []
            for i in range(n):
                tag, perm, eid = struct.unpack_from("<HHI", v, 4 + i * 8)
                names = {1:"USER_OBJ",2:"USER",4:"GROUP_OBJ",8:"GROUP",16:"MASK",32:"OTHER"}
                out.append(f"{names.get(tag,tag)}={perm:o}:{eid}")
            print(f"  {p} {name} v{ver}: {' '.join(out)}")
        except OSError as e:
            print(f"  {p} {name}: {e.strerror}")
EOF
else
    echo "  python3 not on device"
fi

echo
echo "=== 6. result ==="
echo "If (4) shows f2fs under /mnt/user/0/emulated and (5) shows GROUP=7:9997,"
echo "then patching MountUserFuse to bind the lower path instead of mounting"
echo "/dev/fuse is sufficient and the ACL path needs no libc hooks."
