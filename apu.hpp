#ifndef APU_HPP
#define APU_HPP

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>

class APU {
public:
    static constexpr int kOutputHz = 32000;

    APU();

    void reset();

    uint8_t readPort(uint8_t port) const;
    void writePort(uint8_t port, uint8_t data);

    void tickCPUCycles(int cpu_cycles);
    void endFrame();
    void mix(int16_t* stream, int stereo_frames);

    bool hasUploadedProgram() const { return uploaded_program; }
    std::size_t getUploadedBytes() const { return uploaded_bytes; }
    uint16_t getUploadAddress() const { return upload_address; }
    uint16_t getExecuteAddress() const { return execute_address; }
    bool isDriverReady() const { return driver_ready; }
    bool isUserCodeRunning() const { return user_code_running; }
    bool hasUnsupportedOpcode() const { return unsupported_opcode; }
    uint8_t getUnsupportedOpcode() const { return unsupported_op; }
    uint16_t getUnsupportedPC() const { return unsupported_pc; }
    uint16_t getSPCPC() const { return pc; }
    uint8_t getSPCA() const { return a; }
    uint8_t getSPCX() const { return x; }
    uint8_t getSPCY() const { return y; }
    uint8_t getSPCSP() const { return sp; }
    uint8_t getSPCPSW() const { return psw; }
    uint8_t getDSPRegister(uint8_t reg) const { return dsp_regs[reg & 0x7F]; }
    std::uint64_t getGeneratedAudioFrames() const { return generated_audio_frames; }
    std::uint64_t getNonZeroAudioFrames() const { return nonzero_audio_frames; }
    int getAudioPeakSample() const { return audio_peak_sample; }
    const std::array<uint8_t, 4>& getCpuToSPCPorts() const { return cpu_to_spc; }
    const std::array<uint8_t, 4>& getSPCToCpuPorts() const { return spc_to_cpu; }
    const uint8_t* getRAMData() const { return ram.data(); }

private:
    struct Voice {
        bool active = false;
        bool releasing = true;
        bool stop_after_block = false;
        uint8_t source_number = 0;
        uint16_t source_start = 0;
        uint16_t loop_start = 0;
        uint16_t next_block_addr = 0;
        uint16_t interp_index = 0;
        int sample_index = 0;
        uint16_t envelope = 0;
        int16_t prev1 = 0;
        int16_t prev2 = 0;
        int16_t last_output = 0;
        std::array<int16_t, 16> decoded{};
    };

    struct Timer {
        uint8_t target = 0;
        uint8_t stage2 = 0;
        uint8_t stage3 = 0;
        int divider = 0;
        bool enabled = false;
        int period = 128;
    };

    static constexpr uint8_t kFlagN = 0x80;
    static constexpr uint8_t kFlagV = 0x40;
    static constexpr uint8_t kFlagP = 0x20;
    static constexpr uint8_t kFlagB = 0x10;
    static constexpr uint8_t kFlagH = 0x08;
    static constexpr uint8_t kFlagI = 0x04;
    static constexpr uint8_t kFlagZ = 0x02;
    static constexpr uint8_t kFlagC = 0x01;

    std::array<uint8_t, 64 * 1024> ram{};
    std::array<uint8_t, 4> cpu_to_spc{};
    std::array<uint8_t, 4> spc_to_cpu{};
    std::array<uint8_t, 0x80> dsp_regs{};
    std::array<Timer, 3> timers{};
    std::array<Voice, 8> voices{};
    std::deque<int16_t> audio_fifo;
    mutable std::mutex audio_mutex;

    uint16_t pc = 0xFFC0;
    uint8_t a = 0;
    uint8_t x = 0;
    uint8_t y = 0;
    uint8_t sp = 0xEF;
    uint8_t psw = kFlagZ;
    uint8_t test_reg = 0x0A;
    uint8_t control_reg = 0xB0;
    uint8_t dsp_addr = 0x00;
    bool ipl_rom_enabled = true;
    bool sleeping = false;
    bool stopped = false;
    bool uploaded_program = false;
    bool driver_ready = false;
    bool user_code_running = false;
    bool unsupported_opcode = false;
    std::size_t uploaded_bytes = 0;
    uint16_t upload_address = 0;
    uint16_t execute_address = 0;
    uint8_t unsupported_op = 0;
    uint16_t unsupported_pc = 0;
    std::uint64_t generated_audio_frames = 0;
    std::uint64_t nonzero_audio_frames = 0;
    int audio_peak_sample = 0;
    std::uint64_t cpu_cycle_accumulator = 0;
    int sample_cycle_accumulator = 0;

    uint8_t readMem(uint16_t address);
    void writeMem(uint16_t address, uint8_t data);
    uint8_t fetchByte();
    uint16_t fetchWord();
    uint16_t readWord(uint16_t address);
    uint16_t readDirectWord(uint8_t direct_page_addr);
    uint16_t directPageAddress(uint8_t offset) const;
    uint16_t directPageIndexedX(uint8_t offset) const;
    uint16_t directPageIndexedY(uint8_t offset) const;
    uint16_t indirectIndexedX(uint8_t direct_page_addr);
    uint16_t indirectIndexedY(uint8_t direct_page_addr);
    void push(uint8_t value);
    uint8_t pop();
    void setNZ8(uint8_t value);
    void setNZ16(uint16_t value);
    void setFlag(uint8_t flag, bool enabled);
    bool getFlag(uint8_t flag) const;
    void updateDriverReadyState();
    void writeDSP(uint8_t reg, uint8_t value);
    void keyOn(uint8_t voice_mask);
    void keyOff(uint8_t voice_mask);
    void resetVoice(int voice_index);
    bool decodeNextBlock(int voice_index);
    int16_t renderVoice(int voice_index);
    void runCycles(int spc_cycles);
    int stepInstruction();
    void tickTimers(int spc_cycles);
    void queueSample(int16_t left, int16_t right);
    void synthesizeSamples(int spc_cycles);
};

#endif
