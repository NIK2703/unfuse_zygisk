# unfuse_zygisk

Zygisk-модуль: доступ приложений к `/data/media` без FUSE/scoped storage.
Репозиторий `github.com/NIK2703/unfuse_zygisk`; рабочая копия `E:\projects\unfuse_zygisk`,
ветка `no-sdcardfs` (до 2026-10-09 звалась `fuse-only`). `main` на origin = `no-sdcardfs`
(fast-forward 2026-10-09, `87df5de`). Считается **не по счётчику коммитов**:
`origin/no-sdcardfs` — соседняя ветка от `bc95307` (README добавлен через веб-интерфейс),
она не предок и не потомок локальной.
**Вторая ветка — «sdcardfs» — живёт НЕ в git:** её рабочая копия
`C:\Users\Nikita\unfuse-build\sdcardfs-only` не является чекаутом (`git rev-parse` →
«not a git repository»), и все её правки делаются в копии. В репозитории она есть только
как устаревшая `origin/sdcardfs-only` (`2a1f43f`, 50 коммитов позади). Когда Nikita
говорит «ветка sdcardfs», он имеет в виду эту копию — смотреть надо туда.
Разбор дизайна — `docs/fuse-root-patch-design.md`; A11 — `docs/android-11-design.md`;
разбор лога с живого A11 crDroid — `docs/android-11-crdroid-log.md`;
варианты ПК-стенда A11 arm64 — `docs/android-11-arm64-stand.md`.

## Жёсткие запреты (Nikita)
- **Никаких сторожей** — ни при каких обстоятельствах («навсегда», 2026-10-09). Прецедент:
  `--guard` у `storage-fix` убирали (v3.1.0). Конфликт (напр. GCam-патчер) лечится без сторожа.
- **Не коммитить и не пушить без прямой просьбы.** «правь файлы» ≠ «оформляй в git».
  Коммит/пуш только по явной команде (`закоммить`, `запушь`, «на origin»). Правило общее.

## Конвенции сборки/установки
- Комментарии/коммиты по-русски, «почему» со ссылкой на AOSP.
- `./build.sh`, NDK `29.0.14206865`, сборка детерминированная (неизменный md5 = правка не доехала).
  ABI: `arm64-v8a`, `armeabi-v7a`.
- На устройство — **только атомарно** (`mv -f`); `cp -f` поверх `zygisk/*.so` усекает inode → SIGBUS zygote.
- **Модуль ничего не пишет**: отчёт только кодом возврата; логгирование удалять целиком.
- Ставить `install.sh` из `C:\Users\Nikita\unfuse-build\` (НЕ `ksud module install` — тот трогает
  отображённый в zygote `*.so`). `zygisk/*.so` обязан быть `u:object_r:system_lib_file:s0`
  (chcon в `install.sh`/4-м аргументе `atomic_put`).

## Ядро дизайна
- FUSE-маунт обязан жить: fd — контракт с MediaProvider, `active` ставит только `pf_init`.
- `mount("/dev/fuse",…,MS_LAZYTIME)` — единственный дискриминатор FUSE-сайта (у AppFuseUtil LAZYTIME нет).
  Решение по **форме**: хвост `fuse_path` == `/emulated` → бинд `/data/media`; иначе том
  (SD-карта/принятый) проходит насквозь. Альтернатива «биндить `/mnt/media_rw/<uuid>`» отвергнута:
  в state-free handler доступен только x1 = `fuse_path`.
- **SD-карта идёт через ту же `MountUserFuse()`** (PublicVolume делит с EmulatedVolume; отличие —
  `relative_upper_path` = UUID, `fuse_path=/mnt/user/<u>/<UUID>`). Без хвост-матчера модуль клал
  `/data/media` поверх карты → файлы карты скрыты (отзыв 2026-10-09).
- **Урок emitted-кода:** ошибки управления потоком ловит ТОЛЬКО исполнение на aarch64.
  Хостовый `--selftest` лишь собирает/дизасм; device `--selftest` реально исполняет handler
  (`mmap` EXEC + `fake_mount` recorder) — авторитетная проверка.
- **caller-saved не переживает вызов**: значение, нужное после `blr`, класть на стек, не в x10.
  (Регрессия 2026-10-09: цель маунта в x10 топтал реальный `mount()` → bind на `""`.)
- Байтовый цикл хвост-матчера закрывается на **первом** `ldrb` (тот двигает путь x12); второй `ldrb`
  двигает только шаблон x13. Структурный selftest проверяет **свойство** (цель — любой ldrb
  post-index; два цикла на разных загрузках), а не «cbnz→ldrb с тем же регистром» (навязывал баг).
- **Ветка `sdcardfs` — другой механизм, а не тот же фикс иначе.** Она не патчит vold и не биндит
  `/data/media` на `/mnt/user/<u>/<UUID>`: `module/storage.sh` поднимает sdcardfs из `/data/media`
  на `/mnt/runtime/{default,read,write,full}/emulated`, а Zygisk-плечо (`src/unfuse_zygisk.cpp`)
  подменяет **только** `.../emulated` в `/mnt/user/<u>`, `/mnt/androidwritable/<u>`,
  `/mnt/runtime/<view>`. Внешних томов не касается вообще → дефект «пропадает карта памяти» там
  невозможен, хвост-матчеру не к чему прицепиться. Обратное тоже верно: правки основной ветки
  (vold-патчеры, `storage-fix`, хуки libc) в копии `sdcardfs-only` неприменимы.

## Версии / ABI (кратко)
SDK 30..37 (A11..A17), shape-based, без ветвлений по релизам. arm64: 9 целей на 11–13, 8 на 14–17;
arm32 — 9 везде. 17 с `-mbranch-protection=standard`; 11 без bti/paciasp. vold-патчеры — только
EM_AARCH64 (32-битный vold не проверен, оставлено как есть). Таблицы — `tools/check-release-table.py`,
`tools/verify-hook-targets.py`, `tools/vold-targets.sh`.

## Сторонние патчеры (GCam, 2026-10-08/09)
Порт MGC dlopen-ит `*.lck`-патчер, читающий указатель по `entry+12` → мусор → SIGBUS. Решение без
сторожа: сняли `open/open64/openat/openat64` из `kHooks[]`, ловим open-семью на голом стабе
`__openat` (`mov x8,#0x38; svc #0; ret`). md5 `7975875a…`, коммит `fb2f2ab`.

## Устройство
- `marble` POCO F5, SDK 36 (`fuse=true`, `external_storage.sdcardfs.enabled` пуст). A11-блок тут
  не проявляется (условие `sdk=30` ложно). Живого стенда SDK 30 нет.
- **A11-отзыв (2026-10-09) — mido**: Redmi Note 4, crDroid 7.22 (`ro.crdroid.build.version`),
  SDK 30, ядро `4.9.295~zLOS`, ARM64-userspace, **Magisk Kitsune 27001** (форк 27.0; Zygisk
  встроенный, API v5 поддержан), рядом `zygisk_lsposed`, SELinux permissive. Форма хранилища у
  него **целевая** (`persist.sys.fuse=true`, sdcardfs `0`), но целевого состояния нет: FUSE на
  `/mnt/user/0/emulated` без bind'а `/data/media` → патч vold не встал либо не сработал.
  Разбор и что запрошено — `docs/android-11-crdroid-log.md`.
- **vold перезапускается незаметно.** `init.svc_debug_pid.<svc>` — текущий pid: у mido
  `vold = 1599` при `ro.boottime.vold = 9.93 с` (в это время pid'ы ~430), у остальных 7 сервисов
  pid согласуется с boottime. Перезапуск vold снимает ОБА хука (они в его памяти), накат — только
  дважды за загрузку, сторожа нет по правилу. Проверка: `stat -c %Y /proc/$(pidof vold)` против
  `/proc/uptime`, `logcat`/`dmesg` на смерть vold.
- **Zygisk-среда двух стендов разная.** Стенд `marble` — KernelSU 3.3.0 + **Zygisk Next**
  (`zn-daemon`, `/data/adb/zygisksu/`), и только там Zygisk-плечо когда-либо проверялось
  (`mount_storage_dirs`, замер 2026-10-07). У mido — **встроенный Zygisk Kitsune**: по changelog
  27001 загрузчик заменён на ptrace-инъектор **от Zygisk Next**, а API остался магазовский
  (`v27.0`: `ZYGISK_API_VERSION 5` + `mount_storage_dirs`; отказ лишь при `api_version > 5`).
  Поэтому «виноват Zygisk Kitsune» объясняет только Zygisk-плечо, не патч vold; главная грабля —
  **SuList** (модули грузятся лишь в приложения-списке). Тест — маркер
  `/data/adb/unfuse_zygisk.state/once`.
- **ПК-стенд A11 arm64** (разбор — `docs/android-11-arm64-stand.md`). Хост Никиты —
  `AMD64`, emulator `36.5.10` (есть `qemu-system-aarch64.exe`), образов/AVD нет.
  Стенд обязан быть настоящим aarch64 (патчер — только `EM_AARCH64`); на x86 arm64-гость
  идёт лишь TCG, ARM-трансляция (Houdini/libndk) не годится — `vold` остаётся x86_64.
  Путь: AVD `arm64-v8a` API 30 (`-accel off -gpu swiftshader_indirect -qemu -machine virt`),
  откат на emulator `34.2.16` (build 12038310), root — rootAVD, но **Magisk ≥ 27.0**
  (наш модуль = API v5; на 26.x отвергается). Альтернатива — redroid `11.0.0` (multi-arch)
  + `catlair/redroid-magisk` на ARM-хосте (Oracle Ampere A1 free).
- **IP меняется после reboot** (наблюдались 10.43.67.59, 10.43.59.38, 10.170.241.19). `adb` не в PATH:
  `C:\Users\Nikita\AppData\Local\Android\Sdk\platform-tools\adb.exe`. Перед заменой — бэкап
  `rollback-device/mod-backup-<дата>.tgz`. `adb push` из Git Bash — `MSYS_NO_PATHCONV=1`.
- `adb shell "su 0 sh …"` рвёт по `;`; демон adb умирает между вызовами. `nsenter` под `su -c` — `--`.
  f2fs `stat -f -c %t` → `0xf2f52010`, fuse → `0x65735546`.

## Бенчмарк (2000×4КиБ, root в ns приложения, 2026-10-09)
FUSE без модуля медленнее сырого дерева в 52–780×; `sdcardfs-only` — 1.0–6.6×. Модуль (сырой f2fs)
быстрее sdcardfs везде, где есть create/rename (rename 5.3×, mkdir 6.6×, create 2.5×). Замер от root —
нижняя оценка; от uid 2000 create/rename/unlink в 6–8× медленнее.
