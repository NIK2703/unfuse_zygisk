# Доступ приложений к своему `Android/data` — причины и исправление

> Повод — логи двух приложений с живого A11 (пользователь модуля, отзыв 2026-10-09):
> `io.github.muntashirakon.AppManager` (uid 10365) и `com.mixplorer.silver` (uid 10321)
> падают на старте. Разбор — в конце документа, §7. Этот документ — про исправление:
> что именно в модуле ломает доступ, какие правки это убирают и как каждую проверить.

## 1. Симптом в логах

Оба приложения падают **не** на чтении хранилища, а на **создании файла в своём же**
каталоге:

```
java.io.FileNotFoundException:
  /storage/emulated/0/Android/data/io.github.muntashirakon.AppManager/cache/am.log:
  open failed: EACCES (Permission denied)
    at libcore.io.IoBridge.open(IoBridge.java:492)
    at io.github.muntashirakon.AppManager.logs.Logger.<init>(Logger.java:32)
Caused by: android.system.ErrnoException: open failed: EACCES (Permission denied)
```

```
E/RootService: External directory unavailable.
  ... io.github.muntashirakon.AppManager.servermanager.ServerConfig.init(ServerConfig.java:56)
Caused by: java.io.FileNotFoundException:
  /storage/emulated/0/Android/data/io.github.muntashirakon.AppManager/cache/am.jar:
  open failed: EACCES (Permission denied)
```

Второй лог — тот же класс:

```
FATAL EXCEPTION: Thread-3
Process: com.mixplorer.silver
java.lang.RuntimeException: No data path!
    at libs.t01.b(SourceFile:53)
```

`No data path!` у Mixplorer — это его собственный «не нашёл ни одного **записываемого**
внешнего каталога»: тот же отказ, только приложение сформулировало его само.

Три вывода, которые из логов следуют жёстко:

1. **Это DAC, а не SELinux.** Все AVC в обоих логах идут с `permissive=1` (в списке модулей
   устройства есть `selinux_permissive`), и они про другое — `name="sdcard"`,
   `tclass=lnk_file`. Разрешение отклонено ядром по правам, не политикой.
2. **Отказано в своём, а не в чужом.** Путь — `Android/data/<свой пакет>/…`; по AOSP это
   ровно тот каталог, который приложение обязано иметь право писать.
3. **`EACCES` на `open(O_CREAT)` означает:** либо в родительском каталоге нет права на
   запись, либо **на одном из компонентов пути нет права на поиск (x)**. `ENOENT` был бы,
   если бы каталога не было. Значит каталог есть — и он приложению не подчиняется.

## 2. Что AOSP гарантирует и чем

Инвариант, который обязан держаться для каждого приложения `A` и каждого объекта `O` внутри
`Android/{data,obb,media}/<пакет A>`:

* `A` проходит по всем компонентам пути от `/storage/emulated/0` до `O`;
* `A` пишет в свои `cache`/`files`;
* и это верно **независимо от того, какой процесс создал объект** и **с какой umask**.

На A11 AOSP держит это **тремя** независимыми механизмами:

1. **Владелец.** `PrepareAppDirFromRoot()` создаёт `<пакет>` с `uid = appUid`
   (`Utils.cpp:305`), `gid = AID_EXT_DATA_RW`, `mode = S_IRWXU|S_IRWXG|S_ISGID` (`:342`).
2. **Per-package default ACL.** Там же, `Utils.cpp:398`:
   `SetDefaultAcl(pathToCreate, mode, uid, gid, additionalGids)` с
   `additionalGids.push_back(uid)` (`:321`). Комментарий AOSP прямо называет обе задачи:

   > «Also add the app's own UID as a group; since apps belong to a group that matches their
   > UID, this ensures that they will always have access to the files created in these dirs,
   > **even if they are created by other processes**»

   и выше, у `SetDefaultAcl`:

   > «to ensure that even if applications run with a **umask of 0077**, new directories within
   > these directories will allow the GID specified here to write»

3. **`other = --x` на двух каталогах.** `PrepareAndroidDirs()` (`Utils.cpp:1588`) делает
   `Android` и `Android/data` как `mode = S_IRWXU|S_IRWXG|S_IXOTH|S_ISGID` = **`02771`**,
   `uid:gid = AID_MEDIA_RW`. То есть `drwxrws--x` — «пройти можно, читать нельзя». Это
   единственный механизм, который работает, когда потребитель **не читает POSIX ACL**, а
   смотрит только на биты режима.

Модуль снимает все три и ставит вместо них один:

* `tools/storage-fix` пишет **одну** именованную запись — `gid 9997` — и **обнуляет `OTHER`**
  (`dir_mode`/`file_mode`/`traverse_mode` не возвращают `m & S_IRWXO`);
* `tools/vold-noacl` **удаляет** механизм (2) целиком: он делает `setxattr` в vold
  no-op'ом, а на A11 это **единственный** `setxattr` в vold — `SetDefaultAcl`
  (`Utils.cpp:192`, вызовы только `:398` и `:1615`; в `FsCrypt.cpp`/`VolumeManager.cpp` A11
  его нет вовсе);
* механизм (1) остаётся, но он покрывает только то, что приложение создало **само**.

## 3. Классы отказа, которые из этого следуют

| # | Класс | Чем вызывается | Что видит приложение |
| - | ----- | -------------- | -------------------- |
| **F1** | Записи для приложения нет | объект внутри `Android/data/<пакет>` создан **другим uid** (installer, MTP, MediaProvider, root-сервис, второй процесс), а default ACL vold'а снят патчем | `EACCES` в своём же каталоге |
| **F2** | Маска обнулена | любое создание с `mode & S_IRWXG == 0` (`0600` файл, `0700` каталог) процессом, чей libc хуки не пропатчили: приложение не в SuList, статический/musl-бинарь, прямой сисколл, второй in-process патчер, root-демон | «листинг работает, открытие — нет» (формулировка самого проекта) |
| **F3** | `other` снят | `storage-fix` обнулил `S_IRWXO` на `Android`/`Android/data`, а потребитель смотрит **только на режим** — прежде всего FUSE-вид MediaProvider, который POSIX ACL нижнего дерева не читает | `EACCES` на **всём** поддереве `/storage/emulated/0/Android/**` |
| **F4** | Модуль применён наполовину | патч vold **не встал** (FUSE остался), а `storage-fix` и `vold-noacl` — встали. Ровно состояние mido по `docs/android-11-crdroid-log.md` | сырое дерево заперто, и при этом в пути по-прежнему FUSE |
| **F5** | Нет починки после позднего писателя | vold переставляет режимы `/Android*` при каждой подготовке каталога приложения; `walk()` в `storage-fix` покрывает только то, что существовало в момент запуска | новые каталоги/файлы зависят от того, кто писал последним |

F2 назван самим проектом в `docs/android-11-design.md`, §«Галереи — отдельная ось»:

> доступ даёт одна именованная запись ACL для 9997 плюс libc-хуки, формирующие режим до
> сисколла, и там, где записи нет или маска обнулена (`posix_acl_create_masq` на `0600`),
> **листинг работает, а открытие — нет**

То есть класс отказа был известен и записан; не хватало только связи «вот эти два приложения
в логе — это он».

**F3 подтверждён замером** (redroid-ВМ, A11, 2026-10-09):

```
до:   drwxrws--x 5 media_rw media_rw  /data/media/0/Android
после storage-fix /data/media/0:
      drwxrws--- 5 media_rw media_rw  /data/media/0/Android
      access: USER_OBJ rwx  GROUP_OBJ rwx  GROUP 9997 rwx  MASK rwx  OTHER ---
```

`other` ушёл из `--x` в `---`, а единственный оставшийся грант — запись для `9997`.

**F1 подтверждён по исходнику:** после `vold-noacl` на A11 не остаётся **ни одного** писателя
default ACL, кроме самого модуля, а модуль пишет только `9997`.

## 4. Исправление

Правки упорядочены «от корня к следствиям». A и C убирают причину, B — следствие, D —
состояние, которое реально случилось у пользователя, E закрывает остаток.

### Fix A — снят: запись дописывает `storage-fix`, а не handler в vold (делает Fix C)

Первоначальный замысел: переписать `tools/vold-noacl.c` так, чтобы он не глушил vold, а
**дописывал** в его default ACL шестую запись `ACL_GROUP` с `e_id = 9997` — redirect на
сгенерированный arm64-handler вместо `mov w0,#0; ret`.

**От этого отказались: Fix C делает то же самое дешевле и безопаснее.** `tools/vold-noacl`
уже делает `setxattr` в vold no-op'ом, поэтому vold свою версию ACL **не пишет вообще** — а
значит `storage-fix` может написать свою, и в ней не надо выбирать между 9997 и uid
приложения: она несёт **обе** записи.

* ACL vold'а на `Android/data/<пакет>` = `{USER_OBJ appUid, GROUP_OBJ 1078, GROUP appUid,
  MASK, OTHER}` (5 записей, `Utils.cpp:321,:398`).
* ACL от Fix C = `{USER_OBJ appUid, GROUP_OBJ 1078, GROUP 9997, GROUP appUid, MASK, OTHER}` —
  **строгое надмножество**.

Merge-handler не дал бы ничего сверх этого, но добавил бы в vold **третий** сгенерированный
arm64-handler. Правило проекта требует, чтобы emitted-код проверялся **исполнением** на aarch64
(`docs/fuse-root-patch-design.md`), а цена ошибки здесь — падение vold, то есть хранилище и,
возможно, bootloop: `build_stub_patch()` в `tools/vold-fusefs.c` описывает ровно такой случай
из истории проекта. Поэтому Fix A **не реализован**, и это осознанное решение, а не пропуск.

Побочный выигрыш Fix C: у `Android/media/<пакет>` vold пишет ACL **без** записи для uid
приложения (`additionalGids` там пуст, `Utils.cpp:330`), а Fix C добавляет её и туда.

### Fix B — `storage-fix`: не снимать `S_IRWXO`

`ACL_OTHER` в `storage-fix` обнулялся намеренно (большой комментарий в `tools/storage-fix.c`).
Аргумент в нём: «оставить on-disk other bits — значит позволить `other` обойти запись 9997,
шире, чем Android 10, где sdcardfs пускал только группу 9997».

**Для тех каталогов, где это важно, аргумент неверен.** В Android 10 sdcardfs показывал
`/storage/emulated/0/Android` как **`drwxrws--x`**, то есть `other = --x`. Вернуть `--x` там —
не «шире Android 10», а **ровно Android 10**. И `--x` — это только проход, ни чтения, ни записи.

Реализовано (`tools/storage-fix.c`):

```c
static mode_t dir_mode(mode_t m)      { return (m & S_IRWXU) | S_IRWXG | (m & S_IRWXO); }
static mode_t file_mode(mode_t m)     { return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IWGRP | (m & S_IRWXO); }
static mode_t traverse_mode(mode_t m) { return (m & S_IRWXU) | (m & S_IRWXG) | S_IRGRP | S_IXGRP | (m & S_IRWXO); }
```

Это **не** общее расширение, и level-логика не нужна: у всего дерева `/data/media` режимы
`0770`/`0660`, так что `m & S_IRWXO == 0` и ACL не меняется. Другие биты есть **только** у
четырёх уровней `Android*`, которые vold готовит как `02771` (`Utils.cpp:1588`) — там и
возвращается `--x`.

В `src/hook_libc.cpp` `acl_build()` теперь тоже берёт `ACL_OTHER` из режима
(`e[4] = {kAclOther, mode & S_IRWXO, 0}`) — чтобы два писателя читались одинаково. Значение там
по-прежнему `0`, и так и должно быть: `mode` — это уже сформированный режим из
`as_sdcardfs_file/dir`, который оставляет только owner+group биты. Объекты, создаваемые
приложением под `/data/media`, и в Android 10 были `0770`/`0660`; `other = --x` несут только
уровни `Android*`, и их пишет `storage-fix` из on-disk режима.

### Fix C — `storage-fix --app-dirs`: per-package запись, как в AOSP

Новый проход по `<root>/Android/{data,obb,media}/<пакет>`: добавляет `ACL_GROUP` с
`e_id = st_uid`. На A11 владелец per-package каталога **и есть** uid приложения — проверено в ВМ:

```
drwxrws--- 3 u0_a117  ext_data_rw  .../Android/data/io.github.muntashirakon.AppManager
```

Это буквально `additionalGids.push_back(uid)` из AOSP. `storage-fix` — нативный инструмент без
доступа к PackageManager, поэтому `st_uid` здесь не «приближение», а правильный источник.

Глубина ограничена одним уровнем намеренно: vold пишет эту ACL только на `depth == 0`
(`Utils.cpp:398`), а всё глубже наследует default ACL оттуда. Каталог, принадлежащий root/system
(`st_uid < AID_APP_START`), пропускается — именованная запись для него ничего не даёт.

### Fix D — порядок и проверка посылки (F4)

Это то, что реально случилось у пользователя. `post-fs-data.sh` запускал `storage.sh`
**до** vold-патчей, и на mido патч vold не сработал — то есть сырое дерево было подготовлено
под состояние, которого не наступило: FUSE остался, а дерево заперто.

Реализованы две правки, обе одноразовые (никаких сторожей — правило проекта):

1. **Порядок.** В `post-fs-data.sh` `sh "$MODDIR/storage.sh"` перенесён **после** обоих патчей;
   в `service.sh` — тоже последним. Стоит ноль и убирает подготовку дерева до того, как решено
   состояние vold.
2. **Проверка посылки.** `service.sh` идёт после того, как vold смонтировал тома, — это
   единственная точка загрузки, где посылка **решаема**: `stat -f -c %t /storage/emulated/0` не
   должен быть `0x65735546` (fuse). Если он всё ещё fuse, `vold-noacl` в этом состоянии только
   снимает гарантию AOSP и ничего не даёт взамен (нашего ACL-режима всё равно не обслуживает
   сырое дерево), поэтому он **не ставится**, и vold пишет свой per-package ACL сам.
   Это одноразовое решение в известной точке, а не сторож.

Отдельного глагола `--restore` **не понадобилось**: после Fix B снятый `other` больше не запирает
дерево под FUSE (`--x` на `Android*` — это ровно поведение Android 10), так что «снять режим ACL»
сводится к тому, чтобы не ставить `vold-noacl`.

### Fix E — остаток

`docs/fuse-root-patch-design.md:191` уже говорит, что libc-хуки — «улучшение, а не условие».
После A+C это становится правдой: запись с uid приложения пишет vold, а `storage-fix`
подтверждает её на каждой загрузке. Остаётся узкая дыра — объект, созданный **другим uid** с
**обнулённой маской** (другой процесс делает `0700` внутри каталога приложения). Это редко и
самонаказуемо; честнее записать это в документации, чем ставить сторожа. Дополнительно:
проверить, что `linkat` есть в `kHooks[]` (для `rename` это уже сделано — `fix_after_move`).

## 5. Чего делать не надо

* **Сторожа/наблюдателя** — правило проекта, без исключений. Fix D — одноразовое решение, не
  наблюдение.
* **Ставить `other = rwx`** где угодно: вот это как раз «шире Android 10».
* **Полагаться на рекурсивный `walk()`** как на единственный источник ACL — он покрывает
  только то, что существовало на момент запуска (F5).
* **Расширять грант на `AID_SDCARD_RW`/`AID_MEDIA_RW`**: у приложений их нет. Проверено на
  живом устройстве: `Groups:` процесса приложения — `9997` плюс производные от его uid
  (`20330`, `50330`), а `1078(ext_data_rw)`/`1079(ext_obb_rw)` есть только у shell/root.

## 6. План проверки

**Матрица в redroid-ВМ (A11)**, по одной загрузке на строку; приложение — то самое
`io.github.muntashirakon.AppManager` (uid 10117 в ВМ), плюс проба F1.

| строка | vold-fusefs | vold-noacl | storage-fix | ожидание |
| ------ | ----------- | ---------- | ----------- | -------- |
| 0 | вкл | вкл (no-op) | до правок | работает; в `Android/data/<пакет>` **только** 9997, `other` на `Android*` = `---` |
| 1 | вкл | вкл (no-op) | Fix B | `other = --x` на `Android`, `Android/data`, `Android/obb`, `Android/media` |
| 2 | вкл | вкл (no-op) | Fix B+C | плюс `GROUP <uid приложения>` в ACL каталога пакета |
| 3 | **выкл** | **не ставится** (Fix D) | Fix B+C | FUSE в пути, поддерево всё равно проходимо; vold пишет свой per-package ACL |

**Проба F1** (того, чего в ВМ сейчас нет): root'ом создать
`/data/media/0/Android/data/<пакет>/cache` с режимом `0700` и **без** ACL, затем писать туда от
uid приложения. Строка 0 обязана упасть, строки 1–3 — пройти.

**На устройстве, где воспроизводится**, снять до любых правок:

```
stat -f -c %t /storage/emulated/0                       # 0x65735546 = bind не встал (F4)
ls -ld /data/media/0 /data/media/0/Android /data/media/0/Android/data \
       /data/media/0/Android/data/<пакет>
<путь>/acl-dump  <та же цепочка>                        # есть ли запись с uid приложения
<модуль>/tools/vold-noacl --check ; echo $?             # 0 = no-op установлен
<модуль>/tools/vold-fusefs --check ; echo $?
cat /proc/<pid приложения>/status | grep ^Groups
```

Этого набора достаточно, чтобы отличить F1/F2/F3/F4 без догадок.

## 7. Что проверено, а что нет (честно)

**Проверено в этой работе:**

* в redroid-ВМ (A11, модуль no-sdcardfs) `io.github.muntashirakon.AppManager` 4.1.0 —
  то самое приложение из лога — **не падает**: создаёт
  `Android/data/<пакет>/cache/am.log` успешно, в трёх состояниях: модуль целиком;
  модуль без bind'а vold (FUSE в пути, `vold-noacl` взведён — состояние mido);
  и с принудительно обнулённым `other` на `Android` (`storage-fix /data/media/0` вручную);
* `vold-noacl --check` и `vold-fusefs --check` в ВМ = 0 (оба патча стоят);
* libc-хуки в ВМ **работают**: `am.log` создан `0660` и с ACL
  `GROUP 9997 rw- / MASK rw-` — подпись `as_sdcardfs_file()` + `acl_build()`;
* `storage-fix /data/media/0` действительно снимает `other` (`drwxrws--x` → `drwxrws---`);
* приложения в группе `9997`, но **не** в `1078/1079`;
* A11: `setxattr` в vold встречается **ровно один раз** — `SetDefaultAcl`
  (`Utils.cpp:192`), вызовы `:398` и `:1615`.

**Не проверено:** воспроизведения падения получить не удалось. Значит **точная** причина у
конкретного пользователя — одна из F1..F4, и выбрать между ними можно только по набору из §6 с
его устройства. Правки B–D при этом обоснованы каждая сама по себе: B — замером `other` и
поведением sdcardfs на Android 10, C — семантикой AOSP (возврат записи, которую снял патч),
D — наблюдаемым состоянием mido.

**Не проверено и в сборке от 2026-10-09:** правки B–D внесены в код и собираются (arm64 +
arm32, `-Wall -Wextra` чисто), но **на устройстве ещё не прогонялись** — матрица из §6 целиком
впереди. Fix A не реализован сознательно (§4).

## 8. Что правится

| файл | правка |
| ---- | ------ |
| `tools/storage-fix.c` | `dir_mode`/`file_mode`/`traverse_mode` сохраняют `S_IRWXO` (Fix B); новый глагол `--app-dirs` + `acl_apply_extra` — per-package запись из `st_uid` (Fix C) |
| `src/hook_libc.cpp` | `acl_build()`: `ACL_OTHER` берётся из `mode & S_IRWXO` — читается синхронно с `storage-fix`; значение остаётся `0`, см. §4 |
| `module/storage.sh` | вызов `--app-dirs` для `Android/{data,obb,media}` |
| `module/post-fs-data.sh` | `storage.sh` — последним, после обоих патчей (Fix D) |
| `module/service.sh` | `vold-fusefs` первым, затем проверка посылки (`stat -f -c %t`), `vold-noacl` — только при вставшем bind, `storage.sh` — последним (Fix D) |
| `tools/vold-noacl.c` | **не меняется**: Fix A снят в пользу Fix C, см. §4 |
| `docs/android-11-design.md` | §«Галереи и свой `Android/data`»: настоящая причина и ссылка сюда |
