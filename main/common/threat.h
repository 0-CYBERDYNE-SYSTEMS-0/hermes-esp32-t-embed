/* Tracker-hunt threat triage over spectrum frames. Header-only so the host
 * test harness can compile it without the SDK, like spectrum_math.h.
 *
 * Code units: a bin code relates to dB by code/2 = 20*log10|X| (see the host
 * decoder's code_to_dbfs). Differences are what matter: +40 codes ~= +20 dB
 * above the frame floor. Duty is measured in frames, periodicity in ms.
 *
 * Level rules (kept in lockstep with rfkit's host-side classifier):
 *   CONCERN: steady strong (duty >= 80% and peak >= floor + 40 codes)
 *            OR periodic burst rhythm (tracker beacon)
 *            OR new-vs-baseline and present (duty >= 10%) and >= 20 codes up
 *   WATCH:   duty in [30%, 80%) OR strong bursts (peak >= floor + 30 codes)
 *   AMBIENT: everything else (Wi-Fi chatter and known background)
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THREAT_TRACKED 8u
#define THREAT_ARRIVALS 8u
#define THREAT_LEVEL_AMBIENT 0u
#define THREAT_LEVEL_WATCH 1u
#define THREAT_LEVEL_CONCERN 2u
/* reason bits */
#define THREAT_REASON_STEADY 0x01u
#define THREAT_REASON_PERIODIC 0x02u
#define THREAT_REASON_NEW 0x04u
#define THREAT_REASON_STRONG 0x08u

typedef struct {
    uint32_t khz;                 /* tracked peak center */
    uint8_t used;
    uint8_t seen_n, win_n;        /* presence counters (halve at 255) */
    int16_t code_max;             /* strongest code seen */
    int16_t baseline_code;        /* snapshot at BASELINE SAVE (INT16_MIN = none) */
    uint32_t last_seen_ms;
    uint32_t arrive_ms[THREAT_ARRIVALS];
    uint8_t arrive_n, arrive_i;
    /* derived */
    uint8_t duty_pct;
    uint8_t periodic;
    uint16_t period_cs;
    uint8_t level;
    uint8_t reason;
} threat_track_t;

typedef struct {
    threat_track_t track[THREAT_TRACKED];
    int16_t floor_code;           /* EMA of per-frame floor */
    uint8_t baseline_valid;       /* set once BASELINE SAVE has run */
    uint8_t verdict;
    uint8_t concern_n, watch_n, periodic_n;
    uint32_t frames;
    uint32_t changed;             /* bumped whenever verdict or a level moves */
} threat_state_t;

static inline uint8_t threat_max(uint8_t a, uint8_t b) { return a > b ? a : b; }

static inline void threat_track_reset(threat_track_t *t) {
    memset(t, 0, sizeof(*t));
    t->baseline_code = INT16_MIN;
}

static inline void threat_init(threat_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->floor_code = 127;
    for (unsigned i = 0; i < THREAT_TRACKED; i++) threat_track_reset(&s->track[i]);
}

static inline void threat_track_arrival(threat_track_t *t, uint32_t now_ms) {
    t->arrive_ms[t->arrive_i] = now_ms;
    t->arrive_i = (uint8_t)((t->arrive_i + 1u) % THREAT_ARRIVALS);
    if (t->arrive_n < THREAT_ARRIVALS) t->arrive_n++;
}

/* Regular-rhythm detector over the last arrivals: a tracker beacon announces
 * itself on a metronome (AirTag/Tile/asset tags: ~0.2-60 s). Returns true and
 * fills period_cs when the inter-arrival spread is tight (MAD/mean < 0.25). */
static inline bool threat_periodicity(const threat_track_t *t, uint16_t *period_cs) {
    if (t->arrive_n < THREAT_ARRIVALS) return false;
    /* oldest entry in the circular arrival ring */
    unsigned oldest = (t->arrive_i + THREAT_ARRIVALS - t->arrive_n) % THREAT_ARRIVALS;
    uint32_t sum = 0;
    unsigned deltas = 0;
    uint32_t d[THREAT_ARRIVALS];
    for (unsigned i = 0; i + 1u < t->arrive_n; i++) {
        uint32_t a = t->arrive_ms[(oldest + i) % THREAT_ARRIVALS];
        uint32_t b = t->arrive_ms[(oldest + i + 1u) % THREAT_ARRIVALS];
        if (b <= a) return false;          /* wrapped or out of order */
        d[deltas++] = b - a;
        sum += b - a;
    }
    if (deltas < THREAT_ARRIVALS - 1u) return false;
    uint32_t mean = sum / deltas;
    if (mean < 200u || mean > 60000u) return false;
    uint32_t dev = 0;
    for (unsigned i = 0; i < deltas; i++) {
        uint32_t x = d[i] > mean ? d[i] - mean : mean - d[i];
        dev += x;
    }
    if (dev / deltas > mean / 4u) return false;
    if (period_cs) *period_cs = (uint16_t)((mean + 5u) / 10u);
    return true;
}

static inline uint8_t threat_level_of(threat_track_t *t, int16_t floor_code,
                                      uint32_t frames, bool baseline_valid) {
    t->reason = 0;
    uint8_t level = THREAT_LEVEL_AMBIENT;
    int16_t over = (int16_t)(t->code_max - floor_code);
    bool steady = t->duty_pct >= 80u;
    bool present = frames >= 32u && t->duty_pct >= 10u;
    t->periodic = threat_periodicity(t, &t->period_cs) ? 1u : 0u;
    if (steady && over >= 40) {
        level = THREAT_LEVEL_CONCERN;
        t->reason |= THREAT_REASON_STEADY;
    }
    if (t->periodic) {
        level = threat_max(level, THREAT_LEVEL_CONCERN);
        t->reason |= THREAT_REASON_PERIODIC;
    }
    /* vs baseline: a slot that was empty when the baseline was taken is NEW;
     * a slot that grew >= 20 codes (~10 dB) over its snapshot is NEW too. */
    if (baseline_valid && present) {
        bool brand_new = t->baseline_code == INT16_MIN;
        bool grew = !brand_new && (int16_t)(t->code_max - t->baseline_code) >= 20;
        if (brand_new || grew) {
            level = threat_max(level, THREAT_LEVEL_CONCERN);
            t->reason |= THREAT_REASON_NEW;
        }
    }
    if (level == THREAT_LEVEL_AMBIENT &&
        (t->duty_pct >= 30u || over >= 30)) {
        level = THREAT_LEVEL_WATCH;
        t->reason |= (over >= 30) ? THREAT_REASON_STRONG : 0u;
    }
    return level;
}

/* Feed one spectrum frame. bins are FFT-order codes; bin k maps to
 * center_khz + (k - nfft/2) * span_khz / nfft. */
static inline void threat_observe(threat_state_t *s, const uint8_t *bins,
                                  unsigned nfft, unsigned center_khz,
                                  unsigned span_khz, uint32_t now_ms) {
    if (!s || !bins || nfft < 2u) return;
    uint32_t sum = 0;
    for (unsigned i = 0; i < nfft; i++) sum += bins[i];
    int16_t floor_now = (int16_t)(sum / nfft);
    s->floor_code = (int16_t)((s->floor_code * 7 + floor_now) / 8);
    s->frames++;
    for (unsigned i = 0; i < THREAT_TRACKED; i++) {
        threat_track_t *t = &s->track[i];
        if (!t->used) continue;
        if (t->win_n == 255u) { t->win_n >>= 1; t->seen_n >>= 1; } /* decay */
        if (t->win_n < 255u) t->win_n++;
        if (now_ms - t->last_seen_ms > 30000u) {
            int16_t keep = t->baseline_code; /* the baseline outlives idle gaps */
            threat_track_reset(t);
            t->baseline_code = keep;
        }
    }
    /* peak-pick local maxima above the detection delta */
    int16_t gate = (int16_t)(floor_now + 20);
    for (unsigned k = 1; k + 1u < nfft; k++) {
        int16_t v = bins[k];
        if (v < gate || v <= bins[k - 1u] || v <= bins[k + 1u]) continue;
        unsigned khz = center_khz + (unsigned)(((int)k - (int)(nfft / 2u)) *
                                               (int)span_khz / (int)nfft);
        /* match an existing track (or a restored baseline-only slot) in range */
        int slot = -1, free_slot = -1;
        for (unsigned i = 0; i < THREAT_TRACKED; i++) {
            threat_track_t *t = &s->track[i];
            if (!t->used && t->baseline_code == INT16_MIN) {
                if (free_slot < 0) free_slot = (int)i;
                continue;
            }
            int32_t d = (int32_t)t->khz - (int32_t)khz;
            if (d < 0) d = -d;
            if (d <= 2000) { slot = (int)i; break; }
            if (!t->used && free_slot < 0) free_slot = (int)i;
        }
        if (slot < 0) slot = free_slot;
        if (slot < 0) continue;
        threat_track_t *t = &s->track[slot];
        if (!t->used) {
            int16_t keep = t->baseline_code; /* restored baseline survives reuse */
            threat_track_reset(t);
            t->baseline_code = keep;
            t->used = 1;
            t->khz = khz;
            t->win_n = 1;
        }
        if (t->seen_n < 255u) t->seen_n++;
        if (v > t->code_max) t->code_max = v;
        if (t->last_seen_ms != now_ms) {
            threat_track_arrival(t, now_ms);
            t->last_seen_ms = now_ms;
        }
    }
    /* derive per-track verdicts */
    uint8_t verdict = THREAT_LEVEL_AMBIENT, concern = 0, watch = 0, periodic = 0;
    for (unsigned i = 0; i < THREAT_TRACKED; i++) {
        threat_track_t *t = &s->track[i];
        if (!t->used) continue;
        if (t->win_n) t->duty_pct = (uint8_t)((unsigned)t->seen_n * 100u / t->win_n);
        uint8_t level = threat_level_of(t, s->floor_code, s->frames,
                                        s->baseline_valid != 0);
        if (level != t->level) s->changed++;
        t->level = level;
        if (t->periodic) periodic++;
        if (level == THREAT_LEVEL_CONCERN) concern++;
        else if (level == THREAT_LEVEL_WATCH) watch++;
        verdict = threat_max(verdict, level);
    }
    if (verdict != s->verdict) s->changed++;
    s->verdict = verdict;
    s->concern_n = concern;
    s->watch_n = watch;
    s->periodic_n = periodic;
}

static inline void threat_baseline_save(threat_state_t *s) {
    for (unsigned i = 0; i < THREAT_TRACKED; i++) {
        threat_track_t *t = &s->track[i];
        t->baseline_code = t->used ? t->code_max : INT16_MIN;
    }
    s->baseline_valid = 1;
}

static inline void threat_baseline_clear(threat_state_t *s) {
    for (unsigned i = 0; i < THREAT_TRACKED; i++)
        s->track[i].baseline_code = INT16_MIN;
    s->baseline_valid = 0;
}

#ifdef __cplusplus
}
#endif
