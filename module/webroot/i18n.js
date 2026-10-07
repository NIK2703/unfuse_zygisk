// Checkmark labels. The key is the same key status.sh prints, so the markup
// list and the shell checks cannot drift apart by accident.
//
// English only. The page used to hold a Russian set beside this one and pick
// between them by navigator.language, so a device with a ru locale showed
// Russian — the reboot banner being the string that stood out. The labels stay
// keyed rather than inlined into index.html: that link to status.sh is the
// reason this file exists, and it survives having one language.
const strings = {
    mount_default: 'sdcardfs on /mnt/runtime/default',
    mount_read: 'sdcardfs on /mnt/runtime/read',
    mount_write: 'sdcardfs on /mnt/runtime/write',
    mount_full: 'sdcardfs on /mnt/runtime/full',
    mount_opts: 'mount options: mask and gid',
    label_media: 'label of /data/media: media_rw_data_file',
    acl_access: 'ACL 9997 — access on /data/media/0',
    acl_default: 'ACL 9997 — default on /data/media/0',
    vold_patched: 'vold patch: setxattr defused',
    fuse_off: 'FUSE off: vold binds the raw tree',
    libc_hooks: 'libc patch in apps',
    pending: 'The selected mode will apply only after a reboot',
    failed: 'Failed to apply the mode'
};

function t(key) {
    return strings[key] || key;
}
