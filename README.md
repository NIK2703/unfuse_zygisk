# sdcardfs Restore

Zygisk-модуль, возвращающий внутренней памяти поведение Android 10 и более
ранних версий: **sdcardfs вместо FUSE**, полный доступ ко всей памяти для
**всех приложений без исключений** и без scoped storage.

Списка приложений нет и быть не должно — подмена выполняется для каждого
процесса, который запускает Zygote.

* Устройство проверки: POCO F5 (marble), Android 16, ядро с `CONFIG_SDCARD_FS`.
* Версия: **v2.5.0**

---

## 1. Что делает модуль

Ровно две вещи:

1. В `preAppSpecialize()` создаёт процессу приватный mount namespace и
   подкладывает под `/mnt/user/<user>/emulated` уже смонтированный sdcardfs.
   Дальше Zygote сам рекурсивно биндит `/mnt/user/<user>` на `/storage`,
   поэтому sdcardfs оказывается и на `/storage/emulated`.
2. Сбрасывает `*args->mount_storage_dirs` в `false`. Штатно Zygote накрывает
   `/storage/emulated/<user>/Android/{data,obb}` временным tmpfs и биндит туда
   каталоги **только своего пакета**; флаг отключает эту изоляцию, и
   `Android/data` с `Android/obb` отдаются целиком.

Всё остальное — вспомогательные скрипты, без которых эти две вещи не заработают
(см. §3).

## 2. Механика

```
preAppSpecialize(args)
  ├─ mount_external ∈ {DEFAULT, ANDROID_WRITABLE}?   иначе выход
  ├─ unshare(CLONE_NEWNS)
  ├─ mount(NULL, "/", MS_REC|MS_PRIVATE)
  ├─ bind /mnt/runtime/full/emulated -> /mnt/user/<user>/emulated
  │    └─ проверка: statfs() по цели должен дать 0x5dca2df5 (sdcardfs),
  │       иначе немедленный umount2(MNT_DETACH)
  ├─ (ANDROID_WRITABLE) то же для /mnt/androidwritable/<user>/emulated
  └─ *args->mount_storage_dirs = JNI_FALSE
```

Источник — `/mnt/runtime/full/emulated`: sdcardfs с маской `0007` и
`gid=9997` (`AID_EVERYBODY`). Драйвер считает права как `0775 & ~mask`, поэтому
каталоги получаются `0770`, файлы `0660`, а `AID_EVERYBODY` есть у любого
процесса (`ProcessList.computeGidsForProcess` всегда добавляет `userGid`). Итог —
полный доступ ко всей памяти, включая `Android/data`, `Android/obb` и каталоги
чужих пакетов. Ровно так же вёл себя маунт `full` на Android 9 и раньше.

Обрабатываются два режима внешнего хранилища:

| режим | кто | обрабатывается |
|---|---|---|
| `DEFAULT` (1) | все обычные приложения | да |
| `ANDROID_WRITABLE` (4) | приложения с `MANAGE_EXTERNAL_STORAGE` и провайдер SAF `com.android.externalstorage`, через который ходит системное приложение «Файлы» | да |
| `NONE` (0) | изолированные процессы — внешнего хранилища у них нет вовсе | нет |
| `INSTALLER` (2) | `installd` | нет |
| `PASS_THROUGH` (3) | MediaProvider — ему нужен сырой `/data/media` | нет |

Последним трём подмена сломала бы сам слой хранилища.

## 3. Почему нужны скрипты

Модуль умеет только подкладывать готовый sdcardfs. Поэтому к моменту старта
первого приложения уже должны существовать две вещи — их делает
`module/storage.sh`, который вызывают `post-fs-data.sh` (до Zygote) и
`service.sh` (повторно, после vold).

### 3.1. Маунты `/mnt/runtime/*/emulated`

На этой прошивке `external_storage.sdcardfs.enabled=0` (задано в
`/vendor/build.prop`), поэтому vold **не** выполняет `/system/bin/sdcard` и
`/mnt/runtime/*/emulated` остаются пустыми каталогами. Модулю нечего
подкладывать — маунты создаёт `storage.sh`.

**Почему не `/system/bin/sdcard`.** AOSP-овский `sdcard.cpp` создаёт реальным
`mount(2)` только `/mnt/runtime/default/emulated`, а `read`/`write`/`full` — через
`sdcardfs_setup_bind_remount()`, то есть bind + `MS_REMOUNT` с новыми
`mask=`/`gid=`. В этом ядре `sdcardfs_remount_fs()` опции не разбирает
(заглушка), а vfsopts живут в superblock, поэтому все четыре маунта получают
опции `default` (`gid=1015`, `mask=6`) и становятся бесполезны для приложений.
Каждый маунт создаётся отдельным `mount(2)` — тогда у каждого свой анонимный
superblock и свои vfsopts.

Опции повторяют `system/core/sdcard/sdcard.cpp` (`mask` печатается в десятичном
виде — там `StringPrintf("mask=%d", mask)` над octal-литералом):

| точка | `mask` | `gid` |
|---|---|---|
| `default` | `6` (oct 0006) | `1015` AID_SDCARD_RW |
| `read` | `23` (oct 0027) | `9997` AID_EVERYBODY |
| `write` | `7` (oct 0007) | `9997` |
| `full` | `7` (oct 0007) | `9997` |

Модуль берёт именно `full`.

### 3.2. Ярлык корня `/data/media`

sdcardfs своих данных не хранит: он пробрасывает операции к нижнему слою
(`/data/media`) и не заводит отдельного инода для корня точки монтирования —
`getattr()` корня форвардится в нижний инод. У этого корня ярлык особый:

```
private/file_contexts:633   /data/media     u:object_r:media_userdir_file:s0
private/file_contexts:634   /data/media/.*  u:object_r:media_rw_data_file:s0
private/domain.te:252       allow { coredomain appdomain } media_userdir_file:dir search;
```

для `media_userdir_file` разрешён ровно один `search`, без `getattr`:

```
stat /storage/emulated    -> Permission denied   (и без единого AVC: dontaudit)
stat /storage/emulated/0  -> ok
```

Содержимое работает и так: там ярлык `media_rw_data_file`, на который у
appdomain полные права (`app.te:149`). А вот сам корень — нет, и это ровно то
ограничение доступа, которого быть не должно.

`storage.sh` возвращает корню ярлык `media_rw_data_file` — тот же, что был у
`/data/media` на Android 10 и раньше (тогда отдельного типа
`media_userdir_file` просто не существовало). Права (DAC) при этом не меняются,
поэтому листать `/data/media` напрямую по-прежнему нельзя.

Патчить sepolicy не нужно: право уже есть, а `relabelto` для домена zygote
запрещён neverallow (`domain.te:791`) — поэтому перемаркировка делается
скриптом, а не из процесса приложения.

## 4. Требования

1. Ядро с `CONFIG_SDCARD_FS` — `grep sdcardfs /proc/filesystems`.
2. Zygisk: Magisk или ZygiskNext (KernelSU/APatch).
3. Android 11+ (проверено на Android 16).

## 5. Сборка и установка

```sh
./build.sh                          # arm64-v8a + armeabi-v7a, zip в out/
./build.sh arm64-v8a                # только один ABI
API=30 ./build.sh                   # другой API level (по умолчанию 26)
ZIP=0 ./build.sh                    # не паковать zip
```

```sh
adb push out/sdcardfs_restore-v2.5.0.zip /data/local/tmp/
adb shell su -c "ksud module install /data/local/tmp/sdcardfs_restore-v2.5.0.zip"
adb reboot
```

Отключить модуль: создать `/data/adb/modules/sdcardfs_restore/disable` и
перезагрузиться (штатный механизм загрузчика — отдельного `config` у модуля нет).

## 6. Проверка

```sh
# 1. маунты: все четыре должны быть sdcardfs с нужными mask/gid
grep mnt/runtime /proc/mounts

# 2. ярлык корня — обязательно media_rw_data_file
ls -Zd /data/media

# 3. что модуль сделал с приложением (одна строка на запуск)
logcat -d | grep SdcardFsRestore
#    I SdcardFsRestore: sdcardfs подключён: uid=10398 user=0

# 4. доступ от имени приложения, внутри ЕГО namespace
sh /data/local/tmp/probe-app.sh <pid>
```

Ожидаемый результат п.4:

```
/storage/emulated      mode=555 uid=0 gid=9997
/storage/emulated/0    mode=770 uid=0 gid=9997
statfs=0x5dca2df5                 <- sdcardfs, не FUSE (0x65735546)
ls /storage/emulated   -> 0  obb  <- корень листается
Android/data           -> 51 пакет
Android/obb            -> 2
запись в Download      -> WRITE_OK
чужой Android/data     -> виден
```

<p align="center">
  <img src="screenshots/4-documentsui-android-data.png" width="45%"
       alt="DocumentsUI: POCO F5 › Android › data со всеми пакетами">
</p>

Системное приложение «Файлы» показывает `Android/data` со всеми пакетами —
значит, провайдер `com.android.externalstorage` тоже получил sdcardfs.

## 7. Ограничения

* Обрабатываются только процессы, которые запускает Zygote. Процессы, читающие
  сырой `/data/media` (MediaProvider, `installd`), намеренно не трогаются.
* Поддержка только внутреннего хранилища; adoptable storage не обрабатывается.
* Многопользовательский режим поддержан (`user_id` берётся из `uid`), но
  проверялся только на user 0.

## 8. Структура

```
build.sh                    сборка .so и упаковка zip
src/sdcardfs_restore.cpp    модуль целиком (161 строка)
src/zygisk.hpp              интерфейс Zygisk
module/module.prop          метаданные
module/customize.sh         установщик: ABI, ядро, права
module/post-fs-data.sh      вызов storage.sh до старта Zygote
module/service.sh           вызов storage.sh после vold
module/storage.sh           ярлык /data/media + маунты /mnt/runtime/*/emulated
module/zygisk/*.so          собранный модуль
```

## 9. История версий

* **v2.5.0** — упрощение до прямой функциональности. Удалены: `config` со всеми
  директивами (`!enabled`, `!android_dirs`, `!verbose`, `!selftest`, `!relabel`,
  `!relax`), вся диагностика (`logIdentity`, `logPathLadder`, `runMountProbes`,
  `dumpSelinuxDiagnostics`, `selftest`), второй уровень маунта с подменой
  личности через `setfsuid`/`setfsgid`/`setgroups`, запасной прямой маунт
  sdcardfs и `relax-storage.sh`. Проверено на устройстве, что всё удалённое не
  требуется: модуль работает с полными capabilities (`CapEff=0x1ffffffffff`),
  поэтому права DAC ему не мешают, а доступ приложений к `/storage/emulated`
  одинаков при `/data/media` 0550 и 0551.
* **v2.4.0** — обработка `ANDROID_WRITABLE`: `Android/data` стал виден в
  системном приложении «Файлы».
* **v2.3.0** — доступ к корню `/storage/emulated` через перемаркировку
  `/data/media`.
