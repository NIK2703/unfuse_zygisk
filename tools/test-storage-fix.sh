#!/system/bin/sh
# test-storage-fix.sh — verifies boot-time normalization zeroes OTHER like the hook; before the fix a 0664 file kept OTHER=4 (bypass read for 9997).

SF=/data/local/tmp/storage-fix
T=/data/local/tmp/hookselftest
D=/data/media/0/.sfixtest

rm -rf "$D"
mkdir -p "$D/sub"

# Mixed source modes; all have OTHER bits set.
: > "$D/f664"; chmod 0664 "$D/f664"
: > "$D/f644"; chmod 0644 "$D/f644"
: > "$D/f600"; chmod 0600 "$D/f600"
: > "$D/sub/f755"; chmod 0755 "$D/sub/f755"
chmod 0755 "$D/sub"

echo "=== ДО: режимы и ACL ==="
ls -la "$D" "$D/sub" | grep -E 'f664|f644|f600|f755|sub'
"$T" acl "$D/f664" "$D/f644" "$D/f600" "$D/sub/f755"

echo
echo "=== прогон storage-fix ==="
"$SF" "$D"

echo
echo "=== ПОСЛЕ: режимы и ACL (ожидается OTHER=0 везде) ==="
ls -la "$D" "$D/sub" | grep -E 'f664|f644|f600|f755|sub'
"$T" acl "$D/f664" "$D/f644" "$D/f600" "$D/sub/f755" "$D"

echo
echo "=== проверка, что чужая группа НЕ получает доступ в обход ACL ==="
# uid 10998 is in 9997 (should read); outsiders (outside 9997) should not.
"$T" readas 10998 "$D/f664"

rm -rf "$D"
echo "готово"
