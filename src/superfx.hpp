#ifndef SUPERFX_HPP
#define SUPERFX_HPP

#include <array>
#include <cstdint>
#include <vector>

class SuperFX {
public:
    enum class Revision : uint8_t {
        None = 0,
        Mario,
        GSU1,
        GSU2,
        GSU2SP1,
    };

    static constexpr uint16_t kRegisterBase = 0x3000;
    static constexpr uint16_t kRegisterEnd = 0x32FF;

    explicit SuperFX(Revision revision = Revision::None);

    void reset();
    void setROM(const uint8_t* data, std::size_t size);
    void setRAM(uint8_t* data, std::size_t size);
    void tick(int cpu_cycles);
    uint8_t cpuRead(uint16_t address);
    void cpuWrite(uint16_t address, uint8_t value);
    bool consumeIRQLine();

    bool isPresent() const { return revision != Revision::None; }
    Revision getRevision() const { return revision; }
    const char* getRevisionName() const;
    const std::array<uint16_t, 16>& getRegisters() const { return R; }
    uint16_t getSFR() const { return sfr; }
    uint8_t getBRAMR() const { return bramr; }
    uint8_t getPBR() const { return pbr; }
    uint8_t getROMBR() const { return rombr; }
    uint8_t getCFGR() const { return cfgr; }
    uint8_t getSCBR() const { return scbr; }
    uint8_t getCLSR() const { return clsr; }
    uint8_t getSCMR() const { return scmr; }
    uint8_t getVCR() const { return vcr; }
    uint8_t getRAMBR() const { return rambr; }
    uint16_t getCBR() const { return cbr; }
    const std::array<uint8_t, 512>& getCache() const { return cache; }
    bool isRunning() const { return (sfr & FLAG_GO) != 0; }
    bool snesCanAccessRAM() const { return (scmr & 0x08) == 0; }
    bool snesCanAccessROM() const { return (scmr & 0x10) == 0; }
    uint64_t getLaunchCount() const { return launch_count; }
    uint16_t getLastLaunchPC() const { return last_launch_pc; }

    // GSU ROM/RAM read for external use
    uint8_t gsuReadROM(uint32_t addr);
    uint8_t gsuReadRAM(uint16_t addr);
    void gsuWriteRAM(uint16_t addr, uint8_t data);

private:
    // SFR flag bits
    static constexpr uint16_t FLAG_GO    = 1u << 5;
    static constexpr uint16_t FLAG_R     = 1u << 6;
    static constexpr uint16_t FLAG_IRQ   = 1u << 15;
    static constexpr uint16_t FLAG_Z     = 1u << 1;
    static constexpr uint16_t FLAG_CY    = 1u << 2;
    static constexpr uint16_t FLAG_S     = 1u << 3;
    static constexpr uint16_t FLAG_OV    = 1u << 4;
    static constexpr uint16_t FLAG_B     = 1u << 8;
    static constexpr uint16_t FLAG_IH    = 1u << 9;
    static constexpr uint16_t FLAG_IL    = 1u << 10;
    static constexpr uint16_t FLAG_ALT1  = 1u << 11;
    static constexpr uint16_t FLAG_ALT2  = 1u << 12;
    static constexpr uint16_t FLAG_PREFIX = FLAG_ALT1 | FLAG_ALT2 | FLAG_B;

    static uint8_t resolveVersionCode(Revision revision);
    void executeInstruction();
    void plotPixel(uint8_t x, uint8_t y, uint8_t color);
    void flushPixelCache();
    uint8_t readCacheOrROM(uint16_t addr);

    // Color / plot helpers
    int getScreenWidth() const;
    int getBPP() const;

    Revision revision = Revision::None;
    std::array<uint16_t, 16> R{};
    uint16_t sfr = 0;
    uint8_t bramr = 0;
    uint8_t pbr = 0;
    uint8_t rombr = 0;
    uint8_t cfgr = 0;
    uint8_t scbr = 0;
    uint8_t clsr = 0;
    uint8_t scmr = 0;
    uint8_t vcr = 0;
    uint8_t rambr = 0;
    uint16_t cbr = 0;
    std::array<uint8_t, 512> cache{};
    std::array<bool, 32> cache_valid{};

    // Prefix state
    uint8_t sreg = 0;   // source register (FROM)
    uint8_t dreg = 0;   // destination register (TO)
    bool sreg_set = false;
    bool dreg_set = false;

    // ROM buffer
    uint8_t rom_buffer = 0;
    uint16_t rom_read_addr = 0;

    // RAM buffer
    uint8_t ram_buffer_lo = 0;
    uint8_t ram_buffer_hi = 0;
    uint16_t ram_write_addr = 0;
    uint8_t ram_write_data = 0;

    // Color register
    uint8_t colr = 0;
    uint8_t por = 0;  // plot option register

    // Pixel cache for plot
    struct PixelCache {
        uint8_t pixels[8]{};
        uint8_t x = 0;
        uint8_t y = 0;
        bool valid = false;
        uint8_t bitplanes = 0;
    } pixel_cache;

    bool irq_line_pending = false;
    uint64_t launch_count = 0;
    uint16_t last_launch_pc = 0;
    int cycle_accumulator = 0;

    // External ROM/RAM
    const uint8_t* rom_data = nullptr;
    std::size_t rom_size = 0;
    uint8_t* ram_data = nullptr;
    std::size_t ram_size = 0;
};

#endif
