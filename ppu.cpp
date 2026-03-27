#include "ppu.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <cstddef>

namespace {
inline uint32_t bgCharBaseWordAddress(uint8_t nibble) {
    return (uint32_t)(nibble & 0x0F) * 0x1000;
}

inline uint32_t objCharBaseWordAddress(uint8_t base_bits) {
    return (uint32_t)(base_bits & 0x07) * 0x2000;
}

inline uint32_t objNameGapWordAddress(uint8_t gap_bits) {
    return (uint32_t)((gap_bits & 0x03) + 1) * 0x1000;
}

inline uint16_t wrapVRAMWordAddress(uint32_t word_address) {
    return (uint16_t)(word_address & 0x7FFF);
}
}

PPU::PPU() {
    vram .resize(64 * 1024, 0);
    cgram.resize(512, 0);
    oam  .resize(544, 0);

    vram_address = 0; vmain = 0; vram_prefetch = 0;
    cgram_address = 0; cgram_latch = false; cgram_buffer = 0;
    oam_address = 0; oam_write_address = 0; oam_latch = false; oam_buffer = 0;
    bgmode = 0; obsel = 0;
    inidisp = 0x80;
    m7a = m7b = m7c = m7d = 0; mpy_result = 0;
    ophct = opvct = 0; ophct_latch = opvct_latch = false;

    memset(bg_sc, 0, sizeof(bg_sc));
    memset(bg_nba, 0, sizeof(bg_nba));
    memset(bg_hofs, 0, sizeof(bg_hofs));
    memset(bg_vofs, 0, sizeof(bg_vofs));
    bg_scroll_latch = 0;
    bg_hofs_latch = 0;
    tm_main = 0; tm_sub = 0;
    tmw_main = 0; tsw_sub = 0;
    cgwsel = 0; cgadsub = 0; fixed_color = 0;
    framebuffer.fill(0);
}

// ═══════════════════════════════════════════════════════════════════════════
//  VRAM Address Translation — bits 2-3 of VMAIN ($2115) control this.
//
//  Games use these modes during DMA to arrange tile data efficiently.
//  Without this, tile bytes end up at wrong addresses → garbled graphics.
//
//  Mode 0 (%00): No translation — address used as-is
//  Mode 1 (%01): 8-bit rotate  — aaaaaaaaBBBccccc → aaaaaaaacccccBBB
//  Mode 2 (%10): 9-bit rotate  — aaaaaaaBBBcccccc → aaaaaaaccccccBBB
//  Mode 3 (%11): 10-bit rotate — aaaaaaBBBccccccc → aaaaaacccccccBBB
// ═══════════════════════════════════════════════════════════════════════════
uint16_t PPU::translateVRAMAddress(uint16_t addr) {
    switch ((vmain >> 2) & 0x03) {
    case 0: return addr & 0x7FFF;
    case 1: return ((addr & 0xFF00) | ((addr & 0x001F) << 3) | ((addr >> 5) & 0x07)) & 0x7FFF;
    case 2: return ((addr & 0xFE00) | ((addr & 0x003F) << 3) | ((addr >> 6) & 0x07)) & 0x7FFF;
    case 3: return ((addr & 0xFC00) | ((addr & 0x007F) << 3) | ((addr >> 7) & 0x07)) & 0x7FFF;
    }
    return addr & 0x7FFF;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register Read
// ─────────────────────────────────────────────────────────────────────────────
uint8_t PPU::readRegister(uint16_t address) {
    switch (address) {
    case 0x2134: return mpy_result & 0xFF;
    case 0x2135: return (mpy_result >> 8) & 0xFF;
    case 0x2136: return (mpy_result >> 16) & 0xFF;

    case 0x2137:
        ophct_latch = false; opvct_latch = false;
        return 0x00;

    case 0x2138: {
        uint8_t val = (oam_write_address < oam.size()) ? oam[oam_write_address] : 0;
        oam_write_address++;
        return val;
    }

    case 0x2139: {
        uint8_t val = vram_prefetch & 0xFF;
        if ((vmain & 0x80) == 0) {
            uint16_t ta = translateVRAMAddress(vram_address);
            vram_prefetch = vram[ta*2] | (vram[ta*2+1] << 8);
            uint8_t inc = vmain & 0x03;
            vram_address = (vram_address + ((inc == 0) ? 1 : (inc == 1) ? 32 : 128)) & 0x7FFF;
        }
        return val;
    }
    case 0x213A: {
        uint8_t val = (vram_prefetch >> 8) & 0xFF;
        if ((vmain & 0x80) != 0) {
            uint16_t ta = translateVRAMAddress(vram_address);
            vram_prefetch = vram[ta*2] | (vram[ta*2+1] << 8);
            uint8_t inc = vmain & 0x03;
            vram_address = (vram_address + ((inc == 0) ? 1 : (inc == 1) ? 32 : 128)) & 0x7FFF;
        }
        return val;
    }

    case 0x213B: {
        uint8_t val;
        if (!cgram_latch) {
            uint16_t idx = (uint16_t)cgram_address * 2;
            val = (idx < cgram.size()) ? cgram[idx] : 0;
            cgram_latch = true;
        } else {
            uint16_t idx = (uint16_t)cgram_address * 2 + 1;
            val = (idx < cgram.size()) ? cgram[idx] : 0;
            cgram_address++;
            cgram_latch = false;
        }
        return val;
    }

    case 0x213C:
        if (!ophct_latch) { ophct_latch = true; return ophct & 0xFF; }
        else { ophct_latch = false; return (ophct >> 8) & 0x01; }
    case 0x213D:
        if (!opvct_latch) { opvct_latch = true; return opvct & 0xFF; }
        else { opvct_latch = false; return (opvct >> 8) & 0x01; }

    case 0x213E: return 0x01;
    case 0x213F:
        ophct_latch = false; opvct_latch = false;
        return 0x03;

    default: return 0x00;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register Write
// ─────────────────────────────────────────────────────────────────────────────
void PPU::writeRegister(uint16_t address, uint8_t data) {
    switch (address) {
    case 0x2100: inidisp = data; break;
    case 0x2101: obsel = data; break;

    case 0x2102:
        oam_address = (oam_address & 0x0200) | ((uint16_t)data << 1);
        oam_write_address = oam_address; oam_latch = false; break;
    case 0x2103:
        oam_address = (oam_address & 0x01FE) | ((data & 0x01) << 9);
        oam_write_address = oam_address; oam_latch = false; break;

    case 0x2104:
        if (oam_write_address >= 0x200) {
            if (oam_write_address < (int)oam.size()) oam[oam_write_address] = data;
            oam_write_address++;
        } else if (!oam_latch) {
            oam_buffer = data; oam_latch = true;
        } else {
            if (oam_write_address < (int)oam.size()) oam[oam_write_address] = oam_buffer;
            if (oam_write_address + 1 < (int)oam.size()) oam[oam_write_address + 1] = data;
            oam_write_address += 2; oam_latch = false;
        }
        break;

    case 0x2105: bgmode = data; break;
    case 0x2107: bg_sc[0] = data; break;
    case 0x2108: bg_sc[1] = data; break;
    case 0x2109: bg_sc[2] = data; break;
    case 0x210A: bg_sc[3] = data; break;
    case 0x210B: bg_nba[0] = data; break;
    case 0x210C: bg_nba[1] = data; break;

    case 0x210D:
        bg_hofs[0] = ((uint16_t)data << 8) | (bg_scroll_latch & ~7) | (bg_hofs_latch & 0x07);
        bg_scroll_latch = data;
        bg_hofs_latch = data;
        break;
    case 0x210E: bg_vofs[0] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x210F:
        bg_hofs[1] = ((uint16_t)data << 8) | (bg_scroll_latch & ~7) | (bg_hofs_latch & 0x07);
        bg_scroll_latch = data;
        bg_hofs_latch = data;
        break;
    case 0x2110: bg_vofs[1] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2111:
        bg_hofs[2] = ((uint16_t)data << 8) | (bg_scroll_latch & ~7) | (bg_hofs_latch & 0x07);
        bg_scroll_latch = data;
        bg_hofs_latch = data;
        break;
    case 0x2112: bg_vofs[2] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2113:
        bg_hofs[3] = ((uint16_t)data << 8) | (bg_scroll_latch & ~7) | (bg_hofs_latch & 0x07);
        bg_scroll_latch = data;
        bg_hofs_latch = data;
        break;
    case 0x2114: bg_vofs[3] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;

    case 0x2115: vmain = data; break;

    // ══════════════════════════════════════════════════════════════════════
    //  VRAM address set — apply translation for prefetch
    // ══════════════════════════════════════════════════════════════════════
    case 0x2116: {
        vram_address = ((vram_address & 0xFF00) | data) & 0x7FFF;
        uint16_t ta = translateVRAMAddress(vram_address);
        vram_prefetch = vram[ta*2] | (vram[ta*2+1] << 8);
        break;
    }
    case 0x2117: {
        vram_address = ((vram_address & 0x00FF) | ((uint16_t)data << 8)) & 0x7FFF;
        uint16_t ta = translateVRAMAddress(vram_address);
        vram_prefetch = vram[ta*2] | (vram[ta*2+1] << 8);
        break;
    }

    // ══════════════════════════════════════════════════════════════════════
    //  VRAM data write — apply address translation!
    // ══════════════════════════════════════════════════════════════════════
    case 0x2118: {
        uint16_t ta = translateVRAMAddress(vram_address);
        uint32_t ba = (uint32_t)ta * 2;
        vram[ba] = data;
        if ((vmain & 0x80) == 0) {
            uint8_t inc = vmain & 0x03;
            vram_address = (vram_address + ((inc == 0) ? 1 : (inc == 1) ? 32 : 128)) & 0x7FFF;
        }
        break;
    }
    case 0x2119: {
        uint16_t ta = translateVRAMAddress(vram_address);
        uint32_t ba = (uint32_t)ta * 2 + 1;
        vram[ba] = data;
        if ((vmain & 0x80) != 0) {
            uint8_t inc = vmain & 0x03;
            vram_address = (vram_address + ((inc == 0) ? 1 : (inc == 1) ? 32 : 128)) & 0x7FFF;
        }
        break;
    }

    case 0x211B: m7a = (data << 8) | bg_scroll_latch; bg_scroll_latch = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211C: m7b = (data << 8) | bg_scroll_latch; bg_scroll_latch = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211D: m7c = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x211E: m7d = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;

    case 0x2121: cgram_address = data; cgram_latch = false; break;
    case 0x2122:
        if (!cgram_latch) { cgram_buffer = data; cgram_latch = true; }
        else {
            uint16_t idx = (uint16_t)cgram_address * 2;
            if (idx + 1 < (int)cgram.size()) { cgram[idx] = cgram_buffer; cgram[idx+1] = data; }
            cgram_address++; cgram_latch = false;
        }
        break;

    case 0x212C: tm_main = data; break;
    case 0x212D: tm_sub = data; break;
    case 0x212E: tmw_main = data; break;
    case 0x212F: tsw_sub = data; break;
    case 0x2130: cgwsel = data; break;
    case 0x2131: cgadsub = data; break;
    case 0x2132: {
        uint16_t value = data & 0x1F;
        if (data & 0x20) fixed_color = (fixed_color & ~0x001F) | value;
        if (data & 0x40) fixed_color = (fixed_color & ~0x03E0) | (value << 5);
        if (data & 0x80) fixed_color = (fixed_color & ~0x7C00) | (value << 10);
        break;
    }

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
uint32_t PPU::colorFromCGRAM(uint16_t cg_idx) {
    if (cg_idx * 2 + 1 >= cgram.size()) return 0xFF000000u;
    uint16_t c15 = (uint16_t)cgram[cg_idx*2] | ((uint16_t)cgram[cg_idx*2+1] << 8);
    uint8_t r = (uint8_t)((c15 & 0x001F) << 3); r |= (r >> 5);
    uint8_t g = (uint8_t)(((c15 >> 5) & 0x1F) << 3); g |= (g >> 5);
    uint8_t b = (uint8_t)(((c15 >> 10) & 0x1F) << 3); b |= (b >> 5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background Rendering
// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderBG(int bg_num, int bpp, bool high_priority, uint32_t* target, uint8_t* source) {
    // BGxSC stores the tilemap base as a word address in 0x200-word units.
    uint32_t map_base = ((bg_sc[bg_num] >> 2) & 0x3F) * 0x200;
    uint8_t sc_size = bg_sc[bg_num] & 0x03;
    bool large_tiles = ((bgmode >> (4 + bg_num)) & 0x01) != 0;
    int cell_size = large_tiles ? 16 : 8;
    int map_w = (sc_size & 0x01) ? 64 : 32;
    int map_h = (sc_size & 0x02) ? 64 : 32;

    uint32_t tile_base;
    if      (bg_num == 0) tile_base = bgCharBaseWordAddress(bg_nba[0] & 0x0F);
    else if (bg_num == 1) tile_base = bgCharBaseWordAddress((bg_nba[0] >> 4) & 0x0F);
    else if (bg_num == 2) tile_base = bgCharBaseWordAddress(bg_nba[1] & 0x0F);
    else                  tile_base = bgCharBaseWordAddress((bg_nba[1] >> 4) & 0x0F);

    const int wpt = (bpp == 2) ? 8 : (bpp == 4) ? 16 : 32;
    const int pe  = (bpp == 2) ? 4 : (bpp == 4) ? 16 : 256;
    int pal_base = 0;
    if (bpp == 2 && (bgmode & 0x07) == 0) pal_base = bg_num * 32;

    uint16_t sx = bg_hofs[bg_num] & 0x03FF;
    uint16_t sy = bg_vofs[bg_num] & 0x03FF;

    for (int scr_y = 0; scr_y < 224; scr_y++) {
        // The SNES blanks the first BG scanline, so the top visible line samples
        // from one line below the nominal scroll origin.
        int y = (scr_y + sy + 1) & ((map_h * cell_size) - 1);
        int ty = y / cell_size, fy = y % cell_size;

        for (int scr_x = 0; scr_x < 256; scr_x++) {
            int x = (scr_x + sx) & ((map_w * cell_size) - 1);
            int tx = x / cell_size, fx = x % cell_size;

            uint32_t sb = 0;
            int ltx = tx, lty = ty;
            if (map_w == 64 && tx >= 32) { sb += 1; ltx -= 32; }
            if (map_h == 64 && ty >= 32) { sb += (map_w == 64) ? 2 : 1; lty -= 32; }

            uint32_t mw = wrapVRAMWordAddress(map_base + sb * 0x400 + (uint32_t)lty * 32 + ltx);

            uint16_t entry = (uint16_t)vram[mw*2] | ((uint16_t)vram[mw*2+1] << 8);
            uint16_t tnum = entry & 0x03FF;
            uint8_t  pal = (entry >> 10) & 0x07;
            bool pri = (entry & 0x2000) != 0;
            bool hf = (entry & 0x4000), vf = (entry & 0x8000);
            if (pri != high_priority) continue;

            int local_x = hf ? (cell_size - 1 - fx) : fx;
            int local_y = vf ? (cell_size - 1 - fy) : fy;
            int subtile_x = local_x >> 3;
            int subtile_y = local_y >> 3;
            int row = local_y & 0x07;
            int bit = 7 - (local_x & 0x07);

            if (large_tiles)
                tnum = (tnum + subtile_x + subtile_y * 16) & 0x03FF;

            uint32_t tw = wrapVRAMWordAddress(tile_base + (uint32_t)tnum * wpt);
            uint32_t bp01 = wrapVRAMWordAddress(tw + row);

            uint8_t b0 = vram[bp01*2], b1 = vram[bp01*2+1];
            uint8_t b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, b7 = 0;
            if (bpp >= 4) {
                uint32_t bp23 = wrapVRAMWordAddress(tw + 8 + row);
                b2 = vram[bp23*2];
                b3 = vram[bp23*2+1];
            }
            if (bpp == 8) {
                uint32_t bp45 = wrapVRAMWordAddress(tw + 16 + row);
                uint32_t bp67 = wrapVRAMWordAddress(tw + 24 + row);
                b4 = vram[bp45*2];
                b5 = vram[bp45*2+1];
                b6 = vram[bp67*2];
                b7 = vram[bp67*2+1];
            }

            uint8_t ci = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
            if (bpp >= 4) ci |= (((b2 >> bit) & 1) << 2) | (((b3 >> bit) & 1) << 3);
            if (bpp == 8) ci |= (((b4 >> bit) & 1) << 4) | (((b5 >> bit) & 1) << 5)
                              | (((b6 >> bit) & 1) << 6) | (((b7 >> bit) & 1) << 7);
            if (ci == 0) continue;

            uint16_t cg = (bpp == 8) ? ci : (uint16_t)pal * pe + ci + pal_base;
            size_t pixel = (size_t)scr_y * 256 + scr_x;
            target[pixel] = colorFromCGRAM(cg);
            source[pixel] = (uint8_t)(bg_num + 1);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sprite Rendering
// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderSprites(int priority, uint32_t* target, uint8_t* source) {
    static const int obj_sizes[8][2][2] = {
        {{8,8},{16,16}}, {{8,8},{32,32}}, {{8,8},{64,64}}, {{16,16},{32,32}},
        {{16,16},{64,64}}, {{32,32},{64,64}}, {{16,32},{32,64}}, {{16,32},{32,32}},
    };
    uint8_t size_sel = (obsel >> 5) & 0x07;
    uint32_t name_base = objCharBaseWordAddress(obsel & 0x07);
    uint32_t name_gap = objNameGapWordAddress((obsel >> 3) & 0x03);

    for (int s = 127; s >= 0; s--) {
        int base = s * 4;
        int16_t ox = oam[base]; uint8_t oy = oam[base+1];
        uint16_t tile = oam[base+2]; uint8_t attr = oam[base+3];
        int hb = 0x200 + (s >> 2); int hs = (s & 3) * 2;
        uint8_t hi = (oam[hb] >> hs) & 0x03;
        if (hi & 0x01) ox = (int16_t)(ox | 0xFF00);
        bool large = (hi & 0x02) != 0;
        uint8_t obj_priority = (attr >> 4) & 0x03;
        if (obj_priority != priority) continue;
        int ow = obj_sizes[size_sel][large?1:0][0], oh = obj_sizes[size_sel][large?1:0][1];
        bool hf = (attr & 0x40), vf = (attr & 0x80);
        uint8_t pal = (attr >> 1) & 0x07;

        for (int py = 0; py < oh; py++) {
            int scr_y = (oy + py + 1) & 0xFF;
            if (scr_y >= 224) continue;
            int row = vf ? (oh-1-py) : py;
            for (int px = 0; px < ow; px++) {
                int scr_x = ox + px;
                if (scr_x < 0 || scr_x >= 256) continue;
                int col = hf ? (ow-1-px) : px;
                uint16_t t = tile;
                uint32_t ba = name_base;
                if (t & 0x100) { ba += name_gap; t &= 0xFF; }
                t += (row/8)*16 + (col/8); t &= 0xFF;
                uint32_t tw = wrapVRAMWordAddress(ba + (uint32_t)t * 16);
                int fr = row % 8, fc = col % 8;
                uint32_t bp01 = wrapVRAMWordAddress(tw + fr);
                uint32_t bp23 = wrapVRAMWordAddress(tw + 8 + fr);
                int bit = 7 - fc;
                uint8_t ci = ((vram[bp01*2]>>bit)&1) | (((vram[bp01*2+1]>>bit)&1)<<1)
                           | (((vram[bp23*2]>>bit)&1)<<2) | (((vram[bp23*2+1]>>bit)&1)<<3);
                if (ci == 0) continue;
                size_t pixel = (size_t)scr_y * 256 + scr_x;
                target[pixel] = colorFromCGRAM(128 + pal*16 + ci);
                source[pixel] = (pal >= 4) ? 6 : 5;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderFrame() {
    static constexpr size_t kPixelCount = 256 * 224;
    vblank_flag = false;
    if (inidisp & 0x80) { framebuffer.fill(0xFF000000u); vblank_flag = true; return; }

    uint8_t brightness = inidisp & 0x0F;
    const uint32_t backdrop = colorFromCGRAM(0);
    std::array<uint32_t, kPixelCount> main_fb;
    std::array<uint32_t, kPixelCount> sub_fb;
    std::array<uint8_t, kPixelCount> main_src;
    std::array<uint8_t, kPixelCount> sub_src;
    main_fb.fill(backdrop);
    sub_fb.fill(backdrop);
    main_src.fill(0);
    sub_src.fill(0);

    auto renderScreen = [this](uint8_t layer_mask, uint32_t* target, uint8_t* source) {
        auto drawOBJ = [&](int prio) {
            if (layer_mask & 0x10) renderSprites(prio, target, source);
        };

        uint8_t mode = bgmode & 0x07;
        switch (mode) {
        case 0:
            if (layer_mask & 0x08) renderBG(3, 2, false, target, source);
            if (layer_mask & 0x04) renderBG(2, 2, false, target, source);
            if (layer_mask & 0x02) renderBG(1, 2, false, target, source);
            if (layer_mask & 0x01) renderBG(0, 2, false, target, source);
            drawOBJ(0);
            if (layer_mask & 0x08) renderBG(3, 2, true, target, source);
            if (layer_mask & 0x04) renderBG(2, 2, true, target, source);
            if (layer_mask & 0x02) renderBG(1, 2, true, target, source);
            if (layer_mask & 0x01) renderBG(0, 2, true, target, source);
            drawOBJ(1); drawOBJ(2); drawOBJ(3);
            break;
        case 1:
            if (bgmode & 0x08) {
                if (layer_mask & 0x04) renderBG(2, 2, false, target, source);
                drawOBJ(0);
                if (layer_mask & 0x02) renderBG(1, 4, false, target, source);
                if (layer_mask & 0x01) renderBG(0, 4, false, target, source);
                drawOBJ(1);
                if (layer_mask & 0x02) renderBG(1, 4, true, target, source);
                if (layer_mask & 0x01) renderBG(0, 4, true, target, source);
                drawOBJ(2);
                if (layer_mask & 0x04) renderBG(2, 2, true, target, source);
                drawOBJ(3);
            } else {
                if (layer_mask & 0x04) renderBG(2, 2, false, target, source);
                drawOBJ(0);
                if (layer_mask & 0x04) renderBG(2, 2, true, target, source);
                drawOBJ(1);
                if (layer_mask & 0x02) renderBG(1, 4, false, target, source);
                if (layer_mask & 0x01) renderBG(0, 4, false, target, source);
                drawOBJ(2);
                if (layer_mask & 0x02) renderBG(1, 4, true, target, source);
                if (layer_mask & 0x01) renderBG(0, 4, true, target, source);
                drawOBJ(3);
            }
            break;
        case 2: case 6:
            if (layer_mask & 0x02) renderBG(1, 4, false, target, source);
            if (layer_mask & 0x01) renderBG(0, 4, false, target, source);
            drawOBJ(0);
            if (layer_mask & 0x02) renderBG(1, 4, true, target, source);
            if (layer_mask & 0x01) renderBG(0, 4, true, target, source);
            drawOBJ(1); drawOBJ(2); drawOBJ(3);
            break;
        case 3:
            if (layer_mask & 0x02) renderBG(1, 4, false, target, source);
            if (layer_mask & 0x01) renderBG(0, 8, false, target, source);
            drawOBJ(0);
            if (layer_mask & 0x02) renderBG(1, 4, true, target, source);
            if (layer_mask & 0x01) renderBG(0, 8, true, target, source);
            drawOBJ(1); drawOBJ(2); drawOBJ(3);
            break;
        case 4:
            if (layer_mask & 0x02) renderBG(1, 2, false, target, source);
            if (layer_mask & 0x01) renderBG(0, 8, false, target, source);
            drawOBJ(0);
            if (layer_mask & 0x02) renderBG(1, 2, true, target, source);
            if (layer_mask & 0x01) renderBG(0, 8, true, target, source);
            drawOBJ(1); drawOBJ(2); drawOBJ(3);
            break;
        case 5:
            if (layer_mask & 0x02) renderBG(1, 2, false, target, source);
            if (layer_mask & 0x01) renderBG(0, 4, false, target, source);
            drawOBJ(0);
            if (layer_mask & 0x02) renderBG(1, 2, true, target, source);
            if (layer_mask & 0x01) renderBG(0, 4, true, target, source);
            drawOBJ(1); drawOBJ(2); drawOBJ(3);
            break;
        default:
            break;
        }
    };

    renderScreen(tm_main, main_fb.data(), main_src.data());
    if (tm_sub) renderScreen(tm_sub, sub_fb.data(), sub_src.data());

    auto fixed_color_argb = [this]() {
        uint16_t c15 = fixed_color & 0x7FFF;
        uint8_t r = (uint8_t)((c15 & 0x001F) << 3); r |= (r >> 5);
        uint8_t g = (uint8_t)(((c15 >> 5) & 0x1F) << 3); g |= (g >> 5);
        uint8_t b = (uint8_t)(((c15 >> 10) & 0x1F) << 3); b |= (b >> 5);
        return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
    };
    const uint32_t fixed_argb = fixed_color_argb();
    const bool use_subscreen = (cgwsel & 0x02) != 0;
    const bool subtract = (cgadsub & 0x80) != 0;
    const bool half = (cgadsub & 0x40) != 0;

    auto colorMathEnabled = [this](uint8_t src) {
        switch (src) {
        case 0: return (cgadsub & 0x20) != 0; // Backdrop
        case 1: return (cgadsub & 0x01) != 0; // BG1
        case 2: return (cgadsub & 0x02) != 0; // BG2
        case 3: return (cgadsub & 0x04) != 0; // BG3
        case 4: return (cgadsub & 0x08) != 0; // BG4
        case 6: return (cgadsub & 0x10) != 0; // OBJ palettes 4-7
        default: return false;
        }
    };
    auto combine = [subtract, half](uint32_t a, uint32_t b) {
        int ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
        int br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
        int rr = subtract ? std::max(0, ar - br) : std::min(255, ar + br);
        int rg = subtract ? std::max(0, ag - bg) : std::min(255, ag + bg);
        int rb = subtract ? std::max(0, ab - bb) : std::min(255, ab + bb);
        if (half) {
            rr >>= 1;
            rg >>= 1;
            rb >>= 1;
        }
        return 0xFF000000u | ((uint32_t)rr << 16) | ((uint32_t)rg << 8) | (uint32_t)rb;
    };

    for (size_t i = 0; i < kPixelCount; i++) {
        uint32_t pixel = main_fb[i];
        if (colorMathEnabled(main_src[i])) {
            pixel = combine(pixel, use_subscreen ? sub_fb[i] : fixed_argb);
        }
        framebuffer[i] = pixel;
    }

    if (brightness < 15) {
        for (auto& pixel : framebuffer) {
            uint8_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
            pixel = 0xFF000000u | (((uint32_t)r*brightness/15) << 16)
                  | (((uint32_t)g*brightness/15) << 8) | ((uint32_t)b*brightness/15);
        }
    }
    vblank_flag = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Debug counters
// ─────────────────────────────────────────────────────────────────────────────
size_t PPU::countNonZeroVRAM() const {
    return (size_t)std::count_if(vram.begin(), vram.end(), [](uint8_t v) { return v != 0; });
}
size_t PPU::countNonZeroCGRAM() const {
    return (size_t)std::count_if(cgram.begin(), cgram.end(), [](uint8_t v) { return v != 0; });
}
size_t PPU::countNonZeroOAM() const {
    return (size_t)std::count_if(oam.begin(), oam.end(), [](uint8_t v) { return v != 0; });
}
size_t PPU::countNonBlackPixels() const {
    return (size_t)std::count_if(framebuffer.begin(), framebuffer.end(), [](uint32_t p) { return (p & 0x00FFFFFFu) != 0; });
}
