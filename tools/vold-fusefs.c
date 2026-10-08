/*
 * vold-fusefs — stop vold from mounting FUSE for emulated storage, so the raw
 *               /data/media tree is what lands on /mnt/user/<user>/emulated,
 *               and keep vold's teardown consistent with that.
 *
 * ============================== why
 *
 * AOSP can serve emulated storage two ways, and picks one with a single boolean
 * (vold-16/Utils.cpp:1112):
 *
 *     bool IsSdcardfsUsed() {
 *         return IsFilesystemSupported("sdcardfs") &&
 *                base::GetBoolProperty(kExternalStorageSdcardfs, true);
 *     }
 *
 * With external_storage.sdcardfs.enabled=0 — the case on modern vendor images
 * even where the kernel still ships sdcardfs — the platform mounts a FUSE
 * volume on /mnt/user/<user>/emulated and runs the MediaProvider FUSE daemon
 * over /data/media. Apps then reach their files through that daemon, which is
 * what scoped storage is built on.
 *
 * The mount code itself has the branch we want already (Utils.cpp:1598,
 * MountUserFuse): the FUSE mount is unconditional, and IsSdcardfsUsed() only
 * decides what gets bind-mounted onto /mnt/pass_through — the sdcardfs view, or
 * the absolute_lower_path (the raw /data/media). So the raw tree is a supported
 * outcome; it is simply never the one that reaches /mnt/user/<user>/emulated.
 *
 * Having the raw tree there is what this module is for. An earlier approach had
 * the Zygisk module try to bind /data/media over the FUSE point from inside the
 * app's own private mount namespace (unfuse_zygisk.cpp). That fails by
 * construction on such images: the point already has a FUSE superblock, the
 * bind cannot displace it, and statfs() still reports FUSE — the module detected
 * this and rolled back, so the raw path never activated. Fighting the FUSE mount
 * from a child namespace is the wrong layer.
 *
 * That second bind is gone as of 2026-10-07, and this tool is why it could go:
 * the bind made below sits on /mnt/user/<user>/emulated, a shared mount, so
 * every app mount namespace receives it by propagation. A second bind of the
 * same tree in the app's own namespace was pure duplication — and making the
 * namespace private to keep that bind from leaking into Zygote was itself what
 * kept this one from propagating in.
 *
 * ============================== what this does instead
 *
 * MountUserFuse() has exactly one mount(2) call for this purpose:
 *
 *     result = TEMP_FAILURE_RETRY(mount("/dev/fuse", fuse_path.c_str(), "fuse",
 *                                       MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_NOATIME | MS_LAZYTIME,
 *                                       opts.c_str()));
 *
 * That call is made through vold's PLT (mount is an imported libc symbol), so
 * it is reachable by name — unlike MountUserFuse itself, which is internal and
 * absent from .dynsym (the .symtab is stripped). We replace the PLT stub of
 * `mount` and, inside the handler, pass through every call EXCEPT this one:
 *
 *   - the first argument is the string "/dev/fuse";
 *   - the third is the string "fuse";
 *   - the flags carry MS_LAZYTIME.
 *
 * All three have to match, and the third is not optional — see "how the call is
 * recognised". When they do, the handler makes TWO mounts instead of one:
 *
 *   1. the FUSE mount itself, with the caller's own arguments, untouched;
 *   2. mount("/data/media", fuse_path, NULL, MS_BIND | MS_REC, NULL) — the raw
 *      tree, on top of it.
 *
 * ============================== why the FUSE mount is still made
 *
 * The obvious version — suppress the FUSE mount and bind the raw tree in its
 * place — does not work, in two separate ways. Both look like bugs in this tool
 * and are not, so they are worth recording:
 *
 *   - MountUserFuse() does not end at the mount. It opened /dev/fuse before it,
 *     and it returns that fd to its caller, which hands it to MediaProvider's
 *     FUSE daemon; the caller then waits for the daemon to come up, and a
 *     scope_guard unmounts the volume if it does not.
 *   - With no FUSE superblock behind the fd the daemon cannot start:
 *
 *         E StorageManagerService: Failed to mount volume VolumeInfo{emulated;0}:
 *           Caused by: java.lang.IllegalStateException: Failed to start FUSE
 *
 *     after which the volume lands in state "unmountable" and vold unmounts the
 *     path again — no storage at all, and the bind is undone.
 *
 * So the FUSE mount has to exist for the volume to reach MOUNTED. Making it and
 * then putting the raw tree on top satisfies the storage session while leaving
 * the FUSE view unreachable: every lookup under /mnt/user/<user>/emulated
 * resolves to the topmost mount, which is the bind, so apps read and write
 * /data/media directly, and the Android/data and Android/obb bind mounts vold
 * makes afterwards become self-binds.
 *
 * The cost is a live FUSE superblock and an idle MediaProvider daemon that
 * nothing reads. That is the platform's design, not a choice made here: the fd
 * is part of MountUserFuse()'s contract, and a bind mount cannot satisfy it.
 *
 * ============================== and why a second stub has to go with it
 *
 * Making two mounts at fuse_path is only half the change, and shipping the
 * first half alone produced the worst bug this tool has had: the volume mounted
 * fine, then could never be mounted again.
 *
 * vold's teardown removes exactly ONE mount per path. UnmountUserFuse()
 * (Utils.cpp:1696) calls ForceUnmount(), which is a single
 * umount2(UMOUNT_NOFOLLOW) (Utils.cpp:477); UnmountTree() is a single
 * umount2(MNT_DETACH) (Utils.cpp:1314). With two mounts stacked, that removed
 * our bind and left the FUSE mount — now daemon-less — at fuse_path. Every
 * access to the path then returned ENOTCONN, so the next MountUserFuse() failed:
 *
 *     E vold: Failed to mount emulated fuse volume: Transport endpoint is not
 *             connected
 *
 * and the volume came up "unmountable" with no storage at all. /storage/emulated
 * degraded with it, because init.rc makes /storage a slave recursive bind of
 * /mnt/user/0 — one mount gone wrong there is visible to every app.
 *
 * The violated invariant is "one umount2 clears fuse_path", and the fix is to
 * make it true again from the teardown side rather than to stop stacking: hook
 * umount2 as well, and for a target shaped /mnt/user/<uid>/emulated unmount
 * repeatedly until the path is really clear. Nothing else changes — every other
 * target is passed through untouched, and the failure of a busy unmount still
 * reaches ForceUnmount's SIGINT/SIGTERM/SIGKILL escalation.
 *
 * The two hooks are installed together or not at all. A vold carrying only the
 * mount hook is worse than an unpatched vold, so a half-install is rolled back
 * and reported rather than left in place. See build_umount_handler() for the
 * handler, and FUSE_PATH_* above for why it recognises the path by shape.
 *
 * ============================== why not name MountUserFuse directly
 *
 * It is not in .dynsym (checked on device: `readelf --dyn-syms` has no such
 * name) and there is no .symtab. Anchoring on a byte signature inside it would
 * be build-specific — the very thing vold-noacl.c avoids. `mount` is imported,
 * so it has a JUMP_SLOT relocation, a GOT slot and a canonical .plt stub, and
 * the naming is stable across releases. The handler then decides by ARGUMENTS,
 * which is the property that actually matters.
 *
 * ============================== the handler
 *
 * The handler is position-dependent arm64 written out as instruction words and
 * placed in executable memory inside vold, because a jump from vold's .plt must
 * land in vold's address space. Where that memory comes from is the subject of
 * "where the handler goes" below: the trailing padding of the .plt page when the
 * build has any, otherwise a page of vold's own binary that vold maps for us.
 * Nothing here executes a stub inside vold or remaps an anonymous mapping — an
 * earlier draft did, and both are gone.
 *
 * The handlers are deliberately small and do only what is needed. The mount
 * handler:
 *
 *   1. keep the target (x1) and the caller's return address;
 *   2. compare x0 with "/dev/fuse", x2 with "fuse", and test x3 against
 *      MS_LAZYTIME;
 *   3. if any of the three fails -> tail-call the original mount with x0..x4
 *      exactly as given, so no other caller can tell the stub was replaced;
 *   4. if all three match:
 *        a. mount("/dev/fuse", target, "fuse", flags, opts) — the call the
 *           caller asked for, with its own x0..x4;
 *        b. mount("/data/media", target, NULL, MS_BIND|MS_REC, NULL);
 *        c. return 0.
 *
 * and the umount2 handler:
 *
 *   1. match the target against /mnt/user/<digits>/emulated exactly;
 *   2. if it does not match -> tail-call the original umount2 with x0/x1
 *      untouched, so an ordinary unmount is bit-for-bit what it was;
 *   3. if it does -> call umount2 repeatedly, at most four times, stopping at
 *      the first failure, and return that last result.
 *
 * Step (a) of the mount handler needs no setup at all: the handler is entered
 * through the `mount` stub, so x0..x4 already hold exactly the arguments mount()
 * wants. Only the target has to survive the call, and it is parked in x10 — a
 * caller-saved temporary, NOT x19: the handler runs in place of a real call, so
 * every callee-saved register must come back untouched, and there is no frame
 * slot for one here.
 *
 * The umount2 handler cannot park anything in a caller-saved register: it makes
 * its call in a loop, so the path and the flags have to survive it. They go in
 * its own 32-byte frame, and the count with them. Nothing else is spilled, and
 * no callee-saved register is touched there either.
 *
 * ============================== where the handler goes
 *
 * Two homes, in order of preference:
 *
 *   1. the trailing padding of the .plt page — a file-backed r-x mapping,
 *      writable through /proc/<pid>/mem by COW, needing no new mapping, no
 *      mprotect and no syscall injection. This is the same page vold-noacl.c
 *      already patches.
 *
 *   2. a page taken from vold's own executable file, when the build has no
 *      such padding: the linker can pack the PLT flush to the end of the
 *      executable segment, leaving 0 bytes after the last stub and no zero run
 *      anywhere in .text (measured on the device's vold). Anonymous executable
 *      memory is not available on Android — mmap(PROT_EXEC, MAP_ANONYMOUS) and
 *      mprotect(anon, PROT_EXEC) both come back -EACCES (W^X / execmem) — but a
 *      private PROT_READ|PROT_EXEC mapping of vold's own binary IS allowed,
 *      because vold already has execute permission on that file. So the tool
 *      has vold open its own binary and mmap a page of it RX; the handler is
 *      written into that page through /proc/<pid>/mem, which breaks COW into a
 *      private page that keeps PROT_EXEC. See vold_exec_page.
 *
 * The handler is built for the address it will live at, so that address is
 * chosen before build_handler() runs; it is absolute in both cases.
 *
 * ============================== which releases
 *
 * The resolver is version-independent: it reads vold's own tables. What is not
 * version-independent is the SET of mount() calls with type "fuse" — the count
 * is printed and must be exactly one, so a release that adds another one is
 * refused rather than half-patched.
 *
 * android_ver.h is NOT consulted here, and the include that used to sit below
 * was dropped on 2026-10-08: the table it carries exists for two things this
 * tool does not have — a per-release count of libc entry points (the Zygisk
 * module's) and the AOSP site that writes the default ACL (vold-noacl's). The
 * release question this tool asks is answered from the target itself, by the
 * call-site count, so a table entry could only ever be a second, staler answer.
 *
 * ============================== when it refuses
 *
 * Premises checked; if any fails the tool refuses (code 2) and writes NOTHING:
 *   (1) `mount` AND `umount2` each have exactly one .rela.plt JUMP_SLOT and one
 *       stub;
 *   (2) both stubs are still intact — neither already redirected, and neither
 *       already OURS. A vold with only one of the two hooks is refused
 *       outright: it cannot clear what it mounts, which is the ENOTCONN
 *       regression itself;
 *   (3) the handlers have a home — the .plt padding, or a page vold grants —
 *       large enough for both (364 bytes today, against a 512-byte search);
 *   (4) exactly one call site in vold's .text passes "/dev/fuse" as the source
 *       (counted by scanning for the string and the adrp/add pairs that build
 *       its address); more than one means the anchor is not what we think.
 *
 * If (1)-(4) hold but the write or the stub verification fails, everything is
 * rolled back: the handler region goes back to zeros and whichever stub was
 * already redirected is restored to the bytes it had, so the outcome is either
 * both hooks or none.
 *
 * Return codes: 0 patch present; 1 vold absent or --check saw an intact stub;
 * 2 could not parse / find / the premises failed; 3 could not write.
 *
 * /proc/<pid>/mem needs PTRACE_MODE_ATTACH on vold: invoke as ONE simple
 * su -c command (a compound one lands in the shell domain, which cannot ptrace
 * vold; a redirect does not change the domain).
 *
 * Build: cc -std=c11 -Oz -o vold-fusefs vold-fusefs.c
 */

#define _GNU_SOURCE

#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__aarch64__)
#include <sys/ptrace.h>
#include <asm/ptrace.h>   /* struct user_pt_regs, NT_PRSTATUS */
#endif

#include "vold-common.h"   /* the ELF-reading half both vold patchers share */

/* The imported symbol whose stub we replace. */
#define TARGET_SYM "mount"

/* The second imported symbol we replace — vold's teardown primitive.
 *
 * mount alone is not enough, and the reason is in vold's own sources rather
 * than in anything this module does. MountUserFuse() (vold/Utils.cpp:1598)
 * makes exactly ONE mount at /mnt/user/<uid>/emulated. The patch makes TWO:
 * the FUSE mount the caller asked for, plus the raw-tree bind stacked on top.
 * Every teardown path vold has removes exactly ONE mount per call:
 *
 *   ForceUnmount()  vold/Utils.cpp:477   one umount2(UMOUNT_NOFOLLOW)
 *   UnmountTree()   vold/Utils.cpp:1314  one umount2(MNT_DETACH)
 *
 * So UnmountUserFuse() (vold/Utils.cpp:1696) removed only our bind and left
 * the daemon-less FUSE mount behind. Every later access to the path returned
 * ENOTCONN, MountUserFuse() on the next mount failed with "Failed to mount
 * emulated fuse volume: Transport endpoint is not connected", the volume was
 * reported unmountable and apps were back on FUSE — the regression this
 * symbol exists to prevent.
 *
 * See build_umount_handler() for the fix and why it is shaped the way it is. */
#define TARGET_SYM2 "umount2"

/* The one path shape whose teardown has to clear more than one layer.
 *
 * /mnt/user/<uid>/emulated is the only path the patch ever stacks a second
 * mount on (fuse_path in MountUserFuse()). The check is deliberately a shape
 * match and not a remembered string: the handler runs inside vold with its
 * page mapped r-x, so it cannot write to any state of its own, and a static
 * buffer in the handler would fault. See build_umount_handler().
 *
 * The shape is fixed by AOSP and verified against vold/Utils.cpp:1600-1606:
 *   fuse_path = StringPrintf("/mnt/user/%d/%s", user_id, relative_upper_path)
 * with relative_upper_path == "emulated" for the emulated volume (label). The
 * deeper binds (Android/data, Android/obb, the shared-storage volume binds)
 * and the pass-through path are all deliberately NOT matched: each of those
 * carries exactly one mount, so one umount2 is already correct for them. */
#define FUSE_PATH_PREFIX_Q 0x6573752f746e6d2full   /* "/mnt/use" — first 8 bytes */
#define FUSE_PATH_SEP_H    0x2f72u                 /* "r/" — bytes 8 and 9       */
#define FUSE_PATH_TAIL_S   "/emulated"             /* bytes 10+n .. — see build_umount_handler */

/* The raw tree, and the strings that identify the mount we intercept. */
#define RAW_PATH   "/data/media"
#define FUSE_TYPE  "fuse"
#define FUSE_SRC   "/dev/fuse"

/* MS_LAZYTIME: set by MountUserFuse() but NOT by AppFuseUtil::Mount().
 * It is what tells the emulated-storage FUSE apart from the per-app one.
 *
 * The kernel value is (1 << 25) = 0x02000000 — NOT 0x20000. Checked in
 * NDK sysroot/usr/include/linux/mount.h. bionic's <sys/mount.h> defines
 * MS_LAZYTIME with the same number, so this #define is only a fallback. */
#ifndef MS_LAZYTIME
#define MS_LAZYTIME 0x02000000u
#endif

#define EXIT_OK         0
#define EXIT_NO_VOLD    1
#define EXIT_NO_RESOLVE 2
#define EXIT_NO_WRITE   3

/* arm64 instruction words used by the handler (see build_handler). Only these
 * two are emitted: a `nop` and a `mov x16, x0` were carried here as unused
 * spellings until 2026-10-08 and were dropped — an instruction word nobody
 * emits is one more thing to keep in step with the encoders below. */
#define INSN_BTI_JC     0xd50324dfu   /* bti jc                       */
#define INSN_RET        0xd65f03c0u   /* ret                          */

/* Nothing below prints: this tool reports by exit code, and that is what every
 * caller reads (module/post-fs-data.sh, module/service.sh, module/status.sh).
 * The only output left is usage() on a bad invocation and the --selftest report,
 * which is the result of a mode a developer asked for by name. */

/* Build the replacement bytes for a stub: `ldr x17,#8 ; br x17 ; .quad handler`.
 *
 * 16 bytes — exactly the space adrp/ldr/add/br occupied, so no neighbouring stub
 * is disturbed. The layout is forced by that budget:
 *
 *     +0  ldr x17, #8      loads from pc+8 = stub+8
 *     +4  br  x17
 *     +8  .quad handler    the whole 64-bit address, 8 bytes
 *
 * The literal MUST sit at +8. An earlier version opened with `bti jc` and put
 * the quad at +12, which needs 20 bytes: it wrote 4 bytes past the end of this
 * buffer, and since only 16 bytes ever reach vold the top half of the address
 * was never written at all. `ldr x17,#8` then read the handler's low 32 bits in
 * the low half and the NEIGHBOURING stub's `adrp` word (0xd0000030 on the
 * device's vold) in the high half, so `br x17` left for a mangled 64-bit
 * address and vold died with SIGSEGV / SEGV_MAPERR on the first mount() call
 * after patching — no tombstone, because crash_dump cannot attach to vold. On a
 * boot where the patch lands before vold mounts emulated storage that first
 * call IS the emulated mount, so init sees a critical service die and reboots:
 * the reported bootloop.
 *
 * Dropping `bti jc` costs nothing here: this stub is reached only by a direct
 * `bl` from vold's own code, and BTI is checked on indirect branches only. The
 * landing pad belongs on the handler, which the stub reaches with `br x17` — and
 * the handler does open with `bti jc`. */
#define STUB_PATCH_LEN 16
static void build_stub_patch(uint8_t out[STUB_PATCH_LEN], uint64_t handler_va) {
    uint32_t w[2];
    w[0] = STUB_PATCH_W0;                   /* ldr x17, #8  (reads stub+8) */
    w[1] = STUB_PATCH_W1;                   /* br  x17                     */
    memcpy(out + 0, &w[0], 4);
    memcpy(out + 4, &w[1], 4);
    memcpy(out + 8, &handler_va, 8);        /* +8..+15 — fits, nothing spilled */
}

static bool stub_is_patched(const uint8_t *p) {
    uint32_t w0, w1;
    memcpy(&w0, p + 0, 4);
    memcpy(&w1, p + 4, 4);
    return w0 == STUB_PATCH_W0 && w1 == STUB_PATCH_W1;
}

/* Where the patch's `ldr x17,#imm` will read its literal from, as a byte offset
 * from the start of the patch — or -1 if the first word is not that ldr.
 *
 * Decoded rather than assumed, because the offset is the whole contract: the
 * patch has to point the load at the 8-byte quad inside its own 16 bytes, and
 * the bootloop came from a layout where it pointed past the end instead. */
static int stub_literal_off(const uint8_t *p) {
    uint32_t w0;
    memcpy(&w0, p + 0, 4);
    /* LDR (literal), 64-bit: 0x58000000 | imm19<<5 | Rt, with Rt = 17 (x17). */
    if ((w0 & 0xff00001fu) != 0x58000011u) return -1;
    int64_t imm19 = (int64_t)((w0 >> 5) & 0x7ffffu);
    imm19 = (imm19 << 45) >> 45;            /* sign-extend the 19-bit field */
    return (int)(imm19 * 4);
}

/* =================================================================== *
 * The handler
 *
 * Entered from the `mount` stub with the caller's registers intact:
 *   x0 = source   ("/dev/fuse" for the FUSE mount we intercept)
 *   x1 = target   (fuse_path; points into a heap std::string's buffer)
 *   x2 = fstype   ("fuse")
 *   x3 = flags, x4 = data (the "fd=%i,..." option string)
 *   lr = return address inside MountUserFuse
 *
 * Two outcomes:
 *
 *   (a) the call is NOT the emulated-storage FUSE mount
 *       -> tail-jump to the real libc mount with x0..x4 untouched, so the
 *          caller cannot tell the stub was replaced.
 *
 *   (b) the call IS that mount
 *       -> make it, then put the raw tree on top:
 *            mount("/dev/fuse", target, "fuse", x3, x4);   // vold's own call,
 *                                                          // x0..x4 as given
 *            mount("/data/media", target, NULL,
 *                  MS_BIND | MS_REC, NULL);                // bind the raw tree
 *          and return 0 — the value MountUserFuse expects from a successful
 *          mount. Both mounts are wanted: see "why the FUSE mount is still
 *          made" at the top. The bind is what anything actually reaches, and
 *          it is reached the way vold's own BindMount() would reach it.
 *
 * ================ how the call is recognised
 *
 * By POINTER, not by bytes, plus one flag bit:
 *
 *     x2 == type_va  &&  x0 == src_va  &&  (x3 & MS_LAZYTIME)
 *
 * where src_va/type_va are the VAs of the standalone "/dev/fuse" and "fuse"
 * literals — the same two the tool located in order to find this call site
 * (find_fuse_site). vold is PIE, so at runtime the values are base+va and the
 * tool knows both. This is exact: there is no byte pattern to get wrong, and
 * no assumption that the caller passes the literal rather than a copy.
 *
 * The flag bit is not decoration. Measured on the device's vold
 * (md5 2319c26fb4c5ccd1492b16fccc895cc5), vold calls mount(2) in exactly this
 * shape from two places, and the compiler MERGED the string literals — both
 * sites build x0 from 0x14e35 and x2 from 0x15cb3, and both `bl` the same
 * stub at 0xf9de0, so the pointer test alone cannot tell them apart:
 *
 *     0x5f0c8  MountUserFuse()       mov w3,#0x40e; movk w3,#0x200,lsl#16
 *                                    -> w3 = 0x0200040e   (MS_LAZYTIME)
 *     0xa6ffc  AppFuseUtil::Mount()  mov w3,#0x40e
 *                                    -> w3 = 0x0000040e
 *
 * AppFuseUtil mounts the per-app point /mnt/appfuse/<uid>_<name>, whose whole
 * point is that the app sees its own directory and not its neighbours'. Binding
 * /data/media there would hand it the entire tree instead. So the flags are
 * tested, and MS_LAZYTIME — which only MountUserFuse sets — is what separates
 * them. It is the same discriminator find_fuse_site() already relies on to
 * choose the site, so the two halves of the tool now agree.
 *
 * Targeting by arguments (not by call site) is deliberate: it means a vendor
 * that reorganises MountUserFuse's internals, inlines it, or adds another call
 * through the same stub is still handled correctly, because the decision is
 * made on what the call actually asks for. The flag test is what makes that
 * promise safe when the extra call happens to look identical.
 *
 * ================ register discipline
 *
 * The handler stands in for a real `mount` call, so it must be a well-behaved
 * callee: every callee-saved register the caller relies on has to come back
 * untouched. The whole handler therefore uses only scratch registers —
 * x0..x4 (arguments), x9 (the ABI's scratch register) and x10 for the target
 * across the two calls. No callee-saved register is written at all.
 *
 * The pass-through path in particular must preserve x0..x18 exactly as the
 * caller left them: classification uses only x9, and the path restores the
 * frame before tail-calling libc mount with the arguments it was given.
 *
 * The take-over path does not return mount's own effects, it synthesises the
 * result — but the caller's view of the callee-saved set still has to be
 * intact, which is why the target is parked in x10 rather than the x19 an
 * earlier revision used.
 *
 * ================ why the frame looks the way it does
 *
 * `stp x29, x30, [sp, #-16]!` is emitted once, before the comparisons. The
 * take-over path pops it before returning, because it returns through the
 * original `lr`. The pass-through path does NOT pop it — it never returns to
 * us; it tail-calls libc mount, which returns straight to the caller. libc
 * mount does not care what sits above sp, and the caller's `bl mount` pushed
 * only its own frame, so the extra 16 bytes are harmless there. */

/* --- encoders, so the emission below reads as assembly --- */
static uint32_t enc_ldr_lit(int rt, int64_t byte_delta) {
    int64_t d = byte_delta >> 2;                     /* ldr Xt, [pc,#imm19*4] */
    return 0x58000000u | ((uint32_t)(d & 0x7ffff) << 5) | (uint32_t)rt;
}
static uint32_t enc_cmp_reg(int rn, int rm) {        /* cmp Xn, Xm */
    return 0xeb00001fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static uint32_t enc_b_cond(int cond, int64_t byte_delta) {
    int64_t d = byte_delta >> 2;
    return 0x54000000u | ((uint32_t)(d & 0x7ffff) << 5) | (uint32_t)(cond & 0xf);
}
/* Decode a b.cond word back to a word-count delta (signed, pc-relative, in
 * instructions). Used to prove branch encodings are legal, not merely that the
 * counters add up: a wrong imm19 is a jump into the void, not a count error. */
static int dec_b_cond(int64_t *delta_words, uint32_t w) {
    int64_t d = (int64_t)((w >> 5) & 0x7ffffu);
    if (d & (1 << 18)) d -= (1 << 19);
    *delta_words = d;
    return (int)(w & 0xfu);         /* condition code */
}
static uint32_t enc_movz_w(int rd, uint32_t imm16) {
    return 0x52800000u | ((imm16 & 0xffffu) << 5) | (uint32_t)rd;
}
static uint32_t enc_mov_reg(int rd, int rm) {        /* mov Xd, Xm */
    return 0xaa0003e0u | ((uint32_t)rm << 16) | (uint32_t)rd;
}
static uint32_t enc_blr(int rn) { return 0xd63f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_br(int rn)  { return 0xd61f0000u | ((uint32_t)rn << 5); }
static uint32_t enc_stp_pre(int rt, int rt2, int imm7) {
    return 0xa9800000u | ((uint32_t)(imm7 & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}
static uint32_t enc_ldp_post(int rt, int rt2, int imm7) {
    return 0xa8c00000u | ((uint32_t)(imm7 & 0x7f) << 15) |
           ((uint32_t)rt2 << 10) | (31u << 5) | (uint32_t)rt;
}

/* --- the wider set the umount2 handler needs ---------------------------
 *
 * These exist so the emission below reads as assembly rather than as magic
 * words. Every one of them is a direct transcription of the ARM ARM encoding,
 * and each is checked by selftest() — a wrong immediate here is not a wrong
 * count, it is vold jumping into the void on the first unmount. */
static uint32_t enc_cmp_w_reg(int rn, int rm) {      /* cmp Wn, Wm */
    return 0x6b00001fu | ((uint32_t)rm << 16) | ((uint32_t)rn << 5);
}
static uint32_t enc_ldr_imm(int rt, int rn, unsigned off) {   /* ldr Xt, [Xn,#off] */
    return 0xf9400000u | ((off / 8u) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_str_imm(int rt, int rn, unsigned off) {   /* str Xt, [Xn,#off] */
    return 0xf9000000u | ((off / 8u) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_ldrh_imm(int rt, int rn, unsigned off) {  /* ldrh Wt, [Xn,#off] */
    return 0x79400000u | ((off / 2u) << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_ldrb_imm(int rt, int rn, unsigned off) {  /* ldrb Wt, [Xn,#off] */
    return 0x39400000u | (off << 10) | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_ldrb_post(int rt, int rn) {               /* ldrb Wt, [Xn], #1 */
    return 0x38401400u | ((uint32_t)rn << 5) | (uint32_t)rt;
}
static uint32_t enc_add_imm(int rd, int rn, unsigned imm) {   /* add Xd, Xn, #imm */
    return 0x91000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_sub_w_imm(int rd, int rn, unsigned imm) { /* sub Wd, Wn, #imm */
    return 0x51000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
static uint32_t enc_subs_w_imm(int rd, int rn, unsigned imm){ /* subs Wd, Wn, #imm */
    return 0x71000000u | (imm << 10) | ((uint32_t)rn << 5) | (uint32_t)rd;
}
/* CBZ/CBNZ (32-bit) is `0x34/0x35 | imm19<<5 | Rt`: the base carries the
 * opcode, sf=0 and op=0/1 in bit 24, and imm19 starts at bit 5. The first
 * version of this used 0x35800000, which sets bit 27 — a different instruction
 * with a zero imm19 and op=0, i.e. a jump into the void. selftest() caught it
 * by decoding the emitted word back; that is why the decoder exists. */
static uint32_t enc_cbnz_w(int rt, int64_t byte_delta) {      /* cbnz Wt, #delta */
    int64_t d = byte_delta >> 2;
    return 0x35000000u | ((uint32_t)(d & 0x7ffff) << 5) | (uint32_t)rt;
}
static uint32_t enc_b(int64_t byte_delta) {                   /* b #delta */
    int64_t d = byte_delta >> 2;
    return 0x14000000u | (uint32_t)(d & 0x3ffffff);
}
/* --- the three the anchor test needs -----------------------------------
 *
 * Nothing in the handler emission uses these; they exist so anchor_selftest()
 * can write a call site out as instructions instead of as magic words. That is
 * the whole point of that test: it asserts the *matcher*, which is what decides
 * whether vold can be patched at all. */
static uint32_t enc_adrp(int rd, uint64_t pc, uint64_t target) {  /* adrp Rd, target */
    int64_t delta = (int64_t)((target & ~0xfffULL) - (pc & ~0xfffULL)) >> 12;
    uint32_t immlo = (uint32_t)delta & 0x3u;
    uint32_t immhi = ((uint32_t)delta >> 2) & 0x7ffffu;
    return 0x90000000u | (immlo << 29) | (immhi << 5) | (uint32_t)rd;
}
static uint32_t enc_movk_w(int rd, uint32_t imm16) {   /* movk Wd, #imm16, lsl 16 */
    return 0x72a00000u | ((imm16 & 0xffffu) << 5) | (uint32_t)rd;
}
static uint32_t enc_bl(uint64_t pc, uint64_t target) {        /* bl target */
    int64_t d = (int64_t)(target - pc) >> 2;
    return 0x94000000u | (uint32_t)(d & 0x3ffffff);
}
/* Decoders for the same two forms, used to prove an emitted branch actually
 * reaches its label instead of merely having the right word count. */
static int64_t dec_b(uint32_t w) {
    int64_t d = (int64_t)(w & 0x3ffffffu);
    if (d & (1 << 25)) d -= (1 << 26);
    return d;
}
static int64_t dec_cbnz_w(uint32_t w, int *rt_out, int *is_nz) {
    int64_t d = (int64_t)((w >> 5) & 0x7ffffu);
    if (d & (1 << 18)) d -= (1 << 19);
    *rt_out = (int)(w & 0x1fu);
    *is_nz  = (int)((w >> 24) & 1u);
    return d;
}

#define COND_NE 0x1
#define COND_EQ 0x0
#define COND_HI 0x8

/* Named pool slots; the enum and the `pool_val[]` assignment must stay in the
 * same order — that is the only invariant in this function. */
enum {
    P_TYPE = 0,     /* runtime VA of vold's "fuse" literal       */
    P_SRC,          /* runtime VA of vold's "/dev/fuse" literal  */
    P_MOUNT,        /* runtime VA of libc mount                  */
    P_RAW,          /* runtime VA of the "/data/media\0" copy    */
    P_COUNT
};

/* Assemble the handler into `out`.
 *
 * out_cap must be >= 256. Returns bytes written (code + pool + string), or -1.
 * `raw_string` is copied after the pool and NUL-terminated; P_RAW points at it.
 */
static int build_handler(uint8_t *out, size_t out_cap, uint64_t code_va,
                         uint64_t real_mount,
                         uint64_t src_va, uint64_t type_va,
                         const char *raw_string) {
    uint32_t w[96];
    uint32_t fld_pool[32];    /* which pool slot each ldr targets */
    uint32_t fld_rt[32];      /* which register each ldr loads into */
    int      fld_at[32];      /* word index of each ldr           */
    int      nfld = 0;
    int      br_at[8];        /* word index of each b.cond        */
    int      br_cond[8];      /* ...and the condition it tests    */
    int      nbr = 0;
    int      n = 0;
    int      idx_restore = -1;   /* word index of the pass-path frame restore */
    int      idx_pass_tail = -1; /* word index of the pass-path `ldr x9` */

    if (out_cap < 256) return -1;
    size_t raw_len = strlen(raw_string) + 1;
    if (raw_len > 64) return -1;

#define E(x) do { if (n >= 88) return -1; w[n++] = (uint32_t)(x); } while (0)
#define LDR(slot, rt) do {                                            \
        if (nfld >= 32) return -1;                                    \
        fld_pool[nfld] = (uint32_t)(slot); fld_rt[nfld] = (uint32_t)(rt); \
        fld_at[nfld] = n; nfld++;                                     \
        E(0);                    /* placeholder, fixed up below */    \
    } while (0)
#define BNE() do { if (nbr >= 8) return -1; br_at[nbr] = n; \
        br_cond[nbr] = COND_NE; nbr++; E(0); } while (0)
#define BEQ() do { if (nbr >= 8) return -1; br_at[nbr] = n; \
        br_cond[nbr] = COND_EQ; nbr++; E(0); } while (0)

    /* bti jc — the stub reaches us with `br x17`, an indirect jump. This is the
     * landing pad; the stub itself cannot carry one (see build_stub_patch). */
    E(INSN_BTI_JC);
    E(enc_stp_pre(29, 30, -2));         /* stp x29, x30, [sp, #-16]! */
    E(0x910003fdu);                     /* mov x29, sp               */

    /* classify: x2 == "fuse" */
    LDR(P_TYPE, 9);                     /* ldr x9, <type>   */
    E(enc_cmp_reg(2, 9));               /* cmp x2, x9       */
    BNE();                              /* b.ne pass        */

    /* classify: x0 == "/dev/fuse" */
    LDR(P_SRC, 9);                      /* ldr x9, <src>    */
    E(enc_cmp_reg(0, 9));               /* cmp x0, x9       */
    BNE();                              /* b.ne pass        */

    /* classify: x3 & MS_LAZYTIME. The two tests above are not enough: vold
     * calls mount(2) this way from TWO places and the compiler merged the
     * string literals, so both reach the handler with the identical x0 and x2
     * pointers —
     *
     *   MountUserFuse()      w3 = 0x40e | MS_LAZYTIME = 0x0200040e
     *   AppFuseUtil::Mount() w3 = 0x40e               (no MS_LAZYTIME)
     *
     * — and AppFuseUtil's target is the per-app point /mnt/appfuse/<uid>_<name>,
     * not the emulated volume. Binding /data/media over that would hand the app
     * the whole tree where it asked for its own directory, so it must pass
     * through untouched. MS_LAZYTIME is the only thing that separates them, and
     * it is the same discriminator find_fuse_site() uses to pick the site; see
     * "how the call is recognised". */
    E(0x52a04009u);                     /* movz w9, #0x200, lsl #16 */
    E(0x6a09007fu);                     /* tst  w3, w9              */
    BEQ();                              /* b.eq pass — not our mount */

    /* ---------------- take over ---------------- */

    /* The target is parked in x10, a caller-saved temporary, NOT in x19: the
     * handler is entered from the PLT stub in place of a real `mount` call, so
     * the caller's callee-saved registers must survive untouched. x19 is
     * callee-saved and the handler has no frame slot for it — using it here
     * would corrupt whatever the caller kept there across the call. */
    E(enc_mov_reg(10, 1));              /* mov x10, x1  (target)           */

    /* (a) the FUSE mount the caller asked for — x0..x4 are still exactly its
     * arguments, so there is nothing to set up. It has to happen: the fd it
     * leaves behind is what MediaProvider's FUSE daemon is started on, and a
     * volume whose daemon does not come up is reported "unmountable". */
    LDR(P_MOUNT, 9);                    /* ldr x9, <mount>                 */
    E(enc_blr(9));                      /* blr x9                          */

    /* (b) the raw tree on top of it. This is the mount anything actually sees:
     * it is the topmost mount at fuse_path, so the FUSE view below it is
     * unreachable through the path. */
    LDR(P_MOUNT, 9);                    /* ldr x9, <mount>                 */
    LDR(P_RAW, 0);                      /* ldr x0, <raw>                   */
    E(enc_mov_reg(1, 10));              /* mov x1, x10                     */
    E(0xd2800002u);                     /* mov x2, #0     (fstype = NULL)  */
    E(enc_movz_w(3, 4096u | 16384u));   /* mov w3,#0x5000 MS_BIND|MS_REC   */
    E(0xd2800004u);                     /* mov x4, #0     (data   = NULL)  */
    E(enc_blr(9));                      /* blr x9                          */

    E(enc_movz_w(0, 0));                /* mov w0, #0    -> "mount worked"  */
    E(enc_ldp_post(29, 30, 2));         /* ldp x29, x30, [sp], #16          */
    E(0xd65f03c0u);                     /* ret                              */

    /* ---------------- pass ----------------
     *
     * This path is entered with the frame pushed at the top (words 1-2) still
     * live, so it must be undone before we leave. It looks like it could be
     * skipped — we never return "through" the push — but it cannot: the caller
     * expects sp to be exactly where it was when it called the stub, and the
     * tail-called libc mount returns to that caller with whatever sp we hand
     * it. Leaving the push in place shifts sp by 16 bytes and the caller's own
     * epilogue then reads the wrong frame.
     *
     * The index of the restore is captured separately from the index of the
     * block: the two `b.ne` above must land on the restore, not after it. */
    idx_restore = n;
    E(enc_ldp_post(29, 30, 2));         /* ldp x29, x30, [sp], #16        */
    idx_pass_tail = n;
    LDR(P_MOUNT, 9);                    /* ldr x9, <mount>                */
    E(enc_br(9));                       /* br x9 — x0..x4 exactly as given */
    if (idx_restore == idx_pass_tail) return -1;

#undef E
#undef LDR
#undef BNE
#undef BEQ

    /* ---- pool + string placement ---- */
    while ((n & 1) != 0) w[n++] = 0x00000000u;      /* align pool to 8 bytes */
    int pool_idx = n;
    uint64_t pool_va = code_va + (uint64_t)pool_idx * 4u;

    n += P_COUNT * 2;                                /* 8 bytes each */
    int str_off_bytes = n * 4;
    int str_words = (int)((raw_len + 3) / 4);
    n += str_words;
    if (n > 96) return -1;
    uint64_t str_va = code_va + (uint64_t)str_off_bytes;

    /* ---- fix up the ldr literals ---- */
    for (int i = 0; i < nfld; i++) {
        int at = fld_at[i];
        uint64_t target_va = pool_va + (uint64_t)fld_pool[i] * 8u;
        uint64_t at_va = code_va + (uint64_t)at * 4u;
        w[at] = enc_ldr_lit((int)fld_rt[i],
                            (int64_t)target_va - (int64_t)at_va);
    }

    /* ---- fix up the b.cond to reach the pass block ----
     *
     * Every classification branch — the two `b.ne` on the argument pointers and
     * the `b.eq` on MS_LAZYTIME — jumps forward to the frame restore at the
     * head of the pass block (`ldp x29,x30,[sp],#16`), not to the tail-call
     * after it: landing on the tail-call would hand libc mount a frame that is
     * still pushed. The delta is in instructions, pc-relative; the encoder
     * masks it to imm19, so the distance must fit, which is asserted rather
     * than assumed. Each branch keeps its own condition code. */
    if (idx_restore < 0 || idx_pass_tail < 0) return -1;
    if (nbr == 0) return -1;
    for (int i = 0; i < nbr; i++) {
        int at = br_at[i];
        if (at >= idx_restore) return -1;   /* must jump forward */
        int64_t delta_words = (int64_t)idx_restore - (int64_t)at;
        if (delta_words < -(1 << 18) || delta_words >= (1 << 18)) return -1;
        w[at] = enc_b_cond(br_cond[i], delta_words * 4);
        /* Prove the encoding survived: decode it back and require the word we
         * get to carry the condition we meant and point where we meant. If the
         * decoder disagrees with the encoder the encoder is lying, and this is
         * worth failing on. */
        int64_t got = 0;
        int cond = dec_b_cond(&got, w[at]);
        if (cond != br_cond[i] || at + got != idx_restore) return -1;
    }

    /* ---- write the pool (each entry is two 32-bit words) ---- */
    uint64_t pool_val[P_COUNT];
    pool_val[P_TYPE]    = type_va;
    pool_val[P_SRC]     = src_va;
    pool_val[P_MOUNT]   = real_mount;
    pool_val[P_RAW]     = str_va;
    for (int i = 0; i < P_COUNT; i++) {
        w[pool_idx + i * 2 + 0] = (uint32_t)(pool_val[i] & 0xffffffffu);
        w[pool_idx + i * 2 + 1] = (uint32_t)(pool_val[i] >> 32);
    }

    /* ---- write everything out: code + pool + string ---- */
    memset(out, 0, (size_t)n * 4u);
    memcpy(out, w, (size_t)n * 4u);
    memcpy(out + str_off_bytes, raw_string, raw_len);

    return n * 4;
}

typedef struct {
    uint64_t got_slot;
    uint64_t stub_va;
    uint32_t sym_index;
    int      reloc_index;
    int      plt_delta;
} Resolved;

/* ------------------------------------------------------------------ *
 * The umount2 handler — completing the teardown
 *
 * ================ what is wrong without it
 *
 * See TARGET_SYM2: the patch stacks two mounts at fuse_path and every vold
 * teardown removes exactly one, so the FUSE mount outlives its daemon and the
 * volume becomes permanently unmountable. The fix is to make the teardown
 * remove every layer the patch added, so the invariant vold's own code assumes
 * — "one umount2 clears fuse_path" — is true again.
 *
 * ================ why the path is recognised by shape, not by memory
 *
 * The natural implementation is to remember fuse_path when the mount handler
 * takes over and compare against it here. It cannot be done: the handler is
 * written into a page vold has mapped r-x (the tail of the .plt page, or a
 * PROT_READ|PROT_EXEC mapping of vold's own binary — see find_handler_room and
 * vold_exec_page), and writes through /proc/<pid>/mem are what make the patch
 * possible at all. A `str` into that page from the handler itself would take a
 * protection fault and kill vold, so the handler has to be position-independent
 * AND state-free: it may only read the literal pool that ships with it.
 *
 * The path is therefore matched by shape — exactly, not as a prefix:
 *
 *   "/mnt/user/" <one or more ASCII digits> "/emulated" <NUL>
 *
 * so the width of the user id does not matter and nothing that merely starts
 * like the path can slip through:
 *
 *   /mnt/user/0/emulated                 match
 *   /mnt/user/10/emulated                match
 *   /mnt/user//emulated                  no  — the digit loop consumed nothing
 *   /mnt/user/0/emulated/0/Android/data  no  — the tail compare sees '/'
 *   /mnt/user/0/emulatedx                no  — the tail compare sees 'x'
 *   /mnt/user/0/emulate                  no  — the tail compare hits the NUL
 *   /mnt/pass_through/0/emulated         no  — the head compare fails
 *
 * selftest() runs every one of those through the emitted machine code.
 *
 * ================ why no read can leave the string
 *
 * The matcher must never read past the NUL of the path it is handed, and that
 * is a real constraint rather than a theoretical one: vold unmounts short
 * paths too (`/data`, `/proc`, every mount point in /proc/mounts reaches
 * ForceUnmount through VolumeManager::tearDownStaleMounts), and a path is a
 * std::string whose buffer is only as long as the string. Every access is
 * therefore bounded by construction:
 *
 *   - the head is one 8-byte load at offset 0. Any std::string has at least a
 *     23-byte SSO buffer or a heap buffer of at least its own length, so this
 *     one is in bounds even for "/data";
 *   - the digit loop stops at the first non-digit, and a NUL is not a digit;
 *   - the tail is a byte-by-byte compare against "/emulated\0" that stops at
 *     the first mismatch — and a NUL never equals a non-NUL template byte, so
 *     the loop always stops at or before the path's own NUL. A single 10-byte
 *     compare here would have read up to 9 bytes past a short string, which is
 *     why it is written as a loop.
 *
 * ================ why a bounded loop, not exactly two unmounts
 *
 * Two layers is the normal case, but the handler cannot know it is looking at
 * the normal case: it may also run against a vold that was mounted before the
 * patch was installed (one layer), or after a partial teardown. "Unmount until
 * the kernel says there is nothing left" is right in all of them, and EINVAL /
 * ENOENT is the only honest signal that the path is clear. The cap of four is
 * a guard against a path remounted underneath us, not a functional limit — it
 * is twice what the patch can ever create.
 *
 * ================ why the last result is returned, not a synthesised 0
 *
 * ForceUnmount() (vold/Utils.cpp:477) uses the first umount2's return value as
 * its only success test, and escalates through SIGINT/SIGTERM/SIGKILL when it
 * fails. Reporting success while a layer is still mounted would hand vold a
 * clean bill of health for a dirty teardown — precisely the state that caused
 * the ENOTCONN regression. So the loop stops on the first failure and returns
 * that failure with errno intact:
 *
 *   one layer,  not busy : call 1 removes it,   call 2 gives EINVAL -> EINVAL
 *   two layers, not busy : calls 1,2 remove them, call 3 gives EINVAL -> EINVAL
 *   either,     busy     : the failing call's errno is returned      -> EBUSY
 *
 * EINVAL is exactly what vold already treats as "unmounted" — ForceUnmount()
 * checks `errno == EINVAL` (Utils.cpp:479), so do UnmountTree() (1315) and
 * UnmountFusePaths() (1733) — so a fully cleared path reports what an
 * unpatched single-layer unmount reported. An EBUSY still reaches the
 * escalation path.
 *
 * ================ why the arguments are spilled to the stack
 *
 * libc umount2 is a real call and may clobber x0..x18, so the path and the
 * flags have to survive it. The handler may not touch any callee-saved
 * register either: it stands in for a call the caller made, so x19..x28 must
 * come back untouched. The frame is therefore 32 bytes — the usual x29/x30
 * pair plus two slots for the arguments — and nothing else is needed.
 *
 * The pass path leaves x0/x1 exactly as the caller set them and tail-calls
 * libc umount2, so an ordinary unmount is bit-for-bit what it was before the
 * patch: same arguments, same return value, same errno. */

enum {
    U_MNTPFX = 0,   /* the constant "/mnt/use"         */
    U_TAIL,         /* VA of the "/emulated\0" copy    */
    U_UMOUNT2,      /* runtime VA of libc umount2      */
    U_POOL_COUNT
};

/* Labels inside the umount2 handler, recorded as word indices while emitting
 * and resolved into pc-relative deltas afterwards — the same two-step
 * build_handler() uses for its `pass` block, generalised because this handler
 * branches both forward and backward. */
enum { L_PASS = 0, L_DIGITS, L_DIGITS_DONE, L_TAIL, L_LOOP, L_DONE,
       L_LABEL_COUNT };

static int build_umount_handler(uint8_t *out, size_t out_cap, uint64_t code_va,
                                uint64_t real_umount2) {
    static const char tail[] = FUSE_PATH_TAIL_S;   /* the NUL is added below */
    uint32_t w[96];
    uint32_t fld_slot[8];
    uint32_t fld_rt[8];
    int      fld_at[8];
    int      nfld = 0;
    /* kind: 0 = b.cond, 1 = b, 2 = cbnz W<rt> */
    struct { int at; uint32_t cond; int label; int kind; int rt; } fx[20];
    int      nfx = 0;
    int      label_at[L_LABEL_COUNT];
    int      n = 0;

    if (out_cap < 256) return -1;
    for (int i = 0; i < L_LABEL_COUNT; i++) label_at[i] = -1;

/* The limits below are the handler's own budget. Exceeding one is a refusal
 * (return -1), not a truncated handler: a handler that outgrows its space must
 * not be written into vold at all. */
#define UE(x) do {                                                    \
        if (n >= 72) return -1;                                       \
        w[n++] = (uint32_t)(x);                                       \
    } while (0)
#define ULDR(slot, rt) do {                                           \
        if (nfld >= 8) return -1;                                     \
        fld_slot[nfld] = (uint32_t)(slot); fld_rt[nfld] = (uint32_t)(rt); \
        fld_at[nfld] = n; nfld++;                                     \
        UE(0);                     /* placeholder, fixed up below */  \
    } while (0)
#define ULBL(l) do { label_at[l] = n; } while (0)
#define UBC(cc, l) do {                                               \
        if (nfx >= 20) return -1;                                     \
        fx[nfx].at = n; fx[nfx].cond = (uint32_t)(cc);                \
        fx[nfx].label = (l); fx[nfx].kind = 0; fx[nfx].rt = -1;       \
        nfx++; UE(0);                                                 \
    } while (0)
#define UB(l) do {                                                    \
        if (nfx >= 20) return -1;                                     \
        fx[nfx].at = n; fx[nfx].cond = 0;                             \
        fx[nfx].label = (l); fx[nfx].kind = 1; fx[nfx].rt = -1;       \
        nfx++; UE(0);                                                 \
    } while (0)
#define UCBZNZ(reg, l) do {                                           \
        if (nfx >= 20) return -1;                                     \
        fx[nfx].at = n; fx[nfx].cond = 0;                             \
        fx[nfx].label = (l); fx[nfx].kind = 2; fx[nfx].rt = (reg);    \
        nfx++; UE(0);                                                 \
    } while (0)

    UE(INSN_BTI_JC);                    /* bti jc — reached by `br x17`  */
    UE(enc_stp_pre(29, 30, -4));        /* stp x29, x30, [sp, #-32]!     */
    UE(0x910003fdu);                    /* mov x29, sp                   */
    UE(enc_str_imm(0, 31, 16));         /* str x0, [sp, #16]   path      */
    UE(enc_str_imm(1, 31, 24));         /* str x1, [sp, #24]   flags     */

    /* ---- shape: "/mnt/user/<digits>/emulated\0" ---- */
    UE(enc_ldr_imm(9, 0, 0));           /* ldr x9, [x0]                  */
    ULDR(U_MNTPFX, 10);                 /* ldr x10, <"/mnt/use">         */
    UE(enc_cmp_reg(9, 10));             /* cmp x9, x10                   */
    UBC(COND_NE, L_PASS);

    UE(enc_ldrh_imm(9, 0, 8));          /* ldrh w9, [x0, #8]             */
    UE(enc_movz_w(10, FUSE_PATH_SEP_H));/* movz w10, #"r/"               */
    UE(enc_cmp_w_reg(9, 10));
    UBC(COND_NE, L_PASS);

    UE(enc_add_imm(11, 0, 10));         /* add x11, x0, #10              */
    UE(enc_mov_reg(12, 11));            /* mov x12, x11  (digits begin)  */
    ULBL(L_DIGITS);
    UE(enc_ldrb_imm(9, 11, 0));         /* ldrb w9, [x11]                */
    UE(enc_sub_w_imm(10, 9, '0'));
    UE(enc_subs_w_imm(10, 10, 9));      /* cmp w10, #9                   */
    UBC(COND_HI, L_DIGITS_DONE);
    UE(enc_add_imm(11, 11, 1));
    UB(L_DIGITS);
    ULBL(L_DIGITS_DONE);
    UE(enc_cmp_reg(11, 12));            /* cmp x11, x12                  */
    UBC(COND_EQ, L_PASS);               /* no digit consumed at all      */

    /* The tail is a byte compare, not a 10-byte one: it stops at the path's
     * own NUL, so it cannot read past the end of a short string. See the note
     * above the function. */
    ULDR(U_TAIL, 13);                   /* ldr x13, <"/emulated">        */
    ULBL(L_TAIL);
    UE(enc_ldrb_post(9, 11));           /* ldrb w9,  [x11], #1           */
    UE(enc_ldrb_post(10, 13));          /* ldrb w10, [x13], #1           */
    UE(enc_cmp_w_reg(9, 10));
    UBC(COND_NE, L_PASS);
    UCBZNZ(9, L_TAIL);                  /* cbnz w9, tail — stop on NUL   */

    /* ---- clear every layer; stop at the first refusal ---- */
    UE(enc_movz_w(12, 4));              /* mov w12, #4                   */
    ULBL(L_LOOP);
    UE(enc_ldr_imm(0, 31, 16));         /* ldr x0, [sp, #16]             */
    UE(enc_ldr_imm(1, 31, 24));         /* ldr x1, [sp, #24]             */
    ULDR(U_UMOUNT2, 9);                 /* ldr x9, <umount2>             */
    UE(enc_blr(9));                     /* blr x9                        */
    UCBZNZ(0, L_DONE);                  /* cbnz w0, done                 */
    UE(enc_subs_w_imm(12, 12, 1));      /* subs w12, w12, #1             */
    UBC(COND_NE, L_LOOP);

    ULBL(L_DONE);
    UE(enc_ldp_post(29, 30, 4));        /* ldp x29, x30, [sp], #32       */
    UE(INSN_RET);

    ULBL(L_PASS);
    UE(enc_ldp_post(29, 30, 4));        /* ldp x29, x30, [sp], #32       */
    ULDR(U_UMOUNT2, 9);
    UE(enc_br(9));                      /* br x9 — x0/x1 exactly as given */

#undef UE
#undef ULDR
#undef ULBL
#undef UBC
#undef UB
#undef UCBZNZ

    /* ---- pool + tail string ---- */
    while ((n & 1) != 0) w[n++] = 0x00000000u;      /* align pool to 8 bytes */
    int pool_idx = n;
    uint64_t pool_va = code_va + (uint64_t)pool_idx * 4u;
    n += U_POOL_COUNT * 2;                           /* 8 bytes each */
    int str_off_bytes = n * 4;
    int str_words = (int)((sizeof(tail) + 3) / 4);   /* sizeof includes the NUL */
    n += str_words;
    if (n > 96) return -1;
    uint64_t str_va = code_va + (uint64_t)str_off_bytes;

    uint64_t pool_val[U_POOL_COUNT];
    pool_val[U_MNTPFX]  = FUSE_PATH_PREFIX_Q;
    pool_val[U_TAIL]    = str_va;
    pool_val[U_UMOUNT2] = real_umount2;

    for (int i = 0; i < nfld; i++) {
        int at = fld_at[i];
        uint64_t target_va = pool_va + (uint64_t)fld_slot[i] * 8u;
        uint64_t at_va = code_va + (uint64_t)at * 4u;
        w[at] = enc_ldr_lit((int)fld_rt[i], (int64_t)target_va - (int64_t)at_va);
    }
    for (int i = 0; i < U_POOL_COUNT; i++) {
        w[pool_idx + i * 2 + 0] = (uint32_t)(pool_val[i] & 0xffffffffu);
        w[pool_idx + i * 2 + 1] = (uint32_t)(pool_val[i] >> 32);
    }

    /* ---- resolve every branch, then prove it by decoding it back ----
     *
     * A branch with the right word count but a wrong imm19 is a jump into the
     * void, and this handler runs on the unmount path of every volume — so the
     * delta is decoded back out of the emitted word and required to land on the
     * label, exactly as build_handler() does for its `pass` block. */
    for (int i = 0; i < nfx; i++) {
        int at  = fx[i].at;
        int tgt = label_at[fx[i].label];
        if (tgt < 0) return -1;
        int64_t dw = (int64_t)tgt - (int64_t)at;
        if (dw == 0) return -1;
        if (dw < -(1 << 18) || dw >= (1 << 18)) return -1;

        if (fx[i].kind == 0) {
            w[at] = enc_b_cond(fx[i].cond, dw * 4);
            int64_t got = 0;
            int cc = dec_b_cond(&got, w[at]);
            if (cc != (int)fx[i].cond || at + got != tgt) return -1;
        } else if (fx[i].kind == 1) {
            w[at] = enc_b(dw * 4);
            if (at + dec_b(w[at]) != tgt) return -1;
        } else {
            w[at] = enc_cbnz_w(fx[i].rt, dw * 4);
            int rt = -1, nz = -1;
            int64_t got = dec_cbnz_w(w[at], &rt, &nz);
            if (rt != fx[i].rt || nz != 1 || at + got != tgt) return -1;
        }
    }

    memset(out, 0, (size_t)n * 4u);
    memcpy(out, w, (size_t)n * 4u);
    memcpy(out + str_off_bytes, tail, sizeof(tail));
    return n * 4;
}

/* ------------------------------------------------------------------ *
 * getting an executable page inside vold
 *
 * The .plt is the cheapest home for the handler, but only when the linker
 * left padding after the last stub. A build that packs its PLT flush to the
 * end of the executable segment has no such room and no zero run anywhere in
 * .text either (measured on the device's vold: 0 bytes after the last stub,
 * no zero run >= 256 bytes in the whole segment). Then the handler needs
 * memory the image does not provide.
 *
 * Anonymous executable memory is not available: measured on the device, both
 * mmap(PROT_READ|PROT_WRITE|PROT_EXEC, MAP_ANONYMOUS) and mprotect(rw anon,
 * PROT_EXEC) come back -EACCES (W^X / SELinux execmem). mmap of an anonymous
 * RX mapping is refused too.
 *
 * What IS allowed is a private executable mapping of the process's own
 * executable file: vold already has execute permission on its binary, so
 * mapping it PROT_READ|PROT_EXEC|MAP_PRIVATE succeeds, and a write into that
 * page through /proc/<pid>/mem (which uses FOLL_FORCE) breaks COW into a
 * private page that keeps PROT_EXEC. Measured end to end: a page mapped this
 * way runs code written into it.
 *
 * So the page is obtained by asking vold itself to do the two syscalls:
 *
 *   fd   = openat(AT_FDCWD, <vold's own path>, O_RDONLY)
 *   home = mmap(NULL, 4096, PROT_READ|PROT_EXEC, MAP_PRIVATE, fd, 0)
 *
 * The path string is placed in a scratch page first, itself obtained with an
 * mmap(PROT_READ|PROT_WRITE, MAP_ANONYMOUS) injection (that one is allowed).
 *
 * Each syscall is run the classic way: attach, save the thread's register
 * file, overwrite the two instructions at the stopped pc with `svc #0` then
 * `brk #0` (PTRACE_POKEDATA bypasses page permissions, so an r-x page works),
 * set x8 and x0..x5, resume, and read x0 back at the brk. The thread is
 * stopped for the whole thing, and its pc and the two clobbered words are
 * restored before it resumes.
 * ------------------------------------------------------------------ */

#if defined(__aarch64__)

#define INSN_SVC_0  0xd4000001u   /* svc #0 */
#define INSN_BRK_0  0xd4200000u   /* brk #0 — the stop after the syscall */

/* arm64 syscall numbers used here. */
#define NR_MMAP     222
#define NR_MPROTECT 226
#define NR_OPENAT   56
#define NR_CLOSE    57

/* Run one syscall inside `pid` from its stopped pc. On success `*ret` holds
 * the raw x0 and the thread is left exactly as it was (pc restored). */
static int inj_syscall(pid_t pid, uint64_t nr, uint64_t a0, uint64_t a1,
                       uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5,
                       int64_t *ret) {
    struct user_pt_regs regs;
    struct iovec iov = { .iov_base = &regs, .iov_len = sizeof(regs) };
    if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &iov) == -1) return -1;

    uint64_t pc0 = regs.pc;
    errno = 0;
    long w0 = ptrace(PTRACE_PEEKDATA, pid, (void *)pc0, NULL);
    if (w0 == -1 && errno != 0) return -1;
    errno = 0;
    long w1 = ptrace(PTRACE_PEEKDATA, pid, (void *)(pc0 + 4), NULL);
    if (w1 == -1 && errno != 0) return -1;

    if (ptrace(PTRACE_POKEDATA, pid, (void *)pc0,
               (void *)(uintptr_t)INSN_SVC_0) == -1) return -1;
    if (ptrace(PTRACE_POKEDATA, pid, (void *)(pc0 + 4),
               (void *)(uintptr_t)INSN_BRK_0) == -1) return -1;

    struct user_pt_regs saved = regs;
    regs.pc = pc0;
    regs.regs[8] = nr;
    regs.regs[0] = a0; regs.regs[1] = a1; regs.regs[2] = a2;
    regs.regs[3] = a3; regs.regs[4] = a4; regs.regs[5] = a5;
    if (ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov) == -1) {
        goto restore;
    }
    if (ptrace(PTRACE_CONT, pid, NULL, NULL) == -1) goto restore;

    {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) goto restore;
        if (WIFSTOPPED(status)) {
            struct user_pt_regs got;
            struct iovec giov = { .iov_base = &got, .iov_len = sizeof(got) };
            if (ptrace(PTRACE_GETREGSET, pid, (void *)NT_PRSTATUS, &giov) == 0) {
                *ret = (int64_t)got.regs[0];
                saved.regs[0] = got.regs[0];
            }
        }
    }

restore:
    ptrace(PTRACE_POKEDATA, pid, (void *)pc0, (void *)(uintptr_t)w0);
    ptrace(PTRACE_POKEDATA, pid, (void *)(pc0 + 4), (void *)(uintptr_t)w1);
    iov.iov_base = &saved;
    iov.iov_len = sizeof(saved);
    ptrace(PTRACE_SETREGSET, pid, (void *)NT_PRSTATUS, &iov);
    return 0;
}

/* A private executable page taken from vold's own file. Returns the address,
 * or 0. `self_path` is vold's executable path (/proc/<pid>/exe). */
static uint64_t vold_exec_page(pid_t pid, const char *self_path) {
    if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) == -1) return 0;
    {
        int status = 0;
        if (waitpid(pid, &status, 0) < 0 || !WIFSTOPPED(status)) {
            ptrace(PTRACE_DETACH, pid, NULL, NULL);
            return 0;
        }
    }

    uint64_t result = 0;
    int64_t scratch = 0, fd = 0, home = 0;
    size_t plen = strlen(self_path) + 1;

    /* 1. a writable page to hold the path string */
    if (inj_syscall(pid, NR_MMAP, 0, 4096, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, (uint64_t)-1, 0,
                    &scratch) != 0 || scratch <= 0) goto out;
    if (mem_write(pid, (uint64_t)scratch, self_path, plen) != 0) goto out;

    /* 2. open the binary from inside vold */
    if (inj_syscall(pid, NR_OPENAT, (uint64_t)-100 /* AT_FDCWD */,
                    (uint64_t)scratch, O_RDONLY, 0, 0, 0,
                    &fd) != 0 || fd <= 0) goto out;

    /* 3. map it executable, private; the page is ours to overwrite */
    if (inj_syscall(pid, NR_MMAP, 0, 4096, PROT_READ | PROT_EXEC, MAP_PRIVATE,
                    (uint64_t)fd, 0, &home) != 0 || home <= 0) goto out_close;

    result = (uint64_t)home;

out_close:
    { int64_t c; inj_syscall(pid, NR_CLOSE, (uint64_t)fd, 0, 0, 0, 0, 0, &c); }
out:
    ptrace(PTRACE_DETACH, pid, NULL, NULL);
    return result;
}
#endif  /* __aarch64__ */

/* Locate the PLT stub for one imported symbol.
 *
 * `sym` is the symbol name to look up in .rela.plt; the two the module needs
 * are TARGET_SYM ("mount") and TARGET_SYM2 ("umount2"). Everything else about
 * the resolution is symbol-independent, so this takes the name rather than
 * being duplicated per symbol — a second copy is a second place for the
 * relocation-count guard below to rot. */
static int resolve_sym(Src *s, const char *sym, Resolved *r) {
    memset(r, 0, sizeof(*r));
    r->reloc_index = -1;
    r->plt_delta = -1;

    Dyn d;
    if (read_dynamic(s, &d) != 0) return EXIT_NO_RESOLVE;

    Elf64_Rela *rel = NULL;
    size_t nrel = 0;
    if (load_relocs(s, &d, &rel, &nrel) != 0) return EXIT_NO_RESOLVE;

    int nmatch = 0;
    for (size_t i = 0; i < nrel; i++) {
        if ((uint32_t)(rel[i].r_info & 0xffffffffu) != R_AARCH64_JUMP_SLOT)
            continue;
        uint32_t idx = (uint32_t)(rel[i].r_info >> 32);
        char nm[512];
        if (sym_name(s, &d, idx, nm, sizeof(nm)) != 0) continue;
        if (strcmp(nm, sym) == 0) {
            nmatch++;
            if (nmatch == 1) {
                r->got_slot = rel[i].r_offset;
                r->sym_index = idx;
                r->reloc_index = (int)i;
            }
            continue;
        }
    }
    if (nmatch == 0) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }
    if (nmatch > 1) {
        free(rel);
        return EXIT_NO_RESOLVE;
    }

    /* Collect all stubs in one pass over executable PT_LOADs. */
    enum { MAXSTUB = 65536 };
    uint64_t *stub_va = calloc(MAXSTUB, sizeof(uint64_t));
    uint64_t *stub_tg = calloc(MAXSTUB, sizeof(uint64_t));
    size_t nstubs = 0;
    if (!stub_va || !stub_tg) {
        free(stub_va); free(stub_tg); free(rel);
        return EXIT_NO_RESOLVE;
    }
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 16) continue;
        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) continue;
        if (src_pread(s, p->p_vaddr, buf, len) != 0) { free(buf); continue; }
        uint64_t start = (p->p_vaddr + 15u) & ~15ULL;
        for (uint64_t va = start; va + 16 <= p->p_vaddr + p->p_filesz; va += 16) {
            const uint8_t *q = buf + (va - p->p_vaddr);
            uint64_t tgt = 0;
            if (decode_stub(q, va, &tgt) && nstubs < MAXSTUB) {
                stub_va[nstubs] = va;
                stub_tg[nstubs] = tgt;
                nstubs++;
            }
        }
        free(buf);
    }

    r->plt_delta = plt_layout_delta(stub_va, stub_tg, nstubs, rel, nrel);
    if (r->plt_delta < 0) {
        free(stub_va); free(stub_tg); free(rel);
        return EXIT_NO_RESOLVE;
    }

    int hits = 0;
    uint64_t direct = 0;
    for (size_t j = 0; j < nstubs; j++) {
        if (stub_tg[j] == r->got_slot) { direct = stub_va[j]; hits++; }
    }
    free(stub_va); free(stub_tg); free(rel);

    if (hits == 1) {
        r->stub_va = direct;
    } else if (hits > 1) {
        return EXIT_NO_RESOLVE;
    } else {
        int64_t cand = (int64_t)r->plt_delta * 16 + (int64_t)r->reloc_index * 16;
        if (cand <= 0) {
            return EXIT_NO_RESOLVE;
        }
        r->stub_va = (uint64_t)cand;
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ *
 * Locating the MountUserFuse mount(2) call
 *
 * The naive approach — "find the string /dev/fuse, count references to it" —
 * does not work, and it is worth writing down why, because both failure modes
 * were measured on a real binary (/system/bin/vold, 1,077,192 bytes):
 *
 *  (1) The FIRST occurrence of the byte run "/dev/fuse" is not the literal:
 *      it is the tail of the log message "Failed to open /dev/fuse". A scan
 *      that stops at the first match anchors on a string that is never passed
 *      to mount(2). The standalone literal is a second, separate occurrence.
 *
 *  (2) The address of the literal is not necessarily built into x0. The real
 *      site is:
 *
 *          adrp x20, 0x14000
 *          add  x20, x20, #0xe35     ; 0x14e35 = "/dev/fuse"
 *          adrp x21, 0x15000
 *          add  x21, x21, #0xcb3     ; 0x15cb3 = "fuse"
 *          mov  x0, x20              ; source = "/dev/fuse"
 *          mov  x2, x21              ; type   = "fuse"
 *          mov  w3, #0x40e
 *          bl   <mount@plt>
 *
 *      so a counter keyed on `Rd == x0` misses it entirely. (It also uses a
 *      mov to shuffle, which is what -Oz does.)
 *
 * Matching on the 4 KB page instead of the exact address is what made the
 * old counter report 55: vold's .rodata packs dozens of unrelated strings on
 * the page holding /dev/fuse.
 *
 * So we identify the call site STRUCTURALLY instead. A site qualifies when,
 * sweeping backwards from a `bl <mount@plt>`:
 *
 *   - a register receives the exact address of a standalone "/dev/fuse"
 *     string (NUL-terminated, not preceded by an identifier byte);
 *   - the same or another register receives the exact address of a standalone
 *     "fuse" string;
 *   - those two registers are moved into x0 and x2 respectively before the
 *     call, and the window does not contain a branch (so the block is linear);
 *   - x4 (the data pointer) is xzr/NULL, or is set to NULL before the call —
 *     MountUserFuse passes NULL for the fuse mount.
 *
 * The result must be exactly one site. Zero or more than one means the
 * anchor is not what this patch assumes, and the tool refuses.
 * ------------------------------------------------------------------ */

typedef struct {
    uint64_t va;        /* address of the literal */
    bool     standalone;/* NUL-terminated, no identifier byte before it */
} StrLoc;

/* Find every occurrence of `needle` (including its NUL) in readable,
 * non-executable, file-backed segments and report the VA + whether it is a
 * standalone string. */
static int find_strings(Src *s, const char *needle, StrLoc *out, int max) {
    size_t nl = strlen(needle) + 1;   /* include NUL */
    int n = 0;

    for (int i = 0; i < s->phnum && n < max; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_R) || (p->p_flags & PF_X))
            continue;
        if (p->p_filesz < nl) continue;
        size_t len = (size_t)p->p_filesz;
        uint8_t *buf = malloc(len);
        if (!buf) continue;
        if (src_pread(s, p->p_vaddr, buf, len) != 0) { free(buf); continue; }

        for (size_t off = 0; off + nl <= len; off++) {
            if (memcmp(buf + off, needle, nl) != 0) continue;
            /* A literal is "standalone" when it begins a string-table entry,
             * i.e. the byte before it is NUL (or it starts the segment). Any
             * other preceding byte means it is the tail of a longer literal —
             * vold has both, e.g.
             *   "Failed to open /dev/fuse\0"   <- the message, NOT the literal
             *   "...failed\0/dev/fuse\0"        <- the real literal
             * The message-tail is what a first-match scan anchors on, and it
             * is never passed to mount(2). Requiring a preceding NUL rejects
             * it. */
            bool standalone = (off == 0) ? true : (buf[off - 1] == '\0');
            if (n < max) {
                out[n].va = p->p_vaddr + off;
                out[n].standalone = standalone;
                n++;
            }
        }
        free(buf);
    }
    return n;
}

/* The page address an `adrp Rd, label` computes, given the instruction's own
 * address. */
static uint64_t adrp_page(uint64_t pc, uint32_t w) {
    uint32_t immlo = (w >> 29) & 0x3u;
    uint32_t immhi = (w >> 5) & 0x7ffffu;
    uint32_t v = (immhi << 2) | immlo;
    int64_t sv = (int64_t)(int32_t)(v << 11) >> 11;
    return (pc & ~0xfffULL) + (uint64_t)(sv << 12);
}

/* Building the address of a literal is a PAIR of instructions — `adrp Rn, page`
 * plus a later `add Rd, Rn, #imm` (or `ldr Rd, [Rn, #imm]`) — but not
 * necessarily an ADJACENT pair. The compiler is free to interleave two of them,
 * and vold 11/12/12L/13 does exactly that:
 *
 *     adrp x21, 0x13000     ; "/dev/fuse"
 *     adrp x22, 0x1c000     ; "fuse"
 *     add  x21, x21, #0x73
 *     add  x22, x22, #0x1f6
 *
 * The first version of this matcher required w[j] == adrp and w[j+1] == its
 * add. On those four releases it therefore decoded neither register, found no
 * call site at all, and the tool refused vold with "anchor did not resolve" —
 * i.e. the FUSE patch silently did not exist on Android 11–13. What carries the
 * meaning is the register, not the distance between the two halves.
 *
 * The scan runs BACKWARDS from the call, so it meets the `add` first and the
 * `adrp` after it. Each register therefore remembers the last `add`/`ldr` it
 * was given (the one nearest the call wins), and an `adrp` closes every pending
 * entry that names it as its base. */
typedef struct {
    bool     live;
    int      base;   /* Rn of the pending add/ldr */
    uint64_t off;    /* its #imm12, already scaled for the ldr form */
} Pending;

/* Record `add Rd, Rn, #imm12` / `ldr Rd, [Rn, #imm12]` as pending on Rd.
 * Returns Rd, or -1 if `w` is neither form. */
static int pending_put(uint32_t w, Pending *p) {
    int rd, rn;
    uint64_t off;

    if ((w & 0xffc00000u) == 0x91000000u) {            /* add Rd, Rn, #imm12 */
        off = (w >> 10) & 0xfffu;
    } else if ((w & 0xffc00000u) == 0xf9400000u) {     /* ldr Rd, [Rn, #imm12] */
        off = (uint64_t)((w >> 10) & 0xfffu) * 8u;
    } else {
        return -1;
    }
    rd = (int)(w & 0x1fu);
    rn = (int)((w >> 5) & 0x1fu);
    if (!p[rd].live) {
        p[rd].live = true;
        p[rd].base = rn;
        p[rd].off  = off;
    }
    return rd;
}

/* `mov x0, xN` (alias of `orr x0, xzr, xN`): 0xaa0003e0 | (N << 16).
 * The encoding is sf=1 opc=01 01010 N=0 shift=0 (bits 21-31), Rn = xzr = 31
 * (bits 5-9), Rm = N (bits 16-20), Rd = 0 (bits 0-4). So the mask must clear
 * ONLY bits 16-20 and keep Rd. Verified against real vold:
 *   mov x0, x20 -> 0xaa1403e0 ; mov x2, x21 -> 0xaa1503e2.
 * `add x0, xN, #0` is 0x910003e0 with the same field layout. */
#define MOV_CORE_X0  0xaa0003e0u
#define MOV_CORE_X2  0xaa0003e2u
#define ADD_CORE_X0  0x910003e0u
#define ADD_CORE_X2  0x910003e2u
#define Rm_MASK      0xffe0ffffu   /* clears bits 16-20 (Rm) only */

static bool mov_to_x0(uint32_t w, int *src_out) {
    if ((w & Rm_MASK) == MOV_CORE_X0) {               /* orr x0, xzr, xN */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    if ((w & Rm_MASK) == ADD_CORE_X0) {               /* add x0, xN, #0 */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    return false;
}

static bool mov_to_x2(uint32_t w, int *src_out) {
    if ((w & Rm_MASK) == MOV_CORE_X2) {               /* orr x2, xzr, xN */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    if ((w & Rm_MASK) == ADD_CORE_X2) {               /* add x2, xN, #0 */
        *src_out = (int)((w >> 16) & 0x1fu);
        return true;
    }
    return false;
}

/* `mov w3, #imm16`  = 0x52800000 | (imm16 << 5) | 3
 * `movk w3, #imm16, lsl #16` = 0x72a00003 | (imm16 << 5)
 * Together they build the 32-bit mount flags.
 *
 * NOTE on direction: the caller scans BACKWARDS from the call, so a
 * `movk …, lsl #16` is seen BEFORE the `mov` that sets the low half. Each
 * helper therefore writes only its own half and never clobbers the other —
 * writing the whole register in `mov` would erase the high half that was
 * already collected from the (program-order-later) movk. */
static bool decode_w3_mov(uint32_t w, uint32_t *acc) {
    if ((w & 0xffe0001fu) == 0x52800003u) {          /* mov w3, #imm16 */
        *acc = (*acc & 0xffff0000u) | ((w >> 5) & 0xffffu);
        return true;
    }
    if ((w & 0xffe0001fu) == 0x72a00003u) {          /* movk w3, #imm16, lsl 16 */
        *acc = (*acc & 0xffffu) | (((w >> 5) & 0xffffu) << 16);
        return true;
    }
    return false;
}

typedef struct {
    uint64_t src_va;     /* VA of the "/dev/fuse" literal used here */
    uint64_t type_va;    /* VA of the "fuse" literal used here */
    uint32_t flags;      /* value loaded into w3, if seen */
    bool     has_flags;  /* whether w3 was decoded */
} FuseSite;

/* Everything one call site's prologue tells us. */
typedef struct {
    int      src_reg, type_reg;   /* registers holding the two literals */
    uint64_t src_val, type_val;   /* and which literals they hold */
    int      x0_from, x2_from;    /* registers moved into x0 / x2 */
    uint32_t w3_acc;              /* mount flags, as built by mov/movk */
    bool     w3_seen;
} SiteRegs;

/* Walk back from the `bl` at w[k] over the straight-line block that sets up the
 * call, and decode it into `out`.
 *
 * This is split out of find_fuse_site() so it can be exercised on a synthetic
 * instruction buffer: the matcher is the part that decides whether vold can be
 * patched at all, and it has already been wrong once (the interleaved adrp
 * pairs of vold 11–13, see pending_put above), silently, on four releases. A
 * test that needs a real vold image cannot run everywhere; this one needs 40
 * bytes of array.
 *
 * We track, per register, the last literal it was loaded with, and the last
 * source register moved into x0/x2. The window closes at the first CONDITIONAL
 * control-flow instruction: inside a conditional block the reading is no longer
 * safe (the register may hold something else on the other path). Unconditional
 * calls (`bl`) are fine — the real code has a `bl StringPrintf` just before the
 * address setup, and the argument registers are re-loaded after it. A
 * `b`/`ret`/`br` ends the window too.
 *
 * Not required: x4 == NULL. The handler does not use x4, and vold builds it
 * with `csel` from an empty std::string (which is NULL in practice but not a
 * literal `mov x4, xzr`), so demanding a literal NULL would reject the very
 * site we want. */
static void scan_back(const uint32_t *w, size_t nw, size_t k, uint64_t seg_va,
                      uint64_t want_src, const uint64_t *type_vas, int ntype,
                      SiteRegs *out) {
    memset(out, 0, sizeof(*out));
    out->src_reg  = -1;
    out->type_reg = -1;
    out->x0_from  = -1;
    out->x2_from  = -1;

    Pending pend[32];
    memset(pend, 0, sizeof(pend));

    size_t lo = (k > 32) ? k - 32 : 0;

    for (size_t j = k; j-- > lo; ) {
        uint32_t a = w[j];
        uint64_t apc = seg_va + (uint64_t)j * 4u;

        /* A conditional branch closes the window. */
        if ((a & 0xff000010u) == 0x54000000u ||   /* b.cond  */
            (a & 0x7e000000u) == 0x34000000u ||   /* cbz/cbnz */
            (a & 0x7e000000u) == 0x36000000u)     /* tbz/tbnz */
            break;

        /* Structure ends: unconditional branch / return. */
        if ((a & 0x7c000000u) == 0x14000000u ||   /* b (not bl) */
            (a & 0xfffffc1fu) == 0xd61f0000u ||   /* br/blr     */
            a == 0xd65f03c0u)                     /* ret        */
            break;

        if ((a & 0x9f000000u) == 0x90000000u) {   /* adrp Rn, page */
            int rn = (int)(a & 0x1fu);
            uint64_t page = adrp_page(apc, a);
            for (int r = 0; r < 32; r++) {
                if (!pend[r].live || pend[r].base != rn) continue;
                uint64_t val = page + pend[r].off;
                pend[r].live = false;   /* the nearest adrp wins */
                if (val == want_src) {
                    out->src_reg = r;
                    out->src_val = val;
                }
                for (int t = 0; t < ntype; t++) {
                    if (val == type_vas[t]) {
                        out->type_reg = r;
                        out->type_val = val;
                    }
                }
            }
        } else {
            pending_put(a, pend);
        }

        int from;
        if (mov_to_x0(a, &from)) out->x0_from = from;
        if (mov_to_x2(a, &from)) out->x2_from = from;
        if (decode_w3_mov(a, &out->w3_acc)) out->w3_seen = true;
    }
}

/* Find the single `mount("/dev/fuse", ..., "fuse", ...)` call site. */
static int find_fuse_site(Src *s, uint64_t stub_va, FuseSite *out) {
    enum { MAXSTR = 64 };
    StrLoc srcs[MAXSTR], types[MAXSTR];
    int nsrc = find_strings(s, FUSE_SRC, srcs, MAXSTR);
    int ntype = find_strings(s, FUSE_TYPE, types, MAXSTR);
    if (nsrc == 0) return -1;
    if (ntype == 0) return -1;

    /* Keep only standalone occurrences. */
    StrLoc sstand[MAXSTR], tstand[MAXSTR];
    int ns = 0, nt = 0;
    for (int i = 0; i < nsrc; i++)
        if (srcs[i].standalone) sstand[ns++] = srcs[i];
    for (int i = 0; i < ntype; i++)
        if (types[i].standalone) tstand[nt++] = types[i];

    if (ns != 1) return -1;
    uint64_t want_src = sstand[0].va;

    /* scan_back() wants the type VAs as a plain array. */
    uint64_t tvas[MAXSTR];
    for (int t = 0; t < nt; t++) tvas[t] = tstand[t].va;

    /* Sweep executable segments for `bl <stub_va>`. */
    FuseSite found[8];
    int nfound = 0;

    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (p->p_filesz < 64) continue;
        size_t len = (size_t)p->p_filesz;
        uint32_t *w = malloc(len);
        if (!w) continue;
        if (src_pread(s, p->p_vaddr, w, len) != 0) { free(w); continue; }
        size_t nw = len / 4;

        for (size_t k = 0; k < nw; k++) {
            uint32_t in = w[k];
            /* bl imm26 ? */
            if ((in & 0xfc000000u) != 0x94000000u) continue;
            int64_t imm = (int64_t)(in & 0x03ffffffu);
            if (imm & (1 << 25)) imm -= (1 << 26);
            uint64_t pc = p->p_vaddr + (uint64_t)k * 4u;
            uint64_t tgt = pc + (uint64_t)(imm << 2);
            if (tgt != stub_va) continue;

            SiteRegs sr;
            scan_back(w, nw, k, p->p_vaddr, want_src, tvas, nt, &sr);

            if (sr.src_reg >= 0 && sr.type_reg >= 0 &&
                sr.x0_from == sr.src_reg && sr.x2_from == sr.type_reg) {
                if (nfound < 8) {
                    found[nfound].src_va = sr.src_val;
                    found[nfound].type_va = sr.type_val;
                    found[nfound].flags = sr.w3_acc;
                    found[nfound].has_flags = sr.w3_seen;
                }
                nfound++;
            }
        }
        free(w);
    }

    if (nfound == 0) return -1;

    /*
     * There are TWO such call sites in vold, and they must be treated
     * differently:
     *
     *   MountUserFuse()   Utils.cpp:1676   flags MS_…|MS_LAZYTIME  -> /mnt/user/<u>/emulated
     *   AppFuseUtil::Mount() AppFuseUtil.cpp:63  flags MS_… (no LAZYTIME) -> /mnt/appfuse/<uid>_<name>
     *
     * The first is the emulated-storage FUSE, which is the whole point of this
     * patch. The second is per-app Android/data sandboxing; killing it would
     * break apps that rely on it and would not help us at all. MS_LAZYTIME
     * (1 << 25 = 0x02000000) is present in exactly one of them on every release
     * we have checked, so it is the discriminator.
     *
     * This choice only decides which site's literal VAs are handed to
     * build_handler(). It is NOT sufficient on its own: on the device's vold
     * the compiler merged the two "/dev/fuse" literals, so both sites pass the
     * SAME pointer to mount(2) and the handler would take over AppFuseUtil's
     * call as well. That is why the handler re-tests MS_LAZYTIME on x3 at
     * runtime — see "how the call is recognised". The two checks have to agree;
     * if this one ever picks a site that the handler's test rejects, nothing is
     * intercepted and nothing says so.
     */
    enum { MAXCAND = 8 };
    FuseSite keep[MAXCAND];
    int nkeep = 0;
    int n_lazytime = 0;
    for (int i = 0; i < nfound; i++) {
        bool lazy = found[i].has_flags && (found[i].flags & MS_LAZYTIME);
        if (lazy) {
            n_lazytime++;
            if (nkeep < MAXCAND) keep[nkeep++] = found[i];
        }
    }

    if (n_lazytime == 0) {
        /* Nothing distinguished by MS_LAZYTIME: refuse rather than guess. */
        return -1;
    }
    if (n_lazytime > 1) return -1;

    *out = keep[0];
    return 0;
}

/* ------------------------------------------------------------------ *
 * selftest — check the handler we emit before it can be written anywhere
 *
 * The handler is hand-written arm64. A wrong instruction word is a jump into
 * the void inside vold, so it is worth asserting its shape rather than
 * trusting the encoders. This runs entirely on the host (or on the device —
 * it needs nothing), and checks the properties that matter:
 *
 *   - it assembles, and long enough for both paths;
 *   - the first word is `bti jc`;
 *   - the literal pool holds exactly the values passed in;
 *   - the P_RAW slot points at the copy of "/data/media" inside the block;
 *   - there are exactly three classification branches — two `b.ne` (the two
 *     argument pointers) and one `b.eq` (MS_LAZYTIME) — and all three point at
 *     the pass block;
 *   - the MS_LAZYTIME test is emitted as `movz w9,#0x200,lsl#16; tst w3,w9;
 *     b.eq`, verified word by word, because it is the only thing that keeps
 *     the handler off AppFuseUtil::Mount();
 *   - the pass block ends in `br x9` preceded by `ldp x29,x30,[sp],#16`;
 *   - the take-over block ends in `ret` preceded by the same `ldp`;
 *   - the "mov w3" immediate is MS_BIND|MS_REC.
 *
 * Any failure prints what was expected and returns non-zero.
 * ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ *
 * Running the umount2 handler for real
 *
 * The handler is position-independent and reads nothing but the literal pool
 * that ships with it, so a copy of it can be run right here with the pool's
 * umount2 entry pointed at a stub of our own. That turns "the encodings look
 * right" into "an aarch64 CPU does the right thing", which is the only claim
 * that matters: this code runs inside vold, on the unmount path of every
 * volume, where a wrong branch is a dead vold and a reboot loop.
 *
 * The stub models the kernel rather than merely counting calls: it holds a
 * layer count, returns 0 while it can remove one, and answers EINVAL once the
 * path is clear — which is what umount2 really does, and what the handler's
 * "stop at the first refusal" rule is written against. A second knob makes it
 * fail with EBUSY from the Nth call on, so the rule is tested from both sides:
 * a cleared path must be reported clear, and a refusal must be reported rather
 * than swallowed.
 *
 * The path list is the contract, not a sample. It covers both accepted widths
 * of user id, the pre-patch one-layer case, both refusal points, and every way
 * a string can look like the path without being it — a prefix, a suffix, a
 * different separator position, a trailing slash, a wrong case, a foreign
 * mount root, and the short paths vold really does unmount (/data, /). */
#define UM_TEST_FLAGS 0x1234

#if defined(__aarch64__)
static int  g_um_calls;
static int  g_um_layers;
static int  g_um_fail_from;
static char g_um_last_path[256];
static int  g_um_last_flags;

static int fake_umount2(const char *path, int flags) {
    g_um_calls++;
    snprintf(g_um_last_path, sizeof(g_um_last_path), "%s",
             path ? path : "(null)");
    g_um_last_flags = flags;
    if (g_um_fail_from && g_um_calls >= g_um_fail_from) {
        errno = EBUSY;
        return -1;
    }
    if (g_um_layers <= 0) {
        errno = EINVAL;
        return -1;
    }
    g_um_layers--;
    return 0;
}
#endif

static int umount_handler_exec_selftest(void) {
#if !defined(__aarch64__)
    return 0;   /* the handler is aarch64 machine code; nothing to run here */
#else
    static const struct {
        const char *path; int layers; int fail_from;
        int want_calls; int want_rc; int want_errno;
    } cases[] = {
        /* ours: two layers, so three calls and a final EINVAL */
        { "/mnt/user/0/emulated",                2, 0, 3, -1, EINVAL },
        { "/mnt/user/10/emulated",               2, 0, 3, -1, EINVAL },
        { "/mnt/user/999/emulated",              2, 0, 3, -1, EINVAL },
        /* ours, mounted before the patch: one layer, two calls */
        { "/mnt/user/0/emulated",                1, 0, 2, -1, EINVAL },
        /* ours and busy: the refusal must come back, not be swallowed */
        { "/mnt/user/0/emulated",                2, 2, 2, -1, EBUSY  },
        { "/mnt/user/0/emulated",                1, 1, 1, -1, EBUSY  },
        /* not ours: passed through exactly once, result handed back as-is */
        { "/mnt/pass_through/0/emulated",        1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulated/0/Android/data", 1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulated/0/Android/obb",  1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulatedx",               1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulated/",               1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulatedx/emulated",      1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulate",                 1, 0, 1,  0, 0      },
        { "/mnt/user/0/emulateX",                1, 0, 1,  0, 0      },
        { "/mnt/user/0/Emulated",                1, 0, 1,  0, 0      },
        { "/mnt/user//emulated",                 1, 0, 1,  0, 0      },
        { "/mnt/user/emulated",                  1, 0, 1,  0, 0      },
        { "/mnt/users/0/emulated",               1, 0, 1,  0, 0      },
        { "/mnt/user",                           1, 0, 1,  0, 0      },
        { "/data",                               1, 0, 1,  0, 0      },
        { "/",                                   1, 0, 1,  0, 0      },
        { "",                                    1, 0, 1,  0, 0      },
    };

    long pg = sysconf(_SC_PAGESIZE);
    size_t pgsz = pg > 0 ? (size_t)pg : 4096;
    void *mem = mmap(NULL, pgsz, PROT_READ | PROT_WRITE | PROT_EXEC,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        fprintf(stderr, "selftest: исполняемую память не дали (%s) — "
                        "прогон машины пропущен\n", strerror(errno));
        return 0;   /* not a failure: the structural checks still ran */
    }

    uint8_t blob[512];
    int n = build_umount_handler(blob, sizeof(blob), (uint64_t)(uintptr_t)mem,
                                 (uint64_t)(uintptr_t)&fake_umount2);
    if (n <= 0) {
        munmap(mem, pgsz);
        fprintf(stderr, "selftest FAIL: build_umount_handler вернул %d\n", n);
        return 1;
    }
    memcpy(mem, blob, (size_t)n);
    __builtin___clear_cache((char *)mem, (char *)mem + n);

    typedef int (*um_fn)(const char *, int);
    um_fn fn = (um_fn)(void *)mem;

    int bad = 0;
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char pbuf[256];
        snprintf(pbuf, sizeof(pbuf), "%s", cases[i].path);
        g_um_calls = 0;
        g_um_layers = cases[i].layers;
        g_um_fail_from = cases[i].fail_from;
        g_um_last_path[0] = '\0';
        g_um_last_flags = 0;
        errno = 0;

        int rc = fn(pbuf, UM_TEST_FLAGS);
        int err = errno;

        bool ok = g_um_calls == cases[i].want_calls &&
                  rc == cases[i].want_rc &&
                  (cases[i].want_rc == 0 || err == cases[i].want_errno) &&
                  strcmp(g_um_last_path, cases[i].path) == 0 &&
                  g_um_last_flags == UM_TEST_FLAGS;
        if (!ok) {
            fprintf(stderr, "selftest FAIL: \"%s\" (слоёв %d, отказ с %d): "
                    "вызовов %d (ждали %d), rc %d (ждали %d), errno %d (ждали "
                    "%d), последний путь \"%s\", flags %#x\n",
                    cases[i].path, cases[i].layers, cases[i].fail_from,
                    g_um_calls, cases[i].want_calls, rc, cases[i].want_rc,
                    err, cases[i].want_errno, g_um_last_path, g_um_last_flags);
            bad++;
        }
    }
    munmap(mem, pgsz);

    if (bad) return 1;
    return 0;
#endif
}

/* The two handlers have to fit in the one zero run find_handler_room() asks
 * for. That constant (512) and this check are two halves of the same claim, so
 * they are asserted together rather than left to drift apart. */
static int handlers_fit_selftest(void) {
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));
    uint64_t code_va  = 0x567c0ef7d0ULL;
    uint64_t base     = 0x567bff3000ULL;

    int a = build_handler(buf, sizeof(buf), code_va, 0x79067a7580ULL,
                          base + 0x14e35ULL, base + 0x15cb3ULL, RAW_PATH);
    if (a <= 0) { fprintf(stderr, "selftest FAIL: build_handler вернул %d\n", a); return 1; }
    int a_len = (a + 7) & ~7;
    int b = build_umount_handler(buf + a_len, sizeof(buf) - (size_t)a_len,
                                 code_va + (uint64_t)a_len, 0x79067a9c40ULL);
    if (b <= 0) { fprintf(stderr, "selftest FAIL: build_umount_handler вернул %d\n", b); return 1; }
    if (a_len + b > 512) {
        fprintf(stderr, "selftest FAIL: обработчики занимают %d байт, "
                        "а find_handler_room ищет 512\n", a_len + b);
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------ *
 * anchor_selftest — the call-site matcher, on a synthetic prologue
 *
 * find_fuse_site() decides whether vold can be patched at all: if it finds no
 * site the tool exits 2 and the FUSE patch is simply not installed. It has been
 * wrong once, silently, on four releases at the same time (vold 11/12/12L/13
 * interleave the two adrp/add pairs; the matcher required them adjacent), and
 * nothing in this file would have noticed — the handler checks all pass, they
 * just never run.
 *
 * The real images cannot be used here: they are not in the tree, and a test
 * that needs one cannot run on a clean checkout. So the two prologue shapes are
 * written out as instructions instead. Both must decode to the same call site.
 * ------------------------------------------------------------------ */
static int anchor_selftest(void) {
    /* A page-aligned fake segment, and the two literals inside it. */
    const uint64_t seg  = 0x10000;
    const uint64_t srcv = 0x10123;      /* "/dev/fuse" */
    const uint64_t typev = 0x10456;     /* "fuse"      */
    const uint64_t stub = 0x20000;      /* where `bl` goes */
    const uint32_t want_w3 = 0x0200040eu;   /* MS_LAZYTIME | MS_… */

    struct { const char *what; uint32_t w[12]; int n; } cases[2];
    memset(cases, 0, sizeof(cases));

    /* Shape 1 — adjacent pairs (vold 14/15/16/17). */
    cases[0].what = "вплотную (14+)";
    cases[0].n = 8;
    cases[0].w[0] = enc_adrp(21, seg + 0, srcv);
    cases[0].w[1] = enc_add_imm(21, 21, (unsigned)(srcv - seg));
    cases[0].w[2] = enc_adrp(22, seg + 8, typev);
    cases[0].w[3] = enc_add_imm(22, 22, (unsigned)(typev - seg));
    cases[0].w[4] = enc_movz_w(3, 0x40eu);
    cases[0].w[5] = enc_movk_w(3, 0x200u);
    cases[0].w[6] = enc_mov_reg(0, 21);
    cases[0].w[7] = enc_mov_reg(2, 22);

    /* Shape 2 — the two pairs interleaved (vold 11/12/12L/13). This is the
     * shape that used to decode to nothing. */
    cases[1].what = "вразбивку (11–13)";
    cases[1].n = 9;
    cases[1].w[0] = enc_adrp(21, seg + 0, srcv);
    cases[1].w[1] = enc_adrp(22, seg + 4, typev);
    cases[1].w[2] = enc_add_imm(21, 21, (unsigned)(srcv - seg));
    cases[1].w[3] = enc_mov_reg(25, 8);                    /* unrelated filler */
    cases[1].w[4] = enc_add_imm(22, 22, (unsigned)(typev - seg));
    cases[1].w[5] = enc_movz_w(3, 0x40eu);
    cases[1].w[6] = enc_movk_w(3, 0x200u);
    cases[1].w[7] = enc_mov_reg(0, 21);
    cases[1].w[8] = enc_mov_reg(2, 22);

    uint64_t tvas[1] = { typev };

    for (int c = 0; c < 2; c++) {
        uint32_t code[12];
        memcpy(code, cases[c].w, sizeof(code));
        size_t npro = (size_t)cases[c].n;
        /* the `bl` sits one word past the prologue */
        code[npro] = enc_bl(seg + (uint64_t)npro * 4u, stub);

        SiteRegs sr;
        scan_back(code, npro + 1, npro, seg, srcv, tvas, 1, &sr);

        if (sr.src_reg != 21 || sr.x0_from != 21) {
            fprintf(stderr, "anchor FAIL (%s): \"/dev/fuse\" -> x%d (mov из x%d),"
                            " ждали x21\n", cases[c].what, sr.src_reg, sr.x0_from);
            return 1;
        }
        if (sr.type_reg != 22 || sr.x2_from != 22) {
            fprintf(stderr, "anchor FAIL (%s): \"fuse\" -> x%d (mov из x%d),"
                            " ждали x22\n", cases[c].what, sr.type_reg, sr.x2_from);
            return 1;
        }
        if (sr.src_val != srcv || sr.type_val != typev) {
            fprintf(stderr, "anchor FAIL (%s): литералы %#llx/%#llx,"
                            " ждали %#llx/%#llx\n", cases[c].what,
                    (unsigned long long)sr.src_val,
                    (unsigned long long)sr.type_val,
                    (unsigned long long)srcv, (unsigned long long)typev);
            return 1;
        }
        if (!sr.w3_seen || sr.w3_acc != want_w3) {
            fprintf(stderr, "anchor FAIL (%s): флаги %#x (seen %d), ждали %#x\n",
                    cases[c].what, sr.w3_acc, (int)sr.w3_seen, want_w3);
            return 1;
        }
    }

    /* And the negative: a prologue that builds neither literal must decode to
     * nothing. Without this, "returns a site" could be satisfied by a matcher
     * that simply returns the first registers it sees. */
    {
        uint32_t code[4];
        code[0] = enc_adrp(21, seg + 0, seg + 0x800);
        code[1] = enc_add_imm(21, 21, 0x40);
        code[2] = enc_mov_reg(0, 21);
        code[3] = enc_bl(seg + 12, stub);
        SiteRegs sr;
        scan_back(code, 4, 3, seg, srcv, tvas, 1, &sr);
        if (sr.src_reg >= 0 || sr.type_reg >= 0) {
            fprintf(stderr, "anchor FAIL: чужой литерал распознан как место"
                            " вызова (src x%d, type x%d)\n",
                    sr.src_reg, sr.type_reg);
            return 1;
        }
    }

    return 0;
}

static int selftest(void) {
    uint8_t buf[512];
    memset(buf, 0, sizeof(buf));
    uint64_t code_va   = 0x567c0ef7d0ULL;
    uint64_t base      = 0x567bff3000ULL;
    uint64_t src_va    = base + 0x14e35ULL;
    uint64_t type_va   = base + 0x15cb3ULL;
    uint64_t mount_va  = 0x79067a7580ULL;

    int n = build_handler(buf, sizeof(buf), code_va, mount_va,
                          src_va, type_va, "/data/media");
    if (n <= 0) { fputs("selftest: build_handler failed\n", stderr); return 1; }

    uint32_t *w = (uint32_t *)buf;
    int nw = n / 4;

    struct { const char *what; uint32_t got, want; } checks[] = {
        { "word0 = bti jc", w[0], 0xd50324dfu },
        { "word1 = stp x29,x30", w[1], 0xa9bf7bfdu },
        { "word2 = mov x29,sp", w[2], 0x910003fdu },
    };
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++) {
        if (checks[i].got != checks[i].want) {
            fprintf(stderr, "selftest FAIL: %s: got %08x want %08x\n",
                    checks[i].what, checks[i].got, checks[i].want);
            return 1;
        }
    }

    /* Find the pass block. Two anchors are acceptable, and which one the
     * branches must hit is decided by what actually follows:
     *
     *   - `br x9` (0xd61f0120) is the tail-call into libc mount. If it is
     *     preceded by `ldp x29,x30,[sp],#16`, then that ldp is the frame
     *     restore and *it* is the branch target — landing on the br would hand
     *     libc mount a frame that is still pushed.
     *   - otherwise the `br x9` stands alone and is itself the target.
     *
     * Exactly one `br x9` must exist either way. */
    /* Find the pass block. The only unambiguous anchor is the `br x9`
     * (0xd61f0120) tail-call into libc mount, and exactly one must exist; the
     * take-over path reaches libc via `blr x9`, so it does not collide.
     *
     * The frame restore (`ldp x29,x30,[sp],#16`) is *not* necessarily the word
     * immediately before the `br`: the emission puts the `ldr x9, <mount>` pool
     * load in between, so the real order is restore, ldr, br. The branch target
     * is therefore the nearest preceding `ldp x29,x30,[sp],#16`, searched
     * backwards, and that word must exist — a handler that jumps into the pool
     * load or the `br` without restoring sp is the exact defect this test
     * exists to catch, so finding nothing is a failure rather than a fallback. */
    int pass = -1, nbr_br = 0;
    for (int i = 0; i < nw; i++) {
        if (w[i] == 0xd61f0120u) { pass = i; nbr_br++; }
    }
    if (nbr_br != 1) {
        fprintf(stderr, "selftest FAIL: %d 'br x9' (ожидался один)\n", nbr_br);
        return 1;
    }
    int branch_target = -1;
    for (int i = pass - 1; i >= 0; i--) {
        if (w[i] == 0xa8c17bfdu) { branch_target = i; break; }
        /* Only the pool load may sit between the restore and the br. */
        if ((w[i] >> 24) != 0x58u && (w[i] >> 24) != 0x18u) break;
    }
    if (branch_target < 0) {
        fprintf(stderr, "selftest FAIL: перед 'br x9' (слово %d) нет "
                        "'ldp x29,x30,[sp],#16'\n", pass);
        return 1;
    }

    /* Every classification branch — two b.ne on the argument pointers, one
     * b.eq on MS_LAZYTIME — must target the pass block. This is checked by
     * *decoding* the emitted imm19, so an encoder that silently lost the delta
     * (the bug this test was written after) is caught even though the word
     * count is right. The conditions are counted separately as well: a handler
     * that lost the b.eq, or emitted it as another b.ne, would still show three
     * branches but would take over AppFuseUtil::Mount() — the defect this
     * branch exists to prevent. */
    int ncond = 0, n_ne = 0, n_eq = 0;
    for (int i = 0; i < nw; i++) {
        /* b.cond is 0x5400_0000 | imm19<<5 | cond: the opcode lives in bits
         * 31..24 and the condition in bits 3..0, with imm19 in between. So the
         * opcode is `>> 24` and the condition is `& 0xf` — neither can be
         * combined into one mask, because any mask low enough to clear the
         * condition also clears bits of imm19 that legitimately vary. Two
         * earlier attempts here (`& 0xff000010`, then `& 0xff00000f`) each
         * matched nothing: the first masks a bit that is not part of a
         * condition code, the second leaves b.ne's cond (1) set. Both times the
         * loop found zero branches, left `pass` at -1, and reported the *last*
         * `br`-family word as if it were a branch target. */
        if ((w[i] >> 24) != 0x54u) continue;
        ncond++;
        int64_t d;
        int cond = dec_b_cond(&d, w[i]);
        if (cond == COND_NE) n_ne++;
        else if (cond == COND_EQ) n_eq++;
        else {
            fprintf(stderr, "selftest FAIL: b.cond со словом %d — cond %d,"
                            " ждали b.ne или b.eq\n", i, cond);
            return 1;
        }
        int dst = i + (int)d;
        if (dst != branch_target) {
            fprintf(stderr, "selftest FAIL: b.cond на слово %d, ждали %d"
                            " (imm19=%lld)\n", dst, branch_target, (long long)d);
            return 1;
        }
        /* and the target must be inside the emitted block */
        if (dst <= i || dst >= nw) {
            fprintf(stderr, "selftest FAIL: b.cond из слова %d вне блока"
                            " -> %d (блок %d слов)\n", i, dst, nw);
            return 1;
        }
    }
    if (ncond != 3 || n_ne != 2 || n_eq != 1) {
        fprintf(stderr, "selftest FAIL: условных переходов %d (b.ne %d, b.eq %d),"
                        " ждали 3 (2 b.ne + 1 b.eq)\n", ncond, n_ne, n_eq);
        return 1;
    }

    /* The MS_LAZYTIME discriminator itself, as a contiguous sequence:
     *
     *     movz w9, #0x200, lsl #16   0x52a04009
     *     tst  w3, w9                0x6a09007f
     *     b.eq pass
     *
     * Checking the words rather than trusting the emitter is the point: this
     * sequence is the only thing keeping the handler off AppFuseUtil::Mount(),
     * and a wrong immediate here would silently redirect app-fuse mounts with
     * nothing else in the test noticing. */
    {
        int seq_at = -1;
        for (int i = 0; i + 2 < nw; i++) {
            if (w[i] == 0x52a04009u && w[i + 1] == 0x6a09007fu &&
                (w[i + 2] >> 24) == 0x54u) { seq_at = i; break; }
        }
        if (seq_at < 0) {
            fprintf(stderr, "selftest FAIL: нет проверки MS_LAZYTIME"
                            " (movz w9,#0x200,lsl#16; tst w3,w9; b.eq)\n");
            return 1;
        }
        int64_t d = 0;
        if (dec_b_cond(&d, w[seq_at + 2]) != COND_EQ) {
            fprintf(stderr, "selftest FAIL: MS_LAZYTIME проверяется не b.eq"
                            " (слово %d)\n", seq_at + 2);
            return 1;
        }
    }

    /* The take-over path must end `ldp x29,x30,[sp],#16; ret`, and that is the
     * end of the *code*. `nw` spans the whole block including the literal pool
     * and the "/data/media" text, so the last words overall are data, not
     * instructions — anchoring on nw-1 would be checking the string. The code
     * ends at the last `ret` reachable before the pass block; the take-over
     * block's `ret` is the one immediately preceding branch_target's region. */
    int ret_at = -1;
    for (int i = 0; i < branch_target; i++)
        if (w[i] == 0xd65f03c0u) ret_at = i;
    if (ret_at < 1 || w[ret_at - 1] != 0xa8c17bfdu) {
        fprintf(stderr, "selftest FAIL: блок не кончается 'ldp; ret'"
                        " (ret на %d, перед ним %08x)\n",
                ret_at, ret_at >= 1 ? w[ret_at - 1] : 0);
        return 1;
    }

    /* pool check: find the "/data/media" text and the P_RAW word pointing to it */
    const char *raw = "/data/media";
    bool have_txt = false;
    for (int off = 0; off + (int)sizeof("/data/media") <= n; off++) {
        if (memcmp(buf + off, raw, sizeof("/data/media")) == 0) {
            have_txt = true;
            uint64_t raw_va = code_va + (uint64_t)off;
            /* P_RAW is the 5th pool entry, so search for a word == raw_va */
            bool found = false;
            for (int i = 0; i + 1 < nw; i++) {
                uint64_t v;
                memcpy(&v, &w[i], 8);
                if (v == raw_va) { found = true; break; }
            }
            if (!found) {
                fprintf(stderr, "selftest FAIL: нет пулового слова, "
                                "указывающего на \"%s\" (%#llx)\n",
                        raw, (unsigned long long)raw_va);
                return 1;
            }
            break;
        }
    }
    if (!have_txt) {
        fprintf(stderr, "selftest FAIL: текст \"%s\" не найден в блоке\n", raw);
        return 1;
    }

    /* the other four pool values must be present verbatim */
    uint64_t want[] = { type_va, src_va, mount_va };
    for (size_t k = 0; k < sizeof(want) / sizeof(want[0]); k++) {
        bool found = false;
        for (int i = 0; i + 1 < nw; i++) {
            uint64_t v;
            memcpy(&v, &w[i], 8);
            if (v == want[k]) { found = true; break; }
        }
        if (!found) {
            fprintf(stderr, "selftest FAIL: в пуле нет значения %#llx\n",
                    (unsigned long long)want[k]);
            return 1;
        }
    }

    /* mov w3, #0x5000 must exist (MS_BIND|MS_REC) */
    bool have_f = false;
    for (int i = 0; i < nw; i++)
        if (w[i] == enc_movz_w(3, 4096u | 16384u)) have_f = true;
    if (!have_f) {
        fprintf(stderr, "selftest FAIL: нет 'mov w3, #0x5000'\n");
        return 1;
    }

    /* ---- the stub patch must fit, and carry the WHOLE address ----
     *
     * This is the check whose absence cost a bootloop. The patch has to encode a
     * full 64-bit handler address inside a 16-byte slot, and exactly one layout
     * does: `ldr x17,#8` at +0 (which therefore reads +8), `br x17` at +4, quad
     * at +8. Asserting the words alone is not enough — the failure mode was a
     * literal that fell past the end of the buffer, so the test also decodes the
     * load the way the CPU does and requires the 8 bytes it addresses to hold the
     * address we asked for, and to lie inside STUB_PATCH_LEN. */
    {
        uint8_t sp[STUB_PATCH_LEN];
        uint64_t want_handler = 0x000000ab1234ef00ULL;  /* deliberately > 32 bits */
        memset(sp, 0xAA, sizeof(sp));   /* poison — a byte we fail to write shows */
        build_stub_patch(sp, want_handler);

        uint32_t s0, s1;
        memcpy(&s0, sp + 0, 4);
        memcpy(&s1, sp + 4, 4);
        if (s0 != 0x58000051u) {
            fprintf(stderr, "selftest FAIL: трамплин, слово0 %08x — не 'ldr x17,#8'\n", s0);
            return 1;
        }
        if (s1 != 0xd61f0220u) {
            fprintf(stderr, "selftest FAIL: трамплин, слово1 %08x — не 'br x17'\n", s1);
            return 1;
        }

        /* decode the literal offset the way the CPU does: pc + sign_extend(imm19)*4 */
        int64_t lit_off = stub_literal_off(sp);
        if (lit_off != 8) {
            fprintf(stderr, "selftest FAIL: 'ldr x17' читает +%lld, ждали +8\n",
                    (long long)lit_off);
            return 1;
        }
        if (lit_off + 8 > STUB_PATCH_LEN) {
            fprintf(stderr, "selftest FAIL: литерал +%lld..+%lld не влезает в %d байт\n",
                    (long long)lit_off, (long long)lit_off + 7, STUB_PATCH_LEN);
            return 1;
        }
        uint64_t got_handler = 0;
        memcpy(&got_handler, sp + lit_off, 8);
        if (got_handler != want_handler) {
            fprintf(stderr, "selftest FAIL: в литерале %016llx, ждали %016llx "
                            "(старшая половина адреса потеряна?)\n",
                    (unsigned long long)got_handler,
                    (unsigned long long)want_handler);
            return 1;
        }
        if (!stub_is_patched(sp)) {
            fprintf(stderr, "selftest FAIL: stub_is_patched не узнал свой же патч\n");
            return 1;
        }
    }

    if (anchor_selftest() != 0) return 1;
    if (handlers_fit_selftest() != 0) return 1;
    if (umount_handler_exec_selftest() != 0) return 1;

    return 0;
}

/* ------------------------------------------------------------------ *
 * find_handler_room — pick where the handler goes
 *
 * The handler must live in vold's own address space, in memory that is
 * executable and that we can write through /proc/<pid>/mem. The .plt is
 * exactly that: a file-backed r-x mapping, writable by COW, and where the
 * linker leaves a run of zero padding after the final stub, that padding is
 * unambiguous free space — nothing jumps into it. Not every build leaves any:
 * the device's vold packs its PLT flush against the end of the executable
 * segment, 0 bytes after the last stub, and then main() falls through to
 * vold_exec_page().
 *
 * We walk the executable segment that contains the stub, find the byte after
 * the last canonical PLT stub, align forward to 16 bytes, and require a run of
 * zeros long enough for the handler. The zero requirement is what makes this
 * safe: if the bytes are not zero, they are code or data and we refuse.
 * ------------------------------------------------------------------ */
static int find_handler_room(Src *s, uint64_t stub_va, uint64_t *out_va) {
    /* Find the executable PT_LOAD containing the stub. */
    const Elf64_Phdr *seg = NULL;
    for (int i = 0; i < s->phnum; i++) {
        const Elf64_Phdr *p = &s->ph[i];
        if (p->p_type != PT_LOAD || !(p->p_flags & PF_X)) continue;
        if (stub_va >= p->p_vaddr && stub_va + 16 <= p->p_vaddr + p->p_filesz) {
            seg = p;
            break;
        }
    }
    if (!seg) {
        return -1;
    }

    size_t len = (size_t)seg->p_filesz;
    if (len < 4096 || len > (64u << 20)) return -1;
    uint8_t *buf = malloc(len);
    if (!buf) return -1;
    if (src_pread(s, seg->p_vaddr, buf, len) != 0) {
        free(buf);
        return -1;
    }

    /* Last stub = last `br x17` (0xd61f0220) at a 16-byte boundary.
     *
     * Reading the whole segment to find it is not avoidable by looking at the
     * caller's own stub first, tempting as that is: the refusal is decided by
     * the LAST stub, not by the one that was asked about. On the device's vold
     * `mount` is .rela.plt[58] of ~470, so its stub sits 544 bytes before the
     * end of an 0xcf000-byte segment — a guard of the form "this stub is too
     * close to the end" would pass and the read would happen anyway, while the
     * answer (no room: the PLT is flush against the segment end) comes from the
     * last stub. Measured with tools/roomtest.c. */
    size_t rel_stub = (size_t)(stub_va - seg->p_vaddr);
    size_t last = 0;
    bool have = false;
    for (size_t off = (rel_stub + 15) & ~(size_t)15; off + 16 <= len; off += 16) {
        uint32_t w3;
        memcpy(&w3, buf + off + 12, 4);
        if (w3 == 0xd61f0220u) { last = off + 16; have = true; }
    }
    if (!have) last = rel_stub + 16;

    /* Align forward, then require zeros. The handler must not cross into the
     * next mapping, i.e. must stay inside the segment. */
    size_t at = (last + 15) & ~(size_t)15;
    size_t need = 512;               /* generous: handler is ~200 bytes */
    if (at + need > len) {
        free(buf);
        return -1;
    }

    /* Find a zero run of `need` bytes at or after `at`, 16-byte aligned. */
    for (size_t off = at; off + need <= len; off += 16) {
        bool all_zero = true;
        for (size_t k = 0; k < need; k += 64) {
            size_t n = (need - k < 64) ? need - k : 64;
            for (size_t j = 0; j < n; j++) {
                if (buf[off + k + j] != 0) { all_zero = false; break; }
            }
            if (!all_zero) break;
        }
        if (all_zero) {
            free(buf);
            *out_va = seg->p_vaddr + off;
            return 0;
        }
    }

    free(buf);
    return -1;
}

/* ------------------------------------------------------------------ *
 * One redirected symbol
 *
 * Both hooks are installed the same way and can fail in the same ways, so the
 * sequence lives in one place: resolve the symbol's PLT stub, read the libc
 * address its GOT slot already holds (BIND_NOW, so it never changes), keep the
 * stub's original bytes so a half-installed patch can be undone, then write
 * and verify. A second copy of this for umount2 would be a second place for
 * the literal-offset check in hook_install() to be forgotten — and that check
 * is the one that caught the bootloop. */
typedef struct {
    const char *sym;
    Resolved    res;
    uint64_t    run_stub;              /* stub VA as the process sees it */
    uint64_t    real;                  /* libc target, from the GOT      */
    uint8_t     cur[STUB_PATCH_LEN];   /* original bytes, for rollback   */
} Hook;

static int hook_resolve(Src *s, Hook *h, const char *sym) {
    memset(h, 0, sizeof(*h));
    h->sym = sym;

    int rc = resolve_sym(s, sym, &h->res);
    if (rc != EXIT_OK) return rc;

    h->run_stub = s->is_proc ? s->base + h->res.stub_va : h->res.stub_va;

    if (s->is_proc) {
        /* With BIND_NOW (vold has it) every GOT slot is resolved at load time
         * and never changes again, so the handler can carry the real libc
         * addresses. A zero slot means the process is not fully relocated and
         * we must not patch. */
        if (mem_read(s->pid, s->base + h->res.got_slot, &h->real,
                     sizeof(h->real)) != 0) {
            return EXIT_NO_RESOLVE;
        }
        if (!h->real) {
            return EXIT_NO_RESOLVE;
        }
        if (mem_read(s->pid, h->run_stub, h->cur, sizeof(h->cur)) != 0) {
            return EXIT_NO_RESOLVE;
        }
    } else {
        if (src_pread(s, h->res.stub_va, h->cur, sizeof(h->cur)) != 0) {
            return EXIT_NO_RESOLVE;
        }
    }

    return EXIT_OK;
}

/* The stub still points at its own GOT slot: untouched, ours to take. */
static bool hook_stub_intact(const Hook *h) {
    uint64_t tgt = 0;
    return decode_stub(h->cur, h->res.stub_va, &tgt) && tgt == h->res.got_slot;
}

static int hook_install(Src *s, Hook *h, uint64_t handler_va) {
    uint8_t patch[STUB_PATCH_LEN];
    build_stub_patch(patch, handler_va);
    if (mem_write(s->pid, h->run_stub, patch, sizeof(patch)) != 0) {
        return EXIT_NO_WRITE;
    }

    /* Verify the stub is now ours — instructions AND literal.
     *
     * Checking the two instruction words alone is not enough, and that is not
     * hypothetical: the version that shipped a bootloop had exactly the right
     * words, and a literal that fell past the end of the 16 bytes, so the load
     * picked up the neighbouring stub's adrp for the top half of the address.
     * `stub_is_patched` said yes. So decode the load the way the CPU will and
     * require the 8 bytes it addresses to be the handler address. */
    uint8_t chk[STUB_PATCH_LEN];
    if (mem_read(s->pid, h->run_stub, chk, sizeof(chk)) != 0 ||
        !stub_is_patched(chk)) {
        mem_write(s->pid, h->run_stub, h->cur, sizeof(h->cur));
        return EXIT_NO_WRITE;
    }
    int lit = stub_literal_off(chk);
    uint64_t got = 0;
    if (lit >= 0 && lit + 8 <= STUB_PATCH_LEN) memcpy(&got, chk + lit, 8);
    if (lit < 0 || lit + 8 > STUB_PATCH_LEN || got != handler_va) {
        mem_write(s->pid, h->run_stub, h->cur, sizeof(h->cur));
        return EXIT_NO_WRITE;
    }
    return EXIT_OK;
}

/* ------------------------------------------------------------------ *
 * Writing the handler blob out for offline disassembly
 *
 * selftest() proves the handlers *work* by running them; this proves they are
 * what the comments say they are, by letting a disassembler read the same
 * bytes. Both are needed, and neither replaces the other: a handler can run
 * correctly by accident (the mount handler did, with a literal that fell past
 * the end of its stub), and a disassembly cannot tell you that the path matcher
 * accepts the right strings.
 *
 * The blob is built for a fixed, arbitrary base address. The handlers are
 * position-independent — every reference to the outside world goes through
 * their own literal pool — so the base only shifts the printed addresses.
 * Layout: the mount handler, then the umount2 handler at the next 8-byte
 * boundary, which is the layout main() installs. */
static int emit_handlers(const char *path) {
    uint8_t buf[1024];
    memset(buf, 0, sizeof(buf));

    const uint64_t code_va   = 0x100000000ULL;      /* arbitrary */
    const uint64_t mount_va  = 0x7f0000000000ULL;   /* plausible libc addrs */
    const uint64_t umount_va = 0x7f0000001000ULL;
    const uint64_t src_va    = 0x5f0000002000ULL;   /* vold's "/dev/fuse"   */
    const uint64_t type_va   = 0x5f0000002040ULL;   /* vold's "fuse"        */

    int a = build_handler(buf, sizeof(buf), code_va, mount_va,
                          src_va, type_va, RAW_PATH);
    if (a <= 0) return EXIT_NO_RESOLVE;
    int a_len = (a + 7) & ~7;
    uint64_t um_va = code_va + (uint64_t)a_len;
    int b = build_umount_handler(buf + a_len, sizeof(buf) - (size_t)a_len,
                                 um_va, umount_va);
    if (b <= 0) return EXIT_NO_RESOLVE;

    FILE *f = fopen(path, "wb");
    if (!f) return EXIT_NO_WRITE;
    size_t wrote = fwrite(buf, 1, (size_t)(a_len + b), f);
    fclose(f);
    if (wrote != (size_t)(a_len + b)) return EXIT_NO_WRITE;

    printf("handlers.bin: %d байт\n", a_len + b);
    printf("  \"%s\"   @ 0x%llx .. 0x%llx (%d байт)\n",
           TARGET_SYM, (unsigned long long)code_va,
           (unsigned long long)(code_va + (uint64_t)a - 1), a);
    printf("  \"%s\" @ 0x%llx .. 0x%llx (%d байт)\n",
           TARGET_SYM2, (unsigned long long)um_va,
           (unsigned long long)(um_va + (uint64_t)b - 1), b);
    return EXIT_OK;
}

static void usage(void) {
    fputs("usage: vold-fusefs [--wait SEC] [--pid PID] [--check] [--dry-run]\n"
          "                   [--file ELF] [--selftest] [--emit FILE] [--quiet]\n"
          "\n"
          "Stops vold from mounting FUSE for emulated storage, and keeps the\n"
          "teardown consistent with it. Two stubs are redirected:\n"
          "\n"
          "  mount    MountUserFuse's call becomes a FUSE mount with a bind\n"
          "           of /data/media stacked on top, so the path everything\n"
          "           sees is the raw tree. The AppFuse call is left alone.\n"
          "  umount2  a target shaped /mnt/user/<uid>/emulated is unmounted\n"
          "           repeatedly until the path is really clear. Without it\n"
          "           vold's single-umount teardown would leave the FUSE\n"
          "           mount behind and the volume would go unmountable.\n"
          "\n"
          "  --selftest   verify the handlers this tool emits. Runs anywhere,\n"
          "               touches nothing, needs no root. Checks the shape of\n"
          "               both blocks (entry point, branch targets, literal\n"
          "               pool, the path string, the mount flags), then runs\n"
          "               the umount2 handler on the CPU against a stub that\n"
          "               models umount2's own semantics, over every path\n"
          "               shape it must accept and reject. Run this first — a\n"
          "               bad instruction word is a jump into the void inside\n"
          "               vold, and a bad path match is a volume that will not\n"
          "               unmount.\n"
          "  --emit FILE  write the handler blob to FILE for offline\n"
          "               disassembly, and print the addresses it was built\n"
          "               for. The handlers are position-independent, so any\n"
          "               base works.\n"
          "  --file ELF   use a file instead of a live process. Combine with\n"
          "               --check to validate the anchor offline against a\n"
          "               copy of /system/bin/vold before touching a device.\n"
          "  --check      resolve and report, patch nothing.\n"
          "  --dry-run    do everything except the writes.\n"
          "  --wait SEC   seconds to wait for vold (default 3).\n"
          "  --pid PID    target this pid instead of finding vold.\n"
          "\n"
          "Verification chain, cheapest first:\n"
          "  vold-fusefs --selftest\n"
          "  vold-fusefs --emit /tmp/h.bin   # then disassemble that\n"
          "  vold-fusefs --file /data/local/tmp/vold-copy --check   (as root\n"
          "      if the copy is only root-readable; exit 1 here means the\n"
          "      anchor resolved but the stub is not patched yet — expected)\n"
          "  su -c 'vold-fusefs --dry-run'\n"
          "  su -c 'vold-fusefs'\n"
          "\n"
          "Both stubs are installed together or not at all: a vold carrying\n"
          "only the mount hook would stack two mounts and clear one, which is\n"
          "the failure this tool exists to prevent.\n"
          "\n"
          "Exit codes: 0 ok; 1 no vold found; 2 anchor did not resolve;\n"
          "            3 could not write.\n",
          stdout);
}

int main(int argc, char **argv) {
    int wait_sec = 3;
    long pid_opt = 0;
    bool check = false, dry_run = false, do_selftest = false;
    const char *file = NULL;
    const char *emit_file = NULL;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--wait") && i + 1 < argc) {
            wait_sec = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--pid") && i + 1 < argc) {
            pid_opt = atol(argv[++i]);
        } else if (!strcmp(argv[i], "--check")) {
            check = true;
        } else if (!strcmp(argv[i], "--dry-run")) {
            dry_run = true;
        } else if (!strcmp(argv[i], "--file") && i + 1 < argc) {
            file = argv[++i];
        } else if (!strcmp(argv[i], "--selftest")) {
            do_selftest = true;
        } else if (!strcmp(argv[i], "--emit") && i + 1 < argc) {
            emit_file = argv[++i];
        } else {
            usage();
            return 2;
        }
    }

    if (do_selftest) return selftest();
    if (emit_file) return emit_handlers(emit_file);

    char exe[4096] = {0};
    Src s;
    memset(&s, 0, sizeof(s));
    s.fd = -1;
    s.is_proc = false;

    if (file) {
        s.label = file;
        s.fd = open(file, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
            return EXIT_NO_RESOLVE;
        }
        if (src_open_header(&s) != 0) { src_close(&s); return EXIT_NO_RESOLVE; }
        goto resolved_as_file;
    }

    {
        pid_t pid;
        if (pid_opt > 0) {
            pid = (pid_t)pid_opt;
        } else {
            pid = find_vold(wait_sec, exe, sizeof(exe));
            if (pid < 0) {
                return EXIT_NO_VOLD;
            }
        }

        uint64_t base = 0;
        if (find_load_base(pid, exe, &base, exe, sizeof(exe)) != 0) {
            return EXIT_NO_RESOLVE;
        }

        s.is_proc = true;
        s.pid = pid;
        s.base = base;
        s.label = "vold";

        char mempath[64];
        snprintf(mempath, sizeof(mempath), "/proc/%d/mem", (int)pid);
        s.fd = open(mempath, O_RDONLY | O_CLOEXEC);
        if (s.fd < 0) {
            return EXIT_NO_RESOLVE;
        }
        if (src_open_header(&s) != 0) { src_close(&s); return EXIT_NO_RESOLVE; }
    }

resolved_as_file:
    ;

    /* ---- both hooks -------------------------------------------------
     *
     * mount and umount2 are one change, not two. The mount handler stacks a
     * second mount at fuse_path; the umount2 handler is what clears it again.
     * A vold carrying only the first is strictly worse than an unpatched vold:
     * it would stack two mounts and remove one, which is the ENOTCONN
     * regression itself. So the two are resolved, verified and installed
     * together, and neither is left behind without the other. */
    Hook hm, hu;
    if (hook_resolve(&s, &hm, TARGET_SYM) != EXIT_OK) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }
    if (hook_resolve(&s, &hu, TARGET_SYM2) != EXIT_OK) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    FuseSite fs;
    memset(&fs, 0, sizeof(fs));
    if (find_fuse_site(&s, hm.res.stub_va, &fs) != 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    bool m_ours = stub_is_patched(hm.cur);
    bool u_ours = stub_is_patched(hu.cur);

    if (m_ours || u_ours) {
        if (m_ours && u_ours) {
            src_close(&s);
            return EXIT_OK;
        }
        /* Half a patch. Saying "OK" here would be a lie with consequences: the
         * volume would mount with two layers and unmount one. */
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (!hook_stub_intact(&hm)) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }
    if (!hook_stub_intact(&hu)) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }

    if (!s.is_proc) {
        /* A file is answered exactly as --check/--dry-run on a process would be. */
        if (check) {
            src_close(&s);
            return 1;
        }
        src_close(&s);
        return EXIT_OK;
    }

    /* ---- live process ---- */

    if (check) {
        src_close(&s);
        return 1;
    }
    if (dry_run) {
        src_close(&s);
        return EXIT_OK;
    }

    /*
     * Where the handlers go: the cheapest home is the trailing padding of the
     * .plt page — a file-backed r-x mapping, writable through /proc/<pid>/mem
     * by COW, no mmap/mprotect/syscall injection needed. But that padding is
     * not guaranteed: a build can pack its PLT flush to the end of the
     * executable segment (measured on the device's vold: 0 bytes after the
     * last stub, and no zero run >= 256 bytes anywhere in .text). Then the
     * handler needs memory the image does not provide, and we ask vold itself
     * for a page: vold is made to map a page of its own binary executable
     * (see vold_exec_page).
     *
     * The handlers are built for the address they will live at, so that
     * address is fixed here first, as an ABSOLUTE one: the PLT search returns
     * an offset into the image, which becomes s.base + off in the process,
     * while the injected page is already absolute. The PLT search is tried
     * first because it leaves no new mapping and no trace in /proc/<pid>/maps.
     */
    uint64_t handler_abs = 0;
    {
        uint64_t off = 0;
        if (find_handler_room(&s, hm.res.stub_va, &off) == 0) {
            handler_abs = s.base + off;
        }
#if defined(__aarch64__)
        else {
            /* No room in the image. Ask vold to map a page of its own binary
             * executable — anonymous executable memory is refused (see the
             * note above vold_exec_page). */
            char self[4096];
            if (!pid_is_vold(s.pid, self, sizeof(self))) {
                src_close(&s);
                return EXIT_NO_RESOLVE;
            }
            handler_abs = vold_exec_page(s.pid, self);
            if (handler_abs == 0) {
                src_close(&s);
                return EXIT_NO_RESOLVE;
            }
        }
#else
        else {
            src_close(&s);
            return EXIT_NO_RESOLVE;
        }
#endif
    }

    /* Both handlers share the one zero run, laid out back to back: the mount
     * handler first, then the umount2 handler 8-byte aligned so its literal
     * pool stays naturally aligned. */
    uint8_t hbuf[1024];
    int hlen = build_handler(hbuf, sizeof(hbuf), handler_abs,
                             hm.real,
                             s.base + fs.src_va, s.base + fs.type_va,
                             RAW_PATH);
    if (hlen <= 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }
    int h1_len = (hlen + 7) & ~7;
    uint64_t um_handler_abs = handler_abs + (uint64_t)h1_len;
    int u_len = build_umount_handler(hbuf + h1_len,
                                     sizeof(hbuf) - (size_t)h1_len,
                                     um_handler_abs, hu.real);
    if (u_len <= 0) {
        src_close(&s);
        return EXIT_NO_RESOLVE;
    }
    int total = h1_len + u_len;


    /* 1) write both handlers into the chosen home */
    if (mem_write(s.pid, handler_abs, hbuf, (size_t)total) != 0) {
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    /* 2) verify they landed, byte for byte */
    uint8_t back[1024];
    if (mem_read(s.pid, handler_abs, back, (size_t)total) != 0 ||
        memcmp(back, hbuf, (size_t)total) != 0) {
        /* Roll back: the region was verified all-zero before we wrote, so
         * restoring it to zero returns vold to its pre-patch state (neither
         * stub is touched yet, so nothing is redirecting). */
        uint8_t zeros[1024];
        memset(zeros, 0, (size_t)total);
        mem_write(s.pid, handler_abs, zeros, (size_t)total);
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    /* 3) redirect both stubs. Each is 16 bytes and so is our replacement, so
     * no neighbouring stub is touched. The mount hook goes first, so that a
     * failure on the second one can be undone by putting the first back. */
    if (hook_install(&s, &hm, handler_abs) != EXIT_OK) {
        uint8_t zeros[1024];
        memset(zeros, 0, (size_t)total);
        mem_write(s.pid, handler_abs, zeros, (size_t)total);
        src_close(&s);
        return EXIT_NO_WRITE;
    }
    if (hook_install(&s, &hu, um_handler_abs) != EXIT_OK) {
        /* Undo the first. A vold with the mount hook but not the teardown hook
         * is worse than an unpatched vold — that is the regression itself — so
         * falling back to "unpatched" is the only safe outcome here. */
        mem_write(s.pid, hm.run_stub, hm.cur, sizeof(hm.cur));
        uint8_t zeros[1024];
        memset(zeros, 0, (size_t)total);
        mem_write(s.pid, handler_abs, zeros, (size_t)total);
        src_close(&s);
        return EXIT_NO_WRITE;
    }

    src_close(&s);
    return EXIT_OK;
}
