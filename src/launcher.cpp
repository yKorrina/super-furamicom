#include "launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>

namespace {
constexpr int kMenuWidth = 256;
constexpr int kMenuHeight = 224;

static const uint32_t MINI_FONT[] = {
    0x000000,0x448400,0xAA0000,0xAEAEA0,0x4E6E40,0xA26480,0x4A4AC0,0x440000,
    0x248840,0x844820,0x0A4A00,0x04E400,0x000480,0x00E000,0x000040,0x224880,
    0xEAAE00,0x4C4E00,0xE2E8E0,0xE2E2E0,0xAAE220,0xE8E2E0,0xE8EAE0,0xE22220,
    0xEAEAE0,0xEAE2E0,0x040400,0x040480,0x248420,0x0E0E00,0x842480,0xE24040,
    0x4AEA40,0x4AEAA0,0xCACAC0,0x688860,0xCAAAAC,0xE8C8E0,0xE8C880,0x68AA60,
    0xAAEAA0,0xE444E0,0x222A40,0xAACAA0,0x8888E0,0xAEEAA0,0xAEEEA0,0x4AAA40,
    0xCAC880,0x4AAE60,0xCACAA0,0x684260,0xE44440,0xAAAA40,0xAAA440,0xAAEEA0,
    0xAA4AA0,0xAA4440,0xE248E0,0xC88C00,0x884220,0xC22C00,0x4A0000,0x0000E0,
    0x840000,0x06AA60,0x88CAC0,0x068860,0x226A60,0x04E860,0x24E440,0x06A620,
    0x88CAA0,0x404440,0x20224C,0x88ACA0,0xC444E0,0x0AEEA0,0x0CAAA0,0x04AA40,
    0x0CAC80,0x06A620,0x068880,0x0E82E0,0x4E44E0,0x0AAA60,0x0AAA40,0x0AEEA0,
    0x0A44A0,0x0AA620,0x0E24E0,0x648460,0x444440,0xC424C0,0x05A000,0x000000,
};

enum class LauncherView {
    Main,
    Controls,
};

enum class MainFocus {
    RomList,
    Visualizer,
    Controls,
    Start,
    Quit,
};

constexpr std::array<InputBindingDefinition, kInputBindingCount> kBindings = {{
    {"B",      0x8000, SDL_SCANCODE_X},
    {"Y",      0x4000, SDL_SCANCODE_Z},
    {"SELECT", 0x2000, SDL_SCANCODE_BACKSPACE},
    {"START",  0x1000, SDL_SCANCODE_RETURN},
    {"UP",     0x0008, SDL_SCANCODE_UP},
    {"DOWN",   0x0004, SDL_SCANCODE_DOWN},
    {"LEFT",   0x0002, SDL_SCANCODE_LEFT},
    {"RIGHT",  0x0001, SDL_SCANCODE_RIGHT},
    {"A",      0x0080, SDL_SCANCODE_S},
    {"X",      0x0040, SDL_SCANCODE_A},
    {"L",      0x0800, SDL_SCANCODE_Q},
    {"R",      0x0400, SDL_SCANCODE_W},
}};

void drawChar(uint32_t* pixels, int stride, int height, int cx, int cy, char ch, uint32_t color) {
    if (ch < 32 || ch > 127) return;
    const uint32_t glyph = MINI_FONT[ch - 32];
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 4; x++) {
            if (((glyph >> (23 - (y * 4 + x))) & 1u) == 0) continue;
            const int px = cx + x;
            const int py = cy + y;
            if (px >= 0 && px < stride && py >= 0 && py < height) {
                pixels[py * stride + px] = color;
            }
        }
    }
}

void drawText(uint32_t* pixels, int x, int y, std::string_view text, uint32_t color) {
    for (std::size_t i = 0; i < text.size(); i++) {
        drawChar(pixels, kMenuWidth, kMenuHeight, x + (int)i * 5, y, text[i], color);
    }
}

void drawRect(uint32_t* pixels, int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    for (int py = 0; py < h; py++) {
        const int yy = y + py;
        if (yy < 0 || yy >= kMenuHeight) continue;
        for (int px = 0; px < w; px++) {
            const int xx = x + px;
            if (xx < 0 || xx >= kMenuWidth) continue;
            pixels[(std::size_t)yy * kMenuWidth + xx] = color;
        }
    }
}

void drawOutline(uint32_t* pixels, int x, int y, int w, int h, uint32_t color) {
    drawRect(pixels, x, y, w, 1, color);
    drawRect(pixels, x, y + h - 1, w, 1, color);
    drawRect(pixels, x, y, 1, h, color);
    drawRect(pixels, x + w - 1, y, 1, h, color);
}

std::string upperCopy(std::string text) {
    for (char& ch : text) {
        ch = (char)std::toupper((unsigned char)ch);
    }
    return text;
}

std::string keyName(SDL_Scancode scancode) {
    const char* name = SDL_GetScancodeName(scancode);
    if (!name || !*name) return "UNBOUND";
    return upperCopy(name);
}

std::string romLabel(const std::string& path) {
    return upperCopy(std::filesystem::path(path).filename().string());
}

std::string trimLine(const std::string& text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace((unsigned char)text[first])) first++;
    std::size_t last = text.size();
    while (last > first && std::isspace((unsigned char)text[last - 1])) last--;
    return text.substr(first, last - first);
}

std::string clipText(const std::string& text, int max_chars) {
    if ((int)text.size() <= max_chars) return text;
    if (max_chars <= 3) return text.substr(0, std::max(0, max_chars));
    return text.substr(0, max_chars - 3) + "...";
}

void assignBinding(InputConfig& config, int target_index, SDL_Scancode scancode) {
    if (target_index < 0 || target_index >= (int)kBindings.size()) return;
    for (int i = 0; i < (int)config.scancodes.size(); i++) {
        if (i != target_index && config.scancodes[i] == scancode) {
            std::swap(config.scancodes[i], config.scancodes[target_index]);
            return;
        }
    }
    config.scancodes[(std::size_t)target_index] = scancode;
}

SDL_Point toLogicalPoint(SDL_Renderer* renderer, int window_x, int window_y) {
    SDL_Rect viewport{};
    SDL_RenderGetViewport(renderer, &viewport);
    if (viewport.w <= 0 || viewport.h <= 0) return {-1, -1};
    if (window_x < viewport.x || window_x >= viewport.x + viewport.w ||
        window_y < viewport.y || window_y >= viewport.y + viewport.h) {
        return {-1, -1};
    }

    const int logical_x = (window_x - viewport.x) * kMenuWidth / viewport.w;
    const int logical_y = (window_y - viewport.y) * kMenuHeight / viewport.h;
    return {logical_x, logical_y};
}

struct MainLayout {
    SDL_Rect rom_panel{12, 24, 152, 170};
    SDL_Rect rom_list{18, 42, 140, 132};
    SDL_Rect visualize_box{182, 40, 50, 16};
    SDL_Rect visualize_check{182, 40, 10, 10};
    SDL_Rect controls_button{176, 72, 60, 18};
    SDL_Rect start_button{176, 100, 60, 18};
    SDL_Rect quit_button{176, 128, 60, 18};
};

bool pointInRect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

void drawMainView(uint32_t* pixels,
                  const std::vector<std::string>& rom_paths,
                  int selected_rom,
                  int rom_scroll,
                  MainFocus focus,
                  bool visualize,
                  const std::string& status) {
    for (int y = 0; y < kMenuHeight; y++) {
        const uint8_t shade = (uint8_t)(8 + (y * 20) / std::max(1, kMenuHeight - 1));
        const uint32_t color = 0xFF000000u | ((uint32_t)(shade / 2) << 16) | ((uint32_t)shade << 8) | (uint32_t)(shade + 8);
        for (int x = 0; x < kMenuWidth; x++) {
            pixels[(std::size_t)y * kMenuWidth + x] = color;
        }
    }

    const MainLayout layout;
    drawRect(pixels, layout.rom_panel.x, layout.rom_panel.y, layout.rom_panel.w, layout.rom_panel.h, 0xFF0C141B);
    drawOutline(pixels, layout.rom_panel.x, layout.rom_panel.y, layout.rom_panel.w, layout.rom_panel.h,
                focus == MainFocus::RomList ? 0xFFFFE7A0 : 0xFF476170);
    drawRect(pixels, 172, 24, 72, 170, 0xFF111A22);
    drawOutline(pixels, 172, 24, 72, 170, 0xFF506A79);

    drawText(pixels, 14, 10, "SUPER FURAMICOM", 0xFFFFE7A0);
    drawText(pixels, 15, 18, "ROMS", 0xFF88B3C7);
    drawText(pixels, 177, 18, "OPTIONS", 0xFF88B3C7);

    const int visible_rows = 12;
    for (int row = 0; row < visible_rows; row++) {
        const int rom_index = rom_scroll + row;
        const int y = layout.rom_list.y + row * 11;
        const bool selected = rom_index == selected_rom && rom_index < (int)rom_paths.size();
        if (selected) {
            drawRect(pixels, layout.rom_list.x - 2, y - 1, layout.rom_list.w, 9,
                     focus == MainFocus::RomList ? 0xFF214D62 : 0xFF1A3340);
        }

        if (rom_index < (int)rom_paths.size()) {
            drawText(pixels, layout.rom_list.x, y,
                     clipText(romLabel(rom_paths[(std::size_t)rom_index]), 27),
                     selected ? 0xFFFFFFFFu : 0xFFCEE7F0u);
        }
    }

    if (rom_paths.empty()) {
        drawText(pixels, 22, 90, "NO .SFC/.SMC FILES", 0xFFFFAA7A);
        drawText(pixels, 22, 100, "FOUND IN ROMS FOLDER", 0xFFFFAA7A);
    }

    const uint32_t option_fill = 0xFF19242C;
    const uint32_t option_active = 0xFF25404F;
    const uint32_t option_border = 0xFF5F879A;
    auto drawButton = [&](const SDL_Rect& rect, MainFocus id, std::string_view label) {
        drawRect(pixels, rect.x, rect.y, rect.w, rect.h, focus == id ? option_active : option_fill);
        drawOutline(pixels, rect.x, rect.y, rect.w, rect.h, focus == id ? 0xFFFFE7A0 : option_border);
        drawText(pixels, rect.x + 8, rect.y + 6, label, 0xFFFFFFFFu);
    };

    drawText(pixels, 196, 42, "VIS", 0xFFFFFFFFu);
    drawRect(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
             focus == MainFocus::Visualizer ? option_active : option_fill);
    drawOutline(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
                focus == MainFocus::Visualizer ? 0xFFFFE7A0 : option_border);
    if (visualize) {
        drawRect(pixels, layout.visualize_check.x + 2, layout.visualize_check.y + 2, 6, 6, 0xFFFFE7A0);
    }

    drawButton(layout.controls_button, MainFocus::Controls, "CONTROLS");
    drawButton(layout.start_button, MainFocus::Start, "START");
    drawButton(layout.quit_button, MainFocus::Quit, "QUIT");

    drawText(pixels, 12, 198, "ARROWS MOVE  ENTER SELECT  ESC QUIT", 0xFF93B6C1);
    if (!status.empty()) {
        drawText(pixels, 12, 208, clipText(upperCopy(status), 48), 0xFFFFAA7A);
    } else if (!rom_paths.empty()) {
        drawText(pixels, 12, 208, clipText(romLabel(rom_paths[(std::size_t)selected_rom]), 48), 0xFFBFE5A9);
    }
}

void drawControlsView(uint32_t* pixels,
                      const InputConfig& config,
                      int selected_binding,
                      bool waiting_for_key,
                      const std::string& status) {
    drawRect(pixels, 0, 0, kMenuWidth, kMenuHeight, 0xFF0B1218);
    drawRect(pixels, 10, 18, 236, 190, 0xFF101922);
    drawOutline(pixels, 10, 18, 236, 190, 0xFF5A7C8D);

    drawText(pixels, 16, 10, "CONTROL MAPPING", 0xFFFFE7A0);
    drawText(pixels, 16, 24, "ENTER REBINDS  BACKSPACE DEFAULT", 0xFF9BC0D0);
    drawText(pixels, 16, 32, "F9 RESET ALL   ESC BACK", 0xFF9BC0D0);

    for (int i = 0; i < (int)kBindings.size(); i++) {
        const int y = 48 + i * 12;
        if (i == selected_binding) {
            drawRect(pixels, 18, y - 2, 220, 10, waiting_for_key ? 0xFF693117 : 0xFF214D62);
            drawOutline(pixels, 18, y - 2, 220, 10, 0xFFFFE7A0);
        }

        drawText(pixels, 24, y, kBindings[(std::size_t)i].label, 0xFFFFFFFFu);
        drawText(pixels, 96, y,
                 clipText(keyName(config.scancodes[(std::size_t)i]), 26),
                 i == selected_binding ? 0xFFFFF7C5u : 0xFFCEE7F0u);
    }

    if (waiting_for_key) {
        drawRect(pixels, 40, 176, 176, 20, 0xFF2A1410);
        drawOutline(pixels, 40, 176, 176, 20, 0xFFFFB27A);
        drawText(pixels, 54, 183, "PRESS A KEY...", 0xFFFFE7A0);
    }

    if (!status.empty()) {
        drawText(pixels, 16, 212, clipText(upperCopy(status), 48), 0xFFFFAA7A);
    }
}
}

const std::array<InputBindingDefinition, kInputBindingCount>& GetInputBindingDefinitions() {
    return kBindings;
}

InputConfig DefaultInputConfig() {
    InputConfig config{};
    for (std::size_t i = 0; i < kBindings.size(); i++) {
        config.scancodes[i] = kBindings[i].default_scancode;
    }
    return config;
}

bool LoadInputConfig(const std::string& path, InputConfig& config) {
    config = DefaultInputConfig();

    std::ifstream in(path);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        line = trimLine(line);
        if (line.empty() || line[0] == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = upperCopy(trimLine(line.substr(0, eq)));
        const std::string value = trimLine(line.substr(eq + 1));
        const int scancode_value = std::atoi(value.c_str());
        if (scancode_value < 0) continue;

        for (std::size_t i = 0; i < kBindings.size(); i++) {
            if (key == kBindings[i].label) {
                config.scancodes[i] = (SDL_Scancode)scancode_value;
                break;
            }
        }
    }
    return true;
}

bool SaveInputConfig(const std::string& path, const InputConfig& config) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# Super Furamicom input mappings\n";
    for (std::size_t i = 0; i < kBindings.size(); i++) {
        out << kBindings[i].label << "=" << (int)config.scancodes[i] << "\n";
    }
    return (bool)out;
}

uint16_t MapInputConfigToPad(const uint8_t* keys, const InputConfig& config) {
    if (!keys) return 0;
    uint16_t pad = 0;
    for (std::size_t i = 0; i < kBindings.size(); i++) {
        const SDL_Scancode scancode = config.scancodes[i];
        if (scancode != SDL_SCANCODE_UNKNOWN && keys[scancode]) {
            pad |= kBindings[i].mask;
        }
    }
    return pad;
}

std::vector<std::string> DiscoverROMs(const std::string& root_dir) {
    std::vector<std::string> roms;
    const std::filesystem::path root(root_dir);
    if (!std::filesystem::exists(root)) return roms;

    for (const auto& entry : std::filesystem::directory_iterator(root)) {
        if (!entry.is_regular_file()) continue;
        const std::string ext = upperCopy(entry.path().extension().string());
        if (ext == ".SFC" || ext == ".SMC") {
            roms.push_back(std::filesystem::absolute(entry.path()).string());
        }
    }

    std::sort(roms.begin(), roms.end(), [](const std::string& lhs, const std::string& rhs) {
        return upperCopy(std::filesystem::path(lhs).filename().string()) <
               upperCopy(std::filesystem::path(rhs).filename().string());
    });
    return roms;
}

LauncherResult RunLauncher(SDL_Window* window,
                           SDL_Renderer* renderer,
                           SDL_Texture* texture,
                           const std::vector<std::string>& rom_paths,
                           const std::string& controls_path,
                           const InputConfig& initial_config,
                           bool visualize_default) {
    LauncherResult result{};
    result.input_config = initial_config;
    result.visualize = visualize_default;

    if (!window || !renderer || !texture) return result;

    std::array<uint32_t, kMenuWidth * kMenuHeight> pixels{};
    LauncherView view = LauncherView::Main;
    MainFocus focus = MainFocus::RomList;
    int selected_rom = rom_paths.empty() ? -1 : 0;
    int rom_scroll = 0;
    int selected_binding = 0;
    bool waiting_for_key = false;
    std::string status;
    uint32_t last_click_ticks = 0;
    int last_click_rom = -1;

    SDL_SetWindowTitle(window, "Super Furamicom Launcher");

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) {
                running = false;
                break;
            }

            if (event.type == SDL_KEYDOWN) {
                const SDL_Keycode key = event.key.keysym.sym;
                if (view == LauncherView::Controls && waiting_for_key) {
                    if (key == SDLK_ESCAPE) {
                        waiting_for_key = false;
                    } else {
                        assignBinding(result.input_config, selected_binding, event.key.keysym.scancode);
                        SaveInputConfig(controls_path, result.input_config);
                        waiting_for_key = false;
                        status = std::string("BOUND ") + kBindings[(std::size_t)selected_binding].label;
                    }
                    continue;
                }

                if (view == LauncherView::Controls) {
                    switch (key) {
                    case SDLK_ESCAPE:
                        view = LauncherView::Main;
                        status.clear();
                        break;
                    case SDLK_UP:
                        selected_binding = (selected_binding + (int)kBindings.size() - 1) % (int)kBindings.size();
                        break;
                    case SDLK_DOWN:
                        selected_binding = (selected_binding + 1) % (int)kBindings.size();
                        break;
                    case SDLK_RETURN:
                    case SDLK_SPACE:
                        waiting_for_key = true;
                        status = std::string("PRESS A KEY FOR ") + kBindings[(std::size_t)selected_binding].label;
                        break;
                    case SDLK_BACKSPACE:
                        result.input_config.scancodes[(std::size_t)selected_binding] =
                            kBindings[(std::size_t)selected_binding].default_scancode;
                        SaveInputConfig(controls_path, result.input_config);
                        status = std::string("RESET ") + kBindings[(std::size_t)selected_binding].label;
                        break;
                    case SDLK_F9:
                    case SDLK_DELETE:
                        result.input_config = DefaultInputConfig();
                        SaveInputConfig(controls_path, result.input_config);
                        status = "RESTORED DEFAULTS";
                        break;
                    default:
                        break;
                    }
                    continue;
                }

                switch (key) {
                case SDLK_ESCAPE:
                    running = false;
                    break;
                case SDLK_TAB:
                case SDLK_RIGHT:
                    if (focus == MainFocus::RomList) focus = MainFocus::Visualizer;
                    break;
                case SDLK_LEFT:
                    if (focus != MainFocus::RomList) focus = MainFocus::RomList;
                    break;
                case SDLK_UP:
                    if (focus == MainFocus::RomList) {
                        if (selected_rom > 0) selected_rom--;
                    } else {
                        focus = (MainFocus)std::max(1, (int)focus - 1);
                    }
                    break;
                case SDLK_DOWN:
                    if (focus == MainFocus::RomList) {
                        if (selected_rom + 1 < (int)rom_paths.size()) selected_rom++;
                    } else {
                        focus = (MainFocus)std::min((int)MainFocus::Quit, (int)focus + 1);
                    }
                    break;
                case SDLK_SPACE:
                    if (focus == MainFocus::Visualizer) result.visualize = !result.visualize;
                    break;
                case SDLK_RETURN:
                    if (focus == MainFocus::RomList || focus == MainFocus::Start) {
                        if (selected_rom >= 0 && selected_rom < (int)rom_paths.size()) {
                            result.launch_requested = true;
                            result.rom_path = rom_paths[(std::size_t)selected_rom];
                            running = false;
                        } else {
                            status = "SELECT A ROM FIRST";
                        }
                    } else if (focus == MainFocus::Visualizer) {
                        result.visualize = !result.visualize;
                    } else if (focus == MainFocus::Controls) {
                        view = LauncherView::Controls;
                        status.clear();
                    } else if (focus == MainFocus::Quit) {
                        running = false;
                    }
                    break;
                default:
                    break;
                }

                if (selected_rom >= 0) {
                    rom_scroll = std::clamp(selected_rom - 5, 0, std::max(0, (int)rom_paths.size() - 12));
                }
            }

            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                const SDL_Point point = toLogicalPoint(renderer, event.button.x, event.button.y);
                if (point.x < 0 || point.y < 0) continue;

                if (view == LauncherView::Controls) {
                    if (pointInRect(point.x, point.y, SDL_Rect{10, 18, 236, 190})) {
                        for (int i = 0; i < (int)kBindings.size(); i++) {
                            const SDL_Rect row{18, 46 + i * 12, 220, 10};
                            if (pointInRect(point.x, point.y, row)) {
                                selected_binding = i;
                                waiting_for_key = true;
                                status = std::string("PRESS A KEY FOR ") + kBindings[(std::size_t)i].label;
                                break;
                            }
                        }
                    } else {
                        view = LauncherView::Main;
                    }
                    continue;
                }

                const MainLayout layout;
                if (pointInRect(point.x, point.y, layout.rom_list)) {
                    const int row = (point.y - layout.rom_list.y) / 11;
                    const int rom_index = rom_scroll + row;
                    if (rom_index >= 0 && rom_index < (int)rom_paths.size()) {
                        focus = MainFocus::RomList;
                        selected_rom = rom_index;
                        const uint32_t now = SDL_GetTicks();
                        if (last_click_rom == rom_index && now - last_click_ticks < 350) {
                            result.launch_requested = true;
                            result.rom_path = rom_paths[(std::size_t)selected_rom];
                            running = false;
                        }
                        last_click_rom = rom_index;
                        last_click_ticks = now;
                    }
                } else if (pointInRect(point.x, point.y, layout.visualize_box)) {
                    focus = MainFocus::Visualizer;
                    result.visualize = !result.visualize;
                } else if (pointInRect(point.x, point.y, layout.controls_button)) {
                    focus = MainFocus::Controls;
                    view = LauncherView::Controls;
                } else if (pointInRect(point.x, point.y, layout.start_button)) {
                    focus = MainFocus::Start;
                    if (selected_rom >= 0 && selected_rom < (int)rom_paths.size()) {
                        result.launch_requested = true;
                        result.rom_path = rom_paths[(std::size_t)selected_rom];
                        running = false;
                    } else {
                        status = "SELECT A ROM FIRST";
                    }
                } else if (pointInRect(point.x, point.y, layout.quit_button)) {
                    running = false;
                }
            }
        }

        if (selected_rom >= 0) {
            rom_scroll = std::clamp(selected_rom - 5, 0, std::max(0, (int)rom_paths.size() - 12));
        }

        if (view == LauncherView::Main) {
            drawMainView(pixels.data(), rom_paths, selected_rom, rom_scroll, focus, result.visualize, status);
        } else {
            drawControlsView(pixels.data(), result.input_config, selected_binding, waiting_for_key, status);
        }

        SDL_UpdateTexture(texture, nullptr, pixels.data(), kMenuWidth * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    return result;
}
