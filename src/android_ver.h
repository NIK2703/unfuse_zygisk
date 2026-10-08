/*
 * android_ver.h — the Android releases this module's patches are validated
 * against, and what each of them expects.
 *
 * Not a per-release byte fork: both patchers read the target's own ELF
 * tables, so 11-17 resolve through the same code — the same 12 libc entry
 * points, the same single setxattr call in vold. Only the EXPECTATION
 * differs: how many of those entry points the list covers there, and which AOSP
 * site writes the ACL vold-noacl disarms — so a shifted shape is visible instead
 * of silently patching less than it claims. Visible where it can be read: this
 * table is consulted by hooks_release(), which tools/hookselftest.cpp calls.
 *
 * An unlisted release takes UNFUSE_VER_LATEST, marked unvalidated so nobody
 * reads "patched" on it as "verified"; the patch's own checks (alignment,
 * function size, thunk detection, and for vold "setxattr is called exactly
 * once") stay unconditional whatever this table says. */

#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

typedef struct {
    int         sdk;        /* ro.build.version.sdk */
    int         release;    /* release number, for hooks_release()'s string */
    const char *codename;   /* AOSP codename, for hooks_release()'s string */
    int         installed;  /* targets covered -> hooks_install()'s return here */
    const char *vold_acl;   /* AOSP site that writes the ACL vold-noacl disarms */
} UnfuseVer;

/* Verified against a real image (device/ holds the reference binaries).
 *
 *   libc: `installed` = how many of the 12 listed targets come out PATCHED,
 *   counted as ROOTS, aliases included (hooks_install counts Ok + Alias): ten
 *   on 11/12/12L/13 -> 12, nine on 14-17 -> 11, since renameat from 14 on is
 *   a tail branch onto the patched renameat2 (8 bytes on 16, reads "коротка").
 *   It is in kHooks for the other four, whose SYSCALLS.TXT still lists it as a
 *   syscall of its own: a leaf patched as a root, rename a thunk onto it. 11
 *   was measured on-device (Android 16, hookselftest); the rest match that.
 *   vold:  the column is a SITE, not a count — setxattr is called exactly once
 *   (11/12/12L/13: Utils.cpp:192, 14/15/16: :195, 17: :196), one
 *   R_AARCH64_JUMP_SLOT each. 11 writes that ACL from fewer places, which is
 *   why the patch disarms setxattr, not a caller: one no-op covers every site.
 *
 * 17 alone is built with -mbranch-protection=standard: its thunks open with
 * bti c and are 4 bytes longer, flipping mkdir and the mktemp wrappers across
 * the 20-byte gate (17 reads 11/5/6, 14/15/16 read 11/8/3). 11 has no bti or
 * paciasp at all (13: 68, 17: 799), yet its mkdir is 20 bytes too, clang
 * spilling the path to x8 before w0 is overwritten:
 *  11/12/12L  mov x8,x0 / mov w0,#AT_FDCWD / mov w2,w1 / mov x1,x8 / b mkdirat
 *  13-16      mov w2,w1 / mov x1,x0 / mov w0,#AT_FDCWD / b mkdirat
 *
 * 20 bytes is exactly the patch width, so 11/12/12L's mkdir clears the size
 * gate and lands in the thunk test instead, skipped and covered either way
 * (12/6/4 vs 13's 12/7/3). The tightest patched fit is 24 bytes (mkdirat,
 * linkat, renameat2, plus renameat on 11/12/12L/13) on 11-16 and exactly 20
 * on 17 — no margin. */
static const UnfuseVer UNFUSE_VERSIONS[] = {
    {30, 11, "R",               12, "vold-11/Utils.cpp:192"},
    {31, 12, "S",               12, "vold-12/Utils.cpp:192"},
    {32, 12, "Sv2",             12, "vold-12l/Utils.cpp:192"},
    {33, 13, "Tiramisu",        12, "vold-13/Utils.cpp:192"},
    {34, 14, "UpsideDownCake",  11, "vold-14/Utils.cpp:195"},
    {35, 15, "VanillaIceCream", 11, "vold-15/Utils.cpp:195"},
    {36, 16, "Baklava",         11, "vold-16/Utils.cpp:195"},
    {37, 17, "CinnamonBun",     11, "vold-17/Utils.cpp:196"},
};

#define UNFUSE_VER_COUNT  ((int)(sizeof(UNFUSE_VERSIONS) / sizeof(UNFUSE_VERSIONS[0])))

/* Unlisted releases borrow the newest profile; keep the table oldest first. */
#define UNFUSE_VER_LATEST (&UNFUSE_VERSIONS[UNFUSE_VER_COUNT - 1])

typedef struct {
    const UnfuseVer *v;      /* profile in force: the matching row, or the newest */
    int              sdk;    /* what was detected; -1 when unreadable */
    int              known;  /* 1: an exact row matched */
} UnfusePick;

/* ro.build.version.sdk, or -1 — a shared read-only mapping, read in place. */
static inline int unfuse_sdk(void) {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.sdk", v) <= 0) return -1;
    return atoi(v);
#else
    return -1;
#endif
}

/* An unreadable sdk is not a refusal: fallback profile, flagged unverified. */
static inline UnfusePick unfuse_pick(int sdk) {
    UnfusePick p;
    p.v = UNFUSE_VER_LATEST;
    p.sdk = sdk;
    p.known = 0;
    for (int i = 0; i < UNFUSE_VER_COUNT; i++) {
        if (UNFUSE_VERSIONS[i].sdk == sdk) {
            p.v = &UNFUSE_VERSIONS[i];
            p.known = 1;
            break;
        }
    }
    return p;
}

/* "android 17 (sdk 37, cinnamonbun)"; unlisted: borrowed, not verified. */
static inline void unfuse_ver_str(const UnfusePick *p, char *out, size_t len) {
    if (out == NULL || len == 0) return;
    if (p->sdk <= 0) {
        snprintf(out, len, "android ? (ro.build.version.sdk не читается) — "
                           "профиль android %d", p->v->release);
    } else if (p->known) {
        snprintf(out, len, "android %d (sdk %d, %s)",
                 p->v->release, p->sdk, p->v->codename);
    } else {
        snprintf(out, len, "sdk %d не в списке — профиль android %d (%s), "
                           "на этой версии не проверялся",
                 p->sdk, p->v->release, p->v->codename);
    }
}
