#!/usr/bin/env python3
"""Показывает содержимое собранного zip с правами."""
import sys
import zipfile

path = sys.argv[1] if len(sys.argv) > 1 else "out/unfuse_zygisk-v1.1.0.zip"
z = zipfile.ZipFile(path)
print("%-34s %9s  %s" % ("файл", "размер", "режим"))
for i in sorted(z.infolist(), key=lambda x: x.filename):
    mode = (i.external_attr >> 16) & 0o777
    print("%-34s %9d  %s" % (i.filename, i.file_size, oct(mode)))
print()
print("всего записей:", len(z.infolist()))
