#include "apu.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
constexpr int kCpuCyclesPerSecond = 29781 * 60;
constexpr int kSpcCyclesPerSecond = 1024000;
constexpr int kSpcCyclesPerSample = 32;
constexpr std::size_t kMaxQueuedSamples = APU::kOutputHz * 2 * 2;
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

inline int16_t signExtend15(uint16_t value) {
    return (value & 0x4000) != 0 ? (int16_t)(value | 0x8000) : (int16_t)value;
}
}

APU::APU() {
    reset();
}

std::array<APU::VoiceDebug, 8> APU::getVoiceDebug() const {
    std::array<VoiceDebug, 8> info{};
    for (int voice_index = 0; voice_index < 8; voice_index++) {
        const Voice& voice = voices[voice_index];
        const int base = voice_index << 4;
        VoiceDebug& out = info[voice_index];
        out.active = voice.active;
        out.releasing = voice.releasing;
        out.noise_enabled = (dsp_regs[0x3D] & (1u << voice_index)) != 0;
        out.pitch_mod_enabled = voice_index > 0 && (dsp_regs[0x2D] & (1u << voice_index)) != 0;
        out.source_number = voice.source_number;
        out.source_start = voice.source_start;
        out.loop_start = voice.loop_start;
        out.next_block_addr = voice.next_block_addr;
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

void APU::reset() {
    ram.fill(0);
    cpu_to_spc.fill(0);
    spc_to_cpu.fill(0);
    dsp_regs.fill(0);
    voices.fill(Voice{});
    recent_audio_mono.fill(0);
    {
        std::lock_guard<std::mutex> lock(audio_mutex);
        audio_fifo.clear();
    }

    timers[0] = Timer{};
    timers[1] = Timer{};
    timers[2] = Timer{};
    timers[0].period = 128;
    timers[1].period = 128;
    timers[2].period = 16;
    timers[0].stage3 = 0x0F;
    timers[1].stage3 = 0x0F;
    timers[2].stage3 = 0x0F;

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
    generated_audio_frames = 0;
    nonzero_audio_frames = 0;
    audio_peak_sample = 0;
    cpu_cycle_accumulator = 0;
    dsp_sample_counter = 0;
    sample_cycle_accumulator = 0;
    recent_audio_pos = 0;
    recent_audio_filled = false;
    noise_lfsr = 0x4000;

    dsp_regs[0x6C] = 0xE0;
    dsp_regs[0x7C] = 0x00;

    // Let the IPL ROM perform its power-on init so the CPU sees $AA/$BB.
    runCycles(4096);
}

uint8_t APU::readPort(uint8_t port) const {
    return spc_to_cpu[port & 0x03];
}

void APU::writePort(uint8_t port, uint8_t data) {
    cpu_to_spc[port & 0x03] = data;
}

void APU::tickCPUCycles(int cpu_cycles) {
    if (cpu_cycles <= 0) return;

    cpu_cycle_accumulator += (std::uint64_t)cpu_cycles * kSpcCyclesPerSecond;
    int spc_cycles = (int)(cpu_cycle_accumulator / kCpuCyclesPerSecond);
    cpu_cycle_accumulator %= kCpuCyclesPerSecond;
    runCycles(spc_cycles);
}

void APU::endFrame() {
    // No frame-boundary work yet. The APU now advances from CPU timing instead.
}

void APU::mix(int16_t* stream, int stereo_frames) {
    if (!stream || stereo_frames <= 0) return;

    std::lock_guard<std::mutex> lock(audio_mutex);
    const int sample_count = stereo_frames * 2;
    for (int i = 0; i < sample_count; i++) {
        if (!audio_fifo.empty()) {
            stream[i] = audio_fifo.front();
            audio_fifo.pop_front();
        } else {
            stream[i] = 0;
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
    case 0x00F3: return dsp_regs[dsp_addr & 0x7F];
    case 0x00F4:
    case 0x00F5:
    case 0x00F6:
    case 0x00F7:
        return cpu_to_spc[address - 0x00F4];
    case 0x00FA:
    case 0x00FB:
    case 0x00FC:
        return 0x00;
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

void APU::writeDSP(uint8_t reg, uint8_t value) {
    reg &= 0x7F;
    if (reg == 0x7C) {
        dsp_regs[reg] = 0x00;
        return;
    }
    dsp_regs[reg] = value;
    switch (reg) {
    case 0x4C:
        keyOn(value);
        break;
    case 0x5C:
        keyOff(value);
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
    ram[address] = data;

    switch (address) {
    case 0x00F0:
        test_reg = data;
        break;
    case 0x00F1: {
        control_reg = data;
        ipl_rom_enabled = (data & 0x80) != 0;

        if (data & 0x20) {
            cpu_to_spc[0] = 0x00;
            cpu_to_spc[1] = 0x00;
        }
        if (data & 0x10) {
            cpu_to_spc[2] = 0x00;
            cpu_to_spc[3] = 0x00;
        }

        for (int i = 0; i < 3; i++) {
            const bool new_enabled = (data & (1 << i)) != 0;
            if (!timers[i].enabled && new_enabled) {
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
            if (timer.stage2 == timer.target) {
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
    }
    audio_peak_sample = std::max(audio_peak_sample,
                                 std::max(std::abs((int)left), std::abs((int)right)));

    std::lock_guard<std::mutex> lock(audio_mutex);
    while (audio_fifo.size() > kMaxQueuedSamples) {
        audio_fifo.pop_front();
    }
    audio_fifo.push_back(left);
    audio_fifo.push_back(right);
}

bool APU::shouldTickRate(uint8_t rate) const {
    rate &= 0x1F;
    if (rate == 0) return false;

    const int period = kCounterRates[rate];
    if (period <= 1) return true;
    return (dsp_sample_counter % (std::uint64_t)period) == 0;
}

void APU::updateNoise() {
    if (!shouldTickRate(dsp_regs[0x6C] & 0x1F)) return;

    const uint16_t lfsr = (uint16_t)noise_lfsr & 0x7FFF;
    const uint16_t feedback = ((lfsr >> 14) ^ (lfsr >> 13)) & 0x01;
    uint16_t next = (uint16_t)(((lfsr << 1) & 0x7FFE) | feedback);
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
            if (shouldTickRate(rate)) {
                const int step = (adsr1 & 0x0F) == 0x0F ? 1024 : 32;
                voice.envelope = (uint16_t)clamp11((int)voice.envelope + step);
            }
            if (voice.envelope >= 0x07FF) {
                voice.envelope = 0x07FF;
                voice.envelope_mode = EnvelopeMode::Decay;
            }
            break;
        }
        case EnvelopeMode::Decay: {
            const uint8_t rate = (uint8_t)((((adsr1 >> 4) & 0x07) << 1) + 16);
            if (shouldTickRate(rate)) {
                int envelope = voice.envelope;
                envelope -= 1;
                envelope -= envelope >> 8;
                voice.envelope = (uint16_t)clamp11(envelope);
            }
            const uint16_t sustain_level =
                (uint16_t)std::min(0x07FF, (((adsr2 >> 5) & 0x07) + 1) << 8);
            if (voice.envelope <= sustain_level) {
                voice.envelope_mode = EnvelopeMode::Sustain;
            }
            break;
        }
        case EnvelopeMode::Sustain: {
            const uint8_t rate = adsr2 & 0x1F;
            if (shouldTickRate(rate)) {
                int envelope = voice.envelope;
                envelope -= 1;
                envelope -= envelope >> 8;
                voice.envelope = (uint16_t)clamp11(envelope);
            }
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
        envelope += envelope < 0x0600 ? 32 : 8;
        break;
    }
    voice.envelope = (uint16_t)clamp11(envelope);
}

void APU::keyOn(uint8_t voice_mask) {
    for (int voice_index = 0; voice_index < 8; voice_index++) {
        if ((voice_mask & (1u << voice_index)) != 0) {
            resetVoice(voice_index);
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

void APU::resetVoice(int voice_index) {
    Voice& voice = voices[voice_index];
    voice = Voice{};

    const int base = voice_index << 4;
    const uint16_t directory_base = (uint16_t)(dsp_regs[0x5D] << 8);
    const uint8_t source_number = dsp_regs[base + 4];
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
    }
}

bool APU::decodeNextBlock(int voice_index) {
    Voice& voice = voices[voice_index];
    if (!voice.active) return false;

    const uint16_t block_addr = voice.next_block_addr;
    const uint8_t header = ram[block_addr];
    const uint8_t range = header >> 4;
    const uint8_t filter = (header >> 2) & 0x03;
    const bool loop = (header & 0x02) != 0;
    const bool end = (header & 0x01) != 0;

    for (int i = 0; i < 16; i++) {
        const uint8_t packed = ram[(uint16_t)(block_addr + 1 + (i >> 1))];
        int nibble = ((i & 1) == 0) ? (packed >> 4) : (packed & 0x0F);
        if (nibble & 0x08) nibble -= 16;

        int sample = (range <= 12) ? ((nibble << range) >> 1)
                                   : (nibble < 0 ? -2048 : 0);

        switch (filter) {
        case 1:
            sample += voice.prev1 + ((-voice.prev1) >> 4);
            break;
        case 2:
            sample += (voice.prev1 << 1) +
                      ((-((voice.prev1 << 1) + voice.prev1)) >> 5) -
                      voice.prev2 + (voice.prev2 >> 4);
            break;
        case 3:
            sample += (voice.prev1 << 1) +
                      ((-(voice.prev1 + (voice.prev1 << 2) + (voice.prev1 << 3))) >> 6) -
                      voice.prev2 + (((voice.prev2 << 1) + voice.prev2) >> 4);
            break;
        default:
            break;
        }

        sample = clamp16(sample) & ~1;
        voice.prev2 = voice.prev1;
        voice.prev1 = (int16_t)sample;
        voice.decoded[i] = (int16_t)sample;
    }

    voice.sample_index = 0;
    voice.stop_after_block = end && !loop;
    voice.next_block_addr = end && loop ? voice.loop_start
                                        : (uint16_t)(block_addr + 9);

    if (end) {
        dsp_regs[0x7C] |= (uint8_t)(1u << voice_index);
        if (!loop) {
            voice.releasing = true;
        }
    }

    return true;
}

int16_t APU::renderVoice(int voice_index) {
    Voice& voice = voices[voice_index];
    const int base = voice_index << 4;
    if (!voice.active) {
        dsp_regs[base + 8] = 0;
        dsp_regs[base + 9] = 0;
        return 0;
    }

    if (voice.sample_index < 0 || voice.sample_index >= 16) {
        if (!decodeNextBlock(voice_index)) {
            voice.active = false;
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
            return 0;
        }
    }

    updateEnvelope(voice_index);
    if (!voice.active || voice.envelope == 0) {
        if (!voice.active) {
            dsp_regs[base + 8] = 0;
            dsp_regs[base + 9] = 0;
            return 0;
        }
    }

    const bool noise_enabled = (dsp_regs[0x3D] & (1u << voice_index)) != 0;
    int sample = noise_enabled ? (int)signExtend15((uint16_t)noise_lfsr & 0x7FFF)
                               : (int)voice.decoded[voice.sample_index];
    const int voice_output = clamp16((sample * (int)voice.envelope) >> 11);
    voice.last_output = (int16_t)voice_output;

    dsp_regs[base + 8] = (uint8_t)std::min(0xFF, (int)(voice.envelope >> 4));
    dsp_regs[base + 9] = (uint8_t)(voice_output >> 8);

    if (!noise_enabled) {
        int pitch = (int)((uint16_t)dsp_regs[base + 2] |
                          ((uint16_t)(dsp_regs[base + 3] & 0x3F) << 8));
        if (voice_index > 0 && (dsp_regs[0x2D] & (1u << voice_index)) != 0) {
            const int8_t mod = (int8_t)dsp_regs[((voice_index - 1) << 4) + 9];
            pitch += (pitch * mod) >> 7;
        }
        pitch = std::clamp(pitch, 0, 0x3FFF);

        voice.interp_index = (uint16_t)(voice.interp_index + pitch);
        while (voice.interp_index >= 0x1000 && voice.active) {
            voice.interp_index -= 0x1000;
            voice.sample_index++;
            if (voice.sample_index >= 16) {
                if (voice.stop_after_block) {
                    voice.releasing = true;
                    voice.sample_index = 15;
                    break;
                }
                if (!decodeNextBlock(voice_index)) {
                    voice.active = false;
                    dsp_regs[base + 8] = 0;
                    dsp_regs[base + 9] = 0;
                    return 0;
                }
            }
        }
    }

    return (int16_t)voice_output;
}

void APU::synthesizeSamples(int spc_cycles) {
    sample_cycle_accumulator += spc_cycles;
    while (sample_cycle_accumulator >= kSpcCyclesPerSample) {
        sample_cycle_accumulator -= kSpcCyclesPerSample;
        dsp_sample_counter++;
        updateNoise();

        int mixed_left = 0;
        int mixed_right = 0;
        for (int voice_index = 0; voice_index < 8; voice_index++) {
            const int base = voice_index << 4;
            const int voice_output = renderVoice(voice_index);
            const int8_t vol_left = (int8_t)dsp_regs[base + 0];
            const int8_t vol_right = (int8_t)dsp_regs[base + 1];
            mixed_left = clamp16(mixed_left + ((voice_output * vol_left) >> 7));
            mixed_right = clamp16(mixed_right + ((voice_output * vol_right) >> 7));
        }

        const int8_t master_left = (int8_t)dsp_regs[0x0C];
        const int8_t master_right = (int8_t)dsp_regs[0x1C];
        mixed_left = clamp16((mixed_left * master_left) >> 7);
        mixed_right = clamp16((mixed_right * master_right) >> 7);

        if (dsp_regs[0x6C] & 0x40) {
            mixed_left = 0;
            mixed_right = 0;
        }

        queueSample((int16_t)mixed_left, (int16_t)mixed_right);
    }
}

void APU::runCycles(int spc_cycles) {
    while (spc_cycles > 0) {
        int used = stepInstruction();
        if (used <= 0) used = 1;
        tickTimers(used);
        synthesizeSamples(used);
        spc_cycles -= used;
    }
}

int APU::stepInstruction() {
    if (unsupported_opcode || stopped || sleeping) {
        return 1;
    }

    const uint16_t instr_pc = pc;
    const uint8_t opcode = fetchByte();

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

    switch (opcode) {
    case 0x00:
        return 2;
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
        uint8_t dst_direct = fetchByte();
        uint8_t src_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        uint8_t value = (uint8_t)(readMem(dst_addr) | readMem(directPageAddress(src_direct)));
        writeMem(dst_addr, value);
        setNZ8(value);
        return 6;
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
        cmp8(a, value);
        writeMem(addr, (uint8_t)(value | a));
        return 6;
    }
    case 0x0F:
        push((uint8_t)(pc >> 8));
        push((uint8_t)pc);
        push(psw);
        setFlag(kFlagB, true);
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
        uint8_t dst_direct = fetchByte();
        uint8_t src_direct = fetchByte();
        uint16_t dst_addr = directPageAddress(dst_direct);
        uint8_t value = (uint8_t)(readMem(dst_addr) ^ readMem(directPageAddress(src_direct)));
        writeMem(dst_addr, value);
        setNZ8(value);
        return 6;
    }
    case 0x4D:
        push(x);
        return 4;
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
        cmp8(a, value);
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
        uint8_t dst_direct = fetchByte();
        uint8_t src_direct = fetchByte();
        uint8_t lhs = readMem(directPageAddress(dst_direct));
        uint8_t rhs = readMem(directPageAddress(src_direct));
        cmp8(lhs, rhs);
        return 6;
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
    case 0x7A: {
        uint8_t direct = fetchByte();
        uint16_t lhs = (uint16_t)a | ((uint16_t)y << 8);
        uint16_t rhs = readDirectWord(direct);
        uint32_t result = (uint32_t)lhs + rhs;
        uint16_t out = (uint16_t)result;
        setFlag(kFlagC, result > 0xFFFF);
        setFlag(kFlagH, ((lhs & 0x0FFF) + (rhs & 0x0FFF)) > 0x0FFF);
        setFlag(kFlagV, (~(lhs ^ rhs) & (lhs ^ out) & 0x8000) != 0);
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
        int result = (int)lhs - rhs;
        uint16_t out = (uint16_t)result;
        setFlag(kFlagC, result >= 0);
        setFlag(kFlagH, ((int)(lhs & 0x0FFF) - (int)(rhs & 0x0FFF)) >= 0);
        setFlag(kFlagV, ((lhs ^ rhs) & (lhs ^ out) & 0x8000) != 0);
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
        if (x != 0) {
            a = (uint8_t)(ya / x);
            y = (uint8_t)(ya % x);
        } else {
            a = 0xFF;
            y = 0x00;
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
        setNZ8(a);
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
    case 0xBF:
        a = readMem(directPageAddress(x));
        x++;
        setNZ8(a);
        return 4;

    case 0xC4:
        writeMem(directPageAddress(fetchByte()), a);
        return 4;
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
        setNZ8(x);
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
        setNZ8(y);
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
        setNZ8(y);
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
