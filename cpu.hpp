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
    
    void nmi(); 

private:
    Bus* bus;

    uint16_t A, X, Y, SP, PC, D; 
    uint8_t PB, DB, P;
    bool E; 

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
    void IMP();   
    void ACC();   // Accumulator Mode
    void AL();   
    void IMM8();  
    void IMM16(); 
    void ABS();   
    void ABSX();  
    void DP();    
    void DPX();   
    void IMM_M(); 
    void IMM_X(); 
    void REL();   

    // Opcodes
    void INC(); void DEC(); void INA(); void DEA(); 
    void JSR(); void RTS(); void RTL(); void RTI(); 
    void LDA(); void ADC(); void NOP(); void LDX(); void LDY();
    void STA(); void TXS(); void DEX(); void BNE();
    void JMP(); void JML(); void JSL(); void STZ();  
    void PHK(); void PHP(); void PLP(); void PHB(); void PLB();  
    void TCD(); void TCS(); void EOR(); void CPX(); void CPY();
    void SBC(); void AND(); void ORA(); void BIT(); void CMP();
    void BRA(); void BEQ(); void BCC(); void BCS(); 
    void BPL(); void BMI(); void BVC(); void BVS(); 
    void XBA(); 

    // New Transfers & Shifts
    void TAX(); void TAY(); void TXA(); void TYA(); void TSX();
    void ASL_A(); void LSR_A();

    // Boot Opcodes
    void SEI(); void CLC(); void XCE(); void REP(); void SEP();
    
    void buildInstructionTable();
}; 

#endif // CPU_HPP