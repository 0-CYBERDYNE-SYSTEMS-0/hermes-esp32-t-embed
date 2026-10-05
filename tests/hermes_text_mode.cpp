// Exercise the vendored core, including its real 320 x 170 framebuffer renderer.
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "hg/app.hpp"
#include "hg/protocol.hpp"

struct FakeHal : hg::Display, hg::AudioIn, hg::AudioOut,
                 hg::Transport, hg::Storage, hg::System {
    uint32_t clock = 1000;
    std::vector<uint16_t> pixels = std::vector<uint16_t>(320 * 170);
    std::map<std::string, std::string> saved;
    std::vector<hg::json::Value> sent;
    size_t mic_bytes = 0, speaker_samples = 0;
    unsigned begins = 0, aborts = 0;
    bool recording = false, playing = false;

    uint32_t now_ms() override { return clock; }
    void random_bytes(uint8_t* out, size_t size) override {
        for (size_t i = 0; i < size; i++) out[i] = static_cast<uint8_t>(i);
    }
    void log(hg::LogLevel, std::string_view) override {}
    hg::DisplayInfo info() const override { return {320, 170, true, false, false}; }
    uint16_t* framebuffer() override { return pixels.data(); }
    void flush(uint16_t y0, uint16_t y1) override { assert(y0 <= y1 && y1 <= 170); }
    bool start(uint32_t rate) override { assert(rate == 16000); return recording = true; }
    void stop() override { recording = false; }
    bool begin(uint32_t rate) override {
        assert(rate == 16000);
        ++begins;
        return playing = true;
    }
    void write(const int16_t*, size_t count) override { speaker_samples += count; }
    void end() override { playing = false; }
    void abort() override { ++aborts; playing = false; }
    bool busy() const override { return playing; }
    void connect(const std::string&, const std::string&) override {}
    void close() override {}
    bool send_text(std::string_view text) override {
        hg::json::Value value;
        assert(hg::json::parse(text, value));
        sent.push_back(value);
        return true;
    }
    bool send_binary(const uint8_t*, size_t size) override { mic_bytes += size; return true; }
    std::optional<std::string> get(std::string_view key) override {
        auto it = saved.find(std::string(key));
        if (it == saved.end()) return std::nullopt;
        return it->second;
    }
    void set(std::string_view key, std::string_view value) override {
        saved[std::string(key)] = std::string(value);
    }
    void erase(std::string_view key) override { saved.erase(std::string(key)); }
    hg::Hal hal() { return {this, this, this, this, this, this, nullptr}; }
    unsigned messages(std::string_view type) const {
        unsigned count = 0;
        for (const auto& message : sent) if (message["type"].as_string() == type) ++count;
        return count;
    }
};

struct Rig {
    FakeHal fake;
    hg::Hal hal = fake.hal();
    hg::App app;
    explicit Rig(bool scroll = true) : app(hal, profile(scroll)) {}
    static hg::DeviceProfile profile(bool scroll) {
        hg::DeviceProfile p;
        p.default_name = "T-Embed";
        p.default_server_url = "ws://test/gadget";
        p.has_scroll_buttons = scroll;
        p.talk_label = "HOLD KNOB";
        return p;
    }
    void server(std::string_view text) { app.on_transport_text(text); }
    void message(const char* type, const std::string& text) {
        auto value = hg::proto::message(type);
        value.set("text", text).set("turn", "t1");
        server(value.dump());
    }
    void advance(uint32_t ms) {
        fake.clock += ms;
        server(R"({"type":"ping"})");
        app.tick();
    }
    void online() {
        app.begin();
        app.on_network(true);
        advance(1000);
        app.on_transport_open();
        server(R"({"type":"challenge","nonce":"bm9uY2U=","enrolled":false})");
        server(R"({"type":"welcome","session":"s1","paired":true,"heartbeat_s":20})");
        assert(app.online() && app.paired());
    }
};

std::string long_reply() {
    std::string text;
    for (int i = 0; i < 45; ++i)
        text += "Line " + std::to_string(i) + ": Read this reply at your own pace.\n";
    return text;
}

void reading() {
    Rig r;
    r.online();
    r.server(R"({"type":"turn.start","turn":"t1"})");
    r.message("reply.delta", long_reply());
    assert(r.app.model().scroll == -1);
    r.app.on_button(hg::Button::Up, true);
    const int position = r.app.model().scroll;
    assert(position > 0);
    r.message("reply.delta", long_reply() + "More arriving text.");
    assert(r.app.model().scroll == position);
    r.message("reply", long_reply() + "Final answer.");
    assert(r.app.model().scroll == position);
    r.server(R"({"type":"turn.end","turn":"t1","outcome":"success"})");
    r.advance(60000);
    assert(r.app.screen() == hg::Screen::Ready);
    assert(!r.app.model().hero && r.app.model().scroll == position);
    r.app.on_button(hg::Button::Down, true);
    assert(r.app.model().scroll == position + 1);
    for (int i = 0; i < 100; ++i) r.app.on_button(hg::Button::Up, true);
    assert(r.app.model().scroll == 0);
    for (int i = 0; i < 100; ++i) r.app.on_button(hg::Button::Down, true);
    const int bottom = r.app.model().scroll;
    r.app.on_button(hg::Button::Down, true);
    assert(r.app.model().scroll == bottom && bottom > position);
    // A shorter cumulative replacement must clamp the actual stored position.
    r.message("reply.delta", "Short replacement.");
    assert(r.app.model().body == "Short replacement.");
    assert(r.app.model().scroll == 0);
    r.message("reply.delta", long_reply());
    assert(r.app.model().scroll == 0);
    r.app.on_button(hg::Button::Talk, true);
    assert(r.app.screen() == hg::Screen::Listening && r.fake.recording);
    r.advance(500);
    r.app.on_button(hg::Button::Talk, false);
    r.server(R"({"type":"turn.start","turn":"t1"})");
    r.message("reply.delta", long_reply());
    assert(r.app.model().scroll == -1);
    r.message("reply", long_reply());
    assert(r.app.model().scroll == 0);
    r.app.console("new-session");
    assert(r.fake.messages("session.new") == 1);
    assert(r.app.model().body.empty());
}

void wrapping() {
    Rig r;
    hg::Ui ui(r.fake);
    assert(ui.layout().body_cols == 52 && ui.layout().body_rows == 13);
    const std::string text = "An answer that wraps at word boundaries.\n\n" + std::string(120, 'x');
    const auto lines = hg::wrap_text(text, ui.layout().body_cols);
    assert(lines.size() == 5 && lines[1].empty());
    for (const auto& line : lines) assert(line.size() <= 52);
    assert(lines[2] + lines[3] + lines[4] == std::string(120, 'x'));

    // Boards without scroll controls retain automatic paging and reply timeout.
    Rig automatic(false);
    automatic.online();
    automatic.message("reply", long_reply());
    automatic.advance(14000);
    assert(automatic.app.model().scroll > 0);
    automatic.message("reply", "Short reply.");
    automatic.server(R"({"type":"turn.end","turn":"t1"})");
    automatic.advance(1);
    automatic.advance(21000);
    assert(automatic.app.model().hero);
}

void voice() {
    Rig r;
    r.app.set_voice_replies_enabled(false);
    r.online();
    assert(!r.app.voice_replies_enabled());
    assert(r.app.model().title == "TEXT | T-Embed");
    r.server(R"({"type":"audio.start","stream":3,"rate":16000})");
    std::vector<uint8_t> frame(hg::proto::kBinaryHeader + 640, 0);
    hg::proto::write_binary_header(frame.data(), hg::proto::Channel::Audio, 3, 0);
    r.app.on_transport_binary(frame.data(), frame.size());
    assert(r.fake.begins == 0 && r.fake.speaker_samples == 0);
    assert(!r.app.model().speaking);
    // Both console and agent volume changes must respect the local preference.
    r.app.console("set volume 100");
    r.server(R"({"type":"action","id":"v1","name":"speaker.volume","args":{"percent":100}})");
    r.server(R"({"type":"audio.start","stream":3,"rate":16000})");
    assert(r.fake.begins == 0);
    r.app.on_button(hg::Button::Talk, true);
    assert(r.fake.recording && r.app.model().title == "TEXT | T-Embed");
    int16_t samples[640]{};
    r.app.on_mic_samples(samples, 640);
    r.advance(500);
    r.app.on_button(hg::Button::Talk, false);
    assert(r.fake.mic_bytes > 0 && r.fake.messages("audio.end") == 1);
    r.message("transcript", "A quiet question.");
    assert(r.app.model().detail == "\"A quiet question.\"");
    r.message("reply.delta", "Here is the answer.");
    assert(r.app.model().body == "Here is the answer.");
    r.app.set_voice_replies_enabled(true);
    assert(r.app.model().title == "T-Embed");
    r.app.on_transport_binary(frame.data(), frame.size());
    assert(r.fake.speaker_samples == 0);  // no replay of a muted stream
    r.server(R"({"type":"audio.start","stream":3,"rate":16000})");
    r.app.on_transport_binary(frame.data(), frame.size());
    assert(r.fake.begins == 1 && r.fake.speaker_samples == 320);
    assert(r.app.model().speaking);
    const unsigned aborts = r.fake.aborts;
    r.app.set_voice_replies_enabled(false);
    assert(r.fake.aborts == aborts + 1 && !r.app.model().speaking);
    assert(r.fake.messages("cancel") == 0);
    assert(r.app.model().body == "Here is the answer.");
    r.app.on_transport_binary(frame.data(), frame.size());
    r.server(R"({"type":"audio.end","stream":3})");
    r.server(R"({"type":"audio.start","stream":4,"rate":16000})");
    assert(r.fake.begins == 1 && r.fake.speaker_samples == 320);
    r.message("reply", "The complete answer.");
    r.server(R"({"type":"turn.end","turn":"t1"})");
    r.advance(1);
    assert(r.app.screen() == hg::Screen::Ready);
    r.app.on_network(false);
    r.app.on_network(true);
    r.advance(2000);
    r.app.on_transport_open();
    r.server(R"({"type":"welcome","session":"s2","paired":true})");
    r.server(R"({"type":"audio.start","stream":4,"rate":16000})");
    assert(r.app.online() && !r.app.voice_replies_enabled() && r.fake.begins == 1);
}

void overlays() {
    Rig r;
    r.online();
    r.message("reply", long_reply());
    r.app.on_button(hg::Button::Down, true);
    assert(r.app.model().scroll == 1);
    r.server(R"({"type":"prompt","id":"p1","title":"Approve?","text":"Read this first.","ttl_s":1})");
    assert(r.app.screen() == hg::Screen::Prompt);
    r.app.on_button(hg::Button::Down, true);
    r.advance(1500);
    assert(r.app.screen() != hg::Screen::Prompt);
    assert(r.fake.messages("prompt.reply") == 0);
    assert(r.app.model().scroll == 1);
    auto card = hg::proto::message("display");
    card.set("title", "Card").set("body", long_reply()).set("ttl_s", 120);
    r.server(card.dump());
    assert(r.app.screen() == hg::Screen::Card);
    r.advance(14000);
    assert(r.app.model().scroll > 0);  // card paging remains unchanged
}

void screenshots(const std::string& directory) {
    Rig r;
    r.app.set_voice_replies_enabled(false);
    r.online();
    r.message("reply", "You can keep speaking into the microphone while voice replies are off. "
        "The answer wraps to fit this landscape screen.\n\n"
        "Turn the knob to read earlier or later lines. Your place stays put as more text arrives.\n\n"
        "Open Reply Audio to turn spoken replies back on. Volume is saved separately.\n\n"
        "This is the current reply. Starting a new question replaces it.");
    for (int page = 0; page < 2; ++page) {
        if (page) for (int i = 0; i < 7; ++i) r.app.on_button(hg::Button::Down, true);
        std::string path = directory + (page ? "/reply-scrolled.ppm" : "/reply-top.ppm");
        FILE* file = std::fopen(path.c_str(), "wb");
        assert(file);
        std::fprintf(file, "P6\n320 170\n255\n");
        for (uint16_t pixel : r.fake.pixels) {
            pixel = static_cast<uint16_t>((pixel >> 8) | (pixel << 8));
            const uint8_t rgb[] = {static_cast<uint8_t>(((pixel >> 11) & 31) * 255 / 31),
                                   static_cast<uint8_t>(((pixel >> 5) & 63) * 255 / 63),
                                   static_cast<uint8_t>((pixel & 31) * 255 / 31)};
            assert(std::fwrite(rgb, 1, 3, file) == 3);
        }
        std::fclose(file);
    }
}

#ifndef HERMES_BOARD_TEST
int main(int argc, char** argv) {
    assert(argc >= 2);
    const std::string test = argv[1];
    if (test == "reading") reading();
    else if (test == "wrapping") wrapping();
    else if (test == "voice") voice();
    else if (test == "overlays") overlays();
    else if (test == "screenshots" && argc == 3) screenshots(argv[2]);
    else return 1;
}
#endif
