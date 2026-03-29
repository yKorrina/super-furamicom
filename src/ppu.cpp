#include "ppu.hpp"
#include <cstring>
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

inline int clipMode7Delta10(int value) {
    const int clipped = value & 0x03FF;
    return (value & 0x2000) ? (clipped - 0x0400) : clipped;
}

inline uint16_t vramIncrementWords(uint8_t vmain) {
    switch (vmain & 0x03) {
    case 1: return 32;
    case 2:
    case 3: return 128;
    default: return 1;
    }
}

inline uint16_t packBgr15(int r, int g, int b) {
    return (uint16_t)((std::clamp(r, 0, 31)) |
        (std::clamp(g, 0, 31) << 5) |
        (std::clamp(b, 0, 31) << 10));
}

inline uint32_t bgr15ToArgb(uint16_t color) {
    uint8_t r = (uint8_t)((color & 0x001F) << 3); r |= (r >> 5);
    uint8_t g = (uint8_t)(((color >> 5) & 0x001F) << 3); g |= (g >> 5);
    uint8_t b = (uint8_t)(((color >> 10) & 0x001F) << 3); b |= (b >> 5);
    return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

inline uint16_t halveAddBgr15(uint16_t main_color, uint16_t addend_color) {
    return packBgr15(
        ((main_color & 0x1F) + (addend_color & 0x1F)) >> 1,
        (((main_color >> 5) & 0x1F) + ((addend_color >> 5) & 0x1F)) >> 1,
        (((main_color >> 10) & 0x1F) + ((addend_color >> 10) & 0x1F)) >> 1);
}

inline uint16_t addBgr15(uint16_t main_color, uint16_t addend_color) {
    return packBgr15(
        (main_color & 0x1F) + (addend_color & 0x1F),
        ((main_color >> 5) & 0x1F) + ((addend_color >> 5) & 0x1F),
        ((main_color >> 10) & 0x1F) + ((addend_color >> 10) & 0x1F));
}

inline uint16_t subtractBgr15(uint16_t main_color, uint16_t addend_color, bool half) {
    int r = main_color & 0x1F;
    int g = (main_color >> 5) & 0x1F;
    int b = (main_color >> 10) & 0x1F;
    r = std::max(0, r - (addend_color & 0x1F));
    g = std::max(0, g - ((addend_color >> 5) & 0x1F));
    b = std::max(0, b - ((addend_color >> 10) & 0x1F));
    if (half) {
        r >>= 1;
        g >>= 1;
        b >>= 1;
    }
    return (uint16_t)(r | (g << 5) | (b << 10));
}

inline uint16_t combineBgr15(uint16_t main_color, uint16_t addend_color, bool subtract, bool half) {
    if (subtract) {
        return subtractBgr15(main_color, addend_color, half);
    }
    if (half) {
        return halveAddBgr15(main_color, addend_color);
    }
    return addBgr15(main_color, addend_color);
}

inline uint16_t applyBrightnessBgr15(uint16_t color, uint8_t brightness) {
    if (brightness >= 15) return color;
    return packBgr15(
        ((color & 0x1F) * brightness) / 15,
        (((color >> 5) & 0x1F) * brightness) / 15,
        (((color >> 10) & 0x1F) * brightness) / 15);
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
    m7a = m7b = m7c = m7d = 0; m7byte = 0; mpy_result = 0;
    ophct = opvct = 0; ophct_latch = opvct_latch = false;
    current_hcounter = 0;
    current_vcounter = 0;

    memset(bg_sc, 0, sizeof(bg_sc));
    memset(bg_nba, 0, sizeof(bg_nba));
    memset(bg_hofs, 0, sizeof(bg_hofs));
    memset(bg_vofs, 0, sizeof(bg_vofs));
    bg_scroll_latch = 0;
    bg_hofs_latch = 0;
    tm_main = 0; tm_sub = 0;
    live_main_layer_mask = 0x1F;
    live_sub_layer_mask = 0x1F;
    tmw_main = 0; tsw_sub = 0;
    cgwsel = 0; cgadsub = 0; fixed_color = 0; setini = 0;
    w12sel = 0; w34sel = 0; wobjsel = 0;
    wbglog = 0; wobjlog = 0;
    wh0 = 0; wh1 = 0; wh2 = 0; wh3 = 0;
    m7x = m7y = 0;
    m7hofs = m7vofs = 0;
    m7sel = 0;
    framebuffer.fill(0);
}

uint16_t PPU::translateVRAMAddress(uint16_t addr) {
    switch ((vmain >> 2) & 0x03) {
    case 0: return addr & 0x7FFF;
    case 1: return ((addr & 0xFF00) | ((addr & 0x001F) << 3) | ((addr >> 5) & 0x07)) & 0x7FFF;
    case 2: return ((addr & 0xFE00) | ((addr & 0x003F) << 3) | ((addr >> 6) & 0x07)) & 0x7FFF;
    case 3: return ((addr & 0xFC00) | ((addr & 0x007F) << 3) | ((addr >> 7) & 0x07)) & 0x7FFF;
    }
    return addr & 0x7FFF;
}

uint8_t PPU::readRegister(uint16_t address) {
    switch (address) {
    case 0x2134: return mpy_result & 0xFF;
    case 0x2135: return (mpy_result >> 8) & 0xFF;
    case 0x2136: return (mpy_result >> 16) & 0xFF;

    case 0x2137:
        ophct = current_hcounter;
        opvct = current_vcounter;
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
            val = (idx < cgram.size()) ? (cgram[idx] & 0x7F) : 0;
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
            if (cpuCanAccessOAM() && oam_write_address < (int)oam.size()) oam[oam_write_address] = data;
            oam_write_address++;
        } else if (!oam_latch) {
            oam_buffer = data; oam_latch = true;
        } else {
            if (cpuCanAccessOAM() && oam_write_address < (int)oam.size()) oam[oam_write_address] = oam_buffer;
            if (cpuCanAccessOAM() && oam_write_address + 1 < (int)oam.size()) oam[oam_write_address + 1] = data;
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
        m7hofs = ((uint16_t)data << 8) | m7byte;
        bg_hofs[0] = ((uint16_t)data << 8) | (bg_scroll_latch & ~7) | (bg_hofs_latch & 0x07);
        m7byte = data;
        bg_scroll_latch = data;
        bg_hofs_latch = data;
        break;
    case 0x210E:
        m7vofs = ((uint16_t)data << 8) | m7byte;
        bg_vofs[0] = (data << 8) | bg_scroll_latch;
        m7byte = data;
        bg_scroll_latch = data;
        break;
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
    case 0x211A: m7sel = data; break;

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

    case 0x2118: {
        const uint16_t inc_words = vramIncrementWords(vmain);
        if (!cpuCanAccessVRAM()) {
            if ((vmain & 0x80) != 0) {
                vram_address = (vram_address + inc_words) & 0x7FFF;
            }
            break;
        }
        uint16_t ta = translateVRAMAddress(vram_address);
        uint32_t ba = (uint32_t)ta * 2;
        vram[ba] = data;
        if ((vmain & 0x80) == 0) {
            vram_address = (vram_address + inc_words) & 0x7FFF;
        }
        break;
    }
    case 0x2119: {
        const uint16_t inc_words = vramIncrementWords(vmain);
        if (!cpuCanAccessVRAM()) {
            if ((vmain & 0x80) != 0) {
                vram_address = (vram_address + inc_words) & 0x7FFF;
            }
            break;
        }
        uint16_t ta = translateVRAMAddress(vram_address);
        uint32_t ba = (uint32_t)ta * 2 + 1;
        vram[ba] = data;
        if ((vmain & 0x80) != 0) {
            vram_address = (vram_address + inc_words) & 0x7FFF;
        }
        break;
    }

    case 0x211B: m7a = (data << 8) | m7byte; m7byte = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211C: m7b = (data << 8) | m7byte; m7byte = data;
        mpy_result = (int16_t)m7a * (int8_t)(m7b >> 8); break;
    case 0x211D: m7c = (data << 8) | m7byte; m7byte = data; break;
    case 0x211E: m7d = (data << 8) | m7byte; m7byte = data; break;
    case 0x211F: m7x = (data << 8) | m7byte; m7byte = data; break;
    case 0x2120: m7y = (data << 8) | m7byte; m7byte = data; break;

    case 0x2121: cgram_address = data; cgram_latch = false; break;
    case 0x2122:
        if (!cgram_latch) { cgram_buffer = data; cgram_latch = true; }
        else {
            uint16_t idx = (uint16_t)cgram_address * 2;
            if (cpuCanAccessCGRAM() && idx + 1 < (int)cgram.size()) {
                cgram[idx] = cgram_buffer;
                cgram[idx+1] = data & 0x7F;
            }
            cgram_address++; cgram_latch = false;
        }
        break;

    case 0x2123: w12sel = data; break;
    case 0x2124: w34sel = data; break;
    case 0x2125: wobjsel = data; break;
    case 0x2126: wh0 = data; break;
    case 0x2127: wh1 = data; break;
    case 0x2128: wh2 = data; break;
    case 0x2129: wh3 = data; break;
    case 0x212A: wbglog = data; break;
    case 0x212B: wobjlog = data; break;
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
    case 0x2133: setini = data; break;

    default: break;
    }
}

uint16_t PPU::colorFromCGRAM(uint16_t cg_idx) const {
    if (cg_idx * 2 + 1 >= cgram.size()) return 0;
    return (uint16_t)cgram[cg_idx * 2] | ((uint16_t)cgram[cg_idx * 2 + 1] << 8);
}

bool PPU::cpuCanAccessVRAM() const {
    return vblank_flag || (inidisp & 0x80) != 0;
}

bool PPU::cpuCanAccessOAM() const {
    return vblank_flag || (inidisp & 0x80) != 0;
}

bool PPU::cpuCanAccessCGRAM() const {
    return vblank_flag || hblank_flag || (inidisp & 0x80) != 0;
}

uint16_t PPU::directColorFromMode7(uint8_t color_byte) const {
    const uint16_t r5 = (uint16_t)((color_byte & 0x07) << 2);
    const uint16_t g5 = (uint16_t)(((color_byte >> 3) & 0x07) << 2);
    const uint16_t b5 = (uint16_t)(((color_byte >> 6) & 0x03) << 3);
    return (uint16_t)(r5 | (g5 << 5) | (b5 << 10));
}

uint16_t PPU::directColorFromBG(uint8_t color_byte, uint8_t palette_bits) const {
    const uint16_t r5 = (uint16_t)(((color_byte & 0x07) << 2) | ((palette_bits & 0x01) << 1));
    const uint16_t g5 = (uint16_t)((((color_byte >> 3) & 0x07) << 2) | (palette_bits & 0x02));
    const uint16_t b5 = (uint16_t)((((color_byte >> 6) & 0x03) << 3) | ((palette_bits & 0x04) << 0));
    return (uint16_t)(r5 | (g5 << 5) | (b5 << 10));
}

bool PPU::evaluateWindowMask(uint8_t select, uint8_t logic, int x) const {
    auto evalWindow = [x](bool enabled, bool invert, uint8_t left, uint8_t right) {
        if (!enabled) return false;
        const bool active = left <= right && x >= left && x <= right;
        return invert ? !active : active;
    };

    const bool win1_enabled = (select & 0x02) != 0;
    const bool win1_invert = (select & 0x01) != 0;
    const bool win2_enabled = (select & 0x08) != 0;
    const bool win2_invert = (select & 0x04) != 0;
    const bool win1 = evalWindow(win1_enabled, win1_invert, wh0, wh1);
    const bool win2 = evalWindow(win2_enabled, win2_invert, wh2, wh3);

    if (win1_enabled && win2_enabled) {
        switch (logic & 0x03) {
        case 0: return win1 || win2;
        case 1: return win1 && win2;
        case 2: return win1 != win2;
        case 3: return win1 == win2;
        }
    }
    if (win1_enabled) return win1;
    if (win2_enabled) return win2;
    return false;
}

bool PPU::layerWindowMasked(int layer_index, bool main_screen, int x) const {
    const uint8_t enable_mask = main_screen ? tmw_main : tsw_sub;
    const uint8_t layer_bit = (layer_index >= 0 && layer_index < 4) ? (uint8_t)(1u << layer_index) : 0x10;
    if ((enable_mask & layer_bit) == 0) return false;

    uint8_t select = 0;
    uint8_t logic = 0;
    switch (layer_index) {
    case 0:
        select = w12sel & 0x0F;
        logic = wbglog & 0x03;
        break;
    case 1:
        select = (w12sel >> 4) & 0x0F;
        logic = (wbglog >> 2) & 0x03;
        break;
    case 2:
        select = w34sel & 0x0F;
        logic = (wbglog >> 4) & 0x03;
        break;
    case 3:
        select = (w34sel >> 4) & 0x0F;
        logic = (wbglog >> 6) & 0x03;
        break;
    default:
        select = wobjsel & 0x0F;
        logic = wobjlog & 0x03;
        break;
    }

    return evaluateWindowMask(select, logic, x);
}

bool PPU::colorWindowActive(int x) const {
    return evaluateWindowMask((wobjsel >> 4) & 0x0F, (wobjlog >> 2) & 0x03, x);
}

bool PPU::colorWindowRegionEnabled(uint8_t region_mode, bool window_active) const {
    switch (region_mode & 0x03) {
    case 0: return false;
    case 1: return !window_active;
    case 2: return window_active;
    case 3: return true;
    }
    return false;
}

void PPU::beginFrame() {
    vblank_flag = false;
    hblank_flag = false;
    current_hcounter = 0;
    current_vcounter = 0;
    ophct = 0;
    opvct = 0;
    ophct_latch = false;
    opvct_latch = false;
    frame_main_source_counts.fill(0);
    frame_sub_source_counts.fill(0);
    frame_color_math_pixels = 0;
    frame_black_window_pixels = 0;
    if (inidisp & 0x80) {
        framebuffer.fill(0xFF000000u);
    }
}

void PPU::endFrame() {
    hblank_flag = false;
    vblank_flag = true;
    current_hcounter = 0;
    current_vcounter = 224;
}

void PPU::renderDebugFrame(uint8_t main_mask, uint8_t sub_mask, bool disable_color_math) {
    const bool saved_override = debug_override_masks;
    const bool saved_disable_math = debug_disable_color_math;
    const uint8_t saved_main = debug_tm_main;
    const uint8_t saved_sub = debug_tm_sub;

    debug_override_masks = true;
    debug_disable_color_math = disable_color_math;
    debug_tm_main = main_mask;
    debug_tm_sub = sub_mask;
    renderFrame();

    debug_override_masks = saved_override;
    debug_disable_color_math = saved_disable_math;
    debug_tm_main = saved_main;
    debug_tm_sub = saved_sub;
}

void PPU::renderMode7(int scanline, bool main_screen, uint16_t* target, uint8_t* source) {
    if (scanline < 0 || scanline >= 224) return;

    const bool direct_color = (cgwsel & 0x01) != 0;
    uint8_t repeat_mode = (m7sel >> 6) & 0x03;
    if (repeat_mode == 1) repeat_mode = 0;
    const bool flip_y = (m7sel & 0x02) != 0;
    const bool flip_x = (m7sel & 0x01) != 0;

    auto signExtend13Local = [](uint16_t value) -> int32_t {
        return (static_cast<int32_t>(value) << 19) >> 19;
    };
    const int32_t hofs = signExtend13Local(m7hofs);
    const int32_t vofs = signExtend13Local(m7vofs);
    const int32_t center_x = signExtend13Local(m7x);
    const int32_t center_y = signExtend13Local(m7y);
    const int32_t a = static_cast<int16_t>(m7a);
    const int32_t b = static_cast<int16_t>(m7b);
    const int32_t c = static_cast<int16_t>(m7c);
    const int32_t d = static_cast<int16_t>(m7d);

    const int start_y = flip_y ? 255 - (scanline + 1) : (scanline + 1);
    const int32_t yy = clipMode7Delta10(vofs - center_y);
    const int32_t bb = ((b * start_y) & ~63) + ((b * yy) & ~63) + (center_x << 8);
    const int32_t dd = ((d * start_y) & ~63) + ((d * yy) & ~63) + (center_y << 8);

    const int start_x = flip_x ? 255 : 0;
    const int32_t step_a = flip_x ? -a : a;
    const int32_t step_c = flip_x ? -c : c;
    const int32_t xx = clipMode7Delta10(hofs - center_x);
    int32_t aa = a * start_x + ((a * xx) & ~63);
    int32_t cc = c * start_x + ((c * xx) & ~63);

    for (int scr_x = 0; scr_x < 256; scr_x++, aa += step_a, cc += step_c) {
        if (layerWindowMasked(0, main_screen, scr_x)) continue;

        int32_t tx = (aa + bb) >> 8;
        int32_t ty = (cc + dd) >> 8;

        uint8_t color = 0;
        if (repeat_mode == 0) {
            tx &= 0x03FF;
            ty &= 0x03FF;
        }

        if (repeat_mode != 0 && ((tx | ty) & ~0x03FF) != 0) {
            if (repeat_mode != 3) continue;
            const uint32_t pixel_addr = 1u + (static_cast<uint32_t>(ty & 0x07) << 4) + (static_cast<uint32_t>(tx & 0x07) << 1);
            color = vram[pixel_addr & 0xFFFF];
        } else {
            const uint32_t map_addr = ((static_cast<uint32_t>(ty & ~0x07) << 5) + (static_cast<uint32_t>(tx >> 2) & ~1u)) & 0xFFFFu;
            const uint8_t tile_index = vram[map_addr];
            const uint32_t pixel_addr = (1u + (static_cast<uint32_t>(tile_index) << 7) +
                (static_cast<uint32_t>(ty & 0x07) << 4) + (static_cast<uint32_t>(tx & 0x07) << 1)) & 0xFFFFu;
            color = vram[pixel_addr];
        }

        if (color == 0) continue;
        target[scr_x] = direct_color ? directColorFromMode7(color) : colorFromCGRAM(color);
        source[scr_x] = 1;
    }
}

void PPU::renderBG(int scanline, int bg_num, int bpp, bool high_priority, bool main_screen, uint16_t* target, uint8_t* source) {
    if (scanline < 0 || scanline >= 224) return;

    uint32_t map_base = ((bg_sc[bg_num] >> 2) & 0x3F) * 0x400;
    uint8_t sc_size = bg_sc[bg_num] & 0x03;
    const bool large_tiles = ((bgmode >> (4 + bg_num)) & 0x01) != 0;
    const uint8_t mode = bgmode & 0x07;
    const bool wide_tiles = large_tiles || mode == 5 || mode == 6;
    const bool tall_tiles = large_tiles;
    const int cell_w = wide_tiles ? 16 : 8;
    const int cell_h = tall_tiles ? 16 : 8;
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

    int y = (scanline + sy) & ((map_h * cell_h) - 1);
    int ty = y / cell_h, fy = y % cell_h;

    for (int scr_x = 0; scr_x < 256; scr_x++) {
        if (layerWindowMasked(bg_num, main_screen, scr_x)) continue;

        int x = (scr_x + sx) & ((map_w * cell_w) - 1);
        int tx = x / cell_w, fx = x % cell_w;

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

        int local_x = hf ? (cell_w - 1 - fx) : fx;
        int local_y = vf ? (cell_h - 1 - fy) : fy;
        int subtile_x = local_x >> 3;
        int subtile_y = local_y >> 3;
        int row = local_y & 0x07;
        int bit = 7 - (local_x & 0x07);

        if (wide_tiles || tall_tiles)
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

        const bool direct_color = bpp == 8 && bg_num == 0 && (cgwsel & 0x01) != 0 && (mode == 3 || mode == 4);
        uint16_t cg = (bpp == 8) ? ci : (uint16_t)pal * pe + ci + pal_base;
        target[scr_x] = direct_color ? directColorFromBG(ci, pal) : colorFromCGRAM(cg);
        source[scr_x] = (uint8_t)(bg_num + 1);
    }
}

void PPU::renderSprites(int scanline, int priority, bool main_screen, uint16_t* target, uint8_t* source) {
    if (scanline < 0 || scanline >= 224) return;

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
        uint8_t tile = oam[base+2]; uint8_t attr = oam[base+3];
        int hb = 0x200 + (s >> 2); int hs = (s & 3) * 2;
        uint8_t hi = (oam[hb] >> hs) & 0x03;
        if (hi & 0x01) ox = (int16_t)(ox | 0xFF00);
        bool large = (hi & 0x02) != 0;
        uint8_t obj_priority = (attr >> 4) & 0x03;
        if (obj_priority != priority) continue;
        int ow = obj_sizes[size_sel][large?1:0][0], oh = obj_sizes[size_sel][large?1:0][1];
        bool hf = (attr & 0x40), vf = (attr & 0x80);
        uint8_t pal = (attr >> 1) & 0x07;

        const int sprite_y = oy;
        int sprite_row = scanline - sprite_y;
        if (sprite_row < 0) sprite_row += 256;
        if (sprite_row < 0 || sprite_row >= oh) continue;

        const int row = vf ? (oh - 1 - sprite_row) : sprite_row;
        const int row_tile = row >> 3;
        const int fine_y = row & 7;
        const int tiles_w = ow >> 3;
        const uint16_t tile_base = (uint16_t)tile | (uint16_t)((attr & 0x01) << 8);
        const uint16_t palette_base = (uint16_t)(128 + pal * 16);
        const uint8_t source_id = (pal >= 4) ? 6 : 5;

        for (int tile_col = 0; tile_col < tiles_w; tile_col++) {
            const int src_tile_col = hf ? (tiles_w - 1 - tile_col) : tile_col;
            uint16_t t = (uint16_t)((tile_base + row_tile * 16 + src_tile_col) & 0x01FF);
            uint32_t ba = name_base;
            if (t & 0x100) {
                ba += name_gap;
                t &= 0xFF;
            }

            const uint32_t tw = wrapVRAMWordAddress(ba + (uint32_t)t * 16);
            const uint32_t bp01 = wrapVRAMWordAddress(tw + fine_y);
            const uint32_t bp23 = wrapVRAMWordAddress(tw + 8 + fine_y);
            const uint8_t plane0 = vram[bp01 * 2];
            const uint8_t plane1 = vram[bp01 * 2 + 1];
            const uint8_t plane2 = vram[bp23 * 2];
            const uint8_t plane3 = vram[bp23 * 2 + 1];
            const int tile_x = ox + tile_col * 8;

            for (int pixel = 0; pixel < 8; pixel++) {
                const int scr_x = tile_x + pixel;
                if (scr_x < 0 || scr_x >= 256) continue;
                if (layerWindowMasked(4, main_screen, scr_x)) continue;

                const int bit = hf ? pixel : (7 - pixel);
                const uint8_t ci = ((plane0 >> bit) & 1) |
                                   (((plane1 >> bit) & 1) << 1) |
                                   (((plane2 >> bit) & 1) << 2) |
                                   (((plane3 >> bit) & 1) << 3);
                if (ci == 0) continue;

                target[scr_x] = colorFromCGRAM((uint16_t)(palette_base + ci));
                source[scr_x] = source_id;
            }
        }
    }
}

void PPU::renderFrame() {
    beginFrame();
    if ((inidisp & 0x80) == 0) {
        for (int scanline = 0; scanline < 224; scanline++) {
            renderScanline(scanline);
        }
    }
    endFrame();
}

void PPU::renderScanline(int scanline) {
    if (scanline < 0 || scanline >= 224) return;

    current_hcounter = 0;
    current_vcounter = (uint16_t)scanline;

    const size_t base = (size_t)scanline * 256;
    if (inidisp & 0x80) {
        std::fill_n(framebuffer.data() + base, 256, 0xFF000000u);
        return;
    }

    std::array<uint16_t, 256> main_fb;
    std::array<uint16_t, 256> sub_fb;
    std::array<uint8_t, 256> main_src;
    std::array<uint8_t, 256> sub_src;

    const uint16_t backdrop = colorFromCGRAM(0);
    const uint16_t fixed_color_15 = fixed_color & 0x7FFF;
    main_fb.fill(backdrop);
    sub_fb.fill(fixed_color_15);
    main_src.fill(0);
    sub_src.fill(0);

    auto renderScreen = [this, scanline](uint8_t layer_mask, bool main_screen, uint16_t* target, uint8_t* source) {
        auto drawOBJ = [&](int prio) {
            if (layer_mask & 0x10) renderSprites(scanline, prio, main_screen, target, source);
        };
        auto drawBG = [&](int bg_num, int bpp, bool high_priority) {
            if (layer_mask & (1u << bg_num)) {
                renderBG(scanline, bg_num, bpp, high_priority, main_screen, target, source);
            }
        };

        uint8_t mode = bgmode & 0x07;
        switch (mode) {
        case 0:
            drawBG(3, 2, false);
            drawBG(2, 2, false);
            drawOBJ(0);
            drawBG(3, 2, true);
            drawBG(2, 2, true);
            drawOBJ(1);
            drawBG(1, 2, false);
            drawBG(0, 2, false);
            drawOBJ(2);
            drawBG(1, 2, true);
            drawBG(0, 2, true);
            drawOBJ(3);
            break;
        case 1:
            if (bgmode & 0x08) {
                drawBG(2, 2, false);
                drawOBJ(0);
                drawOBJ(1);
                drawBG(1, 4, false);
                drawBG(0, 4, false);
                drawOBJ(2);
                drawBG(1, 4, true);
                drawBG(0, 4, true);
                drawOBJ(3);
                drawBG(2, 2, true);
            } else {
                drawBG(2, 2, false);
                drawOBJ(0);
                drawBG(2, 2, true);
                drawOBJ(1);
                drawBG(1, 4, false);
                drawBG(0, 4, false);
                drawOBJ(2);
                drawBG(1, 4, true);
                drawBG(0, 4, true);
                drawOBJ(3);
            }
            break;
        case 2:
            drawBG(1, 4, false);
            drawOBJ(0);
            drawBG(0, 4, false);
            drawOBJ(1);
            drawBG(1, 4, true);
            drawOBJ(2);
            drawBG(0, 4, true);
            drawOBJ(3);
            break;
        case 3:
            drawBG(1, 4, false);
            drawOBJ(0);
            drawBG(0, 8, false);
            drawOBJ(1);
            drawBG(1, 4, true);
            drawOBJ(2);
            drawBG(0, 8, true);
            drawOBJ(3);
            break;
        case 4:
            drawBG(1, 2, false);
            drawOBJ(0);
            drawBG(0, 8, false);
            drawOBJ(1);
            drawBG(1, 2, true);
            drawOBJ(2);
            drawBG(0, 8, true);
            drawOBJ(3);
            break;
        case 5:
            drawBG(1, 2, false);
            drawOBJ(0);
            drawBG(0, 4, false);
            drawOBJ(1);
            drawBG(1, 2, true);
            drawOBJ(2);
            drawBG(0, 4, true);
            drawOBJ(3);
            break;
        case 6:
            drawOBJ(0);
            drawBG(0, 4, false);
            drawOBJ(1);
            drawOBJ(2);
            drawBG(0, 4, true);
            drawOBJ(3);
            break;
        case 7:
            drawOBJ(0);
            if (layer_mask & 0x01) renderMode7(scanline, main_screen, target, source);
            drawOBJ(1);
            drawOBJ(2);
            drawOBJ(3);
            break;
        default:
            break;
        }
    };

    const uint8_t main_mask = debug_override_masks ? debug_tm_main : (tm_main & live_main_layer_mask);
    const uint8_t sub_mask = debug_override_masks ? debug_tm_sub : (tm_sub & live_sub_layer_mask);

    renderScreen(main_mask, true, main_fb.data(), main_src.data());
    if (sub_mask) renderScreen(sub_mask, false, sub_fb.data(), sub_src.data());

    const bool use_subscreen = !debug_disable_color_math && (cgwsel & 0x02) != 0;
    const bool subtract = !debug_disable_color_math && (cgadsub & 0x80) != 0;
    const bool half = !debug_disable_color_math && (cgadsub & 0x40) != 0;
    const uint8_t main_black_region = (cgwsel >> 6) & 0x03;
    const uint8_t sub_transparent_region = (cgwsel >> 4) & 0x03;
    const uint8_t brightness = inidisp & 0x0F;

    auto colorMathEnabled = [this](uint8_t src) {
        switch (src) {
        case 0: return (cgadsub & 0x20) != 0;
        case 1: return (cgadsub & 0x01) != 0;
        case 2: return (cgadsub & 0x02) != 0;
        case 3: return (cgadsub & 0x04) != 0;
        case 4: return (cgadsub & 0x08) != 0;
        case 6: return (cgadsub & 0x10) != 0;
        default: return false;
        }
    };

    for (int x = 0; x < 256; x++) {
        frame_main_source_counts[std::min<std::size_t>(frame_main_source_counts.size() - 1, main_src[x])]++;
        frame_sub_source_counts[std::min<std::size_t>(frame_sub_source_counts.size() - 1, sub_src[x])]++;

        const bool color_window = colorWindowActive(x);
        uint16_t pixel = main_fb[x];
        if (colorWindowRegionEnabled(main_black_region, color_window)) {
            pixel = 0;
            frame_black_window_pixels++;
        }

        const bool allow_math = !debug_disable_color_math && colorMathEnabled(main_src[x]);
        if (allow_math) {
            uint16_t addend = use_subscreen ? sub_fb[x] : fixed_color_15;
            if (colorWindowRegionEnabled(sub_transparent_region, color_window)) {
                addend = 0;
            }
            pixel = combineBgr15(pixel, addend, subtract, half);
            frame_color_math_pixels++;
        }

        framebuffer[base + x] = bgr15ToArgb(applyBrightnessBgr15(pixel, brightness));
    }
}

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
