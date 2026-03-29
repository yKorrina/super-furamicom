#include "cartridge.hpp"
#include "dsp1.hpp"
#include "sa1.hpp"
#include "superfx.hpp"
#include <algorithm>
#include <cctype>
#include <fstream>
#include <iostream>

namespace {
constexpr std::size_t kSuperFXCartRamSize = 128 * 1024;

bool IsSuperFXChipset(uint8_t chipset) {
    switch (chipset) {
    case 0x13:
    case 0x14:
    case 0x15:
    case 0x1A:
        return true;
    default:
        return false;
    }
}

SuperFX::Revision DetectSuperFXRevision(uint8_t chipset) {
    switch (chipset) {
    case 0x13: return SuperFX::Revision::Mario;
    case 0x14:
    case 0x15:
        return SuperFX::Revision::GSU1;
    case 0x1A:
        return SuperFX::Revision::GSU2;
    default:
        return SuperFX::Revision::None;
    }
}

bool IsSuperFXLoROMWindow(uint8_t bank) {
    return bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF);
}

bool IsSuperFXHiROMWindow(uint8_t bank) {
    return (bank >= 0x40 && bank <= 0x5F) || (bank >= 0xC0 && bank <= 0xDF);
}

bool IsSuperFXRamBank(uint8_t bank) {
    return bank == 0x70 || bank == 0x71 || bank == 0xF0 || bank == 0xF1;
}

std::size_t MirrorIndex(std::size_t index, std::size_t size) {
    return size == 0 ? 0 : (index % size);
}

std::string HeaderTitleString(const std::vector<uint8_t>& rom_data, std::size_t header_base) {
    std::string title;
    for (std::size_t i = 0; i < 21 && header_base + i < rom_data.size(); i++) {
        char c = static_cast<char>(rom_data[header_base + i]);
        if (c == '\0') break;
        title.push_back(c);
    }
    while (!title.empty() && std::isspace(static_cast<unsigned char>(title.back()))) {
        title.pop_back();
    }
    return title;
}

bool TitleContainsUpper(const std::string& title, const std::string& pattern) {
    std::string upper_title = title;
    for (char& c : upper_title) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return upper_title.find(pattern) != std::string::npos;
}

bool IsKnownDSP2Title(const std::string& title) {
    return TitleContainsUpper(title, "DUNGEON MASTER");
}

bool IsKnownDSP3Title(const std::string& title) {
    return TitleContainsUpper(title, "SD GUNDAM GX");
}

bool IsDSP1Chipset(uint8_t map_mode, uint8_t chipset, const std::string& title) {
    if (chipset == 0x03) {
        return (map_mode & 0xF0) != 0x30;
    }
    if (chipset == 0x05) {
        return !IsKnownDSP2Title(title) && !IsKnownDSP3Title(title);
    }
    return false;
}

DSP1::MapType DetectDSP1MapType(bool is_hirom, std::size_t rom_size) {
    if (is_hirom) return DSP1::MapType::HiROM;
    return rom_size > 0x100000 ? DSP1::MapType::LoROMLarge : DSP1::MapType::LoROMSmall;
}

bool IsSA1MapMode(uint8_t map_mode) {
    return (map_mode & 0x2F) == 0x23;
}
}

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

    std::string sram_path = filepath + ".srm";
    loadSRAM(sram_path);

    const char* mapper_label = has_sa1 ? " (SA-1)" : (is_hirom ? " (HiROM)" : (has_superfx ? " (SuperFX)" : " (LoROM)"));
    std::cout << "[CART] Loaded " << rom_data.size() / 1024 << "KB ROM"
              << mapper_label
              << " | SRAM: " << sram.size() / 1024 << "KB\n";
    if (has_superfx && superfx) {
        std::cout << "[SFX] Detected " << superfx->getRevisionName()
                  << " cart: map=$" << std::hex << (unsigned)map_mode_byte
                  << " chip=$" << (unsigned)chipset_byte
                  << " rom_size=$" << (unsigned)rom_size_byte
                  << " ram_size=$" << (unsigned)ram_size_byte
                  << std::dec
                  << " | Super FX GSU execution core active\n";
    }
    if (has_sa1 && sa1) {
        std::cout << "[SA1] Detected SA-1 cart: map=$" << std::hex << (unsigned)map_mode_byte
                  << " chip=$" << (unsigned)chipset_byte
                  << " rom_size=$" << (unsigned)rom_size_byte
                  << " ram_size=$" << (unsigned)ram_size_byte
                  << std::dec
                  << " | SA-1 65816 coprocessor active\n";
    }
    if (dsp1) {
        std::cout << "[DSP1] Detected DSP-1 family cart: map=$" << std::hex << (unsigned)map_mode_byte
                  << " chip=$" << (unsigned)chipset_byte
                  << " rom_size=$" << (unsigned)rom_size_byte
                  << " ram_size=$" << (unsigned)ram_size_byte
                  << std::dec
                  << " | " << dsp1->getMapTypeName()
                  << " command/status interface active\n";
    }
}

bool Cartridge::isLoaded() const { return loaded; }

void Cartridge::parseHeader() {
    if (rom_data.size() < 0x10000) return;

    int lorom_score = 0, hirom_score = 0;

    if (rom_data.size() >= 0x8000) {
        uint16_t lo_chk  = rom_data[0x7FDE] | (rom_data[0x7FDF] << 8);
        uint16_t lo_comp = rom_data[0x7FDC] | (rom_data[0x7FDD] << 8);
        if ((lo_chk ^ lo_comp) == 0xFFFF) lorom_score += 4;
        uint8_t lo_map = rom_data[0x7FD5];
        if ((lo_map & 0x0F) == 0x00) lorom_score += 2;
        if (lo_map == 0x20) lorom_score += 3;
        uint8_t lo_rom_size = rom_data[0x7FD7];
        if (lo_rom_size >= 0x08 && lo_rom_size <= 0x0C) lorom_score += 1;
    }

    if (rom_data.size() >= 0x10000) {
        uint16_t hi_chk  = rom_data[0xFFDE] | (rom_data[0xFFDF] << 8);
        uint16_t hi_comp = rom_data[0xFFDC] | (rom_data[0xFFDD] << 8);
        if ((hi_chk ^ hi_comp) == 0xFFFF) hirom_score += 4;
        uint8_t hi_map = rom_data[0xFFD5];
        if ((hi_map & 0x0F) == 0x01) hirom_score += 2;
        uint8_t hi_rom_size = rom_data[0xFFD7];
        if (hi_rom_size >= 0x08 && hi_rom_size <= 0x0C) hirom_score += 1;
    }

    is_hirom = (hirom_score > lorom_score);
    const std::size_t header_base = is_hirom ? 0xFFC0 : 0x7FC0;
    const std::string title = HeaderTitleString(rom_data, header_base);

    if (rom_data.size() >= header_base + 0x1A) {
        map_mode_byte = rom_data[header_base + 0x15];
        chipset_byte = rom_data[header_base + 0x16];
        rom_size_byte = rom_data[header_base + 0x17];
        ram_size_byte = rom_data[header_base + 0x18];
    }

    has_superfx = !is_hirom &&
        map_mode_byte == 0x20 &&
        IsSuperFXChipset(chipset_byte);
    if (has_superfx) {
        superfx = std::make_unique<SuperFX>(DetectSuperFXRevision(chipset_byte));
    } else {
        superfx.reset();
    }

    // Deferred: SuperFX ROM/RAM setup happens after SRAM allocation below

    has_sa1 = IsSA1MapMode(map_mode_byte);
    if (has_sa1) {
        sa1 = std::make_unique<SA1>();
        sa1->setROM(rom_data.data(), rom_data.size());
    } else {
        sa1.reset();
    }

    if (!has_superfx && !has_sa1 && IsDSP1Chipset(map_mode_byte, chipset_byte, title)) {
        dsp1 = std::make_unique<DSP1>(DetectDSP1MapType(is_hirom, rom_data.size()));
    } else {
        dsp1.reset();
    }

    uint8_t sram_byte;
    if (is_hirom)  sram_byte = (rom_data.size() >= 0xFFD9) ? rom_data[0xFFD8] : 0;
    else           sram_byte = (rom_data.size() >= 0x7FD9) ? rom_data[0x7FD8] : 0;

    if (sram_byte > 0 && sram_byte <= 7) {
        size_t sram_size = 1024 << sram_byte;
        sram.resize(sram_size, 0x00);
    } else {
        sram.resize(8192, 0x00);
    }

    if (has_superfx) {
        sram.resize(std::max<std::size_t>(sram.size(), kSuperFXCartRamSize), 0x00);
        if (superfx) {
            superfx->setROM(rom_data.data(), rom_data.size());
            superfx->setRAM(sram.data(), sram.size());
        }
    }
}

const char* Cartridge::getMapperName() const {
    if (has_sa1) return "SA-1";
    if (has_superfx) return "SuperFX";
    return is_hirom ? "HiROM" : "LoROM";
}

void Cartridge::tickCoprocessors(int cpu_cycles) {
    if (superfx) superfx->tick(cpu_cycles);
    if (sa1) sa1->tick(cpu_cycles);
}

bool Cartridge::consumeCoprocessorIRQ() {
    if (superfx && superfx->consumeIRQLine()) return true;
    if (sa1 && sa1->consumeSNESIRQ()) return true;
    return false;
}

uint8_t Cartridge::readCoprocessorRegister(uint16_t address) {
    if (superfx) return superfx->cpuRead(address);
    return 0x00;
}

void Cartridge::writeCoprocessorRegister(uint16_t address, uint8_t data) {
    if (superfx) superfx->cpuWrite(address, data);
}

uint8_t Cartridge::read(uint32_t address) {
    uint8_t bank = (address >> 16) & 0xFF;
    uint16_t offset = address & 0xFFFF;

    if (dsp1 && dsp1->handlesAddress(bank, offset)) {
        return dsp1->cpuRead(offset);
    }

    if (has_sa1 && sa1) {
        return sa1->cartRead(address);
    }

    if (has_superfx) {
        if (!sram.empty()) {
            if (IsSuperFXRamBank(bank)) {
                if (superfx && !superfx->snesCanAccessRAM()) {
                    return 0x00;
                }
                const std::size_t bank_base = ((bank & 0x01) != 0) ? 0x10000 : 0x00000;
                return sram[MirrorIndex(bank_base + offset, sram.size())];
            }
            if (IsSuperFXLoROMWindow(bank) && offset >= 0x6000 && offset < 0x8000) {
                if (superfx && !superfx->snesCanAccessRAM()) {
                    return 0x00;
                }
                return sram[MirrorIndex(offset - 0x6000, sram.size())];
            }
        }

        if (!rom_data.empty()) {
            if (IsSuperFXLoROMWindow(bank) && offset >= 0x8000) {
                if (superfx && !superfx->snesCanAccessROM()) {
                    return 0x00;
                }
                const std::size_t phys =
                    MirrorIndex(((std::size_t)(bank & 0x3F) * 0x8000) + (offset - 0x8000), rom_data.size());
                return rom_data[phys];
            }
            if (IsSuperFXHiROMWindow(bank)) {
                if (superfx && !superfx->snesCanAccessROM()) {
                    return 0x00;
                }
                const std::size_t phys =
                    MirrorIndex(((std::size_t)(bank & 0x1F) * 0x10000) + offset, rom_data.size());
                return rom_data[phys];
            }
        }

        return 0x00;
    }

    if (!is_hirom) {
        if (((bank >= 0x70 && bank <= 0x7D) || bank >= 0xF0) && offset < 0x8000) {
            const uint32_t bank_index = bank >= 0xF0 ? (uint32_t)(bank - 0xF0)
                                                     : (uint32_t)(bank - 0x70);
            uint32_t sram_addr = (bank_index * 0x8000 + offset) % sram.size();
            return sram.empty() ? 0x00 : sram[sram_addr];
        }
        if ((bank <= 0x3F || (bank >= 0x80 && bank <= 0xBF)) && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = offset - 0x6000;
            if (bank >= 0x80) sram_addr += (bank - 0x80) * 0x2000;
            else              sram_addr += bank * 0x2000;
            return sram.empty() ? 0x00 : sram[sram_addr % sram.size()];
        }
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) {
            if (offset >= 0x8000) {
                uint32_t phys = ((bank & 0x3F) * 0x8000) + (offset - 0x8000);
                if (phys < rom_data.size()) return rom_data[phys];
            }
        }
        if (bank >= 0x40 && bank <= 0x6F) {
            uint32_t phys = ((bank - 0x40) * 0x10000) + offset + (0x200000);
            if (phys < rom_data.size()) return rom_data[phys];
        }
    } else {
        if (((bank >= 0x20 && bank <= 0x3F) || (bank >= 0xA0 && bank <= 0xBF))
            && offset >= 0x6000 && offset < 0x8000) {
            uint32_t sram_addr = ((bank & 0x1F) * 0x2000) + (offset - 0x6000);
            return sram.empty() ? 0x00 : sram[sram_addr % sram.size()];
        }
        if ((bank <= 0x3F) || (bank >= 0x80 && bank <= 0xBF)) {
            if (offset >= 0x8000) {
                uint32_t phys = ((bank & 0x3F) << 16) | offset;
                if (phys < rom_data.size()) return rom_data[phys];
            }
            if (offset < 0x8000 && bank <= 0x3F) {
                uint32_t phys = (bank << 16) | offset;
                if (phys < rom_data.size()) return rom_data[phys];
            }
        }
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

    if (dsp1 && dsp1->handlesAddress(bank, offset)) {
        dsp1->cpuWrite(offset, data);
        return;
    }

    if (has_sa1 && sa1) {
        sa1->cartWrite(address, data);
        return;
    }

    if (sram.empty()) return;

    if (has_superfx) {
        if (IsSuperFXRamBank(bank)) {
            if (superfx && !superfx->snesCanAccessRAM()) {
                return;
            }
            const std::size_t bank_base = ((bank & 0x01) != 0) ? 0x10000 : 0x00000;
            sram[MirrorIndex(bank_base + offset, sram.size())] = data;
            return;
        }
        if (IsSuperFXLoROMWindow(bank) && offset >= 0x6000 && offset < 0x8000) {
            if (superfx && !superfx->snesCanAccessRAM()) {
                return;
            }
            sram[MirrorIndex(offset - 0x6000, sram.size())] = data;
            return;
        }
        return;
    }

    if (!is_hirom) {
        if (((bank >= 0x70 && bank <= 0x7D) || bank >= 0xF0) && offset < 0x8000) {
            const uint32_t bank_index = bank >= 0xF0 ? (uint32_t)(bank - 0xF0)
                                                     : (uint32_t)(bank - 0x70);
            uint32_t sram_addr = (bank_index * 0x8000 + offset) % sram.size();
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
