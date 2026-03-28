#ifndef BUS_HPP
#define BUS_HPP

#include <cstdint>
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
    bool     hdma_do_transfer = false;
    bool     hdma_terminated = false;
};

class Bus {
public:
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

    bool isNMIEnabled() const { return nmi_enabled; }
    bool consumeNMI();
    bool consumeIRQ();
    void setJoypadState(uint16_t pad1, uint16_t pad2);

    // Save SRAM to disk
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
    
    bool nmi_enabled; 
    uint8_t hdma_enable = 0;
    uint8_t nmitimen = 0;
    uint16_t htime = 0x01FF;
    uint16_t vtime = 0x01FF;
    bool nmi_pending = false;
    bool irq_pending = false;
    bool auto_joypad_busy = false;
    bool irq_fired_this_scanline = false;
    
    void     handleAPUWrite(uint8_t port, uint8_t data);

    uint16_t joy1_state = 0;
    uint16_t joy2_state = 0;

    uint8_t  wrmpya = 0;
    uint16_t wrdivl = 0;
    uint8_t  wrdivb = 0;
    uint16_t rddiv  = 0;
    uint16_t rdmpy  = 0;

    uint32_t wram_address = 0;
    uint8_t  open_bus = 0;
};

#endif
