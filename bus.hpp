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

    // Save SRAM to disk
    void saveSRAM();

private:
    CPU* cpu;
    PPU* ppu;
    APU* apu;
    Cartridge* cartridge = nullptr;
    std::vector<uint8_t> wram;

    DMAChannel dma[8];
    void executeDMA(uint8_t channels);
    
    bool nmi_enabled; 
    
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

#endif
