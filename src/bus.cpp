#include "bus.hpp"
#include "apu.hpp"
#include "cpu.hpp"
#include "sa1.hpp"
#include <iostream>

namespace {
constexpr int kAutoJoypadBusyCpuCycles = 352;
}

Bus::Bus(CPU* c, PPU* p, APU* a) : cpu(c), ppu(p), apu(a) {
    cpu->connectBus(this);
    wram.resize(128 * 1024, 0);
    nmi_enabled = false;
    wram_address = 0;
    resetDebugHistory();
}

void Bus::insertCartridge(Cartridge* cart) { cartridge = cart; }

uint16_t Bus::encodeJoypadState(uint16_t state) {
    uint16_t encoded = 0;
    if (state & 0x8000) encoded |= 0x8000;
    if (state & 0x4000) encoded |= 0x4000;
    if (state & 0x2000) encoded |= 0x2000;
    if (state & 0x1000) encoded |= 0x1000;
    if (state & 0x0008) encoded |= 0x0800;
    if (state & 0x0004) encoded |= 0x0400;
    if (state & 0x0002) encoded |= 0x0200;
    if (state & 0x0001) encoded |= 0x0100;
    if (state & 0x0080) encoded |= 0x0080;
    if (state & 0x0040) encoded |= 0x0040;
    if (state & 0x0800) encoded |= 0x0020;
    if (state & 0x0400) encoded |= 0x0010;
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
                dma[i].hdma_do_transfer = !dma[i].hdma_repeat;
            }
        } else {
            dma[i].hdma_repeat = false;
            dma[i].hdma_do_transfer = false;
            dma[i].hdma_terminated = true;
            dma[i].line_counter = 0;
        }
    }
}

void Bus::startVBlank() {
    vblank_nmi_flag = true;
    nmi_request_pending = nmi_enabled;
    if (nmitimen & 0x01) {
        auto_joypad_busy = true;
        auto_joypad_cycles_remaining = kAutoJoypadBusyCpuCycles;
        joy1_auto_read_latch = encodeJoypadState(joy1_state);
        joy2_auto_read_latch = encodeJoypadState(joy2_state);
        joy1_shift = joy1_auto_read_latch;
        joy2_shift = joy2_auto_read_latch;
    } else {
        auto_joypad_busy = false;
        auto_joypad_cycles_remaining = 0;
    }
}

void Bus::endVBlank() {
    vblank_nmi_flag = false;
    auto_joypad_busy = false;
    auto_joypad_cycles_remaining = 0;
}

void Bus::tick(int cpu_cycles) {
    if (cartridge && cartridge->isLoaded()) {
        cartridge->tickCoprocessors(cpu_cycles);
        if (cartridge->consumeCoprocessorIRQ()) {
            irq_flag = true;
            irq_request_pending = true;
        }
    }

    if (cpu_cycles <= 0 || !auto_joypad_busy) return;

    auto_joypad_cycles_remaining -= cpu_cycles;
    if (auto_joypad_cycles_remaining <= 0) {
        auto_joypad_busy = false;
        auto_joypad_cycles_remaining = 0;
        joy1_auto_read = joy1_auto_read_latch;
        joy2_auto_read = joy2_auto_read_latch;
        joy1_shift = 0xFFFF;
        joy2_shift = 0xFFFF;
    }
}

void Bus::beginScanline(int scanline) {
    irq_fired_this_scanline = false;
    const uint8_t irq_mode = (nmitimen >> 4) & 0x03;

    if ((irq_mode == 2 || irq_mode == 3) && scanline == (int)(vtime & 0x01FF) && (htime & 0x01FF) == 0) {
        irq_flag = true;
        irq_request_pending = true;
        irq_fired_this_scanline = true;
    }
}

void Bus::beginHBlank(int scanline) {
    if (irq_fired_this_scanline) return;

    const uint8_t irq_mode = (nmitimen >> 4) & 0x03;
    switch (irq_mode) {
    case 1:
        irq_flag = true;
        irq_request_pending = true;
        break;
    case 3:
        if (scanline == (int)(vtime & 0x01FF)) {
            irq_flag = true;
            irq_request_pending = true;
        }
        break;
    default:
        break;
    }

    if (irq_request_pending) {
        irq_fired_this_scanline = true;
    }
}

bool Bus::consumeNMI() {
    if (!nmi_request_pending) return false;
    nmi_request_pending = false;
    return true;
}

bool Bus::consumeIRQ() {
    if (!irq_request_pending) return false;
    irq_request_pending = false;
    return true;
}

void Bus::initializeHDMAChannel(int channel) {
    dma[channel].table_address = (uint16_t)(dma[channel].src_address & 0xFFFF);
    dma[channel].line_counter = 0;
    dma[channel].hdma_repeat = false;
    dma[channel].hdma_do_transfer = false;
    dma[channel].hdma_terminated = false;
    reloadHDMALineCounter(channel);
}

void Bus::reloadHDMALineCounter(int channel) {
    DMAChannel& ch = dma[channel];
    const uint8_t table_bank = (uint8_t)((ch.src_address >> 16) & 0xFF);
    const uint32_t base = ((uint32_t)table_bank << 16) | ch.table_address;
    const uint8_t raw_line_counter = read(base);
    ch.table_address++;

    if (raw_line_counter == 0) {
        if (ch.control & 0x40) {
            ch.table_address += 2;
        }
        ch.hdma_terminated = true;
        ch.hdma_repeat = false;
        ch.hdma_do_transfer = false;
        ch.line_counter = 0;
        return;
    }

    ch.hdma_repeat = (raw_line_counter & 0x80) != 0;
    ch.line_counter = (uint8_t)(raw_line_counter & 0x7F);
    if (ch.line_counter == 0) {
        ch.line_counter = 128;
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

        if (ch.hdma_do_transfer) {
            transferHDMAChannel(i);
        }

        if (ch.line_counter > 0) {
            ch.line_counter--;
        }

        if (ch.line_counter == 0) {
            reloadHDMALineCounter(i);
        } else {
            ch.hdma_do_transfer = !ch.hdma_repeat;
        }
    }
}

void Bus::setJoypadState(uint16_t pad1, uint16_t pad2) {
    joy1_state = pad1;
    joy2_state = pad2;
    joy1_auto_read_latch = encodeJoypadState(joy1_state);
    joy2_auto_read_latch = encodeJoypadState(joy2_state);
    if (joypad_strobe) {
        latchJoypads();
    }
}

void Bus::saveSRAM() {
    if (cartridge && cartridge->hasSRAM()) {
        cartridge->saveSRAM(std::string("save.srm"));
    }
}

void Bus::resetDebugHistory() {
    apu_port_write_history.fill(APUPortWriteDebug{});
    apu_port_write_history_pos = 0;
    apu_port_write_history_filled = false;
    apu_port_write_sequence = 0;
}

void Bus::handleAPUWrite(uint8_t port, uint8_t data) {
    if (apu) {
        APUPortWriteDebug& entry = apu_port_write_history[apu_port_write_history_pos];
        entry.sequence = ++apu_port_write_sequence;
        entry.cpu_pc = cpu ? cpu->getProgramCounter() : 0;
        entry.a = cpu ? cpu->getA() : 0;
        entry.x = cpu ? cpu->getX() : 0;
        entry.y = cpu ? cpu->getY() : 0;
        entry.sp = cpu ? cpu->getSP() : 0;
        entry.d = cpu ? cpu->getD() : 0;
        entry.db = cpu ? cpu->getDB() : 0;
        entry.p = cpu ? cpu->getP() : 0;
        entry.opcode = cpu ? cpu->getLastOpcode() : 0;
        const uint16_t direct_page_base = cpu ? cpu->getD() : 0;
        entry.dp0 = wram[direct_page_base & 0xFFFF];
        entry.dp1 = wram[(direct_page_base + 1) & 0xFFFF];
        entry.dp2 = wram[(direct_page_base + 2) & 0xFFFF];
        entry.port = port & 0x03;
        entry.data = data;
        apu_port_write_history_pos =
            (apu_port_write_history_pos + 1) % apu_port_write_history.size();
        apu_port_write_history_filled =
            apu_port_write_history_filled || apu_port_write_history_pos == 0;
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
        if (offset >= 0x2200 && offset <= 0x23FF) {
            if (cartridge && cartridge->hasSA1()) {
                return cartridge->getSA1()->snesRead(offset);
            }
        }
        if (offset >= 0x3000 && offset <= 0x32FF) {
            if (cartridge && cartridge->hasSuperFX()) {
                return cartridge->readCoprocessorRegister(offset);
            }
            return open_bus;
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
            const uint8_t value = (vblank_nmi_flag ? 0x80 : 0x00) | 0x02;
            vblank_nmi_flag = false;
            return value;
        }
        if (offset == 0x4211) {
            const uint8_t value = irq_flag ? 0x80 : 0x00;
            irq_flag = false;
            irq_request_pending = false;
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
        if (offset >= 0x2200 && offset <= 0x23FF) {
            if (cartridge && cartridge->hasSA1()) {
                cartridge->getSA1()->snesWrite(offset, data);
                return;
            }
        }
        if (offset >= 0x3000 && offset <= 0x32FF) {
            if (cartridge && cartridge->hasSuperFX()) {
                cartridge->writeCoprocessorRegister(offset, data);
            }
            return;
        }
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
            const bool old_nmi_enabled = nmi_enabled;
            nmitimen = data;
            nmi_enabled = (data & 0x80) != 0;
            if (!old_nmi_enabled && nmi_enabled && vblank_nmi_flag) {
                nmi_request_pending = true;
            }
            if ((data & 0x30) == 0) {
                irq_flag = false;
                irq_request_pending = false;
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

void Bus::executeDMA(uint8_t channels) {
    for (int i = 0; i < 8; i++) {
        if ((channels & (1 << i)) == 0) continue;

        uint8_t mode = dma[i].control & 0x07;
        bool direction = (dma[i].control & 0x80) != 0;
        bool fixed     = (dma[i].control & 0x08) != 0;
        bool decrement = (dma[i].control & 0x10) != 0;

        int transfer_size = (dma[i].size == 0) ? 0x10000 : dma[i].size;
        uint16_t bytes_transferred = 0;

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

            uint32_t src_full = ((uint32_t)src_bank << 16) | src_offset;

            if (!direction)
                write(b_reg, read(src_full));
            else
                write(src_full, read(b_reg));

            if (!fixed) {
                if (decrement) src_offset--;
                else           src_offset++;
            }

            transfer_size--;
            bytes_transferred++;
        }

        dma[i].src_address = ((uint32_t)src_bank << 16) | src_offset;
        dma[i].size = 0;
    }
}
