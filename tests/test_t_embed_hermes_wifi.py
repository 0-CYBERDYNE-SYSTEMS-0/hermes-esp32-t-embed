from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "main/targets/esp32s3/t_embed_hermes.cpp"


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
class TEmbedHermesWifi(unittest.TestCase):
    def test_saved_networks_and_connection_fallback(self):
        source = SOURCE.read_text()
        storage = source[source.index("class HermesStorage "):source.index("class HermesDisplay ")]
        wifi = source[source.index("struct NetworkEvent "):source.index("enum class LocalView ")]
        parser = source[source.index("bool parse_quoted("):source.index("bool has_prefix(")]
        parser += source[source.index("    static bool parse_wifi("):source.index("    static bool parse_server(")]
        # Compile the production classes against host NVS/Wi-Fi fakes so tests
        # exercise the actual saved keys, credential copies and retry behavior.
        harness = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
using esp_err_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_ARG = 1, ESP_ERR_INVALID_STATE = 2;
constexpr int ESP_ERR_NO_MEM = 3, ESP_ERR_NVS_NOT_FOUND = 4;
constexpr int ESP_ERR_INVALID_SIZE = 5, ESP_ERR_NVS_INVALID_NAME = 6;
constexpr int ESP_ERR_WIFI_NOT_CONNECT = 7;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
using nvs_handle_t = int;
using nvs_open_mode_t = int;
const char *esp_err_to_name(int) { return "fake error"; }
#define ESP_LOGE(...) do {} while (0)
#define ESP_LOGW(...) do {} while (0)
const char *kTag = "test";
constexpr int kNetworkBackoffMs[] = {1000, 2000, 4000, 8000, 15000, 30000};
void secure_zero(void *p, size_t n) { std::memset(p, 0, n); }
void wipe(std::string &s) { secure_zero(s.data(), s.size()); s.clear(); }
namespace hg {
struct Storage {
    virtual ~Storage() = default;
    virtual std::optional<std::string> get(std::string_view) = 0;
    virtual void set(std::string_view, std::string_view) = 0;
    virtual void erase(std::string_view) = 0;
};
}
std::map<std::string, std::string> saved;
int nvs_flash_init() { return ESP_OK; }
int nvs_open(const char *name, int, int *h) {
    assert(std::string(name) == "hgadget"); *h = 1; return ESP_OK;
}
int nvs_get_str(int, const char *key, char *value, size_t *size) {
    auto it = saved.find(key);
    if (it == saved.end()) return ESP_ERR_NVS_NOT_FOUND;
    *size = it->second.size() + 1;
    if (value) std::memcpy(value, it->second.c_str(), *size);
    return ESP_OK;
}
int nvs_set_str(int, const char *key, const char *value) {
    saved[key] = value; return ESP_OK;
}
int nvs_commit(int) { return ESP_OK; }
void nvs_close(int) {}
int nvs_erase_key(int, const char *key) { saved.erase(key); return ESP_OK; }
using QueueHandle_t = void *;
using esp_event_base_t = int;
using esp_event_handler_instance_t = void *;
struct esp_netif_t {};
struct wifi_init_config_t {};
struct wifi_config_t {
    struct { uint8_t ssid[32]; uint8_t password[64]; } sta;
};
struct wifi_event_sta_disconnected_t { uint16_t reason; int8_t rssi; };
constexpr int WIFI_EVENT = 1, IP_EVENT = 2, ESP_EVENT_ANY_ID = 0;
constexpr int WIFI_EVENT_STA_DISCONNECTED = 3, IP_EVENT_STA_GOT_IP = 4;
constexpr int WIFI_STORAGE_RAM = 0, WIFI_MODE_STA = 0, WIFI_IF_STA = 0, WIFI_PS_NONE = 0;
#define WIFI_INIT_CONFIG_DEFAULT() wifi_init_config_t{}
wifi_config_t applied{};
int connect_count = 0, connect_error = ESP_OK, config_error = ESP_OK;
int esp_netif_init() { return ESP_OK; }
int esp_event_loop_create_default() { return ESP_OK; }
esp_netif_t *esp_netif_create_default_wifi_sta() { static esp_netif_t n; return &n; }
int esp_wifi_init(wifi_init_config_t *) { return ESP_OK; }
int esp_event_handler_instance_register(int, int, void (*)(void *, int, int, void *), void *, void **) {
    return ESP_OK;
}
int esp_wifi_set_storage(int) { return ESP_OK; }
int esp_wifi_set_mode(int) { return ESP_OK; }
int esp_wifi_set_config(int, wifi_config_t *config) {
    if (config_error != ESP_OK) return config_error;
    applied = *config; return ESP_OK;
}
int esp_wifi_start() { return ESP_OK; }
int esp_wifi_set_ps(int) { return ESP_OK; }
int esp_wifi_connect() { ++connect_count; return connect_error; }
int esp_wifi_disconnect() { return ESP_OK; }
int xQueueSend(void *, const void *, int) { return 1; }
std::string ssid() {
    return std::string(reinterpret_cast<char *>(applied.sta.ssid),
                       strnlen(reinterpret_cast<char *>(applied.sta.ssid), 32));
}
std::string password() { return reinterpret_cast<char *>(applied.sta.password); }
'''
        checks = r'''
int main() {
    std::string parsed_ssid, parsed_password;
    assert(parse_wifi(R"(WIFI2 "hotspot" "a\"b\\c")", parsed_ssid, parsed_password));
    assert(parsed_ssid == "hotspot" && parsed_password == "a\"b\\c");
    assert(parse_wifi(R"(WIFI "home" "")", parsed_ssid, parsed_password));
    assert(!parse_wifi(R"(WIFI2 "" "password")", parsed_ssid, parsed_password));
    assert(!parse_wifi(R"(WIFI2 "hotspot" "password" extra)", parsed_ssid, parsed_password));
    assert(!parse_wifi(R"(WIFI2 "hotspot" "bad\n")", parsed_ssid, parsed_password));
    HermesStorage storage;
    assert(storage.initialize());
    storage.set("server", "ws://home/gadget");
    storage.set("device_key", "existing-pairing");
    assert(storage.set_wifi("home", "home password"));
    assert(storage.set_wifi("hotspot", "hotspot password", true));
    assert(storage.get("wifi_ssid") == "home");
    assert(storage.get("wifi_pass") == "home password");
    assert(storage.get("wifi2_ssid") == "hotspot");
    assert(storage.get("wifi2_pass") == "hotspot password");
    assert(storage.get("server") == "ws://home/gadget");
    assert(storage.get("device_key") == "existing-pairing");

    HermesWifi wifi(nullptr);
    assert(wifi.start("home", "home password", "hotspot", "hotspot password"));
    assert(ssid() == "home" && password() == "home password");
    assert(!wifi.using_fallback());
    wifi.disconnected(100);
    assert(!wifi.connect_retry_due(1099));
    assert(wifi.connect_retry_due(1100));
    wifi.retry(1100);
    assert(ssid() == "hotspot" && password() == "hotspot password");
    assert(wifi.using_fallback());
    // Do not change profiles again while the hotspot attempt is in progress.
    assert(!wifi.connect_retry_due(100000));
    wifi.disconnected(1200);
    assert(!wifi.connect_retry_due(3199));
    assert(wifi.connect_retry_due(3200));
    wifi.retry(3200);
    assert(ssid() == "home" && password() == "home password");
    wifi.connected();
    assert(!wifi.connect_retry_due(100000));
    wifi.disconnected(4000);
    assert(wifi.connect_retry_due(5000));
    wifi.retry(5000);
    assert(ssid() == "hotspot");
    wifi.connected();
    assert(wifi.using_fallback());

    // Immediate SDK failures schedule another attempt and do not call connect
    // after a rejected configuration. A later attempt can recover on home Wi-Fi.
    wifi.disconnected(6000);
    config_error = ESP_ERR_INVALID_STATE;
    int before = connect_count;
    wifi.retry(7000);
    assert(connect_count == before);
    assert(wifi.last_error() == ESP_ERR_INVALID_STATE);
    assert(wifi.connect_retry_due(9000));
    config_error = ESP_OK;
    connect_error = ESP_ERR_INVALID_STATE;
    wifi.retry(9000);
    assert(wifi.connect_retry_due(13000));
    connect_error = ESP_OK;
    wifi.retry(13000);
    wifi.connected();
    assert(ssid() == "home" && wifi.last_error() == ESP_OK);

    // A manual hotspot choice must remain on hotspot when it fails, and must
    // not rewrite either saved network. Auto explicitly returns to home first.
    assert(wifi.choose_network(1, false, 14000));
    assert(!wifi.automatic());
    assert(!wifi.connect_retry_due(14999));
    assert(wifi.connect_retry_due(15000));
    wifi.retry(15000);
    assert(ssid() == "hotspot" && wifi.using_fallback());
    wifi.disconnected(16000);
    wifi.retry(18000);
    assert(ssid() == "hotspot");
    wifi.connected();
    assert(wifi.choose_network(0, false, 19000));
    wifi.retry(20000);
    assert(ssid() == "home" && !wifi.automatic());
    wifi.disconnected(21000);
    wifi.retry(23000);
    assert(ssid() == "home");
    assert(wifi.choose_network(0, true, 24000));
    wifi.retry(25000);
    assert(ssid() == "home" && wifi.automatic());
    wifi.disconnected(26000);
    wifi.retry(28000);
    assert(ssid() == "hotspot");
    assert(storage.get("wifi_ssid") == "home");
    assert(storage.get("wifi_pass") == "home password");
    assert(storage.get("wifi2_ssid") == "hotspot");
    assert(storage.get("wifi2_pass") == "hotspot password");

    HermesWifi single(nullptr);
    assert(single.start("home", "home password"));
    single.disconnected(0);
    single.retry(1000);
    assert(ssid() == "home" && password() == "home password");
    assert(!single.using_fallback());
    assert(!single.choose_network(1, false, 2000));
    assert(single.automatic());
    assert(!single.choose_network(2, false, 2000));
    single.connected();
    single.disconnected(UINT32_MAX - 499);
    assert(!single.connect_retry_due(499));
    assert(single.connect_retry_due(500));

    // A full 32-byte SSID has no NUL terminator; preserve all of it.
    HermesWifi limits(nullptr);
    assert(limits.start(std::string(32, 'h'), std::string(63, 'p'), "hotspot", ""));
    assert(ssid() == std::string(32, 'h') && password() == std::string(63, 'p'));
    limits.disconnected(0);
    limits.retry(1000);
    assert(ssid() == "hotspot" && password().empty());
    HermesWifi invalid(nullptr);
    assert(!invalid.start("home", "", std::string(33, 'x'), ""));
    assert(!invalid.initialized());
    assert(!invalid.start("home", "", "hotspot", std::string(64, 'x')));
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "check.cpp"
            path.write_text(harness + storage + wifi + parser + checks)
            exe = Path(tmp) / "check"
            subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                            str(path), "-o", str(exe)], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
