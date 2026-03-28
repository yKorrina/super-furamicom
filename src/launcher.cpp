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
constexpr int kRomRowHeight = 15;
constexpr int kVisibleRomRows = 8;
constexpr int kPrettyGlyphWidth = 5;
constexpr int kPrettyGlyphHeight = 7;
constexpr int kPrettyGlyphAdvance = 6;
constexpr const char* kLauncherSettingsPath = "launcher.cfg";

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

struct PrettyGlyph {
    char ch;
    const char* rows[kPrettyGlyphHeight];
};

static const PrettyGlyph PRETTY_FONT[] = {
    {'A', {" ### ","#   #","#   #","#####","#   #","#   #","#   #"}},
    {'B', {"#### ","#   #","#   #","#### ","#   #","#   #","#### "}},
    {'C', {" ### ","#   #","#    ","#    ","#    ","#   #"," ### "}},
    {'D', {"#### ","#   #","#   #","#   #","#   #","#   #","#### "}},
    {'E', {"#####","#    ","#    ","#### ","#    ","#    ","#####"}},
    {'F', {"#####","#    ","#    ","#### ","#    ","#    ","#    "}},
    {'G', {" ####","#    ","#    ","# ###","#   #","#   #"," ####"}},
    {'H', {"#   #","#   #","#   #","#####","#   #","#   #","#   #"}},
    {'I', {"#####","  #  ","  #  ","  #  ","  #  ","  #  ","#####"}},
    {'J', {"#####","    #","    #","    #","#   #","#   #"," ### "}},
    {'K', {"#   #","#  # ","# #  ","##   ","# #  ","#  # ","#   #"}},
    {'L', {"#    ","#    ","#    ","#    ","#    ","#    ","#####"}},
    {'M', {"#   #","## ##","# # #","# # #","#   #","#   #","#   #"}},
    {'N', {"#   #","##  #","# # #","#  ##","#   #","#   #","#   #"}},
    {'O', {" ### ","#   #","#   #","#   #","#   #","#   #"," ### "}},
    {'P', {"#### ","#   #","#   #","#### ","#    ","#    ","#    "}},
    {'Q', {" ### ","#   #","#   #","#   #","# # #","#  ##"," ####"}},
    {'R', {"#### ","#   #","#   #","#### ","# #  ","#  # ","#   #"}},
    {'S', {" ####","#    ","#    "," ### ","    #","    #","#### "}},
    {'T', {"#####","  #  ","  #  ","  #  ","  #  ","  #  ","  #  "}},
    {'U', {"#   #","#   #","#   #","#   #","#   #","#   #"," ### "}},
    {'V', {"#   #","#   #","#   #","#   #","#   #"," # # ","  #  "}},
    {'W', {"#   #","#   #","#   #","# # #","# # #","## ##","#   #"}},
    {'X', {"#   #","#   #"," # # ","  #  "," # # ","#   #","#   #"}},
    {'Y', {"#   #","#   #"," # # ","  #  ","  #  ","  #  ","  #  "}},
    {'Z', {"#####","    #","   # ","  #  "," #   ","#    ","#####"}},
    {'0', {" ### ","#   #","#  ##","# # #","##  #","#   #"," ### "}},
    {'1', {"  #  "," ##  ","  #  ","  #  ","  #  ","  #  ","#####"}},
    {'2', {" ### ","#   #","    #","   # ","  #  "," #   ","#####"}},
    {'3', {" ### ","#   #","    #"," ### ","    #","#   #"," ### "}},
    {'4', {"   # ","  ## "," # # ","#  # ","#####","   # ","   # "}},
    {'5', {"#####","#    ","#    ","#### ","    #","#   #"," ### "}},
    {'6', {" ### ","#   #","#    ","#### ","#   #","#   #"," ### "}},
    {'7', {"#####","    #","   # ","  #  "," #   "," #   "," #   "}},
    {'8', {" ### ","#   #","#   #"," ### ","#   #","#   #"," ### "}},
    {'9', {" ### ","#   #","#   #"," ####","    #","#   #"," ### "}},
    {'.', {"     ","     ","     ","     ","     "," ##  "," ##  "}},
    {'/', {"    #","   # ","   # ","  #  "," #   "," #   ","#    "}},
    {'-', {"     ","     ","     ","#####","     ","     ","     "}},
    {':', {"     "," ##  "," ##  ","     "," ##  "," ##  ","     "}},
    {'+', {"     ","  #  ","  #  ","#####","  #  ","  #  ","     "}},
    {',', {"     ","     ","     ","     "," ##  "," ##  "," #   "}},
    {'!', {"  #  ","  #  ","  #  ","  #  ","  #  ","     ","  #  "}},
    {'?', {" ### ","#   #","    #","   # ","  #  ","     ","  #  "}},
    {'(', {"   # ","  #  "," #   "," #   "," #   ","  #  ","   # "}},
    {')', {" #   ","  #  ","   # ","   # ","   # ","  #  "," #   "}},
    {' ', {"     ","     ","     ","     ","     ","     ","     "}},
};

enum class LauncherView {
    Main,
    Controls,
};

enum class LauncherTheme : uint8_t {
    Midnight = 0,
    Aurora = 1,
    Ember = 2,
};

enum class MainFocus {
    RomList,
    Visualizer,
    Theme,
    Controls,
    Start,
    Quit,
};

struct LauncherPalette {
    const char* name;
    uint32_t bg_top;
    uint32_t bg_bottom;
    uint32_t bg_diag;
    uint32_t bg_grid;
    uint32_t top_bar;
    uint32_t panel_fill;
    uint32_t panel_border;
    uint32_t panel_border_focus;
    uint32_t options_fill;
    uint32_t options_border;
    uint32_t row_fill;
    uint32_t row_active;
    uint32_t row_idle_border;
    uint32_t option_fill;
    uint32_t option_active;
    uint32_t option_border;
    uint32_t title;
    uint32_t header;
    uint32_t text;
    uint32_t text_soft;
    uint32_t status;
    uint32_t rom_status;
    uint32_t warning;
    uint32_t shadow;
    uint32_t check_fill;
};

constexpr std::array<LauncherPalette, 3> kLauncherPalettes = {{
    {
        "MIDNIGHT",
        0xFF081018u, 0xFF112337u, 0xFF1A2733u, 0xFF12202Bu,
        0xFF071015u, 0xFF141E26u, 0xFF5E8395u, 0xFFFFE7A0u,
        0xFF17222Bu, 0xFF62889Au, 0xFF1E2C36u, 0xFF2D5E6Fu,
        0xFF88B3C7u, 0xFF1D2A33u, 0xFF2F5160u, 0xFF7BA1B4u,
        0xFFFFE7A0u, 0xFF9DCEE0u, 0xFFFFFFFFu, 0xFFD7EEF7u,
        0xFFFFAA7Au, 0xFFCAEFBAu, 0xFFFFB18Au, 0xFF071018u, 0xFFFFE7A0u,
    },
    {
        "AURORA",
        0xFF091310u, 0xFF183A31u, 0xFF1E3A33u, 0xFF122C26u,
        0xFF08120Fu, 0xFF15251Fu, 0xFF74A88Eu, 0xFFF5FFB9u,
        0xFF162721u, 0xFF82B79Bu, 0xFF22352Du, 0xFF3A6E5Du,
        0xFFA2D3B8u, 0xFF203229u, 0xFF345F51u, 0xFF86BBA3u,
        0xFFF5FFB9u, 0xFFB0F0DAu, 0xFFFFFFFFu, 0xFFDDF7ECu,
        0xFFFFC78Fu, 0xFFD2F5B8u, 0xFFFFC29Au, 0xFF09130Fu, 0xFFF5FFB9u,
    },
    {
        "EMBER",
        0xFF140B09u, 0xFF35211Au, 0xFF3A241Cu, 0xFF271711u,
        0xFF160A08u, 0xFF241612u, 0xFFCC8C5Du, 0xFFFFE2A3u,
        0xFF281A16u, 0xFFD1986Au, 0xFF38241Cu, 0xFF7A4A34u,
        0xFFE6B184u, 0xFF2F1E18u, 0xFF6B4130u, 0xFFDAA077u,
        0xFFFFD68Eu, 0xFFFFC9A0u, 0xFFFFFFFFu, 0xFFFFECDCu,
        0xFFFFAD80u, 0xFFE6F3B0u, 0xFFFFC09Bu, 0xFF140A08u, 0xFFFFE2A3u,
    },
}};

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

const PrettyGlyph* findPrettyGlyph(char ch) {
    for (const PrettyGlyph& glyph : PRETTY_FONT) {
        if (glyph.ch == ch) return &glyph;
    }
    return nullptr;
}

void drawChar(uint32_t* pixels, int stride, int height, int cx, int cy, char ch, uint32_t color, int scale = 1) {
    scale = std::max(1, scale);
    if (const PrettyGlyph* glyph = findPrettyGlyph(ch)) {
        for (int y = 0; y < kPrettyGlyphHeight; y++) {
            for (int x = 0; x < kPrettyGlyphWidth; x++) {
                if (glyph->rows[y][x] == ' ') continue;
                for (int sy = 0; sy < scale; sy++) {
                    for (int sx = 0; sx < scale; sx++) {
                        const int px = cx + x * scale + sx;
                        const int py = cy + y * scale + sy;
                        if (px >= 0 && px < stride && py >= 0 && py < height) {
                            pixels[py * stride + px] = color;
                        }
                    }
                }
            }
        }
        return;
    }

    if (ch < 32 || ch > 127) return;
    const uint32_t glyph = MINI_FONT[ch - 32];
    for (int y = 0; y < 6; y++) {
        for (int x = 0; x < 4; x++) {
            if (((glyph >> (23 - (y * 4 + x))) & 1u) == 0) continue;
            for (int sy = 0; sy < scale; sy++) {
                for (int sx = 0; sx < scale; sx++) {
                    const int px = cx + x * scale + sx;
                    const int py = cy + y * scale + sy;
                    if (px >= 0 && px < stride && py >= 0 && py < height) {
                        pixels[py * stride + px] = color;
                    }
                }
            }
        }
    }
}

void drawText(uint32_t* pixels, int x, int y, std::string_view text, uint32_t color, int scale = 1) {
    for (std::size_t i = 0; i < text.size(); i++) {
        drawChar(pixels, kMenuWidth, kMenuHeight, x + (int)i * kPrettyGlyphAdvance * scale, y, text[i], color, scale);
    }
}

void drawTextShadow(uint32_t* pixels,
                    int x,
                    int y,
                    std::string_view text,
                    uint32_t color,
                    int scale = 1,
                    uint32_t shadow = 0xFF071018u) {
    drawText(pixels, x + scale, y + scale, text, shadow, scale);
    drawText(pixels, x, y, text, color, scale);
}

int textWidth(std::string_view text, int scale = 1) {
    if (text.empty()) return 0;
    return (int)text.size() * kPrettyGlyphAdvance * scale - scale;
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

uint32_t lerpColor(uint32_t lhs, uint32_t rhs, int index, int count) {
    if (count <= 0) return lhs;

    auto lerp = [index, count](int a, int b) {
        return a + ((b - a) * index) / count;
    };

    const int lr = (lhs >> 16) & 0xFF;
    const int lg = (lhs >> 8) & 0xFF;
    const int lb = lhs & 0xFF;
    const int rr = (rhs >> 16) & 0xFF;
    const int rg = (rhs >> 8) & 0xFF;
    const int rb = rhs & 0xFF;
    return 0xFF000000u
        | ((uint32_t)lerp(lr, rr) << 16)
        | ((uint32_t)lerp(lg, rg) << 8)
        | (uint32_t)lerp(lb, rb);
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

const LauncherPalette& paletteForTheme(LauncherTheme theme) {
    const std::size_t index = std::min<std::size_t>((std::size_t)theme, kLauncherPalettes.size() - 1);
    return kLauncherPalettes[index];
}

std::string_view launcherThemeName(LauncherTheme theme) {
    return paletteForTheme(theme).name;
}

LauncherTheme nextLauncherTheme(LauncherTheme theme) {
    const std::size_t index = ((std::size_t)theme + 1) % kLauncherPalettes.size();
    return (LauncherTheme)index;
}

bool parseLauncherTheme(const std::string& text, LauncherTheme& theme) {
    const std::string upper = upperCopy(trimLine(text));
    for (std::size_t i = 0; i < kLauncherPalettes.size(); i++) {
        if (upper == kLauncherPalettes[i].name) {
            theme = (LauncherTheme)i;
            return true;
        }
    }
    return false;
}

bool LoadLauncherTheme(const std::string& path, LauncherTheme& theme) {
    theme = LauncherTheme::Midnight;

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
        if (key == "THEME" && parseLauncherTheme(value, theme)) {
            return true;
        }
    }

    return false;
}

bool SaveLauncherTheme(const std::string& path, LauncherTheme theme) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# Super Furamicom launcher settings\n";
    out << "THEME=" << launcherThemeName(theme) << "\n";
    return (bool)out;
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
    SDL_Rect rom_panel{12, 34, 152, 154};
    SDL_Rect rom_list{18, 58, 140, kVisibleRomRows * kRomRowHeight};
    SDL_Rect options_panel{168, 34, 80, 154};
    SDL_Rect visualize_box{174, 50, 68, 16};
    SDL_Rect visualize_check{174, 53, 10, 10};
    SDL_Rect theme_button{174, 82, 68, 16};
    SDL_Rect controls_button{174, 106, 68, 18};
    SDL_Rect start_button{174, 132, 68, 18};
    SDL_Rect quit_button{174, 158, 68, 18};
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
                  LauncherTheme theme,
                  const std::string& status) {
    const LauncherPalette& palette = paletteForTheme(theme);
    for (int y = 0; y < kMenuHeight; y++) {
        const uint32_t color = lerpColor(palette.bg_top, palette.bg_bottom, y, std::max(1, kMenuHeight - 1));
        for (int x = 0; x < kMenuWidth; x++) {
            uint32_t pixel = color;
            if (((x + y) & 0x0F) == 0) {
                pixel = palette.bg_diag;
            } else if ((x % 32) == 0) {
                pixel = palette.bg_grid;
            }
            pixels[(std::size_t)y * kMenuWidth + x] = pixel;
        }
    }

    const MainLayout layout;
    drawRect(pixels, 0, 0, kMenuWidth, 28, palette.top_bar);
    drawRect(pixels, layout.rom_panel.x, layout.rom_panel.y, layout.rom_panel.w, layout.rom_panel.h, palette.panel_fill);
    drawOutline(pixels, layout.rom_panel.x, layout.rom_panel.y, layout.rom_panel.w, layout.rom_panel.h,
                focus == MainFocus::RomList ? palette.panel_border_focus : palette.panel_border);
    drawRect(pixels, layout.options_panel.x, layout.options_panel.y, layout.options_panel.w, layout.options_panel.h,
             palette.options_fill);
    drawOutline(pixels, layout.options_panel.x, layout.options_panel.y, layout.options_panel.w, layout.options_panel.h,
                palette.options_border);

    drawTextShadow(pixels, 14, 7, "SUPER FURAMICOM", palette.title, 2, palette.shadow);
    drawTextShadow(pixels, 18, 25, "ROM LIBRARY", palette.header, 1, palette.shadow);
    drawTextShadow(pixels, 176, 25, "OPTIONS", palette.header, 1, palette.shadow);

    for (int row = 0; row < kVisibleRomRows; row++) {
        const int rom_index = rom_scroll + row;
        const int y = layout.rom_list.y + row * kRomRowHeight;
        const bool selected = rom_index == selected_rom && rom_index < (int)rom_paths.size();
        drawRect(pixels, layout.rom_list.x - 3, y - 2, layout.rom_list.w + 2, 13, palette.row_fill);
        if (selected) {
            drawRect(pixels, layout.rom_list.x - 3, y - 2, layout.rom_list.w + 2, 13,
                     focus == MainFocus::RomList ? palette.row_active : palette.option_fill);
            drawOutline(pixels, layout.rom_list.x - 3, y - 2, layout.rom_list.w + 2, 13,
                        focus == MainFocus::RomList ? palette.panel_border_focus : palette.row_idle_border);
        }

        if (rom_index < (int)rom_paths.size()) {
            const std::string label = clipText(romLabel(rom_paths[(std::size_t)rom_index]), 23);
            drawTextShadow(pixels,
                           layout.rom_list.x,
                           y + 1,
                           label,
                           selected ? palette.text : palette.text_soft,
                           1,
                           palette.shadow);
        }
    }

    if (rom_paths.empty()) {
        drawTextShadow(pixels, 26, 94, "NO ROMS FOUND", palette.warning, 2, palette.shadow);
        drawTextShadow(pixels, 28, 122, "DROP .SFC OR .SMC FILES INTO /ROMS", palette.header, 1, palette.shadow);
    }

    auto drawButton = [&](const SDL_Rect& rect, MainFocus id, std::string_view label) {
        drawRect(pixels, rect.x, rect.y, rect.w, rect.h, focus == id ? palette.option_active : palette.option_fill);
        drawOutline(pixels, rect.x, rect.y, rect.w, rect.h,
                    focus == id ? palette.panel_border_focus : palette.option_border);
        drawTextShadow(pixels,
                       rect.x + std::max(4, (rect.w - textWidth(label)) / 2),
                       rect.y + std::max(4, (rect.h - kPrettyGlyphHeight) / 2),
                       label,
                       palette.text,
                       1,
                       palette.shadow);
    };

    drawTextShadow(pixels, 188, 52, "VISUALIZER", palette.text, 1, palette.shadow);
    drawRect(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
             focus == MainFocus::Visualizer ? palette.option_active : palette.option_fill);
    drawOutline(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
                focus == MainFocus::Visualizer ? palette.panel_border_focus : palette.option_border);
    if (visualize) {
        drawRect(pixels, layout.visualize_check.x + 2, layout.visualize_check.y + 2, 6, 6, palette.check_fill);
    }

    drawTextShadow(pixels, 192, 72, "THEME", palette.header, 1, palette.shadow);
    drawButton(layout.theme_button, MainFocus::Theme, launcherThemeName(theme));
    drawButton(layout.controls_button, MainFocus::Controls, "CONTROLS");
    drawButton(layout.start_button, MainFocus::Start, "START");
    drawButton(layout.quit_button, MainFocus::Quit, "QUIT");

    drawTextShadow(pixels, 12, 198, "ARROWS MOVE  ENTER SELECT  ESC QUIT", palette.header, 1, palette.shadow);
    if (!status.empty()) {
        drawTextShadow(pixels, 12, 212, clipText(upperCopy(status), 38), palette.status, 1, palette.shadow);
    } else if (!rom_paths.empty()) {
        drawTextShadow(pixels,
                       12,
                       212,
                       clipText(romLabel(rom_paths[(std::size_t)selected_rom]), 38),
                       palette.rom_status,
                       1,
                       palette.shadow);
    }
}

void drawControlsView(uint32_t* pixels,
                      const InputConfig& config,
                      int selected_binding,
                      bool waiting_for_key,
                      LauncherTheme theme,
                      const std::string& status) {
    const LauncherPalette& palette = paletteForTheme(theme);
    drawRect(pixels, 0, 0, kMenuWidth, kMenuHeight, palette.bg_top);
    drawRect(pixels, 10, 18, 236, 190, palette.panel_fill);
    drawOutline(pixels, 10, 18, 236, 190, palette.options_border);

    drawTextShadow(pixels, 16, 7, "CONTROL MAPPING", palette.title, 2, palette.shadow);
    drawTextShadow(pixels, 16, 28, "ENTER REBINDS  BACKSPACE DEFAULT", palette.header, 1, palette.shadow);
    drawTextShadow(pixels, 16, 36, "F9 RESET ALL   ESC BACK", palette.header, 1, palette.shadow);

    for (int i = 0; i < (int)kBindings.size(); i++) {
        const int y = 54 + i * 12;
        if (i == selected_binding) {
            drawRect(pixels, 18, y - 2, 220, 10, waiting_for_key ? 0xFF693117u : palette.row_active);
            drawOutline(pixels, 18, y - 2, 220, 10, palette.panel_border_focus);
        }

        drawTextShadow(pixels, 24, y, kBindings[(std::size_t)i].label, palette.text, 1, palette.shadow);
        drawTextShadow(pixels,
                       96,
                       y,
                       clipText(keyName(config.scancodes[(std::size_t)i]), 22),
                       i == selected_binding ? palette.title : palette.text_soft,
                       1,
                       palette.shadow);
    }

    if (waiting_for_key) {
        drawRect(pixels, 40, 176, 176, 20, 0xFF2A1410u);
        drawOutline(pixels, 40, 176, 176, 20, palette.warning);
        drawTextShadow(pixels, 54, 183, "PRESS A KEY...", palette.title, 1, palette.shadow);
    }

    if (!status.empty()) {
        drawTextShadow(pixels, 16, 212, clipText(upperCopy(status), 40), palette.status, 1, palette.shadow);
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
    LauncherTheme theme = LauncherTheme::Midnight;
    LoadLauncherTheme(kLauncherSettingsPath, theme);

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
                    else if (focus == MainFocus::Theme) {
                        theme = nextLauncherTheme(theme);
                        SaveLauncherTheme(kLauncherSettingsPath, theme);
                        status = std::string("THEME ") + std::string(launcherThemeName(theme));
                    }
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
                    } else if (focus == MainFocus::Theme) {
                        theme = nextLauncherTheme(theme);
                        SaveLauncherTheme(kLauncherSettingsPath, theme);
                        status = std::string("THEME ") + std::string(launcherThemeName(theme));
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
                    rom_scroll = std::clamp(selected_rom - (kVisibleRomRows / 2),
                                            0,
                                            std::max(0, (int)rom_paths.size() - kVisibleRomRows));
                }
            }

            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                const SDL_Point point = toLogicalPoint(renderer, event.button.x, event.button.y);
                if (point.x < 0 || point.y < 0) continue;

                if (view == LauncherView::Controls) {
                    if (pointInRect(point.x, point.y, SDL_Rect{10, 18, 236, 190})) {
                        for (int i = 0; i < (int)kBindings.size(); i++) {
                            const SDL_Rect row{18, 52 + i * 12, 220, 10};
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
                    const int row = (point.y - layout.rom_list.y) / kRomRowHeight;
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
                } else if (pointInRect(point.x, point.y, layout.theme_button)) {
                    focus = MainFocus::Theme;
                    theme = nextLauncherTheme(theme);
                    SaveLauncherTheme(kLauncherSettingsPath, theme);
                    status = std::string("THEME ") + std::string(launcherThemeName(theme));
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
            rom_scroll = std::clamp(selected_rom - (kVisibleRomRows / 2),
                                    0,
                                    std::max(0, (int)rom_paths.size() - kVisibleRomRows));
        }

        if (view == LauncherView::Main) {
            drawMainView(pixels.data(), rom_paths, selected_rom, rom_scroll, focus, result.visualize, theme, status);
        } else {
            drawControlsView(pixels.data(), result.input_config, selected_binding, waiting_for_key, theme, status);
        }

        SDL_UpdateTexture(texture, nullptr, pixels.data(), kMenuWidth * (int)sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    return result;
}
