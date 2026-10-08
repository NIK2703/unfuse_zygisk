# unfuse_zygisk

Zygisk-модуль: доступ приложений к `/data/media` без FUSE и scoped storage. `github.com/NIK2703/unfuse_zygisk`.
Работать только в `E:\projects\unfuse_zygisk` (ветка `fuse-only`); `H:\projects\unfuse_zygisk` заброшен.
Разборы — `docs/fuse-root-patch-design.md`; устройство — скилл `zygisk-module-device-regression-sweep`.

## Конвенции
- Комментарии/коммиты по-русски, **почему** со ссылками на AOSP; в `tools/` — английская проза.
- `./build.sh`, NDK `29.0.14206865`, **детерминированная** (неизменный md5 при изменённом исходнике = правка не доехала). ABI: `arm64-v8a`, `armeabi-v7a`.
- Бинарники с устройства → `device/` (`.gitignore`). Файлы не удалять без согласия.
- Эталонные образы **сверены с устройством** (2026-10-08, md5): `device/libc/libc-arm64.so` = `/apex/com.android.runtime/lib64/bionic/libc.so`, `E:\projects\12\bin\vold` = `/system/bin/vold`. Оба — **Android 16 (sdk 36, marble)**: имя без суффикса версии = 16, у libc и у vold одинаково. `aosp-ref/` (нужен `check-mount-modes.py`) в дереве **нет**, есть однорелизный `E:\projects\aosp` — этот чек сейчас не запускается.
- На живое устройство — **только атомарно** (временный файл + `mv -f`; `cp -f` поверх `zygisk/*.so` усекает inode → SIGBUS в zygote). Новая сборка **не активна до перезагрузки** (zn-daemon держит старый fd).
- **Модуль не выдаёт ничего**: ни logcat, ни журнала, ни stdout — отчёт только кодом возврата (`usage()` и отчёты режимов `--emit`/`--selftest` — исключение). Логгирование **не заменять на `printf`**, а удалять целиком (указание Nikita). Дев-харнессы (`hookselftest.cpp`, `roomtest.c`, `final-verify.sh`, `device-e2e.sh`, `test-storage-fix.sh`) свой вывод сохраняют — он и есть результат. `-llog` больше не нужен нигде.
- Наблюдение (2026-10-08): в `tools/` проза на самом деле **смешанная** — русская в `vold-*.c/.h` и в `.sh`-скриптах устройства, английская в отдельных пробах (`hookselftest.cpp`, `roomtest.c`, `storage-fix.c`, `*-probe.cpp`, `elf-plt-map.py`). Новые файлы этого захода написаны по-русски, как соседние `vold-*`. **Спрошено у Nikita** — правило выше и практика расходятся.

## Артефакты
`module/zygisk/<abi>.so` + `storage-fix` (ACL 9997), `vold-noacl` (`setxattr`→no-op), `vold-fusefs` (PLT `mount`+`umount2`). `hook_libc.cpp` — режимы как у sdcardfs + ACL (12 целей), **не избыточен**. `unfuse_zygisk.cpp` — снимает изоляцию `Android/data`/`obb`.

## Ядро дизайна
- FUSE-маунт **обязан** жить: fd — контракт с MediaProvider; `active` ставит только `pf_init` (ядро, на живом маунте). Убрать FUSE целиком нельзя (том `unmountable`).
- Маунт на `/mnt/user/<u>/emulated` приходит в namespace приложения сам (слейв init); `MS_REC|MS_PRIVATE` в namespace приложения рвёт связь.
- Путь распознаётся **по форме** (цель кончается на `/emulated` + `MS_LAZYTIME`), не по значению.
- Площадку вызова ищет `find_fuse_site()` в `tools/vold-fusefs.c`, и **не по смежности**: `adrp`+`add` компилятор ставит то подряд (14–17), то **вразбивку** (`adrp x21; adrp x22; add x21; add x22` — 11/12/12L/13). Требование смежности стоило патча FUSE на четырёх релизах молча (rc=2, модуль не работал). Сейчас регистр помнит последний `add`/`ldr`, `adrp` закрывает его — **одна функция на все версии, ветвлений по релизам нет**. `MS_LAZYTIME`-площадка ровно одна во всех 8 образах. Регрессия ловится `anchor_selftest()` (обе формы пролога синтетически) — коммит `3a83678`.
- Применимость по образам: libc — `verify-hook-targets.py` (8 образов arm64: ok=22/0/0), vold — `tools/vold-targets.sh` (8 образов, оба патчера, `--dry-run --file`).
- **Отказ называет причину, а не «2».** `vold-fusefs`: `5..10` — якорь, `11..17` — где обработчик не смог получить дом (`vold_exec_page`, ptrace), `18` — площадок-кандидатов больше списка, `19` — ошибка аргументов. `word()` в `vold-targets.sh` печатает их; ни один скрипт модуля кодов не различает. `ep_exit_code()` — **без `default:`** (новый шаг обязан ронять компиляцию), и `EP_OK` там = **не** успех, а `17`: функция зовётся только при нулевом адресе, значит путь отказа забыл причину — это её баг.
- **Места в образе нет ни на одном релизе** (`--room` → 1 везде): `find_handler_room()` никогда не срабатывает, `vold_exec_page()` — **единственный** путь. Поэтому он же единственный, кого может отвергнуть политика релиза, и поэтому же он не проверяется на уже пропатченном vold (`hook_stub_intact()` отсекает раньше).
- **`scan_back()`: связывание — «ближайшее к вызову».** Скан идёт назад, поэтому первое найденное связывание и есть живое значение регистра; у `src_reg`/`type_reg`/`x0_from`/`x2_from` берётся **первое найденное** (`out->… < 0`). Перезапись оставляла самое старое → `x0_from == src_reg` не сходится на правильной площадке → `-4`/rc 8 → патч FUSE не встаёт молча (та же форма, что регрессия 11–13). `w3` — исключение: обязан **накапливать** половины (`mov w3,#lo` + `movk w3,#hi,lsl#16`; высокое слово видно первым). Окно — 32 слова назад, закрывается условным переходом; `bl` окно не рвёт. Тест — случай 3 в `anchor_selftest()` (поле связано дважды), проверен отрицательным контролем.
- **`find_fuse_site()`: считать только сохранённые площадки.** Было `nfound++` вне `if (nfound < 8)` → 9-я площадка заставляла фильтр по LAZYTIME читать за массивом; мусор со стека → чужой `src_va` → `mount(2)` падает в рантайме молча. Теперь переполнение → `too_many` → отказ `18` **до** фильтра. Доказано A/B при `MAXSITE=1`: старый rc=0 (ложный успех) на всех 8 образах, новый rc=18.
- **armeabi-v7a не реализован, хотя собирается и упаковывается**: `patch_entry()` в `hook_libc.cpp` под `#else` → `false` (хук — пустышка), `vold-common.h` разбирает только ELF64 → vold-патчеры отказывают (rc=2, до любой записи, безопасно). `verify-hook-targets.py` теперь даёт rc=1 на не-AArch64 (раньше — ложный 0). Решение по ABI — за Nikita.
- `vold.rc` → `reboot_on_failure`: **SIGSEGV в vold = bootloop**.
- `libfuse` в vold **нет** (сырой syscall `mount("/dev/fuse",…)`, `Utils.cpp:1676`), входит только через `libfuse_jni`; не грузить нельзя (`isFuseThread()`, native, ~20 мест). §9 док.

## Имя vold и разбор vold (что нельзя забывать)
- vold запускается **не голым**: `/system/bin/vold --blkid_context=u:r:blkid:s0 --blkid_untrusted_context=… --fsck_context=… --fsck_untrusted_context=…` (снято `tools/proc-names-dump.sh`, Android 16). `/proc/<pid>/comm` у него `binder:<pid>_<n>` (главный поток ушёл в binder) — **по comm vold не опознать**. Опознание: `/proc/<pid>/exe` (под root всегда работает) → имя целиком; резерв `/proc/<pid>/cmdline` (мир-читаемый, для не-root) → NUL→пробел, **обрезать по первому пробелу**, потом имя целиком. Сравнение префикса в 4 байта принимает `vold_prepare_subdirs` — соседа, которого vold сам exec-ает (бинарь на устройстве есть).
- Правило вынесено в `tools/proc-name.h` (`base_name`, `cmdline_is_vold`), контейнер трамплинов — в `tools/stubs.h` (`Stubs` + `stubs_free/add/add_patched/lookup/is_patched`). Оба выноса **тождественны**: md5 всех 8 артефактов до и после совпал побайтово (детерминированная сборка = бесплатный сертификат «перенос ничего не изменил»).
- Хостовые тесты (без устройства и без root): `tools/test-stubs-oom.sh` (инъекция отказа `realloc`; старое тело → двойное освобождение), `tools/test-proc-name.sh` (правило имени на строках с устройства). Хостовый `cc` = MSYS2 ucrt64 gcc; `<elf.h>` в MSYS2 **нет ни в одном тулчейне**, `O_CLOEXEC`/`pread`/`pwrite` в MinGW нет — поэтому хостовые тесты **не должны включать `vold-common.h`**, только беззависимые заголовки.
- Отказ чтения в `count_call_sites()` — это `-1`, а не частичный счёт: проверка «вызовов ровно 1» — **ворота**, и частичный счёт их открывает, а не закрывает. Тот же вопрос в `resolve_sym()` (vold-fusefs) решён иначе и осознанно: там пропуск сегмента fail-safe, потому что трамплины живут только в `.plt` (0 собранных → `plt_layout_delta()` = -1 = отказ), а ложные срабатывания `decode_stub()` отсекаются совпадением цели `ldr` с `r_offset`.

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
- `nsenter` **работает** под `su -c` (KernelSU, контекст `ksu`), но обязателен `--` перед командой: иначе toybox-`nsenter` примет `-c` от `stat -c %d` за свою опцию и упадёт с `Unknown option 'c'`. Без nsenter путь читается резолвингом `/proc/<pid>/root/…`.
- toybox `stat` не разыменовывает симлинк, `chown` — разыменовывает (`chown … /sdcard` меняет `/data/media/0`).
- f2fs: `stat -f -c %t` → `0xf2f52010` (`%T` = UNKNOWN); fuse → `0x65735546`.
- IP устройства меняется после перезагрузки (сообщает пользователь).
