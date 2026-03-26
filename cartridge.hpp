#ifndef CARTRIDGE_HPP
#define CARTRIDGE_HPP

#include <cstdint>
#include <vector>
#include <string>

class Cartridge {
public:
    Cartridge(const std::string& filepath);
    
    bool isLoaded() const;
    uint8_t read(uint32_t address);

private:
    bool loaded;
    bool is_hirom;
    std::vector<uint8_t> rom_data;

    void parseHeader();
};

#endif // CARTRIDGE_HPP