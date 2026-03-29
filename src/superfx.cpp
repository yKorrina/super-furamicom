#include "superfx.hpp"
#include <cstring>

namespace {
uint8_t lowByte(uint16_t v) { return (uint8_t)(v & 0xFF); }
uint8_t highByte(uint16_t v) { return (uint8_t)((v >> 8) & 0xFF); }
}

SuperFX::SuperFX(Revision revision_in)
    : revision(revision_in),
      vcr(resolveVersionCode(revision_in)) {
    reset();
}

uint8_t SuperFX::resolveVersionCode(Revision rev) {
    switch (rev) {
    case Revision::Mario:
    case Revision::GSU1: return 0x01;
    case Revision::GSU2: return 0x02;
    case Revision::GSU2SP1: return 0x03;
    default: return 0x00;
    }
}

const char* SuperFX::getRevisionName() const {
    switch (revision) {
    case Revision::Mario: return "MARIO";
    case Revision::GSU1: return "GSU-1";
    case Revision::GSU2: return "GSU-2";
    case Revision::GSU2SP1: return "GSU-2 SP1";
    default: return "NONE";
    }
}

void SuperFX::reset() {
    R.fill(0);
    sfr = 0;
    bramr = 0;
    pbr = 0;
    rombr = 0;
    cfgr = 0;
    scbr = 0;
    clsr = 0;
    scmr = 0;
    rambr = 0;
    cbr = 0;
    cache.fill(0);
    cache_valid.fill(false);
    sreg = 0; dreg = 0;
    sreg_set = false; dreg_set = false;
    rom_buffer = 0;
    rom_read_addr = 0;
    ram_buffer_lo = 0; ram_buffer_hi = 0;
    colr = 0; por = 0;
    pixel_cache = {};
    irq_line_pending = false;
    launch_count = 0;
    last_launch_pc = 0;
    cycle_accumulator = 0;
}

void SuperFX::setROM(const uint8_t* data, std::size_t size) {
    rom_data = data;
    rom_size = size;
}

void SuperFX::setRAM(uint8_t* data, std::size_t size) {
    ram_data = data;
    ram_size = size;
}

// --- ROM/RAM access ---

uint8_t SuperFX::gsuReadROM(uint32_t addr) {
    if (!rom_data || rom_size == 0) return 0;
    return rom_data[addr % rom_size];
}

uint8_t SuperFX::gsuReadRAM(uint16_t addr) {
    if (!ram_data || ram_size == 0) return 0;
    return ram_data[addr % ram_size];
}

void SuperFX::gsuWriteRAM(uint16_t addr, uint8_t data) {
    if (!ram_data || ram_size == 0) return;
    ram_data[addr % ram_size] = data;
}

uint8_t SuperFX::readCacheOrROM(uint16_t addr) {
    uint16_t cache_base = cbr & 0xFFF0;
    if (addr >= cache_base && addr < (uint16_t)(cache_base + 512)) {
        uint16_t off = addr - cache_base;
        uint8_t line = off >> 4;
        if (cache_valid[line]) return cache[off];
    }
    // Read from ROM
    uint32_t full = ((uint32_t)pbr << 16) | addr;
    return gsuReadROM(full);
}

int SuperFX::getScreenWidth() const {
    switch (scmr & 0x24) {
    case 0x00: return 128;
    case 0x04: return 160;
    case 0x20: return 192;
    case 0x24: return 256;
    default: return 128;
    }
}

int SuperFX::getBPP() const {
    switch (scmr & 0x03) {
    case 0: return 2;
    case 1: return 4;
    case 2:
    case 3: return 8;
    default: return 2;
    }
}

void SuperFX::plotPixel(uint8_t x, uint8_t y, uint8_t color) {
    if (!ram_data || ram_size == 0) return;

    int bpp = getBPP();
    int width = getScreenWidth();
    int tile_x = x >> 3;
    int tile_y = y >> 3;
    int pixel_x = x & 7;
    int pixel_y = y & 7;

    int tiles_per_row = width >> 3;
    int tile_index = tile_y * tiles_per_row + tile_x;
    int char_size = (bpp == 2) ? 16 : (bpp == 4 ? 32 : 64);

    uint32_t base = ((uint32_t)scbr << 10) + tile_index * char_size;

    for (int bp = 0; bp < bpp; bp++) {
        int byte_offset;
        if (bp < 2) {
            byte_offset = pixel_y * 2 + bp;
        } else if (bp < 4) {
            byte_offset = 16 + pixel_y * 2 + (bp - 2);
        } else {
            byte_offset = 32 + pixel_y * 2 + (bp - 4);
        }

        uint32_t addr = (base + byte_offset) % ram_size;
        uint8_t mask = 1 << (7 - pixel_x);
        uint8_t bit = (color >> bp) & 1;

        if (por & 0x01) {
            // Transparent: don't write zero bits
            if (bit) ram_data[addr] |= mask;
        } else if (por & 0x02) {
            // Dither mode
            if (bit) ram_data[addr] |= mask;
            else ram_data[addr] &= ~mask;
        } else {
            if (bit) ram_data[addr] |= mask;
            else ram_data[addr] &= ~mask;
        }
    }
}

// --- GSU instruction execution ---

void SuperFX::executeInstruction() {
    if (!(sfr & FLAG_GO)) return;

    uint8_t opcode = readCacheOrROM(R[15]);
    R[15]++;

    // Capture prefix state
    bool alt1 = (sfr & FLAG_ALT1) != 0;
    bool alt2 = (sfr & FLAG_ALT2) != 0;
    bool b_flag = (sfr & FLAG_B) != 0;

    // Source/dest register selection
    uint8_t src = sreg_set ? sreg : 0;
    uint8_t dst = dreg_set ? dreg : 0;

    // Clear prefix after use for most instructions
    auto clearPrefix = [&]() {
        sfr &= ~FLAG_PREFIX;
        sreg_set = false;
        dreg_set = false;
        sreg = 0;
        dreg = 0;
    };

    auto setZ = [&](uint16_t v) { if (v == 0) sfr |= FLAG_Z; else sfr &= ~FLAG_Z; };
    auto setS = [&](uint16_t v) { if (v & 0x8000) sfr |= FLAG_S; else sfr &= ~FLAG_S; };
    auto setZS = [&](uint16_t v) { setZ(v); setS(v); };

    auto writeDreg = [&](uint16_t val) {
        R[dst] = val;
        setZS(val);
        if (dst == 14) {
            // R14 written: buffer ROM byte
            uint32_t full = ((uint32_t)rombr << 16) | R[14];
            rom_buffer = gsuReadROM(full);
        }
    };

    auto readSreg = [&]() -> uint16_t { return R[src]; };

    switch (opcode) {
    // STOP
    case 0x00:
        if (!alt1) {
            sfr &= ~FLAG_GO;
            sfr &= ~FLAG_PREFIX;
            if ((cfgr & 0x80) == 0) {
                sfr |= FLAG_IRQ;
                irq_line_pending = true;
            }
            flushPixelCache();
        }
        clearPrefix();
        return;

    // NOP
    case 0x01:
        clearPrefix();
        return;

    // CACHE
    case 0x02: {
        uint16_t pc_base = R[15] & 0xFFF0;
        if (cbr != pc_base) {
            cbr = pc_base;
            cache_valid.fill(false);
        }
        clearPrefix();
        return;
    }

    // LSR
    case 0x03:
        sfr &= ~FLAG_CY;
        if (readSreg() & 1) sfr |= FLAG_CY;
        writeDreg(readSreg() >> 1);
        clearPrefix();
        return;

    // ROL
    case 0x04: {
        uint16_t val = readSreg();
        uint16_t carry = (sfr & FLAG_CY) ? 1 : 0;
        sfr &= ~FLAG_CY;
        if (val & 0x8000) sfr |= FLAG_CY;
        writeDreg((val << 1) | carry);
        clearPrefix();
        return;
    }

    // BRA
    case 0x05: {
        int8_t rel = (int8_t)readCacheOrROM(R[15]);
        R[15]++;
        R[15] = (uint16_t)(R[15] + rel);
        clearPrefix();
        return;
    }

    // BGE/BLT/BNE/BEQ/BPL/BMI/BCC/BCS/BVC/BVS
    case 0x06: case 0x07: case 0x08: case 0x09:
    case 0x0A: case 0x0B: case 0x0C: case 0x0D:
    case 0x0E: case 0x0F: {
        int8_t rel = (int8_t)readCacheOrROM(R[15]);
        R[15]++;
        bool take = false;
        switch (opcode) {
        case 0x06: take = ((sfr & FLAG_S) != 0) == ((sfr & FLAG_OV) != 0); break; // BGE
        case 0x07: take = ((sfr & FLAG_S) != 0) != ((sfr & FLAG_OV) != 0); break; // BLT
        case 0x08: take = (sfr & FLAG_Z) == 0; break;  // BNE
        case 0x09: take = (sfr & FLAG_Z) != 0; break;  // BEQ
        case 0x0A: take = (sfr & FLAG_S) == 0; break;  // BPL
        case 0x0B: take = (sfr & FLAG_S) != 0; break;  // BMI
        case 0x0C: take = (sfr & FLAG_CY) == 0; break; // BCC
        case 0x0D: take = (sfr & FLAG_CY) != 0; break; // BCS
        case 0x0E: take = (sfr & FLAG_OV) == 0; break; // BVC
        case 0x0F: take = (sfr & FLAG_OV) != 0; break; // BVS
        }
        if (take) R[15] = (uint16_t)(R[15] + rel);
        clearPrefix();
        return;
    }

    // TO R0-R15
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x14: case 0x15: case 0x16: case 0x17:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x1C: case 0x1D: case 0x1E: case 0x1F:
        if (b_flag) {
            // MOVE Rn, Rsrc
            R[opcode & 0x0F] = readSreg();
            setZS(R[opcode & 0x0F]);
            clearPrefix();
        } else {
            dreg = opcode & 0x0F;
            dreg_set = true;
        }
        return;

    // WITH R0-R15
    case 0x20: case 0x21: case 0x22: case 0x23:
    case 0x24: case 0x25: case 0x26: case 0x27:
    case 0x28: case 0x29: case 0x2A: case 0x2B:
    case 0x2C: case 0x2D: case 0x2E: case 0x2F:
        sreg = opcode & 0x0F;
        dreg = opcode & 0x0F;
        sreg_set = true;
        dreg_set = true;
        sfr |= FLAG_B;
        return;

    // STW (Rn) / STB (Rn)
    case 0x30: case 0x31: case 0x32: case 0x33:
    case 0x34: case 0x35: case 0x36: case 0x37:
    case 0x38: case 0x39: case 0x3A: case 0x3B: {
        uint8_t n = opcode & 0x0F;
        uint16_t addr = R[n];
        uint16_t val = readSreg();
        if (alt1) {
            // STB
            gsuWriteRAM(addr, val & 0xFF);
        } else {
            // STW
            gsuWriteRAM(addr, val & 0xFF);
            gsuWriteRAM(addr ^ 1, (val >> 8) & 0xFF);
        }
        clearPrefix();
        return;
    }

    // LOOP
    case 0x3C: {
        R[12]--;
        setZS(R[12]);
        if (R[12] != 0) R[15] = R[13];
        clearPrefix();
        return;
    }

    // ALT1/ALT2/ALT3
    case 0x3D:
        sfr |= FLAG_ALT1;
        return;
    case 0x3E:
        sfr |= FLAG_ALT2;
        return;
    case 0x3F:
        sfr |= FLAG_ALT1 | FLAG_ALT2;
        return;

    // LDW (Rn) / LDB (Rn)
    case 0x40: case 0x41: case 0x42: case 0x43:
    case 0x44: case 0x45: case 0x46: case 0x47:
    case 0x48: case 0x49: case 0x4A: case 0x4B: {
        uint8_t n = opcode & 0x0F;
        uint16_t addr = R[n];
        uint16_t val;
        if (alt1) {
            // LDB
            val = gsuReadRAM(addr);
        } else {
            // LDW
            val = gsuReadRAM(addr) | ((uint16_t)gsuReadRAM(addr ^ 1) << 8);
        }
        writeDreg(val);
        clearPrefix();
        return;
    }

    // PLOT / RPIX
    case 0x4C:
        if (alt1) {
            // RPIX - read pixel (rarely used)
            writeDreg(0);
        } else {
            plotPixel(R[1] & 0xFF, R[2] & 0xFF, colr);
            R[1]++;
        }
        clearPrefix();
        return;

    // SWAP
    case 0x4D:
        writeDreg(((readSreg() >> 8) & 0xFF) | ((readSreg() & 0xFF) << 8));
        clearPrefix();
        return;

    // COLOR / CMODE
    case 0x4E:
        if (alt1) {
            // CMODE
            por = readSreg() & 0xFF;
        } else {
            // COLOR
            colr = readSreg() & 0xFF;
        }
        clearPrefix();
        return;

    // NOT
    case 0x4F:
        writeDreg(~readSreg());
        clearPrefix();
        return;

    // ADD Rn / ADC Rn / ADD #n / ADC #n
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        uint8_t n = opcode & 0x0F;
        uint16_t operand;
        if (alt2) {
            operand = n; // immediate
        } else {
            operand = R[n];
        }
        uint32_t result;
        if (alt1) {
            // ADC
            result = (uint32_t)readSreg() + (uint32_t)operand + ((sfr & FLAG_CY) ? 1 : 0);
        } else {
            // ADD
            result = (uint32_t)readSreg() + (uint32_t)operand;
        }
        sfr &= ~(FLAG_CY | FLAG_OV);
        if (result > 0xFFFF) sfr |= FLAG_CY;
        if (~(readSreg() ^ operand) & (readSreg() ^ result) & 0x8000) sfr |= FLAG_OV;
        writeDreg((uint16_t)result);
        clearPrefix();
        return;
    }

    // SUB Rn / SBC Rn / SUB #n / CMP Rn
    case 0x60: case 0x61: case 0x62: case 0x63:
    case 0x64: case 0x65: case 0x66: case 0x67:
    case 0x68: case 0x69: case 0x6A: case 0x6B:
    case 0x6C: case 0x6D: case 0x6E: case 0x6F: {
        uint8_t n = opcode & 0x0F;
        uint16_t operand;
        if (alt2) {
            operand = n;
        } else {
            operand = R[n];
        }
        uint32_t result;
        if (alt1 && !alt2) {
            // SBC
            result = (uint32_t)readSreg() - (uint32_t)operand - ((sfr & FLAG_CY) ? 0 : 1);
        } else if (alt1 && alt2) {
            // CMP
            result = (uint32_t)readSreg() - (uint32_t)operand;
            sfr &= ~(FLAG_CY | FLAG_OV);
            if (readSreg() >= operand) sfr |= FLAG_CY;
            if ((readSreg() ^ operand) & (readSreg() ^ result) & 0x8000) sfr |= FLAG_OV;
            setZS((uint16_t)result);
            clearPrefix();
            return;
        } else {
            // SUB
            result = (uint32_t)readSreg() - (uint32_t)operand;
        }
        sfr &= ~(FLAG_CY | FLAG_OV);
        if (readSreg() >= operand) sfr |= FLAG_CY;
        if ((readSreg() ^ operand) & (readSreg() ^ result) & 0x8000) sfr |= FLAG_OV;
        writeDreg((uint16_t)result);
        clearPrefix();
        return;
    }

    // MERGE
    case 0x70:
        writeDreg(((R[7] & 0xFF00)) | ((R[8] >> 8) & 0xFF));
        sfr &= ~(FLAG_CY | FLAG_OV | FLAG_S | FLAG_Z);
        if (R[dst] & 0xF0F0) sfr |= FLAG_CY;
        if (R[dst] & 0xC0C0) sfr |= FLAG_S;
        if (R[dst] & 0x0F0F) sfr |= FLAG_OV;
        if (R[dst] & 0x0303) sfr |= FLAG_Z; // not standard Z behavior
        clearPrefix();
        return;

    // AND Rn / BIC Rn / AND #n / BIC #n
    case 0x71: case 0x72: case 0x73: case 0x74:
    case 0x75: case 0x76: case 0x77: case 0x78:
    case 0x79: case 0x7A: case 0x7B: case 0x7C:
    case 0x7D: case 0x7E: case 0x7F: {
        uint8_t n = opcode & 0x0F;
        uint16_t operand = alt2 ? n : R[n];
        if (alt1) {
            writeDreg(readSreg() & ~operand); // BIC
        } else {
            writeDreg(readSreg() & operand);  // AND
        }
        clearPrefix();
        return;
    }

    // MULT Rn / UMULT Rn / MULT #n / UMULT #n
    case 0x80: case 0x81: case 0x82: case 0x83:
    case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B:
    case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        uint8_t n = opcode & 0x0F;
        uint16_t operand = alt2 ? n : R[n];
        if (alt1) {
            // UMULT
            writeDreg((uint16_t)((readSreg() & 0xFF) * (operand & 0xFF)));
        } else {
            // MULT (signed 8x8)
            int16_t result = (int8_t)(readSreg() & 0xFF) * (int8_t)(operand & 0xFF);
            writeDreg((uint16_t)result);
        }
        clearPrefix();
        return;
    }

    // SBK - store back to last RAM address
    case 0x90: {
        uint16_t val = readSreg();
        gsuWriteRAM(ram_write_addr, val & 0xFF);
        gsuWriteRAM(ram_write_addr ^ 1, (val >> 8) & 0xFF);
        clearPrefix();
        return;
    }

    // LINK #n
    case 0x91: case 0x92: case 0x93: case 0x94:
        R[11] = R[15] + (opcode & 0x0F) - 0x91 + 1;
        clearPrefix();
        return;

    // SEX
    case 0x95:
        writeDreg((readSreg() & 0x80) ? (readSreg() | 0xFF00) : (readSreg() & 0x00FF));
        clearPrefix();
        return;

    // ASR / DIV2
    case 0x96:
        if (alt1) {
            // DIV2
            int16_t val = (int16_t)readSreg();
            int16_t result = val >> 1;
            if ((val & 1) && val < 0) result++;
            writeDreg((uint16_t)result);
        } else {
            sfr &= ~FLAG_CY;
            if (readSreg() & 1) sfr |= FLAG_CY;
            writeDreg((uint16_t)((int16_t)readSreg() >> 1));
        }
        clearPrefix();
        return;

    // ROR
    case 0x97: {
        uint16_t val = readSreg();
        uint16_t carry_in = (sfr & FLAG_CY) ? 0x8000 : 0;
        sfr &= ~FLAG_CY;
        if (val & 1) sfr |= FLAG_CY;
        writeDreg((val >> 1) | carry_in);
        clearPrefix();
        return;
    }

    // JMP Rn / LJMP Rn
    case 0x98: case 0x99: case 0x9A: case 0x9B:
    case 0x9C: case 0x9D: {
        uint8_t n = opcode & 0x0F;
        if (alt1) {
            // LJMP
            pbr = readSreg() & 0x7F;
            R[15] = R[n];
            cbr = R[15] & 0xFFF0;
            cache_valid.fill(false);
        } else {
            R[15] = R[n];
        }
        clearPrefix();
        return;
    }

    // LOB
    case 0x9E:
        writeDreg(readSreg() & 0xFF);
        sfr &= ~FLAG_S;
        if (readSreg() & 0x80) sfr |= FLAG_S; // check bit 7 of low byte
        clearPrefix();
        return;

    // FMULT / LMULT
    case 0x9F:
        if (alt1) {
            // LMULT
            uint32_t result = (uint32_t)((int16_t)readSreg() * (int16_t)R[6]);
            R[4] = result & 0xFFFF;
            writeDreg((result >> 16) & 0xFFFF);
            sfr &= ~FLAG_CY;
            if (result & 0x8000) sfr |= FLAG_CY;
        } else {
            // FMULT
            uint32_t result = (uint32_t)((int16_t)readSreg() * (int16_t)R[6]);
            writeDreg((result >> 16) & 0xFFFF);
            sfr &= ~FLAG_CY;
            if (result & 0x8000) sfr |= FLAG_CY;
        }
        clearPrefix();
        return;

    // IBT Rn, #imm8 / LMS Rn, (imm8) / SMS (imm8), Rn
    case 0xA0: case 0xA1: case 0xA2: case 0xA3:
    case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xA8: case 0xA9: case 0xAA: case 0xAB:
    case 0xAC: case 0xAD: case 0xAE: case 0xAF: {
        uint8_t n = opcode & 0x0F;
        if (alt1) {
            // LMS Rn, (imm8)
            uint8_t imm = readCacheOrROM(R[15]); R[15]++;
            uint16_t addr = (uint16_t)imm << 1;
            R[n] = gsuReadRAM(addr) | ((uint16_t)gsuReadRAM(addr + 1) << 8);
            setZS(R[n]);
        } else if (alt2) {
            // SMS (imm8), Rn
            uint8_t imm = readCacheOrROM(R[15]); R[15]++;
            uint16_t addr = (uint16_t)imm << 1;
            gsuWriteRAM(addr, readSreg() & 0xFF);
            gsuWriteRAM(addr + 1, (readSreg() >> 8) & 0xFF);
        } else {
            // IBT Rn, #imm8
            int8_t imm = (int8_t)readCacheOrROM(R[15]); R[15]++;
            R[n] = (uint16_t)(int16_t)imm;
            setZS(R[n]);
            if (n == 14) {
                uint32_t full = ((uint32_t)rombr << 16) | R[14];
                rom_buffer = gsuReadROM(full);
            }
            if (n == 15) {
                cbr = R[15] & 0xFFF0;
                cache_valid.fill(false);
            }
        }
        clearPrefix();
        return;
    }

    // FROM Rn
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        sreg = opcode & 0x0F;
        sreg_set = true;
        sfr |= FLAG_B;
        return;

    // HIB
    case 0xC0:
        writeDreg((readSreg() >> 8) & 0xFF);
        sfr &= ~FLAG_S;
        if (readSreg() & 0x8000) sfr |= FLAG_S;
        clearPrefix();
        return;

    // OR Rn / XOR Rn / OR #n / XOR #n
    case 0xC1: case 0xC2: case 0xC3: case 0xC4:
    case 0xC5: case 0xC6: case 0xC7: case 0xC8:
    case 0xC9: case 0xCA: case 0xCB: case 0xCC:
    case 0xCD: case 0xCE: case 0xCF: {
        uint8_t n = opcode & 0x0F;
        uint16_t operand = alt2 ? n : R[n];
        if (alt1) {
            writeDreg(readSreg() ^ operand); // XOR
        } else {
            writeDreg(readSreg() | operand); // OR
        }
        clearPrefix();
        return;
    }

    // INC Rn
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:
    case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB:
    case 0xDC: case 0xDD: case 0xDE: {
        uint8_t n = opcode & 0x0F;
        R[n]++;
        setZS(R[n]);
        clearPrefix();
        return;
    }

    // GETC / RAMB / ROMB
    case 0xDF:
        if (alt2) {
            // ROMB
            rombr = readSreg() & 0x7F;
        } else if (alt1) {
            // RAMB
            rambr = readSreg() & 0x01;
        } else {
            // GETC
            colr = rom_buffer;
        }
        clearPrefix();
        return;

    // DEC Rn
    case 0xE0: case 0xE1: case 0xE2: case 0xE3:
    case 0xE4: case 0xE5: case 0xE6: case 0xE7:
    case 0xE8: case 0xE9: case 0xEA: case 0xEB:
    case 0xEC: case 0xED: case 0xEE: {
        uint8_t n = opcode & 0x0F;
        R[n]--;
        setZS(R[n]);
        clearPrefix();
        return;
    }

    // GETB / GETBH / GETBL / GETBS
    case 0xEF:
        if (alt1) {
            // GETBH
            writeDreg((readSreg() & 0x00FF) | ((uint16_t)rom_buffer << 8));
        } else if (alt2) {
            // GETBL
            writeDreg((readSreg() & 0xFF00) | rom_buffer);
        } else {
            // GETB
            writeDreg(rom_buffer);
        }
        clearPrefix();
        return;

    // IWT Rn, #imm16 / LM Rn, (imm16) / SM (imm16), Rn
    case 0xF0: case 0xF1: case 0xF2: case 0xF3:
    case 0xF4: case 0xF5: case 0xF6: case 0xF7:
    case 0xF8: case 0xF9: case 0xFA: case 0xFB:
    case 0xFC: case 0xFD: case 0xFE: case 0xFF: {
        uint8_t n = opcode & 0x0F;
        if (alt1) {
            // LM Rn, (imm16)
            uint8_t lo = readCacheOrROM(R[15]); R[15]++;
            uint8_t hi = readCacheOrROM(R[15]); R[15]++;
            uint16_t addr = lo | ((uint16_t)hi << 8);
            R[n] = gsuReadRAM(addr) | ((uint16_t)gsuReadRAM(addr + 1) << 8);
            setZS(R[n]);
        } else if (alt2) {
            // SM (imm16), Rn
            uint8_t lo = readCacheOrROM(R[15]); R[15]++;
            uint8_t hi = readCacheOrROM(R[15]); R[15]++;
            uint16_t addr = lo | ((uint16_t)hi << 8);
            gsuWriteRAM(addr, R[n] & 0xFF);
            gsuWriteRAM(addr + 1, (R[n] >> 8) & 0xFF);
        } else {
            // IWT Rn, #imm16
            uint8_t lo = readCacheOrROM(R[15]); R[15]++;
            uint8_t hi = readCacheOrROM(R[15]); R[15]++;
            R[n] = lo | ((uint16_t)hi << 8);
            setZS(R[n]);
            if (n == 14) {
                uint32_t full = ((uint32_t)rombr << 16) | R[14];
                rom_buffer = gsuReadROM(full);
            }
            if (n == 15) {
                cbr = R[15] & 0xFFF0;
                cache_valid.fill(false);
            }
        }
        clearPrefix();
        return;
    }

    default:
        clearPrefix();
        return;
    }
}

void SuperFX::flushPixelCache() {
    // pixel cache flushing is handled inline in plotPixel for simplicity
}

// --- Tick ---

void SuperFX::tick(int cpu_cycles) {
    if (!isPresent() || !(sfr & FLAG_GO)) return;

    int speed = (clsr & 0x01) ? 2 : 1;
    cycle_accumulator += cpu_cycles * speed;

    // Run ~1 instruction per SNES cycle (approximation)
    while (cycle_accumulator > 0 && (sfr & FLAG_GO)) {
        executeInstruction();
        cycle_accumulator--;
    }
    if (cycle_accumulator < 0) cycle_accumulator = 0;
}

bool SuperFX::consumeIRQLine() {
    if (!irq_line_pending) return false;
    irq_line_pending = false;
    return true;
}

// --- SNES CPU register interface ---

uint8_t SuperFX::cpuRead(uint16_t address) {
    if (address < kRegisterBase || address > kRegisterEnd) return 0x00;
    uint16_t rel = (uint16_t)(address - kRegisterBase);

    // R0-R15
    if (rel <= 0x1F) {
        uint16_t val = R[(rel >> 1) & 0x0F];
        return (rel & 1) ? highByte(val) : lowByte(val);
    }

    switch (rel) {
    case 0x30: return lowByte(sfr);
    case 0x31: {
        uint8_t v = highByte(sfr);
        sfr &= ~FLAG_IRQ;
        irq_line_pending = false;
        return v;
    }
    case 0x33: return bramr;
    case 0x34: return pbr;
    case 0x36: return rombr;
    case 0x37: return cfgr;
    case 0x38: return scbr;
    case 0x39: return clsr;
    case 0x3A: return scmr;
    case 0x3B: return vcr;
    case 0x3C: return rambr;
    case 0x3E: return lowByte(cbr & 0xFFF0);
    case 0x3F: return highByte(cbr & 0xFFF0);
    default: break;
    }

    if (rel >= 0x0100 && rel <= 0x02FF) {
        return cache[rel - 0x0100];
    }
    return 0x00;
}

void SuperFX::cpuWrite(uint16_t address, uint8_t value) {
    if (address < kRegisterBase || address > kRegisterEnd) return;
    uint16_t rel = (uint16_t)(address - kRegisterBase);

    // R0-R15
    if (rel <= 0x1F) {
        int idx = (rel >> 1) & 0x0F;
        if (rel & 1)
            R[idx] = (uint16_t)((R[idx] & 0x00FF) | ((uint16_t)value << 8));
        else
            R[idx] = (uint16_t)((R[idx] & 0xFF00) | value);
        if (idx == 15 && (rel & 1)) {
            sfr |= FLAG_GO;
            launch_count++;
            last_launch_pc = R[15];
            cbr = R[15] & 0xFFF0;
            cache_valid.fill(false);
        }
        return;
    }

    switch (rel) {
    case 0x30:
    case 0x31: {
        bool was_running = isRunning();
        if (rel & 1)
            sfr = (uint16_t)((sfr & 0x00FF) | ((uint16_t)value << 8));
        else
            sfr = (uint16_t)((sfr & 0xFF00) | value);
        if (!isRunning()) cycle_accumulator = 0;
        return;
    }
    case 0x33: bramr = value; return;
    case 0x34: pbr = value & 0x7F; return;
    case 0x36: rombr = value; rom_buffer = 0; return;
    case 0x37: cfgr = value; return;
    case 0x38: scbr = value; return;
    case 0x39: clsr = value & 0x01; return;
    case 0x3A: scmr = value & 0x3F; return;
    case 0x3C: rambr = value & 0x01; return;
    case 0x3E: cbr = (uint16_t)((cbr & 0xFF00) | value); cbr &= 0xFFF0; return;
    case 0x3F: cbr = (uint16_t)((cbr & 0x00FF) | ((uint16_t)value << 8)); cbr &= 0xFFF0; return;
    default: break;
    }

    if (rel >= 0x0100 && rel <= 0x02FF) {
        cache[rel - 0x0100] = value;
        cache_valid[(rel - 0x0100) >> 4] = true;
        return;
    }
}
