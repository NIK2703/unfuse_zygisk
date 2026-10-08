/*
 * android_ver.h — the Android releases this module's patches are validated
 * against, and what each of them expects.
 *
 * This is NOT a per-release byte fork. Both patchers read the target's own ELF
 * tables, so neither holds an offset to swap, and 11, 12, 12L, 13, 14, 15, 16 and
 * 17 all resolve through the same code: same 12 listed libc entry points, same
 * single setxattr call in vold. What differs per release is the EXPECTATION — how
 * many of those entry points the module's list actually covers there, and which
 * AOSP site writes the ACL that vold-noacl disarms. The table records those, so a
 * release that shifts the shape shows up in the log instead of silently patching
 * less than it claims.
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

/* Verified against a real image (device/ holds the reference binaries, one GSI per
 * release). Codenames are spelled as the platform itself does. 11, 12 and 12L carry
 * no ro.build.version.known_codenames at all, so their names are read off the lists
 * the later images ship: 13's ends "...O,OMr1,P,Q,R,S,Sv2,Tiramisu" and 14's adds
 * UpsideDownCake after it — R is 11, S is 12, Sv2 is 12L. 13 and up name themselves
 * there.
 *
 *   libc: 14, 15, 16 and 17 yield the same 9 distinct roots (__open_2, __openat_2,
 *         fchmod, fchmodat, linkat, mkdirat, open, openat, renameat2), and
 *         open64/open and openat64/openat share those addresses, so 11 of the 12
 *         listed targets are patched there — the twelfth, renameat, is a thunk onto
 *         the patched renameat2 and is covered through it. 11, 12, 12L and 13 yield
 *         TEN: renameat is a syscall stub of its own there and is patched as a root,
 *         so all 12 are patched. The verifier counts roots rather than names because
 *         aliases share an address; 11 was measured on-device (Android 16,
 *         hookselftest) and the rest follow from a classification it finds identical,
 *         target for target.
 *   vold:  setxattr is called exactly once and the stub resolves on all eight
 *         (11/12/12L/13: Utils.cpp:192, 14/15/16: :195, 17: :196), each binary
 *         carries exactly one R_AARCH64_JUMP_SLOT for it, and it is the only
 *         xattr-named import any of them has.
 *
 * 11, 12, 12L and 13 are the releases that differ in WHAT is patched, and they
 * differ upward. Their SYSCALLS.TXT still lists renameat as a syscall of its own, so
 * renameat is a leaf the module patches as a root and rename is a thunk onto it.
 * From 14 on, renameat moved into rename.cpp as a thunk onto renameat2: it became a
 * "переходник" and the root count fell from 10 to 9. The rename family is covered
 * either way — through renameat on 11/12/12L/13, through renameat2 later — and that
 * is why renameat is in kHooks at all: leaning on the renameat2 chain alone would
 * leave rename() unpatched on those four. rename itself is in kHooks on no release:
 * as a thunk it is covered through renameat there and through renameat2 later.
 *
 * Everything else the patch does is identical on all eight. Only the verifier's
 * summary line moves, and it has two unrelated causes. The paragraphs below are
 * about the ten names kHooks[] does NOT list — they keep the labels those names
 * used to carry, because that classification is the measurement behind dropping
 * them: a name that resolves, classifies as a thunk and is skipped does nothing
 * but colour the report. With them gone the report has twelve entries, and on
 * 14/15/16/17 the only one not patched is renameat.
 *
 * 17 alone is built with -mbranch-protection=standard, so its thunks open with
 * bti c and are 4 bytes longer: mkdir (16 -> 20), mkstemp and mkostemp (16 -> 20)
 * cross the 20-byte gate and get reported as "переходник" rather than "коротка".
 * creat/creat64 (12 -> 16) and mkstemps/mkostemps (12 -> 16) stay under the gate
 * and keep their label; rename and link (28 -> 32) and chmod (24) were thunks above
 * it already. That is why 17 reads 11/5/6 where 14/15/16 read 11/8/3. 11 is the other
 * extreme and no help to compare against: libc and vold there contain not one bti or
 * paciasp instruction at all — 13 has 68, 17 has 799 — so branch protection is simply
 * not a factor on that release.
 *
 * 11, 12 and 12L reach the same place without it: their mkdir is a 20-byte thunk, one
 * instruction longer than 13's 16-byte one, because clang there orders the shuffle so
 * that w0 is overwritten before x0 has been copied to x1 and the path has to be
 * spilled to x8 first:
 *
 *     11/12/12L  mov x8,x0 / mov w0,#AT_FDCWD / mov w2,w1 / mov x1,x8 / b mkdirat
 *     13-16      mov w2,w1 / mov x1,x0 / mov w0,#AT_FDCWD / b mkdirat
 *
 * 20 bytes is exactly the patch width, so mkdir clears the size gate there and is then
 * caught by the thunk test, because it ends in an unconditional branch — the same
 * place 17 lands, for a different reason. Skipped either way and covered either way,
 * since a call to mkdir already reaches the patched mkdirat through .plt. So 11, 12
 * and 12L read 12/6/4 against 13's 12/7/3: one label moved, coverage did not.
 *
 * The gate those labels are measured against is the patch width, so that is the number
 * that matters: the tightest fit among patched targets is 24 bytes on 11/12/12L/13/14/
 * 15/16 — mkdirat, linkat and renameat2, plus renameat on 11/12/12L/13 — and exactly
 * 20 on 17, against a 20-byte patch, so 17 has no margin at all. Below 20 bytes of room
 * a target drops out of the count, and the tally check in unfuse_zygisk.cpp says so
 * rather than passing quietly.
 *
 * vold writes that ACL from fewer places on 11 than later, which is why the column is
 * a SITE and not a count. 11 has no FsCrypt.cpp caller at all: its CE prep goes through
 * the local prepare_dir() -> fs_prepare_dir(), which is mkdir/chown/chmod and no ACL,
 * and only Utils.cpp:398 (app dirs, depth 0) and :1643 (the OBB dir) reach SetDefaultAcl.
 * 12 adds FsCrypt.cpp:864 and 16 :1027. The patch does not care, and that is exactly
 * why it disarms setxattr rather than a caller: setxattr appears once in every one of
 * these binaries, so a single no-op covers every call site however many there are. */
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
