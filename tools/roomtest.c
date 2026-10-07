/*
 * roomtest.c — where would vold-fusefs put the handler, on this vold?
 *
 * Developer tool, not part of the module.
 *
 * find_handler_room() is unreachable through the tool's own flags. On the live
 * patch path it is only reached once both stubs are intact, and on the device
 * they never are (the patch is installed, and reinstalling it is refused). The
 * two read-only flags return before it — --check and --dry-run both return
 * above it — and --file returns before it as well, with "файл не меняется —
 * запись только в живой процесс". So the single decision that picks the
 * handler's home was never exercised by a normal run. That is how it went
 * unnoticed that on the device's vold it always answers "no room" — after
 * reading the whole executable segment (~850 KB) to find out.
 *
 * This calls it directly, on a file, so the answer is visible and can be
 * compared before and after a change to it.
 *
 * vold-fusefs.c is pulled in as one translation unit, with its main() renamed
 * away, so this exercises the real function rather than a copy of it.
 *
 * Build: tools/build-roomtest.sh
 * Run:   roomtest [<elf> [<stub-va-hex>]]
 *        With no stub given, the "mount" trampoline is resolved out of the
 *        file's own tables (the device's /system/bin/vold by default). A stub
 *        address may be passed instead, which is what lets a synthetic image
 *        exercise the "there is room" branch — a branch no real image on the
 *        device reaches.
 * Exit:  0 room found, 1 no room, 2 could not resolve
 */

#define main vold_fusefs_main_renamed
#include "vold-fusefs.c"
#undef main

int main(int argc, char **argv) {
    const char *path = (argc > 1) ? argv[1] : "/system/bin/vold";

    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;
    s.is_proc = false;
    s.label = path;

    s.fd = open(path, O_RDONLY | O_CLOEXEC);
    if (s.fd < 0) {
        perror(path);
        return 2;
    }
    if (src_open_header(&s) != 0) {
        src_close(&s);
        return 2;
    }

    uint64_t stub = 0;
    if (argc > 2) {
        stub = strtoull(argv[2], NULL, 16);
        info("%s: трамплин задан вручную = 0x%llx", path,
             (unsigned long long)stub);
    } else {
        Hook hm;
        if (hook_resolve(&s, &hm, TARGET_SYM) != EXIT_OK) {
            warn("%s: трамплин \"%s\" не разрешился", path, TARGET_SYM);
            src_close(&s);
            return 2;
        }
        stub = hm.res.stub_va;
        info("%s: трамплин \"%s\" = 0x%llx, GOT 0x%llx", path, TARGET_SYM,
             (unsigned long long)stub, (unsigned long long)hm.res.got_slot);
    }

    uint64_t home = 0;
    const int rc = find_handler_room(&s, stub, &home);
    if (rc == 0) {
        info("вердикт: место есть — дом обработчиков 0x%llx",
             (unsigned long long)home);
    } else {
        info("вердикт: места нет — обработчики уйдут в отдельную страницу "
             "(vold_exec_page)");
    }
    src_close(&s);
    return rc == 0 ? 0 : 1;
}
