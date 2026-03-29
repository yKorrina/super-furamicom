#ifndef BUS_HPP
#define BUS_HPP

#include <cstdint>
#include <array>
#include <vector>
#include "cartridge.hpp"
#include "ppu.hpp"

class CPU;
class APU;

struct DMAChannel {
    uint8_t  control = 0;
    uint8_t  dest_reg = 0;
    uint32_t src_address = 0;
    uint16_t size = 0;
    uint8_t  indirect_bank = 0;
    uint16_t table_address = 0;
    uint8_t  line_counter = 0;
    bool     hdma_repeat = false;
    bool     hdma_do_transfer = false;
    bool     hdma_terminated = false;
};

class Bus {
public:
    static constexpr std::size_t kAPUPortWriteHistory = 256;

    struct APUPortWriteDebug {
        std::uint64_t sequence = 0;
        uint32_t cpu_pc = 0;
        uint16_t a = 0;
        uint16_t x = 0;
        uint16_t y = 0;
        uint16_t sp = 0;
        uint16_t d = 0;
        uint8_t db = 0;
        uint8_t p = 0;
        uint8_t opcode = 0;
        uint8_t dp0 = 0;
        uint8_t dp1 = 0;
        uint8_t dp2 = 0;
        uint8_t port = 0;
        uint8_t data = 0;
    };

    Bus(CPU* c, PPU* p, APU* a);

    uint8_t read(uint32_t address);
    void write(uint32_t address, uint8_t data);
    void insertCartridge(Cartridge* cart);
    void beginFrame();
    void startVBlank();
    void endVBlank();
    void beginScanline(int scanline);
    void beginHBlank(int scanline);
    void stepHDMA();
    void tick(int cpu_cycles);

    bool isNMIEnabled() const { return nmi_enabled; }
    bool consumeNMI();
    bool consumeIRQ();
    void setJoypadState(uint16_t pad1, uint16_t pad2);
    uint16_t getJoy1AutoRead() const { return joy1_auto_read; }
    uint16_t getJoy2AutoRead() const { return joy2_auto_read; }
    uint16_t getJoy1Shift() const { return joy1_shift; }
    uint16_t getJoy2Shift() const { return joy2_shift; }
    bool getJoypadStrobe() const { return joypad_strobe; }
    uint8_t getLastJoy4016Write() const { return last_joy4016_write; }
    std::uint64_t getJoy4016Reads() const { return joy4016_reads; }
    std::uint64_t getJoy4017Reads() const { return joy4017_reads; }
    std::uint64_t getJoy4218Reads() const { return joy4218_reads; }
    std::uint64_t getJoy4219Reads() const { return joy4219_reads; }
    std::uint64_t getJoy421AReads() const { return joy421A_reads; }
    std::uint64_t getJoy421BReads() const { return joy421B_reads; }
    std::uint64_t getJoy4016Writes() const { return joy4016_writes; }
    const std::array<APUPortWriteDebug, kAPUPortWriteHistory>& getAPUPortWriteHistory() const {
        return apu_port_write_history;
    }
    std::size_t getAPUPortWriteHistoryPos() const { return apu_port_write_history_pos; }
    bool hasAPUPortWriteHistoryWrapped() const { return apu_port_write_history_filled; }
    void resetDebugHistory();
    const uint8_t* getWRAMData() const { return wram.empty() ? nullptr : wram.data(); }
    std::size_t getWRAMSize() const { return wram.size(); }
    uint32_t getWRAMAddress() const { return wram_address; }
    uint8_t getOpenBus() const { return open_bus; }
    const DMAChannel* getDMAChannels() const { return dma; }
    uint8_t getHDMAEnable() const { return hdma_enable; }

    void saveSRAM();

private:
    CPU* cpu;
    PPU* ppu;
    APU* apu;
    Cartridge* cartridge = nullptr;
    std::vector<uint8_t> wram;

    DMAChannel dma[8];
    void executeDMA(uint8_t channels);
    void initializeHDMAChannel(int channel);
    void reloadHDMALineCounter(int channel);
    void transferHDMAChannel(int channel);
    void latchJoypads();
    static uint16_t encodeJoypadState(uint16_t state);
    uint8_t readJoypadSerial(int port);

    bool nmi_enabled;
    uint8_t hdma_enable = 0;
    uint8_t nmitimen = 0;
    uint16_t htime = 0x01FF;
    uint16_t vtime = 0x01FF;
    bool vblank_nmi_flag = false;
    bool nmi_request_pending = false;
    bool irq_flag = false;
    bool irq_request_pending = false;
    bool auto_joypad_busy = false;
    bool irq_fired_this_scanline = false;
    int auto_joypad_cycles_remaining = 0;

    void     handleAPUWrite(uint8_t port, uint8_t data);

    uint16_t joy1_state = 0;
    uint16_t joy2_state = 0;
    uint16_t joy1_auto_read = 0;
    uint16_t joy2_auto_read = 0;
    uint16_t joy1_auto_read_latch = 0;
    uint16_t joy2_auto_read_latch = 0;
    uint16_t joy1_shift = 0;
    uint16_t joy2_shift = 0;
    bool joypad_strobe = false;
    uint8_t last_joy4016_write = 0;
    std::uint64_t joy4016_reads = 0;
    std::uint64_t joy4017_reads = 0;
    std::uint64_t joy4218_reads = 0;
    std::uint64_t joy4219_reads = 0;
    std::uint64_t joy421A_reads = 0;
    std::uint64_t joy421B_reads = 0;
    std::uint64_t joy4016_writes = 0;
    std::array<APUPortWriteDebug, kAPUPortWriteHistory> apu_port_write_history{};
    std::size_t apu_port_write_history_pos = 0;
    bool apu_port_write_history_filled = false;
    std::uint64_t apu_port_write_sequence = 0;

    uint8_t  wrmpya = 0;
    uint16_t wrdivl = 0;
    uint8_t  wrdivb = 0;
    uint16_t rddiv  = 0;
    uint16_t rdmpy  = 0;

    uint32_t wram_address = 0;
    uint8_t  open_bus = 0;
};

#endif
