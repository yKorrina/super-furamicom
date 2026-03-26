#ifndef CPU_HPP
#define CPU_HPP

#include <cstdint>
#include <fstream>
#include <string>

class Bus;

class CPU {
public:
    using AddrModeFunc = void (CPU::*)();
    using OpcodeFunc   = void (CPU::*)();

    struct Instruction {
        OpcodeFunc   operate;
        AddrModeFunc addrmode;
        uint8_t      cycles;
        const char*  name;   // Mnemonic for trace log
    };

    CPU();
    ~CPU();
    void connectBus(Bus* b);
    void reset();
    uint8_t step();
    void printState();
    void nmi();

    // Diagnostic trace — call before loading ROM to enable
    void enableTrace(const std::string& path, uint64_t max_lines = 500000);

    // Check if CPU is halted (STP/WAI/infinite loop detection)
    bool isHalted() const { return halted; }

private:
    Bus* bus;

    uint16_t A, X, Y, SP, PC, D;
    uint8_t  PB, DB, P;
    bool     E;

    uint8_t  fetched_opcode;
    uint32_t absolute_address;
    uint32_t effective_address;
    uint16_t fetched_data;
    uint8_t  cycles_remaining;

    bool halted = false;

    Instruction instruction_table[256];

    std::ofstream log_file;
    uint64_t instruction_count;
    uint64_t max_trace_lines;
    bool     trace_enabled;

    // Infinite loop detection
    uint32_t prev_pc_full = 0;
    int      same_pc_count = 0;

    void setFlag(uint8_t flag, bool value);
    bool getFlag(uint8_t flag);

    uint8_t  read8 (uint32_t addr);
    uint16_t read16(uint32_t addr);
    void     write8 (uint32_t addr, uint8_t  data);
    void     write16(uint32_t addr, uint16_t data);

    void     push8 (uint8_t  data);
    void     push16(uint16_t data);
    uint8_t  pop8 ();
    uint16_t pop16();

    // ── Addressing Modes ──────────────────────────────────────────────────────
    void IMP();
    void ACC();
    void IMM8();
    void IMM16();
    void IMM_M();
    void IMM_X();
    void ABS();
    void ABSX();
    void ABSY();
    void AL();
    void ALX();
    void DP();
    void DPX();
    void DPY();
    void DP_IND();
    void DP_IND_X();
    void DP_IND_Y();
    void DP_IND_Y_LONG();
    void SR();
    void SR_IND_Y();
    void REL();
    void REL16();

    // ── Opcodes ───────────────────────────────────────────────────────────────
    void LDA(); void LDX(); void LDY();
    void STA(); void STX(); void STY(); void STZ();

    void ADC(); void SBC();
    void INC(); void DEC(); void INA(); void DEA();
    void INX(); void INY(); void DEX(); void DEY();

    void AND(); void ORA(); void EOR();
    void BIT();
    void TSB(); void TRB();

    void ASL_A(); void LSR_A();
    void ROL_A(); void ROR_A();
    void ASL_M(); void LSR_M();
    void ROL_M(); void ROR_M();

    void CMP(); void CPX(); void CPY();

    void BRA(); void BRL();
    void BEQ(); void BNE();
    void BCC(); void BCS();
    void BPL(); void BMI();
    void BVC(); void BVS();

    void JMP(); void JML();
    void JMP_IND();
    void JMP_IND_X();
    void JML_IND();
    void JSR(); void JSL();
    void JSR_IND_X();
    void RTS(); void RTL(); void RTI();

    void PHA(); void PLA();
    void PHX(); void PLX();
    void PHY(); void PLY();
    void PHK(); void PHP(); void PLP();
    void PHB(); void PLB();
    void PHD(); void PLD();
    void PEA(); void PEI(); void PER();

    void TAX(); void TAY();
    void TXA(); void TYA();
    void TXS(); void TSX();
    void TXY(); void TYX();
    void TCD(); void TDC();
    void TCS(); void TSC();
    void XBA();

    void SEI(); void CLC();
    void SEC(); void CLI(); void CLV();
    void SED(); void CLD();
    void REP(); void SEP();
    void XCE();
    void NOP();
    void BRK();
    void STP();
    void WAI();

    void MVN(); void MVP();

    void buildInstructionTable();
};

#endif // CPU_HPP
