# Стенд Android 11 arm64 с Magisk для ПК — что реально работает

> Задача: получить на ПК Android 11 (SDK 30), **aarch64**, с настоящим `vold` и
> FUSE-хранилищем, root/Magisk — чтобы прогонять `vold-fusefs --check` / `--dry-run`
> и разделять §3.A/§3.B разбора `docs/android-11-crdroid-log.md`.
> Дата разбора вариантов — 2026-10-09.

## 0. Жёсткое ограничение, из которого всё следует

`vold-fusefs` патчит **только EM_AARCH64** (`tools/vold-targets.sh`, `word()`), а
площадка FUSE — это `mount("/dev/fuse", …, MS_LAZYTIME)` внутри aarch64-кода `vold`.
Значит стенд обязан быть **настоящим aarch64**: `uname -m = aarch64`,
`ro.product.cpu.abi = arm64-v8a`, `/system/bin/vold` — ELF aarch64.

Отсюда:

- **На x86-ПК (наш случай, `PROCESSOR_ARCHITECTURE=AMD64`) arm64-гость идёт только
  программной эмуляцией** (QEMU TCG). KVM/HAXM/WHPX ускоряют только x86-гостей —
  для aarch64 они не применимы. Быстро — только на ARM-хосте.
- **Любая ARM-трансляция (Houdini / libndk / native bridge) не годится**: под ней
  `vold` остаётся x86_64, патчить нечего. Это вычёркивает BlissOS/Android-x86 11,
  LDPlayer, MuMu, Genymotion на x86, WSA.

## 1. Вариант A (рекомендую для этой задачи) — Android Studio AVD, arm64-v8a, API 30 + rootAVD

Работает на x86_64-хосте, бесплатно, ставится из уже имеющегося SDK.

### 1.1 Почему это возможно у нас

| проверка | результат |
| --- | --- |
| Образ API 30 arm64 в репозитории Google | **есть**: `system-images;android-30;google_apis;arm64-v8a` и `…;google_apis_playstore;arm64-v8a` (проверено по `dl.google.com/android/repository/sys-img/*/sys-img2-3.xml`) |
| Бэкенд в нашем эмуляторе 36.5.10 | **есть**: `%LOCALAPPDATA%\Android\Sdk\emulator\qemu\windows-x86_64\qemu-system-aarch64.exe` (и `-headless`) |
| cmdline-tools | `…\Sdk\cmdline-tools\latest\bin\{sdkmanager,avdmanager}.bat` |
| Свободное место | C: 15 ГБ, E: 39 ГБ — образ ~1.5 ГБ + userdata до 16 ГБ, **AVD класть на E:** |

### 1.2 Установка

```bat
set SDK=%LOCALAPPDATA%\Android\Sdk
"%SDK%\cmdline-tools\latest\bin\sdkmanager.bat" ^
  "platform-tools" "platforms;android-30" "system-images;android-30;google_apis;arm64-v8a"
"%SDK%\cmdline-tools\latest\bin\avdmanager.bat" create avd ^
  -n a11_arm64 -k "system-images;android-30;google_apis;arm64-v8a" -d pixel_2
```

AVD на E: — задать `ANDROID_AVD_HOME=E:\android-avd` до `avdmanager create`.

### 1.3 Запуск (ключевые флаги)

```bat
"%SDK%\emulator\emulator.exe" -avd a11_arm64 ^
  -gpu swiftshader_indirect ^
  -accel off ^
  -memory 4096 -cores 4 ^
  -no-snapshot -no-boot-anim -no-audio ^
  -qemu -machine virt
```

- `-accel off` — обязателен: аппаратного ускорения aarch64 на x86 нет.
- `-qemu -machine virt` — лечит `PCI bus not available for hda`, типовой отказ
  arm/arm64-образов (подтверждено рабочим рецептом).
- `-gpu swiftshader_indirect` — против чёрного экрана (нет GPU-драйвера под virt).
- **Если 36.5.10 откажется грузить arm64-образ** — откат на старый эмулятор:
  `https://dl.google.com/android/repository/emulator-windows_x64-12038310.zip`
  (= 34.2.16), распаковать в отдельную папку и звать его `emulator.exe` через
  `-sysdir <образ>`. Именно так собран публично подтверждённый рабочий кейс
  (Windows x86_64 + arm64-v8a).

### 1.4 Проверка, что это действительно aarch64

```sh
adb shell getprop ro.product.cpu.abi      # arm64-v8a
adb shell getprop ro.dalvik.vm.native.bridge   # пусто (не Houdini!)
adb shell uname -m                        # aarch64
adb shell getprop ro.build.version.sdk    # 30
```

### 1.5 Что смотреть в первую очередь (ради этого стенд и нужен)

```sh
adb root
adb shell 'getprop persist.sys.fuse; getprop external_storage.sdcardfs.enabled'
adb shell 'grep emulated /proc/mounts'
adb shell 'pidof vold; md5sum /system/bin/vold'
```

Два исхода, оба полезны:

- FUSE-форма (`persist.sys.fuse=true`, `/mnt/user/0/emulated` = `fuse /dev/fuse …lazytime`)
  → стенд **точно воспроизводит** проблему с mido, можно гонять `--check`/`--dry-run`
  и разделять «патч не встал» / «встал и не сработал».
- sdcardfs / `/mnt/runtime/*` → это pre-FUSE-стенд, то есть ровно тот случай, под
  который писалась правка `post-fs-data.sh` в `docs/android-11-design.md`. Тоже
  ценно, но проверяет другую половину.

`vold` в образе — **AOSP-шный**, то есть байт-в-байт родня тому `vold-a11`, на
котором уже проверялась площадка. Это позволяет отделить «наш инструмент сломан»
от «vold у crDroid другой» (гипотеза §3.A разбора лога).

### 1.6 Root/Magisk на AVD

Инструмент — **rootAVD** (патчит `ramdisk.img` образа, ставит Magisk внутрь AVD):

- основная репа: `https://gitlab.com/newbit/rootAVD` (GitHub-зеркало read-only);
- форк с фиксом Magisk 30.x: `https://github.com/dynamicfire/rootAVD`;
- использование: `./rootAVD.sh system-images/android-30/google_apis/arm64-v8a/ramdisk.img`
  (для API ≤34 rootAVD по умолчанию тянет Magisk v26.4; `FAKEBOOTIMG` — если патчить
  APK-ом вручную).

**Грабля, критичная именно для нас:** наш модуль объявляет `ZYGISK_API_VERSION 5`
(проверено по тегам Magisk: `v27.0` и `v28.1` → 5). При Magisk **26.x (API v4)**
загрузчик отвергнет модуль: `module.cpp:27` — `if (api_version > ZYGISK_API_VERSION)
return false;`. Значит на стенде ставить **Magisk 27.0+** либо сразу
**Kitsune 27001** — то же, что у пользователя на mido. Иначе Zygisk-плечо модуля
не поднимется и тест `mount_storage_dirs` будет ложноотрицательным.

Оговорка про скорость: TCG-эмуляция aarch64 годится для **функциональных** проверок
(`--check`, `--dry-run`, коды возврата, таблица маунтов). Для **бенчмарка хранилища**
она негодна — числа будут искажены, замер надо делать на варианте B.

## 2. Вариант B (аутентичный и быстрый) — redroid arm64 на ARM-Linux-хосте

redroid — Android в Docker-контейнере на **хостовом ядре**, поэтому реальное arm64,
реальный `vold`, реальный FUSE и никакой TCG-эмуляции.

- Образ **Android 11 — multi-arch**: `redroid/redroid:11.0.0-latest` содержит
  `linux/amd64` **и** `linux/arm64` в одном теге (проверено по Docker Hub API).
  На arm64-хосте `docker pull` сам возьмёт arm64-вариант.
- Требования к хосту: Linux + ядро с `binder_linux` / `ashmem_linux` (или
  `androidboot.use_memfd=1`), `--privileged`.
- Magisk + Zygisk + LSPosed: `catlair/redroid-magisk` —
  `python redroid.py -a 11.0.0 -m` (собирает производный образ, не пересобирая
  redroid целиком). Альтернатива: `AytuRed/redroid-kernelsu`.
- Где взять ARM-хост:
  - **Oracle Cloud Ampere A1 free tier** — 4 OCPU / 24 ГБ ARM, бесплатно, нативно
    быстро (самый дешёвый способ получить честный aarch64);
  - Raspberry Pi 4/5 (ядро собрать с binder/ashmem);
  - Apple Silicon Mac (Linux-VM) — нативно.
- **На Windows это неудобно**: нужен Linux-хост; WSL2 со стоковым ядром не имеет
  `binder`/`ashmem`, потребуется своё ядро. Не для «просто попробовать».

## 3. Вариант C (максимум аутентичности, максимум возни) — QEMU full-system + GSI

Полноценная виртуалка: edk2 UEFI + Android 11 arm64 **GSI** (LineageOS 18.1 GSI или
AOSP GSI) + свой ядро с virtio. Даёт настоящий arm64-userspace с реальным `vold`
(LineageOS-сборка — как у mido, что ценно для гипотезы «у crDroid vold другой»),
Magisk ставится патчем `boot.img`. Цена — ручная сборка образа и UEFI-загрузка.
Имеет смысл, если AVD-образ окажется не той формы хранилища, а ARM-хоста нет.

## 4. Что для этой задачи не подходит

| вариант | почему нет |
| --- | --- |
| BlissOS / Android-x86 11, LDPlayer, MuMu, WSA | x86_64-`vold`; ARM-трансляция (Houdini/libndk) даёт arm-**приложения**, но не aarch64-`vold`. Патчер (`EM_AARCH64`) применить не к чему |
| Genymotion (на x86) | то же: на x86-хосте x86-образ с трансляцией; arm64 — только на ARM-хосте |
| Cuttlefish | arm64-образы требуют arm64-хоста |
| Waydroid на x86 | x86_64-образ + трансляция; arm64 — только на arm64-хосте |
| «Платные облачные ARM-телефоны» (LDCloud/Redfinger) | нет доступа к `/system/bin/vold` и `su`-скриптам модуля |

Исключение по BlissOS: если нужен **просто root-Android 11 на ПК** (не для проверки
aarch64-патча) — он годится, Magisk там ставится сообществом.

## 5. Итоговая рекомендация под наш кейс

1. **Сейчас, на этом ПК** — вариант A: AVD `arm64-v8a` API 30 + Magisk 27.0+
   (или Kitsune 27001). Даёт ответ на `--check`/`--dry-run` и разделяет §3.A и §3.B
   разбора лога. Если 36.5.10 не заведёт образ — эмулятор 34.2.16.
2. **Если нужен честный aarch64 и замеры** — вариант B на бесплатном ARM-VM
   (Oracle Ampere A1): redroid `11.0.0` + `redroid-magisk`.
3. Вариант C — только если понадобится именно LineageOS-сборка `vold` (как у mido).

## 6. Источники

- Репозиторий образов Google: `dl.google.com/android/repository/sys-img/{google_apis,google_apis_playstore}/sys-img2-3.xml`
- Рабочий рецепт arm64-v8a на Windows x86_64 (эмулятор 34.2.16, `-machine virt`,
  `swiftshader_indirect`, `-accel off`): qiita.com/fna/items/1ca8cc6f64faa0e6b64f
- rootAVD: `gitlab.com/newbit/rootAVD`, форк `github.com/dynamicfire/rootAVD`
- redroid: `github.com/remote-android/redroid-doc`; Docker Hub `redroid/redroid` (multi-arch)
- redroid + Magisk: `github.com/catlair/redroid-magisk`, `github.com/AytuRed/redroid-kernelsu`
- Zygisk API: Magisk `native/src/core/zygisk/api.hpp` (v27.0/v28.1 → 5),
  `native/src/core/zygisk/module.cpp:27`
