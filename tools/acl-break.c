/*
 * acl-break — harness for the ACL guard (tools/storage-fix.c --guard).
 * Reproduces vold::SetDefaultAcl (vold-16/Utils.cpp:142) as called from
 * FsCrypt.cpp:1027: a 5-entry default ACL whose named entry is group 1023
 * (media_rw) — exactly how vold wipes the module's 9997 entry. Test-only; build by
 * hand like acl-dump.c (README §6.3).
 * usage: acl-break <dir>...
 */

#define _GNU_SOURCE

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/xattr.h>

#define XATTR_ACL_DEFAULT "system.posix_acl_default"

#define POSIX_ACL_XATTR_VERSION 0x0002
#define ACL_USER_OBJ 0x01
#define ACL_GROUP_OBJ 0x04
#define ACL_GROUP 0x08
#define ACL_MASK 0x10
#define ACL_OTHER 0x20

#define AID_MEDIA_RW 1023

struct acl_entry {
    uint16_t e_tag;
    uint16_t e_perm;
    uint32_t e_id;
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <dir>...\n", argv[0]);
        return 2;
    }

    /* mode 02770 as in FsCrypt.cpp:1027: owner rwx, group rwx, other — */
    const uint16_t group_perm = 7;

    struct acl_entry e[5];
    e[0] = (struct acl_entry){ACL_USER_OBJ, 7, (uint32_t)-1};
    e[1] = (struct acl_entry){ACL_GROUP_OBJ, group_perm, (uint32_t)-1};
    e[2] = (struct acl_entry){ACL_GROUP, group_perm, AID_MEDIA_RW};
    e[3] = (struct acl_entry){ACL_MASK, group_perm, 0};
    e[4] = (struct acl_entry){ACL_OTHER, 0, 0};

    uint8_t buf[sizeof(uint32_t) + sizeof(e)];
    const uint32_t version = POSIX_ACL_XATTR_VERSION;
    memcpy(buf, &version, sizeof(version));
    memcpy(buf + sizeof(version), e, sizeof(e));

    int rc = 0;
    for (int i = 1; i < argc; i++) {
        if (setxattr(argv[i], XATTR_ACL_DEFAULT, buf, sizeof(buf), 0) != 0) {
            perror(argv[i]);
            rc = 1;
        } else {
            printf("acl-break: %s: default-ACL переписан как у vold (GROUP id=%d)\n",
                   argv[i], AID_MEDIA_RW);
        }
    }
    return rc;
}
