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

uint16_t Bus::encodeJoypadState(uint16_t state) {
    uint16_t encoded = 0;
    if (state & 0x8000) encoded |= 0x8000; // B
    if (state & 0x4000) encoded |= 0x4000; // Y
    if (state & 0x2000) encoded |= 0x2000; // Select
    if (state & 0x1000) encoded |= 0x1000; // Start
    if (state & 0x0008) encoded |= 0x0800; // Up
    if (state & 0x0004) encoded |= 0x0400; // Down
    if (state & 0x0002) encoded |= 0x0200; // Left
    if (state & 0x0001) encoded |= 0x0100; // Right
    if (state & 0x0080) encoded |= 0x0080; // A
    if (state & 0x0040) encoded |= 0x0040; // X
    if (state & 0x0800) encoded |= 0x0020; // L
    if (state & 0x0400) encoded |= 0x0010; // R
    return encoded;
}

void Bus::latchJoypads() {
    joy1_shift = encodeJoypadState(joy1_state);
    joy2_shift = encodeJoypadState(joy2_state);
}

uint8_t Bus::readJoypadSerial(int port) {
    const uint16_t live = encodeJoypadState(port == 0 ? joy1_state : joy2_state);
    uint16_t& shift = (port == 0) ? joy1_shift : joy2_shift;
    if (joypad_strobe) {
        return (uint8_t)((live >> 15) & 0x01);
    }

    const uint8_t bit = (uint8_t)((shift >> 15) & 0x01);
    shift = (uint16_t)((shift << 1) | 0x0001);
    return bit;
}

void Bus::beginFrame() {
    irq_fired_this_scanline = false;
    for (int i = 0; i < 8; i++) {
        if (hdma_enable & (1 << i)) {
            initializeHDMAChannel(i);
            if (!dma[i].hdma_terminated && dma[i].hdma_do_transfer) {
                transferHDMAChannel(i);
                dma[i].hdma_do_transfer = (dma[i].line_counter & 0x80) != 0;
            }
        } else {
            dma[i].hdma_do_transfer = false;
            dma[i].hdma_terminated = true;
            dma[i].line_counter = 0;
        }
    }
}

void Bus::startVBlank() {
    nmi_pending = true;
    if (nmitimen & 0x01) {
        auto_joypad_busy = true;
        joy1_auto_read = encodeJoypadState(joy1_state);
        joy2_auto_read = encodeJoypadState(joy2_state);
        auto_joypad_busy = false;
    }
    auto_joypad_busy = false;
}

void Bus::endVBlank() {
    nmi_pending = false;
    auto_joypad_busy = false;
}

void Bus::beginScanline(int scanline) {
    irq_fired_this_scanline = false;
    const uint8_t irq_mode = (nmitimen >> 4) & 0x03;

    if ((irq_mode == 2 || irq_mode == 3) && scanline == (int)(vtime & 0x01FF) && (htime & 0x01FF) == 0) {
        irq_pending = true;
        irq_fired_this_scanline = true;
    }
}

void Bus::beginHBlank(int scanline) {
    if (irq_fired_this_scanline) return;

    const uint8_t irq_mode = (nmitimen >> 4) & 0x03;
    switch (irq_mode) {
    case 1:
        irq_pending = true;
        break;
    case 3:
        if (scanline == (int)(vtime & 0x01FF)) {
            irq_pending = true;
        }
        break;
    default:
        break;
    }

    if (irq_pending) {
        irq_fired_this_scanline = true;
    }
}

bool Bus::consumeNMI() {
    if (!nmi_enabled || !nmi_pending) return false;
    nmi_pending = false;
    return true;
}

bool Bus::consumeIRQ() {
    if (!irq_pending) return false;
    irq_pending = false;
    return true;
}

void Bus::initializeHDMAChannel(int channel) {
    dma[channel].table_address = (uint16_t)(dma[channel].src_address & 0xFFFF);
    dma[channel].line_counter = 0;
    dma[channel].hdma_do_transfer = false;
    dma[channel].hdma_terminated = false;
    reloadHDMALineCounter(channel);
}

void Bus::reloadHDMALineCounter(int channel) {
    DMAChannel& ch = dma[channel];
    const uint8_t table_bank = (uint8_t)((ch.src_address >> 16) & 0xFF);
    const uint32_t base = ((uint32_t)table_bank << 16) | ch.table_address;
    ch.line_counter = read(base);
    ch.table_address++;

    if (ch.line_counter == 0) {
        ch.hdma_terminated = true;
        ch.hdma_do_transfer = false;
        return;
    }

    if (ch.control & 0x40) {
        const uint32_t indirect_base = ((uint32_t)table_bank << 16) | ch.table_address;
        const uint8_t low = read(indirect_base);
        const uint8_t high = read(((uint32_t)table_bank << 16) | (uint16_t)(ch.table_address + 1));
        ch.size = (uint16_t)low | ((uint16_t)high << 8);
        ch.table_address += 2;
    }

    ch.hdma_do_transfer = true;
}

void Bus::transferHDMAChannel(int channel) {
    static const uint8_t kBytesPerMode[8] = {1, 2, 2, 4, 4, 4, 2, 4};

    DMAChannel& ch = dma[channel];
    const uint8_t mode = ch.control & 0x07;
    const uint8_t byte_count = kBytesPerMode[mode];
    const bool direction = (ch.control & 0x80) != 0;
    const bool indirect = (ch.control & 0x40) != 0;
    const uint8_t table_bank = (uint8_t)((ch.src_address >> 16) & 0xFF);

    for (uint8_t byte_index = 0; byte_index < byte_count; byte_index++) {
        uint16_t b_reg = 0x2100 + ch.dest_reg;
        switch (mode) {
        case 0: break;
        case 1: b_reg += (byte_index & 1); break;
        case 2: break;
        case 3: b_reg += ((byte_index >> 1) & 1); break;
        case 4: b_reg += (byte_index & 3); break;
        case 5: b_reg += (byte_index & 1); break;
        case 6: break;
        case 7: b_reg += ((byte_index >> 1) & 1); break;
        default: break;
        }

        uint32_t src_full;
        if (indirect) {
            src_full = ((uint32_t)ch.indirect_bank << 16) | ch.size;
            ch.size++;
        } else {
            src_full = ((uint32_t)table_bank << 16) | ch.table_address;
            ch.table_address++;
        }

        if (!direction) write(b_reg, read(src_full));
        else write(src_full, read(b_reg));
    }
}

void Bus::stepHDMA() {
    for (int i = 0; i < 8; i++) {
        if ((hdma_enable & (1 << i)) == 0) continue;

        DMAChannel& ch = dma[i];
        if (ch.hdma_terminated) continue;

        const bool repeat_mode = (ch.line_counter & 0x80) != 0;
        if (ch.hdma_do_transfer) transferHDMAChannel(i);

        uint8_t remaining = ch.line_counter & 0x7F;
        if (remaining > 0) remaining--;

        if (remaining == 0) {
            reloadHDMALineCounter(i);
        } else {
            ch.line_counter = (uint8_t)((repeat_mode ? 0x80 : 0x00) | remaining);
            ch.hdma_do_transfer = repeat_mode;
        }
    }
}

void Bus::setJoypadState(uint16_t pad1, uint16_t pad2) {
    joy1_state = pad1;
    joy2_state = pad2;
    joy1_auto_read = encodeJoypadState(joy1_state);
    joy2_auto_read = encodeJoypadState(joy2_state);
    if (joypad_strobe) {
        latchJoypads();
    }
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
        if (offset == 0x4016) {
            joy4016_reads++;
            return (uint8_t)((open_bus & 0xFC) | readJoypadSerial(0));
        }
        if (offset == 0x4017) {
            joy4017_reads++;
            return (uint8_t)(0x1C | readJoypadSerial(1));
        }
        if (offset == 0x4210) {
            const uint8_t value = (nmi_pending ? 0x80 : 0x00) | 0x02;
            nmi_pending = false;
            return value;
        }
        if (offset == 0x4211) {
            const uint8_t value = irq_pending ? 0x80 : 0x00;
            irq_pending = false;
            return value;
        }
        if (offset == 0x4212) {
            return (ppu->getVBlank() ? 0x80 : 0x00) |
                   (ppu->getHBlank() ? 0x40 : 0x00) |
                   (auto_joypad_busy ? 0x01 : 0x00);
        }
        if (offset == 0x4214) return rddiv & 0xFF;
        if (offset == 0x4215) return (rddiv >> 8) & 0xFF;
        if (offset == 0x4216) return rdmpy & 0xFF;
        if (offset == 0x4217) return (rdmpy >> 8) & 0xFF;
        if (offset == 0x4218) { joy4218_reads++; return joy1_auto_read & 0xFF; }
        if (offset == 0x4219) { joy4219_reads++; return (joy1_auto_read >> 8) & 0xFF; }
        if (offset == 0x421A) { joy421A_reads++; return joy2_auto_read & 0xFF; }
        if (offset == 0x421B) { joy421B_reads++; return (joy2_auto_read >> 8) & 0xFF; }
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
                case 0x07: return dma[ch].indirect_bank;
                case 0x08: return dma[ch].table_address & 0xFF;
                case 0x09: return (dma[ch].table_address >> 8) & 0xFF;
                case 0x0A: return dma[ch].line_counter;
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
        if (offset == 0x4200) {
            nmitimen = data;
            nmi_enabled = (data & 0x80) != 0;
            if ((data & 0x30) == 0) {
                irq_pending = false;
            }
            return;
        }
        if (offset == 0x4016) {
            joy4016_writes++;
            last_joy4016_write = data;
            const bool new_strobe = (data & 0x01) != 0;
            if (new_strobe || (joypad_strobe && !new_strobe)) {
                latchJoypads();
            }
            joypad_strobe = new_strobe;
            return;
        }
        if (offset == 0x4207) { htime = (htime & 0x0100) | data; return; }
        if (offset == 0x4208) { htime = (htime & 0x00FF) | (((uint16_t)data & 0x01) << 8); return; }
        if (offset == 0x4209) { vtime = (vtime & 0x0100) | data; return; }
        if (offset == 0x420A) { vtime = (vtime & 0x00FF) | (((uint16_t)data & 0x01) << 8); return; }
        if (offset == 0x420B) { executeDMA(data); return; }
        if (offset == 0x420C) { hdma_enable = data; return; }
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
                case 0x07: dma[ch].indirect_bank = data; break;
                case 0x08: dma[ch].table_address = (dma[ch].table_address & 0xFF00) | data; break;
                case 0x09: dma[ch].table_address = (dma[ch].table_address & 0x00FF) | ((uint16_t)data << 8); break;
                case 0x0A: dma[ch].line_counter = data; break;
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
                case 6: break;
                case 7: b_reg += ((bytes_transferred >> 1) & 1); break;
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
