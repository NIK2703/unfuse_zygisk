# Один репозиторий, две сборки: unfuse и unfuse-sdcardfs

> **Что это.** Раньше два подхода к одной задаче жили на двух ветках:
> `no-sdcardfs` (патч vold + libc-хуки, сырое `/data/media` вместо FUSE) и
> `sdcardfs-only` (sdcardfs на `/mnt/runtime/*/emulated`). Общего у них было
> больше, чем разного — META-INF установщика, `src/zygisk.hpp`, проверка релиза,
> смена свойства, перемаркировка `/data/media`, упаковка архива. Держать это в
> двух ветках значило править каждую правку дважды и однажды забыть во второй.
> Теперь это одно дерево на ветке `master`, из которого `build.sh` собирает
> **два модуля**.

## Что собирается

| модуль            | id (`module.prop`) | архив                      | что делает                                                       |
| ----------------- | ------------------ | -------------------------- | ---------------------------------------------------------------- |
| `unfuse`          | `unfuse_zygisk`    | `out/unfuse-<version>.zip` | патчит vold (`vold-fusefs`, `vold-noacl`), формует ACL, хукает libc |
| `unfuse-sdcardfs` | `unfuse_zygisk`    | `out/unfuse-sdcardfs-<version>.zip` | монтирует sdcardfs на `/mnt/runtime/*/emulated`          |

**id у обоих один и тот же — `unfuse_zygisk`, и это осознанно.** Модули
альтернативные: `unfuse` на Android 11 ставит `persist.sys.fuse=true`, а
`unfuse-sdcardfs` — `false`, то есть вместе они не только не нужны, но и
противоречат друг другу. Один id означает, что установка второго модуля
заменяет первый, а не оставляет на устройстве два враждующих.

> **Важно про пути.** От id зависит всё, что модуль создаёт на устройстве:
> `/data/adb/modules/unfuse_zygisk`, `/data/adb/unfuse_zygisk.state`,
> `/data/adb/unfuse_zygisk.debug.log` (unfuse), `/data/adb/unfuse_zygisk.log`
> (unfuse-sdcardfs), `/data/adb/unfuse_zygisk.once`, `/data/adb/unfuse_zygisk.nolog`.
> Переименование id — это не правка одного поля, а смена всех этих путей
> (в скриптах, в `diag.sh` и в докой). Поэтому id не трогается.

## Дерево

```
build.sh                  один скрипт на оба модуля (см. ниже)
module/
  common/                 в оба архива одинаковым куском
    META-INF/…            update-binary + updater-script установщика
    lib.sh                общие примитивы (см. «Что общее»)
  unfuse/                 плечо + утилиты первой сборки
    module.prop  description.txt
    customize.sh  post-fs-data.sh  service.sh  storage.sh
    log.sh  diag.sh       диагностика: лог и снимок состояния (только здесь)
    tools/                storage-fix-*, vold-noacl-*, vold-fusefs-* (сборка)
    zygisk/               arm64-v8a.so, armeabi-v7a.so (сборка)
  unfuse-sdcardfs/        плечо второй сборки
    module.prop  description.txt
    customize.sh  post-fs-data.sh  service.sh  storage.sh  status.sh
    zygisk/               arm64-v8a.so, armeabi-v7a.so (сборка)
src/
  unfuse_zygisk.cpp       плечо unfuse: перехват mount() в vold, гашение
                          Android/{data,obb}, libc-хуки
  unfuse_sdcardfs.cpp     плечо unfuse-sdcardfs: bind sdcardfs в namespace
  zygisk.hpp              общий, ОДИН файл на обе сборки
  hook_libc.*  openat_stub.h  arm32_patch.h  gnu_props.h  func_size.*
  android_ver.h           только unfuse (таблица релизов для хуков)
tools/                    стенд и исходники утилит
  storage-fix.c  vold-noacl.c  vold-fusefs.c  vold-common.h
  …                       хостовые тесты, проверялки, скрипты устройства
  sdcardfs/               стенд ветки sdcardfs-only целиком (device-*.sh,
                          probe*.sh, nsprobe.c, runas.c, storage-map.sh, _fetch.sh)
docs/                     разборы по AOSP и по стендам
screenshots/  LICENSE  README.md
```

`module/<вариант>/zygisk/*.so` и `module/unfuse/tools/*` — **собранные бинарники,
и они лежат в git**. Так было и в двух ветках: сборка детерминированная, поэтому
изменение md5 в диффе означает ровно то, что исходник поехал.

## Что общее, а что своё

Общее (`module/common/`, подключается через `. "$MODPATH/lib.sh"`):

* **META-INF** установщика — был побайтово одинаков у обеих сборок;
* **`unfuse_is_android_11`** — SDK 30, единственный релиз, где форму хранилища
  переключает `persist.sys.fuse`;
* **`unfuse_setprop`** — `resetprop -n` с фолбэком на `setprop`;
* **`unfuse_relabel_media_root`** — корень `/data/media` из `media_userdir_file`
  в `media_rw_data_file`; возвращает код (уже была / перемаркировали / не вышло)
  и печатает прежнюю метку, чтобы каждая сборка записала её в свой лог;
* **`unfuse_require_zygisk_so`** и **`unfuse_install_common`** — проверка сборки
  и общий шаг установщика (права на дерево, уборка мёртвого конфига);
* **`src/zygisk.hpp`** — оба плеча объявляют `ZYGISK_API_VERSION 4` по одной и
  той же причине (сломанный `valid()` в Magisk 27.0), и код заголовка совпадает.

Своё у каждой сборки — и это **не** недосмотр, а суть:

* **политика хранилища**: `unfuse` выключает FUSE в vold и правит ACL,
  `unfuse-sdcardfs` поднимает sdcardfs. Это разные механизмы, а не два написания
  одного;
* **значение `persist.sys.fuse`** на Android 11: `true` против `false`;
* **логи**: у `unfuse` — `log.sh` + `diag.sh` (`.debug.log`, снимок состояния,
  `--probe`), у `unfuse-sdcardfs` — простой `log()` в `unfuse_zygisk.log` и
  `status.sh`, пишущий знак в описание `module.prop`. Сводить их в один
  механизм значило бы менять то, что уже проверено на устройстве;
* **утилиты**: три C-утилиты есть только у `unfuse`.

## Сборка

```sh
./build.sh                    # оба модуля, arm64-v8a + armeabi-v7a
./build.sh unfuse             # только unfuse
./build.sh unfuse-sdcardfs    # только unfuse-sdcardfs
./build.sh unfuse arm64-v8a   # вариант и ABI — в любом порядке
API=30 ./build.sh             # другой API level (по умолчанию 26)
ZIP=0 ./build.sh              # без упаковки
NDK=/path/to/ndk ./build.sh   # явный путь к NDK
STRIP=0 ./build.sh            # без strip (отладка)
```

Как устроена упаковка: `module/common/` и `module/<вариант>/` складываются в
один staging-каталог (`out/.staging-<вариант>`), и в архив идёт ровно он. Права
в архиве нормализованы (`.sh`, `tools/**` и `update-binary` — `0755`, остальное
`0644`), а не взяты с файловой системы: иначе zip зависел бы от umask сборочной
машины. `customize.sh` выставляет те же права ещё раз при установке.

## Грабли, которые здесь легко повторить

* **`cp -f` поверх `zygisk/*.so` на устройстве усекает inode** под живым mmap —
  только `mv -f` (см. `docs/diagnostics.md`). К сборке не относится, но к
  установке собранного — прямо.
* **Zygisk Next держит `module.so` по fd, а не по пути**: после `mv` новая сборка
  активна только после перезагрузки.
* **Плечо `unfuse` и утилиты проверяются на aarch64.** Хостовый `--selftest`
  только собирает и дизассемблирует; ошибки потока выполнения ловит запуск на
  устройстве.
