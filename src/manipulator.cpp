#include "manipulator.hpp"

#include "apu.hpp"
#include "ppu.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {
constexpr int kPrettyGlyphWidth = 5;
constexpr int kPrettyGlyphHeight = 7;
constexpr int kPrettyGlyphAdvance = 6;
constexpr uint32_t kBlack = 0xFF000000u;
constexpr uint32_t kPanelFill = 0xFF060606u;
constexpr uint32_t kPanelFillAlt = 0xFF0C0C0Cu;
constexpr uint32_t kPanelBorder = 0xFF808080u;
constexpr uint32_t kPanelHighlight = 0xFFFFFFFFu;
constexpr uint32_t kText = 0xFFFFFFFFu;
constexpr uint32_t kTextSoft = 0xFFD0D0D0u;
constexpr uint32_t kTextMuted = 0xFF8A8A8Au;
constexpr uint32_t kBarFill = 0xFFFFFFFFu;
constexpr uint32_t kBarBack = 0xFF1A1A1Au;

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

const PrettyGlyph* findPrettyGlyph(char ch) {
    for (const PrettyGlyph& glyph : PRETTY_FONT) {
        if (glyph.ch == ch) return &glyph;
    }
    return nullptr;
}

void drawChar(uint32_t* pixels, int stride, int height, int cx, int cy, char ch, uint32_t color) {
    if (const PrettyGlyph* glyph = findPrettyGlyph(ch)) {
        for (int y = 0; y < kPrettyGlyphHeight; y++) {
            for (int x = 0; x < kPrettyGlyphWidth; x++) {
                if (glyph->rows[y][x] == ' ') continue;
                const int px = cx + x;
                const int py = cy + y;
                if (px >= 0 && px < stride && py >= 0 && py < height) {
                    pixels[py * stride + px] = color;
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
            const int px = cx + x;
            const int py = cy + y;
            if (px >= 0 && px < stride && py >= 0 && py < height) {
                pixels[py * stride + px] = color;
            }
        }
    }
}

bool pointInRect(int x, int y, const SDL_Rect& rect) {
    return x >= rect.x && y >= rect.y && x < rect.x + rect.w && y < rect.y + rect.h;
}

SDL_Point logicalPointFromWindow(int logical_w, int logical_h, int x, int y) {
    if (logical_w <= 0 || logical_h <= 0) return {-1, -1};
    if (x < 0 || x >= logical_w || y < 0 || y >= logical_h) return {-1, -1};
    return {x, y};
}

constexpr SDL_Rect kResetButton{24, 20, 156, 28};
constexpr SDL_Rect kFullscreenButton{460, 20, 156, 28};
constexpr SDL_Rect kVoicePanel{24, 68, 284, 388};
constexpr SDL_Rect kLayerPanel{328, 68, 288, 226};
constexpr SDL_Rect kStatusPanel{328, 314, 288, 142};

SDL_Rect voiceCellRect(int index) {
    const int cell_w = 130;
    const int cell_h = 86;
    const int col = index % 2;
    const int row = index / 2;
    return {kVoicePanel.x + 12 + col * 136, kVoicePanel.y + 34 + row * 90, cell_w, cell_h};
}

SDL_Rect mainLayerRect(int index) {
    return {kLayerPanel.x + 132, kLayerPanel.y + 56 + index * 28, 14, 14};
}

SDL_Rect subLayerRect(int index) {
    return {kLayerPanel.x + 210, kLayerPanel.y + 56 + index * 28, 14, 14};
}
}

Manipulator::Manipulator() {
    pixels.fill(kBlack);
}

Manipulator::~Manipulator() {
    destroy();
}

bool Manipulator::init() {
    window = SDL_CreateWindow("Super Furamicom Manipulator",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE);
    if (!window) return false;
    SDL_GetWindowPosition(window, &windowed_bounds.x, &windowed_bounds.y);
    SDL_GetWindowSize(window, &windowed_bounds.w, &windowed_bounds.h);
    SDL_SetWindowMinimumSize(window, MIN_W, MIN_H);

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        window = nullptr;
        return false;
    }

    SDL_RenderSetLogicalSize(renderer, WIN_W, WIN_H);
    refreshScaleMode();
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H);
    if (!texture) {
        destroy();
        return false;
    }

    return true;
}

void Manipulator::show() {
    if (!window) return;
    SDL_ShowWindow(window);
}

void Manipulator::placeBeside(SDL_Window* anchor, int gap) {
    if (!window || !anchor) return;

    int anchor_x = 0;
    int anchor_y = 0;
    int anchor_w = 0;
    int anchor_h = 0;
    SDL_GetWindowPosition(anchor, &anchor_x, &anchor_y);
    SDL_GetWindowSize(anchor, &anchor_w, &anchor_h);

    SDL_Rect usable_bounds{};
    const int display_index = SDL_GetWindowDisplayIndex(anchor);
    const bool have_bounds = display_index >= 0 &&
        SDL_GetDisplayUsableBounds(display_index, &usable_bounds) == 0;

    int target_x = anchor_x + anchor_w + gap;
    int target_y = anchor_y;

    if (have_bounds) {
        if (target_x + WIN_W > usable_bounds.x + usable_bounds.w) {
            target_x = anchor_x - WIN_W - gap;
        }
        if (target_x < usable_bounds.x) {
            target_x = std::max(usable_bounds.x, usable_bounds.x + (usable_bounds.w - WIN_W) / 2);
        }
        target_y = std::clamp(target_y, usable_bounds.y,
            usable_bounds.y + std::max(0, usable_bounds.h - WIN_H));
    }

    SDL_SetWindowPosition(window, target_x, target_y);
}

void Manipulator::destroy() {
    fullscreen = false;
    if (texture) {
        SDL_DestroyTexture(texture);
        texture = nullptr;
    }
    if (renderer) {
        SDL_DestroyRenderer(renderer);
        renderer = nullptr;
    }
    if (window) {
        SDL_DestroyWindow(window);
        window = nullptr;
    }
}

uint32_t Manipulator::getWindowID() const {
    return window ? SDL_GetWindowID(window) : 0;
}

bool Manipulator::processEvent(const SDL_Event& e) {
    if (!window) return false;
    const uint32_t wid = SDL_GetWindowID(window);

    if (e.type == SDL_WINDOWEVENT && e.window.windowID == wid) {
        if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
            destroy();
            return false;
        }
        if (!fullscreen &&
            (e.window.event == SDL_WINDOWEVENT_MOVED ||
             e.window.event == SDL_WINDOWEVENT_RESIZED ||
             e.window.event == SDL_WINDOWEVENT_SIZE_CHANGED)) {
            SDL_GetWindowPosition(window, &windowed_bounds.x, &windowed_bounds.y);
            SDL_GetWindowSize(window, &windowed_bounds.w, &windowed_bounds.h);
            refreshScaleMode();
        }
    }

    if (e.type == SDL_KEYDOWN && e.key.windowID == wid) {
        switch (e.key.keysym.sym) {
        case SDLK_F11:
            setFullscreen(!fullscreen);
            break;
        case SDLK_RETURN:
            if ((e.key.keysym.mod & KMOD_ALT) != 0) {
                setFullscreen(!fullscreen);
            }
            break;
        case SDLK_r:
            desired_voice_mask = 0xFF;
            desired_main_layer_mask = 0x1F;
            desired_sub_layer_mask = 0x1F;
            synced_masks = true;
            break;
        default:
            break;
        }
    }

    if (e.type == SDL_MOUSEBUTTONDOWN &&
        e.button.windowID == wid &&
        e.button.button == SDL_BUTTON_LEFT) {
        const SDL_Point point = toLogicalPoint(e.button.x, e.button.y);
        if (point.x >= 0 && point.y >= 0) {
            handleClick(point.x, point.y);
        }
    }

    return true;
}

void Manipulator::setFullscreen(bool enable) {
    if (!window || fullscreen == enable) return;

    if (enable) {
        SDL_GetWindowPosition(window, &windowed_bounds.x, &windowed_bounds.y);
        SDL_GetWindowSize(window, &windowed_bounds.w, &windowed_bounds.h);
        if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
            fullscreen = true;
            refreshScaleMode();
        }
        return;
    }

    if (SDL_SetWindowFullscreen(window, 0) == 0) {
        fullscreen = false;
        SDL_SetWindowPosition(window, windowed_bounds.x, windowed_bounds.y);
        SDL_SetWindowSize(window, windowed_bounds.w, windowed_bounds.h);
        refreshScaleMode();
    }
}

SDL_Point Manipulator::toLogicalPoint(int window_x, int window_y) const {
    return logicalPointFromWindow(WIN_W, WIN_H, window_x, window_y);
}

void Manipulator::handleClick(int x, int y) {
    if (pointInRect(x, y, kResetButton)) {
        desired_voice_mask = 0xFF;
        desired_main_layer_mask = 0x1F;
        desired_sub_layer_mask = 0x1F;
        synced_masks = true;
        return;
    }
    if (pointInRect(x, y, kFullscreenButton)) {
        setFullscreen(!fullscreen);
        return;
    }

    for (int i = 0; i < 8; i++) {
        if (pointInRect(x, y, voiceCellRect(i))) {
            desired_voice_mask ^= (uint8_t)(1u << i);
            synced_masks = true;
            return;
        }
    }

    for (int i = 0; i < 5; i++) {
        if (pointInRect(x, y, mainLayerRect(i))) {
            desired_main_layer_mask ^= (uint8_t)(1u << i);
            synced_masks = true;
            return;
        }
        if (pointInRect(x, y, subLayerRect(i))) {
            desired_sub_layer_mask ^= (uint8_t)(1u << i);
            synced_masks = true;
            return;
        }
    }
}

void Manipulator::clear(uint32_t color) {
    pixels.fill(color);
}

void Manipulator::drawRect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    for (int py = 0; py < h; py++) {
        const int yy = y + py;
        if (yy < 0 || yy >= WIN_H) continue;
        for (int px = 0; px < w; px++) {
            const int xx = x + px;
            if (xx < 0 || xx >= WIN_W) continue;
            pixels[(std::size_t)yy * WIN_W + xx] = color;
        }
    }
}

void Manipulator::drawRectOutline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 1 || h <= 1) return;
    drawRect(x, y, w, 1, color);
    drawRect(x, y + h - 1, w, 1, color);
    drawRect(x, y, 1, h, color);
    drawRect(x + w - 1, y, 1, h, color);
}

void Manipulator::drawText(int x, int y, const char* text, uint32_t color) {
    if (!text) return;
    for (int i = 0; text[i] != '\0'; i++) {
        drawChar(pixels.data(), WIN_W, WIN_H, x + i * kPrettyGlyphAdvance, y, text[i], color);
    }
}

void Manipulator::drawPanel(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    drawRect(x, y, w, h, fill);
    drawRectOutline(x, y, w, h, border);
    drawRect(x, y, w, 2, border);
}

void Manipulator::drawBackground() {
    clear(kBlack);
    for (int y = 0; y < WIN_H; y += 4) {
        drawRect(0, y, WIN_W, 1, 0xFF050505u);
    }
    for (int x = 0; x < WIN_W; x += 32) {
        drawRect(x, 0, 1, WIN_H, 0xFF050505u);
    }
}

void Manipulator::drawCheckbox(const SDL_Rect& rect, bool checked, bool highlighted) {
    drawRect(rect.x, rect.y, rect.w, rect.h, highlighted ? 0xFF111111u : kBlack);
    drawRectOutline(rect.x, rect.y, rect.w, rect.h, highlighted ? kPanelHighlight : kPanelBorder);
    if (checked) {
        drawRect(rect.x + 3, rect.y + 3, rect.w - 6, rect.h - 6, kPanelHighlight);
    }
}

void Manipulator::refreshScaleMode() {
    if (!renderer || !window) return;
    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(window, &window_w, &window_h);
    const SDL_bool integer_scale = (window_w >= WIN_W && window_h >= WIN_H) ? SDL_TRUE : SDL_FALSE;
    SDL_RenderSetIntegerScale(renderer, integer_scale);
}

void Manipulator::syncMasksFromCore(const APU& apu, const PPU& ppu) {
    if (synced_masks) return;
    desired_voice_mask = apu.getVoiceEnableMask();
    desired_main_layer_mask = ppu.getLiveMainLayerMask();
    desired_sub_layer_mask = ppu.getLiveSubLayerMask();
    synced_masks = true;
}

void Manipulator::applyMasks(APU& apu, PPU& ppu) {
    apu.setVoiceEnableMask(desired_voice_mask);
    ppu.setLiveLayerMasks(desired_main_layer_mask, desired_sub_layer_mask);
}

void Manipulator::drawHeader() {
    drawPanel(24, 20, 156, 28, kPanelFillAlt, kPanelBorder);
    drawText(40, 31, "R RESET ALL", kText);

    drawPanel(460, 20, 156, 28, kPanelFillAlt, kPanelBorder);
    drawText(474, 31, fullscreen ? "F11 WINDOWED" : "F11 FULLSCREEN", kText);

    drawText(24, 8, "SUPER FURAMICOM MANIPULATOR", kText);
}

void Manipulator::drawVoicePanel(const APU& apu) {
    drawPanel(kVoicePanel.x, kVoicePanel.y, kVoicePanel.w, kVoicePanel.h, kPanelFill, kPanelBorder);
    drawText(kVoicePanel.x + 12, kVoicePanel.y + 12, "AUDIO VOICES", kText);

    const auto voices = apu.getVoiceDebug();
    char line[96];
    for (int i = 0; i < 8; i++) {
        const SDL_Rect rect = voiceCellRect(i);
        const bool enabled = (desired_voice_mask & (1u << i)) != 0;
        const bool active = voices[(size_t)i].active;
        drawPanel(rect.x, rect.y, rect.w, rect.h, active ? kPanelFillAlt : kBlack,
            enabled ? kPanelHighlight : kPanelBorder);

        std::snprintf(line, sizeof(line), "VOICE %d", i);
        drawText(rect.x + 10, rect.y + 10, line, kText);
        drawCheckbox(SDL_Rect{rect.x + rect.w - 24, rect.y + 8, 14, 14}, enabled, enabled);

        std::snprintf(line, sizeof(line), "SRC %02X  ENV %03X", voices[(size_t)i].source_number,
            voices[(size_t)i].envelope);
        drawText(rect.x + 10, rect.y + 28, line, kText);

        std::snprintf(line, sizeof(line), "%s  OUT %02X", active ? "ACTIVE" : "IDLE",
            voices[(size_t)i].outx & 0xFF);
        drawText(rect.x + 10, rect.y + 42, line, enabled ? kTextSoft : kTextMuted);

        const int level = std::min(100, std::abs((int)voices[(size_t)i].last_output) * 100 / 32768);
        drawRect(rect.x + 10, rect.y + 60, rect.w - 20, 8, kBarBack);
        drawRect(rect.x + 10, rect.y + 60, ((rect.w - 20) * level) / 100, 8, enabled ? kBarFill : kTextMuted);
    }
}

void Manipulator::drawLayerPanel(const PPU& ppu) {
    drawPanel(kLayerPanel.x, kLayerPanel.y, kLayerPanel.w, kLayerPanel.h, kPanelFill, kPanelBorder);
    drawText(kLayerPanel.x + 12, kLayerPanel.y + 12, "PPU LAYERS", kText);
    drawText(kLayerPanel.x + 132, kLayerPanel.y + 34, "MAIN", kText);
    drawText(kLayerPanel.x + 212, kLayerPanel.y + 34, "SUB", kText);

    static const char* labels[] = {"BG1", "BG2", "BG3", "BG4", "OBJ"};
    for (int i = 0; i < 5; i++) {
        const int y = kLayerPanel.y + 58 + i * 28;
        drawText(kLayerPanel.x + 18, y + 2, labels[i], kText);
        drawCheckbox(mainLayerRect(i), (desired_main_layer_mask & (1u << i)) != 0,
            (ppu.getTMMain() & (1u << i)) != 0);
        drawCheckbox(subLayerRect(i), (desired_sub_layer_mask & (1u << i)) != 0,
            (ppu.getTMSub() & (1u << i)) != 0);
    }

    char line[96];
    std::snprintf(line, sizeof(line), "TM %02X  TS %02X", ppu.getTMMain(), ppu.getTMSub());
    drawText(kLayerPanel.x + 18, kLayerPanel.y + 206, line, kTextSoft);
}

void Manipulator::drawStatusPanel(const APU& apu, const PPU& ppu) {
    drawPanel(kStatusPanel.x, kStatusPanel.y, kStatusPanel.w, kStatusPanel.h, kPanelFill, kPanelBorder);
    drawText(kStatusPanel.x + 12, kStatusPanel.y + 12, "LIVE STATUS", kText);

    char line[128];
    std::snprintf(line, sizeof(line), "VOICE MASK %02X  ACTIVE %d",
        desired_voice_mask, apu.getActiveVoiceCount());
    drawText(kStatusPanel.x + 12, kStatusPanel.y + 36, line, kText);

    std::snprintf(line, sizeof(line), "MAIN MASK %02X  SUB MASK %02X",
        desired_main_layer_mask, desired_sub_layer_mask);
    drawText(kStatusPanel.x + 12, kStatusPanel.y + 54, line, kText);

    std::snprintf(line, sizeof(line), "EFFECTIVE TM %02X  TS %02X",
        ppu.getEffectiveTMMain(), ppu.getEffectiveTMSub());
    drawText(kStatusPanel.x + 12, kStatusPanel.y + 72, line, kText);

    drawText(kStatusPanel.x + 12, kStatusPanel.y + 102,
        "CLICK BOXES TO MUTE VOICES OR HIDE LAYERS", kTextSoft);
    drawText(kStatusPanel.x + 12, kStatusPanel.y + 118,
        "CHECKED BOXES STAY LIVE WHILE THE GAME RUNS", kTextSoft);
}

void Manipulator::update(APU& apu, PPU& ppu) {
    if (!window || !renderer || !texture) return;

    syncMasksFromCore(apu, ppu);
    applyMasks(apu, ppu);

    drawBackground();
    drawHeader();
    drawVoicePanel(apu);
    drawLayerPanel(ppu);
    drawStatusPanel(apu, ppu);

    SDL_UpdateTexture(texture, nullptr, pixels.data(), WIN_W * (int)sizeof(uint32_t));
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}
