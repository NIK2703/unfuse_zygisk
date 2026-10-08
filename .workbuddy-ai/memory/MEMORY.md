# unfuse_zygisk

Zygisk-модуль: доступ приложений к `/data/media` без FUSE и scoped storage. `github.com/NIK2703/unfuse_zygisk`.
Работать только в `E:\projects\unfuse_zygisk` (ветка `fuse-only`); `H:\projects\unfuse_zygisk` заброшен.
Разборы — `docs/fuse-root-patch-design.md`; устройство — скилл `zygisk-module-device-regression-sweep`.

## Конвенции
- Комментарии/коммиты по-русски, **почему** со ссылками на AOSP; в `tools/` — английская проза.
- `./build.sh`, NDK `29.0.14206865`, **детерминированная** (неизменный md5 при изменённом исходнике = правка не доехала). ABI: `arm64-v8a`, `armeabi-v7a`.
- Бинарники с устройства → `device/` (`.gitignore`). Файлы не удалять без согласия.
- На живое устройство — **только атомарно** (временный файл + `mv -f`; `cp -f` поверх `zygisk/*.so` усекает inode → SIGBUS в zygote). Новая сборка **не активна до перезагрузки** (zn-daemon держит старый fd).
- **Модуль не выдаёт ничего**: ни logcat, ни журнала, ни stdout — отчёт только кодом возврата (`usage()` и отчёты режимов `--emit`/`--selftest` — исключение). Логгирование **не заменять на `printf`**, а удалять целиком (указание Nikita). Дев-харнессы (`hookselftest.cpp`, `roomtest.c`, `final-verify.sh`, `device-e2e.sh`, `test-storage-fix.sh`) свой вывод сохраняют — он и есть результат. `-llog` больше не нужен нигде.

## Артефакты
`module/zygisk/<abi>.so` + `storage-fix` (ACL 9997), `vold-noacl` (`setxattr`→no-op), `vold-fusefs` (PLT `mount`+`umount2`). `hook_libc.cpp` — режимы как у sdcardfs + ACL (12 целей), **не избыточен**. `unfuse_zygisk.cpp` — снимает изоляцию `Android/data`/`obb`.

## Ядро дизайна
- FUSE-маунт **обязан** жить: fd — контракт с MediaProvider; `active` ставит только `pf_init` (ядро, на живом маунте). Убрать FUSE целиком нельзя (том `unmountable`).
- Маунт на `/mnt/user/<u>/emulated` приходит в namespace приложения сам (слейв init); `MS_REC|MS_PRIVATE` в namespace приложения рвёт связь.
- Путь распознаётся **по форме** (цель кончается на `/emulated` + `MS_LAZYTIME`), не по значению.
- `vold.rc` → `reboot_on_failure`: **SIGSEGV в vold = bootloop**.
- `libfuse` в vold **нет** (сырой syscall `mount("/dev/fuse",…)`, `Utils.cpp:1676`), входит только через `libfuse_jni`; не грузить нельзя (`isFuseThread()`, native, ~20 мест). §9 док.

## Два варианта `vold-fusefs` (оба обработчика обязательны)
- **STACK** = `fuse-only` @ `5c743f3`: FUSE + bind `/data/media` **сверху** на одном `fuse_path`; `umount2` снимает до 4 слоёв.
- **RELOCATE** = ветка `relocate` @ `ecb48eb`: FUSE уходит на `SCRATCH = fuse_path + ".fuse"`, `fuse_path` — **один** слой bind. Путь читается **побайтово** (8-байтовое чтение чужого `std::string` → SIGSEGV → bootloop). §9.4: три прочих `.fuse` — копии рекурсивного bind'а, фантом убирается выносом SCRATCH из `/mnt/user/<u>/`. §9.5/§9.6: bdi и `InitializeDeviceId` уезжают в f2fs, а не в FUSE.

## Проверка (минимум)
1. `--check` → оба патча стоят, rc=0.
2. Цикл `sm unmount`/`sm mount`: маунтов 0 → 2 (STACK) / 0 → 1 (RELOCATE); fstype только после монта.
3. Четыре дерева: `/mnt/user`,`/mnt/installer`,`/mnt/androidwritable` (f2fs сверху), `/mnt/pass_through` (n=1).
4. logcat без ENOTCONN / `Failed to mount emulated fuse`.
5. **Каждое** приложение: `/proc/<pid>/root/storage/emulated/0` → f2fs **и** `dev:ino` == `/data/media/0`. Строка `fuse` в `/proc/<pid>/mounts` — норма (нижний слой).
6. Активность сборки: inode на диске == `ino=` строки `unfuse_zygisk 64` в `/data/adb/zygisksu/modules_info`.

## Грабли устройства
- `adb shell "su 0 sh /data/local/tmp/x.sh"` (adb рвёт по `;`); демон adb умирает между вызовами — переподключаться.
- `nsenter` запрещён (KernelSU): путь читать резолвингом `/proc/<pid>/root/…`.
- toybox `stat` не разыменовывает симлинк, `chown` — разыменовывает (`chown … /sdcard` меняет `/data/media/0`).
- f2fs: `stat -f -c %t` → `0xf2f52010` (`%T` = UNKNOWN); fuse → `0x65735546`.
- IP устройства меняется после перезагрузки (сообщает пользователь).
