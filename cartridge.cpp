#include "cartridge.hpp"
#include <fstream>
#include <iostream>

Cartridge::Cartridge(const std::string& filepath) : loaded(false), is_hirom(false) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Error: Could not open " << filepath << "\n";
        return;
    }

    // Determine file size
    file.seekg(0, std::ios::end);
    size_t size = file.tellg();
    file.seekg(0, std::ios::beg);

    // SNES ROMs sometimes have a 512-byte copier header. Skip it if present.
    size_t header_offset = (size % 1024 == 512) ? 512 : 0;
    file.seekg(header_offset, std::ios::beg);
    
    rom_data.resize(size - header_offset);
    file.read(reinterpret_cast<char*>(rom_data.data()), rom_data.size());
    file.close();

    loaded = true;
    parseHeader();
}

bool Cartridge::isLoaded() const {
    return loaded;
}

void Cartridge::parseHeader() {
    // SNES header locations depend on the map type. 
    // LoROM header is at 0x7FC0. HiROM is at 0xFFC0.
    // We check the checksum and its complement to verify the header location.
    
    if (rom_data.size() < 0x8000) return; // File too small

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
    uint32_t physical_address = 0;

    if (!is_hirom) {
        // LoROM Mapping
        // Banks 00-3F, 80-BF. ROM is mapped to 8000-FFFF in each bank.
        if (offset < 0x8000) return 0x00; // Not ROM space
        physical_address = ((bank & 0x7F) * 0x8000) + (offset - 0x8000);
    } else {
        // HiROM Mapping
        // Banks 40-7D, C0-FF. ROM is mapped continuously.
        physical_address = ((bank & 0x3F) * 0x10000) + offset;
    }

    if (physical_address < rom_data.size()) {
        return rom_data[physical_address];
    }
    return 0x00;
}