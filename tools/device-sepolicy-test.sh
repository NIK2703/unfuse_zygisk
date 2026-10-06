#!/system/bin/sh
echo "=== check: allow appdomain ==="
ksud sepolicy check "allow appdomain media_userdir_file:dir { getattr read open search }" 2>&1
echo "rc=$?"
echo
echo "=== check: allow coredomain ==="
ksud sepolicy check "allow coredomain media_userdir_file:dir { getattr read open search }" 2>&1
echo "rc=$?"
echo
echo "=== check: typeattribute ==="
ksud sepolicy check "typeattribute media_userdir_file mlstrustedobject" 2>&1
echo "rc=$?"
echo
echo "=== apply sepolicy.rule ==="
ksud sepolicy apply /data/adb/modules/sdcardfs_restore/sepolicy.rule 2>&1
echo "rc=$?"
echo
echo "=== готово ==="
