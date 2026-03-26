#include "cpu.hpp"
#include "bus.hpp"
#include <cstdio> 

CPU::CPU() : bus(nullptr) {
    reset();
    buildInstructionTable();
}

void CPU::connectBus(Bus* b) { bus = b; }

void CPU::reset() {
    PC = read16(0x00FFFC); 
    PB = 0x00; DB = 0x00; SP = 0x01FF; P = 0x34; E = true; 
    cycles_remaining = 0;
}

uint8_t CPU::step() {
    uint32_t pc_address = (PB << 16) | PC;
    fetched_opcode = read8(pc_address);
    PC++;

    Instruction instr = instruction_table[fetched_opcode];
    cycles_remaining = instr.cycles;

    if (instr.addrmode) (this->*instr.addrmode)(); 
    if (instr.operate)  (this->*instr.operate)();

    return cycles_remaining;
}

void CPU::printState() {
    printf("PC:%04X A:%04X X:%04X Y:%04X P:%02X SP:%04X E:%d Opcode:%02X\n", 
           PC, A, X, Y, P, SP, E, bus ? bus->read((PB << 16) | PC) : 0);
}

void CPU::nmi() {
    uint16_t vector = read16(0xFFEA);
    push8(PB); push16(PC); push8(P);
    setFlag(0x04, true); 
    PB = 0x00; PC = vector;
    cycles_remaining += 8;
}

void CPU::setFlag(uint8_t flag, bool value) {
    if (value) P |= flag; else P &= ~flag;
}

bool CPU::getFlag(uint8_t flag) { return (P & flag) > 0; }

uint8_t CPU::read8(uint32_t addr) { return bus ? bus->read(addr) : 0; }

uint16_t CPU::read16(uint32_t addr) {
    uint16_t lo = read8(addr);
    uint16_t hi = read8(addr + 1);
    return (hi << 8) | lo;
}

void CPU::write8(uint32_t addr, uint8_t data) {
    if (bus) bus->write(addr, data);
}

void CPU::write16(uint32_t addr, uint16_t data) {
    write8(addr, data & 0xFF);
    write8(addr + 1, (data >> 8) & 0xFF);
}

void CPU::push8(uint8_t data) {
    write8(SP, data);
    SP--;
    if (E && SP < 0x0100) SP = 0x01FF; 
}

void CPU::push16(uint16_t data) {
    push8((data >> 8) & 0xFF); 
    push8(data & 0xFF);        
}

uint8_t CPU::pop8() {
    SP++;
    if (E && SP > 0x01FF) SP = 0x0100;
    return read8(SP);
}

uint16_t CPU::pop16() {
    uint8_t lo = pop8();
    uint8_t hi = pop8();
    return (hi << 8) | lo;
}

// --- Addressing Modes ---
void CPU::IMP() {}
void CPU::ACC() {}
void CPU::IMM8() { effective_address = (PB << 16) | PC++; }
void CPU::IMM16() { effective_address = (PB << 16) | PC; PC += 2; }
void CPU::ABS() { effective_address = (DB << 16) | read16((PB << 16) | PC); PC += 2; }
void CPU::ABSX() {
    uint16_t base = read16((PB << 16) | PC);
    PC += 2;
    effective_address = (DB << 16) | (base + X);
}
void CPU::IMM_M() {
    effective_address = (PB << 16) | PC;
    PC += getFlag(0x20) ? 1 : 2; 
}
void CPU::IMM_X() {
    effective_address = (PB << 16) | PC;
    PC += getFlag(0x10) ? 1 : 2; 
}
void CPU::REL() { effective_address = (PB << 16) | PC++; }
void CPU::AL() {
    uint16_t low = read16((PB << 16) | PC); PC += 2;
    uint8_t bank = read8((PB << 16) | PC++);
    effective_address = (bank << 16) | low;
}
void CPU::DP() { effective_address = read8((PB << 16) | PC++); }
void CPU::DPX() {
    uint8_t offset = read8((PB << 16) | PC++);
    effective_address = (offset + X) & 0xFFFF;
}

// --- Opcodes ---
void CPU::XBA() {
    uint8_t low = A & 0xFF;
    uint8_t high = (A >> 8) & 0xFF;
    A = (low << 8) | high;
    setFlag(0x02, high == 0);
    setFlag(0x80, (high & 0x80) != 0);
    cycles_remaining = 3;
}

// Transfers
void CPU::TAX() {
    X = getFlag(0x10) ? (A & 0xFF) : A;
    setFlag(0x02, X == 0);
    setFlag(0x80, (X & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
    cycles_remaining = 2;
}

void CPU::TAY() {
    Y = getFlag(0x10) ? (A & 0xFF) : A;
    setFlag(0x02, Y == 0);
    setFlag(0x80, (Y & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
    cycles_remaining = 2;
}

void CPU::TXA() {
    if (getFlag(0x20)) {
        A = (A & 0xFF00) | (X & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A = X;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::TYA() {
    if (getFlag(0x20)) {
        A = (A & 0xFF00) | (Y & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A = Y;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::TSX() {
    X = getFlag(0x10) ? (SP & 0xFF) : SP;
    setFlag(0x02, X == 0);
    setFlag(0x80, (X & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
    cycles_remaining = 2;
}

// Shifts
void CPU::ASL_A() {
    if (getFlag(0x20)) {
        setFlag(0x01, (A & 0x80) != 0);
        A = (A & 0xFF00) | ((A << 1) & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        setFlag(0x01, (A & 0x8000) != 0);
        A <<= 1;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::LSR_A() {
    if (getFlag(0x20)) {
        setFlag(0x01, (A & 0x01) != 0);
        A = (A & 0xFF00) | ((A >> 1) & 0x7F);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, false);
    } else {
        setFlag(0x01, (A & 0x01) != 0);
        A >>= 1;
        setFlag(0x02, A == 0);
        setFlag(0x80, false);
    }
    cycles_remaining = 2;
}

void CPU::LDA() {
    bool m_flag = getFlag(0x20); 
    if (m_flag) {
        uint8_t data = read8(effective_address);
        A = (A & 0xFF00) | data; 
        setFlag(0x02, (A & 0x00FF) == 0x00);         
        setFlag(0x80, (A & 0x0080) == 0x80);         
    } else {
        uint16_t data = read16(effective_address);
        A = data;
        setFlag(0x02, A == 0x0000);                  
        setFlag(0x80, (A & 0x8000) == 0x8000);       
        cycles_remaining++; 
    }
}

void CPU::NOP() {}

void CPU::JML() {
    PB = (effective_address >> 16) & 0xFF;
    PC = effective_address & 0xFFFF;
}

void CPU::BRA() {
    int8_t offset = static_cast<int8_t>(read8(effective_address));
    PC += offset; cycles_remaining++;
}
void CPU::BEQ() {
    if (getFlag(0x02)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BNE() {
    if (!getFlag(0x02)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BCC() {
    if (!getFlag(0x01)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BCS() {
    if (getFlag(0x01)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BPL() {
    if (!getFlag(0x80)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BMI() {
    if (getFlag(0x80)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BVC() {
    if (!getFlag(0x40)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}
void CPU::BVS() {
    if (getFlag(0x40)) { 
        int8_t offset = static_cast<int8_t>(read8(effective_address));
        PC += offset; cycles_remaining++;
    }
}

void CPU::JMP() { PC = effective_address & 0xFFFF; }
void CPU::JSL() {
    push8(PB);
    push16(PC - 1);
    uint16_t target_pc = read16((PB << 16) | PC);
    uint8_t target_pb = read8((PB << 16) | (PC + 2));
    PC = target_pc;
    PB = target_pb;
    cycles_remaining = 8;
}

void CPU::AND() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    if (getFlag(0x20)) { 
        A = (A & 0xFF00) | ((A & 0xFF) & (val & 0xFF));
        setFlag(0x02, (A & 0xFF) == 0);         
        setFlag(0x80, (A & 0x80) != 0);         
    } else { 
        A &= val;
        setFlag(0x02, A == 0);                  
        setFlag(0x80, (A & 0x8000) != 0);       
    }
}

void CPU::SEI() { setFlag(0x04, true); cycles_remaining = 2; }
void CPU::CLC() { setFlag(0x01, false); cycles_remaining = 2; }

void CPU::XCE() { 
    bool carry = getFlag(0x01);
    setFlag(0x01, E);
    E = carry;
    if (!E) { setFlag(0x20, true); setFlag(0x10, true); }
    cycles_remaining = 2; 
}

void CPU::ORA() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    if (getFlag(0x20)) {
        A = (A & 0xFF00) | ((A & 0xFF) | (val & 0xFF));
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A |= val;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::STZ() {
    if (getFlag(0x20)) { write8(effective_address, 0); } 
    else { write16(effective_address, 0); }
}

void CPU::PHK() { push8(PB); cycles_remaining = 3; }
void CPU::PHP() { push8(P);  cycles_remaining = 3; }
void CPU::PLP() { P = pop8(); cycles_remaining = 4; }
void CPU::PHB() { push8(DB); cycles_remaining = 3; }
void CPU::PLB() {
    DB = pop8();
    setFlag(0x02, DB == 0);
    setFlag(0x80, (DB & 0x80) != 0);
    cycles_remaining = 4;
}

void CPU::REP() {
    uint8_t data = read8(effective_address);
    P &= ~data; cycles_remaining = 3;
}
void CPU::SEP() {
    uint8_t data = read8(effective_address);
    P |= data; cycles_remaining = 3;
}

void CPU::LDX() {
    bool x_flag = getFlag(0x10);
    if (x_flag) {
        X = read8(effective_address);
        setFlag(0x02, (X & 0x00FF) == 0x00);
        setFlag(0x80, (X & 0x0080) == 0x80);
    } else {
        X = read16(effective_address);
        setFlag(0x02, X == 0x0000);
        setFlag(0x80, (X & 0x8000) == 0x8000);
        cycles_remaining++; 
    }
}

void CPU::LDY() {
    bool x_flag = getFlag(0x10); 
    if (x_flag) {
        Y = read8(effective_address);
        setFlag(0x02, (Y & 0x00FF) == 0x00);
        setFlag(0x80, (Y & 0x0080) == 0x80);
    } else {
        Y = read16(effective_address);
        setFlag(0x02, Y == 0x0000);
        setFlag(0x80, (Y & 0x8000) == 0x8000);
        cycles_remaining++; 
    }
}

void CPU::STA() {
    bool m_flag = getFlag(0x20);
    if (m_flag) write8(effective_address, A & 0xFF); 
    else write16(effective_address, A);
}

void CPU::TXS() {
    SP = X;
    if (E) SP = 0x0100 | (SP & 0xFF); 
    cycles_remaining = 2;
}

void CPU::TCD() {
    D = A;
    setFlag(0x02, D == 0x0000);
    setFlag(0x80, (D & 0x8000) == 0x8000);
    cycles_remaining = 2;
}

void CPU::TCS() {
    SP = A;
    if (E) SP = 0x0100 | (SP & 0xFF);
    cycles_remaining = 2;
}

void CPU::DEX() {
    bool x_flag = getFlag(0x10);
    if (x_flag) {
        X = (X - 1) & 0xFF;
        setFlag(0x02, X == 0x00);
        setFlag(0x80, (X & 0x80) != 0);
    } else {
        X = (X - 1) & 0xFFFF;
        setFlag(0x02, X == 0x0000);
        setFlag(0x80, (X & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::BIT() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint16_t mask = getFlag(0x20) ? (A & 0xFF) : A;
    setFlag(0x02, (val & mask) == 0); 
    if (getFlag(0x20)) {
        setFlag(0x80, (val & 0x80) != 0); 
        setFlag(0x40, (val & 0x40) != 0); 
    } else {
        setFlag(0x80, (val & 0x8000) != 0); 
        setFlag(0x40, (val & 0x4000) != 0); 
    }
}

void CPU::EOR() {
    if (getFlag(0x20)) {
        uint8_t data = read8(effective_address);
        A = (A & 0xFF00) | ((A & 0xFF) ^ data);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A ^= read16(effective_address);
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::CMP() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x20) ? (A & 0xFF) : A;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val); 
    if (getFlag(0x20)) {
        setFlag(0x02, (res & 0xFF) == 0);
        setFlag(0x80, (res & 0x80) != 0);
    } else {
        setFlag(0x02, (res & 0xFFFF) == 0);
        setFlag(0x80, (res & 0x8000) != 0);
    }
}

void CPU::CPX() {
    uint16_t val = getFlag(0x10) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x10) ? (X & 0xFF) : X;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val);
    setFlag(0x02, (res & (getFlag(0x10) ? 0xFF : 0xFFFF)) == 0);
    setFlag(0x80, (res & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}

void CPU::CPY() {
    uint16_t val = getFlag(0x10) ? read8(effective_address) : read16(effective_address);
    uint16_t reg = getFlag(0x10) ? (Y & 0xFF) : Y;
    uint32_t res = reg - val;
    setFlag(0x01, reg >= val);
    setFlag(0x02, (res & (getFlag(0x10) ? 0xFF : 0xFFFF)) == 0);
    setFlag(0x80, (res & (getFlag(0x10) ? 0x80 : 0x8000)) != 0);
}

void CPU::ADC() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    uint32_t result;
    if (getFlag(0x20)) { 
        uint8_t a8 = A & 0xFF;
        result = a8 + (val & 0xFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFF); 
        setFlag(0x40, ~(a8 ^ (val & 0xFF)) & (a8 ^ result) & 0x80); 
        A = (A & 0xFF00) | (result & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else { 
        result = A + val + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFFFF);
        setFlag(0x40, ~(A ^ val) & (A ^ result) & 0x8000);
        A = result & 0xFFFF;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::JSR() {
    push16(PC - 1);
    PC = effective_address & 0xFFFF;
    cycles_remaining = 6;
}

void CPU::INC() {
    if (getFlag(0x20)) { 
        uint8_t data = read8(effective_address) + 1;
        write8(effective_address, data);
        setFlag(0x02, data == 0);
        setFlag(0x80, (data & 0x80) != 0);
    } else { 
        uint16_t data = read16(effective_address) + 1;
        write16(effective_address, data);
        setFlag(0x02, data == 0);
        setFlag(0x80, (data & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::DEC() {
    if (getFlag(0x20)) {
        uint8_t data = read8(effective_address) - 1;
        write8(effective_address, data);
        setFlag(0x02, data == 0);
        setFlag(0x80, (data & 0x80) != 0);
    } else {
        uint16_t data = read16(effective_address) - 1;
        write16(effective_address, data);
        setFlag(0x02, data == 0);
        setFlag(0x80, (data & 0x8000) != 0);
    }
    cycles_remaining = 2;
}

void CPU::INA() {
    if (getFlag(0x20)) {
        A = (A & 0xFF00) | ((A + 1) & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A++;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::DEA() {
    if (getFlag(0x20)) {
        A = (A & 0xFF00) | ((A - 1) & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        A--;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::RTS() {
    PC = pop16() + 1;
    cycles_remaining = 6;
}

void CPU::RTL() {
    PC = pop16() + 1;
    PB = pop8();
    cycles_remaining = 6;
}

void CPU::RTI() {
    P = pop8();
    PC = pop16();
    PB = pop8();
    cycles_remaining = 6;
}

void CPU::SBC() {
    uint16_t val = getFlag(0x20) ? read8(effective_address) : read16(effective_address);
    val = ~val; 
    uint32_t result;
    if (getFlag(0x20)) {
        uint8_t a8 = A & 0xFF;
        result = a8 + (val & 0xFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFF);
        setFlag(0x40, ~(a8 ^ (val & 0xFF)) & (a8 ^ result) & 0x80);
        A = (A & 0xFF00) | (result & 0xFF);
        setFlag(0x02, (A & 0xFF) == 0);
        setFlag(0x80, (A & 0x80) != 0);
    } else {
        result = A + (val & 0xFFFF) + (getFlag(0x01) ? 1 : 0);
        setFlag(0x01, result > 0xFFFF);
        setFlag(0x40, ~(A ^ (val & 0xFFFF)) & (A ^ result) & 0x8000);
        A = result & 0xFFFF;
        setFlag(0x02, A == 0);
        setFlag(0x80, (A & 0x8000) != 0);
    }
}

void CPU::buildInstructionTable() {
    for (int i = 0; i < 256; i++) {
        instruction_table[i] = { &CPU::NOP, &CPU::IMP, 2 };
    }
    
    instruction_table[0xEB] = { &CPU::XBA, &CPU::IMP, 3 }; 
    instruction_table[0xEA] = { &CPU::NOP, &CPU::IMP, 2 }; 
    
    instruction_table[0x49] = { &CPU::EOR, &CPU::IMM_M, 2 };
    instruction_table[0x45] = { &CPU::EOR, &CPU::DP,    3 };
    instruction_table[0x4D] = { &CPU::EOR, &CPU::ABS,   4 };

    instruction_table[0xE0] = { &CPU::CPX, &CPU::IMM_X, 2 };
    instruction_table[0xC0] = { &CPU::CPY, &CPU::IMM_X, 2 };
    instruction_table[0xE4] = { &CPU::CPX, &CPU::DP,    3 };
    instruction_table[0xC4] = { &CPU::CPY, &CPU::DP,    3 };

    instruction_table[0x69] = { &CPU::ADC, &CPU::IMM_M, 2 };
    instruction_table[0x65] = { &CPU::ADC, &CPU::DP,    3 };
    instruction_table[0x6D] = { &CPU::ADC, &CPU::ABS,   4 };

    instruction_table[0xE9] = { &CPU::SBC, &CPU::IMM_M, 2 };
    instruction_table[0xE5] = { &CPU::SBC, &CPU::DP,    3 };
    instruction_table[0xED] = { &CPU::SBC, &CPU::ABS,   4 };
    instruction_table[0x08] = { &CPU::PHP, nullptr, 3 }; 
    instruction_table[0x18] = { &CPU::CLC, nullptr, 2 };
    instruction_table[0x1B] = { &CPU::TCS, nullptr, 2 }; 
    instruction_table[0x22] = { &CPU::JSL, &CPU::AL, 8 }; 
    instruction_table[0x28] = { &CPU::PLP, nullptr, 4 }; 
    instruction_table[0x40] = { &CPU::RTI, nullptr, 6 }; 
    instruction_table[0x4B] = { &CPU::PHK, nullptr, 3 }; 
    instruction_table[0x4C] = { &CPU::JMP, &CPU::ABS, 3 };
    instruction_table[0x5B] = { &CPU::TCD, nullptr, 2 }; 
    instruction_table[0x5C] = { &CPU::JML, &CPU::AL, 4 };
    instruction_table[0x64] = { &CPU::STZ, &CPU::DP, 3 }; 
    instruction_table[0x78] = { &CPU::SEI, nullptr, 2 }; 
    instruction_table[0x85] = { &CPU::STA, &CPU::DP, 3 };
    instruction_table[0x8B] = { &CPU::PHB, nullptr, 3 }; 
    instruction_table[0x8D] = { &CPU::STA, &CPU::ABS, 4 }; 
    instruction_table[0x95] = { &CPU::STA, &CPU::DPX, 4 };
    instruction_table[0x9A] = { &CPU::TXS, nullptr, 2 };
    instruction_table[0x9C] = { &CPU::STZ, &CPU::ABS, 4 }; 
    instruction_table[0x9D] = { &CPU::STA, &CPU::ABSX, 5 }; 
    instruction_table[0xA0] = { &CPU::LDY, &CPU::IMM_X, 2 };
    instruction_table[0xA2] = { &CPU::LDX, &CPU::IMM_X, 2 };
    instruction_table[0xA5] = { &CPU::LDA, &CPU::DP, 3 };
    instruction_table[0xA9] = { &CPU::LDA, &CPU::IMM_M, 2 }; 
    instruction_table[0xAB] = { &CPU::PLB, nullptr, 4 }; 
    instruction_table[0xBD] = { &CPU::LDA, &CPU::ABSX, 4 }; 
    instruction_table[0xC2] = { &CPU::REP, &CPU::IMM8, 3 }; 
    instruction_table[0xCA] = { &CPU::DEX, nullptr, 2 }; 
    instruction_table[0xD0] = { &CPU::BNE, &CPU::REL, 2 };
    instruction_table[0xE2] = { &CPU::SEP, &CPU::IMM8, 3 };
    instruction_table[0xFB] = { &CPU::XCE, nullptr, 2 };
    instruction_table[0x8F] = { &CPU::STA, &CPU::AL, 5 };   
    instruction_table[0xAF] = { &CPU::LDA, &CPU::AL, 5 };   
    instruction_table[0xCF] = { &CPU::CMP, &CPU::AL, 5 };   
    instruction_table[0x29] = { &CPU::AND, &CPU::IMM_M, 2 }; 
    instruction_table[0x25] = { &CPU::AND, &CPU::DP,    3 }; 
    instruction_table[0x2D] = { &CPU::AND, &CPU::ABS,   4 }; 
    instruction_table[0x09] = { &CPU::ORA, &CPU::IMM_M, 2 };
    instruction_table[0x24] = { &CPU::BIT, &CPU::DP,    3 };
    instruction_table[0x2C] = { &CPU::BIT, &CPU::ABS,   4 };

    instruction_table[0x20] = { &CPU::JSR, &CPU::ABS, 6 };
    instruction_table[0x60] = { &CPU::RTS, nullptr,   6 };
    instruction_table[0x6B] = { &CPU::RTL, nullptr,   6 };

    instruction_table[0xC9] = { &CPU::CMP, &CPU::IMM_M, 2 }; 
    instruction_table[0xC5] = { &CPU::CMP, &CPU::DP,    3 }; 
    instruction_table[0xCD] = { &CPU::CMP, &CPU::ABS,   4 }; 
    instruction_table[0x80] = { &CPU::BRA, &CPU::REL, 3 };
    instruction_table[0xF0] = { &CPU::BEQ, &CPU::REL, 2 };
    instruction_table[0x90] = { &CPU::BCC, &CPU::REL, 2 };
    instruction_table[0xB0] = { &CPU::BCS, &CPU::REL, 2 };
    instruction_table[0x10] = { &CPU::BPL, &CPU::REL, 2 };
    instruction_table[0x30] = { &CPU::BMI, &CPU::REL, 2 };
    instruction_table[0x50] = { &CPU::BVC, &CPU::REL, 2 }; 
    instruction_table[0x70] = { &CPU::BVS, &CPU::REL, 2 }; 
    instruction_table[0x1A] = { &CPU::INA, nullptr,   2 };
    instruction_table[0x3A] = { &CPU::DEA, nullptr,   2 };
    instruction_table[0xEE] = { &CPU::INC, &CPU::ABS, 6 };
    instruction_table[0xE6] = { &CPU::INC, &CPU::DP,  5 };
    instruction_table[0xCE] = { &CPU::DEC, &CPU::ABS, 6 };
    instruction_table[0xC6] = { &CPU::DEC, &CPU::DP,  5 };

    // New Mappings
    instruction_table[0xAA] = { &CPU::TAX, &CPU::IMP, 2 };
    instruction_table[0xA8] = { &CPU::TAY, &CPU::IMP, 2 };
    instruction_table[0x8A] = { &CPU::TXA, &CPU::IMP, 2 };
    instruction_table[0x98] = { &CPU::TYA, &CPU::IMP, 2 };
    instruction_table[0xBA] = { &CPU::TSX, &CPU::IMP, 2 };
    instruction_table[0x0A] = { &CPU::ASL_A, &CPU::ACC, 2 };
    instruction_table[0x4A] = { &CPU::LSR_A, &CPU::ACC, 2 };
}