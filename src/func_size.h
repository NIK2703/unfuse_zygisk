/*
 * func_size.h — function sizes from the .dynsym of the object they live in.
 *
 * Needed so the 20-byte patch never overwrites the next function's start: only
 * functions >= patch size may be patched. On Android 16 libc eight hook targets
 * are shorter than that (creat/creat64 12, mkdir 16, renameat 8, mkstemp/mkostemp
 * 16, mkstemps/mkostemps 12) — four thunks that jump to the patched root through
 * .plt, and the four mktemp wrappers, which reach open indirectly through
 * mktemp_internal. Only the sizes reveal any of it, so they are read here rather
 * than assumed. The set is not fixed across releases: mkdir is 16 on 13-16 but 20
 * on 12/12L and on 17, which moves it from "too short" to "thunk" without changing
 * that it is skipped. android_ver.h records what each release settles at.
 *
 * Check: tools/verify-hook-targets.py device/libc/libc-arm64.so
 */

#pragma once

// Fills sizes[i] with the byte size of fns[i]; 0 means unknown (do not patch).
// All addresses must belong to one object; addresses from another object get 0.
void func_sizes(void *const *fns, int n, unsigned *sizes);
