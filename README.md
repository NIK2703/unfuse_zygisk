# sdcardfs Restore

Zygisk-модуль, который возвращает внутренней памяти **поведение Android 10 и
более ранних версий**: **sdcardfs** вместо **FUSE** на Android 16 (Xiaomi marble /
POCO F5) — с **полным доступом ко всей памяти для ВСЕХ приложений без исключений**,
включая корень `/storage/emulated`, `Android/data`, `Android/obb` и каталоги чужих
пакетов. Никакого scoped storage и никакого списка приложений.

Это реализация **варианта B** из `../aosp-ref/DESIGN-zygisk-sdcardfs.md`.

---

## 1. Что делает модуль в двух словах

Ничего не пересобирает и никуда не патчит system. Он использует то, что на
Android 16 **уже есть**:

* ядро умеет `sdcardfs` (`CONFIG_SDCARD_FS=y`);
* vold монтирует sdcardfs в четыре точки `/mnt/runtime/{default,read,write,full}/emulated`;
* Zygote **уже** биндит `/mnt/user/<user>` на `/storage` рекурсивно.

Не хватает ровно одного: приложениям выдаётся **FUSE**-ветка (`/mnt/user/0/emulated`),
а не sdcardfs. Модуль подменяет эту ветку **в приватном mount namespace процесса** —
ровно перед тем, как Zygote сделает бинд на `/storage`.

Дополнительно модуль:

* снимает tmpfs-обвязку с `Android/data` и `Android/obb` (`!android_dirs=raw`);
* перемаркирует **корень** `/data/media` из `media_userdir_file` в
  `media_rw_data_file` (`!relabel=1`), иначе `stat()`/`ls()` самого
  `/storage/emulated` отдают `EACCES` (см. §3).

---

## 2. Механика (по шагам)

```
0. post-fs-data.sh  ── relabel-media.sh:  chcon /data/media -> media_rw_data_file
                       relax-storage.sh:  /mnt/runtime 0700->0711, /data/media 0550->0551
                       sdcardfs-bringup.sh: sdcardfs на /mnt/runtime/*/emulated
                       (всё это ДО старта Zygote, поэтому все приложения видят
                        уже верные ярлык и права)

1. vold  ── FUSE на /mnt/user/0/emulated                                [уже есть в A16]

2. Zygisk ── preAppSpecialize() в УЖЕ форкнутом ребёнке
             uid=0, домен u:r:zygote:s0, /storage ещё не подменён

3. модуль ── unshare(CLONE_NEWNS)          ← приватная копия таблицы монтирования
             mount("/mnt/runtime/full/emulated",
                   "/mnt/user/0/emulated",
                   NULL, MS_BIND | MS_REC, NULL)   ← биндим ПОВЕРХ FUSE
             (если источника не видно — свой mount("sdcardfs") прямо на цель)
             проверка: statfs(цель) == 0x5dca2df5, иначе немедленный откат

4. Zygote ── MountEmulatedStorage() → BindMount("/mnt/user/0", "/storage", MS_BIND|MS_REC)
             рекурсивный бинд протаскивает sdcardfs на /storage/emulated

5. модуль ── *args->mount_storage_dirs = JNI_FALSE
             → Zygote НЕ вызывает BindMountStorageDirs() и не накрывает
               Android/{data,obb} временным tmpfs (при !android_dirs=raw)

6. service.sh ── повторяет relabel/relax/bringup после vold + пишет диагностику
```

Ключевой момент — шаг 3 выполняется **до** `DropCapabilitiesBoundingSet()`, то есть
процесс ещё root и имеет `CAP_SYS_ADMIN`, а `mount_external` ещё не применён.

---

## 3. Три причины, которые пришлось обойти

Это самая важная часть: причин было **две, и они разные**. Обе давали
`EACCES` **без единого AVC** в логе, из-за чего их легко принять за одну.

### 3.1. Права DAC — почему падал сам маунт

В момент `preAppSpecialize` процесс ещё не root в полном смысле: `/mnt/runtime`
имеет режим `0700 root:root`, `/data/media` — `0550 uid 1023` (`media_rw`).
Из-за этого **предварительный `stat()` по источнику бинда** отдавал `EACCES` —
отказ чисто файловый, SELinux ни при чём.

Что сделано:

* модуль больше **не делает `stat()` по источнику**; результат маунта
  проверяется через `statfs()` по цели;
* `post-fs-data.sh` и `service.sh` снимают лишний бит с `/mnt/runtime` и
  `/data/media` (директива `!relax=`);
* сам маунт дополнительно пробуется под подменённой личностью
  (`fsuid=1023 media_rw`, `fsgid=9997 everybody`) — это ровно владельцы
  `/data/media` и `/mnt/user/0`.

### 3.2. SELinux — почему отваливался КОРЕНЬ хранилища

`sdcardfs` не заводит собственный инод для корня точки монтирования:
`getattr()` корня **форвардится в НИЖНИЙ инод**, то есть в `/data/media`.
А ярлык корня особый:

```
private/file_contexts:633   /data/media     u:object_r:media_userdir_file:s0
private/file_contexts:634   /data/media/.*  u:object_r:media_rw_data_file:s0
```

и для `media_userdir_file` разрешено **ровно одно** право:

```
private/domain.te:252
allow { coredomain appdomain } media_userdir_file:dir search;
```

`search` пускает внутрь каталога, но **не даёт `getattr`**. Отсюда:

```
stat /mnt/runtime/full/emulated    -> Permission denied   (AVC нет: dontaudit)
stat /mnt/runtime/full/emulated/0  -> ok
```

Содержимое работало, потому что `/data/media/.*` помечен `media_rw_data_file`,
а на него у приложений права полные:

```
private/app.te:149
allow appdomain media_rw_data_file:dir create_dir_perms;
```

**Решение:** вернуть корню ярлык `media_rw_data_file` — ровно тот, что был у
`/data/media` на Android 10 и раньше (тогда существовало одно правило
`/data/media(/.*)? u:object_r:media_rw_data_file:s0`, а отдельный тип
`media_userdir_file` появился позже именно чтобы закрыть корень).

Делает это `module/relabel-media.sh` (`!relabel=1`), вызываемый из
`post-fs-data.sh` (до Zygote) и `service.sh` (после vold, который мог вернуть
исходный ярлык). Права DAC при этом **не меняются**: `/data/media` остаётся
`0551 uid 1023`, `/data/media/0` — `2770 uid/gid 1023`.

Почему `chcon`, а не `sepolicy.rule`: право на `media_rw_data_file` у `appdomain`
**уже есть**, патчить политику не нужно; а сделать `chcon` из самого модуля
нельзя — `relabelto` для домена `zygote` запрещён `neverallow`
(`domain.te:791`). Поэтому перемаркировка вынесена в скрипт модуля.

> Проверено на устройстве: до перемаркировки `stat /mnt/runtime/full/emulated`
> отдавал `Permission denied`, после — `555`, и в namespace приложения
> `/storage/emulated` тоже стал `555`, а `statfs` — `0x5dca2df5` (sdcardfs).

### 3.3. Почему системное приложение «Файлы» показывало `Android/data` пустым

Это **не** FUSE и **не** Java-фильтр. Причина — отдельные маунты **сырой ФС**
поверх FUSE, которые видны в таблице маунтов:

```
fuse   /storage/emulated
f2fs   /storage/emulated/0/Android/data    ← /dev/block/sda33, сырой f2fs
f2fs   /storage/emulated/0/Android/obb
```

их права:

```
/data/media/0/Android/data = 2771 uid 1023 gid 1023   ← остальным только --x
/data/media/0/Android/obb  = 2771 uid 1059 gid 1059
```

`--x` без `r` — войти в каталог можно, **листинг запрещён**. Эти маунты создаёт
vold глобально, а Zygote повторяет per-app (`BindMountStorageDirs`,
`mirrorAppDataPath → actualAppDataPath` в
`com_android_internal_os_Zygote.cpp:1046-1087`). Именно их отключает наш
`mount_storage_dirs = JNI_FALSE` — поэтому Termux и MiXplorer видят все пакеты.

Но **DocumentsUI сам файлы не читает**: он спрашивает провайдера
`com.android.externalstorage`, а тот запущен с

```
ProcessRecord{... :com.android.externalstorage/u0a300}
    mountMode=ANDROID_WRITABLE
```

Пока модуль обрабатывал только `DEFAULT`, провайдер оставался с mirror-маунтом
→ `EACCES` → «Файлы» рисовали «Ничего нет».

**Решение:** обрабатывать и `ANDROID_WRITABLE` (v2.4.0). Проверено живьём —
подкладка sdcardfs в namespace провайдера:

| | до | после |
|---|---|---|
| ФС `/storage/emulated` | `0x65735546` (fuse) | `0x5dca2df5` (**sdcardfs**) |
| `ls …/Android/data` | `Permission denied` | **51 пакет** |
| `ls …/Android/obb` | `Permission denied` | 2 |

после чего DocumentsUI отрисовал `POCO F5 > Android > data` со всеми пакетами.

---

## 4. Права, которые получает приложение

Драйвер считает `visible_mode = 0775 & ~mask`, а `gid` берёт из опции `gid=`.
Модуль монтирует с `mask=7` (десятичное, т.е. oct `0007`) и `gid=9997`
(`AID_EVERYBODY`):

| backend | mask | gid | каталоги | файлы |
|---|---|---|---|---|
| `read`  | 0027 | 9997 | `0750` | `0640` |
| `write` | 0007 | 9997 | `0770` | `0660` |
| `full`  | 0007 | 9997 | `0770` | `0660` |

`AID_EVERYBODY` есть у **любого** процесса (`Process.SHARED_USER_GID` в
`ProcessList.computeGidsForProcess`), а маска `0007` оставляет `0770` на каталоги
и `0660` на файлы. Итог: все процессы получают `rwx` на всё дерево
`/storage/emulated/0` — ровно то, что было в Android 9 и раньше, когда `/storage`
указывал на `/mnt/runtime/write`.

Модуль использует источники `full` → `write` (у обоих `gid 9997` и `mask 0007`).
Маунт `default` (`gid AID_SDCARD_RW = 1015`) приложениям бесполезен и **не
используется**.

---

## 5. `Android/data` и `Android/obb`

Штатно Zygote (`SpecializeCommon` → `BindMountStorageDirs`) накрывает
`/storage/emulated/<u>/Android/{data,obb}` временным tmpfs и биндит туда **только
каталоги своего пакета** — остальные пакеты не видны вообще.

Директива `!android_dirs=raw` (по умолчанию) отключает это через флаг
`mount_storage_dirs` в аргументах `nativeForkAndSpecialize`, и каталоги начинают
отдаваться напрямую через sdcardfs.

Механика: Zygisk в `jni_hooks.hpp` кладёт в `AppSpecializeArgs` указатель на
**локальную переменную хука**

```cpp
jboolean mount_storage_dirs;
...
args.mount_storage_dirs = &mount_storage_dirs;
...
reinterpret_cast<...>(fork_app_methods[0].fnPtr)(..., mount_storage_dirs, ...);
```

и та же переменная уходит в оригинальный `nativeForkAndSpecialize`, поэтому
запись `*args->mount_storage_dirs = JNI_FALSE` в `preAppSpecialize` доезжает до
`SpecializeCommon`, и `BindMountStorageDirs()` не вызывается.

Модуль делает это **только** если подмена маунта прошла успешно — иначе
`Android/data` остался бы на FUSE без tmpfs-обвязки, что хуже штатного варианта.

---

## 6. Кого модуль покрывает

Подмена выполняется для **каждого** процесса, который запускает Zygote с
`mount_external == MOUNT_EXTERNAL_DEFAULT` **или `MOUNT_EXTERNAL_ANDROID_WRITABLE`** —
то есть для всех приложений.

`ANDROID_WRITABLE` нужен отдельно: с ним работают приложения с
`MANAGE_EXTERNAL_STORAGE` и системный провайдер SAF `com.android.externalstorage`,
через который ходит **DocumentsUI**. Без этого «Файлы» показывали `Android/data`
и `Android/obb` пустыми — см. §3.3.

Пропускаются не приложения, а системные потребители сырого `/data/media`:

| `mount_external` | кто | почему пропускается |
|---|---|---|
| `NONE` (0) | изолированные процессы | внешнего хранилища у них нет вовсе |
| `INSTALLER` (2) | installd | нужен сырой `/data/media` |
| `PASS_THROUGH` (3) | MediaProvider | нужен сырой `/data/media`, иначе отвалится сам слой хранилища |

---

## 7. Требования

* **Ядро с `CONFIG_SDCARD_FS=y`** (для marble — собрано, `sdcardfs_splice_read`
  присутствует в сборке).
* **Включённый Zygisk**: Magisk 27+ либо ZygiskNext (KernelSU / APatch).
* `ro.build.version.sdk` >= 29.
* `getprop external_storage.sdcardfs.enabled` не обязан быть `true` — модуль сам
  поднимает sdcardfs на `/mnt/runtime/*/emulated` (`sdcardfs-bringup.sh`) и не
  вмешивается в ветку vold/MediaProvider.

---

## 8. Установка

```bash
# сборка (нужен NDK 29, путь подхватывается автоматически)
cd sdcardfs-restore
./build.sh                    # → module/zygisk/*.so + out/sdcardfs_restore-v2.3.0.zip
```

Установить `out/sdcardfs_restore-v2.3.0.zip` через Magisk / KernelSU Manager,
перезагрузиться.

> При установке KernelSU распаковывает модуль в
> `/data/adb/modules_update/sdcardfs_restore/` и применяет при следующей
> загрузке. До перезагрузки в `/data/adb/modules/sdcardfs_restore/` лежат
> **старые** файлы — не пугайтесь.

Список приложений настраивать не нужно и нельзя: модуль работает для всех.

---

## 9. Конфиг

`/data/adb/modules/sdcardfs_restore/config`

```
!enabled=1            # !enabled=0 — выключить модуль целиком
!android_dirs=raw     # raw (по умолчанию) | isolated
!verbose=0            # !verbose=1 — подробный лог (тег SdcardFsRestore)
!relabel=1            # перемаркировка корня /data/media (обязательна, см. §3.2)
!relax=1              # снять лишние биты с /mnt/runtime и /data/media (см. §3.1)
!selftest=0           # !selftest=1 — полная диагностика при каждом запуске приложения
```

Изменения подхватываются **без перезагрузки**: конфиг читается один раз за запуск
процесса, поэтому достаточно выгрузить приложение из недавних. Исключение —
`!relabel=` и `!relax=`: они применяются скриптами при загрузке, их смена требует
перезагрузки.

Аварийное выключение без правки конфига:

```bash
su -c 'touch /data/adb/sdcardfs_restore.disable'
```

---

## 10. Диагностика

```bash
# что модуль сделал (logcat)
su -c 'logcat -d -s SdcardFsRestore'

# отчёт, собранный service.sh после загрузки
su -c 'cat /data/adb/sdcardfs_restore.log'

# ключевое: ярлык корня ДОЛЖЕН быть media_rw_data_file
su -c 'ls -Zd /data/media /data/media/0'

# sdcardfs в ядре и точки монтирования
su -c 'grep sdcardfs /proc/filesystems'
su -c 'grep -E "runtime|pass_through|/mnt/user" /proc/mounts'
```

Проверка внутри namespace конкретного приложения:

```bash
P=com.mixplorer
su -c "nsenter -t \$(pidof $P) -m -- sh -c '
  stat -c \"%n mode=%a\" /storage/emulated /storage/emulated/0
  stat -f -c \"%T\" /storage/emulated          # ожидается 0x5dca2df5 (sdcardfs)
  ls /storage/emulated
  ls /storage/emulated/0/Android/data         # видны ВСЕ пакеты, не только свой
  touch /storage/emulated/0/Download/.probe && echo WRITE_OK
  rm -f /storage/emulated/0/Download/.probe
'"
```

Ожидаемый результат:

* `/storage/emulated` → `555`, `/storage/emulated/0` → `770`;
* `stat -f` → `0x5dca2df5` (sdcardfs, не `fuse`);
* `Android/data` показывает **все** пакеты.

Если корень отдаёт `EACCES`, а содержимое работает — значит не применилась
перемаркировка: проверьте в логе строку `relabel:` и ярлык `/data/media`.

---

## 11. Ограничения

* **Только user 0** покрыт дешёвым bind: vold монтирует sdcardfs лишь при
  `getMountUserId() == 0` (`EmulatedVolume.cpp:380`). Для остальных пользователей
  модуль использует второй путь — собственный `mount("sdcardfs")` с
  `userid=<n>`, — он от vold не зависит.
* **Только `MOUNT_EXTERNAL_DEFAULT` и `MOUNT_EXTERNAL_ANDROID_WRITABLE`** — см. §6.
  Пропускаются не приложения, а системные потребители сырого `/data/media`
  (изолированные процессы, installd, MediaProvider).
* `sdcardfs` — устаревший драйвер: часть приложений, рассчитанных на семантику
  FUSE, может вести себя иначе.
* **`!android_dirs=raw` расширяет доступ.** Отключая tmpfs-обвязку, модуль делает
  `Android/data` и `Android/obb` видимыми целиком — включая каталоги других
  приложений. Это и есть цель («как раньше»), но это ослабляет изоляцию. Если
  приложение ведёт себя странно, поставьте `!android_dirs=isolated`.
* Модуль **не отключает FUSE** — он накрывает его биндом в namespace процесса.
  Нижний FUSE-маунт остаётся живым, поэтому откат = удалить модуль.

---

## 12. Структура

```
sdcardfs-restore/
├── build.sh                        сборка (NDK 29, arm64-v8a + armeabi-v7a, zip)
├── src/
│   ├── sdcardfs_restore.cpp        реализация варианта B
│   └── zygisk.hpp                  публичный Zygisk API (api.hpp, ZYGISK_API_VERSION 5)
├── module/                         ← корень flashable zip
│   ├── module.prop
│   ├── config                      глобальные флаги (списка приложений нет)
│   ├── customize.sh                проверка сборки, права, подсказки
│   ├── post-fs-data.sh             relabel + relax + bringup до старта Zygote
│   ├── relabel-media.sh            chcon /data/media -> media_rw_data_file
│   ├── relax-storage.sh            снятие лишних битов с /mnt/runtime и /data/media
│   ├── sdcardfs-bringup.sh         per-point mount(2) на /mnt/runtime/*/emulated
│   ├── service.sh                  повтор relabel/relax/bringup + диагностика
│   └── zygisk/
│       ├── arm64-v8a.so
│       └── armeabi-v7a.so
├── device/                         ← снятое с устройства (см. §15), в git не входит
├── screenshots/                    снимки экрана с проверками (см. §3.3, §14)
├── tools/                          probe/test-скрипты и помощники
└── out/
    └── sdcardfs_restore-v2.4.0.zip
```

---

## 13. Ссылки

* `../aosp-ref/DESIGN-zygisk-sdcardfs.md` — полный разбор вариантов A/B/C,
  SELinux-таблица, порядок вызовов Zygisk.
* `../aosp-ref/REFS.md` — выжимка по исходникам (метки, gid, API).

Исходники, на которые опирается реализация:

| Что | Файл |
|---|---|
| активатор sdcardfs | `system/core/sdcard/sdcard.cpp` |
| монтирование | `vold/model/EmulatedVolume.cpp:356-423`, `vold/Utils.cpp:1684` |
| выбор режима | `frameworks/base/services/core/java/com/android/server/StorageManagerService.java:4522` |
| бинд на `/storage` | `frameworks/base/core/jni/com_android_internal_os_Zygote.cpp:832-880`, `:1935-1960` |
| права драйвера | `fs/sdcardfs/sdcardfs.h:413 get_gid`, `:432 get_mode` |
| доступ по имени | `fs/sdcardfs/packagelist.c:152 check_caller_access_to_name` |
| gid'ы процесса | `frameworks/base/services/core/java/com/android/server/am/ProcessList.java:1809` |
| передача `mount_storage_dirs` | Magisk `native/src/core/zygisk/jni_hooks.hpp:16-28` |
| ярлык корня хранилища | `system/sepolicy/private/file_contexts:633-634`, `private/domain.te:252`, `private/app.te:149` |

---

## 14. Статус проверки

**Проверено на живом устройстве** (POCO F5 / marble, Android 16 SDK 36,
KernelSU 3.3.0, ZygiskNext, ядро `5.10.269-Bouquet-v5.1`), модуль **v2.4.0**:

* Zygisk-хук вызывается живым Zygote для каждого приложения; в logcat —
  `sdcardfs подключён (uid=…, <пакет>): bind /mnt/runtime/full/emulated`
  (проверено на `com.mixplorer`, `com.android.documentsui`,
  `com.x8bit.bitwarden`, `org.amnezia.vpn` и других);
* **DocumentsUI показывает `Android/data` со всеми пакетами** — см. §3.3:
  провайдер SAF `com.android.externalstorage` (`mountMode=ANDROID_WRITABLE`)
  получает sdcardfs, и «Файлы» отрисовывают `POCO F5 > Android > data`
  (`screenshots/4-documentsui-android-data.png`);
* перемаркировка корня выполняется модулем при загрузке; в логе —
  `relabel: /data/media: u:object_r:media_userdir_file:s0 -> media_rw_data_file`
  (специально проверялось на сброшенном вручную ярлыке);
* **`/mnt/runtime/full/emulated` из домена `shell` (coredomain)**:
  до перемаркировки `stat` → `Permission denied`, после — `mode=555`,
  `ls` отдаёт `0` и `obb`;
* **внутри namespace приложения от имени uid приложения** (uid 10398,
  группы `1077,3001,3002,3003,9997,20398,50398`; заход через
  `nsenter -m` + сброс личности — root сам по себе DAC обходит и ничего
  не доказывает):

  | Проверка | Результат |
  |---|---|
  | `stat /storage/emulated` | `mode=555 uid=0 gid=9997` |
  | `stat /storage/emulated/0` | `mode=770 uid=0 gid=9997` |
  | `stat -f /storage/emulated` | `0x5dca2df5` (**sdcardfs**, не fuse) |
  | `ls /storage/emulated` | `0`, `obb` |
  | `ls …/Android/data` | **51** пакет, включая чужие |
  | `ls …/Android/obb` | каталоги чужих пакетов |
  | `touch …/Download/.probe` | `WRITE_OK` |
  | доступ к `…/Android/data/ai.qwenlm.chat.android` | виден |

* то же самое подтверждено на втором, независимом приложении
  (`com.android.documentsui`): sdcardfs, корень `555`, содержимое `770`,
  51 пакет в `Android/data`;
* режимы драйвера: каталоги `770 gid 9997`, файлы `660 gid 9997`;
* причина прежних отказов объяснена полностью: DAC на компонентах пути
  (§3.1) плюс `media_userdir_file` на корне нижнего слоя (§3.2);
* ABI Zygisk-хуков совпадает с ZygiskNext — точки входа, версия API,
  раскладка `api_table` (сверено с рабочим модулем LSPosed с того же аппарата);
* правки sepolicy **не нужны** — доказано по коду ядра 5.10
  (`selinux_mount()` проверяет только `FILE__MOUNTON` на каталоге-цели);
* откат: `touch /data/adb/sdcardfs_restore.disable` + перезагрузка.

---

## 15. Проверка хуков на устройстве

Аппарат: POCO F5 / marble, Android 16 (SDK 36), KernelSU 32601,
ZygiskNext **1.4.5 (836-b13d58a-release)**, ядро `5.10.269-Bouquet-v5.1`.

С устройства сняты сами файлы хуков (`device/`), чтобы проверять не по
документации, а по тому, что реально исполняется:

| Файл | Зачем |
|---|---|
| `zygisksu/libzygisk64.so` / `libzygisk32.so` | загрузчик Zygisk, маппится в zygote |
| `zygisksu/libzn_loader64/32.so`, `libpayload64.so` | инжект в процесс |
| `zygisksu/zygiskd64/32` | демон, умеет `znctl` |
| `zygisksu/sepolicy.rule` | какие правила ZygiskNext накладывает на `zygote` |
| `state/modules_info`, `state/znctx` | как демон видит список модулей |
| `state/zygote64.maps` | что реально загружено в zygote |
| `lsposed/lsposed64.so` | эталон рабочего модуля на этом же аппарате |

### 15.1. ABI совпадает

| Проверка | Наш модуль | LSPosed (эталон) |
|---|---|---|
| `zygisk_module_entry` | есть, `T` (глобальный, C) | есть, `T` |
| `zygisk_companion_entry` | есть, `T` | есть, `T` |
| `NEEDED` | `liblog, libm, libdl, libc` | `libz, liblog, libm, libdl, libc` |

Версия API: наш `zygisk.hpp` — канонический `ZYGISK_API_VERSION 5`
(`api_table` = `impl, registerModule, hookJniNativeMethods, pltHookRegister,
exemptFd, pltHookCommit, connectCompanion, setOption, getModuleDir, getFlags`).
ZygiskNext 1.4.5 реализует ту же v5, а `AppSpecializeArgs_v5` с
`mount_storage_dirs` — то самое поле, которое использует `!android_dirs=raw`.

### 15.2. Домен `zygote` имеет всё необходимое

```
zygote64  pid=1322  label=u:r:zygote:s0  uid=0
          CapEff=000001ffffffffff   (CAP_SYS_ADMIN присутствует)
          Seccomp=0   NoNewPrivs=0
```

`getenforce` → `Enforcing`, policyvers 33. Ключевой момент — как ядро 5.10
проверяет `mount(2)`:

```c
static int selinux_mount(const char *dev_name, const struct path *path,
                         const char *type, unsigned long flags, void *data)
{
        if (flags & MS_REMOUNT)
                return superblock_has_perm(cred, path->dentry->d_sb,
                                           FILESYSTEM__REMOUNT, NULL);
        else
                return path_has_perm(cred, path, FILE__MOUNTON);
}
```

То есть для обычного (в том числе bind) маунта проверяется **только
`mounton` на каталоге-цели** — права `filesystem mount` не участвуют вовсе.
Проверено по двум деревьям: vanilla `v5.10.200` и `android12-5.10` — код
идентичен.

Наши цели:

| Каталог | Метка | Нужное право | Есть в AOSP 16 `zygote.te` |
|---|---|---|---|
| `/mnt/user/0/emulated` | `u:object_r:fuse:s0` | `fuse:dir mounton` | да (`{ sdcard_type fuse }:dir`) |
| `/storage` | `u:object_r:mnt_user_file:s0` | `mnt_user_file:dir mounton` | да |

Плюс `allow zygote self:global_capability_class_set sys_admin;` — под это
попадает `unshare(CLONE_NEWNS)`.

### 15.3. Перезагрузка не обязательна

`zygiskd` (он же `znctl`) умеет перечитывать список модулей без ребута:

```
znctl start|stop|exit|status
znctl znmod <enable|disable|reload> <mod[:lib]> [svc]
```

Этого достаточно, чтобы прогнать хук на живой системе и не перезагружать
телефон.
