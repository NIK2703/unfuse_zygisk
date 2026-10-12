# shellcheck shell=sh
#
# lib.sh — то, что у сборок unfuse и unfuse-sdcardfs совпадает буквально.
#
# Лежит в module/common/, то есть попадает в оба архива, и подключается через `.`
# из customize.sh (установка), post-fs-data.sh и storage.sh (загрузка). Здесь
# только примитивы, у которых не бывает варианта: смена системного свойства,
# проверка релиза, перемаркировка корня /data/media и общий шаг установщика.
#
# Политики здесь нет намеренно. Какое значение свойства нужно и что в каком
# порядке монтировать — это у сборок разное (unfuse выключает FUSE в vold и
# правит ACL, unfuse-sdcardfs поднимает sdcardfs на /mnt/runtime/*/emulated) и
# общей быть не может. Логирования нет ни здесь, ни в сборках: модуль ничего не
# пишет — ни журнала, ни logcat, — и отчитывается только кодом возврата.

# unfuse_is_android_11 — Android 11 (SDK 30) единственный релиз, где форму
# хранилища FUSE/sdcardfs переключает булево свойство persist.sys.fuse
# (EmulatedVolume.cpp:304 `isFuse = GetBoolProperty(kPropFuse, false)`,
# Zygote.cpp:834 `if (isFuse)`). С 12 обеих веток нет — там всегда FUSE, и
# свойство не читает никто (проверено по 30..37, docs/android-11-design.md).
unfuse_is_android_11() { [ "$(getprop ro.build.version.sdk)" = "30" ]; }

# unfuse_setprop <имя> <значение>
#
# Через resetprop -n, если он есть. resetprop, а не setprop, по двум причинам:
#   * -n кладёт значение прямо в область свойств, минуя property_service, — а
#     тот до load_persist_props молча не пишет на диск persist.* (заслон:
#     property_service.cpp:195-198, снимается только по её завершении);
#   * external_storage.* не объявлен в property_contexts и падает в catch-all
#     `* u:object_r:default_prop:s0` (property_contexts:114), а не в system_prop.
# setprop оставлен фолбэком на случай, когда resetprop недоступен.
#
# Чего -n НЕ делает: не персистит. load_persist_props (init.rc:405-406, из
# late-init init.rc:437) читает /data/property/persistent_properties и
# перезаписывает память тем, что там лежит, — всё, что выставлено раньше, теряется
# (персист — это -p, Magisk 26+). Именно на этом спотыкалась sdcardfs-сборка:
# её `persist.sys.fuse false` не доживало до zygote-start. Сборка больше на это не
# рассчитывает — см. её post-fs-data.sh и src/unfuse_sdcardfs.cpp. Тем же
# механизмом (-n) работает и system.prop той сборки: его применяет сам загрузчик
# модулей, и он тоже не персистится (module.cpp:282-286 → resetprop.cpp:452-457).
unfuse_setprop() {
    if command -v resetprop >/dev/null 2>&1; then
        resetprop -n "$1" "$2"
    else
        setprop "$1" "$2"
    fi
}

# unfuse_relabel_media_root
#
# Без этого getattr() на /storage/emulated отдаёт EACCES у appdomain и coredomain
# (domain.te:252: один search и ни одного getattr), причём без AVC — отказ помечен
# dontaudit. Содержимое уже несёт media_rw_data_file, где у appdomain полные права
# (app.te:149), так что перемаркировать надо ровно корень.
#
# Молчит: раньше функция печатала ПРЕЖНЮЮ метку и различала три кода возврата
# (метка уже нужная / перемаркировали / chcon не прошёл) — это уходило в лог
# сборок, а лога больше нет и читать это некому. Остался обычный признак успеха:
# 0 — метка на месте, не 0 — chcon не прошёл.
unfuse_relabel_media_root() {
    _cur=$(ls -Zd /data/media 2>/dev/null | awk '{print $1}')
    case "$_cur" in
        *:media_rw_data_file:*) return 0 ;;
    esac
    chcon u:object_r:media_rw_data_file:s0 /data/media 2>/dev/null
}

# unfuse_require_zygisk_so <ABI> — сборка не доехала. Только для customize.sh:
# abort() определяет установщик Magisk/KernelSU.
unfuse_require_zygisk_so() {
    [ -f "$MODPATH/zygisk/$1.so" ] ||
        abort "! zygisk/$1.so is missing - the build did not run (build.sh)"
}

# unfuse_install_common <ABI> — шаг установщика, одинаковый у обеих сборок:
# права на распакованное дерево, на zygisk/<ABI>.so и на общие файлы, плюс
# уборка конфига, который не читает никто (остался от снятой настройки пути).
# Вызывается из customize.sh ПОСЛЕ своих проверок; set_perm/ui_print определяет
# установщик.
unfuse_install_common() {
    set_perm_recursive "$MODPATH" 0 0 0755 0644
    set_perm "$MODPATH/zygisk/$1.so" 0 0 0644
    set_perm "$MODPATH/lib.sh"          0 0 0755 2>/dev/null
    set_perm "$MODPATH/customize.sh"    0 0 0755 2>/dev/null
    set_perm "$MODPATH/post-fs-data.sh" 0 0 0755 2>/dev/null
    set_perm "$MODPATH/service.sh"      0 0 0755 2>/dev/null
    set_perm "$MODPATH/storage.sh"      0 0 0755 2>/dev/null
    set_perm "$MODPATH/description.txt" 0 0 0644 2>/dev/null

    # Present only in the sdcardfs build; set_perm_recursive above already gives
    # it 0644, this line is here so the file's permissions are stated where the
    # other files' are. Not executable: the module loader reads it, no shell does.
    set_perm "$MODPATH/system.prop" 0 0 0644 2>/dev/null

    # Leftovers of the old config; nothing reads them.
    rm -f /data/adb/unfuse_zygisk.conf 2>/dev/null
}
