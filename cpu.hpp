#ifndef CPU_HPP
#define CPU_HPP

#include <cstdint>

class Bus;

class CPU {
public:
    using AddrModeFunc = void (CPU::*)();
    using OpcodeFunc = void (CPU::*)();

    struct Instruction {
        OpcodeFunc operate;
        AddrModeFunc addrmode;
        uint8_t cycles;
    };

    CPU();
    void connectBus(Bus* b);
    void reset();
    uint8_t step();
    void printState();

private:
    Bus* bus;

    // Registers
    uint16_t A, X, Y, SP, PC, D; // Added D (Direct Page)
    uint8_t PB, DB, P;
    bool E; 

    // Internal state
    uint8_t  fetched_opcode;
    uint32_t absolute_address;
    uint32_t effective_address;
    uint16_t fetched_data;
    uint8_t  cycles_remaining;

    Instruction instruction_table[256];

    void setFlag(uint8_t flag, bool value);
    bool getFlag(uint8_t flag);

    uint8_t read8(uint32_t addr);
    uint16_t read16(uint32_t addr);
    void write8(uint32_t addr, uint8_t data);
    void write16(uint32_t addr, uint16_t data);

    void push8(uint8_t data);
    void push16(uint16_t data);
    uint8_t pop8();
    uint16_t pop16();

    // Addressing Modes
    void AL();   // Absolute Long (24-bit)
    void IMM8();  
    void IMM16(); 
    void ABS();   
    void ABSX();  // Added Absolute Indexed X
    void DP();    // Added Direct Page
    void DPX();   // Added Direct Page Indexed X
    void IMM_M(); // Dynamic based on M flag
    void IMM_X(); // Dynamic based on X flag
    void REL();   // Relative addressing for branches

    // Opcodes
    void JSR(); // Jump to Subroutine
    void RTS(); // Return from Subroutine
    void RTL(); // Return from Subroutine Long
    void LDA(); 
    void ADC(); 
    void NOP(); 
    void LDX();
    void LDY();
    void STA();
    void TXS();
    void DEX();
    void BNE();
    void JMP();  // Added Jump Absolute
    void JML();  // Jump Long
    void JSL();  // Added Jump Subroutine Long
    void STZ();  // Store Zero 
    void PHK();  // Push Program Bank
    void PHP();  // Push Processor Status
    void PLP();  // Pop Processor Status
    void TCD();  // Added Transfer C to D
    void TCS();  // Added Transfer C to SP
    void EOR(); 
    void CPX(); 
    void CPY();
    void SBC(); // Add this next to ADC()
void AND();
void ORA();
void BIT();
void CMP();
void BRA(); // Branch Always
    void BEQ(); // Branch if Equal (Z=1)
    void BCC(); // Branch if Carry Clear (C=0)
    void BCS(); // Branch if Carry Set (C=1)
    void BPL(); // Branch if Plus (N=0)
    void BMI(); // Branch if Minus (N=1)
    void BVC(); // Branch if Overflow Clear (V=0)
    void BVS(); // Branch if Overflow Set (V=1)
    // Boot Opcodes
    void SEI();
    void CLC();
    void XCE();
    void REP();
    void SEP();
    

    void buildInstructionTable();
}; 

#endif // CPU_HPP