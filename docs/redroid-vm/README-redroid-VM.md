# redroid Android 11 + Magisk на этом телефоне

Виртуальная машина **Android 11 (API 30) с Magisk Delta**, запущенная прямо на телефоне
через контейнер redroid. Даёт полноценную среду Android с root для тестов и разработки.

Работает нативно (без эмуляции CPU — полная скорость ARM), потому что redroid
использует binder и DMA-BUF ядра самого телефона.

---

## ⚠️ ГЛАВНОЕ ПРАВИЛО

**Никогда не выполняйте `adb reboot` для этой ВМ.**

Контейнер запущен с `--privileged` и **делит ядро с телефоном**. Вызов `reboot()`
из гостевой Android перезагрузит **реальное устройство**, а не виртуальную машину.

| Хочу перезагрузить | Правильно |
|---|---|
| Гостевую Android | `rd restart` (= `docker restart`) |
| Телефон | как обычно, но ВМ после этого поднимать через `rd up` |

Проверено на практике: один `adb reboot` погасил телефон.

---

## Быстрый старт

После любой перезагрузки телефона:

```bash
rd up
```

Команда автоматически: поднимет `/run`, смонтирует CA-сертификаты, восстановит
маршрутизацию, запустит dockerd, создаст контейнер с пробросом порта наружу,
дождётся загрузки Android и подключит adb.

Заняло ~2 минуты (после первого раза — быстрее).

---

## Подключение к ВМ

| Откуда | Адрес |
|---|---|
| С телефона / из Termux | `127.0.0.1:5556` |
| С ПК или другого устройства (Wi-Fi) | `<IP-телефона>:5556` |
| Внутри контейнера | `5555` |

Узнать IP телефона:

```bash
rd ip
```

```bash
adb connect 127.0.0.1:5556      # или adb connect 10.57.53.217:5556
adb -s 127.0.0.1:5556 shell
```

> Порт 5555 на телефоне занят его собственным adbd, поэтому ВМ отдана на 5556.

**Безопасность:** порт слушает `0.0.0.0`, adb работает без авторизации
(`ro.adb.secure=0`) и даёт **root**. Любой в вашей сети получает root в ВМ.
Не нужно постоянно — гасите через `rd down`.

---

## Команды

```bash
rd up          # поднять всё с нуля (после перезагрузки телефона)
rd status      # состояние: dockerd, порт, мост, Android, Magisk, модули
rd restart     # перезапуск ТОЛЬКО гостевой Android
rd down        # остановить контейнер и dockerd

rd shot        # скриншот → ~/shot.png
rd tap X Y     # тап
rd swipe X1 Y1 X2 Y2 [мс]
rd text "текст"   # ввод текста
rd key KEYCODE_HOME
rd sh "команда"   # shell внутри Android
rd su "id"        # команда от root через Magisk
rd install file.apk
rd pull /sdcard/x ./x
rd log 100      # логи контейнера
rd ip           # внешний IP для подключения
rd shell        # интерактивный adb shell
```

---

## Magisk

```bash
adb -s 127.0.0.1:5556 shell su -c id
# uid=0(root) gid=0(root) groups=0(root)
```

| Параметр | Значение |
|---|---|
| Magisk | Delta 25206 (build `4dbd8358`) |
| Zygisk | включён |
| DenyList | выключен |
| APK менеджера | `io.github.huskydg.magisk` (установлен) |

`sbin/su` и `system/bin/su` → `magisk`.

---

## Экран с управлением

Веб-панель: `rd web`, затем <http://127.0.0.1:8080> (с ПК — `http://<IP>:8080`).
Показывает экран ВМ с тачем, свайпами, кнопками и вводом текста.

**Ограничение:** ~2–3 fps. `screencap` через adb снимает кадр отдельным запуском
процесса, поэтому плавной анимации нет. Для статичного интерфейса достаточно,
для видео/игр — нет.

Всегда надёжнее управлять через `rd tap` / `rd sh` / `adb shell`.

---

## Устройство установки

```
/data/local/tmp/redroid/
├── docker, dockerd, containerd, runc   статические бинари Docker CE 27.3.1 (glibc)
├── host.sh                              root-часть: монтирования, маршруты, контейнер
├── daemon.json                          конфиг dockerd (storage-driver vfs)
├── data11/                              /data ВМ (bind-mount, ПЕРСИСТЕНТЕН)
└── log/dockerd.log

~/bin/rd                                обёртка ( Termux )
~/bin/rdweb                             веб-панель
~/bin/redroid-host.sh                   исходник host.sh
```

Образы Docker:
- `redroid/redroid:11.0.0-latest` — официальный (1.67 GB)
- `redroid/magisk:11` — наш, с Magisk поверх (1.69 GB)

> Данные ВМ лежат в `data11/` и переживают пересоздание контейнера.
> Полное удаление: `rd down` + удалить `/data/local/tmp/redroid`.

---

## Технические решения

Проблемы, которые пришлось решить на этом устройстве, — чтобы не ломать голову
при повторной настройке.

### 1. Docker не запускается в Termux
Бинари Docker собраны под glibc, Termux — bionic. Запуск даёт
`Unknown signal 31`. Решение: статическая сборка Docker CE 27.3.1, запуск
**только от root** через `su`. Termux не может их выполнить (SELinux).

### 2. `/` смонтирован read-only
`containerd` падает с `mkdir /run: read-only file system`. Решение:
`mount -o remount,rw /` + tmpfs на `/run`. Всё делает `host.sh prepare`.

### 3. TLS к Docker Hub
`x509: certificate signed by unknown authority`. Решение: bind-mount системных
CA телефона `/system/etc/security/cacerts` → `/etc/ssl/certs`.

### 4. Нет overlayfs
Ядро телефона не поддерживает overlay для каталогов → storage-driver `vfs`
(медленнее на копировании, но работает).

### 5. Policy routing Android netd — самая коварная проблема
`ip route get 172.17.0.2` возвращал `dev tun0`, а не `docker0`: Android netd
перехватывает маршрутизацию **всего**, включая подсеть Docker, и уводит в
VPN-туннель. Контейнер был полностью недоступен.

Решение — два правила с приоритетом выше системных:
```bash
ip rule add pref 5000 to 172.17.0.0/16 lookup main   # хост → контейнер
ip rule add pref 5001 iif docker0 lookup main        # контейнер → интернет
ip route add default dev tun0 table main             # в main нет default
```
Это делает `host.sh prepare` при каждом `rd up`.

### 6. Модули Magisk не применяются — НЕ РЕШЕНО
Модуль `unfuse_zygisk` установлен в `/data/adb/modules/`, но не работает.

Причина: `/data` ВМ — bind-mount с телефона, а его f2fs несёт опцию
`seclabel`. Ядро берёт SELinux-метку из суперблока **политики телефона**, а
политика контейнера её не понимает → все файлы имеют невалидный контекст
`HACKED`.

Следствия:
- `post-fs-data.sh` модуля не выполняется (нет `/data/adb/unfuse_zygisk.state/`)
- `/storage/emulated` остаётся на FUSE — модуль не даёт эффекта
- `chcon` не помогает (`seclabel` перекрывает)
- `magiskpolicy` падает: `failed to load policy from /sys/fs/selinux/policy`

**Как чинить:** перенести `/data` ВМ на файловую систему **без `seclabel`**
(tmpfs или ext4-образ). Тогда метки станут валидными и модули заработают.
Побочный эффект — данные ВМ перестанут переживать пересоздание тома.

Обходной путь: запускать `post-fs-data.sh` вручную после старта, но
`vold-noacl`/`vold-fusefs` патчат уже запущенный vold, и без корректных меток
доступа права на хранилище полностью не выдадутся.

### 7. Установка модулей — вручную
`magisk --install-module` возвращает `Incomplete Magisk install`: в bootless-
сборке Magisk Delta для контейнеров нет `/data/adb/magisk/util_functions.sh`.

Установка вручную:
```bash
adb -s 127.0.0.1:5556 push module.zip /data/local/tmp/
adb -s 127.0.0.1:5556 shell su -c '
  M=/data/adb/modules/<id>
  mkdir -p $M && cd $M
  unzip -q -o /data/local/tmp/module.zip
  chmod 0755 post-fs-data.sh service.sh customize.sh
  chown -R 0:0 $M'
rd restart
```

Отключение модуля: `touch /data/adb/modules/<id>/disable`, затем `rd restart`.

---

## Требования

- Телефон с **root** (KernelSU — работает)
- Ядро с `binderfs`, `ashmem`/`memfd`, DMA-BUF heaps — есть на всех современных
- Архитектура aarch64
- ~10 ГБ свободного места (образы Docker занимают ~3.4 ГБ, `/data` ВМ — своё)

---

## Полезные команды внутри ВМ

```bash
rd sh "pm list packages"                    # список пакетов
rd sh "settings put global window_animation_scale 0"
rd su "ls -la /data/adb/modules"            # модули
rd su "magisk --sqlite 'SELECT key,value FROM settings'"
rd su "mount | grep storage"                # что с чем смонтировано
```

---

## Полезные ссылки

- [redroid-документация](https://github.com/remote-android/redroid-doc)
- [redroid-script (GApps/Magisk/Widevine)](https://github.com/ltlly/redroid-script)
- [Magisk](https://github.com/topjohnwu/Magisk)