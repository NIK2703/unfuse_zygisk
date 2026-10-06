#!/bin/bash
cd /tmp || exit 1
urls=(
  "https://android.googlesource.com/kernel/common/+/refs/heads/android11-5.4/fs/sdcardfs/inode.c?format=TEXT"
  "https://android.googlesource.com/kernel/common/+/refs/heads/android11-5.4/fs/sdcardfs/super.c?format=TEXT"
)
for u in "${urls[@]}"; do
  echo "=== $u"
  timeout 45 curl -sL "$u" -o t.b64
  sz=$(wc -c < t.b64)
  echo "size=$sz"
  if [ "$sz" -gt 5000 ]; then
    out=$(basename "${u%%\?*}")
    base64 -d t.b64 > "$out" 2>/dev/null || cp t.b64 "$out"
    wc -l "$out"
  else
    head -c 120 t.b64; echo
  fi
done
ls -la /tmp/inode.c /tmp/super.c 2>/dev/null
