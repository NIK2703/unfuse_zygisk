#!/system/bin/sh
#
# test-storage-fix.sh — проверка, что нормализация на загрузке обнуляет
# «остальных», как это делает хук.
#
# До правки storage-fix сохранял бит «остальных» с диска, и файл 0664 получал
# ACL с OTHER=4, то есть доступ в обход записи для 9997.

SF=/data/local/tmp/storage-fix
T=/data/local/tmp/hookselftest
D=/data/media/0/.sfixtest

rm -rf "$D"
mkdir -p "$D/sub"

# Разные исходные режимы: важно, что у всех выставлены биты «остальных».
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
# uid 10998 состоит в 9997 — должен читать. А вот «остальные» (вне 9997) — нет.
"$T" readas 10998 "$D/f664"

rm -rf "$D"
echo "готово"
