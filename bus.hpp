#ifndef BUS_HPP
#define BUS_HPP

#include <cstdint>
#include <vector>
#include "cartridge.hpp"

// Forward declarations: Tells the compiler these exist without loading the whole file
class CPU;
class PPU;
class APU;

class Bus {
public:
    Bus(CPU* c, PPU* p, APU* a);
    
    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t data);
    
    void insertCartridge(Cartridge* cart);

private:
    CPU* cpu;
    PPU* ppu;
    APU* apu;
    
    Cartridge* cartridge = nullptr;
    std::vector<uint8_t> wram;
};

#endif // BUS_HPP