/*
 * gnu_props.h — read the AArch64 feature word out of a GNU property note.
 *
 * A PT_GNU_PROPERTY segment (or a .note.gnu.property section — same bytes) holds
 * NT_GNU_PROPERTY_TYPE_0 notes, and one of the properties inside is
 * GNU_PROPERTY_AARCH64_FEATURE_1_AND: a bitmask saying whether the image was
 * built for BTI, for PAC and for GCS. The loader reads it to decide whether to
 * map the image's executable pages with PROT_BTI, which is what makes bti c /
 * paciasp at a function entry enforced rather than inert.
 *
 * Nothing here is Android- or AArch64-specific beyond the property number, so the
 * host self-test (tools/gnu-props-selftest.cpp) exercises exactly this code on
 * synthetic notes and on a real note dumped from an object file. The parser is
 * what stands between a hostile ELF and the app process: it runs on every launch,
 * in postAppSpecialize, so every read is bounded by the segment size the caller
 * passes and no field is trusted before it has been checked against the bytes
 * actually available.
 *
 * C-compatible on purpose, like android_ver.h: one definition, includable from the
 * C++ module and from a plain C tool.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define UNFUSE_PT_GNU_PROPERTY            0x6474e553u
#define UNFUSE_NT_GNU_PROPERTY_TYPE_0     5u

/* AAELF64: GNU_PROPERTY_AARCH64_FEATURE_1_AND. Read off a real note rather than
 * recalled — clang emits it as the first word of the description, e.g.
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
    int      matched;   /* the caller found the image; set by the caller */
} UnfuseImageProps;

/* Walks one note segment of `size` bytes at `base`. Reads nothing past
 * base + size, whatever the sizes inside claim. */
static inline void unfuse_props_parse(const void *base, size_t size,
                                      UnfuseImageProps *out) {
    if (out == NULL) return;
    if (base == NULL || size == 0) return;

    const unsigned char *p = (const unsigned char *)base;
    size_t left = size;

    /* Elf64_Nhdr: n_namesz, n_descsz, n_type — 4 bytes each. */
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
            /* A final note is allowed to be unpadded; the name and the
             * description still have to fit. */
            total = hdr + namesz + descsz;
            if (total > left) break;
        }

        const unsigned char *name = p + hdr;
        const unsigned char *desc = name + name_p;

        if (type == UNFUSE_NT_GNU_PROPERTY_TYPE_0 && namesz == 4 &&
            memcmp(name, "GNU", 4) == 0) {
            out->has_note = 1;

            /* Elf64_Prop: pr_type, pr_datasz, then pr_datasz bytes padded to 8.
             * Bounded by the UNPADDED description size, as the ABI says. */
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
