// Checkmark labels. The key is the same key status.sh prints, so the markup
// list and the shell checks cannot drift apart by accident.
const strings = {
    ru: {
        mount_default: 'sdcardfs на /mnt/runtime/default',
        mount_read: 'sdcardfs на /mnt/runtime/read',
        mount_write: 'sdcardfs на /mnt/runtime/write',
        mount_full: 'sdcardfs на /mnt/runtime/full',
        mount_opts: 'опции маунтов: mask и gid',
        label_media: 'ярлык /data/media: media_rw_data_file',
        acl_access: 'ACL 9997 — access на /data/media/0',
        acl_default: 'ACL 9997 — default на /data/media/0',
        vold_patched: 'патч vold: setxattr обезврежен',
        libc_hooks: 'правка libc в приложениях',
        pending: 'Выбранный режим применится только после перезагрузки',
        failed: 'Не удалось применить режим'
    },
    en: {
        mount_default: 'sdcardfs on /mnt/runtime/default',
        mount_read: 'sdcardfs on /mnt/runtime/read',
        mount_write: 'sdcardfs on /mnt/runtime/write',
        mount_full: 'sdcardfs on /mnt/runtime/full',
        mount_opts: 'mount options: mask and gid',
        label_media: 'label of /data/media: media_rw_data_file',
        acl_access: 'ACL 9997 — access on /data/media/0',
        acl_default: 'ACL 9997 — default on /data/media/0',
        vold_patched: 'vold patch: setxattr defused',
        libc_hooks: 'libc patch in apps',
        pending: 'The selected mode will apply only after a reboot',
        failed: 'Failed to apply the mode'
    }
}

function t(key) {
    const lang = navigator.language.startsWith('ru') ? 'ru' : 'en';
    return strings[lang][key] || key;
}
