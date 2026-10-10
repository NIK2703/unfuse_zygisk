# Проверка sdcardfs-варианта модуля в ВМ redroid (A11) — 2026-10-09

Стенд: контейнер `redroid11` (`redroid/magisk:11`) **на телефоне** `marble`,
порт **5556** → 5555. Магisk — **Delta** `4dbd8358-delta(25206)`, API 30,
`zygisk=1`, `denylist=0`, списки `hidelist`/`sulist` пусты.

Проверялся **sdcardfs-вариант** (набор с `status.sh`, без `tools/`), установленный
в ВМ ранее: `md5 00cabb75…` arm64 / `ff25db2f…` armeabi-v7a, `disable` отсутствует.

## Итог одной строкой

Модуль в ВМ **работает**: sdcardfs поднят на всех четырёх точках `/mnt/runtime`,
приложения в своих namespace видят sdcardfs вместо FUSE, запись через
`/storage/emulated/0` ложится в `/data/media/0`. Ускорение против FUSE — **6–228×**.
Две оговорки ниже (§4, §5) — про этот стенд, а не про сам фикс.

## 1. Модуль загрузился

`/cache/magisk.log`:
```
I : unfuse_zygisk: exec [post-fs-data.sh]     16:56:16.933
I : unfuse_zygisk: exec [service.sh]          16:56:18.907
I : zygisk64: replaced com/android/internal/os/Zygote#nativeForkAndSpecialize
```
`/data/adb/unfuse_zygisk.log` (пишет `storage.sh`):
```
[2026-10-09 16:56:17] post-fs-data: OK   /mnt/runtime/default/emulated (mask=6 gid=1015)
[2026-10-09 16:56:17] post-fs-data: OK   /mnt/runtime/read/emulated (mask=23 gid=9997)
[2026-10-09 16:56:17] post-fs-data: OK   /mnt/runtime/write/emulated (mask=7 gid=9997)
[2026-10-09 16:56:17] post-fs-data: OK   /mnt/runtime/full/emulated (mask=7 gid=9997)
[2026-10-09 16:56:17] post-fs-data: основной путь готов и проверен
[2026-10-09 16:56:20] service: основной путь готов и проверен
```
Плечо отчиталось один раз за загрузку (так задумано, `claim_once`):
```
I UnfuseZygisk: sdcardfs подключён: uid=1002
```

## 2. sdcardfs поднят с проектными масками

`/proc/mounts` — все четыре точки `5dca2df5` (SDCARDFS_SUPER_MAGIC):

| точка | gid | mask |
|---|---|---|
| `/mnt/runtime/default/emulated` | 1015 | 6 |
| `/mnt/runtime/read/emulated` | 9997 | 23 |
| `/mnt/runtime/write/emulated` | 9997 | 7 |
| `/mnt/runtime/full/emulated` | 9997 | 7 |

Общие опции — `fsuid=1023,fsgid=1023,multiuser,derive_gid,default_normal,unshared_obb`
ровно как в `storage.sh`. До установки модуля эти же точки были пустыми
(`stat -f -c %t` → `1021994`).

## 3. Приложения видят sdcardfs, запись доходит до `/data/media`

Namespace'ы (`nsenter -t <pid> -m -- grep emulated /proc/self/mounts`), все дети
патченного зиготы pid 194:

| pid | процесс | `/mnt/user/0/emulated` | `/storage/emulated` |
|---|---|---|---|
| 724 | `android.ext.services` | **sdcardfs** | **sdcardfs** |
| 844 | `com.android.launcher3` | **sdcardfs** | **sdcardfs** |
| 963 | `com.android.inputmethod.latin` | **sdcardfs** | **sdcardfs** |
| 982 | `com.android.deskclock` | **sdcardfs** | **sdcardfs** |
| 447 | `com.android.systemui` | fuse | fuse |
| 1030 | `com.android.providers.media.module` | fuse | f2fs (сырой) |

Функциональная проверка (запись **из namespace** `launcher3`):
```
$ nsenter -t 844 -m -- sh -c 'echo vm-probe > /storage/emulated/0/unfuse-vm-probe.txt'
$ ls -la /storage/emulated/0/unfuse-vm-probe.txt   -> -rw-rw---- root everybody 9
$ ls -la /data/media/0/unfuse-vm-probe.txt         -> -rw-rw---- media_rw media_rw 9
$ cat /data/media/0/unfuse-vm-probe.txt            -> vm-probe
```
То есть путь приложения — это действительно `sdcardfs` поверх `/data/media`.

## 4. Оговорка 1: в этой ВМ не удерживается `persist.sys.fuse`

`post-fs-data.sh` на SDK 30 снимает `persist.sys.fuse` (модулю нужна форма без
FUSE). В ВМ после загрузки свойство всё равно **`true`**.

Причина: `/system/etc/init/hw/init.rc:791` в `on post-fs-data` делает
`setprop persist.sys.fuse true`, и в redroid скрипт модуля отрабатывает **раньше**
этой строки — init его перетирает. Сам `resetprop` при этом исправен: проверено
вручную, `resetprop -n persist.sys.fuse false` → `rc=0`, `getprop` → `false`.

Следствие: `/storage/emulated/0` в **корневом** namespace остаётся FUSE
(`65735546`) — Zygote биндит `/mnt/user/<u>`, а не `/mnt/runtime/<view>`.
Результат всё равно достигается, но **только плечом Zygisk** (оно подменяет
`.../emulated` уже в приватном namespace процесса). На реальном A11 (`mido`)
порядок может быть другим — там это надо перепроверить.

## 5. Оговорка 2: два системных процесса не подменяются

`com.android.systemui` (447) и `com.android.providers.media.module` (1030) остаются
на FUSE. Это **не отказ**: в `src/unfuse_zygisk.cpp:177` плечо выходит сразу, если
`mount_external` не `DEFAULT` и не `ANDROID_WRITABLE` —

```cpp
if (mode != kMountModeExternalDefault && !android_writable) return;
```

то есть эти процессы запущены с другим режимом монтирования. Для MediaProvider это
к тому же необходимо: именно он держит FUSE-демон и fd-контракт.

## 6. Симлинки — отвергаются в обеих формах

| namespace | результат `ln -s` |
|---|---|
| sdcardfs (`launcher3`) | `Operation not permitted` (EPERM) |
| fuse (`systemui`) | `Function not implemented` (ENOSYS) |

Разные `errno`, одинаковый итог — это штатное ограничение Android на эмулированное
хранилище, а не регрессия модуля.

## 7. Батарея файловых операций

200 файлов из namespace `launcher3` (sdcardfs, `5dca2df5`): создано 200,
прочитано 200, `rm -rf` — OK. То же в namespace `systemui` (fuse, `65735546`):
200/200, OK. Провалов нет ни в одной форме.

## 8. Бенчмарк: sdcardfs против FUSE в ВМ

`bench-arm64 <dir> 500 4096` (500 файлов по 4 КиБ), запуск через `nsenter` в
namespace процесса. Первое число — мс на всю фазу, второе — операций в секунду.

| операция | sdcardfs (launcher3) | fuse (systemui) | выигрыш |
|---|---|---|---|
| create | 25.01 мс / 19 993 | 277.54 мс / 1 802 | **11.1×** |
| stat | 6.23 / 80 193 | 37.69 / 13 266 | 6.0× |
| openclose | 3.06 / 163 482 | 292.88 / 1 707 | **95.7×** |
| read | 3.52 / 141 947 | 361.72 / 1 382 | **102.8×** |
| rewrite | 7.40 / 67 541 | 947.18 / 528 | **128.0×** |
| rename | 16.17 / 30 913 | 759.92 / 658 | **47.0×** |
| readdir | 0.10 / 5 212 768 | 21.84 / 22 893 | **218.4×** |
| mkdir | 18.59 / 26 897 | 266.42 / 1 877 | 14.3× |
| rmdir | 8.78 / 56 980 | 222.31 / 2 249 | 25.3× |
| unlink | 6.76 / 73 978 | 706.64 / 708 | **104.5×** |

Диапазон ускорения — **6–228×**, в среднем около 50×.

## 9. Особенности стенда, которые надо помнить

- **`su` в ВМ не пускает** («Permission denied») — у Magisk Delta это SuList.
  Корень берётся через `adb root` (сборка userdebug, `ro.debuggable=1`).
- В `magisk.log` есть ошибки 32-битного Zygisk:
  `execl failed with 2: No such file or directory`, `failed to extract zygisk-ld (254)`,
  `open /sbin/magisk32 failed`. 64-битное плечо при этом работает
  (`zygisk64: replaced … nativeForkAndSpecialize`). То есть **в этой ВМ 32-битные
  процессы модуль не получают** — артефакт Delta/redroid, не модуля.
- SELinux в ВМ выключен, контексты не привязываются
  (`SELinux: Context u:object_r:magisk_file:s0 is not valid (left unmapped)`) —
  `chcon` из `storage.sh` там no-op.
- `/proc/uptime` в контейнере — это uptime **телефона** (ядро общее), возраст
  контейнера по нему не определить.
- `date +%s%N` в toybox даёт корректное значение, но арифметика `$(( ))` в `mksh`
  на таких числах врёт (получались отрицательные дельты) — мерить надо
  `bench-arm64`, а не шелл-циклом.

## 10. Что осталось

- Вернуть в ВМ **no-sdcardfs**-вариант: бэкап прежнего состава лежит в
  `/data/local/tmp/mod-prev.tgz`.
- Проверить порядок «`post-fs-data.sh` модуля против `init.rc:791`» на реальном
  A11 (`mido`) — от этого зависит, работает ли фикс через свойство или только
  через плечо Zygisk.
