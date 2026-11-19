/* PCAPNG + radiotap block writers for the 802.11 sniffer.
 *
 * Dependency-free (like spectrum_math.h) so the host harness compiles the
 * exact bytes the device emits and a real pcap reader can be pointed at them.
 *
 * Link type is RADIOTAP (127), not raw 802.11 (105): Wireshark then shows the
 * channel and per-frame RSSI, which is half the point of capturing frames with
 * a device that also measures the spectrum.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PG_LINKTYPE_RADIOTAP 127u
#define PG_SNAPLEN 1600u
#define PG_RADIOTAP_LEN 13u

/* Fixed radiotap: present = CHANNEL | DBM_ANTSIGNAL. */
static inline size_t pg_radiotap(uint8_t *out, size_t cap, uint16_t freq_mhz,
                                 int8_t dbm) {
    if (!out || cap < PG_RADIOTAP_LEN) return 0;
    out[0] = 0;              /* version */
    out[1] = 0;              /* pad */
    out[2] = PG_RADIOTAP_LEN;/* header length */
    out[3] = 0;              /* present flags, first word only */
    uint32_t present = (1u << 3) | (1u << 5); /* CHANNEL | DBM_ANTSIGNAL */
    memcpy(out + 4, &present, 4);
    out[8] = (uint8_t)(freq_mhz & 0xff);
    out[9] = (uint8_t)(freq_mhz >> 8);
    out[10] = 0;             /* channel flags */
    out[11] = 0;
    out[12] = (uint8_t)dbm;
    return PG_RADIOTAP_LEN;
}

static inline void pg_put_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
    p[2] = (uint8_t)((v >> 16) & 0xff);
    p[3] = (uint8_t)((v >> 24) & 0xff);
}

static inline void pg_put_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v & 0xff);
    p[1] = (uint8_t)((v >> 8) & 0xff);
}

static inline uint32_t pg_pad4(uint32_t v) { return (v + 3u) & ~3u; }

/* Section Header Block: 28 bytes, little endian, unspecified section length. */
static inline size_t pg_shb(uint8_t *out, size_t cap) {
    if (!out || cap < 28u) return 0;
    pg_put_u32(out + 0, 0x0A0D0D0Au);
    pg_put_u32(out + 4, 28u);
    pg_put_u32(out + 8, 0x1A2B3C4Du);
    pg_put_u16(out + 12, 1u);   /* major */
    pg_put_u16(out + 14, 0u);   /* minor */
    memset(out + 16, 0xff, 8);  /* section length -1 */
    pg_put_u32(out + 24, 28u);
    return 28u;
}

/* Interface Description Block with if_name and nanosecond timestamps. */
static inline size_t pg_idb(char *out, size_t cap, const char *name,
                            uint32_t snaplen) {
    if (!out || !cap) return 0;
    uint8_t *p = (uint8_t *)out;
    size_t name_len = name ? strlen(name) : 0u;
    if (name_len > 32u) name_len = 32u;
    size_t name_opt = 4u + pg_pad4((uint32_t)name_len);
    size_t len = 20u + name_opt + 8u + 4u; /* header + name + tsresol + end */
    if (cap < len) return 0;
    pg_put_u32(p + 0, 1u);
    pg_put_u32(p + 4, (uint32_t)len);
    pg_put_u16(p + 8, PG_LINKTYPE_RADIOTAP);
    pg_put_u16(p + 10, 0u);
    pg_put_u32(p + 12, snaplen);
    size_t at = 16u;
    pg_put_u16(p + at, 2u);                     /* if_name */
    pg_put_u16(p + at + 2, (uint16_t)name_len);
    if (name_len) memcpy(p + at + 4, name, name_len);
    memset(p + at + 4 + name_len, 0, pg_pad4((uint32_t)name_len) - name_len);
    at += name_opt;
    pg_put_u16(p + at, 9u);                     /* if_tsresol */
    pg_put_u16(p + at + 2, 1u);
    p[at + 4] = 9u;                             /* 10^-9 = nanoseconds */
    p[at + 5] = 0; p[at + 6] = 0; p[at + 7] = 0;
    at += 8u;
    pg_put_u16(p + at, 0u);                     /* opt_endofopt */
    pg_put_u16(p + at + 2, 0u);
    pg_put_u32(p + 4, (uint32_t)len);
    pg_put_u32(p + (size_t)len - 4u, (uint32_t)len);
    return len;
}

/* Enhanced Packet Block. ts_ns is nanoseconds since the Unix epoch (or since
 * boot when the clock is unsynced; the manifest says which). */
static inline size_t pg_epb(uint8_t *out, size_t cap, uint64_t ts_ns,
                            const uint8_t *frame, uint32_t len) {
    if (!out || !cap || !frame) return 0;
    uint32_t padded = pg_pad4(len);
    size_t total = 32u + (size_t)padded;
    if (cap < total) return 0;
    pg_put_u32(out + 0, 6u);
    pg_put_u32(out + 4, (uint32_t)total);
    pg_put_u32(out + 8, 0u);                          /* interface id */
    pg_put_u32(out + 12, (uint32_t)(ts_ns >> 32));    /* timestamp high */
    pg_put_u32(out + 16, (uint32_t)(ts_ns & 0xffffffffu));
    pg_put_u32(out + 20, len);                        /* captured length */
    pg_put_u32(out + 24, len);                        /* original length */
    memcpy(out + 28, frame, len);
    if (padded > len) memset(out + 28 + len, 0, padded - len);
    pg_put_u32(out + 28 + padded, (uint32_t)total);
    return total;
}

#ifdef __cplusplus
}
#endif
