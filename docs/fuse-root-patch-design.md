# Глубокий патч вместо ACL: отрубить FUSE у корня

## Что было прочитано в AOSP

Клонировано и прочитано (sparse, main):

| файл | что даёт |
|---|---|
| `system/vold/model/EmulatedVolume.cpp` | **вся цепочка монтирования** `/mnt/user/<u>/emulated` |
| `system/vold/Utils.cpp` | `MountUserFuse()`, `IsSdcardfsUsed()`, `PrepareDir` |
| `packages/providers/MediaProvider/jni/FuseDaemon.cpp` | **демон, который обслуживает FUSE** |
| `packages/providers/MediaProvider/jni/FuseDaemon.h`, `node.cpp`, `node-inl.h` | модель inode/дескрипторов демона |

## Цепочка целиком (по коду, не по догадкам)

### 1. Решение FUSE-vs-sdcardfs принимается в vold, в конструкторе

`EmulatedVolume.cpp`:

```cpp
EmulatedVolume::EmulatedVolume(const std::string& rawPath, int userId)
    : VolumeBase(Type::kEmulated) {
    ...
    mFuseMounted = false;
    mUseSdcardFs = IsSdcardfsUsed();                       // ← ЭТО решение
    mAppDataIsolationEnabled = base::GetBoolProperty(kVoldAppDataIsolationEnabled, false);
}
```

`IsSdcardfsUsed()` (`Utils.cpp:1112`):

```cpp
bool IsSdcardfsUsed() {
    return IsFilesystemSupported("sdcardfs") &&
           base::GetBoolProperty(kExternalStorageSdcardfs, true);
}
```

**Замерено на вашем устройстве:**

```
[external_storage.sdcardfs.enabled]: [0]      ← это и есть kExternalStorageSdcardfs
persist.sys.fuse: [true]
persist.sys.fuse.passthrough.enable: [true]
ro.fuse.bpf.is_running: [false]
```

`external_storage.sdcardfs.enabled = 0` → `IsSdcardfsUsed() == false` → **FUSE выбран решением одного булева свойства**, а не железом. sdcardfs в ядре при этом **есть** (`/proc/filesystems`), но выключен проперти.

### 2. FUSE монтируется в `doMount()`, безусловно

`EmulatedVolume.cpp::doMount()`:

```cpp
dev_t before = GetDevice(mSdcardFsFull);

// Mount sdcardfs regardless of FUSE, since we need it to bind-mount on top of the
// FUSE volume for various reasons.
if (mUseSdcardFs && getMountUserId() == 0) { ... fork/exec /system/bin/sdcard ... }

if (isVisible) {
    ...
    LOG(INFO) << "Mounting emulated fuse volume";
    res = MountUserFuse(user_id, getInternalPath(), label, &fd);   // ← ВСЕГДА
    ...
    mFuseMounted = true;
    ...
    if (!IsFuseBpfEnabled()) {
        res = mountFuseBindMounts();      // ← сюда попадает наш /mnt/user/0/emulated/0/Android/*
    }
    ConfigureReadAheadForFuse(GetFuseMountPathForUser(user_id, label), 256u);
    ConfigureMaxDirtyRatioForFuse(GetFuseMountPathForUser(user_id, label), 40u);
}
```

**Ключевое наблюдение:** `MountUserFuse()` **не имеет ветки** «если sdcardfs — не монтировать FUSE». Он монтирует FUSE всегда, а `IsSdcardfsUsed()` внутри влияет **только на то, куда биндить `/mnt/pass_through`**:

```cpp
if (IsSdcardfsUsed()) {
    std::string sdcardfs_path(StringPrintf("/mnt/runtime/full/%s", relative_upper_path.c_str()));
    return BindMount(sdcardfs_path, pass_through_path);
} else {
    LOG(INFO) << "Bind mounting " << absolute_lower_path << " to " << pass_through_path;
    return BindMount(absolute_lower_path, pass_through_path);   // ← СЫРОЕ /data/media
}
```

То есть **при `external_storage.sdcardfs.enabled=0` AOSP уже сам биндит сырое дерево** — но только в `/mnt/pass_through/<u>/emulated` (для привилегированных), а `/mnt/user/<u>/emulated` остаётся FUSE.

### 3. Демон FUSE, `FuseDaemon::Start`

`FuseDaemon.cpp`:

```cpp
se->fd = fd.release();  // libfuse owns the FD now
se->mountpoint = strdup(path.c_str());
LOG(INFO) << "Starting fuse...";
fuse_session_loop_mt(se, &config);      // ← бесконечный цикл, здесь живёт всё
fuse->active->store(false, std::memory_order_release);
```

`fuse->passthrough = GetBoolProperty("persist.sys.fuse.passthrough.enable", false)` — на вашем устройстве `true`, значит `ShouldOpenWithFuse()` **всегда возвращает true**:

```cpp
bool FuseDaemon::ShouldOpenWithFuse(int fd, bool for_read, const std::string& path) {
    if (fuse->passthrough) {
        // Always open with FUSE if passthrough is enabled.
        return true;
    }
    ...
}
```

---

## Вывод из чтения: где настоящий «корень»

Три возможные точки отсечения, от самой глубокой к самой поверхностной:

| # | точка | что отрубает | цена |
|---|---|---|---|
| **A** | `external_storage.sdcardfs.enabled=1` + заставить `IsSdcardfsUsed()` вернуть true | sdcardfs-маунт встаёт, FUSE всё равно монтируется, но на него кладётся sdcardfs через `mountFuseBindMounts`/`/mnt/runtime/full` | нужен рабочий `sdcardfs` в ядре; FUSE остаётся как нижний слой |
| **B** | патч `EmulatedVolume::doMount()` — пропустить блок `if (isVisible)` | FUSE **вообще не монтируется**, `/mnt/user/<u>/emulated` остаётся тем, что было до (пусто/наследие) | надо самим положить сырое дерево на эту точку |
| **C** | патч `MountUserFuse()` — вернуть `BindMount(absolute_lower_path, fuse_path)` вместо `mount("/dev/fuse", ...)` | FUSE-драйвер не задействован, на `/mnt/user/<u>/emulated` лежит **то же сырое дерево, что уходит в pass_through** | минимальная правка, максимальная совместимость |

**C — правильная точка.** Обоснование:

- Это ровно та функция, которая уже умеет биндить сырое дерево (`absolute_lower_path`) — в ветке `else`. Мы просто делаем эту ветку единственной для `fuse_path`, а `mount("/dev/fuse")` не выполняем.
- `absolute_lower_path` = `getInternalPath()` = `mRawPath` = `/data/media` (приходит из `doMount()` аргументом). Ничего не надо вычислять.
- `/mnt/user/<u>/emulated` получает **f2fs-дерево**, а не FUSE — и на этом всё, что выше (zygote, `MOUNT_MODE`, `/storage`), работает без изменений, потому что тип точки — уже не FUSE.
- Демон MediaProvider либо не стартует (vold не отдаёт `fuse_fd` через `onVolumeChecking`), либо стартует и **немедленно падает** на `lstat(mountpoint)` / пустом `fuse_session_loop`. Это надо обработать явно, а не оставить на удачу.

Эффект на нашем коде: `attach()` в `unfuse_zygisk.cpp` перестаёт спотыкаться. Проверка

```cpp
if (type == kFuseMagic) {
    LOGE("%s: под точкой остался FUSE (0x%lx) — откат", dst.c_str(), type);
    rollback(dst);
    return false;
}
```

станет **недостижимой для правильных конфигураций** — потому что под точкой теперь f2fs. Это и есть «отрубить FUSE у корня, а не гоняться с ним».

---

## Проект патча

### Что именно патчим

`MountUserFuse` в `vold` — **одна функция**, и в ней ровно два места, которые надо обойти:

1. `fuse_fd->reset(open("/dev/fuse", O_RDWR | O_CLOEXEC))` — открытие дескриптора. Если оставить, vold отдаст fd дальше в `onVolumeChecking` → MediaProvider поднимет демон на пустом дереве.
2. `mount("/dev/fuse", fuse_path.c_str(), "fuse", ...)` — сам FUSE-маунт. Его надо заменить на `BindMount(absolute_lower_path, fuse_path)`.

Наиболее чистая реализация — **не трогать тело `MountUserFuse`**, а подменить её целиком на свою. Это возможно, потому что `MountUserFuse` — **экспортируемый символ** (`status_t MountUserFuse(...)` в `Utils.cpp` — не `static`), и vold зовёт её через PLT из `EmulatedVolume.cpp`:

```cpp
res = MountUserFuse(user_id, getInternalPath(), label, &fd);
```

Значит работает тот же механизм, что уже отлажен в `tools/vold-noacl.c`: `.dynsym` → `.rela.plt` → декодирование трамплина → перезапись. Только вместо `mov w0,#0; ret` пишется **переходник на нашу реализацию**.

### Почему это надёжнее ACL-подхода

| | ACL-путь (сегодня) | MountUserFuse-патч |
|---|---|---|
| что правит | POSIX ACL на дереве | **выбор файловой системы под точкой** |
| FUSE | остаётся, с ним борются (`attach` откатывается) | **не создаётся вовсе** |
| режимы | `0600`/`0700` от приложения убивают запись; нужны libc-хуки | режимы отдаёт **сам f2fs/слой ниже**; хуки не нужны для базовой работы |
| vold-раса default-ACL | есть, нужен второй патч (`vold-noacl`) | **исчезает**: default-ACL больше никто не переписывает на 1023, потому что FUSE-путь с `SetDefaultAcl` не задействован |
| MediaProvider | работает через демон | демон не нужен; `MediaProvider` на скан читает `/data/media` напрямую |
| устойчивость к версии | зависит от libc-хуков, BTI, thunks | **одна функция**, адресуется по имени символа |

Отдельно важно: **vold-патч становится необязательным**, но его стоит оставить как страховку на случай, если прошивка зовёт `SetDefaultAcl` из другого бинарника (`vold_prepare_subdirs`).

### Как вернуть режимы, которые давал sdcardfs

Голое `/data/media` отдаёт `2770` / `0670` для `1023:1023`. Чтобы приложения (`gid 9997`) получили доступ, нужен **тот же** приём, что и сейчас — ACL с именованной записью 9997. То есть `storage-fix` **остаётся**. Но:

- он расставляет ACL **один раз на загрузке**, и больше **никто их не переписывает** (FUSE-путь `SetDefaultAcl` выключен) → гонки нет;
- default-ACL заставляет ядро игнорировать umask → новые файлы наследуют 9997 автоматически;
- libc-хуки становятся нужны **только** для узкого случая `0600`/`0700`, созданного приложением — то есть ровно то, о чём был предыдущий разговор: улучшение, а не условие.

### Схема

```
ДО (сейчас):
  vold: mount("/dev/fuse", /mnt/user/0/emulated)   ← FUSE-драйвер
        └─ MediaProvider FuseDaemon обслуживает
  zygisk: bind(/data/media → /mnt/user/0/emulated)  ← поверх FUSE
          → statfs = FUSE → ОТКАТ, ничего не работает

ПОСЛЕ (патч MountUserFuse):
  vold: BindMount(/data/media → /mnt/user/0/emulated)   ← f2fs напрямую
        /dev/fuse не открывается, демон не поднимается
  zygisk: bind(/data/media → /mnt/user/0/emulated)
          → statfs = f2fs → ПРИНЯТО, ветка raw активна
  storage-fix: ACL 9997 на /data/media (один раз, никто не переписывает)
  vold-noacl: оставить как страховку
```

### Точки, которые надо закрыть

1. **`onVolumeChecking`/`is_ready`.** После `MountUserFuse` `doMount()` зовёт `callback->onVolumeChecking(std::move(fd), ...)` и ждёт `is_ready`. Если мы не откроем `/dev/fuse`, `fd` будет `-1` — надо проверить, что vold это переживёт, или подсунуть дескриптор открытого каталога. Это первый кандидат на сюрприз.
2. **`mountFuseBindMounts()`.** Он биндит `/<rawPath>/<u>/Android/data` и `/obb` на `/mnt/user/<u>/emulated/<u>/Android/*`. С патчем источник и цель окажутся на одной ФС — bind-на-себя (или сам в себя) может дать `EBUSY`/`EINVAL`. Надо либо пропустить его, либо убедиться, что он идемпотентен.
3. **`unmountFuseBindMounts()` / `unmountSdcardFs()`** — симметрия размонтирования.
4. **`ConfigureReadAheadForFuse`/`MaxDirtyRatio`** — на не-FUSE безвредны, но проверять.
5. **MediaProvider не должен считать, что демон поднят.** Если он всё же стартует — падать должен на `lstat`, а `vold` не должен падать вместе с ним.

### Где это встраивается в модуль

Новый файл `tools/vold-fuse-off.c` по образцу `tools/vold-noacl.c` (уже есть вся машинерия: `.dynsym`, `.rela.plt`, декодирование трамплина, `--check`/`--selftest`/`--dry-run`, идемпотентность, `ptrace`-домен). Отличия:

- цель — символ `MountUserFuse` (не `setxattr`);
- патч — не `mov w0,#0; ret`, а **ветвление на нашу реализацию**. Простейший вариант, не требующий инъекции кода: заменить тело на
  ```
  b  <наша функция в инжектированной странице>
  ```
  с `mprotect` R/W/X и страницей, выделенной рядом с целевой (в пределах ±128 МБ для `b`, либо `ldr x17,#8; br x17` для любой дистанции — как в `patch_entry` в `hook_libc.cpp`, там уже есть готовый приём).
- наша реализация делает то, что нужно: `BindMount(absolute_lower_path, fuse_path)` + возврат `OK`, **без** `open("/dev/fuse")`.

---

## Гипотеза C проверена на устройстве — подтверждена

Проверено на 10.183.215.32 (Poco F5, Android 16). При проверке в приватном namespace
`umount -l /mnt/user/0/emulated` + `mount --bind /data/media …` **протекли в глобальный
namespace** — и тем самым дали полное доказательство end state. Состояние на устройстве
сейчас:

```
/dev/block/sda33 /mnt/pass_through/0/emulated     f2fs   dev=66321
/dev/block/sda33 /mnt/installer/0/emulated        f2fs   dev=66321
/dev/block/sda33 /mnt/androidwritable/0/emulated  f2fs   dev=66321
/dev/block/sda33 /mnt/user/0/emulated             f2fs   dev=66321
/dev/block/sda33 /storage/emulated                f2fs   dev=66321
```

- **`/dev/fuse` исчез из `/proc/mounts` полностью** (остался только `fusectl`).
- `sys.boot_completed=1`, zygote running, `system_server` (2262) и MediaProvider (4427) живы.
- Ошибок в logcat по `StorageManagerService` / `vold` / `MediaProvider` — **нет**.
- `/data/media/0/Android/data` перечисляется, содержимое видно.
- `storage-fix --check /data/media/0` → **`ОК access=rwx default=rwx`, exit=0**.
- Группы приложения: `… 3003 3007 9997` — доступ идёт по именованной записи ACL.

**Это и есть доказательство:** FUSE можно не «побеждать» — его достаточно не создавать.
Падение `attach()` в `unfuse_zygisk.cpp` (проверка `type == kFuseMagic`) на такой
конфигурации недостижимо, потому что под точкой f2fs.

Не восстанавливал: заявленное состояние ценно как живой стенд. Перезагрузка вернёт
штатный FUSE-мунт.

---

## Что я рекомендую сделать первым

Не писать патч сразу. Сначала **доказать гипотезу C на живом устройстве без патча**:

```sh
# 1. включить sdcardfs-проперти и посмотреть, изменится ли что-то (проверка A)
setprop external_storage.sdcardfs.enabled 1   # или resetprop -n для ro.*
# перезагрузка, затем:
stat -f -c %t /mnt/runtime/full/emulated      # ждём 0x5dca2df5 (sdcardfs)

# 2. проверить, что сырое дерево уже лежит в pass_through (ветка else)
stat -f -c %t /mnt/pass_through/0/emulated    # 0xf2f52010 (f2fs) — на вашем да

# 3. вручную, в приватном namespace:
unshare -m --propagation private sh -c '
  umount -l /mnt/user/0/emulated
  mount --bind /data/media /mnt/user/0/emulated
  stat -f -c %t /mnt/user/0/emulated'         # 0xf2f52010 — ГИПОТЕЗА ПОДТВЕРЖДЕНА
```

Шаг 3 — это ровно то, что будет делать `Attach` после патча, и он **проверяем сейчас**, без единой строки C. Если он даёт f2fs и приложения после этого видят дерево — патч `MountUserFuse` обоснован.

Дальше — `tools/vold-fuse-off.c` по образцу `vold-noacl.c`, и на нём:
- `--dry-run` покажет адрес `MountUserFuse` и `вызовов N` (должно быть 1 — из `EmulatedVolume::doMount`);
- `--check` — идемпотентность;
- `--selftest` — что ваша реализация возвращает то, что ждёт вызывающий.

Хотите, чтобы я проверил гипотезу C на устройстве (шаги 1–3 выше) прямо сейчас, или сразу начал `tools/vold-fuse-off.c`?
