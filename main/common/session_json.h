/* Session-log serialization: manifest, NDJSON frames/events, SigMF metadata.
 *
 * Dependency-free on purpose (no ESP-IDF, no libc time zone), so the host test
 * harness compiles and exercises exactly what the firmware writes, like
 * spectrum_math.h and threat.h.
 *
 * Wire compatibility: sj_frame_ndjson() emits the same per-frame keys the host
 * toolkit already writes in frames.jsonl (host/rfkit/rfkit.py stream_spec), so
 * `rfkit analyze <session dir>` runs unchanged on files pulled off the card.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Civil date from a Unix day number (Howard Hinnant's algorithm): no locale,
 * no time zone database, correct for negative day numbers. */
static inline void sj_civil_from_days(int32_t days, int *year, unsigned *month,
                                      unsigned *day) {
    int32_t z = days + 719468;
    int32_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460u + doe / 36524u - doe / 146096u) / 365u;
    int32_t y = (int32_t)yoe + era * 400;
    unsigned doy = doe - (365u * yoe + yoe / 4u - yoe / 100u);
    unsigned mp = (5u * doy + 2u) / 153u;
    *day = doy - (153u * mp + 2u) / 5u + 1u;
    *month = mp < 10u ? mp + 3u : mp - 9u;
    *year = (int)(y + (*month <= 2u));
}

/* ISO 8601 UTC with milliseconds: 2026-11-18T23:04:05.123Z */
static inline size_t sj_iso8601(char *out, size_t cap, int64_t unix_ms) {
    if (!out || !cap) return 0;
    if (unix_ms <= 0) {
        if (cap >= 1) out[0] = '\0';
        return 0;
    }
    int64_t seconds = unix_ms / 1000;
    int64_t days = seconds / 86400;
    if (seconds < 0 && seconds % 86400) days--;      /* floor division */
    int64_t sod = seconds - days * 86400;
    int year;
    unsigned month, day;
    sj_civil_from_days((int32_t)days, &year, &month, &day);
    int n = snprintf(out, cap, "%04d-%02u-%02uT%02u:%02u:%02u.%03uZ", year, month,
                     day, (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60),
                     (unsigned)(sod % 60), (unsigned)(unix_ms % 1000));
    return n > 0 ? (size_t)n : 0;
}

/* Session directory name: 20261118-230405, or unsynced-<n> before a clock. */
static inline size_t sj_stamp(char *out, size_t cap, int64_t unix_ms,
                              unsigned unsynced_index) {
    if (!out || !cap) return 0;
    if (unix_ms <= 0) {
        int n = snprintf(out, cap, "unsynced-%03u", unsynced_index);
        return n > 0 ? (size_t)n : 0;
    }
    char iso[32];
    if (!sj_iso8601(iso, sizeof(iso), unix_ms)) { out[0] = '\0'; return 0; }
    /* Built from the same civil-time split rather than slicing the ISO text:
     * separators make character offsets fragile. */
    int64_t seconds = unix_ms / 1000;
    int64_t days = seconds / 86400;
    if (seconds < 0 && seconds % 86400) days--;
    int64_t sod = seconds - days * 86400;
    int year;
    unsigned month, day;
    sj_civil_from_days((int32_t)days, &year, &month, &day);
    int n = snprintf(out, cap, "%04d%02u%02u-%02u%02u%02u", year, month, day,
                     (unsigned)(sod / 3600), (unsigned)(sod / 60 % 60),
                     (unsigned)(sod % 60));
    return n > 0 ? (size_t)n : 0;
}

typedef struct {
    const char *build;      /* firmware git SHA / build tag */
    const char *session;    /* session stamp */
    const char *label;      /* free-form, may be NULL */
    const char *time_source;/* "host" | "unsynced" */
    const char *board;      /* "lilygo-t-embed-s3r8" */
    int64_t started_unix_ms;
    unsigned lo_mhz, rate_code, rate_hz, nfft;
    unsigned lpf_code;      /* analog filter code, 0 when auto */
    int gain_index;         /* -1 hardware AGC */
    unsigned battery_mv;    /* 0 when unknown */
} sj_manifest_t;

/* Returns bytes written, 0 if the buffer was too small. */
static inline size_t sj_manifest_json(char *out, size_t cap, const sj_manifest_t *m) {
    if (!out || !cap || !m) return 0;
    char iso[32] = "";
    if (m->started_unix_ms > 0) sj_iso8601(iso, sizeof(iso), m->started_unix_ms);
    int n = snprintf(out, cap,
        "{\"schema\":\"tembed-sigint/1\",\"board\":\"%s\",\"build\":\"%s\","
        "\"session\":\"%s\",\"label\":\"%s\",\"started_unix_ms\":%lld,"
        "\"started_iso\":\"%s\",\"time_source\":\"%s\",\"lo_mhz\":%u,"
        "\"rate_code\":%u,\"rate_hz\":%u,\"nfft\":%u,\"lpf_code\":%u,"
        "\"gain_index\":%d,\"battery_mv\":%u}",
        m->board ? m->board : "unknown", m->build ? m->build : "unknown",
        m->session ? m->session : "unknown", m->label ? m->label : "",
        (long long)m->started_unix_ms, iso,
        m->time_source ? m->time_source : "unsynced", m->lo_mhz, m->rate_code,
        m->rate_hz, m->nfft, m->lpf_code, m->gain_index, m->battery_mv);
    return n > 0 ? (size_t)n : 0;
}

typedef struct {
    uint32_t seq;
    int64_t t_ms;        /* milliseconds since session start */
    const char *iso;     /* wall clock at capture, NULL when unsynced */
    unsigned lo_mhz, nfft, rate_hz;
    int gain_index;
    unsigned floor_code;
} sj_frame_t;

/* 20th percentile of the code histogram: the host's robust floor estimator. */
static inline unsigned sj_floor_code(const uint8_t *codes, unsigned n) {
    unsigned hist[256];
    memset(hist, 0, sizeof(hist));
    if (!codes || !n) return 0;
    for (unsigned i = 0; i < n; i++) hist[codes[i]]++;
    unsigned target = (unsigned)(0.2 * n);
    if (target) target--;                    /* matches max(0, int(0.2n)-1) */
    unsigned seen = 0;
    for (unsigned code = 0; code < 256; code++) {
        seen += hist[code];
        if (seen > target) return code;
    }
    return 255;
}

/* One NDJSON frame line, newline terminated. Returns 0 when it does not fit:
 * a truncated line is invalid JSON, so the caller must drop the whole frame
 * and count it rather than emit a partial record. */
static inline size_t sj_frame_ndjson(char *out, size_t cap, const sj_frame_t *f,
                                     const uint8_t *codes) {
    if (!out || !cap || !f || !codes || !f->nfft) return 0;
    int n = snprintf(out, cap,
        "{\"seq\":%lu,\"t_ms\":%lld,\"t_s\":%u.%03u,\"ts\":\"%s\",\"lo_mhz\":%u,"
        "\"nfft\":%u,\"rate_hz\":%u,\"gain\":%d,\"floor_code\":%u,\"codes\":[",
        (unsigned long)f->seq, (long long)f->t_ms,
        (unsigned)(f->t_ms / 1000), (unsigned)(f->t_ms % 1000),
        f->iso ? f->iso : "", f->lo_mhz, f->nfft, f->rate_hz, f->gain_index,
        f->floor_code);
    if (n < 0 || (size_t)n >= cap) return 0;
    size_t used = (size_t)n;
    for (unsigned i = 0; i < f->nfft; i++) {
        int k = snprintf(out + used, cap - used, i ? ",%u" : "%u", codes[i]);
        if (k < 0 || (size_t)k >= cap - used) return 0;
        used += (size_t)k;
    }
    int k = snprintf(out + used, cap - used, "]}\n");
    if (k < 0 || (size_t)k >= cap - used) return 0;
    return used + (size_t)k;
}

static inline size_t sj_event_ndjson(char *out, size_t cap, int64_t t_ms,
                                     const char *iso, unsigned khz,
                                     const char *level, unsigned reason,
                                     int over_floor) {
    if (!out || !cap) return 0;
    int n = snprintf(out, cap,
        "{\"t_ms\":%lld,\"ts\":\"%s\",\"khz\":%u,\"level\":\"%s\",\"reason\":%u,"
        "\"over_floor\":%d}\n",
        (long long)t_ms, iso ? iso : "", khz, level ? level : "", reason, over_floor);
    return n > 0 && (size_t)n < cap ? (size_t)n : 0;
}

typedef struct {
    const char *datatype;     /* "ci8" for IQ8, "ci16_le" for unpacked IQ10 */
    const char *hw;
    const char *description;
    const char *recording_id; /* stable per file, e.g. "0007-ble-burst" */
    const char *iso;          /* capture start, NULL when unsynced */
    double center_hz;
    double sample_rate_hz;
    uint64_t num_samples;
    unsigned rate_code;
    int gain_index;
} sj_sigmf_t;

/* SigMF v1.2.6 metadata: core:datatype, core:sample_rate, core:version,
 * core:frequency + core:sample_start in captures[]. */
static inline size_t sj_sigmf_meta(char *out, size_t cap, const sj_sigmf_t *s) {
    if (!out || !cap || !s) return 0;
    int n = snprintf(out, cap,
        "{\"global\":{\"core:datatype\":\"%s\",\"core:sample_rate\":%.0f,"
        "\"core:version\":\"1.2.6\",\"core:recorder\":\"esp_sdr\","
        "\"core:hw\":\"%s\",\"core:description\":\"%s\","
        "\"esp_sdr:rate_code\":%u,\"esp_sdr:gain_index\":%d,"
        "\"esp_sdr:packing\":\"%s\",\"esp_sdr:calibration\":\"uncalibrated\"},"
        "\"captures\":[{\"core:sample_start\":0,\"core:frequency\":%.0f,"
        "\"esp_sdr:started_iso\":\"%s\"}],\"annotations\":[]}",
        s->datatype ? s->datatype : "ci8", s->sample_rate_hz,
        s->hw ? s->hw : "lilygo-t-embed-s3r8",
        s->description ? s->description : "",
        s->rate_code, s->gain_index,
        s->datatype && !strcmp(s->datatype, "ci8") ? "iq8" : "none",
        s->center_hz, s->iso ? s->iso : "");
    return n > 0 ? (size_t)n : 0;
}

#ifdef __cplusplus
}
#endif
