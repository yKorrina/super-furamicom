#ifndef SA1_HPP
#define SA1_HPP

#include <array>
#include <cstdint>
#include <vector>

class SA1 {
public:
    SA1();

    void reset();
    void setROM(const uint8_t* data, std::size_t size);
    void tick(int snes_cpu_cycles);

    // SNES CPU facing register access ($2200-$23FF)
    uint8_t  snesRead(uint16_t address);
    void     snesWrite(uint16_t address, uint8_t data);

    // Cartridge memory access (called from bus for SA-1 mapped regions)
    uint8_t  cartRead(uint32_t address);
    void     cartWrite(uint32_t address, uint8_t data);

    bool consumeSNESIRQ();
    bool hasSNESIRQ() const { return snes_irq_pending; }

    // BW-RAM / I-RAM access for SNES CPU side
    uint8_t readIRAM(uint16_t offset) const;
    void writeIRAM(uint16_t offset, uint8_t data);
    uint8_t readBWRAM(uint32_t offset) const;
    void writeBWRAM(uint32_t offset, uint8_t data);

    std::size_t getBWRAMSize() const { return bwram.size(); }

private:
    // SA-1 internal 65816 CPU state
    struct CPU65816 {
        uint16_t A = 0, X = 0, Y = 0, SP = 0, PC = 0, D = 0;
        uint8_t PB = 0, DB = 0, P = 0x34;
        bool E = true;
        bool halted = true;
        bool waiting = false;
        bool nmi_pending = false;
        bool irq_pending = false;
    } sa1cpu;

    // Internal memory
    std::array<uint8_t, 2048> iram{};     // 2KB I-RAM
    std::vector<uint8_t> bwram;            // BW-RAM (typically 8-64KB)
    const uint8_t* rom = nullptr;
    std::size_t rom_size = 0;

    // SA-1 register file ($2200-$23FF)
    // SNES -> SA-1 control
    uint8_t sa1_control = 0;          // $2200 CCNT
    uint8_t snes_irq_enable = 0;      // $2201 SIE
    uint8_t snes_irq_clear = 0;       // $2202 SIC
    uint16_t sa1_reset_vector = 0;    // $2203-$2204 CRV
    uint16_t sa1_nmi_vector = 0;      // $2205-$2206 CNV
    uint16_t sa1_irq_vector = 0;      // $2207-$2208 CIV
    uint8_t snes_to_sa1_msg = 0;      // $2209 (low nibble of SCNT)

    // SA-1 -> SNES control
    uint8_t sa1_cpu_control = 0;      // $2209 SCNT
    uint8_t sa1_irq_enable = 0;       // $220A CIE
    uint8_t sa1_irq_clear = 0;        // $220B CIC
    uint16_t snes_nmi_vector = 0;     // $220C-$220D SNV
    uint16_t snes_irq_vector = 0;     // $220E-$220F SIV

    // Timer
    uint16_t h_count = 0;             // $2212-$2213
    uint16_t v_count = 0;             // $2214-$2215

    // ROM bank mapping
    uint8_t rom_bank_c = 0;           // $2220 CXB (default: bank 0)
    uint8_t rom_bank_d = 1;           // $2221 DXB (default: bank 1)
    uint8_t rom_bank_e = 2;           // $2222 EXB (default: bank 2)
    uint8_t rom_bank_f = 3;           // $2223 FXB (default: bank 3)

    // BW-RAM mapping
    uint8_t snes_bwram_bank = 0;      // $2224 BMAPS
    uint8_t sa1_bwram_bank = 0;       // $2225 BMAP
    uint8_t bwram_write_enable = 0;   // $2226 SBWE / $2228 CBWE
    uint8_t sa1_bwram_write_en = 0;

    // BW-RAM bitmap mode
    uint8_t bitmap_mode = 0;          // $2230 bit in DCNT or $223F BBFLAG

    // Arithmetic
    uint16_t math_a = 0;              // $2250-$2251 MA
    uint16_t math_b = 0;              // $2252-$2253 MB
    uint8_t  math_control = 0;        // $2249 (arithmetic control: 0=mul, 1=div, 2=accum)
    int64_t  math_result = 0;         // 40-bit accumulator for multiply-accumulate
    bool     math_overflow = false;

    // DMA
    uint8_t  dma_control = 0;         // $2230 DCNT
    uint8_t  dma_src_device = 0;      // $2231 CDMA
    uint32_t dma_src_addr = 0;        // $2232-$2234
    uint32_t dma_dst_addr = 0;        // $2235-$2237
    uint16_t dma_length = 0;          // $2238-$2239
    bool     dma_running = false;

    // Variable-length bitstream
    uint32_t vda = 0;                 // $2259-$225B VDA
    uint32_t vlbit_buffer = 0;
    uint8_t  vlbit_remaining = 0;
    uint8_t  vbd = 0;                 // $2258 VBD

    // Status flags
    bool snes_irq_pending = false;
    bool sa1_irq_from_snes = false;
    bool sa1_nmi_from_snes = false;
    uint8_t snes_irq_flags = 0;       // $2301 CFR
    uint8_t sa1_irq_flags = 0;        // (internal)

    // SNES vector override
    bool override_snes_nmi = false;
    bool override_snes_irq = false;

    // Cycle accumulator for running SA-1 CPU at 3x speed
    int cycle_accumulator = 0;

    // SA-1 CPU execution
    void sa1Step();
    uint8_t sa1Read(uint32_t addr);
    void sa1Write(uint32_t addr, uint8_t data);
    uint8_t sa1ReadImm();
    uint16_t sa1Read16(uint32_t addr);
    void sa1Write16(uint32_t addr, uint16_t data);
    void sa1Push8(uint8_t data);
    void sa1Push16(uint16_t data);
    uint8_t sa1Pop8();
    uint16_t sa1Pop16();

    void sa1NMI();
    void sa1IRQ();

    // ROM address mapping
    uint32_t mapROMAddress(uint32_t address) const;

    // Flag helpers
    void setFlag(uint8_t flag, bool v);
    bool getFlag(uint8_t flag) const;
    void updateNZ8(uint8_t val);
    void updateNZ16(uint16_t val);

    // Arithmetic unit
    void runArithmetic();

    // DMA
    void runDMA();

    // Variable-length bitstream
    void vlBitstreamStart();
    void vlBitstreamShift();
};

#endif
