#include "apu.hpp"
#include "timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>

namespace {
constexpr int kSpcCyclesPerSecond = 1024000;
constexpr int kSpcCyclesPerSample = 32;
constexpr int kSimpleCounterRange = 2048 * 5 * 3;
constexpr std::size_t kMaxQueuedFrames = 4096;
constexpr std::size_t kMaxQueuedSamples = kMaxQueuedFrames * 2;
constexpr double kPi = 3.14159265358979323846;
constexpr std::array<int, 32> kCounterRates = {
    0, 2048, 1536, 1280, 1024, 768, 640, 512,
    384, 320, 256, 192, 160, 128, 96, 80,
    64, 48, 40, 32, 24, 20, 16, 12,
    10, 8, 6, 5, 4, 3, 2, 1,
};

constexpr std::array<uint8_t, 64> kIPLROM = {
    0xCD, 0xEF, 0xBD, 0xE8, 0x00, 0xC6, 0x1D, 0xD0,
    0xFC, 0x8F, 0xAA, 0xF4, 0x8F, 0xBB, 0xF5, 0x78,
    0xCC, 0xF4, 0xD0, 0xFB, 0x2F, 0x19, 0xEB, 0xF4,
    0xD0, 0xFC, 0x7E, 0xF4, 0xD0, 0x0B, 0xE4, 0xF5,
    0xCB, 0xF4, 0xD7, 0x00, 0xFC, 0xD0, 0xF3, 0xAB,
    0x01, 0x10, 0xEF, 0x7E, 0xF4, 0x10, 0xEB, 0xBA,
    0xF6, 0xDA, 0x00, 0xBA, 0xF4, 0xC4, 0xF4, 0xDD,
    0x5D, 0xD0, 0xDB, 0x1F, 0x00, 0x00, 0xC0, 0xFF,
};

inline int clamp16(int value) {
    return std::clamp(value, -32768, 32767);
}

inline int clamp11(int value) {
    return std::clamp(value, 0, 0x07FF);
}

inline int clip15(int value) {
    return std::clamp(value, -0x4000, 0x3FFF);
}

inline int decodeBrrSample(int nibble, uint8_t range, uint8_t filter, int prev1, int prev2) {
    int sample = nibble;
    if (range <= 12) {
        sample = (sample << range) >> 1;
    } else {
        sample &= ~0x07FF;
    }

    const int p1 = prev1;
    const int p2 = prev2 >> 1;
    switch (filter) {
    case 1:
        sample += p1 >> 1;
        sample += (-p1) >> 5;
        break;
    case 2:
        sample += p1;
        sample -= p2;
        sample += p2 >> 4;
        sample += (p1 * -3) >> 6;
        break;
    case 3:
        sample += p1;
        sample -= p2;
        sample += (p1 * -13) >> 7;
        sample += (p2 * 3) >> 4;
        break;
    default:
        break;
    }

    sample = clamp16(sample);
    return (int16_t)(sample * 2);
}

inline int16_t signExtend15(uint16_t value) {
    return (value & 0x4000) != 0 ? (int16_t)(value | 0x8000) : (int16_t)value;
}

inline int clampIndex(int value, int lo, int hi) {
    return std::max(lo, std::min(value, hi));
}

inline int counterOffset(uint8_t rate) {
    if (rate == 0) {
        return 1;
    }
    if (rate >= 30) {
        return 0;
    }

    switch ((rate - 1) % 3) {
    case 1:
        return 1040;
    case 2:
        return 536;
    default:
        return 0;
    }
}

std::array<int16_t, 512> BuildGaussianTable() {
    std::array<int16_t, 512> table{};
    std::array<double, 512> weights{};

    for (int n = 0; n < 512; n++) {
        const double k = 0.5 + (double)n;
        const double s = std::sin(kPi * k * 1.280 / 1024.0);
        const double t = (std::cos(kPi * k * 2.000 / 1023.0) - 1.0) * 0.50;
        const double u = (std::cos(kPi * k * 4.000 / 1023.0) - 1.0) * 0.08;
        weights[511 - n] = s * (t + u + 1.0) / k;
    }

    for (int phase = 0; phase < 128; phase++) {
        const double sum = weights[phase + 0] +
                           weights[phase + 256] +
                           weights[511 - phase] +
                           weights[255 - phase];
        const double scale = 2048.0 / sum;

        table[phase + 0] = (int16_t)(weights[phase + 0] * scale + 0.5);
        table[phase + 256] = (int16_t)(weights[phase + 256] * scale + 0.5);
        table[511 - phase] = (int16_t)(weights[511 - phase] * scale + 0.5);
        table[255 - phase] = (int16_t)(weights[255 - phase] * scale + 0.5);
    }

    return table;
}

const std::array<int16_t, 512>& GaussianTable() {
    static const std::array<int16_t, 512> table = BuildGaussianTable();
    return table;
}
}

APU::APU() {
    reset();
}

void APU::configureWriteWatch(uint16_t start, uint16_t length) {
    write_watch_enabled = length != 0;
    write_watch_start = start;
    write_watch_length = length;
    watched_write_history.fill(WatchedWriteDebug{});
    watched_write_history_pos = 0;
    watched_write_history_filled = false;
    watched_write_sequence = 0;
}

void APU::clearWriteWatch() {
    configureWriteWatch(0, 0);
}

std::array<APU::VoiceDebug, 8> APU::getVoiceDebug() const {
    std::array<VoiceDebug, 8> info{};
    for (int voice_index = 0; voice_index < 8; voice_index++) {
        const Voice& voice = voices[voice_index];
        const int base = voice_index << 4;
        VoiceDebug& out = info[voice_index];
        out.active = voice.active;
        out.releasing = voice.releasing;
        out.key_on_pending = voice.key_on_pending;
        out.noise_enabled = (dsp_regs[0x3D] & (1u << voice_index)) != 0;
        out.pitch_mod_enabled = voice_index > 0 && (dsp_regs[0x2D] & (1u << voice_index)) != 0;
        out.source_number = voice.source_number;
        out.adsr1 = dsp_regs[base + 5];
        out.max_adsr1_written = max_voice_adsr1_written[voice_index];
        out.adsr2 = dsp_regs[base + 6];
        out.gain = dsp_regs[base + 7];
        out.source_start = voice.source_start;
        out.loop_start = voice.loop_start;
        out.next_block_addr = voice.next_block_addr;
        out.interp_index = voice.interp_index;
        out.sample_index = voice.sample_index;
        out.decoded_sample_count = (int)voice.sample_ring.size();
        auto sampleAt = [&voice](int index) -> int16_t {
            if (voice.sample_ring.empty()) return 0;
            index = clampIndex(index, 0, voice.sample_ring.size() - 1);
            return voice.sample_ring[index];
        };
        out.tap_prev2 = sampleAt(voice.sample_index - 2);
        out.tap_prev1 = sampleAt(voice.sample_index - 1);
        out.tap_curr = sampleAt(voice.sample_index + 0);
        out.tap_next = sampleAt(voice.sample_index + 1);
        out.current_sample = voice.current_sample;
        out.pitch = (uint16_t)dsp_regs[base + 2] | ((uint16_t)(dsp_regs[base + 3] & 0x3F) << 8);
        out.envelope = voice.envelope;
        out.last_output = voice.last_output;
        out.volume_left = (int8_t)dsp_regs[base + 0];
        out.volume_right = (int8_t)dsp_regs[base + 1];
        out.envx = dsp_regs[base + 8];
        out.outx = dsp_regs[base + 9];
    }
    return info;
}

std::array<APU::TimerDebug, 3> APU::getTimerDebug() const {
    std::array<TimerDebug, 3> info{};
    for (int i = 0; i < 3; i++) {
        info[i].target = timers[i].target;
        info[i].stage2 = timers[i].stage2;
        info[i].stage3 = timers[i].stage3;
        info[i].divider = timers[i].divider;
        info[i].enabled = timers[i].enabled;
        info[i].period = timers[i].period;
    }
    return info;
}

int APU::getActiveVoiceCount() const {
    int active = 0;
    for (const Voice& voice : voices) {
        if (voice.active || voice.key_on_pending) {
            active++;
        }
    }
    return active;
}

std::size_t APU::getQueuedAudioFrames() const {
    std::lock_guard<std::mutex> lock(audio_mutex);
    return audio_count / 2;
}

void APU::reset() {
    ram.fill(0);
    cpu_to_spc.fill(0);
    spc_to_cpu.fill(0);
    dsp_regs.fill(0);
    dsp_read_counts.fill(0);
    dsp_write_counts.fill(0);
    max_voice_adsr1_written.fill(0);
    voices.fill(Voice{});
    recent_audio_mono.fill(0);
    echo_history_left.fill(0);
    echo_history_right.fill(0);
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        audio_fifo.fill(0);
        audio_read_pos = 0;
        audio_write_pos = 0;
        audio_count = 0;
    }
    audio_staging.fill(0);

    timers[0] = Timer{};
    timers[1] = Timer{};
    timers[2] = Timer{};
    timers[0].period = 128;
    timers[1].period = 128;
    timers[2].period = 16;

    pc = 0xFFC0;
    a = 0x00;
    x = 0x00;
    y = 0x00;
    sp = 0x00;
    psw = 0x00;
    test_reg = 0x0A;
    control_reg = 0xB0;
    dsp_addr = 0x00;
    ipl_rom_enabled = true;
    sleeping = false;
    stopped = false;
    uploaded_program = false;
    driver_ready = false;
    user_code_running = false;
    unsupported_opcode = false;
    uploaded_bytes = 0;
    upload_address = 0;
    execute_address = 0;
    unsupported_op = 0;
    unsupported_pc = 0;
    current_instruction_pc = 0;
    current_instruction_opcode = 0;
    instruction_pc_history.fill(0);
    instruction_opcode_history.fill(0);
    instruction_a_history.fill(0);
    instruction_x_history.fill(0);
    instruction_y_history.fill(0);
    directory_write_history.fill(DirectoryWriteDebug{});
    watched_write_history.fill(WatchedWriteDebug{});
    instruction_history_pos = 0;
    instruction_history_filled = false;
    directory_write_history_pos = 0;
    directory_write_history_filled = false;
    directory_write_sequence = 0;
    watched_write_history_pos = 0;
    watched_write_history_filled = false;
    watched_write_sequence = 0;
    write_watch_enabled = false;
    write_watch_start = 0;
    write_watch_length = 0;
    generated_audio_frames = 0;
    nonzero_audio_frames = 0;
    silent_audio_frames = 0;
    blank_loop_fallbacks = 0;
    audio_peak_sample = 0;
    audio_peak_queued_frames = 0;
    audio_underrun_frames = 0;
    audio_dropped_frames = 0;
    cpu_cycle_accumulator = 0;
    dsp_sample_counter = 0;
    dsp_counter = 0;
    dsp_key_poll_phase = true;
    sample_cycle_accumulator = 0;
    spc_cycle_budget = 0;
    recent_audio_pos = 0;
    recent_audio_filled = false;
    echo_history_pos = 0;
    audio_staging_count = 0;
    noise_lfsr = 0x4000;
    last_mixed_left = 0;
    last_mixed_right = 0;
    playback_hold_left = 0;
    playback_hold_right = 0;
    pending_kon = 0;
    pending_koff = 0;
    latched_kon = 0;
    voice_enable_mask = 0xFF;
    key_on_count = 0;
    key_off_count = 0;
    last_key_on_mask = 0;
    last_key_off_mask = 0;
    cpu_port_write_counts.fill(0);
    spc_port_write_counts.fill(0);
    last_cpu_port_writes.fill(0);
    last_spc_port_writes.fill(0);
    pending_cpu_port_write_count = 0;

    dsp_regs[0x6C] = 0xE0;
    dsp_regs[0x7C] = 0x00;

    runCycles(4096);
}

uint8_t APU::readPort(uint8_t port) const {
    return spc_to_cpu[port & 0x03];
}

void APU::writePort(uint8_t port, uint8_t data) {
    const std::size_t index = port & 0x03;
    cpu_port_write_counts[index]++;
    last_cpu_port_writes[index] = data;
    commitCpuPortWrite((uint8_t)index, data);
}

void APU::tickCPUCycles(int cpu_cycles) {
    if (cpu_cycles <= 0) return;

    auto advanceCpuCycles = [this](int cycles) {
        if (cycles <= 0) return;
        cpu_cycle_accumulator += (std::uint64_t)cycles * kSpcCyclesPerSecond;
        int spc_cycles = (int)(cpu_cycle_accumulator / EmuTiming::kCpuCyclesPerSecond);
        cpu_cycle_accumulator %= EmuTiming::kCpuCyclesPerSecond;
        runCycles(spc_cycles);
    };

    pending_cpu_port_write_count = 0;
    advanceCpuCycles(cpu_cycles);
    flushQueuedAudio();
}

void APU::endFrame() {
    flushQueuedAudio();
}

void APU::mix(int16_t* stream, int stereo_frames) {
    if (!stream || stereo_frames <= 0) return;

    std::lock_guard<std::mutex> lock(audio_mutex);
    for (int frame = 0; frame < stereo_frames; frame++) {
        const int sample_index = frame * 2;
        if (audio_count >= 2) {
            stream[sample_index + 0] = audio_fifo[audio_read_pos];
            audio_read_pos = (audio_read_pos + 1) % audio_fifo.size();
            stream[sample_index + 1] = audio_fifo[audio_read_pos];
            audio_read_pos = (audio_read_pos + 1) % audio_fifo.size();
            audio_count -= 2;
            playback_hold_left = stream[sample_index + 0];
            playback_hold_right = stream[sample_index + 1];
        } else {
            stream[sample_index + 0] = playback_hold_left;
            stream[sample_index + 1] = playback_hold_right;
            audio_underrun_frames++;
        }
    }
}

uint16_t APU::directPageAddress(uint8_t offset) const {
    return (uint16_t)((getFlag(kFlagP) ? 0x0100 : 0x0000) | offset);
}

uint16_t APU::directPageIndexedX(uint8_t offset) const {
    return directPageAddress((uint8_t)(offset + x));
}

uint16_t APU::directPageIndexedY(uint8_t offset) const {
    return directPageAddress((uint8_t)(offset + y));
}

uint16_t APU::readDirectWord(uint8_t direct_page_addr) {
    uint8_t lo = readMem(directPageAddress(direct_page_addr));
    uint8_t hi = readMem(directPageAddress((uint8_t)(direct_page_addr + 1)));
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

uint16_t APU::indirectIndexedX(uint8_t direct_page_addr) {
    return readDirectWord((uint8_t)(direct_page_addr + x));
}

uint16_t APU::indirectIndexedY(uint8_t direct_page_addr) {
    return (uint16_t)(readDirectWord(direct_page_addr) + y);
}

uint8_t APU::readMem(uint16_t address) {
    if (ipl_rom_enabled && address >= 0xFFC0) {
        return kIPLROM[address - 0xFFC0];
    }

    switch (address) {
    case 0x00F0: return 0x00;
    case 0x00F1: return 0x00;
    case 0x00F2: return dsp_addr;
    case 0x00F3: {
        const uint8_t reg = dsp_addr & 0x7F;
        dsp_read_counts[reg]++;
        if (reg == 0x4C || reg == 0x5C) {
            return 0x00;
        }
        const uint8_t value = dsp_regs[reg];
        if (reg == 0x7C) {
            dsp_regs[reg] = 0x00;
        }
        return value;
    }
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F7:
        return cpu_to_spc[address - 0x00F4];
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
        return ram[address];
    case 0x00FD:
    case 0x00FE:
    case 0x00FF: {
        const int timer_index = address - 0x00FD;
        const uint8_t value = timers[timer_index].stage3 & 0x0F;
        timers[timer_index].stage3 = 0;
        ram[address] = 0;
        return value;
    }
    default:
        return ram[address];
    }
}

void APU::updateDriverReadyState() {
    driver_ready = spc_to_cpu[0] == 0xAA &&
                   spc_to_cpu[1] == 0xBB &&
                   spc_to_cpu[2] == 0x00 &&
                   spc_to_cpu[3] == 0x00;
}

void APU::commitCpuPortWrite(uint8_t port, uint8_t data) {
    cpu_to_spc[port & 0x03] = data;
}

void APU::writeDSP(uint8_t reg, uint8_t value) {
    reg &= 0x7F;
    dsp_write_counts[reg]++;
    if (reg == 0x7C) {
        dsp_regs[reg] = 0x00;
        return;
    }
    dsp_regs[reg] = value;
    if ((reg & 0x0F) == 0x05) {
        max_voice_adsr1_written[(reg >> 4) & 0x07] =
            std::max(max_voice_adsr1_written[(reg >> 4) & 0x07], value);
    }
    switch (reg) {
    case 0x4C:
        pending_kon |= value;
        if (value != 0) {
            key_on_count++;
            last_key_on_mask = value;
        }
        break;
    case 0x5C:
        pending_koff |= value;
        if (value != 0) {
            key_off_count++;
            last_key_off_mask = value;
        }
        break;
    case 0x6C:
        if (value & 0x80) {
            for (auto& voice : voices) {
                voice.envelope_mode = EnvelopeMode::Release;
                voice.releasing = true;
            }
        }
        break;
    default:
        break;
    }
}

void APU::writeMem(uint16_t address, uint8_t data) {
    const uint8_t before = ram[address];
    const uint32_t directory_base = (uint32_t)(dsp_regs[0x5D] << 8);
    const uint32_t directory_offset = ((uint32_t)address - directory_base) & 0xFFFFu;
    const bool is_directory_write = directory_offset < 0x0400u;
    if (is_directory_write && before != data) {
        recordDirectoryWrite(address, before, data);
    }
    if (write_watch_enabled && before != data) {
        const uint32_t watch_offset = ((uint32_t)address - write_watch_start) & 0xFFFFu;
        if (watch_offset < write_watch_length) {
            recordWatchedWrite(address, before, data);
        }
    }
    ram[address] = data;

    switch (address) {
    case 0x00F0:
        test_reg = data;
        break;
    case 0x00F1: {
        control_reg = data;
        ipl_rom_enabled = (data & 0x80) != 0;

        if (data & 0x10) {
            cpu_to_spc[0] = 0x00;
            cpu_to_spc[1] = 0x00;
        }
        if (data & 0x20) {
            cpu_to_spc[2] = 0x00;
            cpu_to_spc[3] = 0x00;
        }

        for (int i = 0; i < 3; i++) {
            const bool was_enabled = timers[i].enabled;
            const bool new_enabled = (data & (1 << i)) != 0;
            if (!was_enabled && new_enabled) {
                timers[i].divider = 0;
                timers[i].stage2 = 0;
                timers[i].stage3 = 0;
            }
            timers[i].enabled = new_enabled;
        }
        break;
    }
    case 0x00F2:
        dsp_addr = data;
        break;
    case 0x00F3:
        if ((dsp_addr & 0x80) == 0) {
            writeDSP(dsp_addr, data);
        }
        break;
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F7:
        spc_to_cpu[address - 0x00F4] = data;
        spc_port_write_counts[address - 0x00F4]++;
        last_spc_port_writes[address - 0x00F4] = data;
        updateDriverReadyState();
        break;
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
        timers[address - 0x00FA].target = data;
        break;
    case 0x00FD:
    case 0x00FE:
    case 0x00FF:
        break;
    default:
        break;
    }
}

void APU::recordDirectoryWrite(uint16_t address, uint8_t before, uint8_t value) {
    DirectoryWriteDebug& entry = directory_write_history[directory_write_history_pos];
    entry.sequence = ++directory_write_sequence;
    entry.pc = current_instruction_pc;
    entry.opcode = current_instruction_opcode;
    entry.a = a;
    entry.x = x;
    entry.y = y;
    entry.psw = psw;
    entry.address = address;
    entry.before = before;
    entry.value = value;
    directory_write_history_pos =
        (directory_write_history_pos + 1) % directory_write_history.size();
    directory_write_history_filled =
        directory_write_history_filled || directory_write_history_pos == 0;
}

void APU::recordWatchedWrite(uint16_t address, uint8_t before, uint8_t value) {
    WatchedWriteDebug& entry = watched_write_history[watched_write_history_pos];
    entry.sequence = ++watched_write_sequence;
    entry.pc = current_instruction_pc;
    entry.opcode = current_instruction_opcode;
    entry.a = a;
    entry.x = x;
    entry.y = y;
    entry.psw = psw;
    entry.address = address;
    entry.before = before;
    entry.value = value;
    watched_write_history_pos =
        (watched_write_history_pos + 1) % watched_write_history.size();
    watched_write_history_filled =
        watched_write_history_filled || watched_write_history_pos == 0;
}

uint8_t APU::fetchByte() {
    uint8_t value = readMem(pc);
    pc = (uint16_t)(pc + 1);
    return value;
}

uint16_t APU::fetchWord() {
    uint8_t lo = fetchByte();
    uint8_t hi = fetchByte();
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

uint16_t APU::readWord(uint16_t address) {
    uint8_t lo = readMem(address);
    uint8_t hi = readMem((uint16_t)(address + 1));
    return (uint16_t)lo | ((uint16_t)hi << 8);
}

void APU::push(uint8_t value) {
    writeMem((uint16_t)(0x0100 | sp), value);
    sp--;
}

uint8_t APU::pop() {
    sp++;
    return readMem((uint16_t)(0x0100 | sp));
}

void APU::setFlag(uint8_t flag, bool enabled) {
    if (enabled) psw |= flag;
    else         psw &= (uint8_t)~flag;
}

bool APU::getFlag(uint8_t flag) const {
    return (psw & flag) != 0;
}

void APU::setNZ8(uint8_t value) {
    setFlag(kFlagZ, value == 0);
    setFlag(kFlagN, (value & 0x80) != 0);
}

void APU::setNZ16(uint16_t value) {
    setFlag(kFlagZ, value == 0);
    setFlag(kFlagN, (value & 0x8000) != 0);
}

void APU::tickTimers(int spc_cycles) {
    for (auto& timer : timers) {
        timer.divider += spc_cycles;
        while (timer.divider >= timer.period) {
            timer.divider -= timer.period;
            if (!timer.enabled) continue;

            timer.stage2 = (uint8_t)(timer.stage2 + 1);
            const bool hit_target = timer.target == 0 ? timer.stage2 == 0
                                                      : timer.stage2 == timer.target;
            if (hit_target) {
                timer.stage2 = 0;
                timer.stage3 = (uint8_t)((timer.stage3 + 1) & 0x0F);
            }
        }
    }
}

void APU::queueSample(int16_t left, int16_t right) {
    generated_audio_frames++;
    if (left != 0 || right != 0) {
        nonzero_audio_frames++;
    } else {
        silent_audio_frames++;
    }
    audio_peak_sample = std::max(audio_peak_sample,
                                 std::max(std::abs((int)left), std::abs((int)right)));

    recent_audio_mono[recent_audio_pos] = (int16_t)(((int)left + (int)right) / 2);
    recent_audio_pos = (recent_audio_pos + 1) % recent_audio_mono.size();
    if (recent_audio_pos == 0) {
        recent_audio_filled = true;
    }

    if (audio_staging_count + 2 > audio_staging.size()) {
        flushQueuedAudio();
    }
    audio_staging[audio_staging_count++] = left;
    audio_staging[audio_staging_count++] = right;
}

void APU::flushQueuedAudio() {
    if (audio_staging_count == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(audio_mutex);
    while (audio_count + audio_staging_count > kMaxQueuedSamples) {
        audio_read_pos = (audio_read_pos + 1) % audio_fifo.size();
        audio_read_pos = (audio_read_pos + 1) % audio_fifo.size();
        audio_count -= 2;
        audio_dropped_frames++;
    }
    for (std::size_t i = 0; i < audio_staging_count; i++) {
        audio_fifo[audio_write_pos] = audio_staging[i];
        audio_write_pos = (audio_write_pos + 1) % audio_fifo.size();
    }
    audio_count += audio_staging_count;
    audio_staging_count = 0;
    audio_peak_queued_frames = std::max(audio_peak_queued_frames, audio_count / 2);
}

bool APU::shouldTickRate(uint8_t rate) const {
    rate &= 0x1F;
    const int period = kCounterRates[rate];
    if (period <= 0) {
        return false;
    }
    if (period <= 1) {
        return true;
    }
    return ((dsp_counter + counterOffset(rate)) % period) == 0;
}

void APU::updateNoise() {
    if (!shouldTickRate(dsp_regs[0x6C] & 0x1F)) return;

    const uint16_t lfsr = (uint16_t)noise_lfsr & 0x7FFF;
    const uint16_t feedback = (uint16_t)(((lfsr << 14) ^ (lfsr << 13)) & 0x4000);
    uint16_t next = (uint16_t)((lfsr >> 1) | feedback);
    if (next == 0) next = 0x4000;
    noise_lfsr = signExtend15(next);
}

void APU::updateEnvelope(int voice_index) {
    Voice& voice = voices[voice_index];
    const int base = voice_index << 4;
    if (!voice.active) {
        voice.envelope = 0;
        return;
    }

    const uint8_t adsr1 = dsp_regs[base + 5];
    const uint8_t adsr2 = dsp_regs[base + 6];
    const uint8_t gain = dsp_regs[base + 7];

    if (voice.releasing || (dsp_regs[0x6C] & 0x80) != 0) {
        voice.envelope_mode = EnvelopeMode::Release;
        voice.envelope = voice.envelope > 8 ? (uint16_t)(voice.envelope - 8) : 0;
        voice.hidden_envelope = voice.envelope;
        if (voice.envelope == 0) {
            voice.active = false;
        }
        return;
    }

    if ((adsr1 & 0x80) != 0) {
        if (voice.envelope_mode == EnvelopeMode::Direct ||
            voice.envelope_mode == EnvelopeMode::Gain ||
            voice.envelope_mode == EnvelopeMode::Release) {
            voice.envelope_mode = EnvelopeMode::Attack;
        }

        switch (voice.envelope_mode) {
        case EnvelopeMode::Attack: {
            const uint8_t rate = (uint8_t)(((adsr1 & 0x0F) << 1) + 1);
            int envelope = voice.envelope;
            if (shouldTickRate(rate)) {
                const int step = (adsr1 & 0x0F) == 0x0F ? 1024 : 32;
                envelope += step;
            }
            voice.hidden_envelope = envelope;
            if ((unsigned)envelope > 0x07FF) {
                envelope = 0x07FF;
                voice.envelope_mode = EnvelopeMode::Decay;
            }
            voice.envelope = (uint16_t)clamp11(envelope);
            break;
        }
        case EnvelopeMode::Decay: {
            const uint8_t rate = (uint8_t)((((adsr1 >> 4) & 0x07) << 1) + 16);
            int envelope = voice.envelope;
            if (shouldTickRate(rate)) {
                envelope -= 1;
                envelope -= envelope >> 8;
            }
            voice.hidden_envelope = envelope;
            voice.envelope = (uint16_t)clamp11(envelope);
            if ((voice.envelope >> 8) == (adsr2 >> 5)) {
                voice.envelope_mode = EnvelopeMode::Sustain;
            }
            break;
        }
        case EnvelopeMode::Sustain: {
            const uint8_t rate = adsr2 & 0x1F;
            int envelope = voice.envelope;
            if (shouldTickRate(rate)) {
                envelope -= 1;
                envelope -= envelope >> 8;
            }
            voice.hidden_envelope = envelope;
            voice.envelope = (uint16_t)clamp11(envelope);
            break;
        }
        default:
            voice.envelope_mode = EnvelopeMode::Attack;
            break;
        }
        return;
    }

    if ((gain & 0x80) == 0) {
        voice.envelope_mode = EnvelopeMode::Direct;
        voice.envelope = (uint16_t)clamp11((gain & 0x7F) << 4);
        voice.hidden_envelope = voice.envelope;
        return;
    }

    voice.envelope_mode = EnvelopeMode::Gain;
    if (!shouldTickRate(gain & 0x1F)) return;

    int envelope = voice.envelope;
    switch ((gain >> 5) & 0x03) {
    case 0:
        envelope -= 32;
        break;
    case 1:
        envelope -= 1;
        envelope -= envelope >> 8;
        break;
    case 2:
        envelope += 32;
        break;
    case 3:
        envelope += voice.hidden_envelope < 0x0600 ? 32 : 8;
        break;
    }
    voice.hidden_envelope = envelope;
    voice.envelope = (uint16_t)clamp11(envelope);
}

void APU::keyOn(uint8_t voice_mask) {
    for (int voice_index = 0; voice_index < 8; voice_index++) {
        if ((voice_mask & (1u << voice_index)) != 0) {
            Voice& voice = voices[voice_index];
            voice = Voice{};
            voice.releasing = false;
            voice.key_on_pending = true;
            voice.kon_delay = 5;

            const int base = voice_index << 4;
            voice.latched_source_number = dsp_regs[base + 4];
            voice.source_number = voice.latched_source_number;
            dsp_regs[0x7C] &= (uint8_t)~(1u << voice_index);
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
        }
    }
}

void APU::keyOff(uint8_t voice_mask) {
    for (int voice_index = 0; voice_index < 8; voice_index++) {
        if ((voice_mask & (1u << voice_index)) != 0) {
            voices[voice_index].envelope_mode = EnvelopeMode::Release;
            voices[voice_index].releasing = true;
        }
    }
}

void APU::pollKeyStates() {
    dsp_key_poll_phase = !dsp_key_poll_phase;
    if (!dsp_key_poll_phase) {
        return;
    }

    pending_kon = (uint8_t)(pending_kon & (uint8_t)~latched_kon);
    const uint8_t koff = pending_koff;
    const uint8_t kon = pending_kon;
    pending_koff = 0;
    pending_kon = 0;
    latched_kon = kon;

    if (koff != 0) {
        keyOff(koff);
    }
    if (kon != 0) {
        keyOn(kon);
    }
}

void APU::resetVoice(int voice_index) {
    Voice& voice = voices[voice_index];
    const uint8_t latched_source_number = voice.latched_source_number;
    voice = Voice{};
    voice.latched_source_number = latched_source_number;

    const int base = voice_index << 4;
    const uint16_t directory_base = (uint16_t)(dsp_regs[0x5D] << 8);
    const uint8_t source_number = voice.latched_source_number;
    const uint16_t entry_addr = (uint16_t)(directory_base + source_number * 4);
    const uint16_t source_start =
        (uint16_t)ram[entry_addr] | ((uint16_t)ram[(uint16_t)(entry_addr + 1)] << 8);
    const uint16_t loop_start =
        (uint16_t)ram[(uint16_t)(entry_addr + 2)] |
        ((uint16_t)ram[(uint16_t)(entry_addr + 3)] << 8);

    voice.active = true;
    voice.releasing = false;
    voice.source_number = source_number;
    voice.source_start = source_start;
    voice.loop_start = loop_start;
    voice.next_block_addr = source_start;
    voice.interp_index = 0;
    voice.sample_index = 0;
    voice.envelope = 0;
    voice.envelope_mode = EnvelopeMode::Attack;

    const uint8_t adsr1 = dsp_regs[base + 5];
    const uint8_t gain = dsp_regs[base + 7];
    if ((adsr1 & 0x80) == 0) {
        if ((gain & 0x80) == 0) {
            voice.envelope_mode = EnvelopeMode::Direct;
            voice.envelope = (uint16_t)clamp11((gain & 0x7F) << 4);
        } else {
            voice.envelope_mode = EnvelopeMode::Gain;
        }
    }

    dsp_regs[0x7C] &= (uint8_t)~(1u << voice_index);
    dsp_regs[base + 8] = 0;
    dsp_regs[base + 9] = 0;

    if (!decodeNextBlock(voice_index)) {
        voice.active = false;
    } else if (!voice.sample_ring.empty()) {
        voice.sample_ring.push_front(0);
        voice.sample_ring.push_front(0);
        voice.sample_ring.push_front(0);
        voice.sample_index = std::min(3, (int)voice.sample_ring.size() - 1);
    }
}

bool APU::decodeNextBlock(int voice_index) {
    Voice& voice = voices[voice_index];
    if (!voice.active) return false;

    if (voice.reset_predictor_on_next_block) {
        voice.prev1 = 0;
        voice.prev2 = 0;
        voice.reset_predictor_on_next_block = false;
    }

    const uint16_t block_addr = voice.next_block_addr;
    const uint8_t header = ram[block_addr];
    const uint8_t range = header >> 4;
    const uint8_t filter = (header >> 2) & 0x03;
    const bool loop = (header & 0x02) != 0;
    const bool end = (header & 0x01) != 0;

    const uint16_t directory_base = (uint16_t)(dsp_regs[0x5D] << 8);
    const uint16_t entry_addr = (uint16_t)(directory_base + voice.source_number * 4);
    voice.loop_start =
        (uint16_t)ram[(uint16_t)(entry_addr + 2)] |
        ((uint16_t)ram[(uint16_t)(entry_addr + 3)] << 8);

    for (int i = 0; i < 16; i++) {
        const uint8_t packed = ram[(uint16_t)(block_addr + 1 + (i >> 1))];
        int nibble = ((i & 1) == 0) ? (packed >> 4) : (packed & 0x0F);
        if (nibble & 0x08) nibble -= 16;
        const int sample = decodeBrrSample(nibble, range, filter, voice.prev1, voice.prev2);
        voice.prev2 = voice.prev1;
        voice.prev1 = (int16_t)sample;
        voice.sample_ring.push_back((int16_t)sample);
    }

    voice.stop_after_block = end && !loop;
    if (end && loop) {
        uint16_t next_addr = voice.loop_start;
        bool reset_predictor = false;
        if (voice.loop_start != voice.source_start &&
            isLikelyBlankLoop(voice.loop_start) &&
            !isLikelyBlankLoop(voice.source_start)) {
            if (voice.recovered_loop_start != 0) {
                next_addr = voice.recovered_loop_start;
                reset_predictor = voice.recovered_loop_reset_predictor;
            } else {
                next_addr = resolveLoopStart(voice, reset_predictor);
                voice.recovered_loop_start = next_addr;
                voice.recovered_loop_reset_predictor = reset_predictor;
            }
            blank_loop_fallbacks++;
        }
        if (next_addr != voice.loop_start && reset_predictor) {
            voice.reset_predictor_on_next_block = true;
        }
        voice.next_block_addr = next_addr;
    } else {
        voice.next_block_addr = (uint16_t)(block_addr + 9);
    }

    if (end) {
        dsp_regs[0x7C] |= (uint8_t)(1u << voice_index);
    }

    return true;
}

int16_t APU::renderVoice(int voice_index, int pitch_mod_source_output) {
    Voice& voice = voices[voice_index];
    const int base = voice_index << 4;
    if (voice.key_on_pending) {
        dsp_regs[base + 8] = 0;
        dsp_regs[base + 9] = 0;
        if (voice.kon_delay > 0) {
            voice.kon_delay--;
            if (voice.kon_delay == 0) {
                voice.key_on_pending = false;
                resetVoice(voice_index);
            }
        }
        return 0;
    }

    if (!voice.active) {
        dsp_regs[base + 8] = 0;
        dsp_regs[base + 9] = 0;
        return 0;
    }

    updateEnvelope(voice_index);
    if (!voice.active || voice.envelope == 0) {
        if (!voice.active) {
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
            return 0;
        }
    }

    while (voice.active &&
           !voice.stop_after_block &&
           voice.sample_index >= (int)voice.sample_ring.size()) {
        if (!decodeNextBlock(voice_index)) {
            voice.active = false;
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
            return 0;
        }
    }

    if (voice.sample_index >= (int)voice.sample_ring.size()) {
        voice.active = false;
        voice.releasing = true;
        voice.envelope = 0;
        dsp_regs[base + 8] = 0;
        dsp_regs[base + 9] = 0;
        return 0;
    }

    auto sampleAt = [&voice](int index) -> int16_t {
        if (voice.sample_ring.empty()) return 0;
        index = clampIndex(index, 0, voice.sample_ring.size() - 1);
        return voice.sample_ring[index];
    };

    const auto& gaussian = GaussianTable();
    const int offset = (voice.interp_index >> 4) & 0xFF;
    const int s0 = sampleAt(voice.sample_index - 3);
    const int s1 = sampleAt(voice.sample_index - 2);
    const int s2 = sampleAt(voice.sample_index - 1);
    const int s3 = sampleAt(voice.sample_index + 0);

    int interpolated = 0;
    interpolated += (gaussian[0x0FF - offset] * s0) >> 11;
    interpolated += (gaussian[0x1FF - offset] * s1) >> 11;
    interpolated += (gaussian[0x100 + offset] * s2) >> 11;
    interpolated = (int16_t)interpolated;
    interpolated += (gaussian[0x000 + offset] * s3) >> 11;

    const bool noise_enabled = (dsp_regs[0x3D] & (1u << voice_index)) != 0;
    int sample = noise_enabled
        ? clamp16((int)signExtend15((uint16_t)noise_lfsr & 0x7FFF) * 2)
        : (clamp16(interpolated) & ~1);
    voice.current_sample = (int16_t)sample;
    const int voice_output = clamp16((sample * (int)voice.envelope) >> 11) & ~1;
    voice.last_output = (int16_t)voice_output;

    dsp_regs[base + 8] = (uint8_t)std::min(0xFF, (int)(voice.envelope >> 4));
    dsp_regs[base + 9] = (uint8_t)(voice_output >> 8);

    int pitch = (int)((uint16_t)dsp_regs[base + 2] |
                      ((uint16_t)(dsp_regs[base + 3] & 0x3F) << 8));
    if (voice_index > 0 && (dsp_regs[0x2D] & (1u << voice_index)) != 0) {
        pitch += ((pitch_mod_source_output >> 5) * pitch) >> 10;
    }
    pitch = std::max(0, pitch);

    if (voice.stop_after_block) {
        voice.releasing = true;
        voice.envelope_mode = EnvelopeMode::Release;
        voice.envelope = 0;
        voice.hidden_envelope = 0;
    }

    int next_interp = (voice.interp_index & 0x3FFF) + pitch;
    if (next_interp > 0x7FFF) {
        next_interp = 0x7FFF;
    }
    voice.interp_index = (uint16_t)next_interp;
    while (voice.interp_index >= 0x1000 && voice.active) {
        voice.interp_index -= 0x1000;
        voice.sample_index++;

        while (voice.active &&
               !voice.stop_after_block &&
               voice.sample_index >= (int)voice.sample_ring.size()) {
            if (!decodeNextBlock(voice_index)) {
                voice.active = false;
                dsp_regs[base + 8] = 0;
                dsp_regs[base + 9] = 0;
                return 0;
            }
        }

        if (voice.stop_after_block &&
            voice.sample_index >= (int)voice.sample_ring.size()) {
            voice.active = false;
            voice.releasing = true;
            voice.envelope = 0;
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
            return 0;
        }

        if (voice.sample_index > 24) {
            const int trim = voice.sample_index - 24;
            voice.sample_ring.erase_front(trim);
            voice.sample_index -= trim;
        }
    }

    return (int16_t)voice_output;
}

int16_t APU::readSample16(uint16_t address) const {
    const uint8_t lo = ram[address];
    const uint8_t hi = ram[(uint16_t)(address + 1)];
    return (int16_t)((uint16_t)lo | ((uint16_t)hi << 8));
}

bool APU::isLikelyBlankLoop(uint16_t address) const {
    for (int block = 0; block < 3; block++) {
        const uint16_t block_addr = (uint16_t)(address + block * 9);
        for (int byte_index = 0; byte_index < 9; byte_index++) {
            if (ram[(uint16_t)(block_addr + byte_index)] != 0) {
                return false;
            }
        }
    }
    return true;
}

bool APU::isUsableLoopTarget(uint16_t address) const {
    if (isLikelyBlankLoop(address)) {
        return false;
    }

    uint16_t block_addr = address;
    for (int block = 0; block < 8; block++) {
        const uint8_t header = ram[block_addr];
        bool all_zero = header == 0;
        for (int byte_index = 1; byte_index < 9; byte_index++) {
            if (ram[(uint16_t)(block_addr + byte_index)] != 0) {
                all_zero = false;
                break;
            }
        }
        if (all_zero) {
            return false;
        }

        const bool loop = (header & 0x02) != 0;
        const bool end = (header & 0x01) != 0;
        if (end) {
            return loop;
        }

        block_addr = (uint16_t)(block_addr + 9);
    }

    return true;
}

bool APU::decodeCandidateSamples(uint16_t address, int16_t prev1, int16_t prev2,
    std::array<int16_t, 8>& out_samples) const {
    out_samples.fill(0);
    if (isLikelyBlankLoop(address)) {
        return false;
    }

    const uint8_t header = ram[address];
    const uint8_t range = header >> 4;
    const uint8_t filter = (header >> 2) & 0x03;
    for (std::size_t i = 0; i < out_samples.size(); i++) {
        const uint8_t packed = ram[(uint16_t)(address + 1 + (i >> 1))];
        int nibble = ((i & 1) == 0) ? (packed >> 4) : (packed & 0x0F);
        if (nibble & 0x08) {
            nibble -= 16;
        }
        const int sample = decodeBrrSample(nibble, range, filter, prev1, prev2);
        prev2 = prev1;
        prev1 = (int16_t)sample;
        out_samples[i] = (int16_t)sample;
    }
    return true;
}

int APU::scoreLoopCandidate(const Voice& voice, uint16_t address, uint16_t requested_loop,
    bool reset_predictor) const {
    const int16_t seed_prev1 = reset_predictor ? 0 : voice.prev1;
    const int16_t seed_prev2 = reset_predictor ? 0 : voice.prev2;
    std::array<int16_t, 128> preview{};
    int preview_count = 0;
    int16_t prev1 = seed_prev1;
    int16_t prev2 = seed_prev2;
    uint16_t block_addr = address;
    while (preview_count < (int)preview.size()) {
        const uint8_t header = ram[block_addr];
        const uint8_t range = header >> 4;
        const uint8_t filter = (header >> 2) & 0x03;
        const bool loop = (header & 0x02) != 0;
        const bool end = (header & 0x01) != 0;

        for (int i = 0; i < 16 && preview_count < (int)preview.size(); i++) {
            const uint8_t packed = ram[(uint16_t)(block_addr + 1 + (i >> 1))];
            int nibble = ((i & 1) == 0) ? (packed >> 4) : (packed & 0x0F);
            if (nibble & 0x08) {
                nibble -= 16;
            }
            const int sample = decodeBrrSample(nibble, range, filter, prev1, prev2);
            prev2 = prev1;
            prev1 = (int16_t)sample;
            preview[preview_count++] = (int16_t)sample;
        }

        if (end) {
            if (!loop) {
                break;
            }
            block_addr = address;
        } else {
            block_addr = (uint16_t)(block_addr + 9);
        }
    }

    if (preview_count < 16) {
        return std::numeric_limits<int>::max();
    }

    std::array<int16_t, 4> tail{};
    const int tail_count = std::min(4, voice.sample_ring.size());
    for (int i = 0; i < tail_count; i++) {
        tail[4 - tail_count + i] = voice.sample_ring[voice.sample_ring.size() - tail_count + i];
    }

    int score = 0;
    score += std::abs((int)preview[0] - (int)tail[3]);
    score += std::abs(((int)preview[1] - (int)preview[0]) - ((int)tail[3] - (int)tail[2]));
    score += std::abs(((int)preview[2] - (int)preview[1]) - ((int)tail[2] - (int)tail[1]));
    score += std::abs(((int)preview[3] - (int)preview[2]) - ((int)tail[1] - (int)tail[0]));
    score += std::abs((int)((address & 0x00FF) - (requested_loop & 0x00FF))) * 128;

    int tail_energy = 0;
    int lively_tail_samples = 0;
    const int tail_start = std::max(0, preview_count - 32);
    for (int i = tail_start; i < preview_count; i++) {
        const int magnitude = std::abs((int)preview[i]);
        tail_energy += magnitude;
        if (magnitude > 1024) {
            lively_tail_samples++;
        }
    }
    score += std::max(0, 262144 - tail_energy);
    score += std::max(0, 32 - lively_tail_samples) * 2048;

    if ((address & 0xFF00) != (voice.source_start & 0xFF00)) {
        score += 2048;
    }
    if ((ram[address] & 0x03) != 0x03) {
        score += 8192;
    }
    if (reset_predictor) {
        score += 128;
    }
    if (preview_count < (int)preview.size()) {
        score += ((int)preview.size() - preview_count) * 256;
    }

    return score;
}

uint16_t APU::resolveLoopStart(const Voice& voice, bool& reset_predictor) const {
    reset_predictor = false;
    if (!isLikelyBlankLoop(voice.loop_start)) {
        return voice.loop_start;
    }

    struct Candidate {
        uint16_t address = 0;
        int score = std::numeric_limits<int>::max();
        bool reset = false;
    };

    Candidate best{};
    bool have_candidate = false;
    auto consider = [&](uint16_t candidate_addr, bool candidate_reset) {
        if (!isUsableLoopTarget(candidate_addr)) {
            return;
        }
        if (candidate_addr != voice.source_start &&
            (ram[candidate_addr] & 0x03) != 0x03) {
            return;
        }
        const int score = scoreLoopCandidate(voice, candidate_addr, voice.loop_start, candidate_reset);
        if (!have_candidate || score < best.score) {
            best.address = candidate_addr;
            best.score = score;
            best.reset = candidate_reset;
            have_candidate = true;
        }
    };

    consider(voice.source_start, true);

    const uint16_t same_page_loop =
        (uint16_t)((voice.source_start & 0xFF00) | (voice.loop_start & 0x00FF));
    if (same_page_loop != voice.loop_start) {
    consider(same_page_loop, true);
    }

    const uint16_t swapped_loop = (uint16_t)((voice.loop_start << 8) | (voice.loop_start >> 8));
    if (swapped_loop != voice.loop_start) {
        consider(swapped_loop, true);
    }

    const uint16_t source_page = (uint16_t)(voice.source_start & 0xFF00);
    const int source_alignment = voice.source_start % 9;
    for (int low = 0; low < 0x100; low++) {
        const uint16_t candidate_addr = (uint16_t)(source_page | low);
        if (candidate_addr == voice.source_start ||
            candidate_addr == same_page_loop ||
            candidate_addr == voice.loop_start) {
            continue;
        }
        if ((candidate_addr % 9) != source_alignment) {
            continue;
        }
        consider(candidate_addr, true);
    }

    const uint16_t directory_base = (uint16_t)(dsp_regs[0x5D] << 8);
    for (int other_source = 0; other_source < 256; other_source++) {
        if (other_source == voice.source_number) {
            continue;
        }
        const uint16_t entry_addr = (uint16_t)(directory_base + other_source * 4);
        const uint16_t other_start =
            (uint16_t)ram[entry_addr] | ((uint16_t)ram[(uint16_t)(entry_addr + 1)] << 8);
        if (other_start != voice.source_start) {
            continue;
        }

        const uint16_t other_loop =
            (uint16_t)ram[(uint16_t)(entry_addr + 2)] |
            ((uint16_t)ram[(uint16_t)(entry_addr + 3)] << 8);
        consider(other_loop, true);

        const uint16_t other_swapped_loop = (uint16_t)((other_loop << 8) | (other_loop >> 8));
        if (other_swapped_loop != other_loop) {
            consider(other_swapped_loop, true);
        }
    }

    if (!have_candidate) {
        reset_predictor = true;
        return voice.source_start;
    }

    reset_predictor = best.reset;
    return best.address;
}

void APU::writeSample16(uint16_t address, int16_t sample) {
    ram[address] = (uint8_t)(sample & 0xFF);
    ram[(uint16_t)(address + 1)] = (uint8_t)(((uint16_t)sample >> 8) & 0xFF);
}

void APU::synthesizeSamples(int spc_cycles) {
    sample_cycle_accumulator += spc_cycles;
    while (sample_cycle_accumulator >= kSpcCyclesPerSample) {
        sample_cycle_accumulator -= kSpcCyclesPerSample;
        dsp_sample_counter++;
        if (--dsp_counter < 0) {
            dsp_counter = kSimpleCounterRange - 1;
        }
        updateNoise();
        pollKeyStates();

        int mixed_left = 0;
        int mixed_right = 0;
        int echo_left = 0;
        int echo_right = 0;
        int pitch_mod_source_output = 0;
        const uint8_t echo_enable = dsp_regs[0x4D];
        for (int voice_index = 0; voice_index < 8; voice_index++) {
            const int base = voice_index << 4;
            const int voice_output = renderVoice(voice_index, pitch_mod_source_output);
            pitch_mod_source_output = voice_output;
            const int audible_output = (voice_enable_mask & (1u << voice_index)) != 0 ? voice_output : 0;
            const int8_t vol_left = (int8_t)dsp_regs[base + 0];
            const int8_t vol_right = (int8_t)dsp_regs[base + 1];
            const int voice_left = (audible_output * vol_left) >> 7;
            const int voice_right = (audible_output * vol_right) >> 7;
            mixed_left += voice_left;
            mixed_right += voice_right;
            if ((echo_enable & (1u << voice_index)) != 0) {
                echo_left += voice_left;
                echo_right += voice_right;
            }
        }

        const int8_t master_left = (int8_t)dsp_regs[0x0C];
        const int8_t master_right = (int8_t)dsp_regs[0x1C];
        const int echo_delay = std::max(4, ((int)dsp_regs[0x7D] & 0x0F) * 2048);
        const int echo_frames = std::max(1, echo_delay / 4);
        const uint16_t echo_base = (uint16_t)dsp_regs[0x6D] << 8;
        const uint16_t echo_addr = (uint16_t)(echo_base + ((dsp_sample_counter % (std::uint64_t)echo_frames) * 4));
        const int16_t echo_sample_left = readSample16(echo_addr);
        const int16_t echo_sample_right = readSample16((uint16_t)(echo_addr + 2));
        const int8_t echo_vol_left = (int8_t)dsp_regs[0x2C];
        const int8_t echo_vol_right = (int8_t)dsp_regs[0x3C];
        echo_history_left[echo_history_pos] = echo_sample_left;
        echo_history_right[echo_history_pos] = echo_sample_right;

        auto filterEcho = [this](const std::array<int16_t, 8>& history) {
            int filtered = 0;
            for (int tap = 0; tap < 8; tap++) {
                const std::size_t index = (echo_history_pos + 1 + tap) & 7;
                const int8_t coeff = (int8_t)dsp_regs[0x0F + tap * 0x10];
                filtered += ((int)history[index] * coeff) >> 7;
            }
            return clamp16(filtered);
        };

        const int filtered_echo_left = filterEcho(echo_history_left);
        const int filtered_echo_right = filterEcho(echo_history_right);
        mixed_left = ((mixed_left * master_left) >> 7) + ((filtered_echo_left * echo_vol_left) >> 7);
        mixed_right = ((mixed_right * master_right) >> 7) + ((filtered_echo_right * echo_vol_right) >> 7);

        if ((dsp_regs[0x6C] & 0x20) == 0) {
            const int8_t echo_feedback = (int8_t)dsp_regs[0x0D];
            const int feedback_left = clamp16(echo_left + ((filtered_echo_left * echo_feedback) >> 7));
            const int feedback_right = clamp16(echo_right + ((filtered_echo_right * echo_feedback) >> 7));
            writeSample16(echo_addr, (int16_t)feedback_left);
            writeSample16((uint16_t)(echo_addr + 2), (int16_t)feedback_right);
        }

        if (dsp_regs[0x6C] & 0x40) {
            mixed_left = 0;
            mixed_right = 0;
        }

        echo_history_pos = (echo_history_pos + 1) & 7;
        const int16_t final_left = (int16_t)clamp16(mixed_left);
        const int16_t final_right = (int16_t)clamp16(mixed_right);
        last_mixed_left = final_left;
        last_mixed_right = final_right;
        queueSample(final_left, final_right);
    }
}

void APU::runCycles(int spc_cycles) {
    spc_cycle_budget += spc_cycles;
    while (spc_cycle_budget > 0) {
        int used = stepInstruction();
        if (used <= 0) used = 1;
        tickTimers(used);
        synthesizeSamples(used);
        spc_cycle_budget -= used;
    }
}

int APU::stepInstruction() {
    if (unsupported_opcode || stopped || sleeping) {
        return 1;
    }

    const uint16_t instr_pc = pc;
    const uint8_t opcode = fetchByte();
    current_instruction_pc = instr_pc;
    current_instruction_opcode = opcode;
    instruction_pc_history[instruction_history_pos] = instr_pc;
    instruction_opcode_history[instruction_history_pos] = opcode;
    instruction_a_history[instruction_history_pos] = a;
    instruction_x_history[instruction_history_pos] = x;
    instruction_y_history[instruction_history_pos] = y;
    instruction_history_pos = (instruction_history_pos + 1) % kInstructionHistory;
    instruction_history_filled = instruction_history_filled || instruction_history_pos == 0;

    auto branch = [this](bool take) -> int {
        int8_t rel = (int8_t)fetchByte();
        if (take) pc = (uint16_t)(pc + rel);
        return take ? 4 : 2;
    };

    auto cmp8 = [this](uint8_t lhs, uint8_t rhs) {
        uint16_t result = (uint16_t)lhs - rhs;
        setFlag(kFlagC, lhs >= rhs);
        setFlag(kFlagZ, (uint8_t)result == 0);
        setFlag(kFlagN, (result & 0x80) != 0);
    };

    auto adc = [this](uint8_t value) {
        const int carry_in = getFlag(kFlagC) ? 1 : 0;
        const uint16_t result = (uint16_t)a + value + carry_in;
        const uint8_t out = (uint8_t)result;
        setFlag(kFlagC, result > 0xFF);
        setFlag(kFlagH, ((a & 0x0F) + (value & 0x0F) + carry_in) > 0x0F);
        setFlag(kFlagV, (~(a ^ value) & (a ^ out) & 0x80) != 0);
        a = out;
        setNZ8(a);
    };

    auto adc8 = [this](uint8_t lhs, uint8_t rhs) {
        const int carry_in = getFlag(kFlagC) ? 1 : 0;
        const uint16_t result = (uint16_t)lhs + rhs + carry_in;
        const uint8_t out = (uint8_t)result;
        setFlag(kFlagC, result > 0xFF);
        setFlag(kFlagH, ((lhs & 0x0F) + (rhs & 0x0F) + carry_in) > 0x0F);
        setFlag(kFlagV, (~(lhs ^ rhs) & (lhs ^ out) & 0x80) != 0);
        setNZ8(out);
        return out;
    };

    auto sbc = [this](uint8_t value) {
        const int borrow = getFlag(kFlagC) ? 0 : 1;
        const int result = (int)a - value - borrow;
        const uint8_t out = (uint8_t)result;
        setFlag(kFlagC, result >= 0);
        setFlag(kFlagH, ((int)(a & 0x0F) - (int)(value & 0x0F) - borrow) >= 0);
        setFlag(kFlagV, ((a ^ value) & (a ^ out) & 0x80) != 0);
        a = out;
        setNZ8(a);
    };

    auto sbc8 = [this](uint8_t lhs, uint8_t rhs) {
        const int borrow = getFlag(kFlagC) ? 0 : 1;
        const int result = (int)lhs - rhs - borrow;
        const uint8_t out = (uint8_t)result;
        setFlag(kFlagC, result >= 0);
        setFlag(kFlagH, ((int)(lhs & 0x0F) - (int)(rhs & 0x0F) - borrow) >= 0);
        setFlag(kFlagV, ((lhs ^ rhs) & (lhs ^ out) & 0x80) != 0);
        setNZ8(out);
        return out;
    };

    auto lsr8 = [this](uint8_t value) {
        setFlag(kFlagC, (value & 0x01) != 0);
        value >>= 1;
        setNZ8(value);
        return value;
    };

    auto asl8 = [this](uint8_t value) {
        setFlag(kFlagC, (value & 0x80) != 0);
        value = (uint8_t)(value << 1);
        setNZ8(value);
        return value;
    };

    auto rol8 = [this](uint8_t value) {
        const uint8_t carry_in = getFlag(kFlagC) ? 1 : 0;
        setFlag(kFlagC, (value & 0x80) != 0);
        value = (uint8_t)((value << 1) | carry_in);
        setNZ8(value);
        return value;
    };

    auto ror8 = [this](uint8_t value) {
        const uint8_t carry_in = getFlag(kFlagC) ? 0x80 : 0x00;
        setFlag(kFlagC, (value & 0x01) != 0);
        value = (uint8_t)((value >> 1) | carry_in);
        setNZ8(value);
        return value;
    };

    auto decodeBitAddress = [this](uint16_t& addr, uint8_t& bit) {
        const uint16_t operand = fetchWord();
        addr = (uint16_t)(operand & 0x1FFF);
        bit = (uint8_t)(operand >> 13);
    };

    switch (opcode) {
    case 0x00:
        return 2;
    case 0x01:
    case 0x11:
    case 0x21:
    case 0x31:
    case 0x41:
    case 0x51:
    case 0x61:
    case 0x71:
    case 0x81:
    case 0x91:
    case 0xA1:
    case 0xB1:
    case 0xC1:
    case 0xD1:
    case 0xE1:
    case 0xF1: {
        push((pc >> 8) & 0xFF);
        push(pc & 0xFF);
        const uint16_t vector = (uint16_t)(0xFFDE - (((opcode >> 4) & 0x0F) * 2));
        pc = readWord(vector);
        return 8;
    }
    case 0x02:
    case 0x12:
    case 0x22:
    case 0x32:
    case 0x42:
    case 0x52:
    case 0x62:
    case 0x72:
    case 0x82:
    case 0x92:
    case 0xA2:
    case 0xB2:
    case 0xC2:
    case 0xD2:
    case 0xE2:
    case 0xF2: {
        uint8_t direct = fetchByte();
        uint8_t value = readMem(directPageAddress(direct));
        const uint8_t bit = opcode >> 5;
        const uint8_t mask = (uint8_t)(1u << bit);
        if (opcode & 0x10) value &= (uint8_t)~mask;
        else               value |= mask;
        writeMem(directPageAddress(direct), value);
        return 4;
    }
    case 0x03:
    case 0x13:
    case 0x23:
    case 0x33:
    case 0x43:
    case 0x53:
    case 0x63:
    case 0x73:
    case 0x83:
    case 0x93:
    case 0xA3:
    case 0xB3:
    case 0xC3:
    case 0xD3:
    case 0xE3:
    case 0xF3: {
        uint8_t direct = fetchByte();
        int8_t rel = (int8_t)fetchByte();
        const uint8_t bit = opcode >> 5;
        const bool bit_set = (readMem(directPageAddress(direct)) & (1 << bit)) != 0;
        const bool branch_if_set = (opcode & 0x10) == 0;
        if (bit_set == branch_if_set) {
            pc = (uint16_t)(pc + rel);
            return 7;
        }
        return 5;
    }

    case 0x04:
        a |= readMem(directPageAddress(fetchByte()));
        setNZ8(a);
        return 3;
    case 0x05:
        a |= readMem(fetchWord());
        setNZ8(a);
        return 4;
    case 0x06:
        a |= readMem(directPageAddress(x));
        setNZ8(a);
        return 3;
    case 0x07:
        a |= readMem(indirectIndexedX(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x08:
        a |= fetchByte();
        setNZ8(a);
        return 2;
    case 0x09: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        uint8_t value = (uint8_t)(readMem(dst_addr) | readMem(directPageAddress(src_direct)));
        writeMem(dst_addr, value);
        setNZ8(value);
        return 6;
    }
    case 0x0A: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        const bool bit_set = (readMem(addr) & (uint8_t)(1u << bit)) != 0;
        setFlag(kFlagC, getFlag(kFlagC) || bit_set);
        return 5;
    }
    case 0x0B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, asl8(readMem(addr)));
        return 4;
    }
    case 0x0C: {
        uint16_t addr = fetchWord();
        writeMem(addr, asl8(readMem(addr)));
        return 5;
    }
    case 0x0E: {
        uint16_t addr = fetchWord();
        uint8_t value = readMem(addr);
        const uint8_t result = (uint8_t)(a - value);
        setFlag(kFlagZ, result == 0);
        setFlag(kFlagN, (result & 0x80) != 0);
        writeMem(addr, (uint8_t)(value | a));
        return 6;
    }
    case 0x0F:
        push((uint8_t)(pc >> 8));
        push((uint8_t)pc);
        push(psw);
        setFlag(kFlagB, true);
        setFlag(kFlagI, false);
        pc = readWord(0xFFDE);
        return 8;

    case 0x0D:
        push(psw);
        return 4;

    case 0x10:
        return branch(!getFlag(kFlagN));
    case 0x14:
        a |= readMem(directPageIndexedX(fetchByte()));
        setNZ8(a);
        return 4;
    case 0x15:
        a |= readMem((uint16_t)(fetchWord() + x));
        setNZ8(a);
        return 5;
    case 0x16:
        a |= readMem((uint16_t)(fetchWord() + y));
        setNZ8(a);
        return 5;
    case 0x17:
        a |= readMem(indirectIndexedY(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x18: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        uint8_t value = (uint8_t)(readMem(addr) | imm);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x19: {
        uint16_t x_addr = directPageAddress(x);
        uint16_t y_addr = directPageAddress(y);
        uint8_t value = (uint8_t)(readMem(x_addr) | readMem(y_addr));
        writeMem(x_addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x1A: {
        uint8_t direct = fetchByte();
        uint16_t value = readDirectWord(direct);
        value--;
        writeMem(directPageAddress(direct), value & 0xFF);
        writeMem(directPageAddress((uint8_t)(direct + 1)), value >> 8);
        setNZ16(value);
        return 6;
    }

    case 0x1C: {
        a = asl8(a);
        return 2;
    }
    case 0x1B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        writeMem(addr, asl8(readMem(addr)));
        return 5;
    }

    case 0x1D:
        x--;
        setNZ8(x);
        return 2;
    case 0x1E: {
        uint8_t value = readMem(fetchWord());
        cmp8(x, value);
        return 4;
    }
    case 0x1F: {
        uint16_t base = fetchWord();
        uint16_t target = readWord((uint16_t)(base + x));
        if (!user_code_running && base == 0x0000) {
            execute_address = target;
            user_code_running = true;
        }
        pc = target;
        return 6;
    }

    case 0x20:
        setFlag(kFlagP, false);
        return 2;
    case 0x24:
        a &= readMem(directPageAddress(fetchByte()));
        setNZ8(a);
        return 3;
    case 0x25:
        a &= readMem(fetchWord());
        setNZ8(a);
        return 4;
    case 0x26:
        a &= readMem(directPageAddress(x));
        setNZ8(a);
        return 3;
    case 0x27:
        a &= readMem(indirectIndexedX(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x28:
        a &= fetchByte();
        setNZ8(a);
        return 2;
    case 0x29: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        uint8_t value = (uint8_t)(readMem(dst_addr) & readMem(directPageAddress(src_direct)));
        writeMem(dst_addr, value);
        setNZ8(value);
        return 6;
    }
    case 0x2A: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        const bool bit_clear = (readMem(addr) & (uint8_t)(1u << bit)) == 0;
        setFlag(kFlagC, getFlag(kFlagC) || bit_clear);
        return 5;
    }
    case 0x2D:
        push(a);
        return 4;
    case 0x2E: {
        uint8_t direct = fetchByte();
        int8_t rel = (int8_t)fetchByte();
        uint8_t value = readMem(directPageAddress(direct));
        if (a != value) {
            pc = (uint16_t)(pc + rel);
            return 7;
        }
        return 5;
    }
    case 0x2F: {
        int8_t rel = (int8_t)fetchByte();
        pc = (uint16_t)(pc + rel);
        return 4;
    }
    case 0x2B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, rol8(readMem(addr)));
        return 4;
    }
    case 0x2C: {
        uint16_t addr = fetchWord();
        writeMem(addr, rol8(readMem(addr)));
        return 5;
    }

    case 0x30:
        return branch(getFlag(kFlagN));
    case 0x34:
        a &= readMem(directPageIndexedX(fetchByte()));
        setNZ8(a);
        return 4;
    case 0x35:
        a &= readMem((uint16_t)(fetchWord() + x));
        setNZ8(a);
        return 5;
    case 0x36:
        a &= readMem((uint16_t)(fetchWord() + y));
        setNZ8(a);
        return 5;
    case 0x37:
        a &= readMem(indirectIndexedY(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x38: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        uint8_t value = (uint8_t)(readMem(addr) & imm);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x39: {
        uint16_t x_addr = directPageAddress(x);
        uint16_t y_addr = directPageAddress(y);
        uint8_t value = (uint8_t)(readMem(x_addr) & readMem(y_addr));
        writeMem(x_addr, value);
        setNZ8(value);
        return 5;
    }

    case 0x3A: {
        uint8_t direct = fetchByte();
        uint16_t value = readDirectWord(direct);
        value++;
        writeMem(directPageAddress(direct), value & 0xFF);
        writeMem(directPageAddress((uint8_t)(direct + 1)), value >> 8);
        setNZ16(value);
        return 6;
    }
    case 0x3D:
        x++;
        setNZ8(x);
        return 2;
    case 0x3B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        writeMem(addr, rol8(readMem(addr)));
        return 5;
    }
    case 0x3C:
        a = rol8(a);
        return 2;
    case 0x3E: {
        uint8_t value = readMem(directPageAddress(fetchByte()));
        cmp8(x, value);
        return 3;
    }
    case 0x3F: {
        uint16_t target = fetchWord();
        push((pc >> 8) & 0xFF);
        push(pc & 0xFF);
        pc = target;
        return 8;
    }

    case 0x40:
        setFlag(kFlagP, true);
        return 2;
    case 0x44:
        a ^= readMem(directPageAddress(fetchByte()));
        setNZ8(a);
        return 3;
    case 0x45:
        a ^= readMem(fetchWord());
        setNZ8(a);
        return 4;
    case 0x46:
        a ^= readMem(directPageAddress(x));
        setNZ8(a);
        return 3;
    case 0x47:
        a ^= readMem(indirectIndexedX(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x48:
        a ^= fetchByte();
        setNZ8(a);
        return 2;
    case 0x49: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        uint8_t value = (uint8_t)(readMem(dst_addr) ^ readMem(directPageAddress(src_direct)));
        writeMem(dst_addr, value);
        setNZ8(value);
        return 6;
    }
    case 0x4A: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        const bool bit_set = (readMem(addr) & (uint8_t)(1u << bit)) != 0;
        setFlag(kFlagC, getFlag(kFlagC) && bit_set);
        return 4;
    }
    case 0x4D:
        push(x);
        return 4;
    case 0x4F: {
        const uint8_t up = fetchByte();
        push((pc >> 8) & 0xFF);
        push(pc & 0xFF);
        pc = (uint16_t)(0xFF00 | up);
        return 6;
    }
    case 0x4C: {
        uint16_t addr = fetchWord();
        writeMem(addr, lsr8(readMem(addr)));
        return 5;
    }
    case 0x4B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, lsr8(readMem(addr)));
        return 4;
    }
    case 0x4E: {
        uint16_t addr = fetchWord();
        uint8_t value = readMem(addr);
        const uint8_t result = (uint8_t)(a - value);
        setFlag(kFlagZ, result == 0);
        setFlag(kFlagN, (result & 0x80) != 0);
        writeMem(addr, (uint8_t)(value & (uint8_t)~a));
        return 6;
    }
    case 0x50:
        return branch(!getFlag(kFlagV));
    case 0x54:
        a ^= readMem(directPageIndexedX(fetchByte()));
        setNZ8(a);
        return 4;
    case 0x55:
        a ^= readMem((uint16_t)(fetchWord() + x));
        setNZ8(a);
        return 5;
    case 0x56:
        a ^= readMem((uint16_t)(fetchWord() + y));
        setNZ8(a);
        return 5;
    case 0x57:
        a ^= readMem(indirectIndexedY(fetchByte()));
        setNZ8(a);
        return 6;
    case 0x58: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        uint8_t value = (uint8_t)(readMem(addr) ^ imm);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x59: {
        uint16_t x_addr = directPageAddress(x);
        uint16_t y_addr = directPageAddress(y);
        uint8_t value = (uint8_t)(readMem(x_addr) ^ readMem(y_addr));
        writeMem(x_addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x5D:
        x = a;
        setNZ8(x);
        return 2;
    case 0x5E: {
        uint8_t value = readMem(fetchWord());
        cmp8(y, value);
        return 4;
    }
    case 0x5F:
        pc = fetchWord();
        return 3;
    case 0x5C:
        a = lsr8(a);
        return 2;
    case 0x5A: {
        uint8_t direct = fetchByte();
        uint16_t lhs = (uint16_t)a | ((uint16_t)y << 8);
        uint16_t rhs = readDirectWord(direct);
        uint32_t result = (uint32_t)lhs - rhs;
        setFlag(kFlagC, lhs >= rhs);
        setFlag(kFlagZ, (uint16_t)result == 0);
        setFlag(kFlagN, (result & 0x8000) != 0);
        return 4;
    }
    case 0x5B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        writeMem(addr, lsr8(readMem(addr)));
        return 5;
    }

    case 0x60:
        setFlag(kFlagC, false);
        return 2;
    case 0x64: {
        uint8_t value = readMem(directPageAddress(fetchByte()));
        cmp8(a, value);
        return 3;
    }
    case 0x65: {
        uint8_t value = readMem(fetchWord());
        cmp8(a, value);
        return 4;
    }
    case 0x66:
        cmp8(a, readMem(directPageAddress(x)));
        return 3;
    case 0x67:
        cmp8(a, readMem(indirectIndexedX(fetchByte())));
        return 6;
    case 0x68:
        cmp8(a, fetchByte());
        return 2;
    case 0x69: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint8_t lhs = readMem(directPageAddress(dst_direct));
        uint8_t rhs = readMem(directPageAddress(src_direct));
        cmp8(lhs, rhs);
        return 6;
    }
    case 0x6A: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        const bool bit_clear = (readMem(addr) & (uint8_t)(1u << bit)) == 0;
        setFlag(kFlagC, getFlag(kFlagC) && bit_clear);
        return 4;
    }
    case 0x6D:
        push(y);
        return 4;
    case 0x6E: {
        uint8_t direct = fetchByte();
        int8_t rel = (int8_t)fetchByte();
        uint8_t value = (uint8_t)(readMem(directPageAddress(direct)) - 1);
        writeMem(directPageAddress(direct), value);
        if (value != 0) {
            pc = (uint16_t)(pc + rel);
            return 7;
        }
        return 5;
    }
    case 0x6F: {
        uint8_t pcl = pop();
        uint8_t pch = pop();
        pc = (uint16_t)pcl | ((uint16_t)pch << 8);
        return 5;
    }
    case 0x6B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, ror8(readMem(addr)));
        return 4;
    }
    case 0x6C: {
        uint16_t addr = fetchWord();
        writeMem(addr, ror8(readMem(addr)));
        return 5;
    }
    case 0x70:
        return branch(getFlag(kFlagV));
    case 0x74:
        cmp8(a, readMem(directPageIndexedX(fetchByte())));
        return 4;
    case 0x75: {
        uint8_t value = readMem((uint16_t)(fetchWord() + x));
        cmp8(a, value);
        return 5;
    }
    case 0x76:
        cmp8(a, readMem((uint16_t)(fetchWord() + y)));
        return 5;
    case 0x77:
        cmp8(a, readMem(indirectIndexedY(fetchByte())));
        return 6;
    case 0x78: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint8_t value = readMem(directPageAddress(direct));
        cmp8(value, imm);
        return 5;
    }
    case 0x79: {
        const uint8_t lhs = readMem(directPageAddress(x));
        const uint8_t rhs = readMem(directPageAddress(y));
        cmp8(lhs, rhs);
        return 5;
    }
    case 0x7A: {
        uint8_t direct = fetchByte();
        uint16_t lhs = (uint16_t)a | ((uint16_t)y << 8);
        uint16_t rhs = readDirectWord(direct);
        setFlag(kFlagC, false);
        const uint8_t low = adc8((uint8_t)lhs, (uint8_t)rhs);
        const uint8_t high =
            adc8((uint8_t)(lhs >> 8), (uint8_t)(rhs >> 8));
        const uint16_t out = (uint16_t)low | ((uint16_t)high << 8);
        a = out & 0xFF;
        y = out >> 8;
        setNZ16(out);
        return 5;
    }
    case 0x7D:
        a = x;
        setNZ8(a);
        return 2;
    case 0x7E: {
        uint8_t value = readMem(directPageAddress(fetchByte()));
        cmp8(y, value);
        return 3;
    }
    case 0x7B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        writeMem(addr, ror8(readMem(addr)));
        return 5;
    }
    case 0x7C:
        a = ror8(a);
        return 2;
    case 0x7F: {
        psw = pop();
        uint8_t pcl = pop();
        uint8_t pch = pop();
        pc = (uint16_t)pcl | ((uint16_t)pch << 8);
        return 6;
    }

    case 0x80:
        setFlag(kFlagC, true);
        return 2;
    case 0x84:
        adc(readMem(directPageAddress(fetchByte())));
        return 3;
    case 0x85:
        adc(readMem(fetchWord()));
        return 4;
    case 0x86:
        adc(readMem(directPageAddress(x)));
        return 3;
    case 0x87:
        adc(readMem(indirectIndexedX(fetchByte())));
        return 6;
    case 0x88:
        adc(fetchByte());
        return 2;
    case 0x89: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        writeMem(dst_addr, adc8(readMem(dst_addr), readMem(directPageAddress(src_direct))));
        return 6;
    }
    case 0x8A: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        const bool bit_set = (readMem(addr) & (uint8_t)(1u << bit)) != 0;
        setFlag(kFlagC, getFlag(kFlagC) != bit_set);
        return 5;
    }
    case 0x8B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        uint8_t value = (uint8_t)(readMem(addr) - 1);
        writeMem(addr, value);
        setNZ8(value);
        return 4;
    }
    case 0x8C: {
        uint16_t addr = fetchWord();
        uint8_t value = (uint8_t)(readMem(addr) - 1);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x8D:
        y = fetchByte();
        setNZ8(y);
        return 2;
    case 0x8E:
        psw = pop();
        return 4;
    case 0x8F: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        writeMem(directPageAddress(direct), imm);
        return 5;
    }
    case 0x90:
        return branch(!getFlag(kFlagC));
    case 0x94:
        adc(readMem(directPageIndexedX(fetchByte())));
        return 4;
    case 0x95:
        adc(readMem((uint16_t)(fetchWord() + x)));
        return 5;
    case 0x96:
        adc(readMem((uint16_t)(fetchWord() + y)));
        return 5;
    case 0x97:
        adc(readMem(indirectIndexedY(fetchByte())));
        return 6;
    case 0x98: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, adc8(readMem(addr), imm));
        return 5;
    }
    case 0x99: {
        uint16_t x_addr = directPageAddress(x);
        uint16_t y_addr = directPageAddress(y);
        writeMem(x_addr, adc8(readMem(x_addr), readMem(y_addr)));
        return 5;
    }
    case 0x9A: {
        uint8_t direct = fetchByte();
        uint16_t lhs = (uint16_t)a | ((uint16_t)y << 8);
        uint16_t rhs = readDirectWord(direct);
        setFlag(kFlagC, true);
        const uint8_t low = sbc8((uint8_t)lhs, (uint8_t)rhs);
        const uint8_t high =
            sbc8((uint8_t)(lhs >> 8), (uint8_t)(rhs >> 8));
        const uint16_t out = (uint16_t)low | ((uint16_t)high << 8);
        a = out & 0xFF;
        y = out >> 8;
        setNZ16(out);
        return 5;
    }
    case 0x9F:
        a = (uint8_t)((a >> 4) | (a << 4));
        setNZ8(a);
        return 5;
    case 0x9E: {
        uint16_t ya = (uint16_t)a | ((uint16_t)y << 8);
        setFlag(kFlagV, y >= x);
        setFlag(kFlagH, (x & 0x0F) <= (y & 0x0F));
        if (y < (uint8_t)(x << 1)) {
            if (x != 0) {
                a = (uint8_t)(ya / x);
                y = (uint8_t)(ya % x);
            } else {
                a = 0xFF;
                y = (uint8_t)(ya >> 8);
            }
        } else {
            const uint16_t adjust = (uint16_t)(ya - ((uint16_t)x << 9));
            const uint16_t divisor = (uint16_t)(0x100 - x);
            a = (uint8_t)(0xFF - (adjust / divisor));
            y = (uint8_t)(x + (adjust % divisor));
        }
        setNZ8(a);
        return 12;
    }

    case 0x9B: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        uint8_t value = (uint8_t)(readMem(addr) - 1);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0x9C:
        a--;
        setNZ8(a);
        return 2;
    case 0x9D:
        x = sp;
        setNZ8(x);
        return 2;

    case 0xA4:
        sbc(readMem(directPageAddress(fetchByte())));
        return 3;
    case 0xA5:
        sbc(readMem(fetchWord()));
        return 4;
    case 0xA6:
        sbc(readMem(directPageAddress(x)));
        return 3;
    case 0xA7:
        sbc(readMem(indirectIndexedX(fetchByte())));
        return 6;
    case 0xA8:
        sbc(fetchByte());
        return 2;
    case 0xA9: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        writeMem(dst_addr, sbc8(readMem(dst_addr), readMem(directPageAddress(src_direct))));
        return 6;
    }
    case 0xA0:
        setFlag(kFlagI, true);
        return 3;
    case 0xAA: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        setFlag(kFlagC, (readMem(addr) & (uint8_t)(1u << bit)) != 0);
        return 4;
    }
    case 0xAB: {
        uint8_t direct = fetchByte();
        uint8_t value = (uint8_t)(readMem(directPageAddress(direct)) + 1);
        writeMem(directPageAddress(direct), value);
        setNZ8(value);
        return 4;
    }
    case 0xAC: {
        uint16_t addr = fetchWord();
        uint8_t value = (uint8_t)(readMem(addr) + 1);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0xAD:
        cmp8(y, fetchByte());
        return 2;
    case 0xAE:
        a = pop();
        return 4;
    case 0xAF:
        writeMem(directPageAddress(x), a);
        x++;
        return 4;

    case 0xB0:
        return branch(getFlag(kFlagC));
    case 0xB4:
        sbc(readMem(directPageIndexedX(fetchByte())));
        return 4;
    case 0xB5:
        sbc(readMem((uint16_t)(fetchWord() + x)));
        return 5;
    case 0xB6:
        sbc(readMem((uint16_t)(fetchWord() + y)));
        return 5;
    case 0xB7:
        sbc(readMem(indirectIndexedY(fetchByte())));
        return 6;
    case 0xB8: {
        uint8_t imm = fetchByte();
        uint8_t direct = fetchByte();
        uint16_t addr = directPageAddress(direct);
        writeMem(addr, sbc8(readMem(addr), imm));
        return 5;
    }
    case 0xB9: {
        uint16_t x_addr = directPageAddress(x);
        uint16_t y_addr = directPageAddress(y);
        writeMem(x_addr, sbc8(readMem(x_addr), readMem(y_addr)));
        return 5;
    }
    case 0xBA: {
        uint8_t direct = fetchByte();
        uint16_t value = readDirectWord(direct);
        a = value & 0xFF;
        y = value >> 8;
        setNZ16(value);
        return 5;
    }
    case 0xBB: {
        uint8_t direct = fetchByte();
        uint16_t addr = directPageIndexedX(direct);
        uint8_t value = (uint8_t)(readMem(addr) + 1);
        writeMem(addr, value);
        setNZ8(value);
        return 5;
    }
    case 0xBC:
        a++;
        setNZ8(a);
        return 2;
    case 0xBD:
        sp = x;
        return 2;
    case 0xBE: {
        uint16_t value = a;
        if (!getFlag(kFlagC) || value > 0x99) {
            value = (uint16_t)(value - 0x60);
            setFlag(kFlagC, false);
        } else {
            setFlag(kFlagC, true);
        }
        if (!getFlag(kFlagH) || (value & 0x0F) > 0x09) {
            value = (uint16_t)(value - 0x06);
        }
        a = (uint8_t)value;
        setNZ8(a);
        return 3;
    }
    case 0xBF:
        a = readMem(directPageAddress(x));
        x++;
        setNZ8(a);
        return 4;

    case 0xC4:
        writeMem(directPageAddress(fetchByte()), a);
        return 4;
    case 0xC0:
        setFlag(kFlagI, false);
        return 3;
    case 0xC5:
        writeMem(fetchWord(), a);
        return 5;
    case 0xC6:
        writeMem(directPageAddress(x), a);
        return 4;
    case 0xC7:
        writeMem(indirectIndexedX(fetchByte()), a);
        return 7;
    case 0xC8:
        cmp8(x, fetchByte());
        return 2;
    case 0xC9:
        writeMem(fetchWord(), x);
        return 5;
    case 0xCA: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        uint8_t value = readMem(addr);
        const uint8_t mask = (uint8_t)(1u << bit);
        value = getFlag(kFlagC) ? (uint8_t)(value | mask) : (uint8_t)(value & ~mask);
        writeMem(addr, value);
        return 6;
    }
    case 0xCB:
        writeMem(directPageAddress(fetchByte()), y);
        return 4;
    case 0xCC:
        writeMem(fetchWord(), y);
        return 5;

    case 0xCD:
        x = fetchByte();
        setNZ8(x);
        return 2;
    case 0xCE:
        x = pop();
        return 4;
    case 0xCF: {
        uint16_t result = (uint16_t)y * (uint16_t)a;
        a = result & 0xFF;
        y = result >> 8;
        setNZ8(y);
        return 9;
    }

    case 0xD0:
        return branch(!getFlag(kFlagZ));
    case 0xD4:
        writeMem(directPageIndexedX(fetchByte()), a);
        return 5;
    case 0xD5:
        writeMem((uint16_t)(fetchWord() + x), a);
        return 6;
    case 0xD6:
        writeMem((uint16_t)(fetchWord() + y), a);
        return 6;
    case 0xD7: {
        uint8_t direct = fetchByte();
        uint16_t addr = indirectIndexedY(direct);
        writeMem(addr, a);
        if (!user_code_running) {
            uploaded_program = true;
            uploaded_bytes++;
            upload_address = (uint16_t)(addr + 1);
        }
        return 7;
    }
    case 0xD8:
        writeMem(directPageAddress(fetchByte()), x);
        return 4;
    case 0xD9:
        writeMem(directPageIndexedY(fetchByte()), x);
        return 5;
    case 0xDA: {
        uint8_t direct = fetchByte();
        writeMem(directPageAddress(direct), a);
        writeMem(directPageAddress((uint8_t)(direct + 1)), y);
        return 5;
    }
    case 0xDB:
        writeMem(directPageIndexedX(fetchByte()), y);
        return 5;
    case 0xDC:
        y--;
        setNZ8(y);
        return 2;
    case 0xDD:
        a = y;
        setNZ8(a);
        return 2;
    case 0xDE: {
        uint8_t direct = fetchByte();
        int8_t rel = (int8_t)fetchByte();
        uint8_t value = readMem(directPageIndexedX(direct));
        if (a != value) {
            pc = (uint16_t)(pc + rel);
            return 8;
        }
        return 6;
    }
    case 0xDF: {
        uint16_t value = a;
        if (getFlag(kFlagC) || value > 0x99) {
            value = (uint16_t)(value + 0x60);
            setFlag(kFlagC, true);
        } else {
            setFlag(kFlagC, false);
        }
        if (getFlag(kFlagH) || (value & 0x0F) > 0x09) {
            value = (uint16_t)(value + 0x06);
        }
        a = (uint8_t)value;
        setNZ8(a);
        return 3;
    }

    case 0xE0:
        setFlag(kFlagV, false);
        setFlag(kFlagH, false);
        return 2;
    case 0xE4:
        a = readMem(directPageAddress(fetchByte()));
        setNZ8(a);
        return 3;
    case 0xE5:
        a = readMem(fetchWord());
        setNZ8(a);
        return 4;
    case 0xE6:
        a = readMem(directPageAddress(x));
        setNZ8(a);
        return 3;
    case 0xE7:
        a = readMem(indirectIndexedX(fetchByte()));
        setNZ8(a);
        return 6;
    case 0xE8:
        a = fetchByte();
        setNZ8(a);
        return 2;
    case 0xE9:
        x = readMem(fetchWord());
        setNZ8(x);
        return 4;
    case 0xEA: {
        uint16_t addr = 0;
        uint8_t bit = 0;
        decodeBitAddress(addr, bit);
        uint8_t value = readMem(addr);
        value ^= (uint8_t)(1u << bit);
        writeMem(addr, value);
        return 5;
    }
    case 0xEB:
        y = readMem(directPageAddress(fetchByte()));
        setNZ8(y);
        return 3;
    case 0xEC:
        y = readMem(fetchWord());
        setNZ8(y);
        return 4;
    case 0xED:
        setFlag(kFlagC, !getFlag(kFlagC));
        return 3;
    case 0xEE:
        y = pop();
        return 4;
    case 0xEF:
        sleeping = true;
        return 3;

    case 0xF0:
        return branch(getFlag(kFlagZ));
    case 0xF4:
        a = readMem(directPageIndexedX(fetchByte()));
        setNZ8(a);
        return 4;
    case 0xF5:
        a = readMem((uint16_t)(fetchWord() + x));
        setNZ8(a);
        return 5;
    case 0xF6:
        a = readMem((uint16_t)(fetchWord() + y));
        setNZ8(a);
        return 5;
    case 0xF7:
        a = readMem(indirectIndexedY(fetchByte()));
        setNZ8(a);
        return 6;
    case 0xF8:
        x = readMem(directPageAddress(fetchByte()));
        setNZ8(x);
        return 3;
    case 0xF9:
        x = readMem(directPageIndexedY(fetchByte()));
        setNZ8(x);
        return 4;
    case 0xFA: {
        uint8_t src_direct = fetchByte();
        uint8_t dst_direct = fetchByte();
        const uint8_t value = readMem(directPageAddress(src_direct));
        const uint16_t dst_addr = directPageAddress(dst_direct);
        (void)readMem(dst_addr);
        writeMem(dst_addr, value);
        return 5;
    }
    case 0xFB:
        y = readMem(directPageIndexedX(fetchByte()));
        setNZ8(y);
        return 4;
    case 0xFC:
        y++;
        setNZ8(y);
        return 2;
    case 0xFD:
        y = a;
        setNZ8(y);
        return 2;
    case 0xFE: {
        int8_t rel = (int8_t)fetchByte();
        y--;
        if (y != 0) {
            pc = (uint16_t)(pc + rel);
            return 6;
        }
        return 4;
    }
    case 0xFF:
        stopped = true;
        return 3;

    default:
        unsupported_opcode = true;
        unsupported_op = opcode;
        unsupported_pc = instr_pc;
        pc = instr_pc;
        return 1;
    }
}
