# unfuse_zygisk

Zygisk-модуль: доступ приложений к `/data/media` без FUSE и scoped storage.
`github.com/NIK2703/unfuse_zygisk`, только `E:\projects\unfuse_zygisk` (ветка `no-sdcardfs`,
до 2026-10-09 звалась `fuse-only` — имя вводило в заблуждение: обе ветки, `sdcardfs-only` и эта,
дают «FUSE в пути приложения нет»; отличает их отсутствие sdcardfs).
Разбор — `docs/fuse-root-patch-design.md`; устройство — скилл `zygisk-module-device-regression-sweep`;
детали заходов — `memory/2026-10-*.md`.

## Запреты (Nikita)
- **Никаких сторожей** (2026-10-09: «навсегда»). Прецедент: `--guard` у `storage-fix` убирали (v3.1.0).
  Цена доводом не считается. Конфликт с патчером GCam лечится **без сторожа**.
- **Не коммитить и не пушить без прямой просьбы** (2026-10-09: «я не просил тебя что-либо коммитить
  и пушить»). Просьба «правь файлы» ≠ просьба «оформляй в git». Коммит/пуш — только по явной команде
  (`закоммить`, `запушь`, «переименовать и на origin» и т.п.). Правило общее, не только для этого проекта.

## Конвенции
- Комментарии/коммиты по-русски, «почему» со ссылками на AOSP.
- `./build.sh`, NDK `29.0.14206865`, сборка детерминированная (неизменный md5 = правка не доехала).
  ABI: `arm64-v8a`, `armeabi-v7a`. Бинарники с устройства → `device/` (gitignore); файлы не удалять.
- На устройство — только атомарно (`mv -f`; `cp -f` поверх `zygisk/*.so` усекает inode → SIGBUS в zygote).
  Новая сборка не активна до перезагрузки (zn-daemon держит fd).
- **Модуль ничего не пишет** — отчёт только кодом возврата; логгирование удалять целиком.

## Ядро дизайна
- FUSE-маунт обязан жить: fd — контракт с MediaProvider, `active` ставит только `pf_init` (иначе том `unmountable`).
- Маунт `/mnt/user/<u>/emulated` приходит в namespace сам (слейв init); `MS_REC|MS_PRIVATE` рвёт связь.
  Путь — **по форме** (конец `/emulated` + `MS_LAZYTIME`), не по значению.
- `find_fuse_site()` — **не по смежности** (adrp+add то подряд, то вразбивку); одна функция на все версии.
  LAZYTIME-площадка ровно одна во всех 8 образах. Регрессия — `anchor_selftest()`, коммит `3a83678`.
- `scan_back()`: связывание «ближайшее к вызову» → **первое найденное**; иначе `x0_from == src_reg`
  не сходится → rc 8. `w3` обязан накапливать половины. Окно 32 слова, `bl` окно не рвёт.
- Переполнение списка площадок → отказ `18` **до** фильтра LAZYTIME (иначе чтение за массивом).
- Отказ называет причину: `5..10` якорь, `11..17` нет дома (`vold_exec_page`), `18` площадок>списка,
  `19` аргументы; печатает `word()`. `ep_exit_code()` **без `default:`**; `EP_OK` там = `17` (баг).
- Места в образе нет ни на одном релизе → `vold_exec_page()` (ptrace) — единственный путь.
- `vold.rc` → `reboot_on_failure`: SIGSEGV в vold = bootloop. `libfuse` в vold нет (сырой
  `mount("/dev/fuse",…)`, `Utils.cpp:1676`), входит через `libfuse_jni`.

## Версии (`src/android_ver.h`)
SDK 30..37 (A11..A17). Целей (arm64): 9 на 11–13 (`renameat` — корень), 8 на 14–17 (`renameat` —
переходник на `renameat2`); на arm32 — 9 на всех (колонка `installed_arm`, читает `unfuse_installed()`).
Незнакомый SDK → профиль 17 («не проверялся»). Патч shape-based, ветвлений по
релизмам нет. 17 с `-mbranch-protection=standard` (bti c, +4); 11 без bti/paciasp.

## Применимость (источник истины)
- libc — `tools/verify-hook-targets.py`: 8 arm64-образов ok=19 rc=0; **8 arm32 ok=19 rc=0**
  (`device/libc/libc-arm-a{11..17}.so`, зеркало `E:\projects\12\libc`).
  Корни (kHooks + стаб `__openat`, считаются только реальные `MODULE_HOOKS`):
  **arm64 9 на 11–13, 8 на 14–17** (`renameat` — переходник); **arm32 9 на ВСЕХ** (`renameat` там корень).
  arm32 `rename`/`link` — тела (28 байт), не из kHooks, покрыты косвенно через `renameat2`/`linkat`.
- vold — `tools/vold-targets.sh --dry-run --file` (образы `/e/projects/12/bin/vold-a{11..17}`).
- Таблица релизов — `tools/check-release-table.py` (8/8 сходится, обе колонки: `installed`/`installed_arm`).

## ABI
- **Обе ABI готовы и измерены.** arm64-v8a и armeabi-v7a реализованы; выбор — препроцессором
  (`#if defined(__aarch64__)/__arm__`), одна форма на все релизы, ветвлений по версии Android нет.
- ARM32-патч входа (`src/arm32_patch.h`, чистые функции, те же байты у verifier'а):
  Thumb-2 4-aligned `ldr.w pc,[pc,#0]`(0xF000F8DF)+литерал = 8 байт; Thumb-2 2-aligned
  `movw/movt r12; bx r12`(0x4760) = 10; ARM `ldr pc,[pc,#-4]`(0xE51FF004)+литерал = 8. Ни BTI, ни PAC
  на AArch32 нет → паддинга нет. Thumb-бит (бит 0 `st_value`/dlsym) теряется у цели ветки → режим
  брать из `st_value`. Запись 10-байтной формы — 2-байтными сторами (адрес 2-выровнен).
- ARM32 `__openat` (`src/openat_stub.h:openat_stub_find_arm`): ARM-режим, 32 байта (17 — 28),
  пролог-признак `mov r12,r7`(0xe1a0c007)+`movw r7,#0x142`(0xe3007142)+`svc #0`(0xef000000) —
  уникален во всех 8 образах; конец = первый безусловный `b` (хвост `b __set_errno_internal`).
  Хвостовой ARM-`b` переходником НЕ считается (иначе 4 сисколл-стаба съели бы все корни).
- `src/func_size.cpp` разбирает и ELF32/EM_ARM (`scan_symtab32`/`scan_object32`), класс берёт из
  заголовка файла, а не из ABI сборки.
- Хостовые тесты формы: `tools/test-arm32-patch.sh` (48 случаев, сверка с verifier'ом, 0 расхождений);
  `tools/test-openat-stub.sh` (110 проверок, 0 провалов, оба класса ELF).
- Устройство (Android 16, zygote64_32), **проверено живьём 2026-10-09**:
  `hookselftest-arm` — 46/0, «установлено 9 из 9», `renameat=ok`, `__openat=ok`;
  arm64 `hookselftest` — 46/0, 8/9 (`renameat=коротка`).
- `tools/check-live-patch.sh {32|64}` — патч в ЖИВОМ процессе приложения: r-xp-сегмент libc
  читается сквозь `/proc/<pid>/mem` и сравнивается с файлом libc; смещения сопоставляются с
  целями через `verify-hook-targets.py --json`. **Zygote не патчится** (патч в `postAppSpecialize`),
  свидетель — только процесс приложения. Замер: 32-бит 9/9 (участки по 8 байт), 64-бит 8/8 (по 20).
  Вспомогательные: `tools/live-patch-pick.sh`, `tools/live-patch-diff.sh` (на устройстве).
- **vold-патчеры — только ELF64/EM_AARCH64** (`tools/vold-common.h`), на 32-битном vold честно
  отказывают. В `E:\projects\12` 32-битного vold нет → форму проверить нечем. На zygote64_32
  vold 64-битный, ущерба нет; для 32-бит-онли устройства нужен эталонный 32-битный vold.
  (Решение Nikita 2026-10-09: оставить как есть.)

## Имя vold
- Запускается не голым (`--blkid_context=…`); по comm не опознать (`binder:<pid>_<n>`). Опознание:
  `/proc/<pid>/exe`; резерв `/proc/<pid>/cmdline` → NUL→пробел, обрезать по 1-му пробелу. Префикс
  4 байта принимает `vold_prepare_subdirs`. Вынесено в `tools/proc-name.h`, `tools/stubs.h` (тождественно).
  `count_call_sites()`: отказ = `-1`, не частичный счёт.

## Сторонние патчеры (порт GCam, 2026-10-08/09)
- Порт `com.android.MGC_9_7_047` dlopen-ит `*.lck` со своим патчером: закрывает вход 16-байтным
  переходником, любой чужой переходник на входе считает хуком и строит трамплин, читающий указатель
  по `entry+12` (середина своего) → мусор → `br x17` → SIGBUS. Формой не лечится, сторожем запрещено.
- **Решение без сторожа:** модуль снял `open/open64/openat/openat64` из `kHooks[]` и ловит open-семью на
  голом стабе сисколла `__openat` (по форме `mov x8,#0x38; svc #0; ret`; локален, вне `.dynsym`).
  `__open_2/__openat_2` остаются целями. Устройство: `hookselftest` 8/9, `__openat=ok`, 46/0; MGC жив.
  md5 `7975875a409272b0d000d187e3b51a3d`, коммит `fb2f2ab`. Откат: `device/rollback/arm64-v8a.so.before-20261009`.

## vold-fusefs — два варианта
- **STACK** (`no-sdcardfs` @ `5c743f3`, в дневниках ещё как `fuse-only`): FUSE + bind `/data/media` сверху на одном `fuse_path`, `umount2` до 4 слоёв.
- **RELOCATE** (`relocate` @ `ecb48eb`): FUSE → `SCRATCH = fuse_path + ".fuse"`, `fuse_path` — один слой bind;
  путь читать **побайтово** (8-байтовое чтение чужого `std::string` → SIGSEGV → bootloop).

## Грабли устройства
- `adb shell "su 0 sh …"` (adb рвёт по `;`); демон adb умирает между вызовами; `adb pull/push` ждёт
  Windows-путь. `nsenter` под `su -c` — обязателен `--`. f2fs `stat -f -c %t` → `0xf2f52010`, fuse → `0x65735546`.
  `head` в пайпе молча обрезает — считать `grep -c`. IP меняется после reboot; подтверждать у пользователя.
