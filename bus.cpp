#include "bus.hpp"
#include "cpu.hpp"
#include <iostream>

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0); 
    nmi_enabled = false;
    wram_address = 0;
    
    // ── SPC700 boot signature ────────────────────────────────────────────────
    // The SPC700's IPL ROM puts $AA on read-port 0 and $BB on read-port 1
    // to tell the CPU "I'm alive and ready for upload."
    apu_spc_to_cpu[0] = 0xAA;   // ← CPU reads this from $2140
    apu_spc_to_cpu[1] = 0xBB;   // ← CPU reads this from $2141
    apu_spc_to_cpu[2] = 0x00;
    apu_spc_to_cpu[3] = 0x00;

    apu_cpu_to_spc[0] = 0x00;   // CPU write side (SPC700 would read these)
    apu_cpu_to_spc[1] = 0x00;
    apu_cpu_to_spc[2] = 0x00;
    apu_cpu_to_spc[3] = 0x00;

    ipl_state = IPLState::WAIT_BEGIN;
    ipl_expected_counter = 0xCC;
}

void Bus::insertCartridge(Cartridge* cart) { cartridge = cart; }

void Bus::setJoypadState(uint16_t pad1, uint16_t pad2) {
    joy1_state = pad1;
    joy2_state = pad2;
}

// ─────────────────────────────────────────────────────────────────────────────
//  Fake SPC700 IPL Boot Protocol
//
//  The real IPL ROM protocol works like this:
//
//  PHASE 1 — HANDSHAKE:
//    SPC700 puts $AA in port 0, $BB in port 1.
//    CPU reads $2140 until it sees $AA, then $2141 until it sees $BB.
//
//  PHASE 2 — BEGIN TRANSFER:
//    CPU writes the destination address to $2142/$2143.
//    CPU writes $CC to $2141 (start signal).
//    CPU writes $CC to $2140 (trigger).
//    SPC700 acknowledges by putting $CC in its port 0 (CPU reads $2140 = $CC).
//
//  PHASE 3 — DATA TRANSFER:
//    For each byte:
//      CPU writes data to $2141.
//      CPU writes counter (starting at $00, incrementing) to $2140.
//      SPC700 acknowledges by echoing the counter to port 0.
//
//  PHASE 4 — END TRANSFER / START ANOTHER:
//    CPU writes new destination to $2142/$2143.
//    CPU writes counter+2 (or same) to $2140 with non-zero in $2141 for
//    "start new block" or $2141=0 for "begin execution."
//
//  We fake this by watching what the CPU writes to port 0 ($2140) and
//  immediately echoing it back to the read side.
// ─────────────────────────────────────────────────────────────────────────────

void Bus::handleAPUWrite(uint8_t port, uint8_t data) {
    apu_cpu_to_spc[port] = data;

    switch (ipl_state) {
    case IPLState::WAIT_BEGIN:
        // Waiting for the CPU to write $CC to port 0 (begin transfer signal)
        if (port == 0 && data == 0xCC) {
            // Acknowledge: echo $CC back on the read side
            apu_spc_to_cpu[0] = 0xCC;
            ipl_state = IPLState::TRANSFERRING;
            ipl_expected_counter = 0x00;
        }
        break;

    case IPLState::TRANSFERRING:
        if (port == 0) {
            // The CPU wrote a counter value to port 0.
            // Check if this is a "new block" signal (counter differs by >=2
            // from expected) or a normal data byte acknowledge.
            
            // If it's the next expected counter, or any counter really,
            // just echo it back — the SPC700 does this after receiving the byte.
            apu_spc_to_cpu[0] = data;
            
            // Track the counter for debugging, but always echo
            ipl_expected_counter = (data + 1) & 0xFF;
            
            // If the CPU wrote a value that would restart a new block
            // (counter jumps), that's fine — we just keep echoing.
            // If port 1 was 0 on the last "block end" write, the real 
            // SPC700 would jump to the uploaded code. Since we have no
            // code to execute, we just stay in TRANSFERRING and keep 
            // echoing. The game will eventually give up or move on.
        }
        break;

    case IPLState::DONE:
        // If something writes again, just echo port 0
        if (port == 0) {
            apu_spc_to_cpu[0] = data;
        }
        break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  READ
// ─────────────────────────────────────────────────────────────────────────────
uint8_t Bus::read(uint32_t address) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // ── 1. WRAM mirror banks ────────────────────────────────────────────────
    if (bank == 0x7E || bank == 0x7F) {
        return wram[((bank & 1) << 16) | offset];
    }

    // ── 2. System area (banks $00-$3F and $80-$BF) ──────────────────────────
    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        // WRAM mirror $0000-$1FFF
        if (offset < 0x2000) {
            return wram[offset];
        }

        // PPU Registers $2100-$213F
        if (offset >= 0x2100 && offset <= 0x213F) {
            return ppu->readRegister(offset);
        }

        // ── APU Ports $2140-$2143 — READ from SPC→CPU side ──────────────────
        if (offset >= 0x2140 && offset <= 0x2143) {
            return apu_spc_to_cpu[offset - 0x2140];
        }
        // APU port mirrors ($2144-$217F mirror $2140-$2143)
        if (offset >= 0x2144 && offset <= 0x217F) {
            return apu_spc_to_cpu[(offset - 0x2140) & 0x03];
        }

        // WRAM access port $2180
        if (offset == 0x2180) {
            uint8_t val = wram[wram_address % wram.size()];
            wram_address = (wram_address + 1) & 0x01FFFF;
            return val;
        }

        // Old-style joypad $4016/$4017
        if (offset == 0x4016) return 0x00;
        if (offset == 0x4017) return 0x00;

        // NMI Flag $4210
        if (offset == 0x4210) {
            uint8_t val = ppu->getVBlank() ? 0x82 : 0x02;
            return val;
        }

        // IRQ Flag $4211
        if (offset == 0x4211) return 0x00;

        // PPU Status $4212
        if (offset == 0x4212) {
            uint8_t val = 0;
            if (ppu->getVBlank()) val |= 0x80;
            val |= 0x01;  // auto-joypad complete
            return val;
        }

        // Multiply / Divide results
        if (offset == 0x4214) return rddiv & 0xFF;
        if (offset == 0x4215) return (rddiv >> 8) & 0xFF;
        if (offset == 0x4216) return rdmpy & 0xFF;
        if (offset == 0x4217) return (rdmpy >> 8) & 0xFF;

        // Auto-read joypad $4218-$421F
        if (offset == 0x4218) return joy1_state & 0xFF;
        if (offset == 0x4219) return (joy1_state >> 8) & 0xFF;
        if (offset == 0x421A) return joy2_state & 0xFF;
        if (offset == 0x421B) return (joy2_state >> 8) & 0xFF;
        if (offset >= 0x421C && offset <= 0x421F) return 0x00;

        // DMA register reads
        if (offset >= 0x4300 && offset <= 0x437F) {
            uint8_t channel = (offset >> 4) & 0x07;
            uint8_t reg = offset & 0x0F;
            switch (reg) {
                case 0x00: return dma[channel].control;
                case 0x01: return dma[channel].dest_reg;
                case 0x02: return dma[channel].src_address & 0xFF;
                case 0x03: return (dma[channel].src_address >> 8) & 0xFF;
                case 0x04: return (dma[channel].src_address >> 16) & 0xFF;
                case 0x05: return dma[channel].size & 0xFF;
                case 0x06: return (dma[channel].size >> 8) & 0xFF;
                default:   return 0x00;
            }
        }

        // Everything else in I/O area → open bus
        if (offset >= 0x2000 && offset < 0x8000) {
            return open_bus;
        }
    }

    // ── 3. Cartridge ROM ─────────────────────────────────────────────────────
    if (cartridge && cartridge->isLoaded()) {
        return cartridge->read(address);
    }

    return open_bus;
}

// ─────────────────────────────────────────────────────────────────────────────
//  WRITE
// ─────────────────────────────────────────────────────────────────────────────
void Bus::write(uint32_t address, uint8_t data) {
    uint8_t bank   = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    open_bus = data;

    // WRAM banks
    if (bank == 0x7E || bank == 0x7F) {
        wram[((bank & 1) << 16) | offset] = data;
        return;
    }

    if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF))) {
        // WRAM mirror
        if (offset < 0x2000) { wram[offset] = data; return; }

        // PPU registers
        if (offset >= 0x2100 && offset <= 0x213F) {
            ppu->writeRegister(offset, data);
            return;
        }

        // ── APU Ports $2140-$2143 — WRITE to CPU→SPC side ───────────────────
        if (offset >= 0x2140 && offset <= 0x2143) {
            handleAPUWrite(offset - 0x2140, data);
            return;
        }
        // APU port mirrors
        if (offset >= 0x2144 && offset <= 0x217F) {
            handleAPUWrite((offset - 0x2140) & 0x03, data);
            return;
        }

        // WRAM access port
        if (offset == 0x2180) {
            wram[wram_address % wram.size()] = data;
            wram_address = (wram_address + 1) & 0x01FFFF;
            return;
        }
        if (offset == 0x2181) { wram_address = (wram_address & 0x01FF00) | data; return; }
        if (offset == 0x2182) { wram_address = (wram_address & 0x0100FF) | ((uint32_t)data << 8); return; }
        if (offset == 0x2183) { wram_address = (wram_address & 0x00FFFF) | (((uint32_t)data & 0x01) << 16); return; }

        // Hardware multiply $4202-$4206
        if (offset == 0x4202) { wrmpya = data; return; }
        if (offset == 0x4203) {
            rdmpy = (uint16_t)wrmpya * (uint16_t)data;
            return;
        }
        if (offset == 0x4204) { wrdivl = (wrdivl & 0xFF00) | data; return; }
        if (offset == 0x4205) { wrdivl = (wrdivl & 0x00FF) | ((uint16_t)data << 8); return; }
        if (offset == 0x4206) {
            wrdivb = data;
            if (wrdivb) {
                rddiv = wrdivl / wrdivb;
                rdmpy = wrdivl % wrdivb;
            } else {
                rddiv = 0xFFFF;
                rdmpy = wrdivl;
            }
            return;
        }

        // NMI / IRQ enable
        if (offset == 0x4200) { nmi_enabled = (data & 0x80) != 0; return; }

        // DMA trigger
        if (offset == 0x420B) { executeDMA(data); return; }
        // HDMA trigger (stub)
        if (offset == 0x420C) { return; }

        // DMA channel registers
        if (offset >= 0x4300 && offset <= 0x437F) {
            uint8_t channel = (offset >> 4) & 0x07;
            uint8_t reg = offset & 0x0F;
            switch (reg) {
                case 0x00: dma[channel].control = data; break;
                case 0x01: dma[channel].dest_reg = data; break;
                case 0x02: dma[channel].src_address = (dma[channel].src_address & 0xFFFF00) | data; break;
                case 0x03: dma[channel].src_address = (dma[channel].src_address & 0xFF00FF) | ((uint32_t)data << 8); break;
                case 0x04: dma[channel].src_address = (dma[channel].src_address & 0x00FFFF) | ((uint32_t)data << 16); break;
                case 0x05: dma[channel].size = (dma[channel].size & 0xFF00) | data; break;
                case 0x06: dma[channel].size = (dma[channel].size & 0x00FF) | ((uint16_t)data << 8); break;
            }
            return;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
//  DMA
// ─────────────────────────────────────────────────────────────────────────────
void Bus::executeDMA(uint8_t channels) {
    for (int i = 0; i < 8; i++) {
        if ((channels & (1 << i)) == 0) continue;
        
        uint8_t mode = dma[i].control & 0x07;
        bool    direction = (dma[i].control & 0x80) != 0;
        bool    fixed     = (dma[i].control & 0x08) != 0;
        bool    decrement = (dma[i].control & 0x10) != 0;
        
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
            
            if (!direction) {
                uint8_t byte = read(dma[i].src_address);
                write(b_reg, byte);
            } else {
                uint8_t byte = read(b_reg);
                write(dma[i].src_address, byte);
            }
            
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
