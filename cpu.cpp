#include "cpu.hpp"
#include "bus.hpp"
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
//  Construction / Reset
// ─────────────────────────────────────────────────────────────────────────────

CPU::CPU() : bus(nullptr), instruction_count(0), max_trace_lines(500000), trace_enabled(false) {
    A = X = Y = D = 0;
    SP = 0x01FF; PC = 0; PB = 0; DB = 0; P = 0x34; E = true;
    fetched_opcode = 0; absolute_address = 0; effective_address = 0;
    fetched_data = 0; cycles_remaining = 0;
    buildInstructionTable();
}

CPU::~CPU() { if (log_file.is_open()) log_file.close(); }

void CPU::connectBus(Bus* b) { bus = b; }

void CPU::enableTrace(const std::string& path, uint64_t max_lines) {
    log_file.open(path);
    max_trace_lines = max_lines;
    trace_enabled = log_file.is_open();
    if (trace_enabled) {
        log_file << "# Super Furamicom CPU Trace\n";
        log_file << "# Columns: count | PB:PC | Op | Mnemonic | A | X | Y | SP | P | D | DB | E\n";
    }
}

void CPU::reset() {
    PB = 0x00; DB = 0x00; D = 0x0000;
    SP = 0x01FF; P = 0x34; E = true;
    halted = false;
    waiting_for_interrupt = false;
    last_opcode = 0;
    history_pos = 0;
    history_filled = false;
    pc_history.fill(0);
    a_history.fill(0);
    x_history.fill(0);
    y_history.fill(0);
    PC = read16(0x00FFFC);
    cycles_remaining = 8;

    if (trace_enabled) {
        char buf[128];
        snprintf(buf, sizeof(buf), "# RESET -> PC=$%04X\n", PC);
        log_file << buf;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Execution
// ─────────────────────────────────────────────────────────────────────────────

uint8_t CPU::step() {
    if (halted) return 1;
    if (waiting_for_interrupt) return 1;

    uint32_t pc_full = ((uint32_t)PB << 16) | PC;
    fetched_opcode = read8(pc_full);
    last_opcode = fetched_opcode;
    pc_history[history_pos] = pc_full;
    a_history[history_pos] = A;
    x_history[history_pos] = X;
    y_history[history_pos] = Y;
    history_pos = (history_pos + 1) % kDebugHistory;
    history_filled = history_filled || history_pos == 0;

    // ── Trace log ────────────────────────────────────────────────────────────
    if (trace_enabled && instruction_count < max_trace_lines) {
        const char* mnem = instruction_table[fetched_opcode].name;
        char buf[256];
        snprintf(buf, sizeof(buf),
            "%lu | %02X:%04X | %02X | %-4s | A:%04X X:%04X Y:%04X SP:%04X P:%02X D:%04X DB:%02X E:%d",
            (unsigned long)instruction_count,
            PB, PC, fetched_opcode, mnem ? mnem : "???",
            A, X, Y, SP, P, D, DB, (int)E);
        log_file << buf << "\n";
        
        if (instruction_count == max_trace_lines - 1) {
            log_file << "# TRACE LIMIT REACHED (" << max_trace_lines << " instructions)\n";
            log_file.flush();
        }
    }
    instruction_count++;

    PC++;

    Instruction& instr = instruction_table[fetched_opcode];
    cycles_remaining = instr.cycles;

    if (instr.addrmode) (this->*instr.addrmode)();
    if (instr.operate)  (this->*instr.operate)();

    return cycles_remaining;
}

void CPU::printState() {}

void CPU::nmi() {
    if (halted) return;
    waiting_for_interrupt = false;
    push8(PB);
    push16(PC);
    push8(P);
    setFlag(0x04, true);
    setFlag(0x08, false);
    PB = 0x00;
    PC = read16(0x00FFEA);
    cycles_remaining += 8;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Helpers
// ─────────────────────────────────────────────────────────────────────────────

void     CPU::setFlag(uint8_t f, bool v)   { if (v) P |= f; else P &= ~f; }
bool     CPU::getFlag(uint8_t f)           { return (P & f) != 0; }

uint8_t  CPU::read8 (uint32_t a)           { return bus ? bus->read(a) : 0; }
uint16_t CPU::read16(uint32_t a)           { return (uint16_t)read8(a) | ((uint16_t)read8(a + 1) << 8); }
void     CPU::write8 (uint32_t a, uint8_t  d) { if (bus) bus->write(a, d); }
void     CPU::write16(uint32_t a, uint16_t d) { write8(a, d & 0xFF); write8(a + 1, d >> 8); }

void    CPU::push8 (uint8_t  d) { write8(SP, d); SP--; if (E && (SP & 0xFF) == 0xFF) SP = (SP & 0xFF00) | 0xFF; /* wrap handled below */ if (E) SP = 0x0100 | (SP & 0xFF); }
void    CPU::push16(uint16_t d) { push8(d >> 8); push8(d & 0xFF); }
uint8_t CPU::pop8 ()            { SP++; if (E) SP = 0x0100 | (SP & 0xFF); return read8(SP); }
uint16_t CPU::pop16()           { uint8_t lo = pop8(); uint8_t hi = pop8(); return (uint16_t)lo | ((uint16_t)hi << 8); }

// ─────────────────────────────────────────────────────────────────────────────
//  Addressing Modes
// ─────────────────────────────────────────────────────────────────────────────

void CPU::IMP()  {}
void CPU::ACC()  {}

void CPU::IMM8()  { effective_address = ((uint32_t)PB << 16) | PC; PC += 1; }
void CPU::IMM16() { effective_address = ((uint32_t)PB << 16) | PC; PC += 2; }

void CPU::IMM_M() { effective_address = ((uint32_t)PB << 16) | PC; PC += getFlag(0x20) ? 1 : 2; }
void CPU::IMM_X() { effective_address = ((uint32_t)PB << 16) | PC; PC += getFlag(0x10) ? 1 : 2; }

void CPU::ABS() {
    uint16_t addr = read16(((uint32_t)PB << 16) | PC); PC += 2;
    effective_address = ((uint32_t)DB << 16) | addr;
}
void CPU::ABSX() {
    uint16_t base = read16(((uint32_t)PB << 16) | PC); PC += 2;
    effective_address = ((uint32_t)DB << 16) | (uint16_t)(base + X);
}
void CPU::ABSY() {
    uint16_t base = read16(((uint32_t)PB << 16) | PC); PC += 2;
    effective_address = ((uint32_t)DB << 16) | (uint16_t)(base + Y);
}

void CPU::AL() {
    uint16_t lo = read16(((uint32_t)PB << 16) | PC); PC += 2;
    uint8_t  hi = read8 (((uint32_t)PB << 16) | PC); PC++;
    effective_address = ((uint32_t)hi << 16) | lo;
}
void CPU::ALX() {
    uint16_t lo = read16(((uint32_t)PB << 16) | PC); PC += 2;
    uint8_t  hi = read8 (((uint32_t)PB << 16) | PC); PC++;
    effective_address = (((uint32_t)hi << 16) | lo) + X;
}

void CPU::DP()  { effective_address = (D + read8(((uint32_t)PB << 16) | PC++)) & 0xFFFF; }
void CPU::DPX() { effective_address = (D + read8(((uint32_t)PB << 16) | PC++) + X) & 0xFFFF; }
void CPU::DPY() { effective_address = (D + read8(((uint32_t)PB << 16) | PC++) + Y) & 0xFFFF; }

void CPU::DP_IND() {
    uint8_t  ptr = read8(((uint32_t)PB << 16) | PC++);
    uint16_t addr = read16((D + ptr) & 0xFFFF);
    effective_address = ((uint32_t)DB << 16) | addr;
}
void CPU::DP_IND_X() {
    uint8_t  ptr = read8(((uint32_t)PB << 16) | PC++);
    uint16_t idx_ptr = (D + ptr + X) & 0xFFFF;
    uint16_t addr = read16(idx_ptr);
    effective_address = ((uint32_t)DB << 16) | addr;
}
void CPU::DP_IND_Y() {
    uint8_t  ptr  = read8(((uint32_t)PB << 16) | PC++);
    uint16_t base = read16((D + ptr) & 0xFFFF);
    effective_address = ((uint32_t)DB << 16) | (uint16_t)(base + Y);
}
void CPU::DP_IND_Y_LONG() {
    uint8_t  ptr  = read8(((uint32_t)PB << 16) | PC++);
    uint16_t lo   = read16((D + ptr) & 0xFFFF);
    uint8_t  bank = read8 ((D + ptr + 2) & 0xFFFF);
    effective_address = ((((uint32_t)bank << 16) | lo) + Y) & 0x00FFFFFF;
}

void CPU::SR() {
    uint8_t offset = read8(((uint32_t)PB << 16) | PC++);
    effective_address = (SP + offset) & 0xFFFF;
}
void CPU::SR_IND_Y() {
    uint8_t  offset = read8(((uint32_t)PB << 16) | PC++);
    uint16_t ptr    = (SP + offset) & 0xFFFF;
    uint16_t base   = read16(ptr);
    effective_address = ((uint32_t)DB << 16) | (uint16_t)(base + Y);
}

void CPU::REL()   { effective_address = ((uint32_t)PB << 16) | PC; PC += 1; }
void CPU::REL16() { effective_address = ((uint32_t)PB << 16) | PC; PC += 2; }

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Load / Store
// ─────────────────────────────────────────────────────────────────────────────

void CPU::LDA() {
    if (getFlag(0x20)) {
        uint8_t v = read8(effective_address);
        A = (A & 0xFF00) | v;
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        A = read16(effective_address);
        setFlag(0x02, A == 0); setFlag(0x80, (A & 0x8000) != 0);
        cycles_remaining++;
    }
}

void CPU::LDX() {
    if (getFlag(0x10)) {
        X = read8(effective_address);
        setFlag(0x02, X == 0); setFlag(0x80, (X & 0x80) != 0);
    } else {
        X = read16(effective_address);
        setFlag(0x02, X == 0); setFlag(0x80, (X & 0x8000) != 0);
        cycles_remaining++;
    }
}

void CPU::LDY() {
    if (getFlag(0x10)) {
        Y = read8(effective_address);
        setFlag(0x02, Y == 0); setFlag(0x80, (Y & 0x80) != 0);
    } else {
        Y = read16(effective_address);
        setFlag(0x02, Y == 0); setFlag(0x80, (Y & 0x8000) != 0);
        cycles_remaining++;
    }
}

void CPU::STA() {
    if (getFlag(0x20)) write8 (effective_address, A & 0xFF);
    else               write16(effective_address, A);
}
void CPU::STX() {
    if (getFlag(0x10)) write8 (effective_address, X & 0xFF);
    else               write16(effective_address, X);
}
void CPU::STY() {
    if (getFlag(0x10)) write8 (effective_address, Y & 0xFF);
    else               write16(effective_address, Y);
}
void CPU::STZ() {
    if (getFlag(0x20)) write8 (effective_address, 0);
    else               write16(effective_address, 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Arithmetic
// ─────────────────────────────────────────────────────────────────────────────

void CPU::ADC() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint32_t result;
    if (getFlag(0x20)) {
        uint8_t a8 = A & 0xFF;
        result = a8 + (val & 0xFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFF);
        setFlag(0x40, ~(a8 ^ val) & (a8 ^ result) & 0x80);
        A = (A & 0xFF00) | (result & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0); setFlag(0x80, (A & 0x80) != 0);
    } else {
        result = A + val + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFFFF);
        setFlag(0x40, ~(A ^ val) & (A ^ result) & 0x8000);
        A = result & 0xFFFF;
        setFlag(0x02, A == 0); setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::SBC() {
    uint16_t raw = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint16_t val = ~raw;
    uint32_t result;
    if (getFlag(0x20)) {
        uint8_t a8 = A & 0xFF;
        result = a8 + (val & 0xFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFF);
        setFlag(0x40, ~(a8 ^ (val & 0xFF)) & (a8 ^ result) & 0x80);
        A = (A & 0xFF00) | (result & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0); setFlag(0x80, (A & 0x80) != 0);
    } else {
        result = A + (val & 0xFFFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFFFF);
        setFlag(0x40, ~(A ^ (val & 0xFFFF)) & (A ^ result) & 0x8000);
        A = result & 0xFFFF;
        setFlag(0x02, A == 0); setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::INC() {
    if (getFlag(0x20)) {
        uint8_t v = (read8(effective_address) + 1) & 0xFF;
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        uint16_t v = (read16(effective_address) + 1) & 0xFFFF;
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x8000) != 0);
    }
}

void CPU::DEC() {
    if (getFlag(0x20)) {
        uint8_t v = (read8(effective_address) - 1) & 0xFF;
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        uint16_t v = (read16(effective_address) - 1) & 0xFFFF;
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x8000) != 0);
    }
}

void CPU::INA() {
    A = getFlag(0x20) ? (A & 0xFF00) | ((A + 1) & 0xFF) : (A + 1) & 0xFFFF;
    uint16_t v = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, v == 0); setFlag(0x80, (v & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}

void CPU::DEA() {
    A = getFlag(0x20) ? (A & 0xFF00) | ((A - 1) & 0xFF) : (A - 1) & 0xFFFF;
    uint16_t v = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, v == 0); setFlag(0x80, (v & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}

void CPU::INX() {
    X = getFlag(0x10) ? (X + 1) & 0xFF : (X + 1) & 0xFFFF;
    setFlag(0x02, X == 0); setFlag(0x80, (X & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}
void CPU::INY() {
    Y = getFlag(0x10) ? (Y + 1) & 0xFF : (Y + 1) & 0xFFFF;
    setFlag(0x02, Y == 0); setFlag(0x80, (Y & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}
void CPU::DEX() {
    X = getFlag(0x10) ? (X - 1) & 0xFF : (X - 1) & 0xFFFF;
    setFlag(0x02, X == 0); setFlag(0x80, (X & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}
void CPU::DEY() {
    Y = getFlag(0x10) ? (Y - 1) & 0xFF : (Y - 1) & 0xFFFF;
    setFlag(0x02, Y == 0); setFlag(0x80, (Y & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Logic
// ─────────────────────────────────────────────────────────────────────────────

void CPU::AND() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    A = getFlag(0x20) ? (A & 0xFF00) | ((A & 0xFF) & val) : A & val;
    uint16_t r = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, (r & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}

void CPU::ORA() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    A = getFlag(0x20) ? (A & 0xFF00) | ((A & 0xFF) | val) : A | val;
    uint16_t r = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, (r & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}

void CPU::EOR() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    A = getFlag(0x20) ? (A & 0xFF00) | ((A & 0xFF) ^ val) : A ^ val;
    uint16_t r = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, (r & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}

void CPU::BIT() {
    uint16_t val  = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint16_t mask = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, (val & mask) == 0);
    setFlag(0x80, (val & (getFlag(0x20) ? 0x80   : 0x8000)) != 0);
    setFlag(0x40, (val & (getFlag(0x20) ? 0x40   : 0x4000)) != 0);
}

void CPU::TSB() {
    if (getFlag(0x20)) {
        uint8_t data = read8(effective_address);
        setFlag(0x02, (data & (A & 0xFF)) == 0);
        write8(effective_address, data | (A & 0xFF));
    } else {
        uint16_t data = read16(effective_address);
        setFlag(0x02, (data & A) == 0);
        write16(effective_address, data | A);
    }
}

void CPU::TRB() {
    if (getFlag(0x20)) {
        uint8_t data = read8(effective_address);
        setFlag(0x02, (data & (A & 0xFF)) == 0);
        write8(effective_address, data & ~(A & 0xFF));
    } else {
        uint16_t data = read16(effective_address);
        setFlag(0x02, (data & A) == 0);
        write16(effective_address, data & ~A);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Shifts & Rotates
// ─────────────────────────────────────────────────────────────────────────────

void CPU::ASL_A() {
    bool m8 = getFlag(0x20);
    uint16_t msb = m8 ? 0x80 : 0x8000;
    setFlag(0x01, (A & msb) != 0);
    A = m8 ? (A & 0xFF00) | ((A << 1) & 0xFF) : (A << 1) & 0xFFFF;
    uint16_t r = m8 ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, (r & msb) != 0);
}
void CPU::LSR_A() {
    bool m8 = getFlag(0x20);
    setFlag(0x01, (A & 0x01) != 0);
    A = m8 ? (A & 0xFF00) | ((A & 0xFF) >> 1) : A >> 1;
    uint16_t r = m8 ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, false);
}

void CPU::ROL_A() {
    bool m8   = getFlag(0x20);
    bool carry = getFlag(0x01);
    uint16_t msb = m8 ? 0x80 : 0x8000;
    setFlag(0x01, (A & msb) != 0);
    if (m8) A = (A & 0xFF00) | (((A << 1) | (carry ? 1 : 0)) & 0xFF);
    else    A = ((A << 1) | (carry ? 1 : 0)) & 0xFFFF;
    uint16_t r = m8 ? A & 0xFF : A;
    setFlag(0x02, r == 0); setFlag(0x80, (r & msb) != 0);
}

void CPU::ROR_A() {
    bool m8   = getFlag(0x20);
    bool carry = getFlag(0x01);
    setFlag(0x01, (A & 0x01) != 0);
    if (m8) A = (A & 0xFF00) | (((A & 0xFF) >> 1) | (carry ? 0x80 : 0));
    else    A = (A >> 1) | (carry ? 0x8000 : 0);
    uint16_t r = m8 ? A & 0xFF : A;
    uint16_t msb = m8 ? 0x80 : 0x8000;
    setFlag(0x02, r == 0); setFlag(0x80, (r & msb) != 0);
}

void CPU::ASL_M() {
    bool m8 = getFlag(0x20);
    if (m8) {
        uint8_t v = read8(effective_address);
        setFlag(0x01, (v & 0x80) != 0);
        v = (v << 1) & 0xFF;
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        uint16_t v = read16(effective_address);
        setFlag(0x01, (v & 0x8000) != 0);
        v = (v << 1) & 0xFFFF;
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x8000) != 0);
    }
}

void CPU::LSR_M() {
    bool m8 = getFlag(0x20);
    if (m8) {
        uint8_t v = read8(effective_address);
        setFlag(0x01, (v & 0x01) != 0);
        v >>= 1;
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, false);
    } else {
        uint16_t v = read16(effective_address);
        setFlag(0x01, (v & 0x01) != 0);
        v >>= 1;
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, false);
    }
}

void CPU::ROL_M() {
    bool carry = getFlag(0x01);
    bool m8    = getFlag(0x20);
    if (m8) {
        uint8_t v = read8(effective_address);
        setFlag(0x01, (v & 0x80) != 0);
        v = ((v << 1) | (carry ? 1 : 0)) & 0xFF;
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        uint16_t v = read16(effective_address);
        setFlag(0x01, (v & 0x8000) != 0);
        v = ((v << 1) | (carry ? 1 : 0)) & 0xFFFF;
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x8000) != 0);
    }
}

void CPU::ROR_M() {
    bool carry = getFlag(0x01);
    bool m8    = getFlag(0x20);
    if (m8) {
        uint8_t v = read8(effective_address);
        setFlag(0x01, (v & 0x01) != 0);
        v = (v >> 1) | (carry ? 0x80 : 0);
        write8(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x80) != 0);
    } else {
        uint16_t v = read16(effective_address);
        setFlag(0x01, (v & 0x01) != 0);
        v = (v >> 1) | (carry ? 0x8000 : 0);
        write16(effective_address, v);
        setFlag(0x02, v == 0); setFlag(0x80, (v & 0x8000) != 0);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Compare
// ─────────────────────────────────────────────────────────────────────────────

void CPU::CMP() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x20) ? A & 0xFF : A;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val);
    setFlag(0x02, (res & (getFlag(0x20) ? 0xFF : 0xFFFF)) == 0);
    setFlag(0x80, (res & (getFlag(0x20) ? 0x80 : 0x8000)) != 0);
}
void CPU::CPX() {
    uint16_t val = getFlag(0x10) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x10) ? X & 0xFF : X;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val);
    setFlag(0x02, (res & (getFlag(0x10) ? 0xFF : 0xFFFF)) == 0);
    setFlag(0x80, (res & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}
void CPU::CPY() {
    uint16_t val = getFlag(0x10) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x10) ? Y & 0xFF : Y;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val);
    setFlag(0x02, (res & (getFlag(0x10) ? 0xFF : 0xFFFF)) == 0);
    setFlag(0x80, (res & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Branches
// ─────────────────────────────────────────────────────────────────────────────

static inline uint16_t branch8(uint16_t PC, uint8_t rel) {
    return PC + (int8_t)rel;
}

void CPU::BRA() { PC = branch8(PC, read8(effective_address)); cycles_remaining++; }
void CPU::BRL() { PC += (int16_t)read16(effective_address); }
void CPU::BEQ() { if ( getFlag(0x02)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BNE() { if (!getFlag(0x02)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BCC() { if (!getFlag(0x01)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BCS() { if ( getFlag(0x01)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BPL() { if (!getFlag(0x80)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BMI() { if ( getFlag(0x80)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BVC() { if (!getFlag(0x40)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }
void CPU::BVS() { if ( getFlag(0x40)) { PC = branch8(PC, read8(effective_address)); cycles_remaining++; } }

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Jumps & Calls
// ─────────────────────────────────────────────────────────────────────────────

void CPU::JMP()     { PC = effective_address & 0xFFFF; }
void CPU::JML()     { PB = (effective_address >> 16) & 0xFF; PC = effective_address & 0xFFFF; }

void CPU::JMP_IND() {
    uint16_t ptr = read16(((uint32_t)PB << 16) | PC); PC += 2;
    PC = read16(ptr);
}
void CPU::JMP_IND_X() {
    uint16_t base = read16(((uint32_t)PB << 16) | PC); PC += 2;
    uint16_t ptr  = base + X;
    PC = read16(((uint32_t)PB << 16) | ptr);
}
void CPU::JML_IND() {
    uint16_t ptr  = read16(((uint32_t)PB << 16) | PC); PC += 2;
    uint32_t dest = read16(ptr) | ((uint32_t)read8(ptr + 2) << 16);
    PC = dest & 0xFFFF; PB = (dest >> 16) & 0xFF;
}

void CPU::JSR() {
    push16(PC - 1);
    PC = effective_address & 0xFFFF;
}
void CPU::JSL() {
    push8(PB); push16(PC - 1);
    PB = (effective_address >> 16) & 0xFF;
    PC = effective_address & 0xFFFF;
}
void CPU::JSR_IND_X() {
    uint16_t base = read16(((uint32_t)PB << 16) | PC); PC += 2;
    push16(PC - 1);
    uint16_t ptr = base + X;
    PC = read16(((uint32_t)PB << 16) | ptr);
}

void CPU::RTS() { PC = pop16() + 1; }
void CPU::RTL() { PC = pop16() + 1; PB = pop8(); }
void CPU::RTI() {
    P  = pop8();
    PC = pop16();
    if (!E) PB = pop8();
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Stack
// ─────────────────────────────────────────────────────────────────────────────

void CPU::PHA() { if (getFlag(0x20)) push8(A & 0xFF); else push16(A); }
void CPU::PLA() {
    if (getFlag(0x20)) { A = (A & 0xFF00) | pop8();  setFlag(0x02, (A&0xFF)==0);  setFlag(0x80, (A&0x80)!=0); }
    else               { A = pop16();                 setFlag(0x02, A==0);          setFlag(0x80, (A&0x8000)!=0); }
}
void CPU::PHX() { if (getFlag(0x10)) push8(X & 0xFF); else push16(X); }
void CPU::PLX() {
    if (getFlag(0x10)) { X = pop8();   setFlag(0x02, (X&0xFF)==0); setFlag(0x80, (X&0x80)!=0); }
    else               { X = pop16();  setFlag(0x02, X==0);         setFlag(0x80, (X&0x8000)!=0); }
}
void CPU::PHY() { if (getFlag(0x10)) push8(Y & 0xFF); else push16(Y); }
void CPU::PLY() {
    if (getFlag(0x10)) { Y = pop8();   setFlag(0x02, (Y&0xFF)==0); setFlag(0x80, (Y&0x80)!=0); }
    else               { Y = pop16();  setFlag(0x02, Y==0);         setFlag(0x80, (Y&0x8000)!=0); }
}

void CPU::PHK() { push8(PB); }
void CPU::PHP() { push8(P); }
void CPU::PLP() { P = pop8(); if (E) P |= 0x30; }
void CPU::PHB() { push8(DB); }
void CPU::PLB() { DB = pop8(); setFlag(0x02, DB==0); setFlag(0x80, (DB&0x80)!=0); }
void CPU::PHD() { push16(D); }
void CPU::PLD() { D = pop16(); setFlag(0x02, D==0); setFlag(0x80, (D&0x8000)!=0); }

void CPU::PEA() { push16(read16(effective_address)); }
void CPU::PEI() { push16(read16(effective_address)); }
void CPU::PER() { push16(PC + (int16_t)read16(effective_address)); }

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Transfers
// ─────────────────────────────────────────────────────────────────────────────

void CPU::TAX() {
    X = getFlag(0x10) ? A & 0xFF : A;
    setFlag(0x02, X==0); setFlag(0x80, (X & (getFlag(0x10)?0x80:0x8000)) != 0);
}
void CPU::TAY() {
    Y = getFlag(0x10) ? A & 0xFF : A;
    setFlag(0x02, Y==0); setFlag(0x80, (Y & (getFlag(0x10)?0x80:0x8000)) != 0);
}
void CPU::TXA() {
    A = getFlag(0x20) ? (A & 0xFF00) | (X & 0xFF) : X;
    uint16_t r = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, r==0); setFlag(0x80, (r & (getFlag(0x20)?0x80:0x8000)) != 0);
}
void CPU::TYA() {
    A = getFlag(0x20) ? (A & 0xFF00) | (Y & 0xFF) : Y;
    uint16_t r = getFlag(0x20) ? A & 0xFF : A;
    setFlag(0x02, r==0); setFlag(0x80, (r & (getFlag(0x20)?0x80:0x8000)) != 0);
}

void CPU::TXS() { SP = E ? (0x0100 | (X & 0xFF)) : X; }
void CPU::TSX() {
    X = getFlag(0x10) ? SP & 0xFF : SP;
    setFlag(0x02, X==0); setFlag(0x80, (X & (getFlag(0x10)?0x80:0x8000)) != 0);
}

void CPU::TXY() {
    Y = getFlag(0x10) ? X & 0xFF : X;
    setFlag(0x02, Y==0); setFlag(0x80, (Y & (getFlag(0x10)?0x80:0x8000)) != 0);
}
void CPU::TYX() {
    X = getFlag(0x10) ? Y & 0xFF : Y;
    setFlag(0x02, X==0); setFlag(0x80, (X & (getFlag(0x10)?0x80:0x8000)) != 0);
}

void CPU::TCD() { D = A; setFlag(0x02, D==0); setFlag(0x80, (D & 0x8000) != 0); }
void CPU::TDC() { A = D; setFlag(0x02, A==0); setFlag(0x80, (A & 0x8000) != 0); }

void CPU::TCS() { SP = E ? (0x0100 | (A & 0xFF)) : A; }
void CPU::TSC() { A = SP; setFlag(0x02, A==0); setFlag(0x80, (A & 0x8000) != 0); }

void CPU::XBA() {
    uint8_t newLo = (A >> 8) & 0xFF;
    uint8_t newHi = A & 0xFF;
    A = ((uint16_t)newHi << 8) | newLo;
    setFlag(0x02, newLo == 0);
    setFlag(0x80, (newLo & 0x80) != 0);
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Processor Control
// ─────────────────────────────────────────────────────────────────────────────

void CPU::SEI() { setFlag(0x04, true); }
void CPU::CLC() { setFlag(0x01, false); }
void CPU::SEC() { setFlag(0x01, true); }
void CPU::CLI() { setFlag(0x04, false); }
void CPU::CLV() { setFlag(0x40, false); }
void CPU::SED() { setFlag(0x08, true); }
void CPU::CLD() { setFlag(0x08, false); }

void CPU::REP() { P &= ~read8(effective_address); if (E) P |= 0x30; }
void CPU::SEP() { P |=  read8(effective_address); if (E) P |= 0x30; }

void CPU::XCE() {
    bool oldCarry = getFlag(0x01);
    setFlag(0x01, E);
    E = oldCarry;
    if (E) {
        setFlag(0x20, true);
        setFlag(0x10, true);
        SP = 0x0100 | (SP & 0xFF);
    }
}

void CPU::NOP() {}

void CPU::BRK() {
    PC++;  // BRK has a signature byte
    if (!E) push8(PB);
    push16(PC);
    push8(P);
    setFlag(0x04, true);
    setFlag(0x08, false);
    PB = 0x00;
    if (E)  PC = read16(0x00FFFE);  // Emulation BRK vector
    else    PC = read16(0x00FFE6);  // Native BRK vector
}

void CPU::STP() {
    halted = true;
    if (trace_enabled) {
        log_file << "# CPU STOPPED (STP instruction)\n";
        log_file.flush();
    }
}

void CPU::WAI() {
    waiting_for_interrupt = true;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Opcodes — Block Move
// ─────────────────────────────────────────────────────────────────────────────

void CPU::MVN() {
    uint8_t dest_bank = read8(((uint32_t)PB << 16) | PC++);
    uint8_t src_bank  = read8(((uint32_t)PB << 16) | PC++);
    DB = dest_bank;
    write8(((uint32_t)dest_bank << 16) | Y, read8(((uint32_t)src_bank << 16) | X));
    X++; Y++; A--;
    if (A != 0xFFFF) PC -= 3;
    cycles_remaining = 7;
}

void CPU::MVP() {
    uint8_t dest_bank = read8(((uint32_t)PB << 16) | PC++);
    uint8_t src_bank  = read8(((uint32_t)PB << 16) | PC++);
    DB = dest_bank;
    write8(((uint32_t)dest_bank << 16) | Y, read8(((uint32_t)src_bank << 16) | X));
    X--; Y--; A--;
    if (A != 0xFFFF) PC -= 3;
    cycles_remaining = 7;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Instruction Table
// ─────────────────────────────────────────────────────────────────────────────

void CPU::buildInstructionTable() {
    for (int i = 0; i < 256; i++)
        instruction_table[i] = { &CPU::NOP, &CPU::IMP, 2, "NOP" };

    // ── BRK / COP / WDM / STP / WAI ─────────────────────────────────────────
    instruction_table[0x00] = { &CPU::BRK, &CPU::IMP,  7, "BRK" };
    instruction_table[0x02] = { &CPU::NOP, &CPU::IMM8, 2, "COP" };  // COP stub
    instruction_table[0x42] = { &CPU::NOP, &CPU::IMM8, 2, "WDM" };  // WDM stub
    instruction_table[0xCB] = { &CPU::WAI, &CPU::IMP,  3, "WAI" };
    instruction_table[0xDB] = { &CPU::STP, &CPU::IMP,  3, "STP" };

    // ── BIT ──────────────────────────────────────────────────────────────────
    instruction_table[0x24] = { &CPU::BIT,  &CPU::DP,    3, "BIT" };
    instruction_table[0x2C] = { &CPU::BIT,  &CPU::ABS,   4, "BIT" };
    instruction_table[0x34] = { &CPU::BIT,  &CPU::DPX,   4, "BIT" };
    instruction_table[0x3C] = { &CPU::BIT,  &CPU::ABSX,  4, "BIT" };
    instruction_table[0x89] = { &CPU::BIT,  &CPU::IMM_M, 2, "BIT" };

    // ── TSB / TRB ────────────────────────────────────────────────────────────
    instruction_table[0x04] = { &CPU::TSB,  &CPU::DP,    5, "TSB" };
    instruction_table[0x0C] = { &CPU::TSB,  &CPU::ABS,   6, "TSB" };
    instruction_table[0x14] = { &CPU::TRB,  &CPU::DP,    5, "TRB" };
    instruction_table[0x1C] = { &CPU::TRB,  &CPU::ABS,   6, "TRB" };

    // ── ORA ──────────────────────────────────────────────────────────────────
    instruction_table[0x01] = { &CPU::ORA,  &CPU::DP_IND_X,      6, "ORA" };
    instruction_table[0x03] = { &CPU::ORA,  &CPU::SR,             4, "ORA" };
    instruction_table[0x05] = { &CPU::ORA,  &CPU::DP,             3, "ORA" };
    instruction_table[0x07] = { &CPU::ORA,  &CPU::DP_IND_Y_LONG, 6, "ORA" };
    instruction_table[0x09] = { &CPU::ORA,  &CPU::IMM_M,          2, "ORA" };
    instruction_table[0x0D] = { &CPU::ORA,  &CPU::ABS,            4, "ORA" };
    instruction_table[0x0F] = { &CPU::ORA,  &CPU::AL,             5, "ORA" };
    instruction_table[0x11] = { &CPU::ORA,  &CPU::DP_IND_Y,       5, "ORA" };
    instruction_table[0x12] = { &CPU::ORA,  &CPU::DP_IND,         5, "ORA" };
    instruction_table[0x13] = { &CPU::ORA,  &CPU::SR_IND_Y,       7, "ORA" };
    instruction_table[0x15] = { &CPU::ORA,  &CPU::DPX,            4, "ORA" };
    instruction_table[0x17] = { &CPU::ORA,  &CPU::DP_IND_Y_LONG, 6, "ORA" };
    instruction_table[0x19] = { &CPU::ORA,  &CPU::ABSY,           4, "ORA" };
    instruction_table[0x1D] = { &CPU::ORA,  &CPU::ABSX,           4, "ORA" };
    instruction_table[0x1F] = { &CPU::ORA,  &CPU::ALX,            5, "ORA" };

    // ── AND ──────────────────────────────────────────────────────────────────
    instruction_table[0x21] = { &CPU::AND,  &CPU::DP_IND_X,      6, "AND" };
    instruction_table[0x23] = { &CPU::AND,  &CPU::SR,             4, "AND" };
    instruction_table[0x25] = { &CPU::AND,  &CPU::DP,             3, "AND" };
    instruction_table[0x27] = { &CPU::AND,  &CPU::DP_IND_Y_LONG, 6, "AND" };
    instruction_table[0x29] = { &CPU::AND,  &CPU::IMM_M,          2, "AND" };
    instruction_table[0x2D] = { &CPU::AND,  &CPU::ABS,            4, "AND" };
    instruction_table[0x2F] = { &CPU::AND,  &CPU::AL,             5, "AND" };
    instruction_table[0x31] = { &CPU::AND,  &CPU::DP_IND_Y,       5, "AND" };
    instruction_table[0x32] = { &CPU::AND,  &CPU::DP_IND,         5, "AND" };
    instruction_table[0x33] = { &CPU::AND,  &CPU::SR_IND_Y,       7, "AND" };
    instruction_table[0x35] = { &CPU::AND,  &CPU::DPX,            4, "AND" };
    instruction_table[0x37] = { &CPU::AND,  &CPU::DP_IND_Y_LONG, 6, "AND" };
    instruction_table[0x39] = { &CPU::AND,  &CPU::ABSY,           4, "AND" };
    instruction_table[0x3D] = { &CPU::AND,  &CPU::ABSX,           4, "AND" };
    instruction_table[0x3F] = { &CPU::AND,  &CPU::ALX,            5, "AND" };

    // ── EOR ──────────────────────────────────────────────────────────────────
    instruction_table[0x41] = { &CPU::EOR,  &CPU::DP_IND_X,      6, "EOR" };
    instruction_table[0x43] = { &CPU::EOR,  &CPU::SR,             4, "EOR" };
    instruction_table[0x45] = { &CPU::EOR,  &CPU::DP,             3, "EOR" };
    instruction_table[0x47] = { &CPU::EOR,  &CPU::DP_IND_Y_LONG, 6, "EOR" };
    instruction_table[0x49] = { &CPU::EOR,  &CPU::IMM_M,          2, "EOR" };
    instruction_table[0x4D] = { &CPU::EOR,  &CPU::ABS,            4, "EOR" };
    instruction_table[0x4F] = { &CPU::EOR,  &CPU::AL,             5, "EOR" };
    instruction_table[0x51] = { &CPU::EOR,  &CPU::DP_IND_Y,       5, "EOR" };
    instruction_table[0x52] = { &CPU::EOR,  &CPU::DP_IND,         5, "EOR" };
    instruction_table[0x53] = { &CPU::EOR,  &CPU::SR_IND_Y,       7, "EOR" };
    instruction_table[0x55] = { &CPU::EOR,  &CPU::DPX,            4, "EOR" };
    instruction_table[0x57] = { &CPU::EOR,  &CPU::DP_IND_Y_LONG, 6, "EOR" };
    instruction_table[0x59] = { &CPU::EOR,  &CPU::ABSY,           4, "EOR" };
    instruction_table[0x5D] = { &CPU::EOR,  &CPU::ABSX,           4, "EOR" };
    instruction_table[0x5F] = { &CPU::EOR,  &CPU::ALX,            5, "EOR" };

    // ── ADC ──────────────────────────────────────────────────────────────────
    instruction_table[0x61] = { &CPU::ADC,  &CPU::DP_IND_X,      6, "ADC" };
    instruction_table[0x63] = { &CPU::ADC,  &CPU::SR,             4, "ADC" };
    instruction_table[0x65] = { &CPU::ADC,  &CPU::DP,             3, "ADC" };
    instruction_table[0x67] = { &CPU::ADC,  &CPU::DP_IND_Y_LONG, 6, "ADC" };
    instruction_table[0x69] = { &CPU::ADC,  &CPU::IMM_M,          2, "ADC" };
    instruction_table[0x6D] = { &CPU::ADC,  &CPU::ABS,            4, "ADC" };
    instruction_table[0x6F] = { &CPU::ADC,  &CPU::AL,             5, "ADC" };
    instruction_table[0x71] = { &CPU::ADC,  &CPU::DP_IND_Y,       5, "ADC" };
    instruction_table[0x72] = { &CPU::ADC,  &CPU::DP_IND,         5, "ADC" };
    instruction_table[0x73] = { &CPU::ADC,  &CPU::SR_IND_Y,       7, "ADC" };
    instruction_table[0x75] = { &CPU::ADC,  &CPU::DPX,            4, "ADC" };
    instruction_table[0x77] = { &CPU::ADC,  &CPU::DP_IND_Y_LONG, 6, "ADC" };
    instruction_table[0x79] = { &CPU::ADC,  &CPU::ABSY,           4, "ADC" };
    instruction_table[0x7D] = { &CPU::ADC,  &CPU::ABSX,           4, "ADC" };
    instruction_table[0x7F] = { &CPU::ADC,  &CPU::ALX,            5, "ADC" };

    // ── STA ──────────────────────────────────────────────────────────────────
    instruction_table[0x81] = { &CPU::STA,  &CPU::DP_IND_X,      6, "STA" };
    instruction_table[0x83] = { &CPU::STA,  &CPU::SR,             4, "STA" };
    instruction_table[0x85] = { &CPU::STA,  &CPU::DP,             3, "STA" };
    instruction_table[0x87] = { &CPU::STA,  &CPU::DP_IND_Y_LONG, 6, "STA" };
    instruction_table[0x8D] = { &CPU::STA,  &CPU::ABS,            4, "STA" };
    instruction_table[0x8F] = { &CPU::STA,  &CPU::AL,             5, "STA" };
    instruction_table[0x91] = { &CPU::STA,  &CPU::DP_IND_Y,       6, "STA" };
    instruction_table[0x92] = { &CPU::STA,  &CPU::DP_IND,         5, "STA" };
    instruction_table[0x93] = { &CPU::STA,  &CPU::SR_IND_Y,       7, "STA" };
    instruction_table[0x95] = { &CPU::STA,  &CPU::DPX,            4, "STA" };
    instruction_table[0x97] = { &CPU::STA,  &CPU::DP_IND_Y_LONG, 6, "STA" };
    instruction_table[0x99] = { &CPU::STA,  &CPU::ABSY,           5, "STA" };
    instruction_table[0x9D] = { &CPU::STA,  &CPU::ABSX,           5, "STA" };
    instruction_table[0x9F] = { &CPU::STA,  &CPU::ALX,            5, "STA" };

    // ── LDA ──────────────────────────────────────────────────────────────────
    instruction_table[0xA1] = { &CPU::LDA,  &CPU::DP_IND_X,      6, "LDA" };
    instruction_table[0xA3] = { &CPU::LDA,  &CPU::SR,             4, "LDA" };
    instruction_table[0xA5] = { &CPU::LDA,  &CPU::DP,             3, "LDA" };
    instruction_table[0xA7] = { &CPU::LDA,  &CPU::DP_IND_Y_LONG, 6, "LDA" };
    instruction_table[0xA9] = { &CPU::LDA,  &CPU::IMM_M,          2, "LDA" };
    instruction_table[0xAD] = { &CPU::LDA,  &CPU::ABS,            4, "LDA" };
    instruction_table[0xAF] = { &CPU::LDA,  &CPU::AL,             5, "LDA" };
    instruction_table[0xB1] = { &CPU::LDA,  &CPU::DP_IND_Y,       5, "LDA" };
    instruction_table[0xB2] = { &CPU::LDA,  &CPU::DP_IND,         5, "LDA" };
    instruction_table[0xB3] = { &CPU::LDA,  &CPU::SR_IND_Y,       7, "LDA" };
    instruction_table[0xB5] = { &CPU::LDA,  &CPU::DPX,            4, "LDA" };
    instruction_table[0xB7] = { &CPU::LDA,  &CPU::DP_IND_Y_LONG, 6, "LDA" };
    instruction_table[0xB9] = { &CPU::LDA,  &CPU::ABSY,           4, "LDA" };
    instruction_table[0xBD] = { &CPU::LDA,  &CPU::ABSX,           4, "LDA" };
    instruction_table[0xBF] = { &CPU::LDA,  &CPU::ALX,            5, "LDA" };

    // ── CMP ──────────────────────────────────────────────────────────────────
    instruction_table[0xC1] = { &CPU::CMP,  &CPU::DP_IND_X,      6, "CMP" };
    instruction_table[0xC3] = { &CPU::CMP,  &CPU::SR,             4, "CMP" };
    instruction_table[0xC5] = { &CPU::CMP,  &CPU::DP,             3, "CMP" };
    instruction_table[0xC7] = { &CPU::CMP,  &CPU::DP_IND_Y_LONG, 6, "CMP" };
    instruction_table[0xC9] = { &CPU::CMP,  &CPU::IMM_M,          2, "CMP" };
    instruction_table[0xCD] = { &CPU::CMP,  &CPU::ABS,            4, "CMP" };
    instruction_table[0xCF] = { &CPU::CMP,  &CPU::AL,             5, "CMP" };
    instruction_table[0xD1] = { &CPU::CMP,  &CPU::DP_IND_Y,       5, "CMP" };
    instruction_table[0xD2] = { &CPU::CMP,  &CPU::DP_IND,         5, "CMP" };
    instruction_table[0xD3] = { &CPU::CMP,  &CPU::SR_IND_Y,       7, "CMP" };
    instruction_table[0xD5] = { &CPU::CMP,  &CPU::DPX,            4, "CMP" };
    instruction_table[0xD7] = { &CPU::CMP,  &CPU::DP_IND_Y_LONG, 6, "CMP" };
    instruction_table[0xD9] = { &CPU::CMP,  &CPU::ABSY,           4, "CMP" };
    instruction_table[0xDD] = { &CPU::CMP,  &CPU::ABSX,           4, "CMP" };
    instruction_table[0xDF] = { &CPU::CMP,  &CPU::ALX,            5, "CMP" };

    // ── SBC ──────────────────────────────────────────────────────────────────
    instruction_table[0xE1] = { &CPU::SBC,  &CPU::DP_IND_X,      6, "SBC" };
    instruction_table[0xE3] = { &CPU::SBC,  &CPU::SR,             4, "SBC" };
    instruction_table[0xE5] = { &CPU::SBC,  &CPU::DP,             3, "SBC" };
    instruction_table[0xE7] = { &CPU::SBC,  &CPU::DP_IND_Y_LONG, 6, "SBC" };
    instruction_table[0xE9] = { &CPU::SBC,  &CPU::IMM_M,          2, "SBC" };
    instruction_table[0xED] = { &CPU::SBC,  &CPU::ABS,            4, "SBC" };
    instruction_table[0xEF] = { &CPU::SBC,  &CPU::AL,             5, "SBC" };
    instruction_table[0xF1] = { &CPU::SBC,  &CPU::DP_IND_Y,       5, "SBC" };
    instruction_table[0xF2] = { &CPU::SBC,  &CPU::DP_IND,         5, "SBC" };
    instruction_table[0xF3] = { &CPU::SBC,  &CPU::SR_IND_Y,       7, "SBC" };
    instruction_table[0xF5] = { &CPU::SBC,  &CPU::DPX,            4, "SBC" };
    instruction_table[0xF7] = { &CPU::SBC,  &CPU::DP_IND_Y_LONG, 6, "SBC" };
    instruction_table[0xF9] = { &CPU::SBC,  &CPU::ABSY,           4, "SBC" };
    instruction_table[0xFD] = { &CPU::SBC,  &CPU::ABSX,           4, "SBC" };
    instruction_table[0xFF] = { &CPU::SBC,  &CPU::ALX,            5, "SBC" };

    // ── STX / STY / STZ ─────────────────────────────────────────────────────
    instruction_table[0x84] = { &CPU::STY,  &CPU::DP,   3, "STY" };
    instruction_table[0x86] = { &CPU::STX,  &CPU::DP,   3, "STX" };
    instruction_table[0x8C] = { &CPU::STY,  &CPU::ABS,  4, "STY" };
    instruction_table[0x8E] = { &CPU::STX,  &CPU::ABS,  4, "STX" };
    instruction_table[0x94] = { &CPU::STY,  &CPU::DPX,  4, "STY" };
    instruction_table[0x96] = { &CPU::STX,  &CPU::DPY,  4, "STX" };
    instruction_table[0x64] = { &CPU::STZ,  &CPU::DP,   3, "STZ" };
    instruction_table[0x74] = { &CPU::STZ,  &CPU::DPX,  4, "STZ" };
    instruction_table[0x9C] = { &CPU::STZ,  &CPU::ABS,  4, "STZ" };
    instruction_table[0x9E] = { &CPU::STZ,  &CPU::ABSX, 5, "STZ" };

    // ── LDX / LDY ────────────────────────────────────────────────────────────
    instruction_table[0xA0] = { &CPU::LDY,  &CPU::IMM_X, 2, "LDY" };
    instruction_table[0xA2] = { &CPU::LDX,  &CPU::IMM_X, 2, "LDX" };
    instruction_table[0xA4] = { &CPU::LDY,  &CPU::DP,    3, "LDY" };
    instruction_table[0xA6] = { &CPU::LDX,  &CPU::DP,    3, "LDX" };
    instruction_table[0xAC] = { &CPU::LDY,  &CPU::ABS,   4, "LDY" };
    instruction_table[0xAE] = { &CPU::LDX,  &CPU::ABS,   4, "LDX" };
    instruction_table[0xB4] = { &CPU::LDY,  &CPU::DPX,   4, "LDY" };
    instruction_table[0xB6] = { &CPU::LDX,  &CPU::DPY,   4, "LDX" };
    instruction_table[0xBC] = { &CPU::LDY,  &CPU::ABSX,  4, "LDY" };
    instruction_table[0xBE] = { &CPU::LDX,  &CPU::ABSY,  4, "LDX" };

    // ── CPX / CPY ────────────────────────────────────────────────────────────
    instruction_table[0xC0] = { &CPU::CPY,  &CPU::IMM_X, 2, "CPY" };
    instruction_table[0xC4] = { &CPU::CPY,  &CPU::DP,    3, "CPY" };
    instruction_table[0xCC] = { &CPU::CPY,  &CPU::ABS,   4, "CPY" };
    instruction_table[0xE0] = { &CPU::CPX,  &CPU::IMM_X, 2, "CPX" };
    instruction_table[0xE4] = { &CPU::CPX,  &CPU::DP,    3, "CPX" };
    instruction_table[0xEC] = { &CPU::CPX,  &CPU::ABS,   4, "CPX" };

    // ── INC / DEC ────────────────────────────────────────────────────────────
    instruction_table[0x1A] = { &CPU::INA,  &CPU::IMP,  2, "INA" };
    instruction_table[0x3A] = { &CPU::DEA,  &CPU::IMP,  2, "DEA" };
    instruction_table[0xC6] = { &CPU::DEC,  &CPU::DP,   5, "DEC" };
    instruction_table[0xCE] = { &CPU::DEC,  &CPU::ABS,  6, "DEC" };
    instruction_table[0xD6] = { &CPU::DEC,  &CPU::DPX,  6, "DEC" };
    instruction_table[0xDE] = { &CPU::DEC,  &CPU::ABSX, 7, "DEC" };
    instruction_table[0xE6] = { &CPU::INC,  &CPU::DP,   5, "INC" };
    instruction_table[0xEE] = { &CPU::INC,  &CPU::ABS,  6, "INC" };
    instruction_table[0xF6] = { &CPU::INC,  &CPU::DPX,  6, "INC" };
    instruction_table[0xFE] = { &CPU::INC,  &CPU::ABSX, 7, "INC" };
    instruction_table[0x88] = { &CPU::DEY,  &CPU::IMP,  2, "DEY" };
    instruction_table[0xC8] = { &CPU::INY,  &CPU::IMP,  2, "INY" };
    instruction_table[0xCA] = { &CPU::DEX,  &CPU::IMP,  2, "DEX" };
    instruction_table[0xE8] = { &CPU::INX,  &CPU::IMP,  2, "INX" };

    // ── ASL / LSR ────────────────────────────────────────────────────────────
    instruction_table[0x06] = { &CPU::ASL_M, &CPU::DP,   5, "ASL" };
    instruction_table[0x0A] = { &CPU::ASL_A, &CPU::ACC,  2, "ASL" };
    instruction_table[0x0E] = { &CPU::ASL_M, &CPU::ABS,  6, "ASL" };
    instruction_table[0x16] = { &CPU::ASL_M, &CPU::DPX,  6, "ASL" };
    instruction_table[0x1E] = { &CPU::ASL_M, &CPU::ABSX, 7, "ASL" };
    instruction_table[0x46] = { &CPU::LSR_M, &CPU::DP,   5, "LSR" };
    instruction_table[0x4A] = { &CPU::LSR_A, &CPU::ACC,  2, "LSR" };
    instruction_table[0x4E] = { &CPU::LSR_M, &CPU::ABS,  6, "LSR" };
    instruction_table[0x56] = { &CPU::LSR_M, &CPU::DPX,  6, "LSR" };
    instruction_table[0x5E] = { &CPU::LSR_M, &CPU::ABSX, 7, "LSR" };

    // ── ROL / ROR ────────────────────────────────────────────────────────────
    instruction_table[0x26] = { &CPU::ROL_M, &CPU::DP,   5, "ROL" };
    instruction_table[0x2A] = { &CPU::ROL_A, &CPU::ACC,  2, "ROL" };
    instruction_table[0x2E] = { &CPU::ROL_M, &CPU::ABS,  6, "ROL" };
    instruction_table[0x36] = { &CPU::ROL_M, &CPU::DPX,  6, "ROL" };
    instruction_table[0x3E] = { &CPU::ROL_M, &CPU::ABSX, 7, "ROL" };
    instruction_table[0x66] = { &CPU::ROR_M, &CPU::DP,   5, "ROR" };
    instruction_table[0x6A] = { &CPU::ROR_A, &CPU::ACC,  2, "ROR" };
    instruction_table[0x6E] = { &CPU::ROR_M, &CPU::ABS,  6, "ROR" };
    instruction_table[0x76] = { &CPU::ROR_M, &CPU::DPX,  6, "ROR" };
    instruction_table[0x7E] = { &CPU::ROR_M, &CPU::ABSX, 7, "ROR" };

    // ── Branches ─────────────────────────────────────────────────────────────
    instruction_table[0x10] = { &CPU::BPL,  &CPU::REL,   2, "BPL" };
    instruction_table[0x30] = { &CPU::BMI,  &CPU::REL,   2, "BMI" };
    instruction_table[0x50] = { &CPU::BVC,  &CPU::REL,   2, "BVC" };
    instruction_table[0x70] = { &CPU::BVS,  &CPU::REL,   2, "BVS" };
    instruction_table[0x80] = { &CPU::BRA,  &CPU::REL,   3, "BRA" };
    instruction_table[0x82] = { &CPU::BRL,  &CPU::REL16, 4, "BRL" };
    instruction_table[0x90] = { &CPU::BCC,  &CPU::REL,   2, "BCC" };
    instruction_table[0xB0] = { &CPU::BCS,  &CPU::REL,   2, "BCS" };
    instruction_table[0xD0] = { &CPU::BNE,  &CPU::REL,   2, "BNE" };
    instruction_table[0xF0] = { &CPU::BEQ,  &CPU::REL,   2, "BEQ" };

    // ── Jumps ────────────────────────────────────────────────────────────────
    instruction_table[0x4C] = { &CPU::JMP,     &CPU::ABS,  3, "JMP" };
    instruction_table[0x5C] = { &CPU::JML,     &CPU::AL,   4, "JML" };
    instruction_table[0x6C] = { &CPU::JMP_IND, &CPU::IMP,  5, "JMP" };
    instruction_table[0x7C] = { &CPU::JMP_IND_X, &CPU::IMP, 6, "JMP" };
    instruction_table[0xDC] = { &CPU::JML_IND, &CPU::IMP,  6, "JML" };

    // ── Calls / Returns ──────────────────────────────────────────────────────
    instruction_table[0x20] = { &CPU::JSR,     &CPU::ABS,  6, "JSR" };
    instruction_table[0x22] = { &CPU::JSL,     &CPU::AL,   8, "JSL" };
    instruction_table[0x40] = { &CPU::RTI,     &CPU::IMP,  6, "RTI" };
    instruction_table[0x60] = { &CPU::RTS,     &CPU::IMP,  6, "RTS" };
    instruction_table[0x6B] = { &CPU::RTL,     &CPU::IMP,  6, "RTL" };
    instruction_table[0xFC] = { &CPU::JSR_IND_X, &CPU::IMP, 8, "JSR" };

    // ── Stack ────────────────────────────────────────────────────────────────
    instruction_table[0x08] = { &CPU::PHP, &CPU::IMP, 3, "PHP" };
    instruction_table[0x28] = { &CPU::PLP, &CPU::IMP, 4, "PLP" };
    instruction_table[0x48] = { &CPU::PHA, &CPU::IMP, 3, "PHA" };
    instruction_table[0x68] = { &CPU::PLA, &CPU::IMP, 4, "PLA" };
    instruction_table[0x5A] = { &CPU::PHY, &CPU::IMP, 3, "PHY" };
    instruction_table[0x7A] = { &CPU::PLY, &CPU::IMP, 4, "PLY" };
    instruction_table[0xDA] = { &CPU::PHX, &CPU::IMP, 3, "PHX" };
    instruction_table[0xFA] = { &CPU::PLX, &CPU::IMP, 4, "PLX" };
    instruction_table[0x4B] = { &CPU::PHK, &CPU::IMP, 3, "PHK" };
    instruction_table[0x8B] = { &CPU::PHB, &CPU::IMP, 3, "PHB" };
    instruction_table[0xAB] = { &CPU::PLB, &CPU::IMP, 4, "PLB" };
    instruction_table[0x0B] = { &CPU::PHD, &CPU::IMP, 4, "PHD" };
    instruction_table[0x2B] = { &CPU::PLD, &CPU::IMP, 5, "PLD" };
    instruction_table[0xF4] = { &CPU::PEA, &CPU::IMM16, 5, "PEA" };
    instruction_table[0xD4] = { &CPU::PEI, &CPU::DP,    6, "PEI" };
    instruction_table[0x62] = { &CPU::PER, &CPU::IMM16, 6, "PER" };

    // ── Transfers ────────────────────────────────────────────────────────────
    instruction_table[0xAA] = { &CPU::TAX, &CPU::IMP, 2, "TAX" };
    instruction_table[0xA8] = { &CPU::TAY, &CPU::IMP, 2, "TAY" };
    instruction_table[0x8A] = { &CPU::TXA, &CPU::IMP, 2, "TXA" };
    instruction_table[0x98] = { &CPU::TYA, &CPU::IMP, 2, "TYA" };
    instruction_table[0x9A] = { &CPU::TXS, &CPU::IMP, 2, "TXS" };
    instruction_table[0xBA] = { &CPU::TSX, &CPU::IMP, 2, "TSX" };
    instruction_table[0x9B] = { &CPU::TXY, &CPU::IMP, 2, "TXY" };
    instruction_table[0xBB] = { &CPU::TYX, &CPU::IMP, 2, "TYX" };
    instruction_table[0x5B] = { &CPU::TCD, &CPU::IMP, 2, "TCD" };
    instruction_table[0x7B] = { &CPU::TDC, &CPU::IMP, 2, "TDC" };
    instruction_table[0x1B] = { &CPU::TCS, &CPU::IMP, 2, "TCS" };
    instruction_table[0x3B] = { &CPU::TSC, &CPU::IMP, 2, "TSC" };
    instruction_table[0xEB] = { &CPU::XBA, &CPU::IMP, 3, "XBA" };

    // ── Processor Control ────────────────────────────────────────────────────
    instruction_table[0x18] = { &CPU::CLC, &CPU::IMP, 2, "CLC" };
    instruction_table[0x38] = { &CPU::SEC, &CPU::IMP, 2, "SEC" };
    instruction_table[0x58] = { &CPU::CLI, &CPU::IMP, 2, "CLI" };
    instruction_table[0x78] = { &CPU::SEI, &CPU::IMP, 2, "SEI" };
    instruction_table[0xB8] = { &CPU::CLV, &CPU::IMP, 2, "CLV" };
    instruction_table[0xD8] = { &CPU::CLD, &CPU::IMP, 2, "CLD" };
    instruction_table[0xF8] = { &CPU::SED, &CPU::IMP, 2, "SED" };
    instruction_table[0xC2] = { &CPU::REP, &CPU::IMM8, 3, "REP" };
    instruction_table[0xE2] = { &CPU::SEP, &CPU::IMM8, 3, "SEP" };
    instruction_table[0xFB] = { &CPU::XCE, &CPU::IMP,  2, "XCE" };

    // ── NOP / Misc ───────────────────────────────────────────────────────────
    instruction_table[0xEA] = { &CPU::NOP, &CPU::IMP, 2, "NOP" };

    // ── Block Move ───────────────────────────────────────────────────────────
    instruction_table[0x44] = { &CPU::MVP, &CPU::IMP, 7, "MVP" };
    instruction_table[0x54] = { &CPU::MVN, &CPU::IMP, 7, "MVN" };
}
