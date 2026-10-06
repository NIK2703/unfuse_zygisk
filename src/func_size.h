/*
 * func_size.h — function sizes from the .dynsym of the object they live in.
 *
 * Needed so 16 bytes never overwrite the next function's start: only functions
 * >= patch size may be patched. On Android 16 libc-16 four hook targets are
 * shorter (creat/creat64 12, renameat 8, mkstemps 12, mkostemps 12); all are
 * thunks reaching the patched root via .plt, but only the sizes reveal it.
 *
 * Check: tools/verify-hook-targets.py device/libc/libc-arm64.so
 */

#pragma once

// Fills sizes[i] with the byte size of fns[i]; 0 means unknown (do not patch).
// All addresses must belong to one object; addresses from another object get 0.
void func_sizes(void *const *fns, int n, unsigned *sizes);
