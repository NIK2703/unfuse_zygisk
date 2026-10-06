/*
 * acl-dump — print a path's POSIX ACL in readable form.
 * Diagnostic tool, not shipped (built by hand, not by build.sh). toybox getfattr
 * prints the value as a C string and stops at the first NUL (the ACL version
 * field), so it is useless here.
 *   acl-dump <path>...
 * Output: <path> <access|default> <TAG> <rwx> id=<N>; GROUP id=9997 is the module's
 * grant, id=1023 (media_rw) in the default ACL is vold's.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/xattr.h>

#define XATTR_ACL_ACCESS "system.posix_acl_access"
#define XATTR_ACL_DEFAULT "system.posix_acl_default"

#define ACL_USER_OBJ  0x01
#define ACL_USER      0x02
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP     0x08
#define ACL_MASK      0x10
#define ACL_OTHER     0x20

static const char *tag_name(uint16_t t)
{
    switch (t) {
    case ACL_USER_OBJ:  return "USER_OBJ";
    case ACL_USER:      return "USER";
    case ACL_GROUP_OBJ: return "GROUP_OBJ";
    case ACL_GROUP:     return "GROUP";
    case ACL_MASK:      return "MASK";
    case ACL_OTHER:     return "OTHER";
    default:            return "?";
    }
}

static void show(const char *path, const char *xattr, const char *label)
{
    uint8_t buf[1024];
    ssize_t n = getxattr(path, xattr, buf, sizeof buf);
    ssize_t off;

    if (n < 0) {
        printf("%s %s: нет (%s)\n", path, label, strerror(errno));
        return;
    }
    if (n < 4) {
        printf("%s %s: слишком короткий (%zd байт)\n", path, label, n);
        return;
    }

    /* uint32 version, then 8-byte entries: u16 tag, u16 perm, u32 id */
    for (off = 4; off + 8 <= n; off += 8) {
        uint16_t tag, perm;
        uint32_t id;

        memcpy(&tag, buf + off, 2);
        memcpy(&perm, buf + off + 2, 2);
        memcpy(&id, buf + off + 4, 4);

        printf("%s %-7s %-9s %c%c%c id=%u\n", path, label, tag_name(tag),
               (perm & 4) ? 'r' : '-', (perm & 2) ? 'w' : '-',
               (perm & 1) ? 'x' : '-', id);
    }
}

int main(int argc, char **argv)
{
    int i;

    if (argc < 2) {
        fprintf(stderr, "использование: acl-dump <путь>...\n");
        return 1;
    }
    for (i = 1; i < argc; i++) {
        show(argv[i], XATTR_ACL_ACCESS, "access");
        show(argv[i], XATTR_ACL_DEFAULT, "default");
    }
    return 0;
}
