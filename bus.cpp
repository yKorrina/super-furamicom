#include "bus.hpp"
#include "cpu.hpp"
#include <iostream>

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0); 
    nmi_enabled = false;
    wram_address = 0;
    
    apu_spc_to_cpu[0] = 0xAA;
    apu_spc_to_cpu[1] = 0xBB;
    apu_spc_to_cpu[2] = 0x00;
    apu_spc_to_cpu[3] = 0x00;
    apu_cpu_to_spc[0] = 0x00;
    apu_cpu_to_spc[1] = 0x00;
    apu_cpu_to_spc[2] = 0x00;
    apu_cpu_to_spc[3] = 0x00;
    ipl_last_port0 = 0;
    ipl_in_transfer = false;
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
    apu_cpu_to_spc[port] = data;

    // The Zelda boot code is still talking to the SPC700 IPL ROM here. Until a
    // real SPC700/APU core exists, treat the four CPU-visible ports as a simple
    // loopback device after the initial $AA/$BB -> $CC handshake so the upload
    // can complete and the CPU can reach video init instead of spinning forever.
    if (!ipl_in_transfer && port == 0 && data == 0xCC) {
        ipl_in_transfer = true;
    }

    if (ipl_in_transfer) {
        apu_spc_to_cpu[port] = data;
        if (port == 0) {
            ipl_last_port0 = data;
        }
    }
}

uint8_t Bus::read(uint32_t address) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // WRAM banks $7E-$7F
    if (bank == 0x7E || bank == 0x7F)
        return wram[((bank & 1) << 16) | offset];

    // System area
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        // WRAM mirror $0000-$1FFF
        if (offset < 0x2000)
            return wram[offset];

        // PPU $2100-$213F
        if (offset >= 0x2100 && offset <= 0x213F)
            return ppu->readRegister(offset);

        // APU $2140-$217F
        if (offset >= 0x2140 && offset <= 0x217F)
            return apu_spc_to_cpu[(offset - 0x2140) & 0x03];

        // WRAM port $2180
        if (offset == 0x2180) {
            uint8_t val = wram[wram_address % wram.size()];
            wram_address = (wram_address + 1) & 0x01FFFF;
            return val;
        }

        // Joypad $4016/$4017
        if (offset == 0x4016) return 0x00;
        if (offset == 0x4017) return 0x00;

        // NMI flag $4210
        if (offset == 0x4210)
            return ppu->getVBlank() ? 0x82 : 0x02;

        // IRQ flag $4211
        if (offset == 0x4211) return 0x00;

        // PPU status $4212
        if (offset == 0x4212)
            return (ppu->getVBlank() ? 0x80 : 0x00) | 0x01;

        // Multiply/divide results
        if (offset == 0x4214) return rddiv & 0xFF;
        if (offset == 0x4215) return (rddiv >> 8) & 0xFF;
        if (offset == 0x4216) return rdmpy & 0xFF;
        if (offset == 0x4217) return (rdmpy >> 8) & 0xFF;

        // Joypad auto-read $4218-$421F
        if (offset == 0x4218) return joy1_state & 0xFF;
        if (offset == 0x4219) return (joy1_state >> 8) & 0xFF;
        if (offset == 0x421A) return joy2_state & 0xFF;
        if (offset == 0x421B) return (joy2_state >> 8) & 0xFF;
        if (offset >= 0x421C && offset <= 0x421F) return 0x00;

        // DMA registers
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

        // SRAM region: pass through to cartridge ($6000-$7FFF in system banks)
        if (offset >= 0x6000 && offset < 0x8000) {
            if (cartridge && cartridge->isLoaded())
                return cartridge->read(address);
            return open_bus;
        }

        // Other I/O returns open bus
        if (offset >= 0x2000 && offset < 0x6000)
            return open_bus;
    }

    // Everything else: cartridge ROM/SRAM
    if (cartridge && cartridge->isLoaded())
        return cartridge->read(address);

    return open_bus;
}

void Bus::write(uint32_t address, uint8_t data) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;
    open_bus = data;

    if (bank == 0x7E || bank == 0x7F) {
        wram[((bank & 1) << 16) | offset] = data;
        return;
    }

    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        if (offset < 0x2000) { wram[offset] = data; return; }

        if (offset >= 0x2100 && offset <= 0x213F) {
            ppu->writeRegister(offset, data);
            return;
        }

        if (offset >= 0x2140 && offset <= 0x217F) {
            handleAPUWrite((offset - 0x2140) & 0x03, data);
            return;
        }

        if (offset == 0x2180) {
            wram[wram_address % wram.size()] = data;
            wram_address = (wram_address + 1) & 0x01FFFF;
            return;
        }
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

        // SRAM write region $6000-$7FFF
        if (offset >= 0x6000 && offset < 0x8000) {
            if (cartridge && cartridge->isLoaded())
                cartridge->write(address, data);
            return;
        }
    }

    // SRAM banks (LoROM $70-$7D, or HiROM $20-$3F:$6000-$7FFF)
    if (cartridge && cartridge->isLoaded()) {
        cartridge->write(address, data);
    }
}

void Bus::executeDMA(uint8_t channels) {
    for (int i = 0; i < 8; i++) {
        if ((channels & (1 << i)) == 0) continue;
        
        uint8_t mode = dma[i].control & 0x07;
        bool direction = (dma[i].control & 0x80) != 0;
        bool fixed     = (dma[i].control & 0x08) != 0;
        bool decrement = (dma[i].control & 0x10) != 0;
        
        int transfer_size = (dma[i].size == 0) ? 0x10000 : dma[i].size;
        uint16_t bytes_transferred = 0;
        
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
            
            if (!direction)
                write(b_reg, read(dma[i].src_address));
            else
                write(dma[i].src_address, read(b_reg));
            
            if (!fixed) {
                if (decrement) dma[i].src_address--;
                else           dma[i].src_address++;
            }
            
            transfer_size--;
            bytes_transferred++;
        }
        dma[i].size = 0;
    }
}
