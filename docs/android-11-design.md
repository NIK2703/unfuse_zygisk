# Android 11: чем отличается и что с этим делать

> Разбор по исходникам AOSP, а не по догадкам. Повод — отзыв с A11 crDroid  
> («установилось, но не работает»). Итог одной строкой: **на A11 форма хранилища  
> выбирается булевым свойством `persist.sys.fuse`, и при `false` модуль не делает  
> ничего.** Это свойство **только Android 11** — с 12 по 17 ветки нет, там править  
> нечего. Исправление внесено в `module/post-fs-data.sh`.

## Что скачано и как повторить

| репозиторий                                 | тег                  | что читалось                                                                             |
| ------------------------------------------- | -------------------- | ---------------------------------------------------------------------------------------- |
| `platform/system/vold`                      | `android-11.0.0_r48` | `Utils.cpp`, `model/EmulatedVolume.cpp`, `model/PublicVolume.cpp`, `VolumeManager.cpp`   |
| `platform/bionic`                           | `android-11.0.0_r48` | `libc/bionic/{open,rename,mkdir,link,fchmod,fchmodat}.cpp`, `libc/SYSCALLS.TXT`          |
| `platform/system/core`                      | `android-11.0.0_r48` | `base/properties.cpp` (это и есть `libbase` в 11), `libcutils/fs.cpp`, `rootdir/init.rc` |
| `platform/frameworks/base`                  | `android-11.0.0_r48` | `core/jni/com_android_internal_os_Zygote.cpp`                                            |
| `platform/packages/providers/MediaProvider` | `android-11.0.0_r48` | `jni/FuseDaemon.cpp`                                                                     |

Локально — `E:\projects\aosp\vold-11`, `bionic-11`, `core-11`, `mp-11`, `fwb11/Zygote.cpp`.  
Плюс **все релизы сразу**: `E:\projects\aosp\byver\a30..a37` — целевые файлы, скачанные  
скриптом `E:\projects\aosp\fetch-targets.sh` (`EmulatedVolume.cpp`, `vold/Utils.cpp`,  
`VolumeManager.cpp`, `PublicVolume.cpp`, `Zygote.cpp`, `libc/bionic/open.cpp`,  
`libc/bionic/rename.cpp`, `libc/SYSCALLS.TXT`, `libcutils/fs.cpp`, `property_contexts`)  
на тегах `android-{11.0.0_r48, 12.0.0_r34, 12.1.0_r27, 13.0.0_r17, 14.0.0_r15,
15.0.0_r9, 16.0.0_r4, 17.0.0_r1}`. Ссылки в тексте — на эти деревья.

## Отличие ровно одно, и оно в vold и в Zygote сразу

### vold: `EmulatedVolume::doMount`

A11 (`model/EmulatedVolume.cpp:304`, `:353`):

```cpp
    bool isFuse = base::GetBoolProperty(kPropFuse, false);   // persist.sys.fuse
    ...
    if (isFuse && isVisible) {
        res = MountUserFuse(user_id, getInternalPath(), label, &fd);   // :373
```

A16 (`a16/EmulatedVolume16.cpp:425`):

```cpp
    if (isVisible) {
        res = MountUserFuse(user_id, getInternalPath(), label, &fd);   // :445
```

Строки `isFuse` в A16 нет вообще. Между этими двумя версиями изменилось **только условие**:  
тело `MountUserFuse()` и его `mount("/dev/fuse", …, "fuse", … MS_LAZYTIME, …)` совпадают  
побайтно по смыслу (`vold-11/Utils.cpp:1560` против `a16/Utils16.cpp`), и `IsSdcardfsUsed()`  
по-прежнему выбирает лишь источник для `/mnt/pass_through` (`vold-11/Utils.cpp:1568-1577`).

### Zygote: `MountEmulatedStorage`

A11 (`fwb11/Zygote.cpp:834`, `:837-857`):

```cpp
  bool isFuse = GetBoolProperty(kPropFuse, false);
  ...
  if (isFuse) {
    ...
    } else {
      BindMount(user_source, "/storage", fail_fn);          // /mnt/user/<u>
    }
  } else {
    const std::string& storage_source = ExternalStorageViews[mount_mode];
    BindMount(storage_source, "/storage", fail_fn);         // /mnt/runtime/<view>
    BindMount(user_source, "/storage/self", fail_fn);
  }
```

`ExternalStorageViews` (`fwb11/Zygote.cpp:331-338`) — это `default / read / write / write /
write / full / full / full`. В A16 от массива и от ветки не осталось ничего: `BindMount(user_source, "/storage")`  
безусловно.

### Свойство и его значение по умолчанию

`vold-11/Utils.h:37` — `kPropFuse = "persist.sys.fuse"`. Читается `GetBoolProperty(…, false)`  
**в двух местах, которые решают всё** (`EmulatedVolume.cpp:304`, `Zygote.cpp:834`), и ещё в  
четырёх второстепенных (`VolumeManager.cpp:383/477/732/887`, `PublicVolume.cpp:230`).

**Значение по умолчанию — `false`.** Если прошивка не выставляет свойство явно, A11 идёт  
по sdcardfs-пути: vold форкает `/system/bin/sdcard` (`EmulatedVolume.cpp:308-351`) и тот  
монтирует sdcardfs на `/mnt/runtime/{default,read,write,full}/emulated`, а Zygote биндит  
`/mnt/runtime/<view>` на `/storage`.

### Только A11 — и нигде больше

Прогнано по всем восьми релизам (`byver/a30..a37`, скачивание — `fetch-targets.sh`):

| SDK | релиз | vold: `isFuse` | Zygote: `ExternalStorageViews` | `mount("/dev/fuse"` | `IsSdcardfsUsed` | `setxattr` | `renameat` стаб | `rename.cpp` |
| --- | ----- | -------------- | ------------------------------ | ------------------- | ---------------- | ---------- | --------------- | ------------ |
| 30  | 11    | **есть**       | **есть**                       | 1                   | 1                | 192        | 1               | 1556         |
| 31  | 12    | нет            | нет                            | 1                   | 1                | 192        | 1               | 1556         |
| 32  | 12L   | нет            | нет                            | 1                   | 1                | 192        | 1               | 1556         |
| 33  | 13    | нет            | нет                            | 1                   | 1                | 192        | 1               | 1556         |
| 34  | 14    | нет            | нет                            | 1                   | 1                | 195        | 0               | 1721         |
| 35  | 15    | нет            | нет                            | 1                   | 1                | 195        | 0               | 1721         |
| 36  | 16    | нет            | нет                            | 1                   | 1                | 195        | 0               | 1721         |
| 37  | 17    | нет            | нет                            | 1                   | 1                | 196        | 0               | 1721         |

**Вывод: с 12 исправления не нужны ни на одном релизе.** Единственная форма хранилища  
там — FUSE, `MountUserFuse()` вызывается безусловно, площадка `mount("/dev/fuse", …,
MS_LAZYTIME)` есть во всех восьми. Две дельты из таблицы уже смоделированы в  
`src/android_ver.h`: `setxattr` 192/195/196 — это `vold-noacl`, `renameat` стаб до 13 и  
обёртка с 14 — это колонки `installed`/`installed_arm`. `open.cpp` побайтно одинаков во  
всех восьми, `__openat:openat` есть во всех, `libcutils/fs.cpp` тоже одинаков.

Это уточняет прежнюю запись в `MEMORY.md`: **pre-FUSE-форма — свойство Android 11, а не 11–13.**

### Почему свойство, а не что-то ещё

`init.rc:792` (внутри `on post-fs-data`):

```
    # Enable FUSE by default
    setprop persist.sys.fuse true

# Switch between sdcardfs and FUSE depending on persist property
on zygote-start && property:persist.sys.fuse=true
  mount none /mnt/user/0 /storage bind rec
  mount none none /storage slave rec
on zygote-start && property:persist.sys.fuse=false
  mount none /mnt/runtime/default /storage bind rec
  mount none none /storage slave rec
```

Два следствия, и оба решают выбор:

1. **AOSP сам включает FUSE на 11** — этой самой строкой, в `on post-fs-data`. Устройство,  
   где свойство `false`, эту строку сняло. Выставить её обратно — не своя политика, а  
   восстановление значения AOSP по умолчанию.
2. **`init` биндит `/storage` в корневом namespace под `zygote-start`**, читая свойство в  
   условии триггера. Значит свойство обязано стоять до `zygote-start`, и `post-fs-data` —  
   единственное для этого место. Заодно снимается вопрос порядка: под `zygote-start`  
   сработает **своя же** ветка `=true`, и `/storage` соберётся правильно без нашей строки.

### Что внесено

`module/post-fs-data.sh` — на SDK 30, если `persist.sys.fuse` не `true`:  
`resetprop -n persist.sys.fuse true` (без `resetprop` — `setprop`) плюс  
`external_storage.sdcardfs.enabled 0`. `-n` не персистит значение: без модуля устройство  
возвращается к тому, что поставил вендор. На остальных SDK блок не выполняется — там это  
свойство не читает никто.

`module/customize.sh` — на SDK 30 сообщает, какая форма найдена и что модуль с ней сделает.  
Отказом не сделано: модуль форму **исправляет**, а не отказывается работать.

**Почему не bind `/mnt/runtime/*/emulated`** (вариант 2 из первого разбора). Он не трогает  
устройство, но упирается в порядок, который на 16 не проверить: bind обязан лежать до форка  
приложения (Zygote биндит `/storage` через `MS_BIND|MS_REC`, то есть снимок), а `service.sh`  
может оказаться позже первого приложения. Плюс `fs_prepare_dir()` сделает `lstat` на точке,  
увидит корень монтирования и уйдёт в `fixup` — `chown`/`chmod` на `/data/media`. Вариант  
оставлен как запасной: пригодится, если на конкретной прошивке свойство выставить не удастся.

## Что это значит для модуля

`unfuse` перехватывает `mount("/dev/fuse", …, "fuse", …, MS_LAZYTIME)` — вызов, который  
делает `MountUserFuse()`. На A11 при `persist.sys.fuse=false`:

- `MountUserFuse()` не вызывается → перехватчик не срабатывает ни разу;
- вид приложения собирается Zygote из `/mnt/runtime/<view>` — место sdcardfs  
  (`EmulatedVolume.cpp:286-289`), куда модуль ничего не кладёт (плечо снято 2026-10-07);
- итог — модуль встаёт, грузится, не делает ничего. Ровно «установилось, но не работает».

При `persist.sys.fuse=true` на A11 всё совпадает с 14+: `MountUserFuse` вызывается,  
перехватчик срабатывает, Zygote биндит `/mnt/user/<u>`. **То есть модуль на A11 работает —  
но только на одной из двух его форм.**

## libc: править нечего

| что                                          | A11                                       | A16                              | следствие                                                                          |
| -------------------------------------------- | ----------------------------------------- | -------------------------------- | ---------------------------------------------------------------------------------- |
| `libc/bionic/open.cpp`                       | —                                         | —                                | **файлы идентичны** (различие в diff — только CR от `autocrlf`)                    |
| `__openat`                                   | `SYSCALLS.TXT:145` `__openat:openat(...)` | `SYSCALLS.TXT:161` то же         | стаб есть в обоих, поиск по форме верен                                            |
| `renameat`                                   | `SYSCALLS.TXT:155` — настоящий стаб       | в `SYSCALLS.TXT` **отсутствует** | на A11 это корень (24 байта), на 14+ — обёртка в `rename.cpp`, зовущая `renameat2` |
| `mkdirat`, `linkat`, `renameat2`, `unlinkat` | стабы                                     | стабы                            | без изменений                                                                      |

`renameat` — единственная разница, и она **уже смоделирована** двумя колонками  
(`installed` / `installed_arm`) в `src/android_ver.h`. Статическая проверка по эталонному  
образу `device/libc/libc-arm64-a11.so` даёт 9/9 и не расходится с этой картиной.  
**Вывод: libc для A11 чинить не нужно; вся проблема A11 живёт в vold и Zygote.**

## Варианты, которые не выбраны

Оставлены здесь как запас: если на конкретной прошивке `persist.sys.fuse` выставить не  
удастся (SELinux не пустит запись в `system_prop`, см. `property_contexts:68`), работа  
продолжается с них.

### 1. Нормализовать свойства — **выбрано**, внесено в `post-fs-data.sh`

```sh
# post-fs-data.sh, до первого чтения vold и Zygote
setprop persist.sys.fuse true
setprop external_storage.sdcardfs.enabled 0
```

Тогда A11 принимает ровно ту форму, под которую модуль собран и проверен на 16:  
`isFuse=true` в обоих местах, `MountUserFuse()` вызывается, `mUseSdcardFs=false` —  
`/system/bin/sdcard` не форкается, а `mountFuseBindMounts()` берёт `Android/*` из  
`mRawPath`, а не из sdcardfs (`EmulatedVolume.cpp:112-116`).

Цена: пишем `persist.`-свойство (может лечь в `/data/property`), нужен `set_prop` в  
SELinux-домене модуля, и меняется поведение **всего** хранилища, включая съёмное  
(`PublicVolume.cpp:230`) и четыре площадки в `VolumeManager.cpp`. Если модуль убрать,  
устройство останется на FUSE-форме — форма штатная, но это уже не «ничего не меняли».

### 2. Bind рантайм-видов (без пропертей и без sdcardfs)

На pre-FUSE-форме вид приложения — это `/mnt/runtime/<view>`. Значит, достаточно положить  
сырое дерево туда — тогда sdcardfs не нужен вовсе:


```sh
for v in default read write full; do
    p=/mnt/runtime/$v/emulated
    [ -d "$p" ] || mkdir -p "$p"
    [ "$(stat -f -c %t "$p")" = f2f52010 ] || mount --bind /data/media "$p"
done
```

Два порядка, оба обязательны:

- **после vold.** `fs_prepare_dir()` (`core-11/libcutils/fs.cpp`, `fs_prepare_path_impl`)  
  делает `lstat`, и на точке монтирования `lstat` возвращает атрибуты **корня монтирования**.  
  Владелец/режим не сойдутся (`/data/media` — `1023:1023 2770`, ждут `root:root 0700`) →  
  ветка `fixup` → **chown/chmod на `/data/media`**. Поэтому bind из `post-fs-data.sh`  
  будет снесён и попортит дерево до повторного `storage.sh` из `service.sh`. Тот же  
  грабль уже знаком по sdcardfs-варианту («vold resets owner and mode of /data/media…»).
- **до первого приложения.** `BindMount()` в Zygote — это `MS_BIND|MS_REC`  
  (`fwb11/Zygote.cpp`), то есть снимок поддерева на момент форка: более поздний маунт  
  на источнике под `/storage` приложения уже не появится.

Что при этом получается бесплатно: `mountFuseBindMounts()` на этой форме не вызывается  
(он внутри `if (isFuse && isVisible)`), значит Android/data и Android/obb не изолируются —  
ровно то, что модуль обещает. Доступ даёт та же именованная запись ACL для 9997, что уже  
ставит `storage-fix`; отдельной работы под A11 не нужно.

Цена: новая стадия в `service.sh` и необходимость **замерить порядок** (см. ниже). Побочный  
плюс: вариант покрывает и третью конфигурацию A11 — `persist.sys.fuse=false` вместе с  
`sdcardfs=0`, где платформа не монтирует **ничего** и `/mnt/runtime/*/emulated` остаются  
пустыми каталогами `0700 root:root`.

### 3. Перехватить чтение свойства (самый хирургический)

`base::GetProperty` в A11 (`core-11/base/properties.cpp`) устроен так:

```cpp
  const prop_info* pi = __system_property_find(key.c_str());
  if (pi == nullptr) return default_value;
  __system_property_read_callback(pi, [](void* cookie, const char*, const char* value, unsigned) {…}, &property_value);
```

Оба символа импортируются libc через PLT — то есть достижимы тем же механизмом, что уже  
отлажен на `mount`. Перехватив `__system_property_read_callback` и подменив `value` для  
ключа `persist.sys.fuse`, получаем `isFuse=true` и в vold, и в Zygote, **не трогая  
состояние устройства**. В Zygote это надо успеть до `MountEmulatedStorage` — и это  
выполнимо: `preAppSpecialize` уже обязан идти раньше (модуль на этом порядке держится,  
когда правит `args->mount_storage_dirs`).

Цена: это **третий** патч vold (правило «оба патча ставятся вместе или ни один» станет  
«три вместе»), плюс обёртка вокруг чужого колбэка в инжектированной странице. Заметно  
дороже варианта 2 при том же результате.

## Что нужно замерить на живом A11

Выбранный вариант (свойство) порядка не боится — он опирается на триггеры самого `init`.  
Проверить нужно другое:

1. Пускает ли SELinux запись в `persist.sys.fuse`. Контекст — `system_prop`  
   (`property_contexts:68`, правило `persist.sys.`), а домен модуля зависит от менеджера:  
   Magisk (`resetprop` есть, `-n` не персистит) против KernelSU (только `setprop`).
2. Что показывает `getprop persist.sys.fuse` после загрузки и какая ветка `init.rc`  
   отработала: `/storage` в корневом namespace должен быть `/mnt/user/0`, а не  
   `/mnt/runtime/default`.
3. `stat -f -c %t /mnt/user/0/emulated` → `0xf2f52010` (сырое дерево на месте), и  
   `vold-fusefs --check` → rc 0.

Если запись не пройдёт, модуль на такой прошивке по-прежнему не сделает ничего —  
тогда переходить на вариант 2, и его замеры такие:

1. Когда именно vold создаёт `/mnt/runtime/*/emulated` относительно `service.sh`  
   (то есть успевает ли bind до первого приложения со storage).
2. Уходит ли `fs_prepare_dir` с точки после того, как там уже лежит наш bind, — и  
   восстанавливает ли `storage.sh` режимы `/data/media` после `fixup`.
3. Что показывает `stat -f -c %t` на всех четырёх точках после bind (ждать `0xf2f52010`).

Чтобы вообще отличить формы, от пользователя нужны те же пять чисел, что уже запрошены:  
`persist.sys.fuse`, `external_storage.sdcardfs.enabled`, `ro.build.version.sdk`,  
`grep -c sdcardfs /proc/filesystems` и `stat -f -c %t` по пяти путям.

## Галереи — отдельная ось, этот разбор её не закрывает

«Почти все галереи не работают» не объясняется формой хранилища. Это цена посылки модуля  
(«нет MediaProvider в пути данных») в паре с моделью доступа к сырому дереву: доступ даёт  
одна именованная запись ACL для 9997 плюс libc-хуки, формирующие режим до сисколла, и там,  
где записи нет или маска обнулена (`posix_acl_create_masq` на `0600`), **листинг работает,  
а открытие — нет**. Галерея — это «листинг плюс открыть каждый файл», файловый менеджер —  
почти только листинг; отсюда и наблюдаемое «галереи не работают, а с файлами явно быстрее».  
Разбирать это надо на A11 отдельно и с живым устройством в руках.
