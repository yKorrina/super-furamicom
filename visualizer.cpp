#include "visualizer.hpp"

#include "apu.hpp"
#include "cpu.hpp"
#include "ppu.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {
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
}

Visualizer::Visualizer() {
    pixels.fill(0xFF04030A);
    spectrogram.fill(0xFF05030A);
}

Visualizer::~Visualizer() {
    destroy();
}

bool Visualizer::init() {
    window = SDL_CreateWindow("Super Furamicom Debugger",
        SDL_WINDOWPOS_CENTERED + 320, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!window) return false;

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) {
        SDL_DestroyWindow(window);
        window = nullptr;
        return false;
    }

    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H);
    if (!texture) {
        destroy();
        return false;
    }

    return true;
}

void Visualizer::destroy() {
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

uint32_t Visualizer::getWindowID() const {
    return window ? SDL_GetWindowID(window) : 0;
}

bool Visualizer::processEvent(const SDL_Event& e) {
    if (!window) return false;
    const uint32_t wid = SDL_GetWindowID(window);

    if (e.type == SDL_WINDOWEVENT && e.window.windowID == wid) {
        if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
            destroy();
            return false;
        }
    }

    if (e.type == SDL_KEYDOWN && e.key.windowID == wid) {
        switch (e.key.keysym.sym) {
        case SDLK_1: active_panel = 0; break;
        case SDLK_2: active_panel = 1; break;
        case SDLK_3: active_panel = 2; break;
        case SDLK_TAB: active_panel = (active_panel + 1) % 3; break;
        default: break;
        }
    }

    return true;
}

float Visualizer::clamp01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

uint32_t Visualizer::lerpColor(uint32_t a, uint32_t b, float t) {
    t = clamp01(t);
    auto lerp = [t](int lhs, int rhs) {
        return lhs + (int)((rhs - lhs) * t);
    };

    const int ar = (a >> 16) & 0xFF;
    const int ag = (a >> 8) & 0xFF;
    const int ab = a & 0xFF;
    const int br = (b >> 16) & 0xFF;
    const int bg = (b >> 8) & 0xFF;
    const int bb = b & 0xFF;
    return 0xFF000000u |
           ((uint32_t)lerp(ar, br) << 16) |
           ((uint32_t)lerp(ag, bg) << 8) |
           (uint32_t)lerp(ab, bb);
}

uint32_t Visualizer::heatColor(float t) {
    static const uint32_t kStops[] = {
        0xFF04030A, 0xFF1C1241, 0xFF6D0A77, 0xFFD5005F,
        0xFFFF3A00, 0xFFFFA500, 0xFFFFF4A8,
    };

    t = clamp01(t);
    const float scaled = t * 6.0f;
    const int index = std::min(5, (int)scaled);
    return lerpColor(kStops[index], kStops[index + 1], scaled - index);
}

uint32_t Visualizer::snesColorToARGB(uint16_t c15) {
    uint8_t r = (uint8_t)((c15 & 0x001F) << 3); r |= (r >> 5);
    uint8_t g = (uint8_t)(((c15 >> 5) & 0x1F) << 3); g |= (g >> 5);
    uint8_t b = (uint8_t)(((c15 >> 10) & 0x1F) << 3); b |= (b >> 5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void Visualizer::putPixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) {
        pixels[(size_t)y * WIN_W + x] = color;
    }
}

void Visualizer::drawRect(int x, int y, int w, int h, uint32_t color) {
    if (w <= 0 || h <= 0) return;
    for (int py = 0; py < h; py++) {
        const int yy = y + py;
        if (yy < 0 || yy >= WIN_H) continue;
        for (int px = 0; px < w; px++) {
            const int xx = x + px;
            if (xx < 0 || xx >= WIN_W) continue;
            pixels[(size_t)yy * WIN_W + xx] = color;
        }
    }
}

void Visualizer::drawRectOutline(int x, int y, int w, int h, uint32_t color) {
    if (w <= 1 || h <= 1) return;
    drawRect(x, y, w, 1, color);
    drawRect(x, y + h - 1, w, 1, color);
    drawRect(x, y, 1, h, color);
    drawRect(x + w - 1, y, 1, h, color);
}

void Visualizer::drawText(int x, int y, const char* text, uint32_t color) {
    if (!text) return;
    for (int i = 0; text[i] != '\0'; i++) {
        drawChar(pixels.data(), WIN_W, WIN_H, x + i * 5, y, text[i], color);
    }
}

void Visualizer::clear(uint32_t color) {
    pixels.fill(color);
}

void Visualizer::drawBackground() {
    for (int y = 0; y < WIN_H; y++) {
        const float t = (float)y / (float)(WIN_H - 1);
        const uint32_t row_color = lerpColor(0xFF030209, 0xFF120412, t);
        for (int x = 0; x < WIN_W; x++) {
            pixels[(size_t)y * WIN_W + x] = row_color;
        }
    }

    for (int y = 0; y < WIN_H; y += 4) {
        drawRect(0, y, WIN_W, 1, 0x12000000);
    }
    for (int x = 0; x < WIN_W; x += 48) {
        drawRect(x, 0, 1, WIN_H, 0x12000000);
    }
}

void Visualizer::drawPanel(int x, int y, int w, int h, uint32_t fill, uint32_t border) {
    drawRect(x, y, w, h, fill);
    drawRectOutline(x, y, w, h, border);
    drawRect(x, y, w, 2, lerpColor(border, 0xFFFFFFFFu, 0.25f));
}

void Visualizer::drawPanelTitle(int x, int y, const char* text, uint32_t color) {
    drawText(x, y, text, color);
}

void Visualizer::drawTabs() {
    static const char* labels[] = { "1 CPU", "2 PPU", "3 APU" };
    for (int i = 0; i < 3; i++) {
        const int tab_x = 24 + i * 112;
        const uint32_t fill = i == active_panel ? 0xFF26081A : 0xFF120B19;
        const uint32_t border = i == active_panel ? heatColor(0.72f) : 0xFF4D2C58;
        drawPanel(tab_x, 20, 96, 30, fill, border);
        drawText(tab_x + 14, 31, labels[i], i == active_panel ? 0xFFFFE9B4 : 0xFFBA8BC8);
    }

    drawText(400, 31, "TAB CYCLES VIEWS", 0xFF9A6CA7);
}

void Visualizer::drawFramePreview(const PPU& ppu, int x, int y, int scale) {
    const uint32_t* frame = ppu.getFramebuffer();
    if (!frame || scale <= 0) return;

    for (int py = 0; py < 224; py++) {
        for (int px = 0; px < 256; px++) {
            const uint32_t color = frame[(size_t)py * 256 + px];
            drawRect(x + px * scale, y + py * scale, scale, scale, color);
        }
    }
}

void Visualizer::drawPaletteStrip(const PPU& ppu, int x, int y, int swatch, int cols) {
    const uint8_t* cgram = ppu.getCGRAMData();
    if (!cgram || swatch <= 0 || cols <= 0) return;

    for (int i = 0; i < 256; i++) {
        const uint16_t c15 = (uint16_t)cgram[i * 2] | ((uint16_t)cgram[i * 2 + 1] << 8);
        const int px = x + (i % cols) * swatch;
        const int py = y + (i / cols) * swatch;
        drawRect(px, py, swatch - 1, swatch - 1, snesColorToARGB(c15));
    }
}

void Visualizer::drawCPUHistoryPlot(const CPU& cpu, int x, int y, int w, int h) {
    const size_t count = cpu.hasHistoryWrapped() ? CPU::kDebugHistory : cpu.getHistoryPos();
    if (count < 2) return;

    const size_t start = cpu.hasHistoryWrapped() ? cpu.getHistoryPos() : 0;
    auto histIndex = [start, count](size_t i) {
        return (start + i) % CPU::kDebugHistory;
    };

    auto drawTrace = [this, x, y, w, h, count](auto fetch, uint32_t color, float scale) {
        int prev_x = x;
        int prev_y = y + h / 2;
        for (size_t i = 0; i < count; i++) {
            const float t = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
            const int plot_x = x + (int)(t * (float)(w - 1));
            const int plot_y = y + h - 1 - (int)(fetch(i) * scale * (float)(h - 1));
            const int dx = plot_x - prev_x;
            const int dy = plot_y - prev_y;
            const int steps = std::max(std::abs(dx), std::abs(dy));
            for (int step = 0; step <= steps; step++) {
                const float s = steps > 0 ? (float)step / (float)steps : 0.0f;
                putPixel(prev_x + (int)(dx * s), prev_y + (int)(dy * s), color);
            }
            prev_x = plot_x;
            prev_y = plot_y;
        }
    };

    for (int gy = 0; gy <= 4; gy++) {
        const int yy = y + (gy * (h - 1)) / 4;
        drawRect(x, yy, w, 1, 0xFF28172C);
    }

    drawTrace([&](size_t i) {
        return (float)(cpu.getPCHistory()[histIndex(i)] & 0xFFFF) / 65535.0f;
    }, heatColor(0.95f), 1.0f);
    drawTrace([&](size_t i) {
        return (float)cpu.getAHistory()[histIndex(i)] / 65535.0f;
    }, 0xFFFF4D6D, 1.0f);
    drawTrace([&](size_t i) {
        return (float)cpu.getXHistory()[histIndex(i)] / 65535.0f;
    }, 0xFFFFA54A, 1.0f);
    drawTrace([&](size_t i) {
        return (float)cpu.getYHistory()[histIndex(i)] / 65535.0f;
    }, 0xFFFFE9B4, 1.0f);
}

void Visualizer::drawWaveform(const APU& apu, int x, int y, int w, int h) {
    const auto& history = apu.getRecentAudioMono();
    const size_t available = apu.hasRecentAudioHistory() ? history.size() : apu.getRecentAudioWritePos();
    if (available < 2) return;

    drawRect(x, y + h / 2, w, 1, 0xFF321B33);

    const size_t write_pos = apu.getRecentAudioWritePos();
    auto sampleAt = [&](size_t i) {
        if (apu.hasRecentAudioHistory()) {
            const size_t idx = (write_pos + history.size() - available + i) % history.size();
            return history[idx];
        }
        return history[i];
    };

    int prev_x = x;
    int prev_y = y + h / 2;
    for (int px = 0; px < w; px++) {
        const size_t idx = (size_t)((double)px * (double)(available - 1) / (double)std::max(1, w - 1));
        const float normalized = (float)sampleAt(idx) / 32768.0f;
        const int py = y + h / 2 - (int)(normalized * (float)(h / 2 - 4));
        const int dx = (x + px) - prev_x;
        const int dy = py - prev_y;
        const int steps = std::max(std::abs(dx), std::abs(dy));
        for (int step = 0; step <= steps; step++) {
            const float s = steps > 0 ? (float)step / (float)steps : 0.0f;
            putPixel(prev_x + (int)(dx * s), prev_y + (int)(dy * s), heatColor(0.62f));
        }
        prev_x = x + px;
        prev_y = py;
    }
}

void Visualizer::updateSpectrogram(const APU& apu) {
    if (!apu.hasRecentAudioHistory()) return;
    if (apu.getGeneratedAudioFrames() == last_audio_frame) return;
    last_audio_frame = apu.getGeneratedAudioFrames();

    for (int row = 0; row < SPECTROGRAM_H; row++) {
        std::memmove(&spectrogram[(size_t)row * SPECTROGRAM_W],
            &spectrogram[(size_t)row * SPECTROGRAM_W + 1],
            sizeof(uint32_t) * (SPECTROGRAM_W - 1));
    }

    const auto& history = apu.getRecentAudioMono();
    const size_t hist_size = history.size();
    const size_t write_pos = apu.getRecentAudioWritePos();
    constexpr int window_size = 256;
    const float max_freq = (float)APU::kOutputHz * 0.5f;

    for (int y = 0; y < SPECTROGRAM_H; y++) {
        const float norm = 1.0f - ((float)y / (float)(SPECTROGRAM_H - 1));
        const float freq = 30.0f * std::pow(max_freq / 30.0f, norm);
        const float omega = 2.0f * 3.1415926535f * freq / (float)APU::kOutputHz;
        const float coeff = 2.0f * std::cos(omega);
        float s_prev = 0.0f;
        float s_prev2 = 0.0f;

        for (int n = 0; n < window_size; n++) {
            const size_t idx = (write_pos + hist_size - window_size + n) % hist_size;
            const float sample = (float)history[idx] / 32768.0f;
            const float s = sample + coeff * s_prev - s_prev2;
            s_prev2 = s_prev;
            s_prev = s;
        }

        const float power = s_prev2 * s_prev2 + s_prev * s_prev - coeff * s_prev * s_prev2;
        const float db = 10.0f * std::log10(1.0e-6f + power);
        const float normalized = clamp01((db + 38.0f) / 28.0f);
        spectrogram[(size_t)y * SPECTROGRAM_W + (SPECTROGRAM_W - 1)] = heatColor(normalized);
    }
}

void Visualizer::drawSpectrogram(int x, int y, int w, int h) {
    for (int py = 0; py < h; py++) {
        const int src_y = std::clamp((py * SPECTROGRAM_H) / std::max(1, h), 0, SPECTROGRAM_H - 1);
        for (int px = 0; px < w; px++) {
            const int src_x = std::clamp((px * SPECTROGRAM_W) / std::max(1, w), 0, SPECTROGRAM_W - 1);
            putPixel(x + px, y + py, spectrogram[(size_t)src_y * SPECTROGRAM_W + src_x]);
        }
    }
}

void Visualizer::drawVoiceMeters(const APU& apu, int x, int y, int w, int h) {
    const auto voices = apu.getVoiceDebug();
    const int cols = 2;
    const int rows = 4;
    const int cell_w = (w - 12) / cols;
    const int cell_h = (h - 16) / rows;

    for (int i = 0; i < 8; i++) {
        const int cx = x + 4 + (i % cols) * cell_w;
        const int cy = y + 4 + (i / cols) * cell_h;
        const auto& voice = voices[i];
        drawPanel(cx, cy, cell_w - 6, cell_h - 6,
            voice.active ? 0xFF140D1B : 0xFF0C0911,
            voice.active ? heatColor(0.55f) : 0xFF34223F);

        char line[64];
        std::snprintf(line, sizeof(line), "V%d SRC:%02X P:%04X", i, voice.source_number, voice.pitch);
        drawText(cx + 8, cy + 8, line, voice.active ? 0xFFFFE1B0 : 0xFF84678F);

        std::snprintf(line, sizeof(line), "ENV:%03X OUT:%02X", voice.envelope, voice.outx & 0xFF);
        drawText(cx + 8, cy + 20, line, 0xFFCC97D6);

        const float env_fill = (float)voice.envelope / 2047.0f;
        const float out_fill = std::min(1.0f, std::abs((int)voice.last_output) / 32768.0f);
        drawRect(cx + 8, cy + 34, cell_w - 24, 10, 0xFF170F1E);
        drawRect(cx + 8, cy + 34, (int)((cell_w - 24) * env_fill), 10, heatColor(env_fill));
        drawRect(cx + 8, cy + 50, cell_w - 24, 8, 0xFF170F1E);
        drawRect(cx + 8, cy + 50, (int)((cell_w - 24) * out_fill), 8, heatColor(out_fill));

        char flags[32];
        std::snprintf(flags, sizeof(flags), "%s%s%s L:%d R:%d",
            voice.releasing ? "REL " : "",
            voice.noise_enabled ? "NON " : "",
            voice.pitch_mod_enabled ? "PMON " : "",
            voice.volume_left, voice.volume_right);
        drawText(cx + 8, cy + 64, flags, 0xFFA87BB8);
    }
}

void Visualizer::drawCPUView(const CPU& cpu) {
    drawPanel(24, 68, 288, 238, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(36, 80, "65816 CORE", 0xFFFFE9B4);

    char line[128];
    std::snprintf(line, sizeof(line), "PC %06X  OP %02X", cpu.getProgramCounter(), cpu.getLastOpcode());
    drawText(36, 102, line, 0xFFFFA54A);
    std::snprintf(line, sizeof(line), "A %04X  X %04X  Y %04X", cpu.getA(), cpu.getX(), cpu.getY());
    drawText(36, 116, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "SP %04X D %04X DB %02X", cpu.getSP(), cpu.getD(), cpu.getDB());
    drawText(36, 130, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "P %02X  E %d  HALT %d", cpu.getP(), cpu.isEmulationMode() ? 1 : 0, cpu.isHalted() ? 1 : 0);
    drawText(36, 144, line, 0xFFDDA0E6);

    drawText(36, 176, "RECENT FLOW", 0xFFCC97D6);
    const size_t count = cpu.hasHistoryWrapped() ? CPU::kDebugHistory : cpu.getHistoryPos();
    const size_t start = cpu.hasHistoryWrapped() ? cpu.getHistoryPos() : 0;
    const size_t show = std::min<size_t>(8, count);
    for (size_t i = 0; i < show; i++) {
        const size_t idx = (start + count - show + i) % CPU::kDebugHistory;
        std::snprintf(line, sizeof(line), "%06X A%04X X%04X Y%04X",
            cpu.getPCHistory()[idx], cpu.getAHistory()[idx], cpu.getXHistory()[idx], cpu.getYHistory()[idx]);
        drawText(36, 190 + (int)i * 12, line, 0xFFB886C0);
    }

    drawPanel(328, 68, 608, 238, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(340, 80, "EXECUTION TRACE", 0xFFFFE9B4);
    drawCPUHistoryPlot(cpu, 340, 104, 584, 186);
    drawText(340, 286, "PC  A  X  Y", 0xFF8F6A99);

    drawPanel(24, 328, 912, 368, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(36, 340, "CPU ACTIVITY MAP", 0xFFFFE9B4);
    drawCPUHistoryPlot(cpu, 36, 364, 888, 320);
}

void Visualizer::drawPPUView(const PPU& ppu) {
    drawPanel(24, 68, 560, 500, 0xFF09070F, 0xFF4E2A58);
    drawPanelTitle(36, 80, "FRAME PREVIEW", 0xFFFFE9B4);
    drawFramePreview(ppu, 36, 104, 2);

    drawPanel(608, 68, 328, 248, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(620, 80, "PPU STATE", 0xFFFFE9B4);

    char line[128];
    std::snprintf(line, sizeof(line), "MODE %d  INIDISP %02X", ppu.getBGMode() & 0x07, ppu.getINIDISP());
    drawText(620, 102, line, 0xFFFFA54A);
    std::snprintf(line, sizeof(line), "TM %02X TS %02X TMW %02X TSW %02X",
        ppu.getTMMain(), ppu.getTMSub(), ppu.getTMW(), ppu.getTSW());
    drawText(620, 116, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "CGWSEL %02X CGADSUB %02X FIX %04X",
        ppu.getCGWSEL(), ppu.getCGADSUB(), ppu.getFixedColor());
    drawText(620, 130, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "W12 %02X W34 %02X WOBJ %02X",
        ppu.getW12SEL(), ppu.getW34SEL(), ppu.getWOBJSEL());
    drawText(620, 144, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "WH %02X %02X %02X %02X",
        ppu.getWH0(), ppu.getWH1(), ppu.getWH2(), ppu.getWH3());
    drawText(620, 158, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "WLOG %02X OBJLOG %02X",
        ppu.getWBGLOG(), ppu.getWOBJLOG());
    drawText(620, 172, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "VRAM %zu  CGRAM %zu  OAM %zu",
        ppu.countNonZeroVRAM(), ppu.countNonZeroCGRAM(), ppu.countNonZeroOAM());
    drawText(620, 186, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "PIXELS %zu  VBL %d",
        ppu.countNonBlackPixels(), ppu.getVBlank() ? 1 : 0);
    drawText(620, 200, line, 0xFFDDA0E6);

    for (int bg = 0; bg < 4; bg++) {
        std::snprintf(line, sizeof(line), "BG%d SC %02X HOFS %04X VOFS %04X",
            bg + 1, ppu.getBGSC(bg), ppu.getBGHOFS(bg), ppu.getBGVOFS(bg));
        drawText(620, 224 + bg * 12, line, 0xFFB886C0);
    }

    drawPanel(608, 340, 328, 228, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(620, 352, "CGRAM", 0xFFFFE9B4);
    drawPaletteStrip(ppu, 620, 376, 18, 16);

    drawPanel(24, 588, 912, 108, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(36, 600, "LAYER AND WINDOW DIAGNOSTICS", 0xFFFFE9B4);
    std::snprintf(line, sizeof(line), "BG12NBA %02X BG34NBA %02X VMAIN %02X VRAM %04X CGRAM %02X OAM %04X",
        ppu.getBGNBA(0), ppu.getBGNBA(1), ppu.getVMAIN(),
        ppu.getVRAMAddress(), ppu.getCGRAMAddress(), ppu.getOAMAddress());
    drawText(36, 624, line, 0xFFDDA0E6);
}

void Visualizer::drawAPUView(const APU& apu) {
    updateSpectrogram(apu);

    drawPanel(24, 68, 620, 276, 0xFF09070F, 0xFF4E2A58);
    drawPanelTitle(36, 80, "LIVE SPECTROGRAM", 0xFFFFE9B4);
    drawSpectrogram(36, 104, 596, 224);

    drawPanel(24, 360, 620, 172, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(36, 372, "WAVEFORM", 0xFFFFE9B4);
    drawWaveform(apu, 36, 396, 596, 120);

    drawPanel(668, 68, 268, 190, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(680, 80, "SPC700 STATE", 0xFFFFE9B4);

    char line[160];
    std::snprintf(line, sizeof(line), "PC %04X A %02X X %02X Y %02X",
        apu.getSPCPC(), apu.getSPCA(), apu.getSPCX(), apu.getSPCY());
    drawText(680, 102, line, 0xFFFFA54A);
    std::snprintf(line, sizeof(line), "SP %02X PSW %02X DSP %02X",
        apu.getSPCSP(), apu.getSPCPSW(), apu.getDSPAddress());
    drawText(680, 116, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "CTRL %02X TEST %02X NOISE %04X",
        apu.getControlReg(), apu.getTestReg(), (uint16_t)apu.getNoiseSample() & 0xFFFF);
    drawText(680, 130, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "UP %d BYTES %zu EXEC %04X",
        apu.hasUploadedProgram() ? 1 : 0, apu.getUploadedBytes(), apu.getExecuteAddress());
    drawText(680, 144, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "READY %d RUN %d BAD %d",
        apu.isDriverReady() ? 1 : 0, apu.isUserCodeRunning() ? 1 : 0, apu.hasUnsupportedOpcode() ? 1 : 0);
    drawText(680, 158, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "FRAMES %llu ACTIVE %llu",
        (unsigned long long)apu.getGeneratedAudioFrames(),
        (unsigned long long)apu.getNonZeroAudioFrames());
    drawText(680, 172, line, 0xFFDDA0E6);
    std::snprintf(line, sizeof(line), "PEAK %d KON %02X FLG %02X",
        apu.getAudioPeakSample(), apu.getDSPRegister(0x4C), apu.getDSPRegister(0x6C));
    drawText(680, 186, line, 0xFFDDA0E6);

    drawPanel(668, 276, 268, 256, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(680, 288, "VOICE METERS", 0xFFFFE9B4);
    drawVoiceMeters(apu, 676, 304, 252, 220);

    drawPanel(24, 548, 912, 148, 0xFF0E0914, 0xFF4E2A58);
    drawPanelTitle(36, 560, "PORTS AND TIMERS", 0xFFFFE9B4);

    const auto& cpu_ports = apu.getCpuToSPCPorts();
    const auto& spc_ports = apu.getSPCToCpuPorts();
    std::snprintf(line, sizeof(line), "CPU->SPC %02X %02X %02X %02X    SPC->CPU %02X %02X %02X %02X",
        cpu_ports[0], cpu_ports[1], cpu_ports[2], cpu_ports[3],
        spc_ports[0], spc_ports[1], spc_ports[2], spc_ports[3]);
    drawText(36, 586, line, 0xFFDDA0E6);

    const auto timers = apu.getTimerDebug();
    for (int i = 0; i < 3; i++) {
        std::snprintf(line, sizeof(line), "T%d EN %d TARGET %02X ST2 %02X ST3 %02X DIV %d PERIOD %d",
            i, timers[i].enabled ? 1 : 0, timers[i].target, timers[i].stage2,
            timers[i].stage3, timers[i].divider, timers[i].period);
        drawText(36, 606 + i * 14, line, 0xFFB886C0);
    }
}

void Visualizer::update(const CPU& cpu, const PPU& ppu, const APU& apu) {
    if (!window || !renderer || !texture) return;

    drawBackground();
    drawTabs();

    switch (active_panel) {
    case 0:
        drawCPUView(cpu);
        break;
    case 1:
        drawPPUView(ppu);
        break;
    case 2:
    default:
        drawAPUView(apu);
        break;
    }

    SDL_UpdateTexture(texture, nullptr, pixels.data(), WIN_W * (int)sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, nullptr, nullptr);
    SDL_RenderPresent(renderer);
}
