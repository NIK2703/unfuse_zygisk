/*
 * func_size.h — function sizes from the .dynsym of the object they live in.
 *
 * The 20-byte patch must never overwrite the next function's start, so only
 * targets >= 20 bytes may be patched. On Android 16 libc eight hook targets are
 * shorter: creat/creat64 12, mkdir 16, renameat 8, mkstemp/mkostemp 16,
 * mkstemps/mkostemps 12. The set moves per release: mkdir is 16 on 13-16 but
 * 20 on 11/12/12L and 17, "too short" there becoming "thunk" (android_ver.h).
 *
 * Check: tools/verify-hook-targets.py device/libc/libc-arm64.so
 */

#pragma once

// Byte size of each fns[i]; 0 = unknown, do not patch. All addresses must
// belong to one object; a target from another object keeps 0.
void func_sizes(void *const *fns, int n, unsigned *sizes);
