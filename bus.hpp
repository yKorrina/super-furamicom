#ifndef BUS_HPP
#define BUS_HPP

#include <cstdint>
#include <vector>
#include "cartridge.hpp"
#include "ppu.hpp"

class CPU;
class APU;

struct DMAChannel {
    uint8_t control;       
    uint8_t dest_reg;      
    uint32_t src_address;  
    uint16_t size;         
};

class Bus {
public:
    Bus(CPU* c, PPU* p, APU* a);
    
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t data);
    void insertCartridge(Cartridge* cart);

    // Let the system check if the game has enabled interrupts
    bool isNMIEnabled() const { return nmi_enabled; }

private:
    CPU* cpu;
    PPU* ppu;
    APU* apu;
    Cartridge* cartridge = nullptr;
    std::vector<uint8_t> wram;

    DMAChannel dma[8];
    void executeDMA(uint8_t channels);
    
    bool nmi_enabled; 
    
    // Fake Audio Chip memory to satisfy the boot handshake
    uint8_t apu_ports[4]; 
};

#endif // BUS_HPP