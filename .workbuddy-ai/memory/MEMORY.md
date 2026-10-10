# unfuse_zygisk — память проекта

Zygisk-модуль: доступ приложений к `/data/media` без FUSE/scoped storage.
`github.com/NIK2703/unfuse_zygisk`; копия `E:\projects\unfuse_zygisk`.
**Одна ветка `master`, два модуля из одного дерева** (сведено 2026-10-10): `unfuse`
(патч vold + libc-хуки) и `unfuse-sdcardfs` (sdcardfs на `/mnt/runtime/*/emulated`).
Ветки `no-sdcardfs` и `sdcardfs-only` слиты в неё и удалены локально (остались в reflog
и на origin). id у обоих — `unfuse_zygisk`: модули альтернативные (на A11 ставят
`persist.sys.fuse` в противоположные значения), вместе не живут, второй заменяет первый.
Собираются одним `./build.sh` (`out/unfuse-<ver>.zip`, `out/unfuse-sdcardfs-<ver>.zip`);
общее — `module/common/` (META-INF + `lib.sh`). Структура — `docs/repo-layout.md`.

Детали — в `docs/` (здесь только индекс и грабли): `repo-layout.md`,
`fuse-root-patch-design.md`, `android-11-design.md`, `android-11-crdroid-log.md`,
`android-11-arm64-stand.md`, `android-11-app-access-fix.md`, `marble-reboot-vold-failed.md`,
`diagnostics.md`, `redroid-vm-module-diagnosis.md`, `redroid-vm-reliability-test.md`,
`sdcardfs-vm-verification.md`, `redroid-vm/README-redroid-VM.md` + скилл **`redroid-vm-magisk`**.

## Жёсткие запреты (Nikita)
- **Никаких сторожей, никогда.** Прецедент: `--guard` у `storage-fix` убран (v3.1.0).
  Одноразовое решение в известной точке — можно; наблюдение за системой — нет.
- **Не коммитить/пушить без прямой просьбы.** «правь файлы» ≠ «оформляй в git».

## Конвенции
- Комментарии/коммиты/доки по-русски, «почему» со ссылкой на AOSP.
- `./build.sh`, NDK `29.0.14206865`; сборка детерминированная (тот же md5 = правка не доехала).
- На устройство — **только атомарно** (`mv -f`); `cp -f` поверх `zygisk/*.so` усекает inode
  под живым mmap → SIGBUS zygote. Ставить `install.sh` из `C:\Users\Nikita\unfuse-build\`
  (НЕ `ksud module install`). `zygisk/*.so` = `u:object_r:system_lib_file:s0`.
- Zygisk Next держит module.so по **fd**, а не по пути: после `mv` новая сборка активна
  только после перезагрузки. Проверять — только после reboot.
- **Модуль ничего не пишет**: отчёт только кодом возврата. Исключение — диагностический лог
  `/data/adb/unfuse_zygisk.debug.log` (`module/log.sh` + `module/diag.sh`, ручной снимок и
  `--probe <пакет>`). Диагностическая сборка пишет его **по умолчанию**, глушится файлом-«стоп»
  `/data/adb/unfuse_zygisk.nolog` (маркер-включатель `.debug` **отменён** — на живом пользователе
  шаг неочевиден, лог не появлялся). **Файл перезаписывается ЦЕЛИКОМ в начале каждой загрузки**
  (`unfuse_log_begin` в `post-fs-data.sh`, безусловно), накопления прошлых загрузок нет;
  прежний механизм `boot_id` + `$MODDIR/.log_boot` убран, `service.sh` файл не обрезает —
  дописывает свою стадию в ту же загрузку. Имя с `.debug.` — потому что `unfuse_zygisk.log` занят
  мёртвым логом снятого `marble_unfuse_auto` (121 КБ на `marble`). Всё — `docs/diagnostics.md`.

## Ядро дизайна
- FUSE-маунт обязан жить: fd — контракт с MediaProvider, `active` ставит только `pf_init`.
- `mount("/dev/fuse",…,MS_LAZYTIME)` — единственный дискриминатор FUSE-сайта. Форма: хвост
  `fuse_path` == `/emulated` → бинд `/data/media`; иначе том насквозь. «Биндить
  `/mnt/media_rw/<uuid>`» отвергнуто: в state-free handler доступен только x1.
- **SD-карта идёт через ту же `MountUserFuse()`**; без хвост-матчера `/data/media` клался
  поверх карты → её файлы скрыты.
- **emitted-код:** ошибки потока выполнения ловит только запуск на aarch64 (device
  `--selftest` реально исполняет handler; хостовый — лишь собирает/дизасм).
- **caller-saved не переживает `blr`**: нужное после вызова — на стек, не в x10.
- Хвост-матчер закрывается на **первом** `ldrb`; selftest проверяет **свойство**, не форму.
- `ZYGISK_API_VERSION` = **4**: в v27.0 `ZygiskModule::valid()` знает 1..4, «5» → `unloaded`.

## Доступ приложений (свежее, правки B–D внесены 2026-10-09)
- Держится на ACL: именованная запись **`gid 9997`** + libc-хуки, shaping режима до сисколла.
- На A11 AOSP даёт доступ **тремя** механизмами: (1) владелец = uid приложения; (2) per-package
  default ACL (`SetDefaultAcl`, `Utils.cpp:192`; вызовы `:398` `Android/data/<pkg>` с
  `additionalGids.push_back(uid)` и `:1615` OBB); (3) `other = --x` (`02771`), `Utils.cpp:1588`.
- Модуль снимал (2) `vold-noacl` и (3) нулём `ACL_OTHER` — отсюда EACCES у приложений.
- Классы отказа: **F1** нет записи для приложения, **F2** маска обнулена на `0600`,
  **F3** снят `other` при живом FUSE, **F4** патч vold не встал, а остальное встало (mido),
  **F5** нет починки после позднего писателя. Разбор — `docs/android-11-app-access-fix.md`.
- **Правки:** B — `storage-fix` сохраняет `S_IRWXO` (только там, где он есть: четыре уровня
  `Android*`, как в Android 10), `hook_libc.cpp` синхронно; C — глагол `--app-dirs`
  (per-package запись из `st_uid`), вызывается из `storage.sh`; D — `storage.sh` идёт **после**
  патчей vold, `service.sh` не ставит `vold-noacl`, если модель модуля не состоялась. **Fix A
  (merge-handler в vold) снят:** `vold-noacl` уже глушит запись vold, поэтому `storage-fix` пишет
  надмножество (9997 + uid) — третий arm64-handler не нужен.
- **Посылка D уточнена 2026-10-09.** Форма хранилища отвечает не всегда: в момент `service.sh`
  `/storage/emulated/0` ещё **tmpfs** (`1021994` = `0x01021994`) — бинды `/storage` делает триггер
  `init.rc` на `zygote-start`, позже `service`. Поэтому теперь требуется ещё и
  `vold-fusefs --check` = 0 (патч в памяти vold). Форма осталась вторым входом: fuse = не ставим.
- **Проверка доступа приложения — только по его mount-namespace и живым группам.** У здорового
  приложения в `/proc/<pid>/mounts` для `/storage/emulated` **две** строки: `fuse` (маунт vold,
  его fd — контракт с MediaProvider, обязан жить) и `f2fs` (наш бинд поверх). Решает последняя.
  «В списке есть fuse» — не признак отказа.
- Приложения реально в группе 9997; `1078/1079` у них **нет** (только shell/root).
- **Грабли:** `su <uid>` у Magisk сбрасывает доп. группы — ACL проверять только `su 9997`;
  `--probe` берёт группы из живого процесса и говорит, если процесса нет.

## Модуль unfuse-sdcardfs (`module/unfuse-sdcardfs/`) — другой механизм
Не патчит vold и не биндит `/data/media`: `storage.sh` поднимает sdcardfs на
`/mnt/runtime/{default,read,write,full}/emulated`, Zygisk-плечо (`src/unfuse_sdcardfs.cpp`)
подменяет только `.../emulated` в `/mnt/user/<u>`, `/mnt/androidwritable/<u>`,
`/mnt/runtime/<view>`. Внешних томов не касается → «пропадает карта» невозможен. Обратно:
правки модуля `unfuse` (vold-fusefs, storage-fix, hook_libc) там неприменимы. Лог свой —
`/data/adb/unfuse_zygisk.log` (простой `log()`, НЕ `log.sh`); статус — `status.sh`
пишет знак в описание `module.prop`. **Дефект:** маркер в `/data/adb/unfuse_zygisk.once`
(`drwx------ root`) → EACCES в `claim_once()`. Лечится переносом в
`/data/adb/unfuse_zygisk.state/` (как у `unfuse`).

## Стенды
- **`marble`** POCO F5, SDK 36, `persist.sys.fuse=true`, ядро `5.10.269-Bouquet-v5.1`
  **умеет sdcardfs**. KernelSU 3.3.0 + **Zygisk Next** (`/data/adb/zygisksu/`) — единственное
  место, где Zygisk-плечо проверялось. На нём стоял модуль `unfuse` (бывш. no-sdcardfs).
- **redroid-ВМ A11** — контейнер `11.0.0` **на телефоне**, порт **5556** (5555 = телефон).
  aarch64, SDK 30, `/mnt/runtime` нет — целевая FUSE-форма. SELinux **Disabled** (на метках
  ничего не строить). Magisk **Delta 25206**; корень через `adb root` (SuList).
  **НИКОГДА `adb reboot`** — `--privileged` делит ядро с телефоном; только
  `su 0 sh /data/local/tmp/redroid/host.sh restart`. Модули не грузились из-за пустого
  `/data/adb/magisk` → лечится `cp -a /system/etc/init/magisk/* /data/adb/magisk/`.
  32-битный Zygisk в ВМ сломан.
- **`mido`** Redmi Note 4, crDroid 7.22, SDK 30, Kitsune 27001, permissive — состояние **F4**.
- **ПК-стенд A11 arm64** — только настоящий aarch64 (AVD API 30 `-accel off`, Magisk ≥ 27.0).

## Инструменты и грабли
- **Обёртки `ph.sh`/`ad.sh`** (телефон, `dev.ip`) и `vm.sh` (5556): сами делают
  `start-server` + `connect` (демон умирает между вызовами). `adb shell "su 0 sh …"` рвёт по `;`.
  IP меняется после reboot.
- **`adb push` и MSYS.** `MSYS_NO_PATHCONV=1` ломает **источник** (`cannot stat '/e/projects/…'`),
  без него MSYS конвертирует **приёмник** (`…/PortableGit/data/local/tmp/…`). Верно:
  `NO_PATHCONV=1` + источники через `cygpath -m`. Готовые скрипты — `push.sh` (телефон) и
  `push-vm.sh` (ВМ).
- **`su` внутри redroid-ВМ после `adb root` висит** (ждёт решения политики Magisk): команда уходит
  в таймаут, инструмент убивает её по SIGTERM **без вывода**. `su` там не нужен — shell уже uid=0.
  `host.sh` живёт на **телефоне**: `ph.sh shell "su 0 sh /data/local/tmp/redroid/host.sh restart"`.
- `stat -f -c %t`: f2fs `0xf2f52010`, fuse `0x65735546`, sdcardfs `0x5dca2df5`, tmpfs `0x01021994`.
  Печатается **hex без `0x`**: `1021994` — это tmpfs, а не десять миллионов.
- **`acl-dump`** (`C:\Users\Nikita\unfuse-build\acl-dump`) — единственный способ прочитать
  POSIX ACL: toybox `getfattr` обрывается на NUL в версии, `ls` знак `+` не рисует.
- **`vold` перезапускается незаметно** (`init.svc_debug_pid.vold` vs `ro.boottime.vold`):
  снимает ОБА хука, накат — дважды за загрузку, сторожа нет.
- **vold'ов может быть несколько.** redroid-ВМ делит с телефоном PID-namespace, и её init
  поднимает свой vold: `pidof vold` на `marble` = `867 36504`, `cmdline` у обоих одинаковый.
  Свой vold — тот, у кого `PPid=1` в своём namespace; `find_vold()` теперь предпочитает его и
  говорит в stderr, если кандидатов больше одного. `--pid` руками не задавать. Иначе патч уходит
  в чужой vold, а `--check` рапортует успех — скрытый F4.
- **Свой давний краш-петль телефона (не наш):** `system_server` ~на 38-й секунде каждой
  загрузки (`AbstractMethodError … IProcessObserver.onProcessStarted`) — LSPosed со scope
  `system`. **Не приписывать модулю.**
- **Перезагрузку не путать с нашими действиями.** `ro.boot.bootreason=reboot,vold-failed`
  = сработала `reboot_on_failure` сервиса vold (патчим его память живьём — исключать нельзя).
  `docker restart` контейнера перезагрузить телефон не может. См. `docs/marble-reboot-vold-failed.md`.

## Бенчмарк (2000×4 КиБ, root в ns приложения)
FUSE без модуля медленнее сырого дерева в 52–780×; `sdcardfs-only` — 1.0–6.6×. Модуль быстрее
sdcardfs везде, где есть create/rename (rename 5.3×, mkdir 6.6×, create 2.5×).
