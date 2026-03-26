#include "bus.hpp"
#include "cpu.hpp"

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0); 
}

uint8_t Bus::read(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // 1. WRAM (Banks 7E and 7F)
    if (bank == 0x7E || bank == 0x7F) {
        uint32_t wram_addr = ((bank & 1) << 16) | offset;
        return wram[wram_addr];
    }
    
    // 2. WRAM Mirror (First 8KB of Banks 00-3F and 80-BF)
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset < 0x2000) {
        return wram[offset];
    }

    // 3. Hardware I/O Registers (Faking V-Blank to prevent freezing)
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        if (offset == 0x4210) return 0x80; // NMI/V-Blank status flag
        if (offset == 0x4212) return 0x80; // H-Blank/V-Blank status flag
    }

    // 4. Cartridge ROM
    if (cartridge && cartridge->isLoaded()) {
        return cartridge->read(address);
    }

    return 0x00; // Open bus behavior
}

void Bus::write(uint32_t address, uint8_t data) {
    // Placeholder: Address routing logic required here
}
void Bus::insertCartridge(Cartridge* cart) {
    cartridge = cart;
}