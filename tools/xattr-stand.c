/* Stand-in binary calling setxattr exactly like vold does — through .plt.
 * Tests vold-noacl on a foreign build (different linker, symbols, relocations).
 * NDK clang aarch64, with and without BTI. Not part of the module. */

#include <sys/types.h>
#include <sys/xattr.h>
#include <string.h>
#include <stdio.h>

int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/data/local/tmp/_xattrtest";
    const char acl[] = { 2, 0, 0, 0, 1, 0, 7, 0, 0, 0, 0, 0, 0, 0, 0, 0,
                         4, 0, 7, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0 };

    int r = setxattr(path, "system.posix_acl_default", acl, sizeof(acl), 0);
    if (r != 0) {
        perror("setxattr");
        return 1;
    }
    printf("setxattr ok\n");
    return 0;
}
