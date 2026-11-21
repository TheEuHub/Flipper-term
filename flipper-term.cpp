// ============================================================================
// flipper-term — Flipper Zero desktop simulator (Linux Mint / X11, pure Xlib)
// ============================================================================
// Hobby simulator, NOT firmware or a hardware emulator. See README for
// accuracy and feature limitations. The embedded dolphin frames are from
// Flipper Devices firmware commit a8a24de3, GPL-3.0; see LICENSE.
// TODO: verify against repo: boot sequence and simulated app layouts.
//
// BUILD (Linux Mint, X11 session):
//   g++ --version                       # expect g++ 11/12/13 (Mint 21/22)
//   ls /usr/include/X11/Xlib.h          # if missing: sudo apt install
//                                       #   libx11-dev (no internet needed
//                                       #   if package cache present)
//   g++ -std=c++17 -O2 -Wall -o flipper-term flipper-term.cpp -lX11
//   ./flipper-term
//   Works on Mint X11 and via XWayland on Wayland sessions (DISPLAY set).
//
// FALLBACK (no X11 headers / no X server): framebuffer console.
//   g++ -std=c++17 -O2 -Wall -DUSE_FB0 -o flipper-term flipper-term.cpp
//   sudo ./flipper-term        # writes /dev/fb0, keyboard-only control
//
// CONTROLS
//   Mouse : click OK / Back / D-pad on the device body (press+release).
//   Keys  : Arrows = D-pad, Enter/Space = OK, Backspace/Esc = Back,
//           +/- = screen zoom (3x/4x/5x), Q = quit.
//   FocusOut pauses animations; FocusIn resumes.
//
// HOW TO ADD A NEW APP IN 5 STEPS (registry pattern):
//   1. Derive MyApp from App.
//   2. Implement handle_input(InputEvent, long long).
//   3. Implement draw(Canvas&, long long).
//   4. Add {"My App", [] { return std::make_unique<MyApp>(); }} to make_registry().
//   5. Rebuild; SceneManager owns the app. Store data under vm_sd if needed.
//
// LAYERING: Canvas/FrameBuffer -> PixelFont -> SceneManager -> Apps -> Compositor
//           -> Backend (X11 | fb0). 128x64 1-bit, nearest-neighbour scale.
// ============================================================================
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#ifndef USE_FB0
#include <X11/Xlib.h>
#include <X11/keysym.h>
#include <X11/Xutil.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <termios.h>
#endif

namespace fs = std::filesystem;

// ----------------------------- small utilities -----------------------------
static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}
static int parse_int(const std::string& s, int fallback) {
    int value = 0;
    auto result = std::from_chars(s.data(), s.data() + s.size(), value);
    return result.ec == std::errc{} && result.ptr == s.data() + s.size() ? value : fallback;
}
static std::string to_lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
    return s;
}
static bool read_text_file(const std::string& path, std::string& out) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream ss; ss << f.rdbuf(); out = ss.str(); return true;
}
static bool write_text_file(const std::string& path, const std::string& data) {
    std::error_code ec;
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::ofstream f(path, std::ios::trunc);
    if (!f) return false;
    f << data; return true;
}
static std::vector<std::string> list_files(const std::string& dir, const std::string& ext) {
    std::vector<std::string> r; std::error_code ec;
    if (!fs::is_directory(dir, ec)) return r;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_regular_file(ec)) {
            std::string n = e.path().filename().string();
            if (ext.empty() || (n.size() >= ext.size() &&
                to_lower(n.substr(n.size() - ext.size())) == to_lower(ext)))
                r.push_back(n);
        }
    }
    std::sort(r.begin(), r.end());
    return r;
}
static std::string hex_u64(uint64_t v, int bytes) {
    std::ostringstream ss; ss << std::hex;
    ss.width(bytes * 2); ss.fill('0'); ss << v; return ss.str();
}
static std::mt19937& rng() { static std::mt19937 r((unsigned)std::time(nullptr) ^ 0xF11u); return r; }
static int rnd(int lo, int hi) { std::uniform_int_distribution<int> d(lo, hi); return d(rng()); }

// ----------------------------- FrameBuffer -----------------------------
// 128x64 1-bit framebuffer. Pixel 1 = black ink on white paper (device LCD).
struct FrameBuffer {
    static constexpr int W = 128, H = 64;
    std::array<uint8_t, (W * H + 7) / 8> px{};
    void clear(bool ink = false) { px.fill(ink ? 0xFF : 0x00); }
    bool get(int x, int y) const {
        if (x < 0 || y < 0 || x >= W || y >= H) return false;
        int i = y * W + x; return (px[i >> 3] >> (i & 7)) & 1;
    }
    void set(int x, int y, bool ink) {
        if (x < 0 || y < 0 || x >= W || y >= H) return;
        int i = y * W + x;
        if (ink) px[i >> 3] |= uint8_t(1u << (i & 7));
        else px[i >> 3] &= uint8_t(~(1u << (i & 7)));
    }
    void flip_x() { for (int y = 0; y < H; ++y) for (int x = 0; x < W / 2; ++x) {
        bool a = get(x, y), b = get(W - 1 - x, y); set(x, y, b); set(W - 1 - x, y, a); } }
};

// Firmware fonts rasterized from the bundled u8g2 assets (ASCII subset).
#include "device_fonts.h"
struct PixelFont { int index; };

// ----------------------------- Canvas -----------------------------
// Mirrors firmware Canvas semantics: Color Black/White/Xor, draw modes.
enum class Color { Black, White, Xor };
struct Canvas {
    FrameBuffer& fb;
    Color color = Color::Black;
    explicit Canvas(FrameBuffer& f) : fb(f) {}
    void set_color(Color c) { color = c; }
    void set_pixel(int x, int y) {
        bool cur = fb.get(x, y);
        bool n = (color == Color::Black) ? true : (color == Color::White) ? false : !cur;
        fb.set(x, y, n);
    }
    void draw_line(int x1, int y1, int x2, int y2) {
        int dx = std::abs(x2 - x1), dy = -std::abs(y2 - y1);
        int sx = x1 < x2 ? 1 : -1, sy = y1 < y2 ? 1 : -1, err = dx + dy;
        for (;;) { set_pixel(x1, y1); if (x1 == x2 && y1 == y2) break;
            int e2 = 2 * err; if (e2 >= dy) { err += dy; x1 += sx; }
            if (e2 <= dx) { err += dx; y1 += sy; } }
    }
    void draw_rect(int x, int y, int w, int h) {
        if (w <= 0 || h <= 0) return;
        draw_line(x, y, x + w - 1, y); draw_line(x, y + h - 1, x + w - 1, y + h - 1);
        draw_line(x, y, x, y + h - 1); draw_line(x + w - 1, y, x + w - 1, y + h - 1);
    }
    void fill_rect(int x, int y, int w, int h) {
        for (int j = y; j < y + h; ++j) for (int i = x; i < x + w; ++i) set_pixel(i, j);
    }
    void invert_rect(int x, int y, int w, int h) {
        Color s = color; color = Color::Xor; fill_rect(x, y, w, h); color = s;
    }
    void clear(bool ink = false) { fb.clear(ink); }
    // 1-bit bitmap, row-major, msb-first per byte.
    void draw_bitmap(int x, int y, int w, int h, const uint8_t* data) {
        int stride = (w + 7) / 8;
        for (int j = 0; j < h; ++j) for (int i = 0; i < w; ++i) {
            uint8_t byte = data[j * stride + (i >> 3)];
            if (byte & (0x80 >> (i & 7))) set_pixel(x + i, y + j);
        }
    }
    int text_width(const std::string& s, const PixelFont& f) const {
        int width = 0;
        for (unsigned char ch : s) width += kDeviceFonts[f.index][ch >= 32 && ch <= 126 ? ch - 32 : 0].advance;
        return width;
    }
    void draw_string(int x, int y, const std::string& s, const PixelFont& f) {
        int cx = x;
        for (unsigned char ch : s) {
            const auto& glyph = kDeviceFonts[f.index][ch >= 32 && ch <= 126 ? ch - 32 : 0];
            for (int j = 0; j < 12; ++j)
                for (int i = 0; i < 16; ++i)
                    if (glyph.rows[j] & (1u << i)) set_pixel(cx + i, y + j - 1);
            cx += glyph.advance;
        }
    }
    void draw_string_centered(int y, const std::string& s, const PixelFont& f) {
        draw_string((FrameBuffer::W - text_width(s, f)) / 2, y, s, f);
    }
    void draw_string_right(int x_right, int y, const std::string& s, const PixelFont& f) {
        draw_string(x_right - text_width(s, f), y, s, f);
    }
    // Truncate with ellipsis to fit max_w pixels.
    std::string fit(const std::string& s, const PixelFont& f, int max_w) const {
        if (text_width(s, f) <= max_w) return s;
        std::string r = s;
        while (!r.empty() && text_width(r + "...", f) > max_w) r.pop_back();
        return r + "...";
    }
    void draw_string_fit(int x, int y, const std::string& s, const PixelFont& f, int max_w) {
        draw_string(x, y, fit(s, f, max_w), f);
    }
};
static const PixelFont kFontPrimary{0};
static const PixelFont kFontSmall{1};

// ----------------------------- Input -----------------------------
enum class InputEvent { Up, Down, Left, Right, Ok, Back, NoInput };
struct Input { InputEvent ev = InputEvent::NoInput; };

// ----------------------------- Settings / virtual SD -----------------------------
struct Settings {
    bool beep = true;
    int backlight_timeout = 30; // seconds, 0 = never
    int zoom = 4;               // 3/4/5
    int hiscore_snake = 0;
    int battery = 87;
    bool bluetooth = true;
    bool locked = false;
    std::string vm_dir = "vm_sd";
    void ensure_dirs() const {
        std::error_code ec;
        for (const char* d : {"lfrfid", "nfc", "infrared", "ibutton", "subghz",
                              "badusb", "scripts", "dolphin"}) {
            fs::create_directories(fs::path(vm_dir) / d, ec);
        }
        fs::create_directories(vm_dir, ec);
    }
    void load() {
        ensure_dirs();
        std::string t;
        if (!read_text_file(vm_dir + "/settings.txt", t)) return;
        std::istringstream ss(t); std::string line;
        while (std::getline(ss, line)) {
            line = trim(line);
            auto eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
            if (k == "beep") beep = (v == "1" || to_lower(v) == "true");
            else if (k == "backlight_timeout") backlight_timeout = clampi(parse_int(v, backlight_timeout), 0, 3600);
            else if (k == "zoom") zoom = clampi(parse_int(v, zoom), 3, 5);
            else if (k == "hiscore_snake") hiscore_snake = clampi(parse_int(v, hiscore_snake), 0, 1000000);
            else if (k == "battery") battery = clampi(parse_int(v, battery), 0, 100);
            else if (k == "bluetooth") bluetooth = (v == "1" || to_lower(v) == "true");
            else if (k == "locked") locked = (v == "1" || to_lower(v) == "true");
        }
    }
    void save() const {
        std::ostringstream ss;
        ss << "beep=" << (beep ? 1 : 0) << "\nbacklight_timeout=" << backlight_timeout
           << "\nzoom=" << zoom << "\nhiscore_snake=" << hiscore_snake
           << "\nbattery=" << battery << "\nbluetooth=" << (bluetooth ? 1 : 0)
           << "\nlocked=" << (locked ? 1 : 0) << "\n";
        write_text_file(vm_dir + "/settings.txt", ss.str());
    }
    void factory_reset() {
        std::error_code ec;
        fs::remove_all(vm_dir, ec);
        beep = true; backlight_timeout = 30; zoom = 4; hiscore_snake = 0;
        battery = 87; bluetooth = true; locked = false;
        ensure_dirs(); save();
    }
};
static Settings g_settings;
static bool g_led = false;       // activity LED (drawn on device body)
static bool g_beep_flag = false; // consumed by backend -> XBell
static void beep() { if (g_settings.beep) g_beep_flag = true; g_led = true; }
static void led_pulse() { g_led = true; }

// ----------------------------- Dolphin -----------------------------
// Firmware L1_Waves_128x50: four source frames, 2 fps.
#include "dolphin_frames.h"
struct Dolphin {
    void draw(Canvas& c, int, int, long long now_ms) {
        static const int order[] = {0, 1, 2, 3, 2, 3};
        c.set_color(Color::Black);
        c.draw_bitmap(0, 14, 128, 50, kWaves[order[(now_ms / 500) % 6]]);
    }
};
static Dolphin g_dolphin;

// ----------------------------- Status bar -----------------------------
static void draw_status_bar(Canvas& c) {
    c.set_color(Color::Black);
    c.fill_rect(0, 0, FrameBuffer::W, 9);
    c.set_color(Color::White);
    // Battery: outline + fill + percent text.
    int bx = 2, by = 1;
    c.draw_rect(bx, by, 16, 7);
    c.fill_rect(bx + 16, by + 2, 2, 3);
    int fill = (g_settings.battery * 13) / 100;
    if (fill > 0) c.fill_rect(bx + 2, by + 2, fill, 3);
    char buf[8]; std::snprintf(buf, sizeof buf, "%d%%", g_settings.battery);
    c.draw_string(bx + 20, 1, buf, kFontSmall);
    // Bluetooth rune (drawn) + lock + clock on the right.
    int rx = 78;
    if (g_settings.bluetooth) {
        c.draw_line(rx, 1, rx, 7); c.draw_line(rx, 1, rx + 3, 4);
        c.draw_line(rx, 7, rx + 3, 4); c.draw_line(rx + 3, 4, rx - 2, 2);
        c.draw_line(rx + 3, 4, rx - 2, 6);
    }
    if (g_settings.locked) {
        c.draw_rect(rx + 6, 4, 6, 4); c.draw_line(rx + 7, 4, rx + 7, 2);
        c.draw_line(rx + 11, 4, rx + 11, 2); c.draw_line(rx + 7, 2, rx + 11, 2);
    }
    std::time_t t = std::time(nullptr); std::tm tmv{};
    localtime_r(&t, &tmv);
    char clk[8]; std::snprintf(clk, sizeof clk, "%02d:%02d", tmv.tm_hour, tmv.tm_min);
    c.draw_string_right(126, 1, clk, kFontSmall);
    c.set_color(Color::Black);
    c.draw_line(0, 9, 127, 9);
}

// ----------------------------- Scene manager -----------------------------
struct App;
struct SceneManager {
    struct Entry { std::unique_ptr<App> app; int transition = 0; long long t0 = 0; };
    std::vector<Entry> stack;
    bool transitioning = false;
    void push(std::unique_ptr<App> a, int trans, long long now) {
        stack.push_back(Entry{std::move(a), trans, now});
        transitioning = (trans != 0);
    }
    void pop(long long now) {
        if (stack.size() > 1) { stack.pop_back(); transitioning = true;
            if (!stack.empty()) { stack.back().transition = 1; stack.back().t0 = now; } }
    }
    App* top() { return stack.empty() ? nullptr : stack.back().app.get(); }
    size_t depth() const { return stack.size(); }
};
static SceneManager g_scenes;

// ----------------------------- App base -----------------------------
struct App {
    virtual ~App() = default;
    virtual void handle_input(InputEvent ev, long long now_ms) = 0;
    virtual void draw(Canvas& c, long long now_ms) = 0;
    virtual const char* title() const { return "App"; }
    bool wants_pop = false;
};
static void draw_header(Canvas& c, const std::string& title) {
    draw_status_bar(c);
    c.set_color(Color::Black);
    c.draw_string(4, 12, title, kFontPrimary);
    c.draw_line(0, 21, 127, 21);
}
static void draw_footer(Canvas& c, const std::string& left, const std::string& right) {
    c.set_color(Color::Black);
    c.draw_line(0, 54, 127, 54);
    if (!left.empty()) c.draw_string(4, 56, left, kFontSmall);
    if (!right.empty()) c.draw_string_right(124, 56, right, kFontSmall);
}
static void draw_center_text(Canvas& c, int y, const std::string& s) {
    c.draw_string_centered(y, s, kFontPrimary);
}

// ----------------------------- Boot scene -----------------------------
struct BootApp : App {
    long long t0 = 0; bool started = false;
    const char* title() const override { return "Boot"; }
    void handle_input(InputEvent, long long) override {}
    void draw(Canvas& c, long long now) override {
        if (!started) { started = true; t0 = now; }
        long long el = now - t0;
        c.clear(false);
        c.set_color(Color::Black);
        if (el < 500) {
            // Flipper logo-ish: rounded square + "F" + progress dots.
            c.draw_rect(49, 14, 30, 30);
            c.draw_string(56, 24, "F", kFontPrimary);
            c.draw_string_centered(48, "Flipper Zero", kFontSmall);
        } else if (el < 1100) {
            c.draw_string_centered(20, "Flipper Zero", kFontPrimary);
            int p = (int)((el - 500) / 600.0 * 80.0);
            c.draw_rect(24, 34, 80, 8);
            if (p > 0) c.fill_rect(25, 35, p - 1, 6);
            c.draw_string_centered(46, "Loading...", kFontSmall);
        } else if (el < 1500) {
            g_dolphin.draw(c, 46, 24, now);
            c.draw_string_centered(46, "Hello!", kFontSmall);
        }
        // TODO: verify against repo: this is a simulated boot sequence.
    }
};

// ----------------------------- Menus and simulated applications -----------------------------
static void menu_draw(Canvas& c, const std::string& title,
                      const std::vector<std::string>& items, int selected) {
    c.clear(); draw_header(c, title);
    if (items.empty()) { draw_center_text(c, 32, "No files"); draw_footer(c, "Back", ""); return; }
    int first = std::max(0, selected - 1);
    first = std::min(first, std::max(0, (int)items.size() - 3));
    for (int i = first; i < (int)items.size() && i < first + 3; ++i) {
        int y = 23 + (i - first) * 10;
        if (i == selected) { c.set_color(Color::Black); c.fill_rect(1, y - 1, 125, 9); c.set_color(Color::White); }
        c.draw_string_fit(4, y, items[i], kFontSmall, 118);
        c.set_color(Color::Black);
    }
    draw_footer(c, "Back", "OK");
}
static void move_selection(InputEvent e, int& sel, int size) {
    if (!size) return;
    if (e == InputEvent::Down) sel = (sel + 1) % size;
    if (e == InputEvent::Up) sel = (sel + size - 1) % size;
}

struct DeviceApp : App {
    std::string name, page = "menu", detail, chosen;
    std::vector<std::string> options, files;
    int sel = 0, subsel = 0, preview = 0;
    long long started = 0, transmitting = 0;
    uint64_t id = 0;
    bool pin[8]{};
    explicit DeviceApp(std::string n) : name(std::move(n)) {
        if (name == "Sub-GHz") options = {"Read", "Read RAW", "Saved", "Add Manually", "Frequency Analyzer", "Region Information", "Radio Settings"};
        else if (name == "125 kHz RFID") options = {"Read", "Saved", "Add Manually", "Extra Actions"};
        else if (name == "NFC") options = {"Read", "Read Specific Card", "Saved", "Add Manually", "Extra Actions"};
        else if (name == "Infrared") options = {"Universal Remotes", "Learn New Remote", "Saved Remotes", "GPIO Settings"};
        else if (name == "GPIO") options = {"USB-UART Bridge", "USB-I2C", "SPI Terminal", "Pinout"};
        else if (name == "iButton") options = {"Read", "Saved", "Add Manually"};
        else if (name == "Bad USB") options = {"Scripts"};
    }
    const char* title() const override { return name.c_str(); }
    std::string folder() const {
        if (name == "Sub-GHz") return "subghz";
        if (name == "125 kHz RFID") return "lfrfid";
        if (name == "NFC") return "nfc";
        if (name == "Infrared") return "infrared";
        if (name == "iButton") return "ibutton";
        return "scripts";
    }
    std::string ext() const {
        if (name == "Sub-GHz") return ".sub";
        if (name == "125 kHz RFID") return ".rfid";
        if (name == "NFC") return ".nfc";
        if (name == "Infrared") return ".ir";
        if (name == "iButton") return ".ibtn";
        return ".txt";
    }
    void show_files() {
        files = list_files(g_settings.vm_dir + "/" + folder(), ext());
        if (name == "Bad USB") {
            auto extra = list_files("scripts", ".txt");
            for (const auto& f : extra) files.push_back("../scripts/" + f);
        }
        page = "files"; subsel = 0;
    }
    void save_id() {
        std::string idtext = hex_u64(id, name == "iButton" ? 8 : 5);
        std::string data = "Filetype: flipper-term simulated data\nVersion: 1\nProtocol: SIMULATED\nData: " + idtext + "\n";
        std::string path = g_settings.vm_dir + "/" + folder() + "/sim_" + idtext + ext();
        detail = write_text_file(path, data) ? "Saved to virtual SD" : "Save failed";
        page = "message";
    }
    void handle_input(InputEvent e, long long now) override {
        if (page == "menu") {
            move_selection(e, sel, (int)options.size());
            if (e == InputEvent::Back) { wants_pop = true; return; }
            if (e != InputEvent::Ok) return;
            chosen = options[sel]; subsel = 0;
            if (chosen == "Saved" || chosen == "Saved Remotes" || chosen == "Scripts") show_files();
            else if (name == "Infrared" && chosen == "Universal Remotes") page = "categories";
            else if (name == "GPIO") page = "gpio";
            else if (chosen == "Read" || chosen == "Read RAW" || chosen == "Frequency Analyzer" || chosen == "Read Specific Card" || chosen == "Learn New Remote") {
                page = "reading"; started = now;
                id = (uint64_t(rng()()) << 32) | rng()();
            } else { detail = "Not simulated"; page = "message"; }
        } else if (page == "files") {
            move_selection(e, subsel, (int)files.size());
            if (e == InputEvent::Back) page = "menu";
            else if (e == InputEvent::Ok && !files.empty()) {
                std::string path = g_settings.vm_dir + "/" + folder() + "/" + files[subsel];
                if (files[subsel].rfind("../scripts/", 0) == 0) path = files[subsel].substr(3);
                if (!read_text_file(path, detail)) detail = "Could not read file";
                chosen = files[subsel]; preview = 0; page = "preview";
            }
        } else if (page == "preview") {
            if (e == InputEvent::Back) page = "files";
            else if (e == InputEvent::Down) {
                int max_line = (int)std::min<size_t>(detail.size(), 1000000);
                preview = std::min(preview + 1, max_line);
            }
            else if (e == InputEvent::Up) preview = std::max(0, preview - 1);
            else if (e == InputEvent::Ok && name != "Bad USB") { transmitting = now + 600; led_pulse(); }
        } else if (page == "categories") {
            const int count = 5; move_selection(e, subsel, count);
            if (e == InputEvent::Back) page = "menu";
            else if (e == InputEvent::Ok) { chosen = std::vector<std::string>{"TV", "Audio", "Air Conditioner", "Projector", "Fan"}[subsel]; subsel = 0; page = "remote"; }
        } else if (page == "remote") {
            move_selection(e, subsel, 4);
            if (e == InputEvent::Back) page = "categories";
            else if (e == InputEvent::Ok) { transmitting = now + 500; led_pulse(); }
        } else if (page == "reading") {
            if (e == InputEvent::Back) page = "menu";
            else if (e == InputEvent::Ok && name != "Sub-GHz") { page = "result"; }
        } else if (page == "result") {
            if (e == InputEvent::Back) page = "menu";
            else if (e == InputEvent::Ok) save_id();
        } else if (page == "gpio") {
            if (e == InputEvent::Back) page = "menu";
            else if (e == InputEvent::Left) subsel = (subsel + 7) % 8;
            else if (e == InputEvent::Right) subsel = (subsel + 1) % 8;
            else if (e == InputEvent::Ok) pin[subsel] = !pin[subsel];
        } else if (page == "message") {
            if (e == InputEvent::Back || e == InputEvent::Ok) page = "menu";
        }
    }
    void draw(Canvas& c, long long now) override {
        if (page == "menu") { menu_draw(c, name, options, sel); return; }
        if (page == "files") { menu_draw(c, "Saved", files, subsel); return; }
        if (page == "categories") { menu_draw(c, "Universal Remotes", {"TV", "Audio", "Air Conditioner", "Projector", "Fan"}, subsel); return; }
        if (page == "remote") {
            menu_draw(c, chosen, {"Power", "Vol +", "Vol -", "Mute"}, subsel);
            if (now < transmitting) { c.fill_rect(26, 25, 78, 14); c.set_color(Color::White); c.draw_string(32, 28, "Simulated", kFontSmall); c.set_color(Color::Black); }
            return;
        }
        c.clear(); draw_header(c, name);
        if (page == "reading") {
            if (name == "Sub-GHz") {
                c.draw_string(3, 24, "433.920 MHz  SIM", kFontSmall);
                for (int x = 4; x < 124; x += 3) {
                    int h = 1 + (int)((x * 13 + now / 140 * 7) % 16);
                    c.draw_line(x, 49, x, 49 - h);
                }
                draw_footer(c, "Back", "");
            } else {
                draw_center_text(c, 25, "SIMULATED READ");
                draw_center_text(c, 38, "OK to detect"); draw_footer(c, "Back", "OK");
            }
        } else if (page == "result") {
            draw_center_text(c, 24, "Simulated ID:");
            draw_center_text(c, 37, hex_u64(id, 8)); draw_footer(c, "Back", "Save");
        } else if (page == "message") {
            draw_center_text(c, 32, detail); draw_footer(c, "Back", "OK");
        } else if (page == "gpio") {
            c.draw_string(4, 24, "SIMULATED PINOUT", kFontSmall);
            for (int i = 0; i < 8; ++i) {
                int x = 5 + i * 15; c.draw_rect(x, 37, 11, 9);
                if (pin[i]) c.fill_rect(x + 2, 39, 7, 5);
                if (subsel == i) c.draw_line(x, 49, x + 10, 49);
            }
            draw_footer(c, "< >", "Toggle");
        } else if (page == "preview") {
            c.draw_string_fit(2, 23, chosen, kFontSmall, 124);
            std::istringstream ss(detail); std::string line; int row = 0, shown = 0;
            while (std::getline(ss, line)) {
                if (row++ < preview) continue;
                if (shown == 2) break;
                c.draw_string_fit(2, 33 + shown * 10, line, kFontSmall, 124); ++shown;
            }
            draw_footer(c, "Up/Down", name == "Bad USB" ? "View only" : "Sim send");
            if (now < transmitting) { c.fill_rect(30, 35, 70, 10); c.set_color(Color::White); c.draw_string(33, 36, "SIM SEND", kFontSmall); c.set_color(Color::Black); }
        }
    }
};

struct SettingsApp : App {
    int sel = 0; bool confirm = false; std::string message;
    const char* title() const override { return "Settings"; }
    void handle_input(InputEvent e, long long) override {
        if (!message.empty()) { if (e == InputEvent::Ok || e == InputEvent::Back) message.clear(); return; }
        if (confirm) {
            if (e == InputEvent::Ok) { g_settings.factory_reset(); message = "Reset complete"; confirm = false; }
            if (e == InputEvent::Back) confirm = false;
            return;
        }
        move_selection(e, sel, 4);
        if (e == InputEvent::Back) wants_pop = true;
        else if (e == InputEvent::Ok || e == InputEvent::Left || e == InputEvent::Right) {
            if (sel == 0) g_settings.beep = !g_settings.beep;
            else if (sel == 1) { int& t = g_settings.backlight_timeout; t = t == 0 ? 15 : t == 15 ? 30 : t == 30 ? 60 : 0; }
            else if (sel == 2) g_settings.zoom = g_settings.zoom == 5 ? 3 : g_settings.zoom + 1;
            else if (sel == 3 && e == InputEvent::Ok) confirm = true;
            g_settings.save();
        }
    }
    void draw(Canvas& c, long long) override {
        if (confirm || !message.empty()) {
            c.clear(); draw_header(c, "Settings");
            draw_center_text(c, 27, confirm ? "Erase virtual SD?" : message);
            draw_footer(c, "Cancel", confirm ? "Erase" : "OK"); return;
        }
        menu_draw(c, "Settings", {std::string("Beep: ") + (g_settings.beep ? "On" : "Off"),
            "Dim: " + (g_settings.backlight_timeout ? std::to_string(g_settings.backlight_timeout) + "s" : "Never"),
            "Zoom: " + std::to_string(g_settings.zoom) + "x", "Factory Reset"}, sel);
    }
};

struct GamesApp : App {
    std::string mode = "menu";
    int sel = 0, dirx = 1, diry = 0, score = 0, cell = 0;
    std::vector<std::pair<int,int>> snake;
    std::pair<int,int> food{8,4};
    int board[9]{}; bool gameover = false;
    long long last = 0;
    const char* title() const override { return "Games"; }
    void new_food() {
        std::vector<std::pair<int,int>> free;
        for (int y = 0; y < 6; ++y) for (int x = 0; x < 16; ++x)
            if (std::find(snake.begin(), snake.end(), std::make_pair(x,y)) == snake.end()) free.push_back({x,y});
        if (free.empty()) { gameover = true; return; }
        food = free[rnd(0, (int)free.size() - 1)];
    }
    int winner() const {
        static const int lines[8][3] = {{0,1,2},{3,4,5},{6,7,8},{0,3,6},{1,4,7},{2,5,8},{0,4,8},{2,4,6}};
        for (const auto& l : lines) if (board[l[0]] && board[l[0]] == board[l[1]] && board[l[1]] == board[l[2]]) return board[l[0]];
        return std::all_of(board, board + 9, [](int x) { return x != 0; }) ? 3 : 0;
    }
    void handle_input(InputEvent e, long long now) override {
        if (mode == "menu") {
            move_selection(e, sel, 2);
            if (e == InputEvent::Back) wants_pop = true;
            if (e == InputEvent::Ok && sel == 0) { mode = "snake"; snake = {{3,3},{2,3},{1,3}}; dirx=1; diry=0; score=0; gameover=false; last=now; new_food(); }
            else if (e == InputEvent::Ok) { mode = "ttt"; std::fill(board, board+9, 0); cell=0; }
        } else if (mode == "snake") {
            if (e == InputEvent::Back) mode = "menu";
            else if (e == InputEvent::Up && diry != 1) {dirx=0; diry=-1;}
            else if (e == InputEvent::Down && diry != -1) {dirx=0; diry=1;}
            else if (e == InputEvent::Left && dirx != 1) {dirx=-1; diry=0;}
            else if (e == InputEvent::Right && dirx != -1) {dirx=1; diry=0;}
        } else {
            if (e == InputEvent::Back) { mode = "menu"; return; }
            if (e == InputEvent::Left) cell = (cell+8)%9;
            if (e == InputEvent::Right) cell = (cell+1)%9;
            if (e == InputEvent::Up) cell = (cell+6)%9;
            if (e == InputEvent::Down) cell = (cell+3)%9;
            if (e == InputEvent::Ok && !winner() && !board[cell]) {
                board[cell] = 1;
                if (!winner()) { std::vector<int> free; for(int i=0;i<9;++i) if(!board[i]) free.push_back(i); if (!free.empty()) board[free[rnd(0,(int)free.size()-1)]]=2; }
            }
        }
    }
    void draw(Canvas& c, long long now) override {
        if (mode == "menu") { menu_draw(c, "Games", {"Snake", "Tic-Tac-Toe"}, sel); return; }
        c.clear();
        if (mode == "snake") {
            if (!gameover && now-last >= 180) {
                last = now; auto head = std::make_pair(snake[0].first+dirx, snake[0].second+diry);
                bool grow = head == food;
                auto end = snake.end() - (grow ? 0 : 1);
                if (head.first<0 || head.first>=16 || head.second<0 || head.second>=6 || std::find(snake.begin(), end, head)!=end) gameover=true;
                else { snake.insert(snake.begin(),head); if (!grow) snake.pop_back(); else { ++score; new_food(); } }
                if (score > g_settings.hiscore_snake) { g_settings.hiscore_snake=score; g_settings.save(); }
            }
            c.draw_string(2, 1, "Snake " + std::to_string(score) + " Best " + std::to_string(g_settings.hiscore_snake), kFontSmall);
            c.draw_rect(0, 10, 98, 38);
            for (auto p : snake) c.fill_rect(2+p.first*6, 11+p.second*6, 5, 5);
            c.draw_rect(2+food.first*6, 11+food.second*6, 5, 5);
            if (gameover) c.draw_string(2, 50, "Game over - Back", kFontSmall);
            else c.draw_string(2, 50, "Arrows  Back:Exit", kFontSmall);
        } else {
            c.draw_string(2, 2, "Tic-Tac-Toe", kFontSmall);
            for (int i=0;i<9;++i) {
                int x=5+(i%3)*15, y=13+(i/3)*15;
                c.draw_rect(x,y,14,14);
                if(i==cell) c.draw_rect(x+1,y+1,12,12);
                if(board[i]) c.draw_string(x+4,y+3,board[i]==1?"X":"O",kFontSmall);
            }
            int w=winner();
            c.draw_string(56, 25, w==1?"You win":w==2?"AI wins":w==3?"Draw":"Your turn", kFontSmall);
            c.draw_string(56, 45, "Back", kFontSmall);
        }
    }
};

using Factory = std::function<std::unique_ptr<App>()>;
struct RegistryItem { std::string name; Factory factory; };
static std::vector<RegistryItem> make_registry() {
    std::vector<RegistryItem> r;
    for (const std::string n : {"Sub-GHz", "125 kHz RFID", "NFC", "Infrared", "GPIO", "iButton", "Bad USB"})
        r.push_back({n, [n] { return std::make_unique<DeviceApp>(n); }});
    r.push_back({"Games", [] { return std::make_unique<GamesApp>(); }});
    r.push_back({"Settings", [] { return std::make_unique<SettingsApp>(); }});
    return r;
}
static std::vector<RegistryItem> g_registry = make_registry();
struct MainMenu : App {
    int sel = 0;
    const char* title() const override { return "Menu"; }
    void handle_input(InputEvent e, long long now) override {
        move_selection(e, sel, (int)g_registry.size());
        if(e == InputEvent::Back) wants_pop = true;
        if(e == InputEvent::Ok) g_scenes.push(g_registry[sel].factory(), -1, now);
    }
    void draw(Canvas& c, long long) override {
        std::vector<std::string> labels; for (auto& i : g_registry) labels.push_back(i.name);
        menu_draw(c, "Main Menu", labels, sel);
    }
};
struct Desktop : App {
    const char* title() const override { return "Desktop"; }
    void handle_input(InputEvent e, long long now) override {
        if (e == InputEvent::Ok) g_scenes.push(std::make_unique<MainMenu>(), -1, now);
    }
    void draw(Canvas& c, long long now) override { c.clear(); g_dolphin.draw(c,0,0,now); draw_status_bar(c); }
};

// ----------------------------- Window / framebuffer backends -----------------------------
#include <chrono>
#include <thread>
#include <unistd.h>
#include <limits.h>
static long long clock_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void use_binary_directory() {
    char path[PATH_MAX + 1]; ssize_t n = readlink("/proc/self/exe", path, PATH_MAX);
    if (n <= 0) return;
    path[n] = 0; std::error_code ec;
    fs::current_path(fs::path(path).parent_path(), ec);
}

#ifndef USE_FB0
struct XBackend {
    Display* display = nullptr; Window window = 0; GC gc = nullptr; Atom wm_delete = 0;
    bool running = true, focused = true, mouse_down = false;
    InputEvent pressed = InputEvent::NoInput;
    int zoom = 4;
    XImage* image = nullptr;
    unsigned long white = 0, body = 0, dark = 0, edge = 0, orange = 0, red = 0, lcd = 0, ink = 0, gray = 0, dim_lcd = 0;
    unsigned long color(const char* name) {
        XColor c{}, exact{};
        if (XAllocNamedColor(display, DefaultColormap(display, DefaultScreen(display)), name, &c, &exact)) return c.pixel;
        return BlackPixel(display, DefaultScreen(display));
    }
    bool open() {
        display = XOpenDisplay(nullptr);
        if (!display) { std::fprintf(stderr,"flipper-term: cannot open display. Start an X11 session or use -DUSE_FB0.\n"); return false; }
        int screen = DefaultScreen(display); zoom = g_settings.zoom;
        window = XCreateSimpleWindow(display, RootWindow(display, screen), 0, 0, 168*zoom, 150*zoom, 0,
                                    BlackPixel(display,screen), WhitePixel(display,screen));
        if (!window) { std::fprintf(stderr,"flipper-term: cannot create X11 window.\n"); return false; }
        gc = XCreateGC(display, window, 0, nullptr);
        const char* title = "Flipper Zero — Simulator";
        XStoreName(display, window, title);
        Atom net_name = XInternAtom(display, "_NET_WM_NAME", False);
        Atom utf8 = XInternAtom(display, "UTF8_STRING", False);
        XChangeProperty(display, window, net_name, utf8, 8, PropModeReplace,
                        reinterpret_cast<const unsigned char*>(title), (int)std::strlen(title));
        XSelectInput(display, window, ExposureMask | KeyPressMask | KeyReleaseMask | ButtonPressMask | ButtonReleaseMask | FocusChangeMask | StructureNotifyMask);
        wm_delete = XInternAtom(display, "WM_DELETE_WINDOW", False);
        XSetWMProtocols(display, window, &wm_delete, 1);
        white=color("#f8f7f0"); body=color("#eeeae2"); dark=color("#34322f"); edge=color("#b7b2a8");
        orange=color("#ff841e"); red=color("#e56052"); lcd=color("#e5ad64"); ink=color("#2d241a"); gray=color("#d3cfc5"); dim_lcd=color("#957454");
        XMapWindow(display, window); XFlush(display); return true;
    }
    ~XBackend() {
        if (image) XDestroyImage(image);
        if (display && gc) XFreeGC(display,gc);
        if (display && window) XDestroyWindow(display,window);
        if (display) XCloseDisplay(display);
    }
    void fg(unsigned long c) { XSetForeground(display,gc,c); }
    void rect(int x,int y,int w,int h,unsigned long c) { fg(c); XFillRectangle(display,window,gc,x*zoom,y*zoom,w*zoom,h*zoom); }
    void oval(int x,int y,int w,int h,unsigned long c) { fg(c); XFillArc(display,window,gc,x*zoom,y*zoom,w*zoom,h*zoom,0,360*64); }
    void roundrect(int x,int y,int w,int h,int r,unsigned long c) {
        rect(x+r,y,w-2*r,h,c); rect(x,y+r,w,h-2*r,c);
        oval(x,y,2*r,2*r,c); oval(x+w-2*r,y,2*r,2*r,c);
        oval(x,y+h-2*r,2*r,2*r,c); oval(x+w-2*r,y+h-2*r,2*r,2*r,c);
    }
    InputEvent hit(int px,int py) const {
        int x=px/zoom, y=py/zoom;
        auto inside=[x,y](int cx,int cy,int r) { int dx=x-cx,dy=y-cy; return dx*dx+dy*dy<=r*r; };
        if (inside(37,119,12)) return InputEvent::Ok;
        if (inside(131,119,12)) return InputEvent::Back;
        if (x>=75 && x<93 && y>=99 && y<111) return InputEvent::Up;
        if (x>=75 && x<93 && y>=127 && y<140) return InputEvent::Down;
        if (x>=64 && x<78 && y>=111 && y<128) return InputEvent::Left;
        if (x>=90 && x<105 && y>=111 && y<128) return InputEvent::Right;
        return InputEvent::NoInput;
    }
    static InputEvent key(KeySym k) {
        switch(k) {
            case XK_Up: return InputEvent::Up; case XK_Down: return InputEvent::Down;
            case XK_Left: return InputEvent::Left; case XK_Right: return InputEvent::Right;
            case XK_Return: case XK_KP_Enter: case XK_space: return InputEvent::Ok;
            case XK_BackSpace: case XK_Escape: return InputEvent::Back;
            default: return InputEvent::NoInput;
        }
    }
    std::vector<InputEvent> pump() {
        std::vector<InputEvent> out;
        while(XPending(display)) {
            XEvent e; XNextEvent(display,&e);
            if(e.type == ClientMessage && (Atom)e.xclient.data.l[0] == wm_delete) running=false;
            else if(e.type == DestroyNotify) { window=0; running=false; }
            else if(e.type == FocusOut) { focused=false; pressed=InputEvent::NoInput; mouse_down=false; }
            else if(e.type == FocusIn) focused=true;
            else if(e.type == KeyPress) {
                KeySym k=XLookupKeysym(&e.xkey,0);
                if(k==XK_q || k==XK_Q) { running=false; continue; }
                if(k==XK_plus || k==XK_equal || k==XK_minus) {
                    g_settings.zoom=clampi(g_settings.zoom+(k==XK_minus?-1:1),3,5); g_settings.save(); continue;
                }
                InputEvent v=key(k); if(v!=InputEvent::NoInput) { pressed=v; out.push_back(v); }
            } else if(e.type == KeyRelease) { if (key(XLookupKeysym(&e.xkey,0))==pressed) pressed=InputEvent::NoInput; }
            else if(e.type == ButtonPress && e.xbutton.button==Button1) { mouse_down=true; pressed=hit(e.xbutton.x,e.xbutton.y); }
            else if(e.type == ButtonRelease && e.xbutton.button==Button1) {
                InputEvent v=hit(e.xbutton.x,e.xbutton.y);
                if(mouse_down && v!=InputEvent::NoInput && v==pressed) out.push_back(v);
                mouse_down=false; pressed=InputEvent::NoInput;
            }
        }
        return out;
    }
    void render(const FrameBuffer& fb, bool dim) {
        if(zoom != g_settings.zoom) { zoom=g_settings.zoom; XResizeWindow(display,window,168*zoom,150*zoom); }
        rect(0,0,168,150,gray);
        roundrect(3,2,162,146,12,edge); roundrect(4,2,160,143,11,body); roundrect(5,3,158,140,10,white);
        roundrect(14,12,140,72,4,dark); rect(20,16,128,64,lcd);
        if (!image || image->width != 128*zoom || image->height != 64*zoom) {
            if (image) XDestroyImage(image);
            int s=DefaultScreen(display);
            image=XCreateImage(display,DefaultVisual(display,s),DefaultDepth(display,s),ZPixmap,0,nullptr,128*zoom,64*zoom,32,0);
            if (!image) return;
            image->data=(char*)std::calloc((size_t)image->bytes_per_line,image->height);
            if (!image->data) { XDestroyImage(image); image=nullptr; return; }
        }
        unsigned long paper=dim ? dim_lcd : lcd;
        for(int y=0;y<64;++y) for(int x=0;x<128;++x) {
            unsigned long col=fb.get(x,y)?ink:paper;
            for(int yy=0;yy<zoom;++yy) for(int xx=0;xx<zoom;++xx) XPutPixel(image,x*zoom+xx,y*zoom+yy,col);
        }
        XPutImage(display,window,gc,image,0,0,20*zoom,16*zoom,128*zoom,64*zoom);
        rect(78,90,7,4,g_led?orange:edge);
        oval(24,106,26,26,edge); oval(25,107,24,24,pressed==InputEvent::Ok?orange:gray);
        oval(118,106,26,26,red); oval(120,108,22,22,pressed==InputEvent::Back?red:gray);
        fg(dark); XDrawString(display,window,gc,32*zoom,120*zoom,"OK",2); XDrawString(display,window,gc,124*zoom,120*zoom,"BACK",4);
        roundrect(75,98,18,43,4,edge); roundrect(63,110,43,19,4,edge);
        rect(77,100,14,39,orange); rect(65,112,39,15,orange);
        auto mark=[&](InputEvent e,int x,int y,const char* s) { if(pressed==e) oval(x-2,y-7,12,12,red); fg(dark); XDrawString(display,window,gc,x*zoom,y*zoom,s,1); };
        mark(InputEvent::Up,82,108,"^"); mark(InputEvent::Down,82,135,"v"); mark(InputEvent::Left,68,122,"<"); mark(InputEvent::Right,96,122,">");
        if (g_beep_flag) { XBell(display, 0); g_beep_flag=false; }
        XFlush(display);
    }
};
#else
#include <linux/fb.h>
#include <poll.h>
struct XBackend {
    int fd=-1; uint8_t* map=nullptr; size_t len=0; fb_var_screeninfo var{}; fb_fix_screeninfo fix{};
    bool running=true, focused=true, term_active=false; termios old{};
    bool open() {
        fd=::open("/dev/fb0", O_RDWR);
        if(fd<0 || ioctl(fd,FBIOGET_VSCREENINFO,&var)<0 || ioctl(fd,FBIOGET_FSCREENINFO,&fix)<0) {
            std::fprintf(stderr,"flipper-term: cannot open /dev/fb0; check permissions and framebuffer availability.\n"); return false;
        }
        if(var.bits_per_pixel!=16 && var.bits_per_pixel!=24 && var.bits_per_pixel!=32) {
            std::fprintf(stderr,"flipper-term: fb0 requires 16, 24 or 32 bits per pixel.\n"); return false;
        }
        auto valid_channel = [this](const fb_bitfield& c) {
            return c.length <= 16 && c.offset <= var.bits_per_pixel &&
                c.length <= var.bits_per_pixel - c.offset;
        };
        if (fix.type != FB_TYPE_PACKED_PIXELS || fix.visual != FB_VISUAL_TRUECOLOR ||
            !valid_channel(var.red) || !valid_channel(var.green) || !valid_channel(var.blue)) {
            std::fprintf(stderr,"flipper-term: unsupported framebuffer layout.\n"); return false;
        }
        len=fix.smem_len; map=(uint8_t*)mmap(nullptr,len,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if(map==MAP_FAILED) { map=nullptr; std::fprintf(stderr,"flipper-term: framebuffer mmap failed.\n"); return false; }
        if(isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO,&old)==0) {
            termios t=old; t.c_lflag &= ~(ICANON|ECHO); t.c_cc[VMIN]=0; t.c_cc[VTIME]=0;
            term_active=tcsetattr(STDIN_FILENO,TCSANOW,&t)==0;
        }
        return true;
    }
    ~XBackend() { if(term_active) tcsetattr(STDIN_FILENO,TCSANOW,&old); if(map) munmap(map,len); if(fd>=0) close(fd); }
    std::string keys;
    std::vector<InputEvent> pump() {
        std::vector<InputEvent> out; pollfd p{STDIN_FILENO,POLLIN,0};
        if(poll(&p,1,0)>0 && (p.revents&POLLIN)) { char b[64]; ssize_t n=read(STDIN_FILENO,b,sizeof b); if(n>0) keys.append(b,(size_t)n); }
        while(!keys.empty()) {
            unsigned char c=(unsigned char)keys[0]; InputEvent ev=InputEvent::NoInput;
            if(c==27 && keys.size()>=3 && keys[1]=='[') { char k=keys[2]; keys.erase(0,3); ev=k=='A'?InputEvent::Up:k=='B'?InputEvent::Down:k=='C'?InputEvent::Right:k=='D'?InputEvent::Left:InputEvent::NoInput; }
            else { keys.erase(0,1); if(c==27 || c==127 || c==8) ev=InputEvent::Back; else if(c=='\n'||c=='\r'||c==' ') ev=InputEvent::Ok; else if(c=='q'||c=='Q') running=false; else if(c=='w') ev=InputEvent::Up; else if(c=='s') ev=InputEvent::Down; else if(c=='a') ev=InputEvent::Left; else if(c=='d') ev=InputEvent::Right; }
            if(ev!=InputEvent::NoInput) out.push_back(ev);
        }
        return out;
    }
    uint32_t rgb(unsigned r,unsigned g,unsigned b) const {
        auto pack=[](unsigned v,const fb_bitfield& f) -> uint32_t { return f.length ? ((v * ((1u<<f.length)-1) / 255u) << f.offset) : 0; };
        return pack(r,var.red)|pack(g,var.green)|pack(b,var.blue);
    }
    void render(const FrameBuffer& fb, bool dim) {
        unsigned z=(unsigned)g_settings.zoom, bytes=var.bits_per_pixel/8;
        uint32_t ink=rgb(45,36,26), paper=dim?rgb(149,116,84):rgb(229,173,100);
        for(unsigned y=0;y<64*z && y<var.yres;++y) for(unsigned x=0;x<128*z && x<var.xres;++x) {
            size_t off=(size_t)(y+var.yoffset)*fix.line_length+(size_t)(x+var.xoffset)*bytes;
            if(off+bytes>len) continue;
            uint32_t value=fb.get((int)(x/z),(int)(y/z))?ink:paper;
            for(unsigned i=0;i<bytes;++i) map[off+i]=(uint8_t)(value>>(8*i));
        }
        g_beep_flag=false;
    }
};
#endif

int main() {
    use_binary_directory(); g_settings.load();
    XBackend backend; if (!backend.open()) return 1;
    g_scenes.push(std::make_unique<BootApp>(), 0, 0);
    FrameBuffer frame, previous; long long boot=clock_ms(), paused_at=0, paused_total=0, last_input=0, led_until=0, transition_at=0;
    size_t previous_depth=1;
    while(backend.running) {
        auto events=backend.pump(); long long raw=clock_ms();
        if (!backend.focused) { if (!paused_at) paused_at=raw; std::this_thread::sleep_for(std::chrono::milliseconds(33)); continue; }
        if (paused_at) { paused_total += raw-paused_at; paused_at=0; }
        long long now=raw-paused_total;
        if (!last_input) last_input=now;
        if (g_scenes.depth()==1 && dynamic_cast<BootApp*>(g_scenes.top()) && raw-boot-paused_total>=1500) {
            g_scenes.stack.clear(); g_scenes.push(std::make_unique<Desktop>(),0,now);
        }
        for(auto e:events) {
            last_input=now; beep(); led_until=now+180;
            App* app=g_scenes.top();
            if(app) { app->handle_input(e,now); if(app->wants_pop) g_scenes.pop(now); }
        }
        if (g_led && !led_until) led_until=now+180;
        if (now>=led_until) { g_led=false; led_until=0; }
        if (g_scenes.depth()!=previous_depth) { previous=frame; transition_at=now; previous_depth=g_scenes.depth(); }
        Canvas c(frame); if(g_scenes.top()) g_scenes.top()->draw(c,now);
        // Horizontal wipe on stack transitions.
        if (transition_at && now-transition_at<160) {
            int cut=clampi((int)((now-transition_at)*128/160),0,128);
            for(int y=0;y<64;++y) for(int x=0;x<128-cut;++x) frame.set(x,y,previous.get(x+cut,y));
        }
        bool dim=g_settings.backlight_timeout>0 && now-last_input>g_settings.backlight_timeout*1000LL;
        backend.render(frame,dim);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
    g_settings.save(); return 0;
}
