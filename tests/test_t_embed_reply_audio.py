from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CORE = ROOT / "platform/hermes_core"


@unittest.skipUnless(shutil.which("c++"), "Host C++ compiler unavailable")
class TEmbedReplyAudio(unittest.TestCase):
    def test_persistence_menu_errors_and_connection_changes(self):
        source = (ROOT / "main/targets/esp32s3/t_embed_hermes.cpp").read_text()
        storage = source[source.index("class HermesStorage "):source.index("class HermesDisplay ")]
        views = source[source.index("enum class LocalView "):source.index("bool parse_quoted(")]
        menus = source[source.index("    void open_action_menu("):source.index("    void send_scroll(")]
        sync = source[source.index("    void synchronize_view("):source.index("    uint32_t input_hold_ms(")]
        render = source[source.index("    void render_local_view("):source.index("    void pump_microphone(")]
        ui = (ROOT / "main/targets/esp32s3/t_embed_ui.h").read_text()
        inputs = ui[ui.index("typedef enum {"):ui.index("/* The standard T-Embed")]
        gesture = (ROOT / "main/targets/esp32s3/t_embed_gesture.c").read_text()
        move = gesture[gesture.index("unsigned t_embed_menu_move("):]
        harness = r'''
#define HERMES_BOARD_TEST
#include "hermes_text_mode.cpp"
#include <cstring>
#include <memory>
using esp_err_t = int;
using nvs_handle_t = int;
using nvs_open_mode_t = int;
constexpr int ESP_OK = 0, ESP_ERR_INVALID_STATE = 1, ESP_ERR_NVS_NOT_FOUND = 2;
constexpr int ESP_ERR_INVALID_SIZE = 3, ESP_ERR_NVS_INVALID_NAME = 4;
constexpr int NVS_READONLY = 0, NVS_READWRITE = 1;
constexpr int64_t kNoExpiry = 0;
#define ESP_LOGE(...)
std::map<std::string, std::string> saved, pending;
int init_error = 0, read_error = 0, write_error = 0, commit_error = 0, open_error = 0;
void wipe(std::string& value) { std::fill(value.begin(), value.end(), 0); value.clear(); }
int nvs_flash_init() { return init_error; }
int nvs_open(const char* name, int, int* handle) {
    assert(std::string(name) == "hgadget"); *handle = 1; return open_error;
}
int nvs_get_str(int, const char* key, char* value, size_t* size) {
    if (read_error) return read_error;
    auto it = saved.find(key);
    if (it == saved.end()) return ESP_ERR_NVS_NOT_FOUND;
    *size = it->second.size() + 1;
    if (value) std::memcpy(value, it->second.c_str(), *size);
    return ESP_OK;
}
int nvs_set_str(int, const char* key, const char* value) {
    if (write_error) return write_error;
    pending[key] = value; return ESP_OK;
}
int nvs_commit(int) {
    if (commit_error) return commit_error;
    for (const auto& entry : pending) saved[entry.first] = entry.second;
    return ESP_OK;
}
void nvs_close(int) { pending.clear(); }
int nvs_erase_key(int, const char* key) { saved.erase(key); return ESP_OK; }
std::string drawn_title, drawn_hint;
std::vector<std::string> drawn_items;
unsigned drawn_selection = 0;
void t_embed_ui_invalidate_overlays() {}
void t_embed_ui_set_frame_flush_suppressed(bool) {}
void t_embed_ui_flush_framebuffer(const uint16_t*, unsigned, unsigned) {}
void t_embed_ui_render_prompt_overlay(const char*, const char*, unsigned, bool, unsigned, unsigned) {}
void t_embed_ui_render_menu_overlay(const char* title, const char* const* items,
                                    unsigned count, unsigned selection, const char* hint) {
    assert(count <= 6 && selection < count);
    drawn_title = title; drawn_hint = hint; drawn_selection = selection;
    drawn_items.assign(items, items + count);
}
struct FakeWifi {
    bool choose_network(unsigned, bool, uint32_t) { return true; }
    bool fallback_configured() const { return true; }
    bool automatic() const { return true; }
    bool using_fallback() const { return false; }
};
struct FakeDisplay {
    bool ready() const { return false; }
    uint16_t* framebuffer() { return nullptr; }
};
'''
        board = r'''
class Board {
public:
    HermesStorage storage_;
    FakeHal audio_, system_;
    FakeDisplay display_;
    std::unique_ptr<FakeWifi> wifi_ = std::make_unique<FakeWifi>();
    LocalView local_view_ = LocalView::None;
    const char* reply_audio_notice_ = nullptr;
    PromptGuard prompt_;
    std::string view_prompt_id_;
    unsigned selection_ = 0, volume_draft_ = 70, prompt_scroll_ = 0, prompt_selection_ = 0;
    bool action_menu_online_ = false, wifi_up_ = false, display_suppressed_ = false;
    bool answer_view_ = false, prompt_arming_notice_ = false;
    char volume_label_[24]{};
    void clear_prompt_guard() { prompt_.active = false; }
    void restart_to_launcher(hg::App&) { assert(false && "menu must not accidentally select Home"); }
'''
        checks = r'''
};
int main() {
    Rig r;
    Board b;
    assert(b.storage_.initialize());
    r.hal.storage = &b.storage_;
    b.load_voice_reply(r.app);
    assert(r.app.voice_replies_enabled());
    r.online();
    t_embed_input_event_t click{};
    click.short_click = true;
    click.press_context = T_EMBED_INPUT_HERMES_MENU;
    b.open_action_menu(r.app);
    assert(b.selection_ == 5);
    b.selection_ = 2;
    b.handle_menu_input(r.app, click);
    assert(b.local_view_ == LocalView::ReplyAudio && b.selection_ == 0);
    assert(b.input_context(r.app) == T_EMBED_INPUT_HERMES_MENU);
    b.render_local_view(r.app);
    assert(drawn_items[0] == "VOICE REPLIES: ON");
    b.handle_menu_input(r.app, click);
    assert(b.local_view_ == LocalView::None && !r.app.voice_replies_enabled());
    assert(saved["voice_reply"] == "off");
    // The saved preference is restored in a fresh app, before begin/network.
    Rig reboot;
    Board loaded;
    assert(loaded.storage_.initialize());
    loaded.load_voice_reply(reboot.app);
    assert(!reboot.app.voice_replies_enabled());

    b.local_view_ = LocalView::ReplyAudio;
    b.selection_ = 1;
    b.handle_menu_input(r.app, click);
    assert(b.local_view_ == LocalView::Volume);
    t_embed_input_event_t rotate{};
    rotate.detents = 2;
    b.handle_menu_input(r.app, rotate);
    b.handle_menu_input(r.app, click);
    assert(saved["volume"] == "80" && saved["voice_reply"] == "off");
    assert(!r.app.voice_replies_enabled());
    assert(b.local_view_ == LocalView::ReplyAudio && b.selection_ == 2);
    b.handle_menu_input(r.app, click);
    assert(b.local_view_ == LocalView::ActionMenu && b.selection_ == 5);

    // Connection transitions reset the selection to Back in either menu.
    b.selection_ = 0;
    r.app.on_network(false);
    b.synchronize_view(r.app);
    assert(b.selection_ == 3);
    b.render_local_view(r.app);
    assert(drawn_items.size() == 4 && drawn_items[drawn_selection] == "BACK");
    b.selection_ = 2;
    b.handle_menu_input(r.app, click);
    assert(b.local_view_ == LocalView::ReplyAudio);
    b.handle_menu_input(r.app, click);
    assert(r.app.voice_replies_enabled() && saved["voice_reply"] == "on");
    b.open_action_menu(r.app);
    b.selection_ = 0;
    r.app.on_network(true);
    r.advance(2000);
    r.app.on_transport_open();
    r.server(R"({"type":"welcome","paired":true})");
    b.synchronize_view(r.app);
    assert(b.selection_ == 5);

    // A failed save keeps the requested runtime mode and explicitly says so.
    b.local_view_ = LocalView::ReplyAudio;
    b.selection_ = 0;
    commit_error = ESP_ERR_INVALID_STATE;
    b.handle_menu_input(r.app, click);
    assert(!r.app.voice_replies_enabled() && saved["voice_reply"] == "on");
    b.render_local_view(r.app);
    assert(drawn_hint == "SETTING NOT SAVED" && drawn_items[0] == "VOICE REPLIES: OFF");
    commit_error = 0;
    write_error = ESP_ERR_INVALID_STATE;
    b.handle_menu_input(r.app, click);
    assert(r.app.voice_replies_enabled() && b.local_view_ == LocalView::ReplyAudio);
    write_error = 0;
    b.handle_menu_input(r.app, click);
    assert(saved["voice_reply"] == "off" && b.reply_audio_notice_ == nullptr);

    saved["voice_reply"] = "bad";
    b.load_voice_reply(r.app);
    assert(!r.app.voice_replies_enabled() && b.selection_ == 2);
    b.render_local_view(r.app);
    assert(drawn_hint == "VOICE SETTING ERROR");
    read_error = ESP_ERR_INVALID_STATE;
    b.load_voice_reply(r.app);
    assert(!r.app.voice_replies_enabled());
    read_error = 0;
    open_error = ESP_ERR_NVS_NOT_FOUND;  // absent namespace on first boot
    b.load_voice_reply(r.app);
    assert(r.app.voice_replies_enabled());
    open_error = ESP_ERR_INVALID_STATE;
    b.load_voice_reply(r.app);
    assert(!r.app.voice_replies_enabled());
    open_error = 0;
    init_error = ESP_ERR_INVALID_STATE;
    Board unavailable;
    assert(!unavailable.storage_.initialize());
    unavailable.load_voice_reply(r.app);
    assert(!r.app.voice_replies_enabled());
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "board.cpp"
            path.write_text(harness + storage + views + inputs + move + board +
                            menus + sync + render + checks)
            exe = Path(tmp) / "board"
            subprocess.run([
                "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-I", str(CORE / "include"), "-I", str(ROOT / "tests"), str(path),
                *map(str, sorted((CORE / "src").glob("*.cpp"))), "-o", str(exe),
            ], check=True)
            subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    unittest.main()
