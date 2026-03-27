#ifndef CARTRIDGE_HPP
#define CARTRIDGE_HPP

#include <cstdint>
#include <vector>
#include <string>

class Cartridge {
public:
    Cartridge(const std::string& filepath);
    
    bool isLoaded() const;
    bool isHiROM() const { return is_hirom; }
    uint8_t read(uint32_t address);
    void    write(uint32_t address, uint8_t data);

    void saveSRAM(const std::string& path);
    void loadSRAM(const std::string& path);
    bool hasSRAM() const { return sram.size() > 0; }

private:
    bool loaded;
    bool is_hirom;
    std::vector<uint8_t> rom_data;
    std::vector<uint8_t> sram;
    std::string rom_path;

    void parseHeader();
};

#endif
