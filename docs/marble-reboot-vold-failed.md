# Перезагрузка `marble` 2026-10-09 22:47:40 — разбор

Повод: во время работы над тестом sdcardfs в ВМ телефон перезагрузился, и возник
вопрос «не я ли его перезагрузил». Ниже — что удалось установить по артефактам,
и что установить не удалось.

## 1. Факт перезагрузки

| источник | значение |
|---|---|
| `/proc/uptime` в 22:49:33 | 113.49 с → загрузка **22:47:40** |
| dropbox `system_server_crash@1791564498714` | `SystemUptimeMs: 38634`, время 22:48:18.714 |
| IP | сменился (10.48.5.24 → 10.83.195.105 → **10.228.131.44**) |

## 2. Что запускалось на телефоне

Ровно **одна** команда: `host.sh restart` → `docker restart redroid11`, в 22:46:51.

`/data/local/tmp/redroid/log/dockerd.log` (время UTC, локальное = +6):

```
16:46:51.937 Container failed to exit within 10s of signal 15 - using the force
16:46:52.578 ShouldRestart failed, container will not be restarted  exitStatus={137 ...}
```

→ 22:46:51, контейнер убит в 22:46:52; загрузка телефона в 22:47:40, т.е. **+49 с**.

`adb reboot` не запускался ни разу.

**Почему `docker restart` не может перезагрузить телефон:** у контейнера свой
PID-namespace — `/data/local/tmp/redroid/root/containers/<id>/hostconfig.json`
содержит `"PidMode":""` (и `"Privileged":true`, `"IpcMode":"private"`). Убийство
контейнера убивает процессы только внутри его namespace.

**Контроль:** такие же рестарты контейнера в 22:27:02 и 22:33:35 перезагрузкой
**не** сопровождались (в dropbox между 21:52:32 и 22:48:18 записей нет).

## 3. Что говорит сам телефон о причине

```
ro.boot.bootreason = reboot,vold-failed
sys.boot.reason    = reboot,vold-failed
```

Это директива `reboot_on_failure reboot,vold-failed` на сервисе `vold`: init
перезагрузился, потому что **vold вышел**.

- Tombstone'а vold нет: последние в `/data/tombstones/` — `audioserver`
  (8 окт 16:23, SIGABRT по таймауту audio policy) и `sensor-notifier` (7 окт).
  Значит это чистый выход или SIGKILL, а не SIGSEGV/SIGABRT.
- Паники и watchdog в dmesg прошлой загрузки нет (kernel cmdline, `/sys/fs/pstore`
  пуст, в текущем dmesg только `gh-watchdog ... Initialized`).
- `bootstat -p`: `boot_reason 0`, `system_boot_reason 1` — содержательного нет.

## 4. Чего исключить нельзя

Наш модуль (ветка no-sdcardfs) на **каждом** буте патчит память vold живьём:
`tools/vold-fusefs` подменяет трамплин `mount()`, `tools/vold-noacl` снимает ACL-проход.
Оба инструмента пересобраны 2026-10-09 21:23, модуль поставлен 21:51.

`vold-failed` — ровно тот отпечаток, который даёт смерть vold. Поэтому версию
«наш vold-патч дестабилизирует vold» из имеющихся данных **исключить нельзя**.
Прямых улик против неё нет (текущая загрузка: vold стартовал на 1.57 с и жив,
pid 838; хранилище f2fs), но и защищающих улик тоже нет.

Как проверить:

1. при следующей перезагрузке сразу снять `dmesg` и `logcat -b all -d`, искать
   tombstone/abort по vold;
2. погонять сутки с `touch /data/adb/modules/unfuse_zygisk/disable` — если
   `vold-failed` повторится, патч ни при чём.

## 5. Отдельный давний дефект телефона (к модулю отношения не имеет)

`system_server` падает примерно на **38-й секунде после КАЖДОЙ загрузки**:

```
java.lang.AbstractMethodError: abstract method
  "void android.app.IProcessObserver.onProcessStarted(int, int, int, java.lang.String, java.lang.String)"
  on receiver java.lang.Class<ku>
    at android.app.IProcessObserver$Stub.onTransact(IProcessObserver.java:127)
```

В `/data/system/dropbox/` — **десятки** записей с 2026-10-06 23:46 (10-06: 23:46,
23:48, 23:50; 10-07: ~25; 10-08; 10-09: 04:05 … 22:48), у всех `SystemUptimeMs`
в диапазоне 26–45 с. То есть это не разовый сбой, а устойчивое состояние.

Наш модуль такого дать не может: он чисто нативный, Java не хукает и в
`system_server` не инжектится.

Виновник — Java-хук внутри `system_server`. Включено 7 LSPosed-модулей; в
`system_server` (scope `system`) идут четверо:

| модуль | что делает |
|---|---|
| `inc.whew.android.fakegapps` | подпись microG |
| `io.github.auag0.disablecamerasound` | выключение звука затвора |
| `xyz.cirno.pseudodcdimming` | псевдо-DC dimming |
| `io.nekohasekai.sfa` | sing-box |

Класс `ku` по APK не локализуется: ни в одном из шести модульных APK нет строки
`IProcessObserver`. Но у **`io.nekohasekai.sfa`** хук в `system_server` сломан явно
— единственная ошибка загрузки модуля в логе LSPosed за эту загрузку:

```
E/LSPosedFramework (system)[framework,LSPosedContext,…]
    Failed to load class io.nekohasekai.sfa.xposed.XposedInit
java.lang.NoSuchMethodException: io.nekohasekai.sfa.xposed.XposedInit.<init> []
```

Перебор виновника начинать с этих четырёх, в первую очередь с `sfa`.

## 6. Состояние на момент разбора — норма

| проверка | значение |
|---|---|
| `/mnt/user/0/emulated` | `f2f52010` (f2fs) |
| `/storage/emulated` | `f2f52010` |
| `/storage/emulated/0` | `f2f52010` |
| `/data/media` | `f2f52010` |
| vold | `running`, pid 838, старт 1.57 с |
| маркер | `/data/adb/unfuse_zygisk.state/once` есть |
| `unfuse_zygisk/service.sh` | отработал 22:48:01 |
| установленный вариант | **no-sdcardfs** (есть `tools/`, нет `status.sh`) |

## 7. Побочное

Контейнер `redroid11` после перезагрузки телефона **не запущен**: dockerd
поднимается вручную (`host.sh prepare` → `host.sh dockerd` → `host.sh start`).
Поэтому тест sdcardfs-варианта в ВМ остался незавершённым — модуль в ВМ уже
установлен (`md5 00cabb75…` arm64 / `ff25db2f…` armv7a, `disable` нет), но гость
после установки не перезапускался. Бэкап прежнего состава —
`/data/local/tmp/mod-prev.tgz`.

Что известно про ВМ заранее (полезно для продолжения): `/mnt/runtime/{default,full,read,write}/emulated`
существуют (пустые, `1021994`), и `sdcardfs` есть в `/proc/filesystems`
(`nodev sdcardfs` — ядро общее с телефоном).
