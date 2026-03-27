#include "visualizer.hpp"
#include "ppu.hpp"
#include <cstring>
#include <cstdio>

namespace {
inline uint32_t bgCharBaseWordAddress(uint8_t nibble) {
    return (uint32_t)(nibble & 0x0F) * 0x1000;
}

inline uint16_t wrapVRAMWordAddress(uint32_t word_address) {
    return (uint16_t)(word_address & 0x7FFF);
}
}

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

static void drawChar(uint32_t* px, int stride, int cx, int cy, char ch, uint32_t col) {
    if (ch < 32 || ch > 127) return;
    uint32_t glyph = MINI_FONT[ch - 32];
    for (int y = 0; y < 6; y++)
        for (int x = 0; x < 4; x++) {
            if ((glyph >> (23 - (y*4+x))) & 1) {
                int px_x = cx+x, px_y = cy+y;
                if (px_x >= 0 && px_x < stride && px_y >= 0 && px_y < 640)
                    px[px_y*stride + px_x] = col;
            }
        }
}

Visualizer::Visualizer() { memset(pixels, 0, sizeof(pixels)); }
Visualizer::~Visualizer() { destroy(); }

bool Visualizer::init() {
    window = SDL_CreateWindow("Super Furamicom — PPU Visualizer",
        SDL_WINDOWPOS_CENTERED + 400, SDL_WINDOWPOS_CENTERED,
        WIN_W, WIN_H, SDL_WINDOW_SHOWN);
    if (!window) return false;
    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) { SDL_DestroyWindow(window); window = nullptr; return false; }
    texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING, WIN_W, WIN_H);
    if (!texture) { destroy(); return false; }
    return true;
}

void Visualizer::destroy() {
    if (texture)  { SDL_DestroyTexture(texture);  texture = nullptr; }
    if (renderer) { SDL_DestroyRenderer(renderer); renderer = nullptr; }
    if (window)   { SDL_DestroyWindow(window);     window = nullptr; }
}

uint32_t Visualizer::getWindowID() const {
    return window ? SDL_GetWindowID(window) : 0;
}

// Called by main loop for events targeting this window
bool Visualizer::processEvent(const SDL_Event& e) {
    if (!window) return false;
    uint32_t wid = SDL_GetWindowID(window);

    if (e.type == SDL_WINDOWEVENT && e.window.windowID == wid) {
        if (e.window.event == SDL_WINDOWEVENT_CLOSE) {
            destroy();
            return false;
        }
    }
    if (e.type == SDL_KEYDOWN && e.key.windowID == wid) {
        if (e.key.keysym.sym == SDLK_1) active_panel = 0;
        if (e.key.keysym.sym == SDLK_2) active_panel = 1;
        if (e.key.keysym.sym == SDLK_3) active_panel = 2;
    }
    return true;
}

uint32_t Visualizer::snesColorToARGB(uint16_t c15) {
    uint8_t r = (uint8_t)((c15 & 0x001F) << 3); r |= (r >> 5);
    uint8_t g = (uint8_t)(((c15 >> 5) & 0x1F) << 3); g |= (g >> 5);
    uint8_t b = (uint8_t)(((c15 >> 10) & 0x1F) << 3); b |= (b >> 5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void Visualizer::putPixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < WIN_W && y >= 0 && y < WIN_H) pixels[y*WIN_W + x] = color;
}

void Visualizer::drawRect(int x, int y, int w, int h, uint32_t color) {
    for (int dy = 0; dy < h; dy++) for (int dx = 0; dx < w; dx++) putPixel(x+dx, y+dy, color);
}

void Visualizer::drawText(int x, int y, const char* text, uint32_t color) {
    for (int i = 0; text[i]; i++) drawChar(pixels, WIN_W, x+i*5, y, text[i], color);
}

void Visualizer::decodeTile(const uint8_t* vram, size_t vram_size,
                            uint32_t tile_word, int bpp,
                            const uint8_t* cgram, int palette,
                            int dest_x, int dest_y) {
    int wpt = (bpp == 2) ? 8 : (bpp == 4) ? 16 : 32;
    int cpp = (bpp == 2) ? 4 : (bpp == 4) ? 16 : 256;
    for (int row = 0; row < 8; row++) {
        uint32_t bp01 = wrapVRAMWordAddress(tile_word + row);
        uint8_t b0 = vram[bp01*2], b1 = vram[bp01*2+1], b2=0, b3=0;
        if (bpp >= 4) {
            uint32_t bp23 = wrapVRAMWordAddress(tile_word + 8 + row);
            b2 = vram[bp23*2];
            b3 = vram[bp23*2+1];
        }
        for (int px = 0; px < 8; px++) {
            int bit = 7 - px;
            uint8_t ci = ((b0>>bit)&1) | (((b1>>bit)&1)<<1);
            if (bpp >= 4) ci |= (((b2>>bit)&1)<<2) | (((b3>>bit)&1)<<3);
            uint32_t color;
            if (ci == 0) { color = 0xFF101018; }
            else {
                int cg = palette*cpp + ci;
                if (cg*2+1 < 512) { uint16_t c15 = cgram[cg*2]|(cgram[cg*2+1]<<8); color = snesColorToARGB(c15); }
                else color = 0xFFFF00FF;
            }
            putPixel(dest_x+px, dest_y+row, color);
        }
    }
}

void Visualizer::drawTileGrid(const uint8_t* vram, size_t vram_size,
                              const uint8_t* cgram, int bpp) {
    int wpt = (bpp == 2) ? 8 : (bpp == 4) ? 16 : 32;
    int total = (int)(vram_size / 2) / wpt;
    if (total > 1024) total = 1024;
    int cols = WIN_W / 9;
    for (int i = 0; i < total; i++) {
        int gx = (i % cols) * 9, gy = 20 + (i / cols) * 9;
        if (gy + 8 >= WIN_H) break;
        decodeTile(vram, vram_size, (uint32_t)i * wpt, bpp, cgram, 0, gx, gy);
    }
}

void Visualizer::drawPalette(const uint8_t* cgram, size_t cgram_size) {
    int sz = 16, cols = WIN_W / sz;
    for (int i = 0; i < 256 && i*2+1 < (int)cgram_size; i++) {
        uint16_t c15 = cgram[i*2] | (cgram[i*2+1] << 8);
        drawRect((i%cols)*sz, 20+(i/cols)*sz, sz-1, sz-1, snesColorToARGB(c15));
    }
    char buf[8];
    for (int p = 0; p < 16; p++) { snprintf(buf, sizeof(buf), "P%X", p); drawText(0, 20+p*sz+4, buf, 0xFF808080); }
}

void Visualizer::drawBGMap(const PPU& ppu, int bg_num) {
    const uint8_t* vram = ppu.getVRAMData();
    const uint8_t* cgram = ppu.getCGRAMData();
    size_t vs = ppu.getVRAMSize();
    uint8_t sc = ppu.getBGSC(bg_num);
    uint32_t map_base = ((sc >> 2) & 0x3F) * 0x200;
    int bpp = ((ppu.getBGMode() & 7) == 0) ? 2 : 4;
    uint8_t nba = (bg_num < 2) ? ppu.getBGNBA(0) : ppu.getBGNBA(1);
    uint32_t tb = bgCharBaseWordAddress((bg_num & 1) ? ((nba >> 4) & 0x0F) : (nba & 0x0F));
    int wpt = (bpp == 2) ? 8 : 16, pe = (bpp == 2) ? 4 : 16;
    int scale = 4, ox = 10, oy = 30;

    for (int ty = 0; ty < 32; ty++) for (int tx = 0; tx < 32; tx++) {
        uint32_t mw = map_base + ty * 32 + tx;
        if (mw*2+1 >= vs) continue;
        uint16_t entry = vram[mw*2] | (vram[mw*2+1] << 8);
        uint16_t tnum = entry & 0x03FF;
        uint8_t pal = (entry >> 10) & 0x07;
        uint32_t tw = wrapVRAMWordAddress(tb + tnum * wpt);
        uint8_t b0 = vram[tw*2], b1 = vram[tw*2+1];
        int bit = 4;
        uint8_t ci = ((b0>>bit)&1) | (((b1>>bit)&1)<<1);
        if (bpp >= 4) {
            uint32_t tw23 = wrapVRAMWordAddress(tw + 8);
            uint8_t b2 = vram[tw23*2], b3 = vram[tw23*2+1];
            ci |= (((b2>>bit)&1)<<2) | (((b3>>bit)&1)<<3);
        }
        uint32_t color = 0xFF080810;
        if (ci) {
            int cg = pal * pe + ci;
            if (cg*2+1 < 512) { uint16_t c15 = cgram[cg*2]|(cgram[cg*2+1]<<8); color = snesColorToARGB(c15); }
        }
        drawRect(ox+tx*scale, oy+ty*scale, scale, scale, color);
    }

    // Viewport outline
    uint16_t hofs = ppu.getBGHOFS(bg_num), vofs = ppu.getBGVOFS(bg_num);
    int vx = ox + ((hofs/8) % 32)*scale, vy = oy + ((vofs/8) % 32)*scale;
    for (int i = 0; i < 32*scale; i++) { putPixel(vx+i, vy, 0xFF00FF00); putPixel(vx+i, vy+28*scale, 0xFF00FF00); }
    for (int i = 0; i < 28*scale; i++) { putPixel(vx, vy+i, 0xFF00FF00); putPixel(vx+32*scale, vy+i, 0xFF00FF00); }

    char buf[80];
    snprintf(buf, sizeof(buf), "BG%d Map@%04X Tile@%04X Scroll:%d,%d",
             bg_num+1, (unsigned)(map_base*2), (unsigned)(tb*2), hofs, vofs);
    drawText(ox, oy-12, buf, 0xFFB0B0C0);

    // BG enable indicators
    for (int bg = 0; bg < 4; bg++) {
        char lbl[8]; snprintf(lbl, sizeof(lbl), "BG%d", bg+1);
        uint32_t c = (ppu.getTMMain() & (1 << bg)) ? 0xFF00FF80 : 0xFF404060;
        drawText(10+bg*70, oy+32*scale+8, lbl, c);
    }
    // OBJ indicator
    drawText(10+4*70, oy+32*scale+8, "OBJ",
             (ppu.getTMMain() & 0x10) ? 0xFF00FF80 : 0xFF404060);
}

void Visualizer::update(const PPU& ppu) {
    if (!window || !renderer || !texture) return;
    memset(pixels, 0x08, sizeof(pixels));

    const char* panels[] = { "[1] TILES", "[2] PALETTE", "[3] BG MAP" };
    for (int i = 0; i < 3; i++)
        drawText(10+i*100, 4, panels[i], (i == active_panel) ? 0xFF00FFAA : 0xFF505070);

    char status[128];
    snprintf(status, sizeof(status), "MODE:%d TM:%02X INIDISP:%02X",
             ppu.getBGMode() & 7, ppu.getTMMain(), ppu.getINIDISP());
    drawText(350, 4, status, 0xFF8080A0);

    switch (active_panel) {
    case 0:
        drawTileGrid(ppu.getVRAMData(), ppu.getVRAMSize(), ppu.getCGRAMData(), 4);
        drawText(10, 12, "4bpp tiles (palette 0)", 0xFF6060A0);
        break;
    case 1:
        drawPalette(ppu.getCGRAMData(), ppu.getCGRAMSize());
        drawText(10, 12, "CGRAM - 256 colors (16 palettes x 16)", 0xFF6060A0);
        break;
    case 2:
        drawBGMap(ppu, 0);
        drawText(10, 12, "BG1 tilemap (green = viewport)", 0xFF6060A0);
        break;
    }

    SDL_UpdateTexture(texture, NULL, pixels, WIN_W * sizeof(uint32_t));
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}
