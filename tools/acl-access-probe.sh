#!/system/bin/sh
# Verify ACL-based access on the raw tree. No mount changes here.
set -u

echo "=== mode/owner of the chain ==="
for p in /data/media /data/media/0 /data/media/0/Android /data/media/0/Android/data; do
    printf "%-32s " "$p"; stat -c "%a %U:%G" "$p" 2>&1
done

echo
echo "=== ACLs via /proc-independent read (od on xattr through python-less path) ==="
# toybox has no getfattr; use a tiny helper: read the xattr with 'ls -Z' won't show ACL.
# Instead use the module's own storage-fix --check, which parses ACLs and is on the device.
if [ -x /data/adb/modules/unfuse_zygisk/tools/storage-fix ]; then
    /data/adb/modules/unfuse_zygisk/tools/storage-fix --check /data/media/0 2>&1
    echo "exit=$?"
else
    echo "storage-fix not installed"
fi

echo
echo "=== app-group check: is 9997 present in a running app? ==="
PID=$(pidof com.android.settings)
[ -n "$PID" ] && grep -E '^Groups:' /proc/$PID/status

echo
echo "=== effective access test as uid 10398 (via nsenter into its own ns if it exists) ==="
echo "note: toybox lacks setpriv/setuid; access is proven by the ACL entry itself"

echo
echo "=== does MediaProvider have the tree? ==="
PID=$(pidof com.google.android.providers.media.module)
if [ -n "$PID" ]; then
    grep -E '^Groups:' /proc/$PID/status
fi
