#ifndef BUS_HPP
#define BUS_HPP

#include <cstdint>
#include <vector>
#include "cartridge.hpp"
#include "ppu.hpp"

class CPU;
class APU;

struct DMAChannel {
    uint8_t  control;       
    uint8_t  dest_reg;      
    uint32_t src_address;  
    uint16_t size;         
};

class Bus {
public:
    Bus(CPU* c, PPU* p, APU* a);
    
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t data);
    void insertCartridge(Cartridge* cart);

    bool isNMIEnabled() const { return nmi_enabled; }

    void setJoypadState(uint16_t pad1, uint16_t pad2);

private:
    CPU* cpu;
    PPU* ppu;
    APU* apu;
    Cartridge* cartridge = nullptr;
    std::vector<uint8_t> wram;

    DMAChannel dma[8];
    void executeDMA(uint8_t channels);
    
    bool nmi_enabled; 
    
    // ══════════════════════════════════════════════════════════════════════════
    //  APU Port Architecture — how the REAL SNES works:
    //
    //  Ports $2140-$2143 are DUAL-PORTED registers:
    //    CPU WRITE  → apu_cpu_to_spc[0..3]   (SPC700 would read these)
    //    CPU READ   ← apu_spc_to_cpu[0..3]   (SPC700 would write these)
    //
    //  They are NOT the same registers!  A CPU write to $2140 does NOT
    //  change what the CPU reads back from $2140.  On real hardware the
    //  SPC700 controls the read-side values.
    //
    //  Since we don't emulate the SPC700, we run a tiny state machine
    //  that fakes the IPL boot protocol so games can finish their
    //  audio-program upload handshake and move on.
    // ══════════════════════════════════════════════════════════════════════════
    uint8_t apu_spc_to_cpu[4];   // CPU reads from these
    uint8_t apu_cpu_to_spc[4];   // CPU writes to these
    
    // Fake IPL boot protocol state
    enum class IPLState { WAIT_BEGIN, TRANSFERRING, DONE };
    IPLState ipl_state;
    uint8_t  ipl_expected_counter;
    void     handleAPUWrite(uint8_t port, uint8_t data);

    uint16_t joy1_state = 0;
    uint16_t joy2_state = 0;

    uint8_t  wrmpya = 0;
    uint16_t wrdivl = 0;
    uint8_t  wrdivb = 0;
    uint16_t rddiv  = 0;
    uint16_t rdmpy  = 0;

    uint32_t wram_address = 0;
    uint8_t  open_bus = 0;
};

#endif // BUS_HPP
