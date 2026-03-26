#include "ppu.hpp"
#include <cstring>
#include <iostream>
#include <algorithm>

// ─────────────────────────────────────────────────────────────────────────────
//  Construction
// ─────────────────────────────────────────────────────────────────────────────

PPU::PPU() {
    vram .resize(64  * 1024, 0);
    cgram.resize(512,        0);
    oam  .resize(544,        0);

    vram_address  = 0;
    vmain         = 0;
    vram_prefetch = 0;
    cgram_address = 0;
    cgram_latch   = false;
    cgram_buffer  = 0;
    oam_address   = 0;
    oam_write_address = 0;
    oam_latch     = false;
    oam_buffer    = 0;

    bgmode  = 0;
    obsel   = 0;
    inidisp = 0x80; // Start in forced blank

    memset(bg_sc,   0, sizeof(bg_sc));
    memset(bg_nba,  0, sizeof(bg_nba));
    memset(bg_hofs, 0, sizeof(bg_hofs));
    memset(bg_vofs, 0, sizeof(bg_vofs));
    bg_scroll_latch = 0;

    tm_main = 0;
    tm_sub  = 0;

    framebuffer.fill(0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register Read
// ─────────────────────────────────────────────────────────────────────────────

uint8_t PPU::readRegister(uint16_t address) {
    switch (address) {
    // VRAM data read low (prefetched)
    case 0x2139:
        return vram_prefetch & 0xFF;
    case 0x213A:
        return (vram_prefetch >> 8) & 0xFF;

    // CGRAM data read  
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

    // OAM data read
    case 0x2138: {
        uint8_t val = 0;
        if (oam_write_address < oam.size())
            val = oam[oam_write_address];
        oam_write_address++;
        return val;
    }

    default:
        return 0x00;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Register Write
// ─────────────────────────────────────────────────────────────────────────────

void PPU::writeRegister(uint16_t address, uint8_t data) {
    switch (address) {

    // ── Screen display $2100 ─────────────────────────────────────────────────
    case 0x2100: inidisp = data; break;

    // ── OBJ settings $2101 ───────────────────────────────────────────────────
    case 0x2101: obsel = data; break;

    // ── OAM address $2102-$2103 ──────────────────────────────────────────────
    case 0x2102:
        oam_address = (oam_address & 0x0200) | ((uint16_t)data << 1);
        oam_write_address = oam_address;
        oam_latch = false;
        break;
    case 0x2103:
        oam_address = (oam_address & 0x01FE) | ((data & 0x01) << 9);
        oam_write_address = oam_address;
        oam_latch = false;
        break;

    // ── OAM data write $2104 ─────────────────────────────────────────────────
    case 0x2104:
        if (oam_write_address >= 0x200) {
            // High table (extra bits — sizes + X bit 8)
            if (oam_write_address < (int)oam.size())
                oam[oam_write_address] = data;
            oam_write_address++;
        } else if (!oam_latch) {
            oam_buffer = data;
            oam_latch  = true;
        } else {
            if (oam_write_address < (int)oam.size()) {
                oam[oam_write_address]     = oam_buffer;
                if (oam_write_address + 1 < (int)oam.size())
                    oam[oam_write_address + 1] = data;
            }
            oam_write_address += 2;
            oam_latch = false;
        }
        break;

    // ── BG Mode $2105 ────────────────────────────────────────────────────────
    case 0x2105: bgmode = data; break;

    // ── BG Screen Base $2107-$210A ───────────────────────────────────────────
    case 0x2107: bg_sc[0] = data; break;
    case 0x2108: bg_sc[1] = data; break;
    case 0x2109: bg_sc[2] = data; break;
    case 0x210A: bg_sc[3] = data; break;

    // ── BG Character Data $210B-$210C ────────────────────────────────────────
    case 0x210B: bg_nba[0] = data; break;
    case 0x210C: bg_nba[1] = data; break;

    // ── BG Scroll Registers (write-twice latch) ──────────────────────────────
    case 0x210D: bg_hofs[0] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[0] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x210E: bg_vofs[0] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x210F: bg_hofs[1] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[1] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2110: bg_vofs[1] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2111: bg_hofs[2] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[2] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2112: bg_vofs[2] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;
    case 0x2113: bg_hofs[3] = (data << 8) | (bg_scroll_latch & ~7) | ((bg_hofs[3] >> 8) & 7); bg_scroll_latch = data; break;
    case 0x2114: bg_vofs[3] = (data << 8) | bg_scroll_latch; bg_scroll_latch = data; break;

    // ── VRAM control $2115 ───────────────────────────────────────────────────
    case 0x2115: vmain = data; break;

    // ── VRAM address $2116-$2117 ─────────────────────────────────────────────
    case 0x2116:
        vram_address = (vram_address & 0xFF00) | data;
        // Prefetch on address set
        if (vram_address * 2 + 1 < (int)vram.size())
            vram_prefetch = vram[vram_address * 2] | (vram[vram_address * 2 + 1] << 8);
        break;
    case 0x2117:
        vram_address = (vram_address & 0x00FF) | ((uint16_t)data << 8);
        if (vram_address * 2 + 1 < (int)vram.size())
            vram_prefetch = vram[vram_address * 2] | (vram[vram_address * 2 + 1] << 8);
        break;

    // ── VRAM data write $2118-$2119 ──────────────────────────────────────────
    case 0x2118: {
        uint32_t byte_addr = (uint32_t)vram_address * 2;
        if (byte_addr < vram.size())
            vram[byte_addr] = data;
        if ((vmain & 0x80) == 0) {
            uint8_t inc = (vmain & 0x03);
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        break;
    }
    case 0x2119: {
        uint32_t byte_addr = (uint32_t)vram_address * 2 + 1;
        if (byte_addr < vram.size())
            vram[byte_addr] = data;
        if ((vmain & 0x80) != 0) {
            uint8_t inc = (vmain & 0x03);
            vram_address += (inc == 0) ? 1 : (inc == 1) ? 32 : 128;
        }
        break;
    }

    // ── CGRAM address $2121 ──────────────────────────────────────────────────
    case 0x2121:
        cgram_address = data;
        cgram_latch   = false;
        break;

    // ── CGRAM data $2122 ─────────────────────────────────────────────────────
    case 0x2122:
        if (!cgram_latch) {
            cgram_buffer = data;
            cgram_latch  = true;
        } else {
            uint16_t idx = (uint16_t)cgram_address * 2;
            if (idx + 1 < (int)cgram.size()) {
                cgram[idx]     = cgram_buffer;
                cgram[idx + 1] = data;
            }
            cgram_address++;
            cgram_latch = false;
        }
        break;

    // ── Layer Enable $212C-$212D ─────────────────────────────────────────────
    case 0x212C: tm_main = data; break;
    case 0x212D: tm_sub  = data; break;

    default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Color lookup helper
// ─────────────────────────────────────────────────────────────────────────────
uint32_t PPU::colorFromCGRAM(uint16_t cg_idx) {
    if (cg_idx * 2 + 1 >= cgram.size()) return 0xFF000000u;
    uint16_t c15 = (uint16_t)cgram[cg_idx * 2] | ((uint16_t)cgram[cg_idx * 2 + 1] << 8);
    uint8_t  r   = (uint8_t)((c15 & 0x001F) << 3); r |= (r >> 5);
    uint8_t  g   = (uint8_t)(((c15 >> 5)  & 0x1F) << 3); g |= (g >> 5);
    uint8_t  b   = (uint8_t)(((c15 >> 10) & 0x1F) << 3); b |= (b >> 5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Background Layer Rendering  (NOW WITH SCROLLING)
// ─────────────────────────────────────────────────────────────────────────────

void PPU::renderBG(int bg_num, int bpp) {
    uint32_t map_base = ((bg_sc[bg_num] >> 2) & 0x3F) * 0x400;

    // Tilemap mirroring: bits 0-1 of bg_sc control 32x32/64x32/32x64/64x64
    uint8_t sc_size = bg_sc[bg_num] & 0x03;
    int map_w = (sc_size & 0x01) ? 64 : 32;  // tiles wide
    int map_h = (sc_size & 0x02) ? 64 : 32;  // tiles tall

    uint32_t tile_base;
    if      (bg_num == 0) tile_base = (uint32_t)(bg_nba[0] & 0x0F)        * 0x1000;
    else if (bg_num == 1) tile_base = (uint32_t)((bg_nba[0] >> 4) & 0x0F) * 0x1000;
    else if (bg_num == 2) tile_base = (uint32_t)(bg_nba[1] & 0x0F)        * 0x1000;
    else                  tile_base = (uint32_t)((bg_nba[1] >> 4) & 0x0F) * 0x1000;

    const int words_per_tile  = (bpp == 2) ? 8 : 16;
    const int palette_entries = (bpp == 2) ? 4 : 16;
    // Palette base for 2bpp varies by BG layer in Mode 0
    int palette_base_offset = 0;
    if (bpp == 2 && (bgmode & 0x07) == 0) {
        palette_base_offset = bg_num * 32;  // Each BG gets 32 colors (8 palettes of 4)
    }

    uint16_t scroll_x = bg_hofs[bg_num];
    uint16_t scroll_y = bg_vofs[bg_num];

    for (int screen_y = 0; screen_y < 224; screen_y++) {
        int y = (screen_y + scroll_y) & ((map_h * 8) - 1);
        int tile_y = y / 8;
        int fine_y = y % 8;

        for (int screen_x = 0; screen_x < 256; screen_x++) {
            int x = (screen_x + scroll_x) & ((map_w * 8) - 1);
            int tile_x = x / 8;
            int fine_x = x % 8;

            // Figure out which 32x32 screen block this tile belongs to
            uint32_t screen_block = 0;
            int local_tx = tile_x;
            int local_ty = tile_y;
            if (map_w == 64 && tile_x >= 32) { screen_block += 1; local_tx -= 32; }
            if (map_h == 64 && tile_y >= 32) { screen_block += (map_w == 64) ? 2 : 1; local_ty -= 32; }

            uint32_t map_word = map_base + screen_block * 0x400 
                              + (uint32_t)local_ty * 32 + local_tx;
            if (map_word * 2 + 1 >= vram.size()) continue;

            uint16_t entry    = (uint16_t)vram[map_word * 2] | ((uint16_t)vram[map_word * 2 + 1] << 8);
            uint16_t tile_num = entry & 0x03FF;
            uint8_t  palette  = (entry >> 10) & 0x07;
            bool     hflip    = (entry & 0x4000) != 0;
            bool     vflip    = (entry & 0x8000) != 0;

            int row = vflip ? (7 - fine_y) : fine_y;
            int bit = hflip ? fine_x : (7 - fine_x);

            uint32_t tile_word = tile_base + (uint32_t)tile_num * words_per_tile;
            uint32_t bp01_word = tile_word + row;
            if (bp01_word * 2 + 1 >= vram.size()) continue;

            uint8_t bp0 = vram[bp01_word * 2];
            uint8_t bp1 = vram[bp01_word * 2 + 1];
            uint8_t bp2 = 0, bp3 = 0;
            if (bpp == 4) {
                uint32_t bp23_word = tile_word + 8 + row;
                if (bp23_word * 2 + 1 < vram.size()) {
                    bp2 = vram[bp23_word * 2];
                    bp3 = vram[bp23_word * 2 + 1];
                }
            }

            uint8_t color_idx = ((bp0 >> bit) & 1) | (((bp1 >> bit) & 1) << 1);
            if (bpp == 4) {
                color_idx |= (((bp2 >> bit) & 1) << 2) | (((bp3 >> bit) & 1) << 3);
            }

            if (color_idx == 0) continue;  // transparent

            uint16_t cg_idx = (uint16_t)palette * palette_entries + color_idx 
                            + palette_base_offset;
            uint32_t c32 = colorFromCGRAM(cg_idx);

            framebuffer[screen_y * 256 + screen_x] = c32;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Sprite Rendering
// ─────────────────────────────────────────────────────────────────────────────

void PPU::renderSprites() {
    // Object sizes based on OBSEL bits 5-7
    static const int obj_sizes[8][2][2] = {
        // {small_w, small_h}, {large_w, large_h}
        {{8,8},   {16,16}},   // 0
        {{8,8},   {32,32}},   // 1
        {{8,8},   {64,64}},   // 2
        {{16,16}, {32,32}},   // 3
        {{16,16}, {64,64}},   // 4
        {{32,32}, {64,64}},   // 5
        {{16,32}, {32,64}},   // 6
        {{16,32}, {32,32}},   // 7
    };

    uint8_t  size_sel = (obsel >> 5) & 0x07;
    uint32_t name_base  = (uint32_t)(obsel & 0x07) * 0x2000;  // word address
    uint32_t name_gap   = ((uint32_t)((obsel >> 3) & 0x03) + 1) * 0x1000;

    // Iterate sprites back-to-front (sprite 127 lowest priority → sprite 0 highest)
    for (int s = 127; s >= 0; s--) {
        // Read OAM low table (4 bytes per sprite)
        int base = s * 4;
        int16_t  sx    = oam[base + 0];
        uint8_t  sy_u  = oam[base + 1];
        uint16_t tile  = oam[base + 2];
        uint8_t  attr  = oam[base + 3];
        
        // Read OAM high table (2 bits per sprite)
        int hi_byte  = 0x200 + (s >> 2);
        int hi_shift = (s & 3) * 2;
        uint8_t hi_bits = (oam[hi_byte] >> hi_shift) & 0x03;
        
        // Bit 0 = X bit 8, Bit 1 = size select
        if (hi_bits & 0x01) sx = (int16_t)(sx | 0xFF00);  // Sign extend X
        bool large = (hi_bits & 0x02) != 0;

        int obj_w = obj_sizes[size_sel][large ? 1 : 0][0];
        int obj_h = obj_sizes[size_sel][large ? 1 : 0][1];

        bool hflip   = (attr & 0x40) != 0;
        bool vflip   = (attr & 0x80) != 0;
        uint8_t pal  = (attr >> 1) & 0x07;  // Palette 0-7
        // bool priority_bit = (attr >> 4) & 0x03; // Priority (simplified: draw all)

        int sy = (int)sy_u;

        for (int py = 0; py < obj_h; py++) {
            int screen_y = (sy + py + 1) & 0xFF;  // +1 because OAM Y is 1 scanline early
            if (screen_y >= 224) continue;

            int row = vflip ? (obj_h - 1 - py) : py;
            int tile_row = row / 8;
            int fine_row = row % 8;

            for (int px = 0; px < obj_w; px++) {
                int screen_x = sx + px;
                if (screen_x < 0 || screen_x >= 256) continue;

                int col = hflip ? (obj_w - 1 - px) : px;
                int tile_col = col / 8;
                int fine_col = col % 8;

                // Calculate which tile in the sprite grid
                uint16_t t = tile;
                // Name table select: if tile bit 8 is set, use gap
                uint32_t base_addr = name_base;
                if (t & 0x100) { base_addr += name_gap; t &= 0xFF; }

                t += tile_row * 16 + tile_col;  // 16 tiles per row in OBJ name table
                t &= 0xFF;  // Wrap within the 256-tile name page

                uint32_t tile_word = base_addr + (uint32_t)t * 16;  // 4bpp = 16 words/tile

                uint32_t bp01 = tile_word + fine_row;
                uint32_t bp23 = tile_word + 8 + fine_row;
                if (bp23 * 2 + 1 >= vram.size()) continue;

                int bit = 7 - fine_col;
                uint8_t b0 = vram[bp01 * 2];
                uint8_t b1 = vram[bp01 * 2 + 1];
                uint8_t b2 = vram[bp23 * 2];
                uint8_t b3 = vram[bp23 * 2 + 1];

                uint8_t ci = ((b0 >> bit) & 1)
                           | (((b1 >> bit) & 1) << 1)
                           | (((b2 >> bit) & 1) << 2)
                           | (((b3 >> bit) & 1) << 3);

                if (ci == 0) continue;

                // Sprite palettes start at CGRAM entry 128 (word offset)
                uint16_t cg_idx = 128 + pal * 16 + ci;
                uint32_t c32 = colorFromCGRAM(cg_idx);
                framebuffer[screen_y * 256 + screen_x] = c32;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Frame Render
// ─────────────────────────────────────────────────────────────────────────────

void PPU::renderFrame() {
    vblank_flag = false;

    // If forced blank is on, render black
    if (inidisp & 0x80) {
        framebuffer.fill(0xFF000000u);
        vblank_flag = true;
        return;
    }

    // Brightness (0-15)
    uint8_t brightness = inidisp & 0x0F;

    // Clear to backdrop colour (CGRAM[0])
    uint32_t backdrop = colorFromCGRAM(0);
    framebuffer.fill(backdrop);

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
    case 3:
        if (tm_main & 0x02) renderBG(1, 4);
        if (tm_main & 0x01) renderBG(0, 4);
        break;
    case 5:
        if (tm_main & 0x02) renderBG(1, 4);
        if (tm_main & 0x01) renderBG(0, 4);
        break;
    case 7:
        break;
    default:
        break;
    }

    // Render sprites on top (if OBJ layer enabled)
    if (tm_main & 0x10) renderSprites();

    // Apply brightness
    if (brightness < 15) {
        for (auto& pixel : framebuffer) {
            uint8_t r = (pixel >> 16) & 0xFF;
            uint8_t g = (pixel >> 8) & 0xFF;
            uint8_t b = pixel & 0xFF;
            r = (uint8_t)((r * brightness) / 15);
            g = (uint8_t)((g * brightness) / 15);
            b = (uint8_t)((b * brightness) / 15);
            pixel = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
        }
    }

    vblank_flag = true;
}
