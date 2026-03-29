#include "sa1.hpp"
#include <algorithm>
#include <cstring>
#include <iostream>

// 65816 flag bits
enum : uint8_t {
    FLAG_C = 0x01,
    FLAG_Z = 0x02,
    FLAG_I = 0x04,
    FLAG_D = 0x08,
    FLAG_X = 0x10,  // index 8-bit (native), break (emulation)
    FLAG_M = 0x20,  // accumulator 8-bit (native)
    FLAG_V = 0x40,
    FLAG_N = 0x80,
};

SA1::SA1() {
    bwram.resize(64 * 1024, 0);
    reset();
}

void SA1::reset() {
    sa1cpu = {};
    sa1cpu.P = 0x34;
    sa1cpu.E = true;
    sa1cpu.SP = 0x01FF;
    sa1cpu.halted = true;
    sa1cpu.waiting = false;

    iram.fill(0);

    sa1_control = 0x20; // SA-1 halted at power on
    snes_irq_enable = 0;
    sa1_cpu_control = 0;
    sa1_irq_enable = 0;
    sa1_reset_vector = 0;
    sa1_nmi_vector = 0;
    sa1_irq_vector = 0;
    snes_nmi_vector = 0;
    snes_irq_vector = 0;
    snes_to_sa1_msg = 0;

    rom_bank_c = 0;
    rom_bank_d = 1;
    rom_bank_e = 2;
    rom_bank_f = 3;

    snes_bwram_bank = 0;
    sa1_bwram_bank = 0;
    bwram_write_enable = 0;
    sa1_bwram_write_en = 0;
    bitmap_mode = 0;

    math_a = 0;
    math_b = 0;
    math_control = 0;
    math_result = 0;
    math_overflow = false;

    dma_control = 0;
    dma_src_device = 0;
    dma_src_addr = 0;
    dma_dst_addr = 0;
    dma_length = 0;
    dma_running = false;

    vda = 0;
    vlbit_buffer = 0;
    vlbit_remaining = 0;
    vbd = 0;

    snes_irq_pending = false;
    sa1_irq_from_snes = false;
    sa1_nmi_from_snes = false;
    snes_irq_flags = 0;
    sa1_irq_flags = 0;

    override_snes_nmi = false;
    override_snes_irq = false;

    h_count = 0;
    v_count = 0;

    cycle_accumulator = 0;
}

void SA1::setROM(const uint8_t* data, std::size_t size) {
    rom = data;
    rom_size = size;
}

// --- Flag helpers ---

void SA1::setFlag(uint8_t flag, bool v) {
    if (v) sa1cpu.P |= flag;
    else   sa1cpu.P &= ~flag;
}

bool SA1::getFlag(uint8_t flag) const {
    return (sa1cpu.P & flag) != 0;
}

void SA1::updateNZ8(uint8_t val) {
    setFlag(FLAG_Z, val == 0);
    setFlag(FLAG_N, (val & 0x80) != 0);
}

void SA1::updateNZ16(uint16_t val) {
    setFlag(FLAG_Z, val == 0);
    setFlag(FLAG_N, (val & 0x8000) != 0);
}

// --- ROM address mapping ---

uint32_t SA1::mapROMAddress(uint32_t address) const {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // HiROM banks $C0-$FF
    if (bank >= 0xC0) {
        uint8_t rom_bank_select;
        if (bank <= 0xCF) rom_bank_select = rom_bank_c;
        else if (bank <= 0xDF) rom_bank_select = rom_bank_d;
        else if (bank <= 0xEF) rom_bank_select = rom_bank_e;
        else rom_bank_select = rom_bank_f;
        uint32_t phys = ((uint32_t)(rom_bank_select & 0x07) << 20) | ((uint32_t)(bank & 0x0F) << 16) | offset;
        return rom_size ? (phys % rom_size) : 0;
    }

    // LoROM banks $00-$3F, $80-$BF (offset >= $8000)
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x8000) {
        uint8_t mapped_bank = bank & 0x3F;
        uint8_t rom_bank_select;
        if (mapped_bank <= 0x1F) rom_bank_select = rom_bank_c;
        else rom_bank_select = rom_bank_d;
        uint32_t phys = ((uint32_t)(rom_bank_select & 0x07) << 20) |
                         ((uint32_t)(mapped_bank & 0x1F) << 15) |
                         (uint32_t)(offset & 0x7FFF);
        return rom_size ? (phys % rom_size) : 0;
    }

    // Banks $40-$7F (LoROM mapped to banks E/F)
    if (bank >= 0x40 && bank <= 0x7F && offset >= 0x8000) {
        uint8_t mapped_bank = bank - 0x40;
        uint8_t rom_bank_select = (mapped_bank <= 0x1F) ? rom_bank_e : rom_bank_f;
        uint32_t phys = ((uint32_t)(rom_bank_select & 0x07) << 20) |
                         ((uint32_t)(mapped_bank & 0x1F) << 15) |
                         (uint32_t)(offset & 0x7FFF);
        return rom_size ? (phys % rom_size) : 0;
    }

    return 0;
}

// --- SA-1 CPU memory access ---

uint8_t SA1::sa1Read(uint32_t addr) {
    uint8_t bank = (addr >> 16) & 0xFF;
    uint16_t offset = addr & 0xFFFF;

    // I-RAM at $0000-$07FF in banks $00-$3F, $80-$BF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset <= 0x07FF) {
        return iram[offset];
    }

    // I-RAM at $3000-$37FF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x3000 && offset <= 0x37FF) {
        return iram[offset - 0x3000];
    }

    // SA-1 register reads ($2300-$23FF)
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x2300 && offset <= 0x23FF) {
        return snesRead(offset); // reuse same register interface
    }

    // BW-RAM: $6000-$7FFF with bank select
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
        uint32_t bw_addr = ((uint32_t)(sa1_bwram_bank & 0x1F) << 13) | (offset - 0x6000);
        if (bw_addr < bwram.size()) return bwram[bw_addr];
        return 0;
    }

    // BW-RAM full: $40-$4F
    if (bank >= 0x40 && bank <= 0x4F) {
        uint32_t bw_addr = ((uint32_t)(bank - 0x40) << 16) | offset;
        if (bw_addr < bwram.size()) return bwram[bw_addr];
        return 0;
    }

    // BW-RAM bitmap view: $60-$6F (SA-1 CPU only)
    if (bank >= 0x60 && bank <= 0x6F) {
        uint32_t linear = ((uint32_t)(bank - 0x60) << 16) | offset;
        if (bitmap_mode & 0x04) {
            // 4bpp mode
            uint32_t bw_addr = (linear >> 1) % bwram.size();
            uint8_t shift = (linear & 1) * 4;
            return (bwram[bw_addr] >> shift) & 0x0F;
        } else {
            // 2bpp mode
            uint32_t bw_addr = (linear >> 2) % bwram.size();
            uint8_t shift = (linear & 3) * 2;
            return (bwram[bw_addr] >> shift) & 0x03;
        }
    }

    // ROM access
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x8000) {
        uint32_t phys = mapROMAddress(addr);
        if (rom && phys < rom_size) return rom[phys];
        return 0;
    }
    if (bank >= 0xC0) {
        uint32_t phys = mapROMAddress(addr);
        if (rom && phys < rom_size) return rom[phys];
        return 0;
    }

    return 0;
}

void SA1::sa1Write(uint32_t addr, uint8_t data) {
    uint8_t bank = (addr >> 16) & 0xFF;
    uint16_t offset = addr & 0xFFFF;

    // I-RAM at $0000-$07FF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset <= 0x07FF) {
        iram[offset] = data;
        return;
    }

    // I-RAM at $3000-$37FF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x3000 && offset <= 0x37FF) {
        iram[offset - 0x3000] = data;
        return;
    }

    // SA-1 registers ($2200-$22FF write)
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x2200 && offset <= 0x22FF) {
        snesWrite(offset, data);
        return;
    }

    // BW-RAM: $6000-$7FFF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
        if (sa1_bwram_write_en) {
            uint32_t bw_addr = ((uint32_t)(sa1_bwram_bank & 0x1F) << 13) | (offset - 0x6000);
            if (bw_addr < bwram.size()) bwram[bw_addr] = data;
        }
        return;
    }

    // BW-RAM full: $40-$4F
    if (bank >= 0x40 && bank <= 0x4F) {
        if (sa1_bwram_write_en) {
            uint32_t bw_addr = ((uint32_t)(bank - 0x40) << 16) | offset;
            if (bw_addr < bwram.size()) bwram[bw_addr] = data;
        }
        return;
    }

    // BW-RAM bitmap view: $60-$6F
    if (bank >= 0x60 && bank <= 0x6F) {
        if (!sa1_bwram_write_en) return;
        uint32_t linear = ((uint32_t)(bank - 0x60) << 16) | offset;
        if (bitmap_mode & 0x04) {
            uint32_t bw_addr = (linear >> 1) % bwram.size();
            uint8_t shift = (linear & 1) * 4;
            bwram[bw_addr] = (bwram[bw_addr] & ~(0x0F << shift)) | ((data & 0x0F) << shift);
        } else {
            uint32_t bw_addr = (linear >> 2) % bwram.size();
            uint8_t shift = (linear & 3) * 2;
            bwram[bw_addr] = (bwram[bw_addr] & ~(0x03 << shift)) | ((data & 0x03) << shift);
        }
        return;
    }
}

uint8_t SA1::sa1ReadImm() {
    uint32_t addr = ((uint32_t)sa1cpu.PB << 16) | sa1cpu.PC;
    sa1cpu.PC++;
    return sa1Read(addr);
}

uint16_t SA1::sa1Read16(uint32_t addr) {
    uint8_t lo = sa1Read(addr);
    uint8_t hi = sa1Read(addr + 1);
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void SA1::sa1Write16(uint32_t addr, uint16_t data) {
    sa1Write(addr, data & 0xFF);
    sa1Write(addr + 1, (data >> 8) & 0xFF);
}

void SA1::sa1Push8(uint8_t data) {
    sa1Write(sa1cpu.SP, data);
    if (sa1cpu.E)
        sa1cpu.SP = 0x0100 | ((sa1cpu.SP - 1) & 0xFF);
    else
        sa1cpu.SP--;
}

void SA1::sa1Push16(uint16_t data) {
    sa1Push8((data >> 8) & 0xFF);
    sa1Push8(data & 0xFF);
}

uint8_t SA1::sa1Pop8() {
    if (sa1cpu.E)
        sa1cpu.SP = 0x0100 | ((sa1cpu.SP + 1) & 0xFF);
    else
        sa1cpu.SP++;
    return sa1Read(sa1cpu.SP);
}

uint16_t SA1::sa1Pop16() {
    uint8_t lo = sa1Pop8();
    uint8_t hi = sa1Pop8();
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void SA1::sa1NMI() {
    if (sa1cpu.halted) return;
    sa1cpu.waiting = false;
    sa1Push8(sa1cpu.PB);
    sa1Push16(sa1cpu.PC);
    sa1Push8(sa1cpu.P);
    setFlag(FLAG_I, true);
    setFlag(FLAG_D, false);
    sa1cpu.PB = 0x00;
    sa1cpu.PC = sa1_nmi_vector;
}

void SA1::sa1IRQ() {
    if (sa1cpu.halted || getFlag(FLAG_I)) return;
    sa1cpu.waiting = false;
    sa1Push8(sa1cpu.PB);
    sa1Push16(sa1cpu.PC);
    sa1Push8(sa1cpu.P);
    setFlag(FLAG_I, true);
    setFlag(FLAG_D, false);
    sa1cpu.PB = 0x00;
    sa1cpu.PC = sa1_irq_vector;
}

// --- SA-1 CPU step (one instruction) ---
// Full 65816 instruction execution for the SA-1's internal CPU

void SA1::sa1Step() {
    if (sa1cpu.halted) return;
    if (sa1cpu.waiting) {
        if (sa1cpu.nmi_pending) {
            sa1cpu.nmi_pending = false;
            sa1NMI();
        } else if (sa1cpu.irq_pending && !getFlag(FLAG_I)) {
            sa1cpu.irq_pending = false;
            sa1IRQ();
        }
        return;
    }

    // Check pending interrupts
    if (sa1cpu.nmi_pending) {
        sa1cpu.nmi_pending = false;
        sa1NMI();
        return;
    }
    if (sa1cpu.irq_pending && !getFlag(FLAG_I)) {
        sa1cpu.irq_pending = false;
        sa1IRQ();
        return;
    }

    uint8_t opcode = sa1ReadImm();
    bool m16 = !sa1cpu.E && !(sa1cpu.P & FLAG_M);
    bool x16 = !sa1cpu.E && !(sa1cpu.P & FLAG_X);

    // Helper lambdas for addressing modes
    auto imm8 = [&]() -> uint8_t { return sa1ReadImm(); };
    auto imm16 = [&]() -> uint16_t { uint8_t lo = sa1ReadImm(); return lo | ((uint16_t)sa1ReadImm() << 8); };
    auto immM = [&]() -> uint16_t { return m16 ? imm16() : imm8(); };
    auto immX = [&]() -> uint16_t { return x16 ? imm16() : imm8(); };

    auto abs_ = [&]() -> uint32_t {
        uint16_t lo = sa1ReadImm();
        lo |= (uint16_t)sa1ReadImm() << 8;
        return ((uint32_t)sa1cpu.DB << 16) | lo;
    };
    auto absX = [&]() -> uint32_t {
        uint16_t base = sa1ReadImm();
        base |= (uint16_t)sa1ReadImm() << 8;
        return ((uint32_t)sa1cpu.DB << 16) | (uint16_t)(base + sa1cpu.X);
    };
    auto absY = [&]() -> uint32_t {
        uint16_t base = sa1ReadImm();
        base |= (uint16_t)sa1ReadImm() << 8;
        return ((uint32_t)sa1cpu.DB << 16) | (uint16_t)(base + sa1cpu.Y);
    };
    auto absLong = [&]() -> uint32_t {
        uint8_t lo = sa1ReadImm();
        uint8_t mid = sa1ReadImm();
        uint8_t hi = sa1ReadImm();
        return ((uint32_t)hi << 16) | ((uint32_t)mid << 8) | lo;
    };
    auto absLongX = [&]() -> uint32_t {
        uint32_t base = absLong();
        return base + sa1cpu.X;
    };
    auto dp = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        return (sa1cpu.D + off) & 0xFFFF;
    };
    auto dpX = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        if (sa1cpu.E)
            return (sa1cpu.D & 0xFF00) | ((sa1cpu.D + off + sa1cpu.X) & 0xFF);
        return (sa1cpu.D + off + sa1cpu.X) & 0xFFFF;
    };
    auto dpY = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        if (sa1cpu.E)
            return (sa1cpu.D & 0xFF00) | ((sa1cpu.D + off + sa1cpu.Y) & 0xFF);
        return (sa1cpu.D + off + sa1cpu.Y) & 0xFFFF;
    };
    auto dpInd = [&]() -> uint32_t {
        uint32_t ptr = dp();
        uint16_t addr16 = sa1Read16(ptr);
        return ((uint32_t)sa1cpu.DB << 16) | addr16;
    };
    auto dpIndLong = [&]() -> uint32_t {
        uint32_t ptr = dp();
        uint8_t lo = sa1Read(ptr);
        uint8_t mid = sa1Read((ptr + 1) & 0xFFFF);
        uint8_t hi = sa1Read((ptr + 2) & 0xFFFF);
        return ((uint32_t)hi << 16) | ((uint32_t)mid << 8) | lo;
    };
    auto dpIndX = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        uint32_t ptr;
        if (sa1cpu.E)
            ptr = (sa1cpu.D & 0xFF00) | ((sa1cpu.D + off + sa1cpu.X) & 0xFF);
        else
            ptr = (sa1cpu.D + off + sa1cpu.X) & 0xFFFF;
        uint16_t addr16 = sa1Read16(ptr);
        return ((uint32_t)sa1cpu.DB << 16) | addr16;
    };
    auto dpIndY = [&]() -> uint32_t {
        uint32_t ptr = dp();
        uint16_t base = sa1Read16(ptr);
        return ((uint32_t)sa1cpu.DB << 16) | (uint16_t)(base + sa1cpu.Y);
    };
    auto dpIndYLong = [&]() -> uint32_t {
        uint32_t ptr = dp();
        uint8_t lo = sa1Read(ptr);
        uint8_t mid = sa1Read((ptr + 1) & 0xFFFF);
        uint8_t hi = sa1Read((ptr + 2) & 0xFFFF);
        return ((uint32_t)hi << 16) | ((uint16_t)((lo | ((uint16_t)mid << 8)) + sa1cpu.Y));
    };
    auto sr = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        return (sa1cpu.SP + off) & 0xFFFF;
    };
    auto srIndY = [&]() -> uint32_t {
        uint8_t off = sa1ReadImm();
        uint32_t ptr = (sa1cpu.SP + off) & 0xFFFF;
        uint16_t base = sa1Read16(ptr);
        return ((uint32_t)sa1cpu.DB << 16) | (uint16_t)(base + sa1cpu.Y);
    };

    // Read memory value (8 or 16 bit depending on M flag)
    auto readM = [&](uint32_t addr) -> uint16_t {
        return m16 ? sa1Read16(addr) : sa1Read(addr);
    };
    auto writeM = [&](uint32_t addr, uint16_t val) {
        if (m16) sa1Write16(addr, val);
        else sa1Write(addr, val & 0xFF);
    };
    auto readX = [&](uint32_t addr) -> uint16_t {
        return x16 ? sa1Read16(addr) : sa1Read(addr);
    };
    auto writeX = [&](uint32_t addr, uint16_t val) {
        if (x16) sa1Write16(addr, val);
        else sa1Write(addr, val & 0xFF);
    };

    // NZ update helpers
    auto nzM = [&](uint16_t val) { if (m16) updateNZ16(val); else updateNZ8(val & 0xFF); };
    auto nzX = [&](uint16_t val) { if (x16) updateNZ16(val); else updateNZ8(val & 0xFF); };

    // Branch helper
    auto branch = [&](bool cond) {
        int8_t rel = (int8_t)sa1ReadImm();
        if (cond) sa1cpu.PC = (uint16_t)(sa1cpu.PC + rel);
    };

    // ADC/SBC helpers
    auto doADC = [&](uint16_t operand) {
        bool decimal = getFlag(FLAG_D);
        if (m16) {
            uint32_t result;
            if (decimal) {
                uint32_t al = (sa1cpu.A & 0x000F) + (operand & 0x000F) + (getFlag(FLAG_C) ? 1 : 0);
                if (al > 0x0009) al += 0x0006;
                uint32_t ah = ((sa1cpu.A >> 4) & 0x000F) + ((operand >> 4) & 0x000F) + (al > 0x000F ? 1 : 0);
                al &= 0x000F;
                if (ah > 0x0009) ah += 0x0006;
                uint32_t ahh = ((sa1cpu.A >> 8) & 0x000F) + ((operand >> 8) & 0x000F) + (ah > 0x000F ? 1 : 0);
                ah &= 0x000F;
                if (ahh > 0x0009) ahh += 0x0006;
                uint32_t ahhh = ((sa1cpu.A >> 12) & 0x000F) + ((operand >> 12) & 0x000F) + (ahh > 0x000F ? 1 : 0);
                ahh &= 0x000F;
                if (ahhh > 0x0009) ahhh += 0x0006;
                setFlag(FLAG_C, ahhh > 0x000F);
                result = (al) | (ah << 4) | (ahh << 8) | ((ahhh & 0x0F) << 12);
            } else {
                result = (uint32_t)sa1cpu.A + (uint32_t)operand + (getFlag(FLAG_C) ? 1 : 0);
                setFlag(FLAG_C, result > 0xFFFF);
                setFlag(FLAG_V, (~(sa1cpu.A ^ operand) & (sa1cpu.A ^ result) & 0x8000) != 0);
            }
            sa1cpu.A = (uint16_t)result;
            updateNZ16(sa1cpu.A);
        } else {
            uint8_t a8 = sa1cpu.A & 0xFF;
            uint8_t op8 = operand & 0xFF;
            uint16_t result;
            if (decimal) {
                uint16_t al = (a8 & 0x0F) + (op8 & 0x0F) + (getFlag(FLAG_C) ? 1 : 0);
                if (al > 9) al += 6;
                uint16_t ah = (a8 >> 4) + (op8 >> 4) + (al > 0x0F ? 1 : 0);
                al &= 0x0F;
                if (ah > 9) ah += 6;
                setFlag(FLAG_C, ah > 0x0F);
                result = (al) | ((ah & 0x0F) << 4);
            } else {
                result = (uint16_t)a8 + (uint16_t)op8 + (getFlag(FLAG_C) ? 1 : 0);
                setFlag(FLAG_C, result > 0xFF);
                setFlag(FLAG_V, (~(a8 ^ op8) & (a8 ^ result) & 0x80) != 0);
            }
            sa1cpu.A = (sa1cpu.A & 0xFF00) | (result & 0xFF);
            updateNZ8(result & 0xFF);
        }
    };

    auto doSBC = [&](uint16_t operand) {
        bool decimal = getFlag(FLAG_D);
        if (m16) {
            if (decimal) {
                // BCD subtraction 16-bit (rare)
                uint32_t result = (uint32_t)sa1cpu.A - (uint32_t)operand - (getFlag(FLAG_C) ? 0 : 1);
                setFlag(FLAG_C, result <= 0xFFFF);
                sa1cpu.A = (uint16_t)result;
            } else {
                uint32_t result = (uint32_t)sa1cpu.A - (uint32_t)operand - (getFlag(FLAG_C) ? 0 : 1);
                setFlag(FLAG_V, ((sa1cpu.A ^ operand) & (sa1cpu.A ^ result) & 0x8000) != 0);
                setFlag(FLAG_C, result <= 0xFFFF);
                sa1cpu.A = (uint16_t)result;
            }
            updateNZ16(sa1cpu.A);
        } else {
            uint8_t a8 = sa1cpu.A & 0xFF;
            uint8_t op8 = operand & 0xFF;
            if (decimal) {
                int16_t al = (a8 & 0x0F) - (op8 & 0x0F) - (getFlag(FLAG_C) ? 0 : 1);
                if (al < 0) al = ((al - 6) & 0x0F) | 0xF0;
                int16_t ah = (a8 >> 4) - (op8 >> 4) + (al < 0 ? -1 : 0);
                if (ah < 0) ah = (ah - 6) & 0x0F;
                setFlag(FLAG_C, (uint16_t)a8 >= (uint16_t)op8 + (getFlag(FLAG_C) ? 0 : 1));
                sa1cpu.A = (sa1cpu.A & 0xFF00) | ((al & 0x0F) | ((ah & 0x0F) << 4));
            } else {
                uint16_t result = (uint16_t)a8 - (uint16_t)op8 - (getFlag(FLAG_C) ? 0 : 1);
                setFlag(FLAG_V, ((a8 ^ op8) & (a8 ^ result) & 0x80) != 0);
                setFlag(FLAG_C, result <= 0xFF);
                sa1cpu.A = (sa1cpu.A & 0xFF00) | (result & 0xFF);
            }
            updateNZ8(sa1cpu.A & 0xFF);
        }
    };

    auto doCMP = [&](uint16_t reg, uint16_t operand, bool wide) {
        if (wide) {
            uint32_t result = (uint32_t)reg - (uint32_t)operand;
            setFlag(FLAG_C, reg >= operand);
            updateNZ16((uint16_t)result);
        } else {
            uint16_t r8 = (reg & 0xFF);
            uint16_t o8 = (operand & 0xFF);
            uint16_t result = r8 - o8;
            setFlag(FLAG_C, r8 >= o8);
            updateNZ8(result & 0xFF);
        }
    };

    switch (opcode) {
    // --- LDA ---
    case 0xA9: { uint16_t v = immM(); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xA5: { uint32_t a = dp(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB5: { uint32_t a = dpX(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xAD: { uint32_t a = abs_(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xBD: { uint32_t a = absX(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB9: { uint32_t a = absY(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xAF: { uint32_t a = absLong(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xBF: { uint32_t a = absLongX(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xA1: { uint32_t a = dpIndX(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB1: { uint32_t a = dpIndY(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB2: { uint32_t a = dpInd(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xA7: { uint32_t a = dpIndLong(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB7: { uint32_t a = dpIndYLong(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xA3: { uint32_t a = sr(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }
    case 0xB3: { uint32_t a = srIndY(); uint16_t v = readM(a); sa1cpu.A = m16 ? v : ((sa1cpu.A & 0xFF00) | (v & 0xFF)); nzM(sa1cpu.A); break; }

    // --- LDX ---
    case 0xA2: { uint16_t v = immX(); sa1cpu.X = x16 ? v : (v & 0xFF); nzX(sa1cpu.X); break; }
    case 0xA6: { uint32_t a = dp(); sa1cpu.X = readX(a); nzX(sa1cpu.X); break; }
    case 0xB6: { uint32_t a = dpY(); sa1cpu.X = readX(a); nzX(sa1cpu.X); break; }
    case 0xAE: { uint32_t a = abs_(); sa1cpu.X = readX(a); nzX(sa1cpu.X); break; }
    case 0xBE: { uint32_t a = absY(); sa1cpu.X = readX(a); nzX(sa1cpu.X); break; }

    // --- LDY ---
    case 0xA0: { uint16_t v = immX(); sa1cpu.Y = x16 ? v : (v & 0xFF); nzX(sa1cpu.Y); break; }
    case 0xA4: { uint32_t a = dp(); sa1cpu.Y = readX(a); nzX(sa1cpu.Y); break; }
    case 0xB4: { uint32_t a = dpX(); sa1cpu.Y = readX(a); nzX(sa1cpu.Y); break; }
    case 0xAC: { uint32_t a = abs_(); sa1cpu.Y = readX(a); nzX(sa1cpu.Y); break; }
    case 0xBC: { uint32_t a = absX(); sa1cpu.Y = readX(a); nzX(sa1cpu.Y); break; }

    // --- STA ---
    case 0x85: { uint32_t a = dp(); writeM(a, sa1cpu.A); break; }
    case 0x95: { uint32_t a = dpX(); writeM(a, sa1cpu.A); break; }
    case 0x8D: { uint32_t a = abs_(); writeM(a, sa1cpu.A); break; }
    case 0x9D: { uint32_t a = absX(); writeM(a, sa1cpu.A); break; }
    case 0x99: { uint32_t a = absY(); writeM(a, sa1cpu.A); break; }
    case 0x8F: { uint32_t a = absLong(); writeM(a, sa1cpu.A); break; }
    case 0x9F: { uint32_t a = absLongX(); writeM(a, sa1cpu.A); break; }
    case 0x81: { uint32_t a = dpIndX(); writeM(a, sa1cpu.A); break; }
    case 0x91: { uint32_t a = dpIndY(); writeM(a, sa1cpu.A); break; }
    case 0x92: { uint32_t a = dpInd(); writeM(a, sa1cpu.A); break; }
    case 0x87: { uint32_t a = dpIndLong(); writeM(a, sa1cpu.A); break; }
    case 0x97: { uint32_t a = dpIndYLong(); writeM(a, sa1cpu.A); break; }
    case 0x83: { uint32_t a = sr(); writeM(a, sa1cpu.A); break; }
    case 0x93: { uint32_t a = srIndY(); writeM(a, sa1cpu.A); break; }

    // --- STX ---
    case 0x86: { uint32_t a = dp(); writeX(a, sa1cpu.X); break; }
    case 0x96: { uint32_t a = dpY(); writeX(a, sa1cpu.X); break; }
    case 0x8E: { uint32_t a = abs_(); writeX(a, sa1cpu.X); break; }

    // --- STY ---
    case 0x84: { uint32_t a = dp(); writeX(a, sa1cpu.Y); break; }
    case 0x94: { uint32_t a = dpX(); writeX(a, sa1cpu.Y); break; }
    case 0x8C: { uint32_t a = abs_(); writeX(a, sa1cpu.Y); break; }

    // --- STZ ---
    case 0x64: { uint32_t a = dp(); writeM(a, 0); break; }
    case 0x74: { uint32_t a = dpX(); writeM(a, 0); break; }
    case 0x9C: { uint32_t a = abs_(); writeM(a, 0); break; }
    case 0x9E: { uint32_t a = absX(); writeM(a, 0); break; }

    // --- ADC ---
    case 0x69: doADC(immM()); break;
    case 0x65: doADC(readM(dp())); break;
    case 0x75: doADC(readM(dpX())); break;
    case 0x6D: doADC(readM(abs_())); break;
    case 0x7D: doADC(readM(absX())); break;
    case 0x79: doADC(readM(absY())); break;
    case 0x6F: doADC(readM(absLong())); break;
    case 0x7F: doADC(readM(absLongX())); break;
    case 0x61: doADC(readM(dpIndX())); break;
    case 0x71: doADC(readM(dpIndY())); break;
    case 0x72: doADC(readM(dpInd())); break;
    case 0x67: doADC(readM(dpIndLong())); break;
    case 0x77: doADC(readM(dpIndYLong())); break;
    case 0x63: doADC(readM(sr())); break;
    case 0x73: doADC(readM(srIndY())); break;

    // --- SBC ---
    case 0xE9: doSBC(immM()); break;
    case 0xE5: doSBC(readM(dp())); break;
    case 0xF5: doSBC(readM(dpX())); break;
    case 0xED: doSBC(readM(abs_())); break;
    case 0xFD: doSBC(readM(absX())); break;
    case 0xF9: doSBC(readM(absY())); break;
    case 0xEF: doSBC(readM(absLong())); break;
    case 0xFF: doSBC(readM(absLongX())); break;
    case 0xE1: doSBC(readM(dpIndX())); break;
    case 0xF1: doSBC(readM(dpIndY())); break;
    case 0xF2: doSBC(readM(dpInd())); break;
    case 0xE7: doSBC(readM(dpIndLong())); break;
    case 0xF7: doSBC(readM(dpIndYLong())); break;
    case 0xE3: doSBC(readM(sr())); break;
    case 0xF3: doSBC(readM(srIndY())); break;

    // --- CMP ---
    case 0xC9: doCMP(sa1cpu.A, immM(), m16); break;
    case 0xC5: doCMP(sa1cpu.A, readM(dp()), m16); break;
    case 0xD5: doCMP(sa1cpu.A, readM(dpX()), m16); break;
    case 0xCD: doCMP(sa1cpu.A, readM(abs_()), m16); break;
    case 0xDD: doCMP(sa1cpu.A, readM(absX()), m16); break;
    case 0xD9: doCMP(sa1cpu.A, readM(absY()), m16); break;
    case 0xCF: doCMP(sa1cpu.A, readM(absLong()), m16); break;
    case 0xDF: doCMP(sa1cpu.A, readM(absLongX()), m16); break;
    case 0xC1: doCMP(sa1cpu.A, readM(dpIndX()), m16); break;
    case 0xD1: doCMP(sa1cpu.A, readM(dpIndY()), m16); break;
    case 0xD2: doCMP(sa1cpu.A, readM(dpInd()), m16); break;
    case 0xC7: doCMP(sa1cpu.A, readM(dpIndLong()), m16); break;
    case 0xD7: doCMP(sa1cpu.A, readM(dpIndYLong()), m16); break;
    case 0xC3: doCMP(sa1cpu.A, readM(sr()), m16); break;
    case 0xD3: doCMP(sa1cpu.A, readM(srIndY()), m16); break;

    // --- CPX ---
    case 0xE0: doCMP(sa1cpu.X, immX(), x16); break;
    case 0xE4: doCMP(sa1cpu.X, readX(dp()), x16); break;
    case 0xEC: doCMP(sa1cpu.X, readX(abs_()), x16); break;

    // --- CPY ---
    case 0xC0: doCMP(sa1cpu.Y, immX(), x16); break;
    case 0xC4: doCMP(sa1cpu.Y, readX(dp()), x16); break;
    case 0xCC: doCMP(sa1cpu.Y, readX(abs_()), x16); break;

    // --- AND ---
    case 0x29: { uint16_t v = immM(); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x25: { uint32_t a = dp(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x35: { uint32_t a = dpX(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x2D: { uint32_t a = abs_(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x3D: { uint32_t a = absX(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x39: { uint32_t a = absY(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x2F: { uint32_t a = absLong(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x3F: { uint32_t a = absLongX(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x21: { uint32_t a = dpIndX(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x31: { uint32_t a = dpIndY(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x32: { uint32_t a = dpInd(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x27: { uint32_t a = dpIndLong(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x37: { uint32_t a = dpIndYLong(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x23: { uint32_t a = sr(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x33: { uint32_t a = srIndY(); uint16_t v = readM(a); if (m16) { sa1cpu.A &= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A & v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }

    // --- ORA ---
    case 0x09: { uint16_t v = immM(); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x05: { uint16_t v = readM(dp()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x15: { uint16_t v = readM(dpX()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x0D: { uint16_t v = readM(abs_()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x1D: { uint16_t v = readM(absX()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x19: { uint16_t v = readM(absY()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x0F: { uint16_t v = readM(absLong()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x1F: { uint16_t v = readM(absLongX()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x01: { uint16_t v = readM(dpIndX()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x11: { uint16_t v = readM(dpIndY()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x12: { uint16_t v = readM(dpInd()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x07: { uint16_t v = readM(dpIndLong()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x17: { uint16_t v = readM(dpIndYLong()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x03: { uint16_t v = readM(sr()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x13: { uint16_t v = readM(srIndY()); if (m16) { sa1cpu.A |= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A | v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }

    // --- EOR ---
    case 0x49: { uint16_t v = immM(); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x45: { uint16_t v = readM(dp()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x55: { uint16_t v = readM(dpX()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x4D: { uint16_t v = readM(abs_()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x5D: { uint16_t v = readM(absX()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x59: { uint16_t v = readM(absY()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x4F: { uint16_t v = readM(absLong()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x5F: { uint16_t v = readM(absLongX()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x41: { uint16_t v = readM(dpIndX()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x51: { uint16_t v = readM(dpIndY()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x52: { uint16_t v = readM(dpInd()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x47: { uint16_t v = readM(dpIndLong()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x57: { uint16_t v = readM(dpIndYLong()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x43: { uint16_t v = readM(sr()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }
    case 0x53: { uint16_t v = readM(srIndY()); if (m16) { sa1cpu.A ^= v; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | ((sa1cpu.A ^ v) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break; }

    // --- BIT ---
    case 0x89: { uint16_t v = immM(); if (m16) { setFlag(FLAG_Z, (sa1cpu.A & v) == 0); } else { setFlag(FLAG_Z, ((sa1cpu.A & 0xFF) & (v & 0xFF)) == 0); } break; }
    case 0x24: { uint32_t a = dp(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_N, (v & 0x8000) != 0); setFlag(FLAG_V, (v & 0x4000) != 0); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); } else { setFlag(FLAG_N, (v & 0x80) != 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_Z, ((sa1cpu.A & 0xFF) & v) == 0); } break; }
    case 0x34: { uint32_t a = dpX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_N, (v & 0x8000) != 0); setFlag(FLAG_V, (v & 0x4000) != 0); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); } else { setFlag(FLAG_N, (v & 0x80) != 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_Z, ((sa1cpu.A & 0xFF) & v) == 0); } break; }
    case 0x2C: { uint32_t a = abs_(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_N, (v & 0x8000) != 0); setFlag(FLAG_V, (v & 0x4000) != 0); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); } else { setFlag(FLAG_N, (v & 0x80) != 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_Z, ((sa1cpu.A & 0xFF) & v) == 0); } break; }
    case 0x3C: { uint32_t a = absX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_N, (v & 0x8000) != 0); setFlag(FLAG_V, (v & 0x4000) != 0); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); } else { setFlag(FLAG_N, (v & 0x80) != 0); setFlag(FLAG_V, (v & 0x40) != 0); setFlag(FLAG_Z, ((sa1cpu.A & 0xFF) & v) == 0); } break; }

    // --- TSB / TRB ---
    case 0x04: { uint32_t a = dp(); uint16_t v = readM(a); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); writeM(a, m16 ? (v | sa1cpu.A) : ((v | sa1cpu.A) & 0xFF)); break; }
    case 0x0C: { uint32_t a = abs_(); uint16_t v = readM(a); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); writeM(a, m16 ? (v | sa1cpu.A) : ((v | sa1cpu.A) & 0xFF)); break; }
    case 0x14: { uint32_t a = dp(); uint16_t v = readM(a); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); writeM(a, m16 ? (v & ~sa1cpu.A) : ((v & ~sa1cpu.A) & 0xFF)); break; }
    case 0x1C: { uint32_t a = abs_(); uint16_t v = readM(a); setFlag(FLAG_Z, (sa1cpu.A & v) == 0); writeM(a, m16 ? (v & ~sa1cpu.A) : ((v & ~sa1cpu.A) & 0xFF)); break; }

    // --- INC/DEC memory ---
    case 0xE6: { uint32_t a = dp(); uint16_t v = readM(a); v++; writeM(a, v); nzM(v); break; }
    case 0xF6: { uint32_t a = dpX(); uint16_t v = readM(a); v++; writeM(a, v); nzM(v); break; }
    case 0xEE: { uint32_t a = abs_(); uint16_t v = readM(a); v++; writeM(a, v); nzM(v); break; }
    case 0xFE: { uint32_t a = absX(); uint16_t v = readM(a); v++; writeM(a, v); nzM(v); break; }
    case 0xC6: { uint32_t a = dp(); uint16_t v = readM(a); v--; writeM(a, v); nzM(v); break; }
    case 0xD6: { uint32_t a = dpX(); uint16_t v = readM(a); v--; writeM(a, v); nzM(v); break; }
    case 0xCE: { uint32_t a = abs_(); uint16_t v = readM(a); v--; writeM(a, v); nzM(v); break; }
    case 0xDE: { uint32_t a = absX(); uint16_t v = readM(a); v--; writeM(a, v); nzM(v); break; }

    // INC/DEC A, X, Y
    case 0x1A: if (m16) { sa1cpu.A++; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | (((sa1cpu.A & 0xFF) + 1) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break;
    case 0x3A: if (m16) { sa1cpu.A--; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | (((sa1cpu.A & 0xFF) - 1) & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break;
    case 0xE8: sa1cpu.X = x16 ? (uint16_t)(sa1cpu.X + 1) : ((sa1cpu.X + 1) & 0xFF); nzX(sa1cpu.X); break;
    case 0xC8: sa1cpu.Y = x16 ? (uint16_t)(sa1cpu.Y + 1) : ((sa1cpu.Y + 1) & 0xFF); nzX(sa1cpu.Y); break;
    case 0xCA: sa1cpu.X = x16 ? (uint16_t)(sa1cpu.X - 1) : ((sa1cpu.X - 1) & 0xFF); nzX(sa1cpu.X); break;
    case 0x88: sa1cpu.Y = x16 ? (uint16_t)(sa1cpu.Y - 1) : ((sa1cpu.Y - 1) & 0xFF); nzX(sa1cpu.Y); break;

    // --- ASL/LSR/ROL/ROR accumulator ---
    case 0x0A: if (m16) { setFlag(FLAG_C, (sa1cpu.A & 0x8000) != 0); sa1cpu.A <<= 1; updateNZ16(sa1cpu.A); } else { uint8_t v = sa1cpu.A & 0xFF; setFlag(FLAG_C, (v & 0x80) != 0); v <<= 1; sa1cpu.A = (sa1cpu.A & 0xFF00) | v; updateNZ8(v); } break;
    case 0x4A: if (m16) { setFlag(FLAG_C, sa1cpu.A & 1); sa1cpu.A >>= 1; updateNZ16(sa1cpu.A); } else { uint8_t v = sa1cpu.A & 0xFF; setFlag(FLAG_C, v & 1); v >>= 1; sa1cpu.A = (sa1cpu.A & 0xFF00) | v; updateNZ8(v); } break;
    case 0x2A: if (m16) { uint16_t c = getFlag(FLAG_C) ? 1 : 0; setFlag(FLAG_C, (sa1cpu.A & 0x8000) != 0); sa1cpu.A = (sa1cpu.A << 1) | c; updateNZ16(sa1cpu.A); } else { uint8_t v = sa1cpu.A & 0xFF; uint8_t c = getFlag(FLAG_C) ? 1 : 0; setFlag(FLAG_C, (v & 0x80) != 0); v = (v << 1) | c; sa1cpu.A = (sa1cpu.A & 0xFF00) | v; updateNZ8(v); } break;
    case 0x6A: if (m16) { uint16_t c = getFlag(FLAG_C) ? 0x8000 : 0; setFlag(FLAG_C, sa1cpu.A & 1); sa1cpu.A = (sa1cpu.A >> 1) | c; updateNZ16(sa1cpu.A); } else { uint8_t v = sa1cpu.A & 0xFF; uint8_t c = getFlag(FLAG_C) ? 0x80 : 0; setFlag(FLAG_C, v & 1); v = (v >> 1) | c; sa1cpu.A = (sa1cpu.A & 0xFF00) | v; updateNZ8(v); } break;

    // --- ASL/LSR/ROL/ROR memory ---
    case 0x06: { uint32_t a = dp(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v <<= 1; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = (v << 1) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x16: { uint32_t a = dpX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v <<= 1; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = (v << 1) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x0E: { uint32_t a = abs_(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v <<= 1; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = (v << 1) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x1E: { uint32_t a = absX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v <<= 1; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = (v << 1) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x46: { uint32_t a = dp(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ16(v); } else { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x56: { uint32_t a = dpX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ16(v); } else { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x4E: { uint32_t a = abs_(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ16(v); } else { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x5E: { uint32_t a = absX(); uint16_t v = readM(a); if (m16) { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ16(v); } else { setFlag(FLAG_C, v & 1); v >>= 1; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x26: { uint32_t a = dp(); uint16_t v = readM(a); uint16_t c = getFlag(FLAG_C) ? 1 : 0; if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v = (v << 1) | c; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = ((v << 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x36: { uint32_t a = dpX(); uint16_t v = readM(a); uint16_t c = getFlag(FLAG_C) ? 1 : 0; if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v = (v << 1) | c; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = ((v << 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x2E: { uint32_t a = abs_(); uint16_t v = readM(a); uint16_t c = getFlag(FLAG_C) ? 1 : 0; if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v = (v << 1) | c; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = ((v << 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x3E: { uint32_t a = absX(); uint16_t v = readM(a); uint16_t c = getFlag(FLAG_C) ? 1 : 0; if (m16) { setFlag(FLAG_C, (v & 0x8000) != 0); v = (v << 1) | c; updateNZ16(v); } else { setFlag(FLAG_C, (v & 0x80) != 0); v = ((v << 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x66: { uint32_t a = dp(); uint16_t v = readM(a); if (m16) { uint16_t c = getFlag(FLAG_C) ? 0x8000 : 0; setFlag(FLAG_C, v & 1); v = (v >> 1) | c; updateNZ16(v); } else { uint8_t c = getFlag(FLAG_C) ? 0x80 : 0; setFlag(FLAG_C, v & 1); v = ((v >> 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x76: { uint32_t a = dpX(); uint16_t v = readM(a); if (m16) { uint16_t c = getFlag(FLAG_C) ? 0x8000 : 0; setFlag(FLAG_C, v & 1); v = (v >> 1) | c; updateNZ16(v); } else { uint8_t c = getFlag(FLAG_C) ? 0x80 : 0; setFlag(FLAG_C, v & 1); v = ((v >> 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x6E: { uint32_t a = abs_(); uint16_t v = readM(a); if (m16) { uint16_t c = getFlag(FLAG_C) ? 0x8000 : 0; setFlag(FLAG_C, v & 1); v = (v >> 1) | c; updateNZ16(v); } else { uint8_t c = getFlag(FLAG_C) ? 0x80 : 0; setFlag(FLAG_C, v & 1); v = ((v >> 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }
    case 0x7E: { uint32_t a = absX(); uint16_t v = readM(a); if (m16) { uint16_t c = getFlag(FLAG_C) ? 0x8000 : 0; setFlag(FLAG_C, v & 1); v = (v >> 1) | c; updateNZ16(v); } else { uint8_t c = getFlag(FLAG_C) ? 0x80 : 0; setFlag(FLAG_C, v & 1); v = ((v >> 1) | c) & 0xFF; updateNZ8(v & 0xFF); } writeM(a, v); break; }

    // --- Branches ---
    case 0x80: branch(true); break;
    case 0x82: { int16_t rel = (int16_t)imm16(); sa1cpu.PC = (uint16_t)(sa1cpu.PC + rel); break; } // BRL
    case 0xF0: branch(getFlag(FLAG_Z)); break;   // BEQ
    case 0xD0: branch(!getFlag(FLAG_Z)); break;  // BNE
    case 0x90: branch(!getFlag(FLAG_C)); break;  // BCC
    case 0xB0: branch(getFlag(FLAG_C)); break;   // BCS
    case 0x10: branch(!getFlag(FLAG_N)); break;  // BPL
    case 0x30: branch(getFlag(FLAG_N)); break;   // BMI
    case 0x50: branch(!getFlag(FLAG_V)); break;  // BVC
    case 0x70: branch(getFlag(FLAG_V)); break;   // BVS

    // --- JMP ---
    case 0x4C: { uint16_t addr16 = imm16(); sa1cpu.PC = addr16; break; }
    case 0x5C: { uint32_t a = absLong(); sa1cpu.PB = (a >> 16) & 0xFF; sa1cpu.PC = a & 0xFFFF; break; }
    case 0x6C: { uint16_t ptr = imm16(); sa1cpu.PC = sa1Read16(ptr); break; }
    case 0x7C: { uint16_t ptr = imm16(); uint32_t a = ((uint32_t)sa1cpu.PB << 16) | (uint16_t)(ptr + sa1cpu.X); sa1cpu.PC = sa1Read16(a); break; }
    case 0xDC: { uint16_t ptr = imm16(); uint8_t lo = sa1Read(ptr); uint8_t mid = sa1Read((uint16_t)(ptr + 1)); uint8_t hi = sa1Read((uint16_t)(ptr + 2)); sa1cpu.PC = (uint16_t)lo | ((uint16_t)mid << 8); sa1cpu.PB = hi; break; }

    // --- JSR/JSL ---
    case 0x20: { uint16_t addr16 = imm16(); sa1Push16((uint16_t)(sa1cpu.PC - 1)); sa1cpu.PC = addr16; break; }
    case 0x22: { uint32_t a = absLong(); sa1Push8(sa1cpu.PB); sa1Push16((uint16_t)(sa1cpu.PC - 1)); sa1cpu.PB = (a >> 16) & 0xFF; sa1cpu.PC = a & 0xFFFF; break; }
    case 0xFC: { uint16_t ptr = imm16(); sa1Push16((uint16_t)(sa1cpu.PC - 1)); uint32_t a = ((uint32_t)sa1cpu.PB << 16) | (uint16_t)(ptr + sa1cpu.X); sa1cpu.PC = sa1Read16(a); break; }

    // --- RTS/RTL/RTI ---
    case 0x60: sa1cpu.PC = (uint16_t)(sa1Pop16() + 1); break;
    case 0x6B: sa1cpu.PC = (uint16_t)(sa1Pop16() + 1); sa1cpu.PB = sa1Pop8(); break;
    case 0x40: {
        sa1cpu.P = sa1Pop8();
        sa1cpu.PC = sa1Pop16();
        if (!sa1cpu.E) sa1cpu.PB = sa1Pop8();
        if (sa1cpu.E) { sa1cpu.P |= FLAG_M | FLAG_X; sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; }
        if (sa1cpu.P & FLAG_X) { sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; }
        break;
    }

    // --- Push/Pull ---
    case 0x48: if (m16) sa1Push16(sa1cpu.A); else sa1Push8(sa1cpu.A & 0xFF); break;
    case 0x68: if (m16) { sa1cpu.A = sa1Pop16(); updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | sa1Pop8(); updateNZ8(sa1cpu.A & 0xFF); } break;
    case 0xDA: if (x16) sa1Push16(sa1cpu.X); else sa1Push8(sa1cpu.X & 0xFF); break;
    case 0xFA: if (x16) { sa1cpu.X = sa1Pop16(); updateNZ16(sa1cpu.X); } else { sa1cpu.X = sa1Pop8(); updateNZ8(sa1cpu.X); } break;
    case 0x5A: if (x16) sa1Push16(sa1cpu.Y); else sa1Push8(sa1cpu.Y & 0xFF); break;
    case 0x7A: if (x16) { sa1cpu.Y = sa1Pop16(); updateNZ16(sa1cpu.Y); } else { sa1cpu.Y = sa1Pop8(); updateNZ8(sa1cpu.Y); } break;
    case 0x4B: sa1Push8(sa1cpu.PB); break;       // PHK
    case 0x08: sa1Push8(sa1cpu.P); break;         // PHP
    case 0x28: {                                   // PLP
        sa1cpu.P = sa1Pop8();
        if (sa1cpu.E) { sa1cpu.P |= FLAG_M | FLAG_X; }
        if (sa1cpu.P & FLAG_X) { sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; }
        break;
    }
    case 0x8B: sa1Push8(sa1cpu.DB); break;        // PHB
    case 0xAB: sa1cpu.DB = sa1Pop8(); updateNZ8(sa1cpu.DB); break; // PLB
    case 0x0B: sa1Push16(sa1cpu.D); break;        // PHD
    case 0x2B: sa1cpu.D = sa1Pop16(); updateNZ16(sa1cpu.D); break; // PLD
    case 0xF4: { uint16_t v = imm16(); sa1Push16(v); break; }     // PEA
    case 0xD4: { uint32_t a = dp(); uint16_t v = sa1Read16(a); sa1Push16(v); break; } // PEI
    case 0x62: { int16_t rel = (int16_t)imm16(); sa1Push16((uint16_t)(sa1cpu.PC + rel)); break; } // PER

    // --- Transfers ---
    case 0xAA: sa1cpu.X = x16 ? sa1cpu.A : (sa1cpu.A & 0xFF); nzX(sa1cpu.X); break;
    case 0xA8: sa1cpu.Y = x16 ? sa1cpu.A : (sa1cpu.A & 0xFF); nzX(sa1cpu.Y); break;
    case 0x8A: if (m16) { sa1cpu.A = sa1cpu.X; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | (sa1cpu.X & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break;
    case 0x98: if (m16) { sa1cpu.A = sa1cpu.Y; updateNZ16(sa1cpu.A); } else { sa1cpu.A = (sa1cpu.A & 0xFF00) | (sa1cpu.Y & 0xFF); updateNZ8(sa1cpu.A & 0xFF); } break;
    case 0x9A: sa1cpu.SP = sa1cpu.E ? (0x0100 | (sa1cpu.X & 0xFF)) : sa1cpu.X; break;
    case 0xBA: sa1cpu.X = x16 ? sa1cpu.SP : (sa1cpu.SP & 0xFF); nzX(sa1cpu.X); break;
    case 0x9B: sa1cpu.Y = x16 ? sa1cpu.X : (sa1cpu.X & 0xFF); nzX(sa1cpu.Y); break;
    case 0xBB: sa1cpu.X = x16 ? sa1cpu.Y : (sa1cpu.Y & 0xFF); nzX(sa1cpu.X); break;
    case 0x5B: sa1cpu.D = sa1cpu.A; updateNZ16(sa1cpu.D); break;
    case 0x7B: sa1cpu.A = sa1cpu.D; updateNZ16(sa1cpu.A); break;
    case 0x1B: sa1cpu.SP = sa1cpu.A; if (sa1cpu.E) sa1cpu.SP = 0x0100 | (sa1cpu.SP & 0xFF); break;
    case 0x3B: sa1cpu.A = sa1cpu.SP; updateNZ16(sa1cpu.A); break;
    case 0xEB: { uint8_t lo = sa1cpu.A & 0xFF; uint8_t hi = (sa1cpu.A >> 8) & 0xFF; sa1cpu.A = ((uint16_t)lo << 8) | hi; updateNZ8(sa1cpu.A & 0xFF); break; } // XBA

    // --- Flag operations ---
    case 0x18: setFlag(FLAG_C, false); break;
    case 0x38: setFlag(FLAG_C, true); break;
    case 0x58: setFlag(FLAG_I, false); break;
    case 0x78: setFlag(FLAG_I, true); break;
    case 0xB8: setFlag(FLAG_V, false); break;
    case 0xF8: setFlag(FLAG_D, true); break;
    case 0xD8: setFlag(FLAG_D, false); break;

    case 0xC2: { // REP
        uint8_t mask = sa1ReadImm();
        sa1cpu.P &= ~mask;
        if (sa1cpu.E) sa1cpu.P |= FLAG_M | FLAG_X;
        if (sa1cpu.P & FLAG_X) { sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; }
        break;
    }
    case 0xE2: { // SEP
        uint8_t mask = sa1ReadImm();
        sa1cpu.P |= mask;
        if (sa1cpu.P & FLAG_X) { sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; }
        break;
    }
    case 0xFB: { // XCE
        bool old_c = getFlag(FLAG_C);
        setFlag(FLAG_C, sa1cpu.E);
        sa1cpu.E = old_c;
        if (sa1cpu.E) { sa1cpu.P |= FLAG_M | FLAG_X; sa1cpu.X &= 0xFF; sa1cpu.Y &= 0xFF; sa1cpu.SP = 0x0100 | (sa1cpu.SP & 0xFF); }
        break;
    }

    // --- NOP/BRK/STP/WAI ---
    case 0xEA: break;
    case 0x00: { // BRK
        sa1cpu.PC++;
        sa1Push8(sa1cpu.PB);
        sa1Push16(sa1cpu.PC);
        sa1Push8(sa1cpu.P);
        setFlag(FLAG_I, true);
        setFlag(FLAG_D, false);
        sa1cpu.PB = 0x00;
        sa1cpu.PC = sa1Read16(sa1cpu.E ? 0x00FFFE : 0x00FFE6);
        break;
    }
    case 0xDB: sa1cpu.halted = true; break; // STP
    case 0xCB: sa1cpu.waiting = true; break; // WAI
    case 0x02: sa1cpu.PC++; break; // COP - skip signature byte

    // --- MVN/MVP ---
    case 0x54: { // MVN
        uint8_t dst_bank = sa1ReadImm();
        uint8_t src_bank = sa1ReadImm();
        sa1cpu.DB = dst_bank;
        uint32_t src = ((uint32_t)src_bank << 16) | sa1cpu.X;
        uint32_t dst = ((uint32_t)dst_bank << 16) | sa1cpu.Y;
        sa1Write(dst, sa1Read(src));
        sa1cpu.X = x16 ? (uint16_t)(sa1cpu.X + 1) : ((sa1cpu.X + 1) & 0xFF);
        sa1cpu.Y = x16 ? (uint16_t)(sa1cpu.Y + 1) : ((sa1cpu.Y + 1) & 0xFF);
        sa1cpu.A--;
        if (sa1cpu.A != 0xFFFF) sa1cpu.PC -= 3;
        break;
    }
    case 0x44: { // MVP
        uint8_t dst_bank = sa1ReadImm();
        uint8_t src_bank = sa1ReadImm();
        sa1cpu.DB = dst_bank;
        uint32_t src = ((uint32_t)src_bank << 16) | sa1cpu.X;
        uint32_t dst = ((uint32_t)dst_bank << 16) | sa1cpu.Y;
        sa1Write(dst, sa1Read(src));
        sa1cpu.X = x16 ? (uint16_t)(sa1cpu.X - 1) : ((sa1cpu.X - 1) & 0xFF);
        sa1cpu.Y = x16 ? (uint16_t)(sa1cpu.Y - 1) : ((sa1cpu.Y - 1) & 0xFF);
        sa1cpu.A--;
        if (sa1cpu.A != 0xFFFF) sa1cpu.PC -= 3;
        break;
    }

    default:
        break;
    }
}

// --- Tick: run SA-1 CPU cycles proportional to SNES CPU cycles ---

void SA1::tick(int snes_cpu_cycles) {
    if (sa1cpu.halted) return;

    // SA-1 runs at ~3x SNES CPU speed
    cycle_accumulator += snes_cpu_cycles * 3;

    int steps = cycle_accumulator;
    cycle_accumulator = 0;

    for (int i = 0; i < steps && !sa1cpu.halted; i++) {
        sa1Step();
    }
}

// --- SNES CPU register interface ($2200-$23FF) ---

uint8_t SA1::snesRead(uint16_t address) {
    switch (address) {
    case 0x2300: // SFR - SA-1 status flags (read by SNES)
        return snes_irq_flags;
    case 0x2301: // CFR - SA-1 CPU flag register
        return sa1_irq_flags |
               (sa1_irq_from_snes ? 0x80 : 0) |
               (sa1_nmi_from_snes ? 0x10 : 0);
    case 0x2302: return (uint8_t)(h_count & 0xFF);
    case 0x2303: return (uint8_t)((h_count >> 8) & 0x01);
    case 0x2304: return (uint8_t)(v_count & 0xFF);
    case 0x2305: return (uint8_t)((v_count >> 8) & 0x01);
    case 0x2306: return (uint8_t)(math_result & 0xFF);
    case 0x2307: return (uint8_t)((math_result >> 8) & 0xFF);
    case 0x2308: return (uint8_t)((math_result >> 16) & 0xFF);
    case 0x2309: return (uint8_t)((math_result >> 24) & 0xFF);
    case 0x230A: return (uint8_t)((math_result >> 32) & 0xFF);
    case 0x230B: return math_overflow ? 0x80 : 0x00;
    case 0x230C: return (uint8_t)(vlbit_buffer & 0xFF);
    case 0x230D: return (uint8_t)((vlbit_buffer >> 8) & 0xFF);
    case 0x230E: return 0x01; // version
    default:
        return 0;
    }
}

void SA1::snesWrite(uint16_t address, uint8_t data) {
    switch (address) {
    case 0x2200: { // CCNT - SA-1 CPU control
        sa1_control = data;
        snes_to_sa1_msg = data & 0x0F;

        // Bit 5: SA-1 halt/resume
        if (data & 0x20) {
            // Request SA-1 resume
            if (sa1cpu.halted) {
                sa1cpu.halted = false;
                sa1cpu.E = true;
                sa1cpu.P = 0x34;
                sa1cpu.SP = 0x01FF;
                sa1cpu.PB = 0x00;
                sa1cpu.DB = 0x00;
                sa1cpu.D = 0x0000;
                sa1cpu.PC = sa1_reset_vector;
            }
        }
        if (data & 0x80) {
            // Reset SA-1
            sa1cpu.halted = false;
            sa1cpu.E = true;
            sa1cpu.P = 0x34;
            sa1cpu.SP = 0x01FF;
            sa1cpu.PB = 0x00;
            sa1cpu.DB = 0x00;
            sa1cpu.D = 0x0000;
            sa1cpu.PC = sa1_reset_vector;
        }
        if (!(data & 0x20) && !(data & 0x80)) {
            // Halt SA-1
            sa1cpu.halted = true;
        }

        // Bit 4: SA-1 NMI
        if (data & 0x10) {
            sa1_nmi_from_snes = true;
            if (sa1_irq_enable & 0x10) {
                sa1cpu.nmi_pending = true;
            }
        }

        // Bit 6: SA-1 IRQ
        if (data & 0x40) {
            sa1_irq_from_snes = true;
            if (sa1_irq_enable & 0x80) {
                sa1cpu.irq_pending = true;
            }
        }
        break;
    }
    case 0x2201: // SIE - SNES IRQ enable
        snes_irq_enable = data;
        break;
    case 0x2202: // SIC - SNES IRQ clear
        if (data & 0x80) { snes_irq_flags &= ~0x80; snes_irq_pending = false; }
        if (data & 0x20) { snes_irq_flags &= ~0x20; }
        if (data & 0x10) { snes_irq_flags &= ~0x10; }
        break;
    case 0x2203: sa1_reset_vector = (sa1_reset_vector & 0xFF00) | data; break;
    case 0x2204: sa1_reset_vector = (sa1_reset_vector & 0x00FF) | ((uint16_t)data << 8); break;
    case 0x2205: sa1_nmi_vector = (sa1_nmi_vector & 0xFF00) | data; break;
    case 0x2206: sa1_nmi_vector = (sa1_nmi_vector & 0x00FF) | ((uint16_t)data << 8); break;
    case 0x2207: sa1_irq_vector = (sa1_irq_vector & 0xFF00) | data; break;
    case 0x2208: sa1_irq_vector = (sa1_irq_vector & 0x00FF) | ((uint16_t)data << 8); break;

    case 0x2209: // SCNT - SA-1 CPU control (written by SA-1 CPU)
        sa1_cpu_control = data;
        override_snes_nmi = (data & 0x10) != 0;
        override_snes_irq = (data & 0x40) != 0;
        // Bit 7: IRQ to SNES CPU
        if (data & 0x80) {
            snes_irq_flags |= 0x80;
            if (snes_irq_enable & 0x80) {
                snes_irq_pending = true;
            }
        }
        break;
    case 0x220A: // CIE - SA-1 IRQ enable
        sa1_irq_enable = data;
        break;
    case 0x220B: // CIC - SA-1 IRQ clear
        if (data & 0x80) { sa1_irq_from_snes = false; sa1cpu.irq_pending = false; }
        if (data & 0x10) { sa1_nmi_from_snes = false; sa1cpu.nmi_pending = false; }
        break;
    case 0x220C: snes_nmi_vector = (snes_nmi_vector & 0xFF00) | data; break;
    case 0x220D: snes_nmi_vector = (snes_nmi_vector & 0x00FF) | ((uint16_t)data << 8); break;
    case 0x220E: snes_irq_vector = (snes_irq_vector & 0xFF00) | data; break;
    case 0x220F: snes_irq_vector = (snes_irq_vector & 0x00FF) | ((uint16_t)data << 8); break;

    case 0x2210: // Timer control (not used by most games)
        break;

    case 0x2220: rom_bank_c = data & 0x07; break;
    case 0x2221: rom_bank_d = data & 0x07; break;
    case 0x2222: rom_bank_e = data & 0x07; break;
    case 0x2223: rom_bank_f = data & 0x07; break;

    case 0x2224: snes_bwram_bank = data & 0x1F; break;
    case 0x2225: sa1_bwram_bank = data & 0x7F; break;
    case 0x2226: bwram_write_enable = data & 0x80 ? 1 : 0; break;
    case 0x2228: sa1_bwram_write_en = data & 0x80 ? 1 : 0; break;

    case 0x2229: // SNES I-RAM write protect (ignored for now)
        break;
    case 0x222A: // SA-1 I-RAM write protect (ignored for now)
        break;

    case 0x2230: // DCNT - DMA control
        dma_control = data;
        break;
    case 0x2231: // CDMA - character DMA / source device
        dma_src_device = data;
        break;
    case 0x2232: dma_src_addr = (dma_src_addr & 0xFFFF00) | data; break;
    case 0x2233: dma_src_addr = (dma_src_addr & 0xFF00FF) | ((uint32_t)data << 8); break;
    case 0x2234: dma_src_addr = (dma_src_addr & 0x00FFFF) | ((uint32_t)data << 16); break;
    case 0x2235: dma_dst_addr = (dma_dst_addr & 0xFFFF00) | data; break;
    case 0x2236:
        dma_dst_addr = (dma_dst_addr & 0xFF00FF) | ((uint32_t)data << 8);
        // Writing high byte of dest triggers normal DMA
        if ((dma_control & 0x80) == 0 && (dma_control & 0x20)) {
            runDMA();
        }
        break;
    case 0x2237:
        dma_dst_addr = (dma_dst_addr & 0x00FFFF) | ((uint32_t)data << 16);
        if ((dma_control & 0x80) == 0 && !(dma_control & 0x20)) {
            runDMA();
        }
        break;
    case 0x2238: dma_length = (dma_length & 0xFF00) | data; break;
    case 0x2239: dma_length = (dma_length & 0x00FF) | ((uint16_t)data << 8); break;

    case 0x223F: // BBFLAG - BW-RAM bitmap format
        bitmap_mode = data;
        break;

    // Arithmetic registers
    case 0x2250: math_control = data & 0x03; if (math_control == 2) math_result = 0; break;
    case 0x2251: math_a = (math_a & 0xFF00) | data; break;
    case 0x2252: math_a = (math_a & 0x00FF) | ((uint16_t)data << 8); break;
    case 0x2253: math_b = (math_b & 0xFF00) | data; break;
    case 0x2254:
        math_b = (math_b & 0x00FF) | ((uint16_t)data << 8);
        runArithmetic();
        break;

    // Variable-length bitstream
    case 0x2258: // VBD
        vbd = data;
        if (data & 0x80) {
            // Auto mode - shift on read (not commonly used)
        } else {
            vlBitstreamShift();
        }
        break;
    case 0x2259: vda = (vda & 0xFFFF00) | data; break;
    case 0x225A: vda = (vda & 0xFF00FF) | ((uint32_t)data << 8); break;
    case 0x225B:
        vda = (vda & 0x00FFFF) | ((uint32_t)data << 16);
        vlBitstreamStart();
        break;

    default:
        break;
    }
}

// --- Cartridge-level read/write (for SNES CPU accessing SA-1 regions) ---

uint8_t SA1::cartRead(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // I-RAM $3000-$37FF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x3000 && offset <= 0x37FF) {
        return iram[offset - 0x3000];
    }

    // SA-1 registers
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x2200 && offset <= 0x23FF) {
        return snesRead(offset);
    }

    // BW-RAM $6000-$7FFF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
        uint32_t bw_addr = ((uint32_t)(snes_bwram_bank & 0x1F) << 13) | (offset - 0x6000);
        if (bw_addr < bwram.size()) return bwram[bw_addr];
        return 0;
    }

    // BW-RAM $40-$4F
    if (bank >= 0x40 && bank <= 0x4F) {
        uint32_t bw_addr = ((uint32_t)(bank - 0x40) << 16) | offset;
        if (bw_addr < bwram.size()) return bwram[bw_addr];
        return 0;
    }

    // ROM via SA-1 mapping
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x8000) {
        uint32_t phys = mapROMAddress(address);
        if (rom && phys < rom_size) return rom[phys];
        return 0;
    }
    if (bank >= 0xC0) {
        uint32_t phys = mapROMAddress(address);
        if (rom && phys < rom_size) return rom[phys];
        return 0;
    }

    return 0;
}

void SA1::cartWrite(uint32_t address, uint8_t data) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // I-RAM $3000-$37FF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x3000 && offset <= 0x37FF) {
        iram[offset - 0x3000] = data;
        return;
    }

    // SA-1 registers
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x2200 && offset <= 0x23FF) {
        snesWrite(offset, data);
        return;
    }

    // BW-RAM $6000-$7FFF
    if (((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
        if (bwram_write_enable) {
            uint32_t bw_addr = ((uint32_t)(snes_bwram_bank & 0x1F) << 13) | (offset - 0x6000);
            if (bw_addr < bwram.size()) bwram[bw_addr] = data;
        }
        return;
    }

    // BW-RAM $40-$4F
    if (bank >= 0x40 && bank <= 0x4F) {
        if (bwram_write_enable) {
            uint32_t bw_addr = ((uint32_t)(bank - 0x40) << 16) | offset;
            if (bw_addr < bwram.size()) bwram[bw_addr] = data;
        }
        return;
    }
}

bool SA1::consumeSNESIRQ() {
    if (!snes_irq_pending) return false;
    snes_irq_pending = false;
    return true;
}

uint8_t SA1::readIRAM(uint16_t offset) const {
    if (offset < iram.size()) return iram[offset];
    return 0;
}

void SA1::writeIRAM(uint16_t offset, uint8_t data) {
    if (offset < iram.size()) iram[offset] = data;
}

uint8_t SA1::readBWRAM(uint32_t offset) const {
    if (offset < bwram.size()) return bwram[offset];
    return 0;
}

void SA1::writeBWRAM(uint32_t offset, uint8_t data) {
    if (offset < bwram.size()) bwram[offset] = data;
}

// --- Arithmetic unit ---

void SA1::runArithmetic() {
    int16_t sa = (int16_t)math_a;
    int16_t sb = (int16_t)math_b;

    switch (math_control & 0x03) {
    case 0: // Multiply
        math_result = (int64_t)sa * (int64_t)sb;
        math_overflow = false;
        break;
    case 1: { // Divide
        uint16_t divisor = math_b;
        if (divisor == 0) {
            math_result = 0;
            math_overflow = true;
        } else {
            int16_t quotient = sa / (int16_t)divisor;
            uint16_t remainder = (uint16_t)(sa % (int16_t)divisor);
            math_result = ((int64_t)(uint16_t)quotient) | ((int64_t)remainder << 16);
            math_overflow = false;
        }
        break;
    }
    case 2: // Multiply-accumulate
        math_result += (int64_t)sa * (int64_t)sb;
        // Clamp to 40-bit signed
        if (math_result > 0x7FFFFFFFFFLL)
            math_result = 0x7FFFFFFFFFLL;
        if (math_result < -0x8000000000LL)
            math_result = -0x8000000000LL;
        math_overflow = false;
        break;
    default:
        break;
    }
}

// --- DMA ---

void SA1::runDMA() {
    if (dma_length == 0) return;

    bool src_is_rom = (dma_control & 0x03) == 0;
    bool dst_is_iram = (dma_control & 0x04) == 0;

    for (uint16_t i = 0; i < dma_length; i++) {
        uint32_t src = dma_src_addr + i;
        uint32_t dst = dma_dst_addr + i;

        uint8_t byte;
        if (src_is_rom) {
            uint32_t phys = mapROMAddress(src);
            byte = (rom && phys < rom_size) ? rom[phys] : 0;
        } else {
            // BW-RAM or I-RAM source
            if ((src & 0xFF0000) >= 0x400000 && (src & 0xFF0000) <= 0x4F0000) {
                uint32_t bw = ((uint32_t)((src >> 16) - 0x40) << 16) | (src & 0xFFFF);
                byte = (bw < bwram.size()) ? bwram[bw] : 0;
            } else {
                uint16_t off = src & 0xFFFF;
                if (off < 0x0800) byte = iram[off];
                else if (off >= 0x3000 && off <= 0x37FF) byte = iram[off - 0x3000];
                else byte = 0;
            }
        }

        if (dst_is_iram) {
            uint16_t off = dst & 0x07FF;
            iram[off] = byte;
        } else {
            // BW-RAM destination
            if ((dst & 0xFF0000) >= 0x400000 && (dst & 0xFF0000) <= 0x4F0000) {
                uint32_t bw = ((uint32_t)((dst >> 16) - 0x40) << 16) | (dst & 0xFFFF);
                if (bw < bwram.size()) bwram[bw] = byte;
            } else {
                uint32_t bw = ((uint32_t)(sa1_bwram_bank & 0x1F) << 13) | ((dst & 0xFFFF) - 0x6000);
                if (bw < bwram.size()) bwram[bw] = byte;
            }
        }
    }
    dma_length = 0;
}

// --- Variable-length bitstream ---

void SA1::vlBitstreamStart() {
    uint32_t rom_addr = mapROMAddress(vda);
    if (!rom || rom_addr + 1 >= rom_size) {
        vlbit_buffer = 0;
        vlbit_remaining = 16;
        return;
    }
    uint16_t first_word = rom[rom_addr] | ((uint16_t)rom[rom_addr + 1] << 8);
    vlbit_buffer = first_word;
    vlbit_remaining = 16;
    vda += 2;
}

void SA1::vlBitstreamShift() {
    uint8_t shift = vbd & 0x0F;
    if (shift == 0) shift = 16;

    vlbit_buffer >>= shift;
    vlbit_remaining -= shift;

    if (vlbit_remaining < 16) {
        uint32_t rom_addr = mapROMAddress(vda);
        if (rom && rom_addr + 1 < rom_size) {
            uint16_t next = rom[rom_addr] | ((uint16_t)rom[rom_addr + 1] << 8);
            vlbit_buffer |= ((uint32_t)next << vlbit_remaining);
            vlbit_remaining += 16;
        }
        vda += 2;
    }
}
