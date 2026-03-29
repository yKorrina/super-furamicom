#include "launcher.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {
constexpr int kMenuWidth = 256;
constexpr int kMenuHeight = 224;
constexpr int kRomRowHeight = 15;
constexpr int kPrettyGlyphWidth = 5;
constexpr int kPrettyGlyphHeight = 7;
constexpr int kPrettyGlyphAdvance = 5;
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
    Cloud = 3,
    Harbor = 4,
    Grove = 5,
    Rose = 6,
    Sunset = 7,
};

enum class MainFocus {
    RomList,
    Visualizer,
    Manipulator,
    IntegerScale,
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

constexpr std::array<LauncherPalette, 8> kLauncherPalettes = {{
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
    {
        "CLOUD",
        0xFFF4F6F8u, 0xFFD7DCE2u, 0xFFE6EAEEu, 0xFFEDEFF2u,
        0xFFE7EBEFu, 0xFFF8FAFCu, 0xFF8B98A5u, 0xFF515E6Au,
        0xFFF0F3F6u, 0xFF95A1ADu, 0xFFF9FBFDu, 0xFFDCE3E9u,
        0xFF66717Du, 0xFFE6EBF0u, 0xFFC7D0D8u, 0xFF9AA6B2u,
        0xFF46515Bu, 0xFF5F6B77u, 0xFF24303Bu, 0xFF4F5A64u,
        0xFF52616Du, 0xFF6D7A86u, 0xFF7B8893u, 0xFFFFFFFFu, 0xFF5E6873u,
    },
    {
        "HARBOR",
        0xFF07131Du, 0xFF14344Du, 0xFF123244u, 0xFF0D2232u,
        0xFF081019u, 0xFF122131u, 0xFF67A9D8u, 0xFFFFF0B4u,
        0xFF162738u, 0xFF71B7E8u, 0xFF1B2F43u, 0xFF24557Du,
        0xFF87C7EEu, 0xFF1A2836u, 0xFF2C5F88u, 0xFF7AB9E4u,
        0xFFFFF0B4u, 0xFF96D5F3u, 0xFFFFFFFFu, 0xFFD7EFFAu,
        0xFFFFC287u, 0xFFC5F0CEu, 0xFFFFC89Fu, 0xFF08121Au, 0xFFFFE7A0u,
    },
    {
        "GROVE",
        0xFF0B1208u, 0xFF23361Du, 0xFF24361Eu, 0xFF182715u,
        0xFF0A1008u, 0xFF162117u, 0xFF8EBB76u, 0xFFFFF2B0u,
        0xFF1A281Bu, 0xFF9ACB81u, 0xFF223222u, 0xFF3B6A35u,
        0xFFB4DA98u, 0xFF1D2A1Bu, 0xFF4C7E43u, 0xFF96C17Eu,
        0xFFFFF2B0u, 0xFFBEE7A5u, 0xFFFFFFFFu, 0xFFE5F6D7u,
        0xFFFFC77Cu, 0xFFD9F0BAu, 0xFFFFD09Cu, 0xFF0A1008u, 0xFFFFF2B0u,
    },
    {
        "ROSE",
        0xFF170C14u, 0xFF45253Au, 0xFF47273Cu, 0xFF301A28u,
        0xFF160A12u, 0xFF24141Fu, 0xFFE59ABAu, 0xFFFFF0C8u,
        0xFF2A1825u, 0xFFF0A8C6u, 0xFF372132u, 0xFF7D4466u,
        0xFFF4BED5u, 0xFF2C1A26u, 0xFF8B5070u, 0xFFE0A2BFu,
        0xFFFFE5B6u, 0xFFF6BDD8u, 0xFFFFFFFFu, 0xFFFFE8F2u,
        0xFFFFB08Fu, 0xFFF2F1BFu, 0xFFFFC3B7u, 0xFF170A12u, 0xFFFFE5B6u,
    },
    {
        "SUNSET",
        0xFF1A0C14u, 0xFF4F2742u, 0xFF5D2D45u, 0xFF311526u,
        0xFF180914u, 0xFF28131Du, 0xFFFFAE73u, 0xFFFFF0BCu,
        0xFF301824u, 0xFFFFB97Eu, 0xFF3C1D2Du, 0xFF94435Cu,
        0xFFFFCB97u, 0xFF321724u, 0xFFB55A54u, 0xFFF09A6Au,
        0xFFFFEDAFu, 0xFFFFC694u, 0xFFFFFFFFu, 0xFFFFE7D2u,
        0xFFFFA57Bu, 0xFFFFE4A8u, 0xFFFFBE9Cu, 0xFF180914u, 0xFFFFEDAFu,
    },
}};

constexpr std::array<InputBindingDefinition, kInputBindingCount> kBindings = {{
    {"A",      0x0080, SDL_SCANCODE_S},
    {"B",      0x8000, SDL_SCANCODE_X},
    {"X",      0x0040, SDL_SCANCODE_A},
    {"Y",      0x4000, SDL_SCANCODE_Z},
    {"UP",     0x0008, SDL_SCANCODE_UP},
    {"DOWN",   0x0004, SDL_SCANCODE_DOWN},
    {"LEFT",   0x0002, SDL_SCANCODE_LEFT},
    {"RIGHT",  0x0001, SDL_SCANCODE_RIGHT},
    {"START",  0x1000, SDL_SCANCODE_RETURN},
    {"SELECT", 0x2000, SDL_SCANCODE_BACKSPACE},
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

std::string launcherThemeDisplayName(LauncherTheme theme) {
    std::string text = paletteForTheme(theme).name;
    bool capitalize = true;
    for (char& ch : text) {
        if (ch == ' ' || ch == '-' || ch == '_') {
            capitalize = true;
            continue;
        }
        ch = capitalize ? (char)std::toupper((unsigned char)ch)
                        : (char)std::tolower((unsigned char)ch);
        capitalize = false;
    }
    return text;
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

struct LauncherSettings {
    LauncherTheme theme = LauncherTheme::Midnight;
    bool visualize = false;
    bool manipulator = false;
    bool integer_scaling = false;
    std::string last_rom;
};

bool LoadLauncherSettings(const std::string& path, LauncherSettings& settings) {
    settings = {};

    std::ifstream in(path);
    if (!in) return false;

    std::string line;
    while (std::getline(in, line)) {
        line = trimLine(line);
        if (line.empty() || line[0] == '#') continue;

        const std::size_t eq = line.find('=');
        if (eq == std::string::npos) continue;

        const std::string key = upperCopy(trimLine(line.substr(0, eq)));
        const std::string value_raw = trimLine(line.substr(eq + 1));
        const std::string value_upper = upperCopy(value_raw);
        if (key == "THEME") parseLauncherTheme(value_upper, settings.theme);
        else if (key == "VISUALIZE") settings.visualize = (value_upper == "1" || value_upper == "TRUE");
        else if (key == "MANIPULATOR") settings.manipulator = (value_upper == "1" || value_upper == "TRUE");
        else if (key == "INTEGER_SCALING") settings.integer_scaling = (value_upper == "1" || value_upper == "TRUE");
        else if (key == "LAST_ROM") settings.last_rom = value_raw;
    }

    return true;
}

bool SaveLauncherSettings(const std::string& path, const LauncherSettings& settings) {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;

    out << "# Super Furamicom launcher settings\n";
    out << "THEME=" << launcherThemeName(settings.theme) << "\n";
    out << "VISUALIZE=" << (settings.visualize ? "1" : "0") << "\n";
    out << "MANIPULATOR=" << (settings.manipulator ? "1" : "0") << "\n";
    out << "INTEGER_SCALING=" << (settings.integer_scaling ? "1" : "0") << "\n";
    if (!settings.last_rom.empty()) {
        out << "LAST_ROM=" << settings.last_rom << "\n";
    }
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

SDL_Point toLogicalPoint(int x, int y) {
    if (x < 0 || x >= kMenuWidth || y < 0 || y >= kMenuHeight) return {-1, -1};
    return {x, y};
}

struct MainLayout {
    SDL_Rect rom_panel{12, 34, 136, 154};
    SDL_Rect rom_list{18, 60, 124, 124};
    SDL_Rect options_panel{152, 34, 96, 154};
    SDL_Rect visualize_box{158, 54, 84, 16};
    SDL_Rect visualize_check{174, 50, 10, 10};
    SDL_Rect manipulator_box{158, 76, 84, 16};
    SDL_Rect manipulator_check{174, 66, 10, 10};
    SDL_Rect intscale_box{158, 98, 84, 16};
    SDL_Rect intscale_check{174, 80, 10, 10};
    SDL_Rect theme_button{158, 126, 40, 15};
    SDL_Rect controls_button{202, 126, 40, 15};
    SDL_Rect start_button{158, 148, 84, 16};
    SDL_Rect quit_button{158, 168, 84, 16};
};

bool pointInRect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

int classicVisibleRomRows(const MainLayout& layout) {
    return std::max(1, layout.rom_list.h / std::max(1, kRomRowHeight));
}

void drawMainView(uint32_t* pixels,
                  const std::vector<std::string>& rom_paths,
                  int selected_rom,
                  int rom_scroll,
                  MainFocus focus,
                  bool visualize,
                  bool manipulator,
                  bool integer_scaling,
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
    drawTextShadow(pixels, 18, 40, "ROM LIBRARY", palette.header, 1, palette.shadow);
    drawTextShadow(pixels, 176, 40, "OPTIONS", palette.header, 1, palette.shadow);

    const int visible_rows = classicVisibleRomRows(layout);
    for (int row = 0; row < visible_rows; row++) {
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

    drawTextShadow(pixels, 186, 49, "VISUALIZER", palette.text, 1, palette.shadow);
    drawRect(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
             focus == MainFocus::Visualizer ? palette.option_active : palette.option_fill);
    drawOutline(pixels, layout.visualize_check.x, layout.visualize_check.y, 10, 10,
                focus == MainFocus::Visualizer ? palette.panel_border_focus : palette.option_border);
    if (visualize) {
        drawRect(pixels, layout.visualize_check.x + 2, layout.visualize_check.y + 2, 6, 6, palette.check_fill);
    }

    drawTextShadow(pixels, 180, 63, "MANIPULATOR", palette.text, 1, palette.shadow);
    drawRect(pixels, layout.manipulator_check.x, layout.manipulator_check.y, 10, 10,
             focus == MainFocus::Manipulator ? palette.option_active : palette.option_fill);
    drawOutline(pixels, layout.manipulator_check.x, layout.manipulator_check.y, 10, 10,
                focus == MainFocus::Manipulator ? palette.panel_border_focus : palette.option_border);
    if (manipulator) {
        drawRect(pixels, layout.manipulator_check.x + 2, layout.manipulator_check.y + 2, 6, 6, palette.check_fill);
    }

    drawTextShadow(pixels, 186, 77, "INT SCALE", palette.text, 1, palette.shadow);
    drawRect(pixels, layout.intscale_check.x, layout.intscale_check.y, 10, 10,
             focus == MainFocus::IntegerScale ? palette.option_active : palette.option_fill);
    drawOutline(pixels, layout.intscale_check.x, layout.intscale_check.y, 10, 10,
                focus == MainFocus::IntegerScale ? palette.panel_border_focus : palette.option_border);
    if (integer_scaling) {
        drawRect(pixels, layout.intscale_check.x + 2, layout.intscale_check.y + 2, 6, 6, palette.check_fill);
    }

    drawTextShadow(pixels, 188, 90, "THEME", palette.header, 1, palette.shadow);
    drawButton(layout.theme_button, MainFocus::Theme, launcherThemeName(theme));
    drawButton(layout.controls_button, MainFocus::Controls, "CONTROLS");
    drawButton(layout.start_button, MainFocus::Start, "START");
    drawButton(layout.quit_button, MainFocus::Quit, "QUIT");

    drawTextShadow(pixels, 12, 198, "ARROWS MOVE  ENTER SELECT  ESC QUIT", palette.header, 1, palette.shadow);
    if (!status.empty()) {
        drawTextShadow(pixels, 12, 212, clipText(upperCopy(status), 38), palette.status, 1, palette.shadow);
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

#ifdef _WIN32
struct RendererColor {
    Uint8 r = 0;
    Uint8 g = 0;
    Uint8 b = 0;
    Uint8 a = 255;
};

RendererColor toRendererColor(uint32_t argb, Uint8 alpha = 255) {
    return RendererColor{
        (Uint8)((argb >> 16) & 0xFF),
        (Uint8)((argb >> 8) & 0xFF),
        (Uint8)(argb & 0xFF),
        alpha,
    };
}

void setRendererColor(SDL_Renderer* renderer, uint32_t argb, Uint8 alpha = 255) {
    const RendererColor color = toRendererColor(argb, alpha);
    SDL_SetRenderDrawColor(renderer, color.r, color.g, color.b, color.a);
}

void fillRendererRect(SDL_Renderer* renderer, const SDL_Rect& rect, uint32_t color, Uint8 alpha = 255) {
    setRendererColor(renderer, color, alpha);
    SDL_RenderFillRect(renderer, &rect);
}

void drawRendererOutline(SDL_Renderer* renderer, const SDL_Rect& rect, uint32_t color, Uint8 alpha = 255) {
    setRendererColor(renderer, color, alpha);
    SDL_RenderDrawRect(renderer, &rect);
}

void drawRendererLine(SDL_Renderer* renderer,
                      int x1,
                      int y1,
                      int x2,
                      int y2,
                      uint32_t color,
                      Uint8 alpha = 255) {
    setRendererColor(renderer, color, alpha);
    SDL_RenderDrawLine(renderer, x1, y1, x2, y2);
}

void fillVerticalGradient(SDL_Renderer* renderer, const SDL_Rect& rect, uint32_t top, uint32_t bottom) {
    const int count = std::max(1, rect.h - 1);
    for (int y = 0; y < rect.h; y++) {
        const uint32_t color = lerpColor(top, bottom, y, count);
        setRendererColor(renderer, color);
        SDL_RenderDrawLine(renderer, rect.x, rect.y + y, rect.x + rect.w - 1, rect.y + y);
    }
}

int scaledValue(float scale, int base) {
    return std::max(1, (int)std::lround(scale * (float)base));
}

SDL_Rect scaledRect(const SDL_Rect& base, float scale, int origin_x, int origin_y) {
    return SDL_Rect{
        origin_x + scaledValue(scale, base.x),
        origin_y + scaledValue(scale, base.y),
        scaledValue(scale, base.w),
        scaledValue(scale, base.h),
    };
}

void drawPanelShadow(SDL_Renderer* renderer, const SDL_Rect& rect, int offset_x, int offset_y, Uint8 alpha) {
    SDL_Rect shadow = rect;
    shadow.x += offset_x;
    shadow.y += offset_y;
    fillRendererRect(renderer, shadow, 0xFF000000u, alpha);
}

struct ModernLayout {
    int output_w = kMenuWidth;
    int output_h = kMenuHeight;
    float scale = 1.0f;
    int origin_x = 0;
    int origin_y = 0;
    int content_w = kMenuWidth;
    int content_h = kMenuHeight;
    int body_font = 13;
    int option_font = 10;
    int small_font = 12;
    int section_font = 12;
    int title_font = 20;
    int button_font = 10;
    int key_font = 12;
    int footer_font = 12;
    int shadow_offset = 1;
    int row_height = 15;
    int check_size = 13;
    SDL_Rect top_bar{};
    SDL_Rect rom_panel{};
    SDL_Rect rom_list{};
    SDL_Rect options_panel{};
    SDL_Rect visualize_box{};
    SDL_Rect manipulator_box{};
    SDL_Rect intscale_box{};
    SDL_Rect theme_button{};
    SDL_Rect controls_button{};
    SDL_Rect start_button{};
    SDL_Rect quit_button{};
    SDL_Rect controls_panel{};
};

int modernVisibleRomRows(const ModernLayout& layout) {
    return std::max(1, layout.rom_list.h / std::max(1, layout.row_height));
}

ModernLayout makeModernLayout(SDL_Renderer* renderer) {
    ModernLayout layout{};
    SDL_GetRendererOutputSize(renderer, &layout.output_w, &layout.output_h);
    if (layout.output_w <= 0) layout.output_w = kMenuWidth;
    if (layout.output_h <= 0) layout.output_h = kMenuHeight;

    layout.scale = std::min(layout.output_w / (float)kMenuWidth, layout.output_h / (float)kMenuHeight);
    if (!(layout.scale > 0.0f)) layout.scale = 1.0f;

    layout.content_w = std::max(1, (int)std::lround(kMenuWidth * layout.scale));
    layout.content_h = std::max(1, (int)std::lround(kMenuHeight * layout.scale));
    layout.origin_x = std::max(0, (layout.output_w - layout.content_w) / 2);
    layout.origin_y = std::max(0, (layout.output_h - layout.content_h) / 2);

    const MainLayout base{};
    layout.top_bar = scaledRect(SDL_Rect{0, 0, kMenuWidth, 28}, layout.scale, layout.origin_x, layout.origin_y);
    layout.rom_panel = scaledRect(base.rom_panel, layout.scale, layout.origin_x, layout.origin_y);
    layout.rom_list = scaledRect(base.rom_list, layout.scale, layout.origin_x, layout.origin_y);
    layout.options_panel = scaledRect(base.options_panel, layout.scale, layout.origin_x, layout.origin_y);
    layout.visualize_box = scaledRect(base.visualize_box, layout.scale, layout.origin_x, layout.origin_y);
    layout.manipulator_box = scaledRect(base.manipulator_box, layout.scale, layout.origin_x, layout.origin_y);
    layout.intscale_box = scaledRect(base.intscale_box, layout.scale, layout.origin_x, layout.origin_y);
    layout.theme_button = scaledRect(base.theme_button, layout.scale, layout.origin_x, layout.origin_y);
    layout.controls_button = scaledRect(base.controls_button, layout.scale, layout.origin_x, layout.origin_y);
    layout.start_button = scaledRect(base.start_button, layout.scale, layout.origin_x, layout.origin_y);
    layout.quit_button = scaledRect(base.quit_button, layout.scale, layout.origin_x, layout.origin_y);
    layout.controls_panel = scaledRect(SDL_Rect{10, 18, 236, 190}, layout.scale, layout.origin_x, layout.origin_y);

    layout.body_font = std::clamp(scaledValue(layout.scale, 6), 12, 15);
    layout.option_font = std::clamp(scaledValue(layout.scale, 4), 10, 13);
    layout.small_font = std::clamp(scaledValue(layout.scale, 5), 11, 13);
    layout.section_font = std::clamp(scaledValue(layout.scale, 5), 12, 15);
    layout.title_font = std::clamp(scaledValue(layout.scale, 9), 18, 24);
    layout.button_font = std::clamp(scaledValue(layout.scale, 4), 10, 13);
    layout.key_font = std::clamp(scaledValue(layout.scale, 5), 11, 13);
    layout.footer_font = std::clamp(scaledValue(layout.scale, 5), 11, 13);
    layout.shadow_offset = std::clamp(scaledValue(layout.scale, 1), 1, 2);
    layout.row_height = std::clamp(scaledValue(layout.scale, kRomRowHeight), 18, 28);
    layout.check_size = std::clamp(scaledValue(layout.scale, 10), 12, 18);
    return layout;
}

std::wstring widenUtf8(std::string_view text) {
    if (text.empty()) return std::wstring();

    const int needed = MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), nullptr, 0);
    if (needed > 0) {
        std::wstring wide((std::size_t)needed, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), wide.data(), needed);
        return wide;
    }

    std::wstring wide;
    wide.reserve(text.size());
    for (char ch : text) {
        wide.push_back((wchar_t)(unsigned char)ch);
    }
    return wide;
}

struct TextCacheKey {
    std::string text;
    uint32_t color = 0;
    int pixel_size = 0;
    int weight = 0;

    bool operator==(const TextCacheKey& rhs) const {
        return color == rhs.color
            && pixel_size == rhs.pixel_size
            && weight == rhs.weight
            && text == rhs.text;
    }
};

struct TextCacheKeyHash {
    std::size_t operator()(const TextCacheKey& key) const {
        std::size_t seed = std::hash<std::string>{}(key.text);
        seed ^= (std::hash<uint32_t>{}(key.color) + 0x9e3779b9u + (seed << 6) + (seed >> 2));
        seed ^= (std::hash<int>{}(key.pixel_size) + 0x9e3779b9u + (seed << 6) + (seed >> 2));
        seed ^= (std::hash<int>{}(key.weight) + 0x9e3779b9u + (seed << 6) + (seed >> 2));
        return seed;
    }
};

struct CachedTextTexture {
    SDL_Texture* texture = nullptr;
    int width = 0;
    int height = 0;
};

class LauncherTextRenderer {
public:
    explicit LauncherTextRenderer(SDL_Renderer* renderer) : renderer_(renderer) {}
    ~LauncherTextRenderer() { clear(); }

    bool available() const { return renderer_ != nullptr; }

    void clear() {
        for (auto& entry : cache_) {
            if (entry.second.texture) SDL_DestroyTexture(entry.second.texture);
        }
        cache_.clear();
    }

    SDL_Point measure(std::string_view text, int pixel_size, int weight = FW_NORMAL) {
        if (text.empty() || pixel_size <= 0) return SDL_Point{0, 0};
        const std::wstring wide = widenUtf8(text);
        if (wide.empty()) return SDL_Point{0, 0};

        SDL_Point point{0, std::max(1, pixel_size)};
        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return point;

        HFONT font = CreateFontW(-pixel_size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!font) {
            DeleteDC(dc);
            return point;
        }

        HGDIOBJ old_font = SelectObject(dc, font);
        SIZE size{};
        if (GetTextExtentPoint32W(dc, wide.c_str(), (int)wide.size(), &size)) {
            point.x = size.cx;
            point.y = size.cy;
        }
        SelectObject(dc, old_font);
        DeleteObject(font);
        DeleteDC(dc);
        return point;
    }

    void draw(std::string_view text,
              int x,
              int y,
              uint32_t color,
              int pixel_size,
              int weight = FW_NORMAL,
              Uint8 alpha = 255) {
        if (!renderer_ || text.empty() || pixel_size <= 0) return;
        CachedTextTexture* cached = getTexture(text, color, pixel_size, weight);
        if (!cached || !cached->texture) return;
        SDL_SetTextureAlphaMod(cached->texture, alpha);
        SDL_Rect dst{x, y, cached->width, cached->height};
        SDL_RenderCopy(renderer_, cached->texture, nullptr, &dst);
    }

private:
    CachedTextTexture* getTexture(std::string_view text, uint32_t color, int pixel_size, int weight) {
        TextCacheKey key{std::string(text), color, pixel_size, weight};
        auto found = cache_.find(key);
        if (found != cache_.end()) return &found->second;

        CachedTextTexture texture = createTexture(key.text, color, pixel_size, weight);
        auto [it, inserted] = cache_.emplace(std::move(key), texture);
        if (!inserted) return nullptr;
        return &it->second;
    }

    CachedTextTexture createTexture(std::string_view text, uint32_t color, int pixel_size, int weight) {
        CachedTextTexture result{};
        if (!renderer_ || text.empty() || pixel_size <= 0) return result;

        const std::wstring wide = widenUtf8(text);
        if (wide.empty()) return result;

        HDC dc = CreateCompatibleDC(nullptr);
        if (!dc) return result;

        HFONT font = CreateFontW(-pixel_size, 0, 0, 0, weight, FALSE, FALSE, FALSE,
                                 DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        if (!font) {
            DeleteDC(dc);
            return result;
        }

        HGDIOBJ old_font = SelectObject(dc, font);
        SIZE measured{};
        GetTextExtentPoint32W(dc, wide.c_str(), (int)wide.size(), &measured);
        const int measured_w = (int)measured.cx;
        const int measured_h = (int)measured.cy;
        const int width = std::max(1, measured_w + 4);
        const int height = std::max(1, std::max(measured_h, pixel_size) + 4);

        BITMAPINFO bmi{};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -height;
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* dib_bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &dib_bits, nullptr, 0);
        if (!bitmap || !dib_bits) {
            SelectObject(dc, old_font);
            DeleteObject(font);
            DeleteDC(dc);
            return result;
        }

        HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
        std::memset(dib_bits, 0, (std::size_t)width * (std::size_t)height * sizeof(uint32_t));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(255, 255, 255));

        RECT rect{2, 1, width, height};
        DrawTextW(dc, wide.c_str(), (int)wide.size(), &rect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);

        std::vector<uint32_t> pixels((std::size_t)width * (std::size_t)height, 0);
        const uint8_t out_r = (uint8_t)((color >> 16) & 0xFF);
        const uint8_t out_g = (uint8_t)((color >> 8) & 0xFF);
        const uint8_t out_b = (uint8_t)(color & 0xFF);
        const uint32_t* src = (const uint32_t*)dib_bits;
        for (std::size_t i = 0; i < pixels.size(); i++) {
            const uint32_t bgra = src[i];
            const uint8_t b = (uint8_t)(bgra & 0xFF);
            const uint8_t g = (uint8_t)((bgra >> 8) & 0xFF);
            const uint8_t r = (uint8_t)((bgra >> 16) & 0xFF);
            const uint8_t coverage = std::max(r, std::max(g, b));
            if (coverage == 0) continue;
            pixels[i] = ((uint32_t)coverage << 24)
                      | ((uint32_t)out_r << 16)
                      | ((uint32_t)out_g << 8)
                      | (uint32_t)out_b;
        }

        result.texture = SDL_CreateTexture(renderer_, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STATIC, width, height);
        if (result.texture) {
            SDL_UpdateTexture(result.texture, nullptr, pixels.data(), width * (int)sizeof(uint32_t));
            SDL_SetTextureBlendMode(result.texture, SDL_BLENDMODE_BLEND);
            result.width = width;
            result.height = height;
        }

        SelectObject(dc, old_bitmap);
        SelectObject(dc, old_font);
        DeleteObject(bitmap);
        DeleteObject(font);
        DeleteDC(dc);
        return result;
    }

    SDL_Renderer* renderer_ = nullptr;
    std::unordered_map<TextCacheKey, CachedTextTexture, TextCacheKeyHash> cache_;
};

void drawTextShadowModern(LauncherTextRenderer& text,
                          std::string_view value,
                          int x,
                          int y,
                          uint32_t color,
                          int pixel_size,
                          int weight,
                          int shadow_offset,
                          Uint8 shadow_alpha = 40) {
    text.draw(value, x + shadow_offset, y + shadow_offset, 0xFF000000u, pixel_size, weight, shadow_alpha);
    text.draw(value, x, y, color, pixel_size, weight);
}

std::string fitTextToWidth(LauncherTextRenderer& text,
                           std::string_view value,
                           int max_width,
                           int pixel_size,
                           int weight = FW_NORMAL) {
    const std::string source(value);
    if (source.empty()) return source;
    if (text.measure(source, pixel_size, weight).x <= max_width) return source;

    const std::string ellipsis = "...";
    if (text.measure(ellipsis, pixel_size, weight).x > max_width) return std::string();

    int low = 0;
    int high = (int)source.size();
    int best = 0;
    while (low <= high) {
        const int mid = (low + high) / 2;
        const std::string candidate = source.substr(0, (std::size_t)mid) + ellipsis;
        if (text.measure(candidate, pixel_size, weight).x <= max_width) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return source.substr(0, (std::size_t)best) + ellipsis;
}

void drawCenteredText(LauncherTextRenderer& text,
                      const SDL_Rect& rect,
                      std::string_view value,
                      uint32_t color,
                      int pixel_size,
                      int weight,
                      int shadow_offset) {
    const SDL_Point size = text.measure(value, pixel_size, weight);
    const int x = rect.x + std::max(0, (rect.w - size.x) / 2);
    const int y = rect.y + std::max(0, (rect.h - size.y) / 2) - std::max(0, shadow_offset / 2);
    drawTextShadowModern(text, value, x, y, color, pixel_size, weight, shadow_offset);
}

void drawLauncherCard(SDL_Renderer* renderer,
                      const SDL_Rect& rect,
                      uint32_t fill,
                      uint32_t border,
                      int shadow_offset,
                      bool focused = false,
                      uint32_t focus_border = 0xFFFFFFFFu) {
    drawPanelShadow(renderer, rect, shadow_offset, shadow_offset + 1, 22);
    fillRendererRect(renderer, rect, fill, 236);
    drawRendererOutline(renderer, rect, focused ? focus_border : border);
}

void drawLauncherBackground(SDL_Renderer* renderer, const ModernLayout& layout, const LauncherPalette& palette) {
    fillVerticalGradient(renderer, SDL_Rect{0, 0, layout.output_w, layout.output_h}, palette.bg_top, palette.bg_bottom);

    const int diag_step = std::max(28, scaledValue(layout.scale, 24));
    const int grid_step = std::max(60, scaledValue(layout.scale, 32));
    for (int x = layout.origin_x - layout.output_h; x < layout.output_w + layout.output_h; x += diag_step) {
        drawRendererLine(renderer, x, 0, x + layout.output_h, layout.output_h, palette.bg_diag, 32);
    }
    for (int x = layout.origin_x; x < layout.origin_x + layout.content_w + grid_step; x += grid_step) {
        drawRendererLine(renderer, x, 0, x, layout.output_h, palette.bg_grid, 40);
    }
}

void renderMainViewModern(SDL_Renderer* renderer,
                          LauncherTextRenderer& text,
                          const std::vector<std::string>& rom_paths,
                          int selected_rom,
                          int rom_scroll,
                          MainFocus focus,
                          bool visualize,
                          bool manipulator,
                          bool integer_scaling,
                          LauncherTheme theme,
                          const std::string& status) {
    const LauncherPalette& palette = paletteForTheme(theme);
    const ModernLayout layout = makeModernLayout(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    const int text_shadow = (theme == LauncherTheme::Cloud) ? 1 : layout.shadow_offset;
    const Uint8 text_shadow_alpha = (theme == LauncherTheme::Cloud) ? 24 : 40;

    drawLauncherBackground(renderer, layout, palette);
    fillRendererRect(renderer, layout.top_bar, palette.top_bar, 232);
    drawPanelShadow(renderer, layout.top_bar, layout.shadow_offset, layout.shadow_offset + 1, 16);

    drawLauncherCard(renderer,
                     layout.rom_panel,
                     palette.panel_fill,
                     palette.panel_border,
                     layout.shadow_offset,
                     focus == MainFocus::RomList,
                     palette.panel_border_focus);
    drawLauncherCard(renderer,
                     layout.options_panel,
                     palette.options_fill,
                     palette.options_border,
                     layout.shadow_offset);

    drawTextShadowModern(text,
                         "Super Furamicom",
                         layout.origin_x + scaledValue(layout.scale, 14),
                         layout.origin_y + scaledValue(layout.scale, 8),
                         palette.title,
                         layout.title_font,
                         FW_SEMIBOLD,
                         text_shadow,
                         text_shadow_alpha);
    drawTextShadowModern(text,
                         "ROM LIBRARY",
                         layout.rom_panel.x + scaledValue(layout.scale, 10),
                         layout.rom_panel.y + scaledValue(layout.scale, 8),
                         palette.header,
                         layout.section_font,
                         FW_SEMIBOLD,
                         text_shadow,
                         text_shadow_alpha);
    drawTextShadowModern(text,
                         "OPTIONS",
                         layout.options_panel.x + scaledValue(layout.scale, 10),
                         layout.options_panel.y + scaledValue(layout.scale, 8),
                         palette.header,
                         layout.section_font,
                         FW_SEMIBOLD,
                         text_shadow,
                         text_shadow_alpha);

    const int rom_pad_x = std::max(8, scaledValue(layout.scale, 6));
    const int row_gap = std::max(3, scaledValue(layout.scale, 2));
    const int row_height = std::max(14, layout.row_height - row_gap);
    const int rom_text_max = layout.rom_list.w - rom_pad_x * 2;
    const int visible_rows = modernVisibleRomRows(layout);
    for (int row = 0; row < visible_rows; row++) {
        const int rom_index = rom_scroll + row;
        SDL_Rect row_rect{
            layout.rom_list.x - scaledValue(layout.scale, 3),
            layout.rom_list.y + row * layout.row_height - scaledValue(layout.scale, 1),
            layout.rom_list.w + scaledValue(layout.scale, 2),
            row_height
        };
        const bool selected = rom_index == selected_rom && rom_index < (int)rom_paths.size();
        fillRendererRect(renderer, row_rect, palette.row_fill, 255);
        if (selected) {
            fillRendererRect(renderer,
                             row_rect,
                             focus == MainFocus::RomList ? palette.row_active : palette.option_fill,
                             255);
            drawRendererOutline(renderer,
                                row_rect,
                                focus == MainFocus::RomList ? palette.panel_border_focus : palette.row_idle_border);
        }

        if (rom_index < (int)rom_paths.size()) {
            const std::string label = fitTextToWidth(text,
                                                     romLabel(rom_paths[(std::size_t)rom_index]),
                                                     rom_text_max,
                                                     layout.body_font,
                                                     FW_SEMIBOLD);
            const SDL_Point text_size = text.measure(label, layout.body_font, FW_SEMIBOLD);
            drawTextShadowModern(text,
                                 label,
                                 row_rect.x + rom_pad_x,
                                 row_rect.y + std::max(0, (row_rect.h - text_size.y) / 2) - 1,
                                 selected ? palette.text : palette.text_soft,
                                 layout.body_font,
                                 FW_SEMIBOLD,
                                 text_shadow,
                                 text_shadow_alpha);
        }
    }

    if (rom_paths.empty()) {
        drawCenteredText(text,
                         SDL_Rect{layout.rom_panel.x, layout.rom_panel.y + scaledValue(layout.scale, 44),
                                  layout.rom_panel.w, scaledValue(layout.scale, 20)},
                         "No ROMs Found",
                         palette.warning,
                         layout.title_font,
                         FW_SEMIBOLD,
                         text_shadow);
        drawCenteredText(text,
                         SDL_Rect{layout.rom_panel.x + scaledValue(layout.scale, 8),
                                  layout.rom_panel.y + scaledValue(layout.scale, 96),
                                  layout.rom_panel.w - scaledValue(layout.scale, 16),
                                  scaledValue(layout.scale, 32)},
                         "Drop .sfc or .smc files into /roms",
                         palette.header,
                         layout.body_font,
                         FW_NORMAL,
                         text_shadow);
    }

    auto drawToggle = [&](const SDL_Rect& row_rect, MainFocus id, std::string_view label, bool enabled) {
        drawLauncherCard(renderer,
                         row_rect,
                         focus == id ? palette.option_active : palette.option_fill,
                         focus == id ? palette.panel_border_focus : palette.option_border,
                         layout.shadow_offset);
        const SDL_Rect check_rect{
            row_rect.x + scaledValue(layout.scale, 5),
            row_rect.y + std::max(0, (row_rect.h - layout.check_size) / 2),
            layout.check_size,
            layout.check_size
        };
        fillRendererRect(renderer,
                         check_rect,
                         focus == id ? palette.option_active : palette.panel_fill,
                         255);
        drawRendererOutline(renderer,
                            check_rect,
                            focus == id ? palette.panel_border_focus : palette.option_border);
        if (enabled) {
            SDL_Rect inner = check_rect;
            inner.x += std::max(2, layout.check_size / 4);
            inner.y += std::max(2, layout.check_size / 4);
            inner.w = std::max(4, check_rect.w - std::max(4, layout.check_size / 2));
            inner.h = std::max(4, check_rect.h - std::max(4, layout.check_size / 2));
            fillRendererRect(renderer, inner, palette.check_fill, 255);
        }

        const int label_x = check_rect.x + check_rect.w + scaledValue(layout.scale, 4);
        const int label_max = row_rect.x + row_rect.w - scaledValue(layout.scale, 4) - label_x;
        const std::string fitted = fitTextToWidth(text, label, label_max, layout.option_font, FW_NORMAL);
        const SDL_Point text_size = text.measure(fitted, layout.option_font, FW_NORMAL);
        drawTextShadowModern(text,
                             fitted,
                             label_x,
                             row_rect.y + std::max(0, (row_rect.h - text_size.y) / 2) - 1,
                             palette.text,
                             layout.option_font,
                             FW_NORMAL,
                             text_shadow,
                             text_shadow_alpha);
    };

    auto drawButton = [&](const SDL_Rect& rect, MainFocus id, std::string_view label, int weight = FW_SEMIBOLD) {
        drawLauncherCard(renderer,
                         rect,
                         focus == id ? palette.option_active : palette.option_fill,
                         focus == id ? palette.panel_border_focus : palette.option_border,
                         layout.shadow_offset);
        drawCenteredText(text, rect, label, palette.text, layout.button_font, weight, text_shadow);
    };

    drawToggle(layout.visualize_box, MainFocus::Visualizer, "Visualizer", visualize);
    drawToggle(layout.manipulator_box, MainFocus::Manipulator, "Manipulator", manipulator);
    drawToggle(layout.intscale_box, MainFocus::IntegerScale, "Integer Scale", integer_scaling);

    drawButton(layout.theme_button, MainFocus::Theme, launcherThemeDisplayName(theme), FW_NORMAL);
    drawButton(layout.controls_button, MainFocus::Controls, "Controls");
    drawButton(layout.start_button, MainFocus::Start, "Start");
    drawButton(layout.quit_button, MainFocus::Quit, "Quit");

    drawTextShadowModern(text,
                         "Arrows move   Enter select   Esc quit",
                         layout.origin_x + scaledValue(layout.scale, 12),
                         layout.origin_y + scaledValue(layout.scale, 196),
                         palette.header,
                         layout.footer_font,
                         FW_NORMAL,
                         text_shadow,
                         text_shadow_alpha);
    if (!status.empty()) {
        drawTextShadowModern(text,
                             fitTextToWidth(text,
                                            upperCopy(status),
                                            layout.content_w - scaledValue(layout.scale, 24),
                                            layout.footer_font,
                                            FW_SEMIBOLD),
                             layout.origin_x + scaledValue(layout.scale, 12),
                             layout.origin_y + scaledValue(layout.scale, 210),
                             palette.status,
                             layout.footer_font,
                             FW_SEMIBOLD,
                             text_shadow,
                             text_shadow_alpha);
    }
}

void renderControlsViewModern(SDL_Renderer* renderer,
                              LauncherTextRenderer& text,
                              const InputConfig& config,
                              int selected_binding,
                              bool waiting_for_key,
                              LauncherTheme theme,
                              const std::string& status) {
    const LauncherPalette& palette = paletteForTheme(theme);
    const ModernLayout layout = makeModernLayout(renderer);
    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);

    drawLauncherBackground(renderer, layout, palette);
    drawLauncherCard(renderer, layout.controls_panel, palette.panel_fill, palette.options_border, layout.shadow_offset);

    drawTextShadowModern(text,
                         "Control Mapping",
                         layout.origin_x + scaledValue(layout.scale, 16),
                         layout.origin_y + scaledValue(layout.scale, 8),
                         palette.title,
                         layout.title_font,
                         FW_SEMIBOLD,
                         layout.shadow_offset);
    drawTextShadowModern(text,
                         "Enter rebinds   Backspace default",
                         layout.origin_x + scaledValue(layout.scale, 16),
                         layout.origin_y + scaledValue(layout.scale, 28),
                         palette.header,
                         layout.small_font,
                         FW_NORMAL,
                         layout.shadow_offset);
    drawTextShadowModern(text,
                         "F9 reset all   Esc back",
                         layout.origin_x + scaledValue(layout.scale, 16),
                         layout.origin_y + scaledValue(layout.scale, 40),
                         palette.header,
                         layout.small_font,
                         FW_NORMAL,
                         layout.shadow_offset);

    const int row_height = std::max(18, scaledValue(layout.scale, 12));
    const int row_x = layout.origin_x + scaledValue(layout.scale, 18);
    const int row_w = scaledValue(layout.scale, 220);
    const int label_x = layout.origin_x + scaledValue(layout.scale, 24);
    const int value_x = layout.origin_x + scaledValue(layout.scale, 108);
    const int value_max = layout.controls_panel.x + layout.controls_panel.w - scaledValue(layout.scale, 16) - value_x;
    for (int i = 0; i < (int)kBindings.size(); i++) {
        const int row_y = layout.origin_y + scaledValue(layout.scale, 54 + i * 12);
        const SDL_Rect row_rect{row_x, row_y - scaledValue(layout.scale, 2), row_w, row_height};
        if (i == selected_binding) {
            fillRendererRect(renderer, row_rect, waiting_for_key ? 0xFF693117u : palette.row_active, 255);
            drawRendererOutline(renderer, row_rect, palette.panel_border_focus);
        }

        drawTextShadowModern(text,
                             kBindings[(std::size_t)i].label,
                             label_x,
                             row_rect.y + std::max(0, (row_rect.h - text.measure(kBindings[(std::size_t)i].label, layout.body_font, FW_SEMIBOLD).y) / 2) - 1,
                             palette.text,
                             layout.body_font,
                             FW_SEMIBOLD,
                             layout.shadow_offset);

        const std::string bound = fitTextToWidth(text,
                                                 keyName(config.scancodes[(std::size_t)i]),
                                                 value_max,
                                                 layout.key_font,
                                                 FW_NORMAL);
        const SDL_Point value_size = text.measure(bound, layout.key_font, FW_NORMAL);
        drawTextShadowModern(text,
                             bound,
                             value_x,
                             row_rect.y + std::max(0, (row_rect.h - value_size.y) / 2) - 1,
                             i == selected_binding ? palette.title : palette.text_soft,
                             layout.key_font,
                             FW_NORMAL,
                             layout.shadow_offset);
    }

    if (waiting_for_key) {
        const SDL_Rect modal = scaledRect(SDL_Rect{40, 176, 176, 20}, layout.scale, layout.origin_x, layout.origin_y);
        drawLauncherCard(renderer, modal, 0xFF2A1410u, palette.warning, layout.shadow_offset);
        drawCenteredText(text, modal, "Press a key...", palette.title, layout.body_font, FW_SEMIBOLD, layout.shadow_offset);
    }

    if (!status.empty()) {
        drawTextShadowModern(text,
                             fitTextToWidth(text,
                                            upperCopy(status),
                                            layout.content_w - scaledValue(layout.scale, 24),
                                            layout.footer_font,
                                            FW_SEMIBOLD),
                             layout.origin_x + scaledValue(layout.scale, 16),
                             layout.origin_y + scaledValue(layout.scale, 210),
                             palette.status,
                             layout.footer_font,
                             FW_SEMIBOLD,
                             layout.shadow_offset);
    }
}
#endif
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
                           bool visualize_default,
                           bool manipulator_default,
                           bool integer_scaling_default) {
    LauncherResult result{};
    result.input_config = initial_config;
    result.visualize = visualize_default;
    result.manipulator = manipulator_default;
    result.integer_scaling = integer_scaling_default;

    if (!window || !renderer || !texture) return result;

    std::array<uint32_t, kMenuWidth * kMenuHeight> pixels{};
#ifdef _WIN32
    LauncherTextRenderer modern_text(renderer);
    const bool use_modern_ui = modern_text.available();
    int saved_logical_w = 0;
    int saved_logical_h = 0;
    SDL_RenderGetLogicalSize(renderer, &saved_logical_w, &saved_logical_h);
    const SDL_bool saved_integer_scale = SDL_RenderGetIntegerScale(renderer);
    if (use_modern_ui) {
        SDL_RenderSetLogicalSize(renderer, 0, 0);
        SDL_RenderSetIntegerScale(renderer, SDL_FALSE);
    }
#else
    const bool use_modern_ui = false;
#endif
    LauncherView view = LauncherView::Main;
    MainFocus focus = MainFocus::RomList;
    int selected_rom = rom_paths.empty() ? -1 : 0;
    int rom_scroll = 0;
    int selected_binding = 0;
    bool waiting_for_key = false;
    std::string status;
    uint32_t last_click_ticks = 0;
    int last_click_rom = -1;
    LauncherSettings settings;
    LoadLauncherSettings(kLauncherSettingsPath, settings);
    LauncherTheme theme = settings.theme;
    result.visualize = settings.visualize || visualize_default;
    result.manipulator = settings.manipulator || manipulator_default;
    result.integer_scaling = settings.integer_scaling || integer_scaling_default;
    auto normalizedPath = [](const std::string& path) {
        try {
            return upperCopy(std::filesystem::weakly_canonical(std::filesystem::path(path)).string());
        } catch (...) {
            return upperCopy(path);
        }
    };
    const std::string last_rom_key = normalizedPath(settings.last_rom);
    bool running = true;

    auto restoreRendererState = [&]() {
#ifdef _WIN32
        if (use_modern_ui) {
            SDL_RenderSetLogicalSize(renderer, saved_logical_w, saved_logical_h);
            SDL_RenderSetIntegerScale(renderer, saved_integer_scale);
        }
#endif
    };
    auto visibleRomRows = [&]() {
#ifdef _WIN32
        if (use_modern_ui) return modernVisibleRomRows(makeModernLayout(renderer));
#endif
        return classicVisibleRomRows(MainLayout{});
    };
    auto clampRomScroll = [&]() {
        const int visible_rows = visibleRomRows();
        rom_scroll = std::clamp(rom_scroll, 0, std::max(0, (int)rom_paths.size() - visible_rows));
    };
    auto syncScrollToSelection = [&]() {
        const int visible_rows = visibleRomRows();
        if (selected_rom >= 0) {
            rom_scroll = std::clamp(selected_rom - (visible_rows / 2),
                                    0,
                                    std::max(0, (int)rom_paths.size() - visible_rows));
        } else {
            clampRomScroll();
        }
    };

    auto saveSettings = [&]() {
        settings.theme = theme;
        settings.visualize = result.visualize;
        settings.manipulator = result.manipulator;
        settings.integer_scaling = result.integer_scaling;
        SaveLauncherSettings(kLauncherSettingsPath, settings);
    };
    auto launchSelectedRom = [&]() {
        if (selected_rom < 0 || selected_rom >= (int)rom_paths.size()) {
            status = "SELECT A ROM FIRST";
            return false;
        }
        result.launch_requested = true;
        result.rom_path = rom_paths[(std::size_t)selected_rom];
        settings.last_rom = result.rom_path;
        saveSettings();
        running = false;
        return true;
    };

    SDL_SetWindowTitle(window, "Super Furamicom Launcher");

    if (!last_rom_key.empty()) {
        for (int i = 0; i < (int)rom_paths.size(); i++) {
            if (normalizedPath(rom_paths[(std::size_t)i]) == last_rom_key) {
                selected_rom = i;
                break;
            }
        }
    }
    syncScrollToSelection();

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
                    if (focus == MainFocus::Visualizer) {
                        result.visualize = !result.visualize;
                        saveSettings();
                    } else if (focus == MainFocus::Manipulator) {
                        result.manipulator = !result.manipulator;
                        saveSettings();
                    } else if (focus == MainFocus::IntegerScale) {
                        result.integer_scaling = !result.integer_scaling;
                        saveSettings();
                    } else if (focus == MainFocus::Theme) {
                        theme = nextLauncherTheme(theme);
                        saveSettings();
                        status.clear();
                    }
                    break;
                case SDLK_RETURN:
                    if (focus == MainFocus::RomList || focus == MainFocus::Start) {
                        launchSelectedRom();
                    } else if (focus == MainFocus::Visualizer) {
                        result.visualize = !result.visualize;
                        saveSettings();
                    } else if (focus == MainFocus::Manipulator) {
                        result.manipulator = !result.manipulator;
                        saveSettings();
                    } else if (focus == MainFocus::IntegerScale) {
                        result.integer_scaling = !result.integer_scaling;
                        saveSettings();
                    } else if (focus == MainFocus::Theme) {
                        theme = nextLauncherTheme(theme);
                        saveSettings();
                        status.clear();
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

                syncScrollToSelection();
            }

            if (event.type == SDL_MOUSEWHEEL && view == LauncherView::Main) {
                int mouse_x = 0;
                int mouse_y = 0;
                SDL_GetMouseState(&mouse_x, &mouse_y);
                const SDL_Point point = use_modern_ui
                    ? SDL_Point{mouse_x, mouse_y}
                    : toLogicalPoint(mouse_x, mouse_y);
                if (point.x < 0 || point.y < 0) continue;

                const MainLayout layout;
#ifdef _WIN32
                const SDL_Rect rom_list_rect = use_modern_ui ? makeModernLayout(renderer).rom_list : layout.rom_list;
#else
                const SDL_Rect& rom_list_rect = layout.rom_list;
#endif
                if (pointInRect(point.x, point.y, rom_list_rect) && event.wheel.y != 0) {
                    const int visible_rows = visibleRomRows();
                    rom_scroll -= event.wheel.y;
                    clampRomScroll();
                    if (selected_rom >= 0) {
                        if (selected_rom < rom_scroll) selected_rom = rom_scroll;
                        if (selected_rom >= rom_scroll + visible_rows) {
                            selected_rom = std::min((int)rom_paths.size() - 1, rom_scroll + visible_rows - 1);
                        }
                    }
                    focus = MainFocus::RomList;
                }
            }

            if (event.type == SDL_MOUSEBUTTONDOWN && event.button.button == SDL_BUTTON_LEFT) {
                const SDL_Point point = use_modern_ui
                    ? SDL_Point{event.button.x, event.button.y}
                    : toLogicalPoint(event.button.x, event.button.y);
                if (point.x < 0 || point.y < 0) continue;

                if (view == LauncherView::Controls) {
                    SDL_Rect controls_panel{10, 18, 236, 190};
#ifdef _WIN32
                    if (use_modern_ui) controls_panel = makeModernLayout(renderer).controls_panel;
#endif
                    if (pointInRect(point.x, point.y, controls_panel)) {
                        for (int i = 0; i < (int)kBindings.size(); i++) {
                            SDL_Rect row{18, 52 + i * 12, 220, 10};
#ifdef _WIN32
                            if (use_modern_ui) {
                                const ModernLayout modern = makeModernLayout(renderer);
                                row = SDL_Rect{
                                    modern.origin_x + scaledValue(modern.scale, 18),
                                    modern.origin_y + scaledValue(modern.scale, 54 + i * 12) - scaledValue(modern.scale, 2),
                                    scaledValue(modern.scale, 220),
                                    std::max(18, scaledValue(modern.scale, 12))
                                };
                            }
#endif
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
#ifdef _WIN32
                const ModernLayout modern = makeModernLayout(renderer);
                const SDL_Rect rom_list_rect = use_modern_ui ? modern.rom_list : layout.rom_list;
                const SDL_Rect visualize_box_rect = use_modern_ui ? modern.visualize_box : layout.visualize_box;
                const SDL_Rect manipulator_box_rect = use_modern_ui ? modern.manipulator_box : layout.manipulator_box;
                const SDL_Rect intscale_box_rect = use_modern_ui ? modern.intscale_box : layout.intscale_box;
                const SDL_Rect theme_button_rect = use_modern_ui ? modern.theme_button : layout.theme_button;
                const SDL_Rect controls_button_rect = use_modern_ui ? modern.controls_button : layout.controls_button;
                const SDL_Rect start_button_rect = use_modern_ui ? modern.start_button : layout.start_button;
                const SDL_Rect quit_button_rect = use_modern_ui ? modern.quit_button : layout.quit_button;
                const int rom_row_height = use_modern_ui ? modern.row_height : kRomRowHeight;
                const int visible_rows = use_modern_ui ? modernVisibleRomRows(modern) : classicVisibleRomRows(layout);
#else
                const SDL_Rect& rom_list_rect = layout.rom_list;
                const SDL_Rect& visualize_box_rect = layout.visualize_box;
                const SDL_Rect& manipulator_box_rect = layout.manipulator_box;
                const SDL_Rect& intscale_box_rect = layout.intscale_box;
                const SDL_Rect& theme_button_rect = layout.theme_button;
                const SDL_Rect& controls_button_rect = layout.controls_button;
                const SDL_Rect& start_button_rect = layout.start_button;
                const SDL_Rect& quit_button_rect = layout.quit_button;
                const int rom_row_height = kRomRowHeight;
                const int visible_rows = classicVisibleRomRows(layout);
#endif
                if (pointInRect(point.x, point.y, rom_list_rect)) {
                    const int row = (point.y - rom_list_rect.y) / std::max(1, rom_row_height);
                    const int rom_index = rom_scroll + row;
                    if (row >= 0 && row < visible_rows && rom_index >= 0 && rom_index < (int)rom_paths.size()) {
                        focus = MainFocus::RomList;
                        selected_rom = rom_index;
                        const uint32_t now = SDL_GetTicks();
                        if (last_click_rom == rom_index && now - last_click_ticks < 350) {
                            launchSelectedRom();
                        }
                        last_click_rom = rom_index;
                        last_click_ticks = now;
                    }
                } else if (pointInRect(point.x, point.y, visualize_box_rect)) {
                    focus = MainFocus::Visualizer;
                    result.visualize = !result.visualize;
                    saveSettings();
                } else if (pointInRect(point.x, point.y, manipulator_box_rect)) {
                    focus = MainFocus::Manipulator;
                    result.manipulator = !result.manipulator;
                    saveSettings();
                } else if (pointInRect(point.x, point.y, intscale_box_rect)) {
                    focus = MainFocus::IntegerScale;
                    result.integer_scaling = !result.integer_scaling;
                    saveSettings();
                } else if (pointInRect(point.x, point.y, theme_button_rect)) {
                    focus = MainFocus::Theme;
                    theme = nextLauncherTheme(theme);
                    saveSettings();
                    status.clear();
                } else if (pointInRect(point.x, point.y, controls_button_rect)) {
                    focus = MainFocus::Controls;
                    view = LauncherView::Controls;
                } else if (pointInRect(point.x, point.y, start_button_rect)) {
                    focus = MainFocus::Start;
                    launchSelectedRom();
                } else if (pointInRect(point.x, point.y, quit_button_rect)) {
                    running = false;
                }
            }
        }

        syncScrollToSelection();

        SDL_RenderClear(renderer);
#ifdef _WIN32
        if (use_modern_ui) {
            if (view == LauncherView::Main) {
                renderMainViewModern(renderer,
                                     modern_text,
                                     rom_paths,
                                     selected_rom,
                                     rom_scroll,
                                     focus,
                                     result.visualize,
                                     result.manipulator,
                                     result.integer_scaling,
                                     theme,
                                     status);
            } else {
                renderControlsViewModern(renderer,
                                         modern_text,
                                         result.input_config,
                                         selected_binding,
                                         waiting_for_key,
                                         theme,
                                         status);
            }
        } else
#endif
        {
            if (view == LauncherView::Main) {
                drawMainView(pixels.data(), rom_paths, selected_rom, rom_scroll, focus,
                    result.visualize, result.manipulator, result.integer_scaling, theme, status);
            } else {
                drawControlsView(pixels.data(), result.input_config, selected_binding, waiting_for_key, theme, status);
            }

            SDL_UpdateTexture(texture, nullptr, pixels.data(), kMenuWidth * (int)sizeof(uint32_t));
            SDL_RenderCopy(renderer, texture, nullptr, nullptr);
        }

        SDL_RenderPresent(renderer);
        SDL_Delay(16);
    }

    restoreRendererState();
    return result;
}
