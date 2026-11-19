#include "t_embed_wifisniff.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"

#include "pcap_ng.h"
#include "t_embed_sdlog.h"

static const char *kTag = "wifisniff";

#define WS_SLOTS 24u
#define WS_FRAME_CAP PG_SNAPLEN
#define WS_QUEUE_BYTES (WS_SLOTS * (WS_FRAME_CAP + 16u))

typedef struct {
    uint32_t len;
    uint64_t ts_ns;
    int8_t dbm;
    uint8_t frame[WS_FRAME_CAP];
} ws_slot_t;

static ws_slot_t *s_slots;
static volatile unsigned s_head;   /* producer */
static volatile unsigned s_tail;   /* consumer */
static volatile bool s_active;
static FILE *s_file;
static char s_path[160];
static unsigned s_channel;
static uint32_t s_seen, s_stored, s_dropped;
static uint32_t s_mgmt, s_ctrl, s_data, s_misc;
static uint32_t s_beacons, s_probes;

static uint64_t now_ns(void) {
    int64_t wall = t_embed_sdlog_now_ms();
    if (wall > 0) return (uint64_t)wall * 1000000ull;
    return (uint64_t)esp_timer_get_time() * 1000ull;
}

static void sniff_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (!s_active || !s_slots) return;
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    unsigned len = pkt->rx_ctrl.sig_len;
    s_seen++;
    switch (type) {
        case WIFI_PKT_MGMT: s_mgmt++; break;
        case WIFI_PKT_CTRL: s_ctrl++; break;
        case WIFI_PKT_DATA: s_data++; break;
        default: s_misc++; break;
    }
    if (len >= 2u) {
        uint8_t subtype = pkt->payload[0];
        if ((subtype & 0xf0u) == 0x80u) s_beacons++;
        if (subtype == 0x40u) s_probes++;
    }
    if (len > WS_FRAME_CAP) len = WS_FRAME_CAP;
    unsigned next = (s_head + 1u) % WS_SLOTS;
    if (next == s_tail) {          /* consumer behind: drop, never block the radio */
        s_dropped++;
        return;
    }
    ws_slot_t *slot = &s_slots[s_head];
    slot->len = len;
    slot->ts_ns = now_ns();
    slot->dbm = (int8_t)pkt->rx_ctrl.rssi;
    memcpy(slot->frame, pkt->payload, len);
    s_head = next;
}

static bool open_capture(void) {
    const char *dir = t_embed_sdlog_dir();
    char target[96];
    if (dir && dir[0]) snprintf(target, sizeof(target), "%s/net", dir);
    else snprintf(target, sizeof(target), "/sdcard/sigint/wifi");
    struct stat st;
    if (stat(target, &st) != 0 && mkdir(target, 0755) != 0 && errno != EEXIST) return false;
    snprintf(s_path, sizeof(s_path), "%s/wifi-%lu.pcapng", target,
             (unsigned long)((esp_timer_get_time() / 1000000) % 1000000u));
    s_file = fopen(s_path, "wb");
    if (!s_file) return false;
    char header[512];
    size_t at = pg_shb((uint8_t *)header, sizeof(header));
    at += pg_idb(header + at, sizeof(header) - at, "esp32s3-sniffer", PG_SNAPLEN);
    if (at != fwrite(header, 1, at, s_file)) {
        fclose(s_file);
        s_file = NULL;
        return false;
    }
    fflush(s_file);
    return true;
}

bool t_embed_wifisniff_start(unsigned channel) {
    if (s_active) return true;
    if (!t_embed_sdlog_mount()) return false;
    if (!s_slots) {
        s_slots = (ws_slot_t *)heap_caps_malloc(WS_QUEUE_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_slots) s_slots = (ws_slot_t *)heap_caps_malloc(WS_QUEUE_BYTES, MALLOC_CAP_8BIT);
        if (!s_slots) return false;
    }
    esp_err_t err = esp_wifi_set_channel((uint8_t)channel, WIFI_SECOND_CHAN_NONE);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "channel %u: %s", channel, esp_err_to_name(err));
        return false;
    }
    wifi_promiscuous_filter_t filter = {.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL};
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(sniff_cb);
    esp_wifi_set_promiscuous(true);
    s_head = s_tail = 0;
    s_seen = s_stored = s_dropped = 0;
    s_mgmt = s_ctrl = s_data = s_misc = s_beacons = s_probes = 0;
    s_channel = channel;
    s_active = true;
    if (!open_capture()) {
        ESP_LOGW(kTag, "no capture file; counting frames only");
    }
    return true;
}

void t_embed_wifisniff_stop(void) {
    if (!s_active) return;
    s_active = false;
    esp_wifi_set_promiscuous_rx_cb(NULL);
    t_embed_wifisniff_service();
    if (s_file) {
        fflush(s_file);
        fclose(s_file);
        s_file = NULL;
    }
}

bool t_embed_wifisniff_active(void) { return s_active; }

bool t_embed_wifisniff_recording(void) { return s_file != NULL; }

void t_embed_wifisniff_service(void) {
    if (!s_slots) return;
    uint8_t packet[PG_RADIOTAP_LEN + WS_FRAME_CAP];
    uint8_t block[32 + PG_RADIOTAP_LEN + WS_FRAME_CAP + 8];
    while (s_tail != s_head) {
        ws_slot_t *slot = &s_slots[s_tail];
        size_t rtlen = pg_radiotap(packet, sizeof(packet), (uint16_t)(2407u + 5u * s_channel),
                                   slot->dbm);
        memcpy(packet + rtlen, slot->frame, slot->len);
        uint32_t total = (uint32_t)(rtlen + slot->len);
        if (s_file) {
            size_t n = pg_epb(block, sizeof(block), slot->ts_ns, packet, total);
            if (n && fwrite(block, 1, n, s_file) == n) {
                s_stored++;
            } else {
                s_dropped++;
            }
        }
        s_tail = (s_tail + 1u) % WS_SLOTS;
    }
    if (s_file && s_stored && (s_stored & 31u) == 0u) fflush(s_file);
}

void t_embed_wifisniff_status_json(char *out, size_t cap) {
    if (!out || !cap) return;
    snprintf(out, cap,
             "{\"active\":%u,\"recording\":%u,\"channel\":%u,\"seen\":%lu,"
             "\"stored\":%lu,\"dropped\":%lu,\"mgmt\":%lu,\"ctrl\":%lu,"
             "\"data\":%lu,\"misc\":%lu,\"beacons\":%lu,\"probes\":%lu,"
             "\"file\":\"%s\"}\n",
             (unsigned)s_active, (unsigned)(s_file != NULL), s_channel,
             (unsigned long)s_seen, (unsigned long)s_stored, (unsigned long)s_dropped,
             (unsigned long)s_mgmt, (unsigned long)s_ctrl, (unsigned long)s_data,
             (unsigned long)s_misc, (unsigned long)s_beacons, (unsigned long)s_probes,
             s_path);
}
