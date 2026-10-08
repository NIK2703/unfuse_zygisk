/*
 * gnu_props.h — read the AArch64 feature word out of a GNU property note.
 *
 * PT_GNU_PROPERTY segments (or .note.gnu.property sections -- same bytes) hold
 * NT_GNU_PROPERTY_TYPE_0 notes; one property is the AArch64 feature bitmask
 * (BTI, PAC, GCS) that makes the loader map executable pages PROT_BTI.
 *
 * Runs on every launch, in postAppSpecialize, on a possibly hostile ELF: every
 * read is bounded by the segment size the caller passes. C-compatible on
 * purpose, like android_ver.h; exercised by tools/gnu-props-selftest.cpp.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UNFUSE_PT_GNU_PROPERTY            0x6474e553u
#define UNFUSE_NT_GNU_PROPERTY_TYPE_0     5u

/* AAELF64: GNU_PROPERTY_AARCH64_FEATURE_1_AND, read off a real note -- clang
 * emits it as the first word of the description:
 *   00000000 04000000 10000000 05000000 474e5500
 *   00000010 000000c0 04000000 07000000 00000000
 * i.e. namesz=4, descsz=16, type=5, "GNU", then pr_type=0xc0000000,
 * pr_datasz=4, data=7 (BTI|PAC|GCS), padding. */
#define UNFUSE_PROP_AARCH64_FEATURE_1_AND 0xc0000000u

#define UNFUSE_FEAT_BTI (1u << 0)
#define UNFUSE_FEAT_PAC (1u << 1)
#define UNFUSE_FEAT_GCS (1u << 2)

typedef struct {
    uint32_t features;  /* GNU_PROPERTY_AARCH64_FEATURE_1_AND; 0 when absent */
    int      has_note;  /* a well-formed GNU property note was seen */
    int      matched;   /* set by the caller, which found the image */
} UnfuseImageProps;

/* Reads nothing past base + size, whatever the sizes inside claim. */
static inline void unfuse_props_parse(const void *base, size_t size,
                                      UnfuseImageProps *out) {
    if (out == NULL) return;
    if (base == NULL || size == 0) return;

    const unsigned char *p = (const unsigned char *)base;
    size_t left = size;

    /* Elf64_Nhdr fields are 4 bytes each: n_namesz, n_descsz, n_type. */
    const size_t hdr = 12;

    while (left >= hdr) {
        uint32_t namesz = 0, descsz = 0, type = 0;
        memcpy(&namesz, p + 0, 4);
        memcpy(&descsz, p + 4, 4);
        memcpy(&type,   p + 8, 4);

        const size_t name_p = ((size_t)namesz + 3u) & ~(size_t)3;
        const size_t desc_p = ((size_t)descsz + 3u) & ~(size_t)3;

        size_t total = hdr + name_p + desc_p;
        if (total > left) {
            /* A final note may be unpadded; name and description must fit. */
            total = hdr + namesz + descsz;
            if (total > left) break;
        }

        const unsigned char *name = p + hdr;
        const unsigned char *desc = name + name_p;

        if (type == UNFUSE_NT_GNU_PROPERTY_TYPE_0 && namesz == 4 &&
            memcmp(name, "GNU", 4) == 0) {
            out->has_note = 1;

            /* Elf64_Prop: pr_type, pr_datasz, then pr_datasz bytes padded to 8;
             * bounded by the UNPADDED description size, as the ABI says. */
            size_t dleft = descsz;
            const unsigned char *d = desc;
            while (dleft >= 8) {
                uint32_t ptype = 0, dsz = 0;
                memcpy(&ptype, d + 0, 4);
                memcpy(&dsz,   d + 4, 4);

                if ((size_t)dsz > dleft - 8) break;

                const size_t step = (8u + (size_t)dsz + 7u) & ~(size_t)7;
                if (step > dleft) break;   /* trailing padding, nothing after it */

                if (ptype == UNFUSE_PROP_AARCH64_FEATURE_1_AND && dsz >= 4) {
                    memcpy(&out->features, d + 8, 4);
                }

                d += step;
                dleft -= step;
            }
        }

        p += total;
        left -= total;
    }
}
