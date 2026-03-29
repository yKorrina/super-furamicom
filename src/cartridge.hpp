#ifndef CARTRIDGE_HPP
#define CARTRIDGE_HPP

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>
#include <string>

class SuperFX;
class DSP1;
class SA1;

class Cartridge {
public:
    Cartridge(const std::string& filepath);

    bool isLoaded() const;
    bool isHiROM() const { return is_hirom; }
    bool hasSuperFX() const { return has_superfx; }
    bool hasSA1() const { return has_sa1; }
    bool hasDSP1() const { return dsp1.get() != nullptr; }
    uint8_t getMapModeByte() const { return map_mode_byte; }
    uint8_t getChipsetByte() const { return chipset_byte; }
    uint8_t getROMSizeByte() const { return rom_size_byte; }
    uint8_t getRAMSizeByte() const { return ram_size_byte; }
    std::size_t getRAMSize() const { return sram.size(); }
    const char* getMapperName() const;
    const SuperFX* getSuperFX() const { return superfx.get(); }
    SuperFX* getSuperFX() { return superfx.get(); }
    const DSP1* getDSP1() const { return dsp1.get(); }
    DSP1* getDSP1() { return dsp1.get(); }
    const SA1* getSA1() const { return sa1.get(); }
    SA1* getSA1() { return sa1.get(); }
    void tickCoprocessors(int cpu_cycles);
    bool consumeCoprocessorIRQ();
    uint8_t readCoprocessorRegister(uint16_t address);
    void writeCoprocessorRegister(uint16_t address, uint8_t data);
    uint8_t read(uint32_t address);
    void    write(uint32_t address, uint8_t data);

    void saveSRAM(const std::string& path);
    void loadSRAM(const std::string& path);
    bool hasSRAM() const { return sram.size() > 0; }

private:
    bool loaded;
    bool is_hirom;
    bool has_superfx = false;
    bool has_sa1 = false;
    uint8_t map_mode_byte = 0;
    uint8_t chipset_byte = 0;
    uint8_t rom_size_byte = 0;
    uint8_t ram_size_byte = 0;
    std::vector<uint8_t> rom_data;
    std::vector<uint8_t> sram;
    std::string rom_path;
    std::unique_ptr<SuperFX> superfx;
    std::unique_ptr<DSP1> dsp1;
    std::unique_ptr<SA1> sa1;

    void parseHeader();
};

#endif
