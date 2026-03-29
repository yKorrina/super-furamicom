#ifndef APU_HPP
#define APU_HPP

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>

class APU {
public:
    static constexpr int kOutputHz = 32000;
    static constexpr std::size_t kDebugAudioHistory = 8192;
    static constexpr std::size_t kAudioFifoSamples = 8192;
    static constexpr std::size_t kAudioStagingSamples = 1024;
    static constexpr std::size_t kInstructionHistory = 16;
    static constexpr std::size_t kDirectoryWriteHistory = 256;
    static constexpr std::size_t kWatchedWriteHistory = 512;

    struct VoiceDebug {
        bool active = false;
        bool releasing = true;
        bool key_on_pending = false;
        bool noise_enabled = false;
        bool pitch_mod_enabled = false;
        uint8_t source_number = 0;
        uint8_t adsr1 = 0;
        uint8_t max_adsr1_written = 0;
        uint8_t adsr2 = 0;
        uint8_t gain = 0;
        uint16_t source_start = 0;
        uint16_t loop_start = 0;
        uint16_t next_block_addr = 0;
        uint16_t interp_index = 0;
        int sample_index = 0;
        int decoded_sample_count = 0;
        int16_t tap_prev2 = 0;
        int16_t tap_prev1 = 0;
        int16_t tap_curr = 0;
        int16_t tap_next = 0;
        int16_t current_sample = 0;
        uint16_t pitch = 0;
        uint16_t envelope = 0;
        int16_t last_output = 0;
        int8_t volume_left = 0;
        int8_t volume_right = 0;
        uint8_t envx = 0;
        uint8_t outx = 0;
    };

    struct TimerDebug {
        uint8_t target = 0;
        uint8_t stage2 = 0;
        uint8_t stage3 = 0;
        int divider = 0;
        bool enabled = false;
        int period = 0;
    };

    struct DirectoryWriteDebug {
        std::uint64_t sequence = 0;
        uint16_t pc = 0;
        uint8_t opcode = 0;
        uint8_t a = 0;
        uint8_t x = 0;
        uint8_t y = 0;
        uint8_t psw = 0;
        uint16_t address = 0;
        uint8_t before = 0;
        uint8_t value = 0;
    };

    using WatchedWriteDebug = DirectoryWriteDebug;

    APU();

    void reset();

    uint8_t readPort(uint8_t port) const;
    void writePort(uint8_t port, uint8_t data);

    void tickCPUCycles(int cpu_cycles);
    void endFrame();
    void mix(int16_t* stream, int stereo_frames);
    void configureWriteWatch(uint16_t start, uint16_t length);
    void clearWriteWatch();

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
    std::uint64_t getSilentAudioFrames() const { return silent_audio_frames; }
    int getAudioPeakSample() const { return audio_peak_sample; }
    std::size_t getQueuedAudioFrames() const;
    std::size_t getPeakQueuedAudioFrames() const { return audio_peak_queued_frames; }
    std::uint64_t getAudioUnderrunFrames() const { return audio_underrun_frames; }
    std::uint64_t getAudioDroppedFrames() const { return audio_dropped_frames; }
    std::uint64_t getBlankLoopFallbacks() const { return blank_loop_fallbacks; }
    uint8_t getDSPAddress() const { return dsp_addr; }
    uint8_t getControlReg() const { return control_reg; }
    uint8_t getTestReg() const { return test_reg; }
    std::uint64_t getDSPReadCount(uint8_t reg) const { return dsp_read_counts[reg & 0x7F]; }
    std::uint64_t getDSPWriteCount(uint8_t reg) const { return dsp_write_counts[reg & 0x7F]; }
    std::uint64_t getKeyOnCount() const { return key_on_count; }
    std::uint64_t getKeyOffCount() const { return key_off_count; }
    uint8_t getLastKeyOnMask() const { return last_key_on_mask; }
    uint8_t getLastKeyOffMask() const { return last_key_off_mask; }
    void setVoiceEnableMask(uint8_t mask) { voice_enable_mask = mask; }
    uint8_t getVoiceEnableMask() const { return voice_enable_mask; }
    const std::array<std::uint64_t, 4>& getCpuPortWriteCounts() const { return cpu_port_write_counts; }
    const std::array<std::uint64_t, 4>& getSpcPortWriteCounts() const { return spc_port_write_counts; }
    const std::array<uint8_t, 4>& getLastCpuPortWrites() const { return last_cpu_port_writes; }
    const std::array<uint8_t, 4>& getLastSpcPortWrites() const { return last_spc_port_writes; }
    const std::array<uint8_t, 4>& getCpuToSPCPorts() const { return cpu_to_spc; }
    const std::array<uint8_t, 4>& getSPCToCpuPorts() const { return spc_to_cpu; }
    const uint8_t* getRAMData() const { return ram.data(); }
    const std::array<int16_t, kDebugAudioHistory>& getRecentAudioMono() const { return recent_audio_mono; }
    std::size_t getRecentAudioWritePos() const { return recent_audio_pos; }
    bool hasRecentAudioHistory() const { return recent_audio_filled; }
    int16_t getNoiseSample() const { return noise_lfsr; }
    int16_t getLastMixedLeft() const { return last_mixed_left; }
    int16_t getLastMixedRight() const { return last_mixed_right; }
    uint8_t getPendingKeyOnMask() const { return pending_kon; }
    uint8_t getPendingKeyOffMask() const { return pending_koff; }
    int getActiveVoiceCount() const;
    std::array<VoiceDebug, 8> getVoiceDebug() const;
    std::array<TimerDebug, 3> getTimerDebug() const;
    const std::array<DirectoryWriteDebug, kDirectoryWriteHistory>& getDirectoryWriteHistory() const {
        return directory_write_history;
    }
    std::size_t getDirectoryWriteHistoryPos() const { return directory_write_history_pos; }
    bool hasDirectoryWriteHistoryWrapped() const { return directory_write_history_filled; }
    const std::array<WatchedWriteDebug, kWatchedWriteHistory>& getWatchedWriteHistory() const {
        return watched_write_history;
    }
    std::size_t getWatchedWriteHistoryPos() const { return watched_write_history_pos; }
    bool hasWatchedWriteHistoryWrapped() const { return watched_write_history_filled; }
    bool isWriteWatchEnabled() const { return write_watch_enabled; }
    uint16_t getWriteWatchStart() const { return write_watch_start; }
    uint16_t getWriteWatchLength() const { return write_watch_length; }
    const std::array<uint16_t, kInstructionHistory>& getInstructionPCs() const { return instruction_pc_history; }
    const std::array<uint8_t, kInstructionHistory>& getInstructionOpcodes() const { return instruction_opcode_history; }
    const std::array<uint8_t, kInstructionHistory>& getInstructionAHistory() const { return instruction_a_history; }
    const std::array<uint8_t, kInstructionHistory>& getInstructionXHistory() const { return instruction_x_history; }
    const std::array<uint8_t, kInstructionHistory>& getInstructionYHistory() const { return instruction_y_history; }
    std::size_t getInstructionHistoryPos() const { return instruction_history_pos; }
    bool hasInstructionHistoryWrapped() const { return instruction_history_filled; }

private:
    enum class EnvelopeMode : uint8_t {
        Attack,
        Decay,
        Sustain,
        Release,
        Gain,
        Direct,
    };

    struct SampleRing {
        static constexpr int kCapacity = 128;
        std::array<int16_t, kCapacity> data{};
        int head = 0;
        int count = 0;

        void clear() { head = 0; count = 0; }
        bool empty() const { return count == 0; }
        int size() const { return count; }

        int16_t operator[](int index) const {
            return data[(head + index) % kCapacity];
        }

        void push_back(int16_t value) {
            if (count < kCapacity) {
                data[(head + count) % kCapacity] = value;
                count++;
            }
        }

        void push_front(int16_t value) {
            head = (head + kCapacity - 1) % kCapacity;
            data[head] = value;
            if (count < kCapacity) count++;
        }

        void erase_front(int n) {
            if (n >= count) { clear(); return; }
            head = (head + n) % kCapacity;
            count -= n;
        }
    };

    struct Voice {
        bool active = false;
        bool releasing = true;
        bool stop_after_block = false;
        bool key_on_pending = false;
        uint8_t latched_source_number = 0;
        uint8_t source_number = 0;
        uint16_t source_start = 0;
        uint16_t loop_start = 0;
        uint16_t next_block_addr = 0;
        uint16_t interp_index = 0;
        int kon_delay = 0;
        int sample_index = 0;
        uint16_t envelope = 0;
        EnvelopeMode envelope_mode = EnvelopeMode::Release;
        int hidden_envelope = 0;
        int16_t prev1 = 0;
        int16_t prev2 = 0;
        int16_t current_sample = 0;
        int16_t last_output = 0;
        bool reset_predictor_on_next_block = false;
        uint16_t recovered_loop_start = 0;
        bool recovered_loop_reset_predictor = false;
        SampleRing sample_ring;
    };

    struct Timer {
        uint8_t target = 0;
        uint8_t stage2 = 0;
        uint8_t stage3 = 0;
        int divider = 0;
        bool enabled = false;
        int period = 128;
    };

    struct PendingCpuPortWrite {
        uint8_t port = 0;
        uint8_t data = 0;
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
    std::array<std::uint64_t, 0x80> dsp_read_counts{};
    std::array<std::uint64_t, 0x80> dsp_write_counts{};
    std::array<uint8_t, 8> max_voice_adsr1_written{};
    std::array<Timer, 3> timers{};
    std::array<Voice, 8> voices{};
    std::array<int16_t, kDebugAudioHistory> recent_audio_mono{};
    std::array<int16_t, 8> echo_history_left{};
    std::array<int16_t, 8> echo_history_right{};
    std::array<int16_t, kAudioFifoSamples> audio_fifo{};
    std::array<int16_t, kAudioStagingSamples> audio_staging{};
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
    uint16_t current_instruction_pc = 0;
    uint8_t current_instruction_opcode = 0;
    std::array<uint16_t, kInstructionHistory> instruction_pc_history{};
    std::array<uint8_t, kInstructionHistory> instruction_opcode_history{};
    std::array<uint8_t, kInstructionHistory> instruction_a_history{};
    std::array<uint8_t, kInstructionHistory> instruction_x_history{};
    std::array<uint8_t, kInstructionHistory> instruction_y_history{};
    std::array<DirectoryWriteDebug, kDirectoryWriteHistory> directory_write_history{};
    std::array<WatchedWriteDebug, kWatchedWriteHistory> watched_write_history{};
    std::size_t instruction_history_pos = 0;
    bool instruction_history_filled = false;
    std::size_t directory_write_history_pos = 0;
    bool directory_write_history_filled = false;
    std::uint64_t directory_write_sequence = 0;
    std::size_t watched_write_history_pos = 0;
    bool watched_write_history_filled = false;
    std::uint64_t watched_write_sequence = 0;
    std::uint64_t generated_audio_frames = 0;
    std::uint64_t nonzero_audio_frames = 0;
    std::uint64_t silent_audio_frames = 0;
    std::uint64_t blank_loop_fallbacks = 0;
    int audio_peak_sample = 0;
    std::size_t audio_peak_queued_frames = 0;
    std::uint64_t audio_underrun_frames = 0;
    std::uint64_t audio_dropped_frames = 0;
    std::uint64_t cpu_cycle_accumulator = 0;
    std::uint64_t dsp_sample_counter = 0;
    int dsp_counter = 0;
    bool dsp_key_poll_phase = false;
    int sample_cycle_accumulator = 0;
    int spc_cycle_budget = 0;
    std::size_t recent_audio_pos = 0;
    bool recent_audio_filled = false;
    std::size_t echo_history_pos = 0;
    std::size_t audio_read_pos = 0;
    std::size_t audio_write_pos = 0;
    std::size_t audio_count = 0;
    std::size_t audio_staging_count = 0;
    int16_t noise_lfsr = 0x4000;
    int16_t last_mixed_left = 0;
    int16_t last_mixed_right = 0;
    int16_t playback_hold_left = 0;
    int16_t playback_hold_right = 0;
    uint8_t pending_kon = 0;
    uint8_t pending_koff = 0;
    uint8_t latched_kon = 0;
    uint8_t voice_enable_mask = 0xFF;
    std::uint64_t key_on_count = 0;
    std::uint64_t key_off_count = 0;
    uint8_t last_key_on_mask = 0;
    uint8_t last_key_off_mask = 0;
    std::array<std::uint64_t, 4> cpu_port_write_counts{};
    std::array<std::uint64_t, 4> spc_port_write_counts{};
    std::array<uint8_t, 4> last_cpu_port_writes{};
    std::array<uint8_t, 4> last_spc_port_writes{};
    bool write_watch_enabled = false;
    uint16_t write_watch_start = 0;
    uint16_t write_watch_length = 0;
    std::array<PendingCpuPortWrite, 16> pending_cpu_port_writes{};
    std::size_t pending_cpu_port_write_count = 0;

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
    void updateEnvelope(int voice_index);
    void updateNoise();
    void pollKeyStates();
    bool shouldTickRate(uint8_t rate) const;
    int16_t renderVoice(int voice_index, int pitch_mod_source_output);
    int16_t readSample16(uint16_t address) const;
    void writeSample16(uint16_t address, int16_t sample);
    void flushQueuedAudio();
    bool isLikelyBlankLoop(uint16_t address) const;
    bool isUsableLoopTarget(uint16_t address) const;
    bool decodeCandidateSamples(uint16_t address, int16_t prev1, int16_t prev2,
        std::array<int16_t, 8>& out_samples) const;
    int scoreLoopCandidate(const Voice& voice, uint16_t address, uint16_t requested_loop,
        bool reset_predictor) const;
    uint16_t resolveLoopStart(const Voice& voice, bool& reset_predictor) const;
    void recordDirectoryWrite(uint16_t address, uint8_t before, uint8_t value);
    void recordWatchedWrite(uint16_t address, uint8_t before, uint8_t value);
    void commitCpuPortWrite(uint8_t port, uint8_t data);
    void runCycles(int spc_cycles);
    int stepInstruction();
    void tickTimers(int spc_cycles);
    void queueSample(int16_t left, int16_t right);
    void synthesizeSamples(int spc_cycles);
};

#endif
