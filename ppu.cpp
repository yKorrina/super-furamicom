#include "ppu.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>
#include <cstddef>

PPU::PPU() {
    vram .resize(64 * 1024, 0);
    cgram.resize(512, 0);
    oam  .resize(544, 0);

    vram_address = 0; vmain = 0; vram_prefetch = 0;
    cgram_address = 0; cgram_latch = false; cgram_buffer = 0;
    oam_address = 0; oam_write_address = 0; oam_latch = false; oam_buffer = 0;
    bgmode = 0; obsel = 0;
    inidisp = 0x80;  // Forced blank on boot
    m7a = m7b = m7c = m7d = 0; mpy_result = 0;
    ophct = opvct = 0; ophct_latch = opvct_latch = false;

    memset(bg_sc, 0, sizeof(bg_sc));
    memset(bg_nba, 0, sizeof(bg_nba));
    memset(bg_hofs, 0, sizeof(bg_hofs));
    memset(bg_vofs, 0, sizeof(bg_vofs));
    bg_scroll_latch = 0;
    tm_main = 0; tm_sub = 0;
    framebuffer.fill(0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register Read — games check many of these during boot
// ─────────────────────────────────────────────────────────────────────────────
uint8_t PPU::readRegister(uint16_t address) {
    switch (address) {
    case 0x2134: return mpy_result & 0xFF;          // MPYL
    case 0x2135: return (mpy_result >> 8) & 0xFF;   // MPYM
    case 0x2136: return (mpy_result >> 16) & 0xFF;  // MPYH

    case 0x2137: // SLHV - latch H/V counter
        ophct_latch = false; opvct_latch = false;
        return 0x00;

    case 0x2138: { // OAM data read
        uint8_t val = (oam_write_address < oam.size()) ? oam[oam_write_address] : 0;
        oam_write_address++;
        return val;
    }

    case 0x2139: { // VRAM data read low (prefetched)
        uint8_t val = vram_prefetch & 0xFF;
        if ((vmain & 0x80) == 0) {
            if (vram_address * 2 + 1 < (int)vram.size())
                vram_prefetch = vram[vram_address*2] | (vram[vram_address*2+1] << 8);
            uint8_t inc = vmain & 0x03;
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        return val;
    }
    case 0x213A: { // VRAM data read high (prefetched)
        uint8_t val = (vram_prefetch >> 8) & 0xFF;
        if ((vmain & 0x80) != 0) {
            if (vram_address * 2 + 1 < (int)vram.size())
                vram_prefetch = vram[vram_address*2] | (vram[vram_address*2+1] << 8);
            uint8_t inc = vmain & 0x03;
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        return val;
    }

    case 0x213B: { // CGRAM data read
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

    case 0x213C: // OPHCT - horizontal counter
        if (!ophct_latch) { ophct_latch = true; return ophct & 0xFF; }
        else { ophct_latch = false; return (ophct >> 8) & 0x01; }

    case 0x213D: // OPVCT - vertical counter
        if (!opvct_latch) { opvct_latch = true; return opvct & 0xFF; }
        else { opvct_latch = false; return (opvct >> 8) & 0x01; }

    case 0x213E: // STAT77 - PPU1 status
        return 0x01;  // PPU1 version 1, no range/time over

    case 0x213F: // STAT78 - PPU2 status
        ophct_latch = false; opvct_latch = false;
        return 0x03;  // PPU2 version 3, NTSC, no interlace

    default:
        return 0x00;
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
        oam_write_address = oam_address; oam_latch = false;
        break;
    case 0x2103:
        oam_address = (oam_address & 0x01FE) | ((data & 0x01) << 9);
        oam_write_address = oam_address; oam_latch = false;
        break;

    case 0x2104: // OAM data write
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

    case 0x210D: bg_hofs[0] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[0] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x210E: bg_vofs[0] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x210F: bg_hofs[1] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[1] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2110: bg_vofs[1] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2111: bg_hofs[2] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[2] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2112: bg_vofs[2] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2113: bg_hofs[3] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[3] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2114: bg_vofs[3] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;

    case 0x2115: vmain = data; break;

    case 0x2116:
        vram_address = (vram_address & 0xFF00) | data;
        if (vram_address * 2 + 1 < (int)vram.size())
            vram_prefetch = vram[vram_address*2] | (vram[vram_address*2+1] << 8);
        break;
    case 0x2117:
        vram_address = (vram_address & 0x00FF) | ((uint16_t)data << 8);
        if (vram_address * 2 + 1 < (int)vram.size())
            vram_prefetch = vram[vram_address*2] | (vram[vram_address*2+1] << 8);
        break;

    case 0x2118: {
        uint32_t ba = (uint32_t)vram_address * 2;
        if (ba < vram.size()) vram[ba] = data;
        if ((vmain & 0x80) == 0) {
            uint8_t inc = vmain & 0x03;
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        break;
    }
    case 0x2119: {
        uint32_t ba = (uint32_t)vram_address * 2 + 1;
        if (ba < vram.size()) vram[ba] = data;
        if ((vmain & 0x80) != 0) {
            uint8_t inc = vmain & 0x03;
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        break;
    }

    // Mode 7 matrix registers
    case 0x211B: m7a = (data << 8) | (bg_scroll_latch); bg_scroll_latch = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211C: m7b = (data << 8) | (bg_scroll_latch); bg_scroll_latch = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211D: m7c = (data << 8) | (bg_scroll_latch); bg_scroll_latch = data; break;
    case 0x211E: m7d = (data << 8) | (bg_scroll_latch); bg_scroll_latch = data; break;

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
//  Background Rendering (with scrolling and tilemap mirroring)
// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderBG(int bg_num, int bpp) {
    uint32_t map_base = ((bg_sc[bg_num] >> 2) & 0x3F) * 0x400;
    uint8_t sc_size = bg_sc[bg_num] & 0x03;
    int map_w = (sc_size & 0x01) ? 64 : 32;
    int map_h = (sc_size & 0x02) ? 64 : 32;

    uint32_t tile_base;
    if      (bg_num == 0) tile_base = (uint32_t)(bg_nba[0] & 0x0F) * 0x1000;
    else if (bg_num == 1) tile_base = (uint32_t)((bg_nba[0] >> 4) & 0x0F) * 0x1000;
    else if (bg_num == 2) tile_base = (uint32_t)(bg_nba[1] & 0x0F) * 0x1000;
    else                  tile_base = (uint32_t)((bg_nba[1] >> 4) & 0x0F) * 0x1000;

    const int wpt = (bpp == 2) ? 8 : 16;
    const int pe  = (bpp == 2) ? 4 : 16;
    int pal_base = 0;
    if (bpp == 2 && (bgmode & 0x07) == 0) pal_base = bg_num * 32;

    uint16_t sx = bg_hofs[bg_num], sy = bg_vofs[bg_num];

    for (int scr_y = 0; scr_y < 224; scr_y++) {
        int y = (scr_y + sy) & ((map_h * 8) - 1);
        int ty = y / 8, fy = y % 8;

        for (int scr_x = 0; scr_x < 256; scr_x++) {
            int x = (scr_x + sx) & ((map_w * 8) - 1);
            int tx = x / 8, fx = x % 8;

            uint32_t sb = 0;
            int ltx = tx, lty = ty;
            if (map_w == 64 && tx >= 32) { sb += 1; ltx -= 32; }
            if (map_h == 64 && ty >= 32) { sb += (map_w == 64) ? 2 : 1; lty -= 32; }

            uint32_t mw = map_base + sb * 0x400 + (uint32_t)lty * 32 + ltx;
            if (mw * 2 + 1 >= vram.size()) continue;

            uint16_t entry = (uint16_t)vram[mw*2] | ((uint16_t)vram[mw*2+1] << 8);
            uint16_t tnum = entry & 0x03FF;
            uint8_t  pal = (entry >> 10) & 0x07;
            bool hf = (entry & 0x4000), vf = (entry & 0x8000);

            int row = vf ? (7 - fy) : fy;
            int bit = hf ? fx : (7 - fx);

            uint32_t tw = tile_base + (uint32_t)tnum * wpt;
            uint32_t bp01 = tw + row;
            if (bp01 * 2 + 1 >= vram.size()) continue;

            uint8_t b0 = vram[bp01*2], b1 = vram[bp01*2+1];
            uint8_t b2 = 0, b3 = 0;
            if (bpp == 4) {
                uint32_t bp23 = tw + 8 + row;
                if (bp23 * 2 + 1 < vram.size()) { b2 = vram[bp23*2]; b3 = vram[bp23*2+1]; }
            }

            uint8_t ci = ((b0 >> bit) & 1) | (((b1 >> bit) & 1) << 1);
            if (bpp == 4) ci |= (((b2 >> bit) & 1) << 2) | (((b3 >> bit) & 1) << 3);
            if (ci == 0) continue;

            uint16_t cg = (uint16_t)pal * pe + ci + pal_base;
            framebuffer[scr_y * 256 + scr_x] = colorFromCGRAM(cg);
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sprite Rendering
// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderSprites() {
    static const int obj_sizes[8][2][2] = {
        {{8,8},{16,16}}, {{8,8},{32,32}}, {{8,8},{64,64}}, {{16,16},{32,32}},
        {{16,16},{64,64}}, {{32,32},{64,64}}, {{16,32},{32,64}}, {{16,32},{32,32}},
    };
    uint8_t size_sel = (obsel >> 5) & 0x07;
    uint32_t name_base = (uint32_t)(obsel & 0x07) * 0x2000;
    uint32_t name_gap = ((uint32_t)((obsel >> 3) & 0x03) + 1) * 0x1000;

    for (int s = 127; s >= 0; s--) {
        int base = s * 4;
        int16_t ox = oam[base]; uint8_t oy = oam[base+1];
        uint16_t tile = oam[base+2]; uint8_t attr = oam[base+3];
        int hb = 0x200 + (s >> 2); int hs = (s & 3) * 2;
        uint8_t hi = (oam[hb] >> hs) & 0x03;
        if (hi & 0x01) ox = (int16_t)(ox | 0xFF00);
        bool large = (hi & 0x02) != 0;
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
                uint32_t tw = ba + (uint32_t)t * 16;
                int fr = row % 8, fc = col % 8;
                uint32_t bp01 = tw + fr, bp23 = tw + 8 + fr;
                if (bp23 * 2 + 1 >= vram.size()) continue;
                int bit = 7 - fc;
                uint8_t ci = ((vram[bp01*2]>>bit)&1) | (((vram[bp01*2+1]>>bit)&1)<<1)
                           | (((vram[bp23*2]>>bit)&1)<<2) | (((vram[bp23*2+1]>>bit)&1)<<3);
                if (ci == 0) continue;
                framebuffer[scr_y * 256 + scr_x] = colorFromCGRAM(128 + pal*16 + ci);
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Frame Render
// ─────────────────────────────────────────────────────────────────────────────
void PPU::renderFrame() {
    vblank_flag = false;

    if (inidisp & 0x80) {
        framebuffer.fill(0xFF000000u);
        vblank_flag = true;
        return;
    }

    uint8_t brightness = inidisp & 0x0F;
    framebuffer.fill(colorFromCGRAM(0));

    uint8_t mode = bgmode & 0x07;
    switch (mode) {
    case 0:
        if (tm_main & 0x08) renderBG(3, 2);
        if (tm_main & 0x04) renderBG(2, 2);
        if (tm_main & 0x02) renderBG(1, 2);
        if (tm_main & 0x01) renderBG(0, 2);
        break;
    case 1:
        if (tm_main & 0x04) renderBG(2, 2);
        if (tm_main & 0x02) renderBG(1, 4);
        if (tm_main & 0x01) renderBG(0, 4);
        break;
    case 2: case 4: case 6:
        if (tm_main & 0x02) renderBG(1, 4);
        if (tm_main & 0x01) renderBG(0, 4);
        break;
    case 3: case 5:
        if (tm_main & 0x02) renderBG(1, 4);
        if (tm_main & 0x01) renderBG(0, 4);
        break;
    default: break;
    }

    if (tm_main & 0x10) renderSprites();

    if (brightness < 15) {
        for (auto& pixel : framebuffer) {
            uint8_t r = (pixel >> 16) & 0xFF, g = (pixel >> 8) & 0xFF, b = pixel & 0xFF;
            r = (uint8_t)((r * brightness) / 15);
            g = (uint8_t)((g * brightness) / 15);
            b = (uint8_t)((b * brightness) / 15);
            pixel = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    vblank_flag = true;
}

size_t PPU::countNonZeroVRAM() const {
    return (size_t)std::count_if(vram.begin(), vram.end(), [](uint8_t value) {
        return value != 0;
    });
}

size_t PPU::countNonZeroCGRAM() const {
    return (size_t)std::count_if(cgram.begin(), cgram.end(), [](uint8_t value) {
        return value != 0;
    });
}

size_t PPU::countNonZeroOAM() const {
    return (size_t)std::count_if(oam.begin(), oam.end(), [](uint8_t value) {
        return value != 0;
    });
}

size_t PPU::countNonBlackPixels() const {
    return (size_t)std::count_if(framebuffer.begin(), framebuffer.end(), [](uint32_t pixel) {
        return (pixel & 0x00FFFFFFu) != 0;
    });
}
