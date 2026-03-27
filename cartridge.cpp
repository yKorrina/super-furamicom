#include "cartridge.hpp"
#include <fstream>
#include <iostream>

Cartridge::Cartridge(const std::string& filepath) : loaded(false), is_hirom(false), rom_path(filepath) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open " << filepath << "\n";
        return;
    }

    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    size_t header_offset = (size % 1024 == 512) ? 512 : 0;
    file.seekg(header_offset, std::ios::beg);
    
    rom_data.resize(size - header_offset);
    file.read(reinterpret_cast<char*>(rom_data.data()), rom_data.size());
    file.close();

    loaded = true;
    parseHeader();
    
    // Try loading existing SRAM
    std::string sram_path = filepath + ".srm";
    loadSRAM(sram_path);
    
    std::cout << "[CART] Loaded " << rom_data.size() / 1024 << "KB ROM"
              << (is_hirom ? " (HiROM)" : " (LoROM)")
              << " | SRAM: " << sram.size() / 1024 << "KB\n";
}

bool Cartridge::isLoaded() const { return loaded; }

void Cartridge::parseHeader() {
    if (rom_data.size() < 0x10000) return;

    // Score both LoROM and HiROM header locations
    int lorom_score = 0, hirom_score = 0;

    // LoROM header at $7FB0-$7FFF
    if (rom_data.size() >= 0x8000) {
        uint16_t lo_chk  = rom_data[0x7FDE] | (rom_data[0x7FDF] << 8);
        uint16_t lo_comp = rom_data[0x7FDC] | (rom_data[0x7FDD] << 8);
        if ((lo_chk ^ lo_comp) == 0xFFFF) lorom_score += 4;
        uint8_t lo_map = rom_data[0x7FD5];
        if ((lo_map & 0x0F) == 0x00) lorom_score += 2;  // LoROM mapping
        uint8_t lo_rom_size = rom_data[0x7FD7];
        if (lo_rom_size >= 0x08 && lo_rom_size <= 0x0C) lorom_score += 1;
    }

    // HiROM header at $FFB0-$FFFF
    if (rom_data.size() >= 0x10000) {
        uint16_t hi_chk  = rom_data[0xFFDE] | (rom_data[0xFFDF] << 8);
        uint16_t hi_comp = rom_data[0xFFDC] | (rom_data[0xFFDD] << 8);
        if ((hi_chk ^ hi_comp) == 0xFFFF) hirom_score += 4;
        uint8_t hi_map = rom_data[0xFFD5];
        if ((hi_map & 0x0F) == 0x01) hirom_score += 2;  // HiROM mapping
        uint8_t hi_rom_size = rom_data[0xFFD7];
        if (hi_rom_size >= 0x08 && hi_rom_size <= 0x0C) hirom_score += 1;
    }

    is_hirom = (hirom_score > lorom_score);

    // Determine SRAM size from header
    uint8_t sram_byte;
    if (is_hirom)  sram_byte = (rom_data.size() >= 0xFFD9) ? rom_data[0xFFD8] : 0;
    else           sram_byte = (rom_data.size() >= 0x7FD9) ? rom_data[0x7FD8] : 0;

    // SRAM size: 1KB << sram_byte (0 = no SRAM, 3 = 8KB, 5 = 32KB)
    if (sram_byte > 0 && sram_byte <= 7) {
        size_t sram_size = 1024 << sram_byte;
        sram.resize(sram_size, 0x00);
    } else {
        // Default: give every game 8KB SRAM just in case
        sram.resize(8192, 0x00);
    }
}

uint8_t Cartridge::read(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    if (!is_hirom) {
        // ── LoROM ────────────────────────────────────────────────────────
        // SRAM: banks $70-$7D at $0000-$7FFF
        if (bank >= 0x70 && bank <= 0x7D && offset < 0x8000) {
            uint32_t sram_addr = ((bank - 0x70) * 0x8000 + offset) % sram.size();
            return sram.empty() ? 0x00 : sram[sram_addr];
        }
        // SRAM mirror: banks $00-$3F at $6000-$7FFF (some games use this)
        if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = offset - 0x6000;
            if (bank >= 0x80) sram_addr += (bank - 0x80) * 0x2000;
            else              sram_addr += bank * 0x2000;
            return sram.empty() ? 0x00 : sram[sram_addr % sram.size()];
        }
        // ROM: banks $00-$3F/$80-$BF at $8000-$FFFF
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) {
            if (offset >= 0x8000) {
                uint32_t phys = ((bank & 0x3F) * 0x8000) + (offset - 0x8000);
                if (phys < rom_data.size()) return rom_data[phys];
            }
        }
        // ROM: banks $40-$6F at $0000-$FFFF (extended LoROM)
        if (bank >= 0x40 && bank <= 0x6F) {
            uint32_t phys = ((bank - 0x40) * 0x10000) + offset + (0x200000);
            if (phys < rom_data.size()) return rom_data[phys];
        }
    } else {
        // ── HiROM ────────────────────────────────────────────────────────
        // SRAM: banks $20-$3F at $6000-$7FFF (HiROM SRAM location)
        if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF)) 
            && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = ((bank & 0x1F) * 0x2000) + (offset - 0x6000);
            return sram.empty() ? 0x00 : sram[sram_addr % sram.size()];
        }
        // ROM: banks $00-$3F/$80-$BF at $8000-$FFFF
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) {
            if (offset >= 0x8000) {
                uint32_t phys = ((bank & 0x3F) << 16) | offset;
                if (phys < rom_data.size()) return rom_data[phys];
            }
            // HiROM also maps $0000-$7FFF to start of ROM in some banks
            if (offset < 0x8000 && bank <= 0x3F) {
                uint32_t phys = (bank << 16) | offset;
                if (phys < rom_data.size()) return rom_data[phys];
            }
        }
        // ROM: banks $C0-$FF (full 64KB banks)
        if (bank >= 0xC0 && bank <= 0xFF) {
            uint32_t phys = ((bank & 0x3F) << 16) | offset;
            if (phys < rom_data.size()) return rom_data[phys];
        }
    }
    return 0x00;
}

void Cartridge::write(uint32_t address, uint8_t data) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    if (sram.empty()) return;

    if (!is_hirom) {
        // LoROM SRAM
        if (bank >= 0x70 && bank <= 0x7D && offset < 0x8000) {
            uint32_t sram_addr = ((bank - 0x70) * 0x8000 + offset) % sram.size();
            sram[sram_addr] = data;
            return;
        }
        if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = offset - 0x6000;
            if (bank >= 0x80) sram_addr += (bank - 0x80) * 0x2000;
            else              sram_addr += bank * 0x2000;
            sram[sram_addr % sram.size()] = data;
            return;
        }
    } else {
        // HiROM SRAM
        if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF)) 
            && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = ((bank & 0x1F) * 0x2000) + (offset - 0x6000);
            sram[sram_addr % sram.size()] = data;
            return;
        }
    }
}

void Cartridge::saveSRAM(const std::string& path) {
    if (sram.empty()) return;
    std::ofstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.write(reinterpret_cast<char*>(sram.data()), sram.size());
        std::cout << "[CART] SRAM saved to " << path << "\n";
    }
}

void Cartridge::loadSRAM(const std::string& path) {
    if (sram.empty()) return;
    std::ifstream file(path, std::ios::binary);
    if (file.is_open()) {
        file.read(reinterpret_cast<char*>(sram.data()), sram.size());
        std::cout << "[CART] SRAM loaded from " << path << "\n";
    }
}
