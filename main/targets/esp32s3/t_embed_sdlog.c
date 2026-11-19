#include "t_embed_sdlog.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "driver/spi_common.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#include "ring_capture.h"
#include "session_json.h"

static const char *kTag = "sdlog";

#define SD_MOUNT_POINT "/sdcard"
#define SD_ROOT SD_MOUNT_POINT "/sigint"
#define SD_PIN_CS 39
#define SD_PIN_SCK 40
#define SD_PIN_MOSI 41
#define SD_PIN_MISO 38
/* A 2048-bin frame line is ~9 KB; anything shorter truncates a valid frame. */
#define SD_LINE_CAP 12288u
#define SD_RETRY_MS 5000

static sdmmc_card_t *s_card;
static bool s_mounted;
static int64_t s_last_mount_attempt_us;
static char s_last_err[48] = "never attempted";

static bool s_clock_synced;
static int64_t s_clock_offset_ms; /* wall - uptime */

static bool s_logging;
static char s_session_dir[80];
static char s_session_stamp[32];
static const char *s_build =
#ifdef HUNT_BUILD_TAG
    HUNT_BUILD_TAG;
#else
    "local";
#endif
static FILE *s_frames;
static FILE *s_events;
static char *s_line;
static uint32_t s_frame_seq;
static uint32_t s_frames_written;
static uint32_t s_frames_dropped;
static uint32_t s_events_written;
static uint32_t s_iq_files;
static int64_t s_session_start_ms;
static int64_t s_last_frame_ms;
static unsigned s_min_interval_ms = 200;

static int64_t uptime_ms(void) { return esp_timer_get_time() / 1000; }

int64_t t_embed_sdlog_now_ms(void) {
    if (!s_clock_synced) return 0;
    return s_clock_offset_ms + uptime_ms();
}

bool t_embed_sdlog_set_clock(int64_t unix_ms) {
    if (unix_ms <= 0) return false;
    s_clock_offset_ms = unix_ms - uptime_ms();
    s_clock_synced = true;
    return true;
}

bool t_embed_sdlog_ready(void) { return s_mounted; }

const char *t_embed_sdlog_session(void) { return s_logging ? s_session_stamp : ""; }

const char *t_embed_sdlog_dir(void) { return s_logging ? s_session_dir : ""; }

void t_embed_sdlog_set_interval_ms(unsigned ms) {
    if (ms >= 20u && ms <= 60000u) s_min_interval_ms = ms;
}

static bool ensure_line_buffer(void) {
    if (s_line) return true;
    /* PSRAM on purpose: the SRAM guard leaves no margin for a 12 KB static. */
    s_line = (char *)heap_caps_malloc(SD_LINE_CAP, MALLOC_CAP_SPIRAM);
    if (!s_line) s_line = (char *)heap_caps_malloc(SD_LINE_CAP, MALLOC_CAP_8BIT);
    return s_line != NULL;
}

bool t_embed_sdlog_mount(void) {
    if (s_mounted) return true;
    int64_t now = esp_timer_get_time();
    if (s_last_mount_attempt_us && now - s_last_mount_attempt_us < SD_RETRY_MS * 1000) {
        return false;
    }
    s_last_mount_attempt_us = now;

    /* The T-Embed TF slot is wired to the SDMMC controller in 1-bit mode
     * (LILYGO's own example: CLK 40, CMD 41, D0 38, DAT3 held high so the
     * card stays in SDMMC mode instead of falling back to SPI). */
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.flags &= ~(uint32_t)(SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_8BIT |
                              SDMMC_HOST_FLAG_DDR);
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.clk = (gpio_num_t)SD_PIN_SCK;
    slot.cmd = (gpio_num_t)SD_PIN_MOSI;
    slot.d0 = (gpio_num_t)SD_PIN_MISO;
    slot.width = 1;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    /* DAT3 idle-high: without it the card enters SPI mode and never answers
     * the SDMMC controller. */
    gpio_config_t dat3 = {
        .pin_bit_mask = 1ULL << SD_PIN_CS,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&dat3);
    gpio_set_level((gpio_num_t)SD_PIN_CS, 1);
    esp_vfs_fat_mount_config_t config = {
        .format_if_mount_failed = false,
        .max_files = 6,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot, &config, &s_card);
    if (err != ESP_OK) {
        snprintf(s_last_err, sizeof(s_last_err), "mount %s", esp_err_to_name(err));
        ESP_LOGW(kTag, "no card: %s", esp_err_to_name(err));
        return false;
    }
    s_last_err[0] = '\0';
    s_mounted = true;
    if (!ensure_line_buffer()) {
        ESP_LOGW(kTag, "no line buffer; logging disabled");
    }
    struct stat st;
    if (stat(SD_ROOT, &st) != 0) mkdir(SD_ROOT, 0755);
    return true;
}

void t_embed_sdlog_status_json(char *out, size_t cap) {
    if (!out || !cap) return;
    uint64_t total = 0, free_bytes = 0;
    if (s_mounted) esp_vfs_fat_info(SD_MOUNT_POINT, &total, &free_bytes);
    int64_t now_ms = t_embed_sdlog_now_ms();
    snprintf(out, cap,
             "{\"mounted\":%u,\"card_kb\":%llu,\"free_kb\":%llu,\"logging\":%u,"
             "\"session\":\"%s\",\"frames\":%lu,\"dropped\":%lu,\"events\":%lu,"
             "\"iq_files\":%lu,\"clock_ms\":%lld,\"clock\":\"%s\",\"interval_ms\":%u,"
             "\"last\":\"%s\"}\n",
             (unsigned)s_mounted, (unsigned long long)(total / 1024),
             (unsigned long long)(free_bytes / 1024), (unsigned)s_logging,
             s_logging ? s_session_stamp : "", (unsigned long)s_frames_written,
             (unsigned long)s_frames_dropped, (unsigned long)s_events_written,
             (unsigned long)s_iq_files, (long long)now_ms,
             s_clock_synced ? "host" : "unsynced", s_min_interval_ms,
             s_last_err);
}

static bool make_session_dir(void) {
    struct stat st;
    if (stat(SD_ROOT, &st) != 0 && mkdir(SD_ROOT, 0755) != 0 && errno != EEXIST) {
        return false;
    }
    int64_t now = t_embed_sdlog_now_ms();
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        if (now > 0) {
            sj_stamp(s_session_stamp, sizeof(s_session_stamp), now + (int64_t)attempt * 1000,
                     0);
        } else {
            snprintf(s_session_stamp, sizeof(s_session_stamp), "unsynced-%06u",
                     (unsigned)(uptime_ms() / 1000));
        }
        snprintf(s_session_dir, sizeof(s_session_dir), SD_ROOT "/%s", s_session_stamp);
        if (mkdir(s_session_dir, 0755) == 0) return true;
        if (errno != EEXIST) return false;
        if (now > 0) continue;         /* same second: try the next one */
        return false;                  /* unsynced name already taken */
    }
    return false;
}

static bool write_manifest(sj_manifest_t *m) {
    char path[128];
    char *buf = s_line;
    if (!buf) return false;
    snprintf(path, sizeof(path), "%s/session.json", s_session_dir);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = sj_manifest_json(buf, SD_LINE_CAP, m);
    bool ok = n && fwrite(buf, 1, n, f) == n;
    fclose(f);
    return ok;
}

bool t_embed_sdlog_start(const char *label, unsigned sample_rate_hz, unsigned nfft,
                         unsigned lo_mhz, unsigned rate_code, unsigned lpf_code,
                         int gain_index) {
    if (s_logging) t_embed_sdlog_stop();
    if (!t_embed_sdlog_mount() || !ensure_line_buffer()) return false;
    if (!make_session_dir()) return false;

    char path[128];
    snprintf(path, sizeof(path), "%s/frames.ndjson", s_session_dir);
    s_frames = fopen(path, "wb");
    snprintf(path, sizeof(path), "%s/events.ndjson", s_session_dir);
    s_events = fopen(path, "wb");
    if (!s_frames || !s_events) {
        if (s_frames) fclose(s_frames);
        if (s_events) fclose(s_events);
        s_frames = s_events = NULL;
        return false;
    }
    s_session_start_ms = uptime_ms();
    s_frame_seq = s_frames_written = s_frames_dropped = 0;
    s_events_written = s_iq_files = 0;
    s_last_frame_ms = 0;

    sj_manifest_t manifest = {
        .build = s_build,
        .session = s_session_stamp,
        .label = label,
        .time_source = s_clock_synced ? "host" : "unsynced",
        .board = "lilygo-t-embed-s3r8",
        .started_unix_ms = t_embed_sdlog_now_ms(),
        .lo_mhz = lo_mhz,
        .rate_code = rate_code,
        .rate_hz = sample_rate_hz,
        .nfft = nfft,
        .lpf_code = lpf_code,
        .gain_index = gain_index,
        .battery_mv = 0, /* ADC sense not wired yet */
    };
    (void)write_manifest(&manifest);
    s_logging = true;
    ESP_LOGI(kTag, "session %s", s_session_stamp);
    return true;
}

void t_embed_sdlog_stop(void) {
    if (!s_logging) return;
    if (s_frames) fflush(s_frames);
    char path[128];
    snprintf(path, sizeof(path), "%s/summary.json", s_session_dir);
    FILE *f = fopen(path, "wb");
    if (f && s_line) {
        int n = snprintf(s_line, SD_LINE_CAP,
                         "{\"session\":\"%s\",\"frames\":%lu,\"dropped\":%lu,"
                         "\"events\":%lu,\"iq_files\":%lu,\"duration_ms\":%lld,"
                         "\"build\":\"%s\",\"clock\":\"%s\"}\n",
                         s_session_stamp, (unsigned long)s_frames_written,
                         (unsigned long)s_frames_dropped, (unsigned long)s_events_written,
                         (unsigned long)s_iq_files,
                         (long long)(uptime_ms() - s_session_start_ms), s_build,
                         s_clock_synced ? "host" : "unsynced");
        if (n > 0) (void)fwrite(s_line, 1, (size_t)n, f);
    }
    if (f) fclose(f);
    if (s_frames) fclose(s_frames);
    if (s_events) fclose(s_events);
    s_frames = s_events = NULL;
    s_logging = false;
}

bool t_embed_sdlog_active(void) { return s_logging; }

uint32_t t_embed_sdlog_frames_written(void) { return s_frames_written; }

void t_embed_sdlog_note_frame(const uint8_t *codes, unsigned nfft, unsigned lo_mhz,
                              unsigned rate_hz, int gain_index) {
    if (!s_logging || !codes || !nfft) return;
    int64_t now = uptime_ms();
    if (s_last_frame_ms && now - s_last_frame_ms < (int64_t)s_min_interval_ms) return;
    s_last_frame_ms = now;
    if (!ensure_line_buffer()) return;
    char iso[32];
    sj_iso8601(iso, sizeof(iso), t_embed_sdlog_now_ms());
    sj_frame_t frame = {
        .seq = s_frame_seq,
        .t_ms = now - s_session_start_ms,
        .iso = iso,
        .lo_mhz = lo_mhz,
        .nfft = nfft,
        .rate_hz = rate_hz,
        .gain_index = gain_index,
        .floor_code = sj_floor_code(codes, nfft),
    };
    size_t n = sj_frame_ndjson(s_line, SD_LINE_CAP, &frame, codes);
    if (!n) {
        s_frames_dropped++;
        return;
    }
    if (fwrite(s_line, 1, n, s_frames) != n) {
        s_frames_dropped++;
        return;
    }
    s_frames_written++;
    s_frame_seq++;
    /* Flush per frame: a yanked card or a dead battery should still leave a
     * readable prefix, which a buffered stream would lose. */
    if ((s_frames_written & 7u) == 0u) fflush(s_frames);
}

void t_embed_sdlog_note_event(unsigned khz, const char *level, unsigned reason,
                              int over_floor) {
    if (!s_logging || !s_events) return;
    if (!ensure_line_buffer()) return;
    char iso[32];
    sj_iso8601(iso, sizeof(iso), t_embed_sdlog_now_ms());
    size_t n = sj_event_ndjson(s_line, SD_LINE_CAP, uptime_ms() - s_session_start_ms,
                               iso, khz, level, reason, over_floor);
    if (!n) return;
    if (fwrite(s_line, 1, n, s_events) == n) {
        s_events_written++;
        fflush(s_events);
    }
}

static void sanitize_label(const char *label, char *out, size_t cap) {
    size_t j = 0;
    if (!label) label = "iq";
    for (size_t i = 0; label[i] && j + 1 < cap && j < 24; i++) {
        char c = label[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_';
        out[j++] = ok ? c : '-';
    }
    if (!j) { snprintf(out, cap, "iq"); return; }
    out[j] = '\0';
}

bool t_embed_sdlog_write_iq(const uint8_t *iq, size_t bytes, unsigned samples,
                            unsigned rate_code, unsigned lo_mhz, int gain_index,
                            const char *label) {
    if (!iq || !bytes || !samples) return false;
    if (!t_embed_sdlog_mount()) return false;
    char dir[128];
    if (s_logging) snprintf(dir, sizeof(dir), "%s/iq", s_session_dir);
    else snprintf(dir, sizeof(dir), SD_ROOT "/lone-iq");
    struct stat st;
    if (stat(dir, &st) != 0 && mkdir(dir, 0755) != 0 && errno != EEXIST) return false;

    char clean[32];
    sanitize_label(label, clean, sizeof(clean));
    char base[224];
    snprintf(base, sizeof(base), "%s/%06lu-%04lu-%.24s", dir,
             (unsigned long)((uptime_ms() / 1000) % 1000000u),
             (unsigned long)(s_iq_files + 1u), clean);
    char path[256];
    snprintf(path, sizeof(path), "%s.sigmf-data", base);
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(iq, 1, bytes, f) == bytes;
    fclose(f);
    if (!ok) return false;

    if (!ensure_line_buffer()) return false;
    char iso[32];
    sj_iso8601(iso, sizeof(iso), t_embed_sdlog_now_ms());
    sj_sigmf_t meta = {
        .datatype = "ci8",
        .hw = "lilygo-t-embed-s3r8",
        .description = "one-press IQ8 evidence capture",
        .recording_id = clean,
        .iso = iso,
        .center_hz = (double)lo_mhz * 1e6,
        .sample_rate_hz = (double)ring_capture_rate_hz(rate_code),
        .num_samples = samples,
        .rate_code = rate_code,
        .gain_index = gain_index,
    };
    snprintf(path, sizeof(path), "%s.sigmf-meta", base);
    f = fopen(path, "wb");
    if (!f) return false;
    size_t n = sj_sigmf_meta(s_line, SD_LINE_CAP, &meta);
    ok = n && fwrite(s_line, 1, n, f) == n;
    fclose(f);
    if (ok) s_iq_files++;
    return ok;
}
