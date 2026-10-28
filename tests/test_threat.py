"""Rule checks for the tracker-hunt threat triage (main/common/threat.h)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "threat.h"

static threat_state_t s;
static uint8_t bins[256];

static void frame(int peak_bin, int peak_code) {
    memset(bins, 120, sizeof(bins));
    if (peak_bin >= 0) {
        bins[peak_bin] = (uint8_t)peak_code;
        bins[peak_bin - 1] = (uint8_t)(peak_code - 8);
        bins[peak_bin + 1] = (uint8_t)(peak_code - 8);
    }
}

static void step(int peak_bin, int peak_code, unsigned ms) {
    frame(peak_bin, peak_code);
    threat_observe(&s, bins, 256, 2437000u, 16000u, ms);
}

static threat_track_t *find_track(unsigned khz_lo, unsigned khz_hi) {
    for (unsigned i = 0; i < THREAT_TRACKED; i++)
        if (s.track[i].used && s.track[i].khz >= khz_lo && s.track[i].khz <= khz_hi)
            return &s.track[i];
    return NULL;
}

int main(void) {
    /* 1: steady strong carrier -> CONCERN with STEADY */
    threat_init(&s);
    for (unsigned i = 0; i < 40u; i++) step(200, 200, i * 100u);
    assert(s.verdict == THREAT_LEVEL_CONCERN);
    assert(s.track[0].reason & THREAT_REASON_STEADY);
    assert(s.track[0].duty_pct >= 95u);
    printf("steady ok\n");

    /* 2: regular beacon rhythm -> CONCERN with PERIODIC */
    threat_init(&s);
    {   unsigned burst_ms[8] = {1020, 2040, 3060, 4080, 5100, 6120, 7140, 8160};
        unsigned t = 0;
        for (unsigned i = 0; i < 8u; i++) {
            while (t < burst_ms[i]) { step(-1, 0, t); t += 100u; }
            step(128, 175, burst_ms[i]);
            t = burst_ms[i] + 100u;
        }
        for (unsigned i = 0; i < 5u; i++) { step(-1, 0, t); t += 100u; }
    }
    assert(s.verdict == THREAT_LEVEL_CONCERN);
    assert(s.track[0].reason & THREAT_REASON_PERIODIC);
    assert(s.track[0].periodic == 1u);
    assert(s.track[0].period_cs > 95u && s.track[0].period_cs < 110u);
    printf("periodic ok\n");

    /* 3: irregular weak chatter -> AMBIENT */
    threat_init(&s);
    {   unsigned times[8] = {100, 250, 900, 1100, 2500, 2700, 5100, 5300};
        unsigned t = 0;
        for (unsigned i = 0; i < 8u; i++) {
            while (t < times[i]) { step(-1, 0, t); t += 100u; }
            step(128, 145, times[i]); t = times[i] + 100u;
        }
        for (unsigned i = 0; i < 20u; i++) { step(-1, 0, t); t += 100u; }
    }
    assert(s.verdict == THREAT_LEVEL_AMBIENT);
    assert(!(s.track[0].reason & THREAT_REASON_PERIODIC));
    printf("ambient ok\n");

    /* 4: new-vs-baseline -> CONCERN with NEW */
    threat_init(&s);
    for (unsigned i = 0; i < 40u; i++) step(100, 140, i * 100u);   /* known weak */
    threat_baseline_save(&s);
    assert(s.baseline_valid == 1u);
    {   unsigned t = 4000u;
        for (unsigned i = 0; i < 40u; i++) {                       /* known + new */
            step(100, 140, t);
            if (i % 3u == 0u) step(180, 165, t + 50u);             /* new, ~33% duty */
            t += 150u;
        }
    }
    assert(s.verdict == THREAT_LEVEL_CONCERN);
    {        threat_track_t *fresh = find_track(2439500u, 2441500u);
        assert(fresh && (fresh->reason & THREAT_REASON_NEW));
        assert(fresh->level == THREAT_LEVEL_CONCERN);
    }
    printf("baseline-new ok\n");

    /* 5: grew-vs-baseline (>= 20 codes over snapshot) -> CONCERN with NEW */
    threat_init(&s);
    for (unsigned i = 0; i < 40u; i++) step(100, 140, i * 100u);
    threat_baseline_save(&s);
    for (unsigned i = 0; i < 20u; i++) step(100, 175, 5000u + i * 100u);
    assert(s.track[0].reason & THREAT_REASON_NEW);
    assert(s.track[0].level == THREAT_LEVEL_CONCERN);
    printf("grew ok\n");

    /* 6: no baseline -> NEW semantics stay off */
    threat_init(&s);
    for (unsigned i = 0; i < 40u; i++) step(100, 165, i * 100u);
    assert(s.baseline_valid == 0u);
    assert(!(s.track[0].reason & THREAT_REASON_NEW));
    printf("no-baseline ok\n");

    printf("ALL PASS\n");
    return 0;
}
'''


class ThreatRules(unittest.TestCase):
    @unittest.skipUnless(shutil.which('cc'), 'Host C compiler unavailable')
    def test_threat_rules(self):
        with tempfile.TemporaryDirectory() as td:
            src = Path(td) / 'harness.c'
            binp = Path(td) / 'harness'
            src.write_text(HARNESS)
            build = subprocess.run(
                ['cc', '-std=c11', '-Wall', '-Werror',
                 '-I', str(ROOT / 'main' / 'common'),
                 str(src), '-o', str(binp)],
                capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(binp)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
            self.assertIn('ALL PASS', run.stdout)


if __name__ == '__main__':
    unittest.main()
