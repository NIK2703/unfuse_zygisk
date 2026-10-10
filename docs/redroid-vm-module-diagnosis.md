# redroid-ВМ (Android 11 + Magisk Delta): почему модуль не работает

Разбор выполнен на живой ВМ `10.57.53.217:5556` (redroid `11.0.0`, hostname
`redroid11_arm64`), 2026-10-09. Инструкции по самой ВМ — `docs/redroid-vm/README-redroid-VM.md`
и скилл `redroid-vm-magisk`.

## 1. Что за устройство

| | |
|---|---|
| образ | redroid `11.0.0-latest`, контейнер на телефоне (`marble`), ядро `5.10.269-Bouquet-v5.1` |
| архитектура | **настоящий aarch64** (`uname -m = aarch64`), `ro.product.cpu.abi = arm64-v8a` |
| SDK | 30 (Android 11) |
| форма хранилища | `persist.sys.fuse = true`, `/mnt/runtime/*` отсутствуют → **целевая FUSE-форма** |
| SELinux | **Disabled**; `ls -Zd` печатает метку буквально `HACKED` |
| root | `adb shell` уже uid 0 (без `su`) |
| Magisk | Delta **25206**, `4dbd8358-delta:MAGISK:R`; zygisk=1, denylist=0 |

Форма хранилища — ровно та, под которую модуль написан (как у mido). Ветка
`android-11-design.md` (вендор снял `persist.sys.fuse true`) тут не при чём.

## 2. Симптом

Целевого состояния нет: `/mnt/user/0/emulated` — по-прежнему
`/dev/fuse … fuse rw,lazytime,…`. Каталога `/data/adb/unfuse_zygisk.state/` нет.

## 3. Слой 1 (корень): Magisk не доходит до скриптов модуля

**Ни один скрипт модуля не выполнялся.** Доказано тремя независимыми способами.

### 3.1. Лог Magisk: post-fs-data сорвался до `handle_modules()`

`/cache/magisk.log` (лог текущей загрузки, 10 МБ):

```
14:42:43.858 I : * Initializing Magisk environment
14:42:43.858 E : open /sbin/stub.apk failed with 2: No such file or directory
              ... ~90 секунд спама «Bad file descriptor» ...
14:42:45.009 E : cert: invalid APK format
14:42:45.009 E : * Magisk environment incomplete, abort
14:42:45.009 I : * Loading modules
14:42:46.410 I : ** late_start service mode running
14:42:46.413 I : * Running module service scripts      <-- и НИ ОДНОЙ строки после
```

Строки `post-fs-data.sh` в логе — **ноль**. Формат исполнения скриптов —
`<module>: exec [<script>]` (сверено с логом mido, где есть
`unfuse_zygisk: exec [post-fs-data.sh]`). Здесь нет ни одной такой строки.

### 3.2. Механика — по исходнику Magisk

`native/src/core/bootstages.cpp` (v26.4):

```c
static bool magisk_env() {
    LOGI("* Initializing Magisk environment\n");
    preserve_stub_apk();                 // -> "open /sbin/stub.apk failed"
    xmkdir(DATABIN, 0755);               // DATABIN = /data/adb/magisk
    ...
    if (access(DATABIN "/busybox", X_OK))
        return false;                    // <-- сюда и попадаем
    ...
}

static void post_fs_data() {
    ...
    if (!magisk_env()) {
        LOGE("* Magisk environment incomplete, abort\n");
        goto early_abort;                // <-- перескок
    }
    ...
    exec_common_scripts("post-fs-data");
    initialize_denylist();
    handle_modules();                    // <-- ВЫПОЛНЕН НЕ БУДЕТ

early_abort:
    load_modules();                      // только magic mount
    boot_state |= FLAG_POST_FS_DATA_DONE;
}
```

`handle_modules()` (`module.cpp:440`) — **единственный** вызов
`prepare_modules()` / `collect_modules()`, а те — единственные, кто наполняет
`module_list`. Раз шаг пропущен, `module_list` остаётся **пустым**.

`late_start()` → `exec_module_scripts("service")` (`scripting.cpp:121`):

```c
void exec_module_scripts(const char *stage, const vector<string_view> &modules) {
    LOGI("* Running module %s scripts\n", stage);   // <-- заголовок в логе ЕСТЬ
    if (modules.empty())
        return;                                     // <-- и сразу выход
```

Отсюда дословно наблюдаемая картина: заголовок есть, скриптов нет.

**Итог:** ни `post-fs-data.sh`, ни `service.sh` ни одного модуля не запускаются,
файлы модулей не магик-маунтятся. К `unfuse_zygisk` это не специфично.

### 3.3. Почему `magisk_env()` падает — дефект сборки образа

`/data/adb/magisk/` **пуст** — в нём только `.` и `..`. Это не следствие SELinux
и не следствие чего-то ещё: каталог создаётся пустым по замыслу rc-файла образа.

Сборка образа (`/data/local/tmp/redroid/build/` на телефоне):

```
# Dockerfile
FROM redroid/redroid:11.0.0-latest
COPY magisk /
```

`magisk/` содержит `sbin/` (**пустой**) и `system/etc/init/magisk/` (payload:
`busybox`, `magisk.apk`, `magisk64`, `magiskboot`, `magiskinit`, `magiskpolicy`),
а также подменённый `system/etc/init/bootanim.rc`:

```
on post-fs-data
    exec u:r:su:s0 root root -- /system/etc/init/magisk/magisk64 --auto-selinux --setup-sbin /system/etc/init/magisk
    exec u:r:su:s0 root root -- /system/etc/init/magisk/magiskpolicy --live --magisk "allow * magisk_file lnk_file *"
    mkdir /sbin/.magisk 700
    ...
    exec u:r:su:s0 root root -- /sbin/magisk --auto-selinux --post-fs-data
on property:sys.boot_completed=1
    mkdir /data/adb/magisk 755          <-- создаётся ПУСТЫМ, и только
```

`--setup-sbin` наполняет `/sbin` (и это работает: `/sbin/magisk64`,
`/sbin/magiskpolicy` на месте), но **`/data/adb/magisk/` наполняет не он, а
установщик Magisk** — тот, что распаковывает payload из APK. В bootless-контейнере
установщик не запускается никогда, и rc его не заменяет: единственное, что образ
делает с `/data/adb/magisk` — создаёт пустой каталог.

Поэтому `magisk_env()` падает **на каждой загрузке**, детерминированно. Это
дефект образа, а не ошибка пользователя и не свойство нашего модуля.

Побочно это же объясняет README §7: `magisk --install-module` → `Incomplete Magisk
install` — нет `/data/adb/magisk/util_functions.sh`.

## 4. Слой 2 (следствие): Zygisk в этой ВМ выключен целиком

`zygisk_enabled` — глобальная переменная `bootstages.cpp:30`, инициализированная
`false`, и присваивается из БД **только внутри пропущенной ветки**
(`bootstages.cpp:384`, в том же `else`, что и `handle_modules()`).

Дальше `module.cpp:307`:

```c
    // Mount on top of modules to enable zygisk
    if (zygisk_enabled) {
        string zygisk_bin = get_magisk_tmp() + "/"s ZYGISKBIN;
        mkdir(zygisk_bin.data(), 0);
        mount_zygisk(32)
        mount_zygisk(64)          // подмена /system/bin/app_process<bit>
    }
```

То есть инжекция Zygisk живёт в `load_modules()`, но **под флагом**, который так и
остался `false`. `collect_modules()` (открытие `zygisk/arm64-v8a.so` модулей) тоже
под этим флагом.

Проверено на ВМ:

- `/system/bin/app_process64` — **тот же inode и размер** (2239158, 33864), что и
  в `/sbin/.magisk/mirror/system/bin/app_process64` → подмены не было;
- `/sbin/.magisk/zygisk/` отсутствует;
- в `/cache/magisk.log` **ни одной** строки со словом `zygisk`;
- процесса `zygiskd` нет.

**Вывод: Zygisk-плечо модуля в этой ВМ мертво, и по той же причине, что и
vold-плечо.** `settings.zygisk=1` в БД не значит ничего: значение просто никто не
прочитал.

## 5. Слой 3 (сам модуль): на этой A11 + AOSP-vold всё исправно

Чтобы проверить модуль в обход Magisk, `post-fs-data.sh` запущен вручную
(штатный обходной путь из README). Результат — **102 мс, rc=0**, и дальше:

| проверка | было | стало |
|---|---|---|
| `vold-fusefs --check` | 1 | **0** |
| `vold-noacl --check` | 1 | **0** |
| `storage-fix --check /data/media /data/media/0` | 1 | **0** |

Предварительно, до всякого запуска:

| проверка | rc | смысл |
|---|---|---|
| `vold-fusefs --dry-run` | 0 | площадка `/dev/fuse` найдена, все 4 предпосылки сходятся |
| `vold-fusefs --selftest` | 0 | эмитированный хендлер **реально исполнен на aarch64** — код верен |
| `vold-noacl --dry-run` | 0 | то же для второго патчера |
| `vold-noacl --selftest` | 0 | отображение JUMP_SLOT ↔ стаб однозначно |

Дальше — цикл размонтирования/маунта тома (`sm unmount 'emulated;0'` →
`sm mount 'emulated;0'`), чтобы патч отработал на живом маунте:

```
/dev/fuse          /mnt/user/0/emulated  fuse  rw,lazytime,...   <- FUSE, снизу
/dev/block/sda33   /mnt/user/0/emulated  f2fs  rw,lazytime,...   <- bind /data/media, СВЕРХУ
```

Ровно то, что модуль и должен делать. Проверки эффекта:

- `stat -f -c %t` на `/mnt/user/0/emulated`, `/storage/emulated`,
  `/storage/emulated/0` → **`f2f52010`** (f2fs), а не `0x65735546` (fuse) — bind
  действительно верхний, FUSE наружу не виден;
- размонтирование прошло **чисто**, без ENOTCONN (хук `umount2` снимает оба слоя —
  то есть регрессия, ради которой он и написан, на этом vold не воспроизводится);
- ACL: `su 9997 -c 'ls /storage/emulated/0'` и `ls /data/media/0` — читается.
  Проверено, что app-процессы действительно имеют gid **9997** в дополнительных
  группах (`/proc/<pid>/status`: у `com.android.systemui` —
  `1065 1077 3001 3002 3006 9997 20109 50109`), то есть дизайн «ACL для группы
  9997» состоятелен. Отказ у uid 2000 (shell) — ожидаем: у shell нет 9997.

**Вывод: сам модуль на этой ВМ работает.** Дефектов в нём на A11 + обычном
AOSP-vold не обнаружено.

### Ложная тревога, чтобы не повторять

`storage-fix --check /data/media /data/media/0 /data/media/obb` возвращает 1 на
этой ВМ не из-за ACL: каталога `/data/media/obb` здесь **нет** (в `/data/media`
только `0`), и `--check` валится на несуществующем пути. Сам `storage.sh` это
учитывает (`if [ -d /data/media/obb ]`), поэтому из скрипта всё в порядке.

## 6. Что этим НЕ объясняется (и что опровергнуто)

- **Гипотеза README §6 «виноват SELinux/seclabel/HACKED»** — наблюдение верное
  (SELinux действительно Disabled, метки фиктивные, `chcon` не работает,
  `magiskpolicy` не может загрузить политику), но **причиной не является**:
  `access(DATABIN "/busybox", X_OK)` падает потому, что файла нет физически
  (каталог создаётся пустым по rc образа), а не потому, что его не пускает
  политика. `HACKED` — следствие, а не причина.
- **`x10`-дефект (caller-saved) тут не при чём** — `--selftest` на устройстве
  исполняет хендлер по-настоящему и проходит.
- **vold не перезапускался.** `ro.boottime` всех десяти сервисов согласованы
  между собой (1264–1273 с при `ro.boottime.init` = 1264.78 с); pid 22 у vold —
  норма для свежего PID-namespace контейнера. (У mido, для сравнения, аномалия была.)
- **Порядок «патч опоздал»** на этой ВМ не проверяем, пока скрипты не запускаются
  штатно.

## 7. Починка

Payload уже лежит в образе. Наполнить `/data/adb/magisk/` — этого достаточно,
чтобы `magisk_env()` прошёл:

```sh
mkdir -p /data/adb/magisk
cp -a /system/etc/init/magisk/* /data/adb/magisk/
chmod 0755 /data/adb/magisk/busybox /data/adb/magisk/magisk64 \
           /data/adb/magisk/magiskpolicy /data/adb/magisk/magiskinit
```

затем `rd restart` (НЕ `adb reboot` — он гасит телефон: контейнер `--privileged`
делит ядро с телефоном). `/data` ВМ персистентен (`data11/`), так что копия
переживёт пересоздание контейнера.

`busybox` — единственный файл, которого требует `magisk_env()`; `magiskpolicy`
копируется, если есть. `util_functions.sh` (нужен, чтобы заработал
`magisk --install-module`) лежит внутри `magisk.apk` — отдельный необязательный шаг.

**Правильнее чинить образ**, а не живой контейнер: в `build/magisk/system/etc/init/bootanim.rc`
перед `exec … /sbin/magisk --auto-selinux --post-fs-data` добавить ту же копию
(или `COPY` наполненный `/data/adb/magisk` — но `/data` в образ не кладётся,
он bind-mount, так что вариант с rc предпочтительнее).

### Критерии успеха (проверять все три)

1. `grep -a "exec \[" /cache/magisk.log` содержит `<id>: exec [post-fs-data.sh]`;
2. появился `/data/adb/unfuse_zygisk.state/`;
3. `tools/vold-fusefs --check` → **rc=0**.

Дополнительно (Zygisk-плечо): в логе появляются строки про zygisk,
`/sbin/.magisk/zygisk/` существует, `app_process64` отличается по inode от
mirror'а, а `/data/adb/unfuse_zygisk.state/once` возникает при запуске
storage-приложения.

## 8. Что это даёт для mido

На mido скрипты **выполнялись** (в логе есть `unfuse_zygisk: exec [post-fs-data.sh]`,
`post-fs-data.sh` занял 14 мс) — значит слои 1 и 2 там ни при чём. Раз на
чистом AOSP-vold модуль отрабатывает полностью (§5), оставшаяся версия для mido —
патч на **crDroid-овском** vold либо его эффективность в реальном порядке загрузки.
Это подтверждает план из `docs/android-11-crdroid-log.md` §3: нужен код возврата
`vold-fusefs --check` от пользователя.
