#include "bus.hpp"
#include "cpu.hpp"
#include <iostream>

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0); 
    nmi_enabled = false;
    
    // Fake APU boot sequence starting values
    apu_ports[0] = 0xAA;
    apu_ports[1] = 0xBB;
    apu_ports[2] = 0x00;
    apu_ports[3] = 0x00;
}

void Bus::insertCartridge(Cartridge* cart) { cartridge = cart; }

uint8_t Bus::read(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // 1. WRAM Access
    if (bank == 0x7E || bank == 0x7F) return wram[((bank & 1) << 16) | offset];
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset < 0x2000) return wram[offset];

    // 2. Hardware Registers
    if (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) {
        if (offset >= 0x2100 && offset <= 0x213F) return ppu->readRegister(offset);
        
        // APU Port Echo (Allows the handshake to complete)
        if (offset >= 0x2140 && offset <= 0x2143) {
            return apu_ports[offset - 0x2140];
        }
        
        // Universal Hardware Polling Bypass
        if (offset >= 0x4200 && offset <= 0x437F) {
            static uint8_t hw_toggle = 0;
            hw_toggle += 0x13; 
            return hw_toggle;
        }
    }

    if (cartridge && cartridge->isLoaded()) return cartridge->read(address);
    return 0x00;
}

void Bus::write(uint32_t address, uint8_t data) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    if (bank == 0x7E || bank == 0x7F) { wram[((bank & 1) << 16) | offset] = data; return; }
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset < 0x2000) { wram[offset] = data; return; }

    if (bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) {
        if (offset >= 0x2100 && offset <= 0x213F) { ppu->writeRegister(offset, data); return; }

        // Echo writes back to the fake APU ports
        if (offset >= 0x2140 && offset <= 0x2143) {
            apu_ports[offset - 0x2140] = data;
            return;
        }

        if (offset == 0x4200) { nmi_enabled = (data & 0x80) != 0; return; }
        if (offset == 0x420B) { executeDMA(data); return; }

        if (offset >= 0x4300 && offset <= 0x437F) {
            uint8_t channel = (offset >> 4) & 0x07;
            uint8_t reg = offset & 0x0F;
            switch (reg) {
                case 0x00: dma[channel].control = data; break;
                case 0x01: dma[channel].dest_reg = data; break;
                
                // CRITICAL FIX: 24-bit Address Masks (Prevents stripping the bank ID)
                case 0x02: dma[channel].src_address = (dma[channel].src_address & 0xFFFF00) | data; break;
                case 0x03: dma[channel].src_address = (dma[channel].src_address & 0xFF00FF) | (data << 8); break;
                case 0x04: dma[channel].src_address = (dma[channel].src_address & 0x00FFFF) | (data << 16); break;
                
                case 0x05: dma[channel].size = (dma[channel].size & 0xFF00) | data; break;
                case 0x06: dma[channel].size = (dma[channel].size & 0x00FF) | (data << 8); break;
            }
            return;
        }
    }
}

void Bus::executeDMA(uint8_t channels) {
    for (int i = 0; i < 8; i++) {
        if ((channels & (1 << i)) != 0) {
            uint8_t mode = dma[i].control & 0x07;
            uint16_t bytes_transferred = 0;
            
            // SNES Quirks: Transferring "0" bytes actually means transferring 65,536 bytes
            int transfer_size = (dma[i].size == 0) ? 0x10000 : dma[i].size;
            
            std::cout << "[DMA] Ch " << i << " -> Reg 0x21" << std::hex << (int)dma[i].dest_reg 
                      << " | Src: " << dma[i].src_address << " | Size: " << std::dec << transfer_size << "\n";
            
            while (transfer_size > 0) {
                uint8_t byte = read(dma[i].src_address);
                uint16_t target_reg = 0x2100 + dma[i].dest_reg;
                
                if (mode == 1) target_reg += (bytes_transferred % 2); 
                
                write(target_reg, byte);
                
                dma[i].src_address++;
                transfer_size--;
                bytes_transferred++;
            }
            dma[i].size = 0; // Clear the size register after transfer
        }
    }
}