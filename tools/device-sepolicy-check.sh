#!/system/bin/sh
# Проверяем, принимает ли KernelSU наши sepolicy-правила.
OUT=/data/local/tmp/sepolicy-check.txt
: > "$OUT"
say() { echo "$@" >> "$OUT"; }

say "===== ksud sepolicy check --help ====="
/data/adb/ksu/bin/ksud sepolicy check --help >> "$OUT" 2>&1

say ""
say "===== проверяем утверждения ====="
for stmt in \
    'allow zygote fuse:filesystem mount' \
    'allow zygote sdcardfs:filesystem mount' \
    'allow zygote fusefs_type:filesystem mount' \
    'allow zygote sdcard_type:filesystem mount' \
    'allow zygote fuse:dir mounton' \
    'allow zygote tmpfs:filesystem mount' \
    'allow zygote self:capability sys_admin' \
    ; do
    say "--- $stmt ---"
    /data/adb/ksu/bin/ksud sepolicy check "$stmt" >> "$OUT" 2>&1
    say "    rc=$?"
done

say ""
say "===== какие типы вообще известны ====="
for t in fuse sdcardfs fusefs_type sdcard_type tmpfs labeledfs; do
    say -n "  $t: "
    /data/adb/ksu/bin/ksud sepolicy check "allow zygote $t:filesystem mount" >> "$OUT" 2>&1
done

say ""
say "===== уже применённые sepolicy.rule модулей ====="
for f in /data/adb/modules/*/sepolicy.rule; do
    [ -f "$f" ] && { say "--- $f ---"; sed 's/^/    /' "$f" >> "$OUT"; }
done

say ""
say "===== лог KernelSU про sepolicy ====="
ls -la /data/adb/ksu/log/ 2>&1 | sed 's/^/    /' >> "$OUT"
for f in /data/adb/ksu/log/*.log /data/adb/ksu/log/*sepolicy* ; do
    [ -f "$f" ] && { say "--- $f ---"; tail -30 "$f" >> "$OUT" 2>&1; }
done

chmod 644 "$OUT"
echo "готово: $OUT ($(wc -c < "$OUT") байт)"
