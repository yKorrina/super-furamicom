#include "cartridge.hpp"
#include <fstream>
#include <iostream>

Cartridge::Cartridge(const std::string& filepath) : loaded(false), is_hirom(false) {
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
}

bool Cartridge::isLoaded() const { return loaded; }

void Cartridge::parseHeader() {
    if (rom_data.size() < 0x8000) return; 

    uint16_t lorom_checksum = (rom_data[0x7FDE] | (rom_data[0x7FDF] << 8));
    uint16_t lorom_complement = (rom_data[0x7FDC] | (rom_data[0x7FDD] << 8));
    
    if ((lorom_checksum ^ lorom_complement) == 0xFFFF) {
        is_hirom = false;
        return;
    }
    is_hirom = true; 
}

uint8_t Cartridge::read(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    // Translation logic to find the physical byte in the ROM array
    if (!is_hirom) { // LoROM 
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) {
            if (offset >= 0x8000) {
                uint32_t phys = ((bank & 0x3F) * 0x8000) + (offset - 0x8000);
                if (phys < rom_data.size()) return rom_data[phys];
            }
        }
    } else { // HiROM
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF) || (bank >= 0xC0 && bank <= 0xFF)) {
            uint32_t phys = ((bank & 0x3F) << 16) | offset;
            if (phys < rom_data.size()) return rom_data[phys];
        }
    }
    return 0x00;
}