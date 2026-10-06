/*
 * android_ver.h — the Android releases this module's patches are validated
 * against, and what each of them expects.
 *
 * This is NOT a per-release byte fork. Both patchers read the target's own ELF
 * tables, so neither holds an offset to swap, and 15, 16 and 17 resolve to the
 * very same bytes: same 22 libc entry points, same 9 roots, same single setxattr
 * call in vold. What does differ per release is the EXPECTATION — how many
 * targets the module's list covers there, and which AOSP site writes the ACL that
 * vold-noacl disarms. The table records those, so a release that shifts the shape
 * shows up in the log instead of silently patching less than it claims.
 *
 * A release not in the table gets the newest profile (UNFUSE_VER_LATEST) — the
 * one most likely to still hold — and the run is marked as unvalidated, so
 * nobody reads "patched" on an untested release as "verified". The patch's own
 * safety checks are version-independent and stay unconditional whatever this
 * table says: alignment, function size, thunk detection, and for vold "setxattr
 * is called exactly once". The table never loosens them.
 *
 * One row per release, oldest first: the LAST row is the fallback.
 */

#pragma once

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#if defined(__ANDROID__)
#include <sys/system_properties.h>
#endif

typedef struct {
    int         sdk;        /* ro.build.version.sdk */
    int         release;    /* release number, for the log */
    const char *codename;   /* AOSP codename, for the log */
    int         installed;  /* targets covered -> hooks_install()'s return here */
    const char *vold_acl;   /* AOSP site that writes the ACL vold-noacl disarms */
} UnfuseVer;

/* Verified against a real image (device/ holds the reference binaries). Codenames
 * are spelled as the platform itself does — ro.build.version.known_codenames on
 * the 15/16/17 images ends "...VanillaIceCream,Baklava,CinnamonBun".
 *
 *   libc: 15, 16 and 17 all yield the same 9 distinct roots
 *         (__open_2, __openat_2, fchmod, fchmodat, linkat, mkdirat, open, openat,
 *         renameat2), and open64/open and openat64/openat share those addresses,
 *         so 11 of the 22 listed targets come out covered. The offline verifier
 *         reports 9 because it counts distinct addresses; 11 was measured
 *         on-device (Android 16, hookselftest) and follows for 15 and 17 from a
 *         classification the verifier finds identical, target for target.
 *   vold:  setxattr is called exactly once and the stub resolves, on all three
 *         (15/16: Utils.cpp:195, 17: :196).
 *
 * The three are indistinguishable to the patch: all 22 targets classify the same
 * on 15, 16 and 17 — патчится=11, коротка=5, переходник=6 — and the same 9 roots
 * come out. The sizes differ (17 is built with -mbranch-protection=standard, so
 * its thunks carry a bti c prefix and its roots open with paciasp, and its leaf
 * syscall wrappers lost an instruction, 24 -> 20 bytes) but never across the patch
 * width: the tightest fit is mkdirat, renameat2 and linkat at 20 bytes on 17 and
 * 24 on 15/16, against a 20-byte patch. Had the patch stayed 16 bytes, the bti c
 * prefix alone would have moved creat, creat64, mkstemps and mkostemps from
 * "коротка" to "переходник" on 17 and made the three releases look different for
 * no reason. Below 20 bytes of room a target drops out of the count, and the tally
 * check in unfuse_zygisk.cpp says so rather than passing quietly. */
static const UnfuseVer UNFUSE_VERSIONS[] = {
    {35, 15, "VanillaIceCream", 11, "vold-15/Utils.cpp:195"},
    {36, 16, "Baklava",         11, "vold-16/Utils.cpp:195"},
    {37, 17, "CinnamonBun",     11, "vold-17/Utils.cpp:196"},
};

#define UNFUSE_VER_COUNT  ((int)(sizeof(UNFUSE_VERSIONS) / sizeof(UNFUSE_VERSIONS[0])))

/* Unlisted releases borrow the newest profile. Keep the table oldest -> newest. */
#define UNFUSE_VER_LATEST (&UNFUSE_VERSIONS[UNFUSE_VER_COUNT - 1])

typedef struct {
    const UnfuseVer *v;      /* profile in force: the matching row, or the newest */
    int              sdk;    /* what was detected; -1 when unreadable */
    int              known;  /* 1: an exact row matched */
} UnfusePick;

/* ro.build.version.sdk, or -1. The property area is a shared read-only mapping,
 * so this is a memcpy, not a round-trip to init. */
static inline int unfuse_sdk(void) {
#if defined(__ANDROID__)
    char v[PROP_VALUE_MAX];
    if (__system_property_get("ro.build.version.sdk", v) <= 0) return -1;
    return atoi(v);
#else
    return -1;
#endif
}

/* Resolve a detected sdk to a profile. An unreadable sdk is not a reason to
 * refuse: it falls through to the fallback profile, flagged unverified, and the
 * patchers' own checks still decide whether the patch is safe. */
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

/* "android 17 (sdk 37, cinnamonbun)" — or, when the release is not in the table,
 * the fact that the profile is borrowed rather than verified. */
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
