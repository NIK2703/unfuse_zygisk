import sys

p = 'src/sdcardfs_restore.cpp'
s = open(p, encoding='utf-8').read()
marker_start = '// ---------------------------------------------------------------- проверки'
marker_end = '// ---------------------------------------------------------------- диагностика'
start = s.index(marker_start)
end = s.index(marker_end)
new = '// ---------------------------------------------------------------- диагностика\n'
s = s[:start] + new + s[end + len(marker_end):]
open(p, 'w', encoding='utf-8').write(s)
print('ok, removed', end - start, 'chars')
