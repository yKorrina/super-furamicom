#include "bus.hpp"
#include "apu.hpp"
#include "cpu.hpp"
#include <iostream>

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0); 
    nmi_enabled = false;
    wram_address = 0;
}

void Bus::insertCartridge(Cartridge* cart) { cartridge = cart; }

void Bus::setJoypadState(uint16_t pad1, uint16_t pad2) {
    joy1_state = pad1;
    joy2_state = pad2;
}

void Bus::saveSRAM() {
    if (cartridge && cartridge->hasSRAM()) {
        cartridge->saveSRAM(std::string("save.srm"));
    }
}

void Bus::handleAPUWrite(uint8_t port, uint8_t data) {
    if (apu) {
        apu->writePort(port, data);
    }
}

uint8_t Bus::read(uint32_t address) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    if (bank == 0x7E || bank == 0x7F)
        return wram[((bank & 1) << 16) | offset];

    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        if (offset < 0x2000) return wram[offset];
        if (offset >= 0x2100 && offset <= 0x213F) return ppu->readRegister(offset);
        if (offset >= 0x2140 && offset <= 0x217F) {
            return apu ? apu->readPort((offset - 0x2140) & 0x03) : 0x00;
        }
        if (offset == 0x2180) {
            uint8_t val = wram[wram_address % wram.size()];
            wram_address = (wram_address + 1) & 0x01FFFF;
            return val;
        }
        if (offset == 0x4016) return 0x00;
        if (offset == 0x4017) return 0x00;
        if (offset == 0x4210) return ppu->getVBlank() ? 0x82 : 0x02;
        if (offset == 0x4211) return 0x00;
        if (offset == 0x4212) return (ppu->getVBlank() ? 0x80 : 0x00) | 0x01;
        if (offset == 0x4214) return rddiv & 0xFF;
        if (offset == 0x4215) return (rddiv >> 8) & 0xFF;
        if (offset == 0x4216) return rdmpy & 0xFF;
        if (offset == 0x4217) return (rdmpy >> 8) & 0xFF;
        if (offset == 0x4218) return joy1_state & 0xFF;
        if (offset == 0x4219) return (joy1_state >> 8) & 0xFF;
        if (offset == 0x421A) return joy2_state & 0xFF;
        if (offset == 0x421B) return (joy2_state >> 8) & 0xFF;
        if (offset >= 0x421C && offset <= 0x421F) return 0x00;
        if (offset >= 0x4300 && offset <= 0x437F) {
            uint8_t ch = (offset >> 4) & 0x07;
            uint8_t reg = offset & 0x0F;
            switch (reg) {
                case 0x00: return dma[ch].control;
                case 0x01: return dma[ch].dest_reg;
                case 0x02: return dma[ch].src_address & 0xFF;
                case 0x03: return (dma[ch].src_address >> 8) & 0xFF;
                case 0x04: return (dma[ch].src_address >> 16) & 0xFF;
                case 0x05: return dma[ch].size & 0xFF;
                case 0x06: return (dma[ch].size >> 8) & 0xFF;
                default:   return 0x00;
            }
        }
        if (offset >= 0x6000 && offset < 0x8000) {
            if (cartridge && cartridge->isLoaded()) return cartridge->read(address);
            return open_bus;
        }
        if (offset >= 0x2000 && offset < 0x6000) return open_bus;
    }

    if (cartridge && cartridge->isLoaded()) return cartridge->read(address);
    return open_bus;
}

void Bus::write(uint32_t address, uint8_t data) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;
    open_bus = data;

    if (bank == 0x7E || bank == 0x7F) { wram[((bank & 1) << 16) | offset] = data; return; }

    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        if (offset < 0x2000) { wram[offset] = data; return; }
        if (offset >= 0x2100 && offset <= 0x213F) { ppu->writeRegister(offset, data); return; }
        if (offset >= 0x2140 && offset <= 0x217F) { handleAPUWrite((offset - 0x2140) & 0x03, data); return; }
        if (offset == 0x2180) { wram[wram_address % wram.size()] = data; wram_address = (wram_address + 1) & 0x01FFFF; return; }
        if (offset == 0x2181) { wram_address = (wram_address & 0x01FF00) | data; return; }
        if (offset == 0x2182) { wram_address = (wram_address & 0x0100FF) | ((uint32_t)data << 8); return; }
        if (offset == 0x2183) { wram_address = (wram_address & 0x00FFFF) | (((uint32_t)data & 0x01) << 16); return; }
        if (offset == 0x4202) { wrmpya = data; return; }
        if (offset == 0x4203) { rdmpy = (uint16_t)wrmpya * (uint16_t)data; return; }
        if (offset == 0x4204) { wrdivl = (wrdivl & 0xFF00) | data; return; }
        if (offset == 0x4205) { wrdivl = (wrdivl & 0x00FF) | ((uint16_t)data << 8); return; }
        if (offset == 0x4206) {
            wrdivb = data;
            if (wrdivb) { rddiv = wrdivl / wrdivb; rdmpy = wrdivl % wrdivb; }
            else        { rddiv = 0xFFFF; rdmpy = wrdivl; }
            return;
        }
        if (offset == 0x4200) { nmi_enabled = (data & 0x80) != 0; return; }
        if (offset == 0x420B) { executeDMA(data); return; }
        if (offset == 0x420C) { return; }
        if (offset >= 0x4300 && offset <= 0x437F) {
            uint8_t ch = (offset >> 4) & 0x07;
            uint8_t reg = offset & 0x0F;
            switch (reg) {
                case 0x00: dma[ch].control = data; break;
                case 0x01: dma[ch].dest_reg = data; break;
                case 0x02: dma[ch].src_address = (dma[ch].src_address & 0xFFFF00) | data; break;
                case 0x03: dma[ch].src_address = (dma[ch].src_address & 0xFF00FF) | ((uint32_t)data << 8); break;
                case 0x04: dma[ch].src_address = (dma[ch].src_address & 0x00FFFF) | ((uint32_t)data << 16); break;
                case 0x05: dma[ch].size = (dma[ch].size & 0xFF00) | data; break;
                case 0x06: dma[ch].size = (dma[ch].size & 0x00FF) | ((uint16_t)data << 8); break;
            }
            return;
        }
        if (offset >= 0x6000 && offset < 0x8000) {
            if (cartridge && cartridge->isLoaded()) cartridge->write(address, data);
            return;
        }
    }

    if (cartridge && cartridge->isLoaded()) cartridge->write(address, data);
}

// ═══════════════════════════════════════════════════════════════════════════
//  DMA — Fixed bank wrapping
//
//  CRITICAL FIX: On real SNES hardware, during a DMA transfer the A-bus
//  source address only increments/decrements the LOW 16 BITS.  The bank
//  byte stays fixed for the entire transfer.  If the offset wraps from
//  $FFFF it goes to $0000 in the SAME bank, NOT the next bank.
//
//  Our old code was doing dma[i].src_address++ on the full 24-bit value,
//  which would cross bank boundaries and read garbage data from the
//  wrong ROM/RAM bank.  This is THE cause of garbled tile graphics.
// ═══════════════════════════════════════════════════════════════════════════
void Bus::executeDMA(uint8_t channels) {
    for (int i = 0; i < 8; i++) {
        if ((channels & (1 << i)) == 0) continue;
        
        uint8_t mode = dma[i].control & 0x07;
        bool direction = (dma[i].control & 0x80) != 0;
        bool fixed     = (dma[i].control & 0x08) != 0;
        bool decrement = (dma[i].control & 0x10) != 0;
        
        int transfer_size = (dma[i].size == 0) ? 0x10000 : dma[i].size;
        uint16_t bytes_transferred = 0;
        
        // Separate bank and offset — bank stays fixed!
        uint8_t  src_bank   = (dma[i].src_address >> 16) & 0xFF;
        uint16_t src_offset = dma[i].src_address & 0xFFFF;
        
        while (transfer_size > 0) {
            uint16_t b_reg = 0x2100 + dma[i].dest_reg;
            switch (mode) {
                case 0: break;
                case 1: b_reg += (bytes_transferred & 1); break;
                case 2: break;
                case 3: b_reg += ((bytes_transferred >> 1) & 1); break;
                case 4: b_reg += (bytes_transferred & 3); break;
                case 5: b_reg += (bytes_transferred & 1); break;
                default: break;
            }
            
            // Build full address from fixed bank + current offset
            uint32_t src_full = ((uint32_t)src_bank << 16) | src_offset;
            
            if (!direction)
                write(b_reg, read(src_full));
            else
                write(src_full, read(b_reg));
            
            // Only adjust the 16-bit offset, bank stays fixed
            if (!fixed) {
                if (decrement) src_offset--;
                else           src_offset++;
                // src_offset naturally wraps as uint16_t
            }
            
            transfer_size--;
            bytes_transferred++;
        }
        
        // Write back the final address state
        dma[i].src_address = ((uint32_t)src_bank << 16) | src_offset;
        dma[i].size = 0;
    }
}
