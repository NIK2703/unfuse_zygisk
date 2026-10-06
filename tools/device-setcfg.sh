#!/system/bin/sh
C=/data/adb/modules/sdcardfs_restore/config
[ -f "$C" ] || { echo "НЕТ $C"; exit 1; }
grep -q '^!enabled='      "$C" || echo '!enabled=1'      >> "$C"
grep -q '^!android_dirs=' "$C" || echo '!android_dirs=raw' >> "$C"
grep -q '^!verbose='      "$C" || echo '!verbose=1'      >> "$C"
grep -q '^!relax='        "$C" || echo '!relax=1'        >> "$C"
echo "--- активные строки config ---"
grep '^!' "$C"
