#include "cpu.hpp"
#include "bus.hpp"
#include "apu.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include "dsp1.hpp"
#include "launcher.hpp"
#include "manipulator.hpp"
#include "sa1.hpp"
#include "superfx.hpp"
#include "timing.hpp"
#include "visualizer.hpp"
#include <iostream>
#include <fstream>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>
#include <string>
#include <algorithm>
#include <cmath>
#include <csignal>
#include <limits>
#include <SDL2/SDL.h>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <SDL2/SDL_syswm.h>
#endif

static void SDLAudioCallback(void* userdata, Uint8* stream, int len) {
    auto* apu = static_cast<APU*>(userdata);
    if (!apu) { std::memset(stream, 0, len); return; }
    apu->mix(reinterpret_cast<int16_t*>(stream), len / (int)sizeof(int16_t) / 2);
}

static void WriteLE16(std::ofstream& out, uint16_t value) {
    const char bytes[2] = {
        (char)(value & 0xFF),
        (char)((value >> 8) & 0xFF),
    };
    out.write(bytes, 2);
}

static void WriteLE32(std::ofstream& out, uint32_t value) {
    const char bytes[4] = {
        (char)(value & 0xFF),
        (char)((value >> 8) & 0xFF),
        (char)((value >> 16) & 0xFF),
        (char)((value >> 24) & 0xFF),
    };
    out.write(bytes, 4);
}

static void PresentMainTexture(SDL_Renderer* renderer, SDL_Texture* texture) {
    if (!renderer || !texture) return;
    SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
    SDL_RenderClear(renderer);
    SDL_RenderCopy(renderer, texture, NULL, NULL);
    SDL_RenderPresent(renderer);
}

static bool WriteFramebufferBMP(const std::string& path, const uint32_t* framebuffer) {
    if (!framebuffer || path.empty()) return false;

    std::ofstream out(path, std::ios::binary);
    if (!out) return false;

    constexpr int width = 256;
    constexpr int height = 224;
    constexpr uint32_t pixel_bytes = width * height * 4;
    constexpr uint32_t file_size = 14 + 40 + pixel_bytes;

    out.put('B');
    out.put('M');
    WriteLE32(out, file_size);
    WriteLE16(out, 0);
    WriteLE16(out, 0);
    WriteLE32(out, 14 + 40);

    WriteLE32(out, 40);
    WriteLE32(out, width);
    WriteLE32(out, height);
    WriteLE16(out, 1);
    WriteLE16(out, 32);
    WriteLE32(out, 0);
    WriteLE32(out, pixel_bytes);
    WriteLE32(out, 2835);
    WriteLE32(out, 2835);
    WriteLE32(out, 0);
    WriteLE32(out, 0);

    for (int y = height - 1; y >= 0; y--) {
        for (int x = 0; x < width; x++) {
            const uint32_t pixel = framebuffer[y * width + x];
            const char bgra[4] = {
                (char)(pixel & 0xFF),
                (char)((pixel >> 8) & 0xFF),
                (char)((pixel >> 16) & 0xFF),
                (char)((pixel >> 24) & 0xFF),
            };
            out.write(bgra, 4);
        }
    }
    return (bool)out;
}

struct PadScriptRange {
    int start_frame = 0;
    int end_frame = 0;
    uint16_t mask = 0;
};

static bool ParsePadScriptMask(const std::string& text, uint16_t& mask) {
    if (text.empty()) return false;

    char* end = nullptr;
    const unsigned long value = std::strtoul(text.c_str(), &end, 16);
    if (!end || *end != '\0' || value > 0xFFFFUL) return false;

    mask = static_cast<uint16_t>(value);
    return true;
}

static bool ParsePadScript(const std::string& spec, std::vector<PadScriptRange>& script) {
    script.clear();
    if (spec.empty()) return true;

    size_t cursor = 0;
    while (cursor < spec.size()) {
        size_t next = spec.find(',', cursor);
        if (next == std::string::npos) next = spec.size();

        std::string token = spec.substr(cursor, next - cursor);
        token.erase(std::remove_if(token.begin(), token.end(), [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        }), token.end());

        if (!token.empty()) {
            const size_t colon = token.find(':');
            if (colon == std::string::npos) return false;

            const std::string range_text = token.substr(0, colon);
            const std::string mask_text = token.substr(colon + 1);

            int start = 0;
            int end = 0;
            const size_t dash = range_text.find('-');
            try {
                if (dash == std::string::npos) {
                    start = end = std::stoi(range_text);
                } else {
                    start = std::stoi(range_text.substr(0, dash));
                    end = std::stoi(range_text.substr(dash + 1));
                }
            } catch (...) {
                return false;
            }

            if (start < 0 || end < start) return false;

            uint16_t mask = 0;
            if (!ParsePadScriptMask(mask_text, mask)) return false;
            script.push_back({start, end, mask});
        }

        cursor = next + 1;
    }

    return true;
}

static uint16_t PadScriptMaskForFrame(const std::vector<PadScriptRange>& script, int frame) {
    uint16_t mask = 0;
    for (const auto& range : script) {
        if (frame >= range.start_frame && frame <= range.end_frame) {
            mask |= range.mask;
        }
    }
    return mask;
}

class System {
public:
    System() : bus(&cpu, &ppu, &apu) {}

    bool loadROM(const std::string& filepath) {
        saveSRAM();
        delete cartridge;
        cartridge = nullptr;
        rom_filepath = filepath;
        Cartridge* next = new Cartridge(filepath);
        if (next->isLoaded()) {
            cartridge = next;
            bus.insertCartridge(cartridge);
            std::string srm = filepath + ".srm";
            cartridge->loadSRAM(srm);
            bus.resetDebugHistory();
            apu.reset();
            cpu.reset();
            std::cout << "=== Super Furamicom Powered On ===\n";
            if (cartridge->hasSuperFX()) {
                std::cout << "[SFX] Super FX GSU execution core active.\n";
            }
            if (cartridge->hasSA1()) {
                std::cout << "[SA1] SA-1 65816 coprocessor active (10.74 MHz, 2KB I-RAM, "
                          << cartridge->getSA1()->getBWRAMSize() / 1024 << "KB BW-RAM).\n";
            }
            if (cartridge->hasDSP1()) {
                std::cout << "[DSP1] DSP-1 command/status interface active.\n";
            }
            return true;
        } else {
            delete next;
            std::cerr << "Failed to load ROM: " << filepath << "\n";
            rom_filepath.clear();
            return false;
        }
    }

    void enableTrace(const std::string& path) { cpu.enableTrace(path); }

    void stepFrame() {
        ppu.setVBlank(false);
        bus.endVBlank();
        ppu.beginFrame();
        bus.beginFrame();
        ppu.setHBlank(false);

        int frame_elapsed = 0;
        auto runTo = [&](int target_cycles) {
            while (frame_elapsed < target_cycles) {
                int used = cpu.step();
                frame_elapsed += used;
                bus.tick(used);
                apu.tickCPUCycles(used);
                if (bus.consumeNMI()) cpu.nmi();
                if (bus.consumeIRQ()) cpu.irq();
                if (cpu.isHalted()) break;
            }
        };

        for (int scanline = 0; scanline < EmuTiming::kScanlinesPerFrame && !cpu.isHalted(); scanline++) {
            if (scanline == EmuTiming::kVisibleScanlines) {
                ppu.endFrame();
                ppu.setVBlank(true);
                bus.startVBlank();
            }

            bus.beginScanline(scanline);
            if (bus.consumeIRQ()) cpu.irq();

            if (scanline < EmuTiming::kVisibleScanlines) {
                ppu.renderScanline(scanline);
            }

            const int line_start = (scanline * EmuTiming::kCpuCyclesPerFrame) / EmuTiming::kScanlinesPerFrame;
            const int line_end = ((scanline + 1) * EmuTiming::kCpuCyclesPerFrame) / EmuTiming::kScanlinesPerFrame;
            const int line_cycles = line_end - line_start;
            const int hblank_start = line_start + (line_cycles * 4) / 5;

            ppu.setHBlank(false);
            runTo(hblank_start);

            ppu.setHBlank(true);
            bus.beginHBlank(scanline);
            if (bus.consumeIRQ()) cpu.irq();
            if (scanline + 1 < EmuTiming::kVisibleScanlines) {
                bus.stepHDMA();
            }
            runTo(line_end);
        }

        apu.endFrame();
    }

    void setJoypad(uint16_t pad1, uint16_t pad2 = 0) { bus.setJoypadState(pad1, pad2); }
    void saveSRAM() {
        if (cartridge && cartridge->hasSRAM()) {
            std::string srm = rom_filepath + ".srm";
            cartridge->saveSRAM(srm);
        }
    }

    bool isCPUHalted() const { return cpu.isHalted(); }
    const uint32_t* getFramebuffer() const { return ppu.getFramebuffer(); }
    const CPU& getCPU() const { return cpu; }
    PPU& getPPU() { return ppu; }
    const PPU& getPPU() const { return ppu; }
    APU* getAPU() { return &apu; }
    const APU* getAPU() const { return &apu; }

    void dumpDebugState(std::ostream& out) const {
        const auto& cpu_ports = apu.getCpuToSPCPorts();
        const auto& spc_ports = apu.getSPCToCpuPorts();
        const auto& cpu_port_writes = apu.getCpuPortWriteCounts();
        const auto& spc_port_writes = apu.getSpcPortWriteCounts();
        const auto& last_cpu_writes = apu.getLastCpuPortWrites();
        const auto& last_spc_writes = apu.getLastSpcPortWrites();
        out << "[CPU] PC=" << std::hex << cpu.getProgramCounter()
            << " A=" << cpu.getA() << " X=" << cpu.getX() << " Y=" << cpu.getY()
            << " SP=" << cpu.getSP() << " D=" << cpu.getD()
            << " DB=" << (unsigned)cpu.getDB()
            << " P=" << (unsigned)cpu.getP()
            << " last_op=$" << (unsigned)cpu.getLastOpcode()
            << " E=" << cpu.isEmulationMode()
            << " halted=" << std::dec << cpu.isHalted() << "\n";
        const auto& pc_history = cpu.getPCHistory();
        const auto& a_history = cpu.getAHistory();
        const auto& x_history = cpu.getXHistory();
        const auto& y_history = cpu.getYHistory();
        const std::size_t history_count = cpu.hasHistoryWrapped()
            ? CPU::kDebugHistory
            : cpu.getHistoryPos();
        if (history_count != 0) {
            const std::size_t tail_count = std::min<std::size_t>(16, history_count);
            const std::size_t start = (cpu.getHistoryPos() + CPU::kDebugHistory - tail_count) % CPU::kDebugHistory;
            out << "[CPU-HIST]";
            for (std::size_t i = 0; i < tail_count; i++) {
                const std::size_t index = (start + i) % CPU::kDebugHistory;
                out << (i == 0 ? " " : " ") << std::hex << pc_history[index];
            }
            out << std::dec << "\n";
            out << "[CPU-HIST-REGS]";
            for (std::size_t i = 0; i < tail_count; i++) {
                const std::size_t index = (start + i) % CPU::kDebugHistory;
                out << (i == 0 ? " " : " ")
                    << std::hex << pc_history[index]
                    << ":a=" << a_history[index]
                    << ":x=" << x_history[index]
                    << ":y=" << y_history[index];
            }
            out << std::dec << "\n";
        }
        if (cartridge) {
            out << "[CART] mapper=" << cartridge->getMapperName()
                << " superfx=" << cartridge->hasSuperFX()
                << " dsp1=" << cartridge->hasDSP1()
                << " map_mode=$" << std::hex << (unsigned)cartridge->getMapModeByte()
                << " chipset=$" << (unsigned)cartridge->getChipsetByte()
                << " rom_size=$" << (unsigned)cartridge->getROMSizeByte()
                << " ram_size=$" << (unsigned)cartridge->getRAMSizeByte()
                << std::dec
                << " cart_ram_kb=" << (cartridge->getRAMSize() / 1024)
                << "\n";
            if (cartridge->hasSuperFX()) {
                const SuperFX* superfx = cartridge->getSuperFX();
                const auto& regs = superfx->getRegisters();
                const auto& cache = superfx->getCache();
                const std::size_t cache_nonzero = std::count_if(
                    cache.begin(), cache.end(), [](uint8_t value) { return value != 0; });
                out << "[SUPERFX] rev=" << superfx->getRevisionName()
                    << " running=" << superfx->isRunning()
                    << " launches=" << superfx->getLaunchCount()
                    << " last_pc=$" << std::hex << superfx->getLastLaunchPC()
                    << " sfr=$" << superfx->getSFR()
                    << " pbr=$" << (unsigned)superfx->getPBR()
                    << " rombr=$" << (unsigned)superfx->getROMBR()
                    << " rambr=$" << (unsigned)superfx->getRAMBR()
                    << " scbr=$" << (unsigned)superfx->getSCBR()
                    << " scmr=$" << (unsigned)superfx->getSCMR()
                    << " cfgr=$" << (unsigned)superfx->getCFGR()
                    << " clsr=$" << (unsigned)superfx->getCLSR()
                    << " cbr=$" << superfx->getCBR()
                    << std::dec
                    << " cache_nonzero=" << cache_nonzero
                    << " snes_rom_access=" << superfx->snesCanAccessROM()
                    << " snes_ram_access=" << superfx->snesCanAccessRAM()
                    << "\n";
                out << "[SUPERFX-R0-7]";
                for (int i = 0; i < 8; i++) {
                    out << (i == 0 ? " " : " ")
                        << "r" << i << "=$" << std::hex << regs[i];
                }
                out << std::dec << "\n";
                out << "[SUPERFX-R8-15]";
                for (int i = 8; i < 16; i++) {
                    out << (i == 8 ? " " : " ")
                        << "r" << i << "=$" << std::hex << regs[i];
                }
                out << std::dec << "\n";
            }
            if (cartridge->hasDSP1()) {
                const DSP1* dsp1 = cartridge->getDSP1();
                out << "[DSP1] map=" << dsp1->getMapTypeName()
                    << " boundary=$" << std::hex << dsp1->getBoundary()
                    << " waiting=" << std::dec << dsp1->isWaitingForCommand()
                    << " command=$" << std::hex << (unsigned)dsp1->getCurrentCommand()
                    << " last_command=$" << (unsigned)dsp1->getLastCommand()
                    << std::dec
                    << " writes=" << dsp1->getDataWriteCount()
                    << " data_reads=" << dsp1->getDataReadCount()
                    << " status_reads=" << dsp1->getStatusReadCount()
                    << " cmds=" << dsp1->getCommandCount()
                    << " unsupported=" << dsp1->getUnsupportedCommandCount()
                    << " in_pending=" << dsp1->getPendingInputBytes()
                    << " out_pending=" << dsp1->getPendingOutputBytes()
                    << "\n";
                out << "[DSP1-RECENT]";
                const auto& recent = dsp1->getRecentCommands();
                for (uint8_t cmd : recent) {
                    out << " $" << std::hex << (unsigned)cmd;
                }
                out << std::dec << "\n";
                out << "[DSP1-IN]";
                const auto& in_words = dsp1->getLastInputWords();
                for (std::size_t i = 0; i < dsp1->getLastInputWordCount(); i++) {
                    out << (i == 0 ? " " : " ") << "$" << std::hex << in_words[i];
                }
                out << std::dec << "\n";
                out << "[DSP1-OUT]";
                const auto& out_words = dsp1->getLastOutputWords();
                for (std::size_t i = 0; i < dsp1->getLastOutputWordCount(); i++) {
                    out << (i == 0 ? " " : " ") << "$" << std::hex << out_words[i];
                }
                out << std::dec << "\n";
                dsp1->dumpProjectionState(out);
                out << "[DSP1-HIST]\n";
                const auto& trace = dsp1->getCommandTrace();
                const std::size_t trace_count = trace.size();
                const std::size_t trace_pos = dsp1->getCommandTracePos();
                for (std::size_t i = 0; i < trace_count; i++) {
                    const std::size_t index = (trace_pos + i) % trace_count;
                    const auto& entry = trace[index];
                    if (entry.command == 0 && entry.input_word_count == 0 && entry.output_word_count == 0) {
                        continue;
                    }
                    out << "  $" << std::hex << (unsigned)entry.command << " in=";
                    if (entry.input_word_count == 0) {
                        out << "-";
                    } else {
                        for (std::size_t word = 0; word < entry.input_word_count; word++) {
                            out << (word == 0 ? "" : ",") << "$" << entry.input_words[word];
                        }
                    }
                    out << " out=";
                    if (entry.output_word_count == 0) {
                        out << "-";
                    } else {
                        for (std::size_t word = 0; word < entry.output_word_count; word++) {
                            out << (word == 0 ? "" : ",") << "$" << entry.output_words[word];
                        }
                    }
                    out << std::dec << "\n";
                }
            }
        }
        out << "[PPU] INIDISP=$" << std::hex << (unsigned)ppu.getINIDISP()
            << " BGMODE=$" << (unsigned)ppu.getBGMode()
            << " TM=$" << (unsigned)ppu.getTMMain()
            << " TS=$" << (unsigned)ppu.getTMSub()
            << " TMW=$" << (unsigned)ppu.getTMW()
            << " TSW=$" << (unsigned)ppu.getTSW()
            << " BG1SC=$" << (unsigned)ppu.getBGSC(0)
            << " BG2SC=$" << (unsigned)ppu.getBGSC(1)
            << " BG3SC=$" << (unsigned)ppu.getBGSC(2)
            << " BG12NBA=$" << (unsigned)ppu.getBGNBA(0)
            << " BG34NBA=$" << (unsigned)ppu.getBGNBA(1)
            << " HOFS1=$" << ppu.getBGHOFS(0)
            << " VOFS1=$" << ppu.getBGVOFS(0)
            << " VMAIN=$" << (unsigned)ppu.getVMAIN()
            << " W12SEL=$" << (unsigned)ppu.getW12SEL()
            << " W34SEL=$" << (unsigned)ppu.getW34SEL()
            << " WOBJSEL=$" << (unsigned)ppu.getWOBJSEL()
            << " WBGLOG=$" << (unsigned)ppu.getWBGLOG()
            << " WOBJLOG=$" << (unsigned)ppu.getWOBJLOG()
            << " WH0=$" << (unsigned)ppu.getWH0()
            << " WH1=$" << (unsigned)ppu.getWH1()
            << " WH2=$" << (unsigned)ppu.getWH2()
            << " WH3=$" << (unsigned)ppu.getWH3()
            << " CGWSEL=$" << (unsigned)ppu.getCGWSEL()
            << " CGADSUB=$" << (unsigned)ppu.getCGADSUB()
            << " FIXCOL=$" << ppu.getFixedColor()
            << " SETINI=$" << (unsigned)ppu.getSETINI()
            << " VBL=" << ppu.getVBlank() << std::dec << "\n";
        out << "[PPU-M7] A=$" << std::hex << ppu.getM7A()
            << " B=$" << ppu.getM7B()
            << " C=$" << ppu.getM7C()
            << " D=$" << ppu.getM7D()
            << " X=$" << ppu.getM7X()
            << " Y=$" << ppu.getM7Y()
            << " HOFS=$" << ppu.getM7HOFS()
            << " VOFS=$" << ppu.getM7VOFS()
            << " SEL=$" << (unsigned)ppu.getM7SEL()
            << std::dec << "\n";
        out << "[PPU-MEM] vram_addr=$" << std::hex << ppu.getVRAMAddress()
            << " cgram_addr=$" << (unsigned)ppu.getCGRAMAddress()
            << " oam_addr=$" << ppu.getOAMAddress()
            << std::dec
            << " vram_nonzero=" << ppu.countNonZeroVRAM()
            << " cgram_nonzero=" << ppu.countNonZeroCGRAM()
            << " oam_nonzero=" << ppu.countNonZeroOAM()
            << " nonblack_pixels=" << ppu.countNonBlackPixels()
            << "\n";
        out << "[HDMA] enable=$" << std::hex << (unsigned)bus.getHDMAEnable();
        const DMAChannel* dma_channels = bus.getDMAChannels();
        for (int i = 0; i < 8; i++) {
            const DMAChannel& ch = dma_channels[i];
            if (ch.control == 0 && ch.dest_reg == 0 && ch.src_address == 0 &&
                ch.size == 0 && ch.indirect_bank == 0 && ch.table_address == 0 &&
                ch.line_counter == 0 && !ch.hdma_do_transfer && !ch.hdma_terminated) {
                continue;
            }
            out << " ch" << i
                << "={ctl:$" << (unsigned)ch.control
                << " dest:$" << (unsigned)ch.dest_reg
                << " src:$" << ch.src_address
                << " size:$" << ch.size
                << " ib:$" << (unsigned)ch.indirect_bank
                << " table:$" << ch.table_address
                << " line:$" << (unsigned)ch.line_counter
                << " xfer:" << std::dec << ch.hdma_do_transfer
                << " term:" << ch.hdma_terminated;
            if ((ch.control & 0x40) != 0 && ch.indirect_bank == 0x7E) {
                out << std::hex << " data:$";
                for (int b = 0; b < 8; b++) {
                    if (b != 0) out << ",";
                    const uint32_t addr = 0x7E0000u | static_cast<uint32_t>((ch.size + b) & 0xFFFF);
                    out << (unsigned)const_cast<Bus&>(bus).read(addr);
                }
            } else if ((ch.control & 0x40) == 0) {
                const uint32_t src_bank = (ch.src_address >> 16) & 0xFFu;
                if (src_bank == 0x7E) {
                    out << std::hex << " data:$";
                    for (int b = 0; b < 8; b++) {
                        if (b != 0) out << ",";
                        const uint32_t addr = 0x7E0000u | static_cast<uint32_t>((ch.table_address + b) & 0xFFFF);
                        out << (unsigned)const_cast<Bus&>(bus).read(addr);
                    }
                }
            }
            out << std::hex << "}";
        }
        out << std::dec << "\n";
        out << "[APU] uploaded_program=" << apu.hasUploadedProgram()
            << " uploaded_bytes=" << apu.getUploadedBytes()
            << " upload_addr=$" << std::hex << apu.getUploadAddress()
            << " exec_addr=$" << apu.getExecuteAddress()
            << " ready=" << std::dec << apu.isDriverReady()
            << " running=" << apu.isUserCodeRunning()
            << " spc_pc=$" << std::hex << apu.getSPCPC()
            << " A=" << (unsigned)apu.getSPCA()
            << " X=" << (unsigned)apu.getSPCX()
            << " Y=" << (unsigned)apu.getSPCY()
            << " SP=" << (unsigned)apu.getSPCSP()
            << " PSW=" << (unsigned)apu.getSPCPSW()
            << " DSP_FLG=$" << (unsigned)apu.getDSPRegister(0x6C)
            << " DSP_DIR=$" << (unsigned)apu.getDSPRegister(0x5D)
            << " DSP_KOFF=$" << (unsigned)apu.getDSPRegister(0x5C)
            << " DSP_KON=$" << (unsigned)apu.getDSPRegister(0x4C)
            << " DSP_NON=$" << (unsigned)apu.getDSPRegister(0x3D)
            << " DSP_PMON=$" << (unsigned)apu.getDSPRegister(0x2D)
            << " DSP_EON=$" << (unsigned)apu.getDSPRegister(0x4D)
            << " DSP_ESA=$" << (unsigned)apu.getDSPRegister(0x6D)
            << " DSP_EDL=$" << (unsigned)apu.getDSPRegister(0x7D)
            << " KON_WRITES=" << std::dec << apu.getKeyOnCount()
            << " KOF_WRITES=" << apu.getKeyOffCount()
            << " LAST_KON=$" << std::hex << (unsigned)apu.getLastKeyOnMask()
            << " LAST_KOF=$" << (unsigned)apu.getLastKeyOffMask()
            << " DSP4C_WRITES=" << std::dec << apu.getDSPWriteCount(0x4C)
            << " DSP5C_WRITES=" << apu.getDSPWriteCount(0x5C)
            << " DSP4C_READS=" << apu.getDSPReadCount(0x4C)
            << " DSP5C_READS=" << apu.getDSPReadCount(0x5C)
            << " DSP7C_READS=" << apu.getDSPReadCount(0x7C)
            << " blank_loop_fallbacks=" << apu.getBlankLoopFallbacks()
            << " audio_frames=" << std::dec << apu.getGeneratedAudioFrames()
            << " audio_nonzero=" << apu.getNonZeroAudioFrames()
            << " audio_silent=" << apu.getSilentAudioFrames()
            << " audio_peak=" << apu.getAudioPeakSample()
            << " audio_queue=" << apu.getQueuedAudioFrames()
            << " audio_queue_peak=" << apu.getPeakQueuedAudioFrames()
            << " underruns=" << apu.getAudioUnderrunFrames()
            << " dropped=" << apu.getAudioDroppedFrames()
            << " cpu_ports="
            << std::hex
            << (unsigned)cpu_ports[0] << "," << (unsigned)cpu_ports[1] << ","
            << (unsigned)cpu_ports[2] << "," << (unsigned)cpu_ports[3]
            << " cpu_port_writes="
            << std::dec
            << cpu_port_writes[0] << "," << cpu_port_writes[1] << ","
            << cpu_port_writes[2] << "," << cpu_port_writes[3]
            << " last_cpu_writes=$"
            << std::hex
            << (unsigned)last_cpu_writes[0] << "," << (unsigned)last_cpu_writes[1] << ","
            << (unsigned)last_cpu_writes[2] << "," << (unsigned)last_cpu_writes[3]
            << " spc_ports="
            << (unsigned)spc_ports[0] << "," << (unsigned)spc_ports[1] << ","
            << (unsigned)spc_ports[2] << "," << (unsigned)spc_ports[3]
            << " spc_port_writes="
            << std::dec
            << spc_port_writes[0] << "," << spc_port_writes[1] << ","
            << spc_port_writes[2] << "," << spc_port_writes[3]
            << " last_spc_writes=$"
            << std::hex
            << (unsigned)last_spc_writes[0] << "," << (unsigned)last_spc_writes[1] << ","
            << (unsigned)last_spc_writes[2] << "," << (unsigned)last_spc_writes[3]
            << " unsupported=" << std::dec << apu.hasUnsupportedOpcode();
        if (apu.hasUnsupportedOpcode()) {
            out << " bad_op=$" << std::hex << (unsigned)apu.getUnsupportedOpcode()
                << " bad_pc=$" << apu.getUnsupportedPC();
        }
        out << std::dec << "\n";

        const uint8_t* aram = apu.getRAMData();
        const auto voices = apu.getVoiceDebug();
        for (int i = 0; i < 8; i++) {
            out << "[APU-V" << i << "] active=" << voices[i].active
                << " rel=" << voices[i].releasing
                << " kon=" << voices[i].key_on_pending
                << " src=$" << std::hex << (unsigned)voices[i].source_number
                << " adsr1=$" << (unsigned)voices[i].adsr1
                << " max_adsr1=$" << (unsigned)voices[i].max_adsr1_written
                << " adsr2=$" << (unsigned)voices[i].adsr2
                << " gain=$" << (unsigned)voices[i].gain
                << " start=$" << voices[i].source_start
                << " loop=$" << voices[i].loop_start
                << " next=$" << voices[i].next_block_addr
                << " interp=$" << voices[i].interp_index
                << " sample_idx=" << std::dec << voices[i].sample_index
                << " decoded=" << voices[i].decoded_sample_count
                << " taps=" << voices[i].tap_prev2 << "," << voices[i].tap_prev1
                << "," << voices[i].tap_curr << "," << voices[i].tap_next
                << " sample=" << voices[i].current_sample
                << std::hex
                << " pitch=$" << voices[i].pitch
                << " env=$" << voices[i].envelope
                << " out=" << std::dec << voices[i].last_output
                << " vl=" << (int)voices[i].volume_left
                << " vr=" << (int)voices[i].volume_right
                << " envx=" << (unsigned)voices[i].envx
                << " outx=" << (unsigned)voices[i].outx
                << " noise=" << voices[i].noise_enabled
                << " pmon=" << voices[i].pitch_mod_enabled
                << "\n";
            if (voices[i].source_start != 0 || voices[i].next_block_addr != 0) {
                const uint16_t start = voices[i].source_start;
                const uint16_t loop = voices[i].loop_start;
                const uint16_t next = voices[i].next_block_addr;
                const uint16_t prev = (uint16_t)(next - 9);
                const uint16_t swapped_start = (uint16_t)((start << 8) | (start >> 8));
                const uint16_t swapped_loop = (uint16_t)((loop << 8) | (loop >> 8));
                const uint16_t directory_base = (uint16_t)(apu.getDSPRegister(0x5D) << 8);
                const uint16_t entry_addr = (uint16_t)(directory_base + voices[i].source_number * 4);
                out << "[APU-BRR" << i << "]"
                    << " dir=$" << std::hex << entry_addr
                    << " dir_bytes=$"
                    << (unsigned)aram[entry_addr] << "," << (unsigned)aram[(uint16_t)(entry_addr + 1)]
                    << "," << (unsigned)aram[(uint16_t)(entry_addr + 2)] << "," << (unsigned)aram[(uint16_t)(entry_addr + 3)]
                    << " start_hdr=$" << std::hex << (unsigned)aram[start]
                    << " start_b1=$" << (unsigned)aram[(uint16_t)(start + 1)]
                    << " start_b2=$" << (unsigned)aram[(uint16_t)(start + 2)]
                    << " start_b3=$" << (unsigned)aram[(uint16_t)(start + 3)]
                    << " loop_hdr=$" << (unsigned)aram[loop]
                    << " loop_b1=$" << (unsigned)aram[(uint16_t)(loop + 1)]
                    << " loop_b2=$" << (unsigned)aram[(uint16_t)(loop + 2)]
                    << " loop_b3=$" << (unsigned)aram[(uint16_t)(loop + 3)]
                    << " swap_start=$" << swapped_start
                    << " swap_start_hdr=$" << (unsigned)aram[swapped_start]
                    << " swap_loop=$" << swapped_loop
                    << " swap_loop_hdr=$" << (unsigned)aram[swapped_loop]
                    << " prev_hdr=$" << (unsigned)aram[prev]
                    << " next_hdr=$" << (unsigned)aram[next]
                    << " next_b1=$" << (unsigned)aram[(uint16_t)(next + 1)]
                    << " next_b2=$" << (unsigned)aram[(uint16_t)(next + 2)]
                    << std::dec << "\n";
            }
        }

        const auto timers = apu.getTimerDebug();
        for (int i = 0; i < 3; i++) {
            out << "[APU-T" << i << "] en=" << timers[i].enabled
                << " target=" << (unsigned)timers[i].target
                << " stage2=" << (unsigned)timers[i].stage2
                << " out=" << (unsigned)timers[i].stage3
                << " div=" << timers[i].divider
                << " period=" << timers[i].period
                << "\n";
        }

        const uint16_t spc_pc = apu.getSPCPC();
        out << "[APU-PC] pc=$" << std::hex << spc_pc << " bytes=";
        for (int i = 0; i < 8; i++) {
            const uint16_t addr = (uint16_t)(spc_pc + i);
            out << (i == 0 ? "" : " ") << (unsigned)aram[addr];
        }
        out << std::dec << "\n";
        const auto& apu_pc_history = apu.getInstructionPCs();
        const auto& apu_op_history = apu.getInstructionOpcodes();
        const auto& apu_a_history = apu.getInstructionAHistory();
        const auto& apu_x_history = apu.getInstructionXHistory();
        const auto& apu_y_history = apu.getInstructionYHistory();
        const std::size_t apu_history_count = apu.hasInstructionHistoryWrapped()
            ? APU::kInstructionHistory
            : apu.getInstructionHistoryPos();
        if (apu_history_count != 0) {
            const std::size_t tail_count = std::min<std::size_t>(16, apu_history_count);
            const std::size_t start =
                (apu.getInstructionHistoryPos() + APU::kInstructionHistory - tail_count) % APU::kInstructionHistory;
            out << "[APU-HIST]";
            for (std::size_t i = 0; i < tail_count; i++) {
                const std::size_t index = (start + i) % APU::kInstructionHistory;
                out << (i == 0 ? " " : " ")
                    << std::hex << apu_pc_history[index]
                    << ":" << (unsigned)apu_op_history[index]
                    << " a=" << (unsigned)apu_a_history[index]
                    << " x=" << (unsigned)apu_x_history[index]
                    << " y=" << (unsigned)apu_y_history[index];
            }
            out << std::dec << "\n";
        }
        if (apu.isWriteWatchEnabled()) {
            const auto& watched_writes = apu.getWatchedWriteHistory();
            const std::size_t watched_write_count = apu.hasWatchedWriteHistoryWrapped()
                ? APU::kWatchedWriteHistory
                : apu.getWatchedWriteHistoryPos();
            if (watched_write_count != 0) {
                const std::size_t tail_count = std::min<std::size_t>(64, watched_write_count);
                const std::size_t start =
                    (apu.getWatchedWriteHistoryPos() + APU::kWatchedWriteHistory - tail_count) %
                    APU::kWatchedWriteHistory;
                out << "[APU-WATCH-WRITES] range=$"
                    << std::hex << apu.getWriteWatchStart()
                    << "+$" << apu.getWriteWatchLength();
                for (std::size_t i = 0; i < tail_count; i++) {
                    const std::size_t index = (start + i) % APU::kWatchedWriteHistory;
                    const auto& write = watched_writes[index];
                    out << (i == 0 ? " " : " ")
                        << "#" << std::dec << write.sequence
                        << " pc=$" << std::hex << write.pc
                        << " op=$" << (unsigned)write.opcode
                        << " a=$" << (unsigned)write.a
                        << " x=$" << (unsigned)write.x
                        << " y=$" << (unsigned)write.y
                        << " psw=$" << (unsigned)write.psw
                        << " [" << write.address << "]="
                        << (unsigned)write.before << "->" << (unsigned)write.value;
                }
                out << std::dec << "\n";
            }
        }
        const auto& directory_writes = apu.getDirectoryWriteHistory();
        const std::size_t directory_write_count = apu.hasDirectoryWriteHistoryWrapped()
            ? APU::kDirectoryWriteHistory
            : apu.getDirectoryWriteHistoryPos();
        if (directory_write_count != 0) {
            const std::size_t tail_count = std::min<std::size_t>(32, directory_write_count);
            const std::size_t start =
                (apu.getDirectoryWriteHistoryPos() + APU::kDirectoryWriteHistory - tail_count) %
                APU::kDirectoryWriteHistory;
            out << "[APU-DIR-WRITES]";
            for (std::size_t i = 0; i < tail_count; i++) {
                const std::size_t index = (start + i) % APU::kDirectoryWriteHistory;
                const auto& write = directory_writes[index];
                out << (i == 0 ? " " : " ")
                    << "#" << std::dec << write.sequence
                    << " pc=$" << std::hex << write.pc
                    << " op=$" << (unsigned)write.opcode
                    << " a=$" << (unsigned)write.a
                    << " x=$" << (unsigned)write.x
                    << " y=$" << (unsigned)write.y
                    << " psw=$" << (unsigned)write.psw
                    << " [" << write.address << "]="
                    << (unsigned)write.before << "->" << (unsigned)write.value;
            }
            out << std::dec << "\n";
        }

        out << "[PAD] joy1_state=$" << std::hex << bus.getJoy1AutoRead()
            << " joy2_state=$" << bus.getJoy2AutoRead()
            << " joy1_shift=$" << bus.getJoy1Shift()
            << " joy2_shift=$" << bus.getJoy2Shift()
            << " strobe=" << std::dec << bus.getJoypadStrobe()
            << " joy4016_reads=" << bus.getJoy4016Reads()
            << " joy4017_reads=" << bus.getJoy4017Reads()
            << " joy4218_reads=" << bus.getJoy4218Reads()
            << " joy4219_reads=" << bus.getJoy4219Reads()
            << " joy421A_reads=" << bus.getJoy421AReads()
            << " joy421B_reads=" << bus.getJoy421BReads()
            << " joy4016_writes=" << bus.getJoy4016Writes()
            << " last_4016=$" << std::hex << (unsigned)bus.getLastJoy4016Write()
            << std::dec << "\n";
        const auto& apu_port_writes = bus.getAPUPortWriteHistory();
        const std::size_t apu_port_write_count = bus.hasAPUPortWriteHistoryWrapped()
            ? Bus::kAPUPortWriteHistory
            : bus.getAPUPortWriteHistoryPos();
        if (apu_port_write_count != 0) {
            const std::size_t tail_count = std::min<std::size_t>(96, apu_port_write_count);
            const std::size_t start =
                (bus.getAPUPortWriteHistoryPos() + Bus::kAPUPortWriteHistory - tail_count) %
                Bus::kAPUPortWriteHistory;
            out << "[CPU-APU-WRITES]";
            for (std::size_t i = 0; i < tail_count; i++) {
                const std::size_t index = (start + i) % Bus::kAPUPortWriteHistory;
                const auto& write = apu_port_writes[index];
                out << (i == 0 ? " " : " ")
                    << "#" << std::dec << write.sequence
                    << " pc=$" << std::hex << write.cpu_pc
                    << " op=$" << (unsigned)write.opcode
                    << " a=$" << write.a
                    << " x=$" << write.x
                    << " y=$" << write.y
                    << " sp=$" << write.sp
                    << " d=$" << write.d
                    << " db=$" << (unsigned)write.db
                    << " p=$" << (unsigned)write.p
                    << " dp=[$" << (unsigned)write.dp0 << ",$" << (unsigned)write.dp1
                    << ",$" << (unsigned)write.dp2 << "]"
                    << " $" << (0x2140u + write.port) << "=" << (unsigned)write.data;
            }
            out << std::dec << "\n";
        }
    }

    ~System() { saveSRAM(); delete cartridge; }

private:
    CPU cpu; PPU ppu; APU apu; Bus bus;
    Cartridge* cartridge = nullptr;
    std::string rom_filepath;
};

static void PrintUsage() {
    std::cout << "Super Furamicom - SNES Emulator\n\n"
              << "Usage: snes.exe [rom.sfc] [options]\n"
              << "  --trace              CPU trace to trace.log\n"
              << "  --visualize          Start with the visualizer open\n"
              << "  --manipulator       Start with the manipulator open\n"
              << "  --headless-frames N  Run N frames and exit\n"
              << "  --pad1-script SPEC   Headless pad script, e.g. 0-15:1000,60-63:8000\n"
              << "  --dump-frame PATH    Save the final frame as a BMP\n"
              << "  --dump-aram PATH     Save 64KB SPC RAM to a file\n"
              << "  --watch-aram S L     Trace ARAM writes in [S, S+L)\n"
              << "  --dump-state         Print CPU/PPU/APU state on exit\n\n"
              << "When no ROM path is provided, the launcher scans ./roms and ./ for .sfc/.smc files.\n"
              << "Controls can be remapped from the launcher, F5 toggles the visualizer, F6 toggles the manipulator, and both side panels support F11/Alt+Enter.\n";
}

static bool WriteBinaryFile(const std::string& path, const uint8_t* data, std::size_t size) {
    if (path.empty() || !data || size == 0) return false;
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(data), (std::streamsize)size);
    return (bool)out;
}

static void AppendUniquePaths(std::vector<std::string>& target, const std::vector<std::string>& source) {
    for (const auto& path : source) {
        if (std::find(target.begin(), target.end(), path) == target.end()) {
            target.push_back(path);
        }
    }
}

static System* g_system_for_crash = nullptr;
static const char* g_rom_path_for_crash = nullptr;
static uint32_t g_start_ticks = 0;
static uint32_t* g_frame_counter_for_crash = nullptr;
static char g_renderer_name_for_crash[64] = "";
static char g_audio_driver_for_crash[64] = "";

struct AtomicTimingStats {
    std::atomic<uint64_t> count{0};
    std::atomic<uint64_t> total_us{0};
    std::atomic<uint64_t> min_us{std::numeric_limits<uint64_t>::max()};
    std::atomic<uint64_t> max_us{0};
    std::atomic<uint64_t> last_us{0};
};

struct RuntimeCrashStats {
    std::atomic<uint32_t> emulated_frames{0};
    std::atomic<uint32_t> presented_frames{0};
    std::atomic<uint32_t> window_w{0};
    std::atomic<uint32_t> window_h{0};
    std::atomic<uint32_t> output_w{0};
    std::atomic<uint32_t> output_h{0};
    std::atomic<uint32_t> renderer_flags{0};
    std::atomic<uint32_t> audio_freq{0};
    std::atomic<uint32_t> audio_samples{0};
    std::atomic<uint32_t> audio_channels{0};
    std::atomic<bool> integer_scale{false};
    std::atomic<bool> main_fullscreen{false};
    std::atomic<bool> visualizer_open{false};
    std::atomic<bool> visualizer_fullscreen{false};
    std::atomic<bool> renderer_vsync{false};
    std::atomic<uint16_t> last_pad1{0};
    std::atomic<uint16_t> last_pad2{0};
    std::atomic<bool> window_focused{false};
    std::atomic<bool> cpu_halted{false};
    std::atomic<uint32_t> cpu_pc{0};
    std::atomic<uint16_t> spc_pc{0};
    std::atomic<uint32_t> framebuffer_signature{0};
    std::atomic<uint32_t> cpu_history_signature{0};
    std::atomic<uint64_t> state_signature{0};
    AtomicTimingStats emulation;
    AtomicTimingStats present;
    AtomicTimingStats render_work;
};

static RuntimeCrashStats g_runtime_crash_stats;

#ifdef _WIN32
struct LiveMainWindowRenderState {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    std::array<uint32_t, 256 * 224>* frame = nullptr;
    std::mutex* frame_mutex = nullptr;
    std::atomic<bool>* session_active = nullptr;
};

static LiveMainWindowRenderState g_live_main_window_render_state{};
static HWND g_main_window_hwnd = nullptr;
static WNDPROC g_main_window_proc = nullptr;
static constexpr UINT_PTR kMainWindowMoveTimerId = 0x53460001u;
static constexpr UINT kMainWindowMoveTimerMs = 15u;

static bool HasLiveMainWindowRenderState() {
    return g_live_main_window_render_state.window &&
        g_live_main_window_render_state.renderer &&
        g_live_main_window_render_state.texture &&
        g_live_main_window_render_state.frame &&
        g_live_main_window_render_state.frame_mutex &&
        g_live_main_window_render_state.session_active &&
        g_live_main_window_render_state.session_active->load(std::memory_order_relaxed);
}

static void RefreshDragRenderOutputStats() {
    if (!HasLiveMainWindowRenderState()) return;

    int window_w = 0;
    int window_h = 0;
    SDL_GetWindowSize(g_live_main_window_render_state.window, &window_w, &window_h);
    g_runtime_crash_stats.window_w.store((uint32_t)std::max(0, window_w), std::memory_order_relaxed);
    g_runtime_crash_stats.window_h.store((uint32_t)std::max(0, window_h), std::memory_order_relaxed);

    int output_w = window_w;
    int output_h = window_h;
    if (SDL_GetRendererOutputSize(
            g_live_main_window_render_state.renderer,
            &output_w,
            &output_h) != 0) {
        output_w = window_w;
        output_h = window_h;
    }
    g_runtime_crash_stats.output_w.store((uint32_t)std::max(0, output_w), std::memory_order_relaxed);
    g_runtime_crash_stats.output_h.store((uint32_t)std::max(0, output_h), std::memory_order_relaxed);
}

static void PresentLiveMainWindowFrame() {
    if (!HasLiveMainWindowRenderState()) return;

    std::unique_lock<std::mutex> frame_lock(
        *g_live_main_window_render_state.frame_mutex,
        std::try_to_lock);
    if (!frame_lock.owns_lock()) return;

    SDL_UpdateTexture(
        g_live_main_window_render_state.texture,
        NULL,
        g_live_main_window_render_state.frame->data(),
        256 * sizeof(uint32_t));
    frame_lock.unlock();

    PresentMainTexture(
        g_live_main_window_render_state.renderer,
        g_live_main_window_render_state.texture);
    RefreshDragRenderOutputStats();
}

static LRESULT CALLBACK MainWindowDragRenderProc(
    HWND hwnd,
    UINT message,
    WPARAM wParam,
    LPARAM lParam) {
    switch (message) {
    case WM_ENTERSIZEMOVE:
        if (HasLiveMainWindowRenderState()) {
            SetTimer(hwnd, kMainWindowMoveTimerId, kMainWindowMoveTimerMs, NULL);
        }
        break;
    case WM_EXITSIZEMOVE:
        KillTimer(hwnd, kMainWindowMoveTimerId);
        break;
    case WM_TIMER:
        if (wParam == kMainWindowMoveTimerId) {
            PresentLiveMainWindowFrame();
            return 0;
        }
        break;
    case WM_PAINT:
        if (HasLiveMainWindowRenderState()) {
            PAINTSTRUCT paint{};
            BeginPaint(hwnd, &paint);
            PresentLiveMainWindowFrame();
            EndPaint(hwnd, &paint);
            return 0;
        }
        break;
    case WM_ERASEBKGND:
        if (HasLiveMainWindowRenderState()) {
            return 1;
        }
        break;
    case WM_NCDESTROY:
        KillTimer(hwnd, kMainWindowMoveTimerId);
        break;
    default:
        break;
    }

    return g_main_window_proc
        ? CallWindowProc(g_main_window_proc, hwnd, message, wParam, lParam)
        : DefWindowProc(hwnd, message, wParam, lParam);
}

static void InstallMainWindowDragRenderer(SDL_Window* window) {
    if (!window || g_main_window_proc) return;

    SDL_SysWMinfo wm_info{};
    SDL_VERSION(&wm_info.version);
    if (SDL_GetWindowWMInfo(window, &wm_info) != SDL_TRUE ||
        wm_info.subsystem != SDL_SYSWM_WINDOWS) {
        return;
    }

    g_main_window_hwnd = wm_info.info.win.window;
    if (!g_main_window_hwnd) return;

    SetLastError(0);
    const LONG_PTR previous = SetWindowLongPtr(
        g_main_window_hwnd,
        GWLP_WNDPROC,
        reinterpret_cast<LONG_PTR>(MainWindowDragRenderProc));
    if (previous == 0 && GetLastError() != 0) {
        g_main_window_hwnd = nullptr;
        return;
    }
    g_main_window_proc = reinterpret_cast<WNDPROC>(previous);
}

static void SetLiveMainWindowRenderState(
    SDL_Window* window,
    SDL_Renderer* renderer,
    SDL_Texture* texture,
    std::array<uint32_t, 256 * 224>* frame,
    std::mutex* frame_mutex,
    std::atomic<bool>* session_active) {
    g_live_main_window_render_state.window = window;
    g_live_main_window_render_state.renderer = renderer;
    g_live_main_window_render_state.texture = texture;
    g_live_main_window_render_state.frame = frame;
    g_live_main_window_render_state.frame_mutex = frame_mutex;
    g_live_main_window_render_state.session_active = session_active;
    RefreshDragRenderOutputStats();
}

static void ClearLiveMainWindowRenderState() {
    if (g_main_window_hwnd) {
        KillTimer(g_main_window_hwnd, kMainWindowMoveTimerId);
    }
    g_live_main_window_render_state = {};
}

static void RestoreMainWindowDragRenderer() {
    ClearLiveMainWindowRenderState();
    if (g_main_window_hwnd && g_main_window_proc) {
        SetWindowLongPtr(
            g_main_window_hwnd,
            GWLP_WNDPROC,
            reinterpret_cast<LONG_PTR>(g_main_window_proc));
    }
    g_main_window_hwnd = nullptr;
    g_main_window_proc = nullptr;
}
#else
static void InstallMainWindowDragRenderer(SDL_Window*) {}
static void SetLiveMainWindowRenderState(
    SDL_Window*,
    SDL_Renderer*,
    SDL_Texture*,
    std::array<uint32_t, 256 * 224>*,
    std::mutex*,
    std::atomic<bool>*) {}
static void ClearLiveMainWindowRenderState() {}
static void RestoreMainWindowDragRenderer() {}
#endif

static void ResetTimingStats(AtomicTimingStats& stats) {
    stats.count.store(0, std::memory_order_relaxed);
    stats.total_us.store(0, std::memory_order_relaxed);
    stats.min_us.store(std::numeric_limits<uint64_t>::max(), std::memory_order_relaxed);
    stats.max_us.store(0, std::memory_order_relaxed);
    stats.last_us.store(0, std::memory_order_relaxed);
}

static void ResetRuntimeCrashStats() {
    g_runtime_crash_stats.emulated_frames.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.presented_frames.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.window_w.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.window_h.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.output_w.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.output_h.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.renderer_flags.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.audio_freq.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.audio_samples.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.audio_channels.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.integer_scale.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.main_fullscreen.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.visualizer_open.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.visualizer_fullscreen.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.renderer_vsync.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.last_pad1.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.last_pad2.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.window_focused.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_halted.store(false, std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_pc.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.spc_pc.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.framebuffer_signature.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_history_signature.store(0, std::memory_order_relaxed);
    g_runtime_crash_stats.state_signature.store(0, std::memory_order_relaxed);
    ResetTimingStats(g_runtime_crash_stats.emulation);
    ResetTimingStats(g_runtime_crash_stats.present);
    ResetTimingStats(g_runtime_crash_stats.render_work);
}

static void UpdateTimingStats(AtomicTimingStats& stats, uint64_t value_us) {
    stats.count.fetch_add(1, std::memory_order_relaxed);
    stats.total_us.fetch_add(value_us, std::memory_order_relaxed);
    stats.last_us.store(value_us, std::memory_order_relaxed);

    uint64_t current_min = stats.min_us.load(std::memory_order_relaxed);
    while (value_us < current_min &&
           !stats.min_us.compare_exchange_weak(current_min, value_us, std::memory_order_relaxed)) {}

    uint64_t current_max = stats.max_us.load(std::memory_order_relaxed);
    while (value_us > current_max &&
           !stats.max_us.compare_exchange_weak(current_max, value_us, std::memory_order_relaxed)) {}
}

struct TimingSnapshot {
    uint64_t count = 0;
    uint64_t total_us = 0;
    uint64_t min_us = 0;
    uint64_t max_us = 0;
    uint64_t last_us = 0;
};

static TimingSnapshot SnapshotTimingStats(const AtomicTimingStats& stats) {
    TimingSnapshot snapshot{};
    snapshot.count = stats.count.load(std::memory_order_relaxed);
    snapshot.total_us = stats.total_us.load(std::memory_order_relaxed);
    snapshot.min_us = stats.min_us.load(std::memory_order_relaxed);
    snapshot.max_us = stats.max_us.load(std::memory_order_relaxed);
    snapshot.last_us = stats.last_us.load(std::memory_order_relaxed);
    if (snapshot.min_us == std::numeric_limits<uint64_t>::max()) {
        snapshot.min_us = 0;
    }
    return snapshot;
}

static double MicrosToMillis(uint64_t value_us) {
    return (double)value_us / 1000.0;
}

static uint32_t FoldHash32(uint32_t hash, uint32_t value) {
    hash ^= value + 0x9E3779B9u + (hash << 6) + (hash >> 2);
    hash *= 16777619u;
    return hash;
}

static uint64_t FoldHash64(uint64_t hash, uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash;
}

static uint32_t ComputeFramebufferSignature(const uint32_t* frame) {
    if (!frame) return 0;
    constexpr int kTotalPixels = 256 * 224;
    constexpr int kSamples = 96;
    uint32_t hash = 2166136261u;
    for (int i = 0; i < kSamples; i++) {
        const int index = (i * (kTotalPixels - 1)) / (kSamples - 1);
        hash = FoldHash32(hash, frame[index]);
    }
    return hash;
}

static uint32_t ComputeCpuHistorySignature(const CPU& cpu) {
    const auto& history = cpu.getPCHistory();
    const std::size_t history_count = cpu.hasHistoryWrapped()
        ? CPU::kDebugHistory
        : cpu.getHistoryPos();
    const std::size_t tail_count = std::min<std::size_t>(16, history_count);

    uint32_t hash = 2166136261u;
    if (tail_count == 0) {
        return FoldHash32(hash, cpu.getProgramCounter());
    }

    const std::size_t start =
        (cpu.getHistoryPos() + CPU::kDebugHistory - tail_count) % CPU::kDebugHistory;
    for (std::size_t i = 0; i < tail_count; i++) {
        const std::size_t index = (start + i) % CPU::kDebugHistory;
        hash = FoldHash32(hash, history[index]);
    }
    hash = FoldHash32(hash, cpu.getA());
    hash = FoldHash32(hash, cpu.getX());
    hash = FoldHash32(hash, cpu.getY());
    return hash;
}

static uint64_t ComputeStateSignature(
    const System& system,
    uint32_t framebuffer_signature,
    uint32_t cpu_history_signature) {
    const CPU& cpu = system.getCPU();
    const APU* apu = system.getAPU();

    uint64_t hash = 1469598103934665603ull;
    hash = FoldHash64(hash, framebuffer_signature);
    hash = FoldHash64(hash, cpu_history_signature);
    hash = FoldHash64(hash, cpu.getProgramCounter());
    hash = FoldHash64(hash, apu->getSPCPC());
    hash = FoldHash64(hash, (uint16_t)apu->getLastMixedLeft());
    hash = FoldHash64(hash, (uint16_t)apu->getLastMixedRight());
    hash = FoldHash64(hash, (uint64_t)apu->getActiveVoiceCount());
    return hash;
}

static std::string SanitizeRomLabel(const char* path) {
    if (!path || !*path) return {};
    try {
        return std::filesystem::path(path).filename().string();
    } catch (...) {
        return std::string(path);
    }
}

static void PublishRuntimeStateStats(const System& system, const uint32_t* frame) {
    const uint32_t framebuffer_signature = ComputeFramebufferSignature(frame);
    const uint32_t cpu_history_signature = ComputeCpuHistorySignature(system.getCPU());
    g_runtime_crash_stats.framebuffer_signature.store(framebuffer_signature, std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_history_signature.store(cpu_history_signature, std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_halted.store(system.isCPUHalted(), std::memory_order_relaxed);
    g_runtime_crash_stats.cpu_pc.store(system.getCPU().getProgramCounter(), std::memory_order_relaxed);
    g_runtime_crash_stats.spc_pc.store(system.getAPU()->getSPCPC(), std::memory_order_relaxed);
    g_runtime_crash_stats.state_signature.store(
        ComputeStateSignature(system, framebuffer_signature, cpu_history_signature),
        std::memory_order_relaxed);
}

static SDL_Renderer* CreateMainRenderer(SDL_Window* window, bool& vsync_enabled) {
    vsync_enabled = false;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (renderer) {
        vsync_enabled = true;
        return renderer;
    }

    renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (renderer) return renderer;

    return SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
}

static const char* signalName(int sig) {
    switch (sig) {
    case SIGSEGV: return "SIGSEGV";
    case SIGABRT: return "SIGABRT";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
    default:      return "UNKNOWN";
    }
}

struct RuntimeReportOptions {
    const char* filename_prefix = "crash";
    const char* header = "RUNTIME REPORT";
    int signal = 0;
    const char* stall_type = nullptr;
    uint64_t emulation_stall_ms = 0;
    uint64_t state_stall_ms = 0;
    uint64_t visual_stall_ms = 0;
    uint64_t cpu_history_stall_ms = 0;
    uint32_t input_changes = 0;
    uint32_t visual_input_changes = 0;
    uint32_t cpu_history_input_changes = 0;
    uint16_t last_pad1 = 0;
    uint16_t last_pad2 = 0;
    bool window_focused = false;
    bool cpu_halted = false;
    const uint32_t* framebuffer_override = nullptr;
};

static void WriteDiagnosticReport(const RuntimeReportOptions& options) {
    if (!g_system_for_crash) return;

    std::filesystem::create_directories("output");
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    char ts[64];
    std::strftime(ts, sizeof(ts), "%Y%m%d_%H%M%S", std::localtime(&time));

    char log_path[128];
    std::snprintf(log_path, sizeof(log_path), "output/%s_%s.log", options.filename_prefix, ts);
    std::ofstream log(log_path);

    log << options.header;
    if (options.signal != 0) {
        log << ": " << options.signal << " (" << signalName(options.signal) << ")";
    }
    log << "\n";

    char time_str[64];
    std::strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", std::localtime(&time));
    log << "[META] time=" << time_str;
    const std::string rom_label = SanitizeRomLabel(g_rom_path_for_crash);
    if (!rom_label.empty()) log << " rom=" << rom_label;
    log << " report=" << options.filename_prefix;
    const uint64_t uptime_ms = SDL_GetTicks64() - (uint64_t)g_start_ticks;
    log << " uptime_sec=" << (uptime_ms / 1000);
    if (g_frame_counter_for_crash) log << " frames=" << *g_frame_counter_for_crash;
    log << " sdl_version=" << SDL_MAJOR_VERSION << "." << SDL_MINOR_VERSION << "." << SDL_PATCHLEVEL;
    log << "\n";

    const TimingSnapshot emu_timing = SnapshotTimingStats(g_runtime_crash_stats.emulation);
    const TimingSnapshot present_timing = SnapshotTimingStats(g_runtime_crash_stats.present);
    const TimingSnapshot render_timing = SnapshotTimingStats(g_runtime_crash_stats.render_work);
    log << "[VIDEO] window=" << g_runtime_crash_stats.window_w.load(std::memory_order_relaxed)
        << "x" << g_runtime_crash_stats.window_h.load(std::memory_order_relaxed)
        << " output=" << g_runtime_crash_stats.output_w.load(std::memory_order_relaxed)
        << "x" << g_runtime_crash_stats.output_h.load(std::memory_order_relaxed)
        << " fullscreen=" << g_runtime_crash_stats.main_fullscreen.load(std::memory_order_relaxed)
        << " integer_scale=" << g_runtime_crash_stats.integer_scale.load(std::memory_order_relaxed)
        << " visualizer_open=" << g_runtime_crash_stats.visualizer_open.load(std::memory_order_relaxed)
        << " visualizer_fullscreen=" << g_runtime_crash_stats.visualizer_fullscreen.load(std::memory_order_relaxed)
        << " renderer_vsync=" << g_runtime_crash_stats.renderer_vsync.load(std::memory_order_relaxed)
        << " renderer=" << g_renderer_name_for_crash
        << " renderer_flags=$" << std::hex << g_runtime_crash_stats.renderer_flags.load(std::memory_order_relaxed)
        << std::dec << "\n";
    log << "[AUDIO-DEV] driver=" << g_audio_driver_for_crash
        << " freq=" << g_runtime_crash_stats.audio_freq.load(std::memory_order_relaxed)
        << " samples=" << g_runtime_crash_stats.audio_samples.load(std::memory_order_relaxed)
        << " channels=" << g_runtime_crash_stats.audio_channels.load(std::memory_order_relaxed)
        << "\n";
    log << "[FRAME] emulated=" << g_runtime_crash_stats.emulated_frames.load(std::memory_order_relaxed)
        << " presented=" << g_runtime_crash_stats.presented_frames.load(std::memory_order_relaxed)
        << " emu_ms_last=" << MicrosToMillis(emu_timing.last_us)
        << " emu_ms_avg=" << MicrosToMillis(emu_timing.count ? (emu_timing.total_us / emu_timing.count) : 0)
        << " emu_ms_min=" << MicrosToMillis(emu_timing.min_us)
        << " emu_ms_max=" << MicrosToMillis(emu_timing.max_us)
        << " present_ms_last=" << MicrosToMillis(present_timing.last_us)
        << " present_ms_avg=" << MicrosToMillis(present_timing.count ? (present_timing.total_us / present_timing.count) : 0)
        << " present_ms_min=" << MicrosToMillis(present_timing.min_us)
        << " present_ms_max=" << MicrosToMillis(present_timing.max_us)
        << " render_ms_last=" << MicrosToMillis(render_timing.last_us)
        << " render_ms_avg=" << MicrosToMillis(render_timing.count ? (render_timing.total_us / render_timing.count) : 0)
        << " render_ms_min=" << MicrosToMillis(render_timing.min_us)
        << " render_ms_max=" << MicrosToMillis(render_timing.max_us)
        << "\n";
    log << "[RUNTIME] state_sig=$" << std::hex
        << g_runtime_crash_stats.state_signature.load(std::memory_order_relaxed)
        << " frame_sig=$" << g_runtime_crash_stats.framebuffer_signature.load(std::memory_order_relaxed)
        << " cpu_hist_sig=$" << g_runtime_crash_stats.cpu_history_signature.load(std::memory_order_relaxed)
        << " cpu_pc=$" << g_runtime_crash_stats.cpu_pc.load(std::memory_order_relaxed)
        << " spc_pc=$" << (unsigned)g_runtime_crash_stats.spc_pc.load(std::memory_order_relaxed)
        << " last_pad1=$" << (unsigned)g_runtime_crash_stats.last_pad1.load(std::memory_order_relaxed)
        << " last_pad2=$" << (unsigned)g_runtime_crash_stats.last_pad2.load(std::memory_order_relaxed)
        << std::dec
        << " focused=" << g_runtime_crash_stats.window_focused.load(std::memory_order_relaxed)
        << " cpu_halted=" << g_runtime_crash_stats.cpu_halted.load(std::memory_order_relaxed)
        << "\n";
    log << "[INPUT] pad1=$" << std::hex << (unsigned)options.last_pad1
        << " pad2=$" << (unsigned)options.last_pad2
        << std::dec << "\n";
    if (options.stall_type) {
        log << "[STALL] type=" << options.stall_type
            << " emulation_stall_ms=" << options.emulation_stall_ms
            << " state_stall_ms=" << options.state_stall_ms
            << " visual_stall_ms=" << options.visual_stall_ms
            << " cpu_history_stall_ms=" << options.cpu_history_stall_ms
            << " input_changes=" << options.input_changes
            << " visual_input_changes=" << options.visual_input_changes
            << " cpu_history_input_changes=" << options.cpu_history_input_changes
            << " last_pad1=$" << std::hex << (unsigned)options.last_pad1
            << " last_pad2=$" << (unsigned)options.last_pad2
            << std::dec
            << " focused=" << options.window_focused
            << " cpu_halted=" << options.cpu_halted
            << "\n";
    }

    const PPU& ppu = g_system_for_crash->getPPU();
    const auto& main_sources = ppu.getFrameMainSourceCounts();
    const auto& sub_sources = ppu.getFrameSubSourceCounts();
    log << "[PPU-FRAME] main_backdrop=" << main_sources[0]
        << " main_bg1=" << main_sources[1]
        << " main_bg2=" << main_sources[2]
        << " main_bg3=" << main_sources[3]
        << " main_bg4=" << main_sources[4]
        << " main_obj=" << (main_sources[5] + main_sources[6])
        << " sub_backdrop=" << sub_sources[0]
        << " sub_bg1=" << sub_sources[1]
        << " sub_bg2=" << sub_sources[2]
        << " sub_bg3=" << sub_sources[3]
        << " sub_bg4=" << sub_sources[4]
        << " sub_obj=" << (sub_sources[5] + sub_sources[6])
        << " color_math_pixels=" << ppu.getFrameColorMathPixels()
        << " black_window_pixels=" << ppu.getFrameBlackWindowPixels()
        << "\n";

    APU* report_apu = g_system_for_crash->getAPU();
    log << "[AUDIO] active_voices=" << report_apu->getActiveVoiceCount()
        << " pending_kon=$" << std::hex << (unsigned)report_apu->getPendingKeyOnMask()
        << " pending_koff=$" << (unsigned)report_apu->getPendingKeyOffMask()
        << std::dec
        << " last_mix=" << report_apu->getLastMixedLeft() << "," << report_apu->getLastMixedRight()
        << " master=" << (int)(int8_t)report_apu->getDSPRegister(0x0C)
        << "," << (int)(int8_t)report_apu->getDSPRegister(0x1C)
        << " echo_vol=" << (int)(int8_t)report_apu->getDSPRegister(0x2C)
        << "," << (int)(int8_t)report_apu->getDSPRegister(0x3C)
        << " echo_feedback=" << (int)(int8_t)report_apu->getDSPRegister(0x0D)
        << " mute=" << (((unsigned)report_apu->getDSPRegister(0x6C) & 0x40u) != 0)
        << " echo_disable=" << (((unsigned)report_apu->getDSPRegister(0x6C) & 0x20u) != 0)
        << " silent_frames=" << report_apu->getSilentAudioFrames()
        << " underruns=" << report_apu->getAudioUnderrunFrames()
        << " dropped=" << report_apu->getAudioDroppedFrames()
        << " queue_peak=" << report_apu->getPeakQueuedAudioFrames()
        << "\n";

    g_system_for_crash->dumpDebugState(log);

    const uint8_t* aram = report_apu->getRAMData();
    uint16_t spc_pc = report_apu->getSPCPC();
    log << "[ARAM-DUMP] pc_region=";
    for (int i = -16; i < 32; i++) {
        uint16_t addr = (uint16_t)(spc_pc + i);
        if (i > -16) log << " ";
        if (i == 0) log << "[";
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02x", aram[addr]);
        log << hex;
        if (i == 0) log << "]";
    }
    log << "\n";

    log << "[ARAM-STACK] sp_region=";
    uint8_t spc_sp = report_apu->getSPCSP();
    for (int i = 0; i < 16; i++) {
        uint16_t addr = (uint16_t)(0x0100 + ((spc_sp + 1 + i) & 0xFF));
        if (i > 0) log << " ";
        char hex[4];
        std::snprintf(hex, sizeof(hex), "%02x", aram[addr]);
        log << hex;
    }
    log << "\n";
    log.flush();

    char bmp_path[128];
    std::snprintf(bmp_path, sizeof(bmp_path), "output/%s_%s.bmp", options.filename_prefix, ts);
    WriteFramebufferBMP(
        bmp_path,
        options.framebuffer_override ? options.framebuffer_override : g_system_for_crash->getFramebuffer());

    PPU& report_ppu = g_system_for_crash->getPPU();
    std::array<uint32_t, 256 * 224> saved_debug_frame{};
    std::memcpy(saved_debug_frame.data(), report_ppu.getFramebuffer(),
        saved_debug_frame.size() * sizeof(uint32_t));
    char main_bmp_path[128];
    std::snprintf(main_bmp_path, sizeof(main_bmp_path), "output/%s_%s_main.bmp", options.filename_prefix, ts);
    report_ppu.renderDebugFrame(report_ppu.getTMMain(), 0, true);
    WriteFramebufferBMP(main_bmp_path, report_ppu.getFramebuffer());

    if (report_ppu.getTMSub() != 0) {
        char sub_bmp_path[128];
        std::snprintf(sub_bmp_path, sizeof(sub_bmp_path), "output/%s_%s_sub.bmp", options.filename_prefix, ts);
        report_ppu.renderDebugFrame(report_ppu.getTMSub(), 0, true);
        WriteFramebufferBMP(sub_bmp_path, report_ppu.getFramebuffer());
    }
    std::memcpy(report_ppu.getFramebufferMutable(), saved_debug_frame.data(),
        saved_debug_frame.size() * sizeof(uint32_t));

    char pcm_path[128];
    std::snprintf(pcm_path, sizeof(pcm_path), "output/%s_%s.pcm", options.filename_prefix, ts);
    std::ofstream pcm(pcm_path, std::ios::binary);
    const auto& audio_hist = report_apu->getRecentAudioMono();
    pcm.write(reinterpret_cast<const char*>(audio_hist.data()), audio_hist.size() * sizeof(int16_t));
    pcm.close();
}

static void CrashHandler(int sig) {
    RuntimeReportOptions options;
    options.filename_prefix = "crash";
    options.header = "CRASH SIGNAL";
    options.signal = sig;
    WriteDiagnosticReport(options);
    std::exit(1);
}

int main(int argc, char* argv[]) {
    bool do_trace = false, do_visualize = false, do_manipulator = false;
    bool dump_state = false, do_integer_scaling = false;
    int headless_frames = 0;
    bool watch_aram_writes = false;
    uint16_t watch_aram_start = 0;
    uint16_t watch_aram_length = 0;
    std::string rom_path;
    std::string dump_frame_path;
    std::string dump_aram_path;
    std::string pad1_script_spec;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) do_trace = true;
        else if (strcmp(argv[i], "--visualize") == 0) do_visualize = true;
        else if (strcmp(argv[i], "--manipulator") == 0) do_manipulator = true;
        else if (strcmp(argv[i], "--dump-state") == 0) dump_state = true;
        else if (strcmp(argv[i], "--dump-frame") == 0 && (i+1) < argc)
            dump_frame_path = argv[++i];
        else if (strcmp(argv[i], "--dump-aram") == 0 && (i+1) < argc)
            dump_aram_path = argv[++i];
        else if (strcmp(argv[i], "--watch-aram") == 0 && (i+2) < argc) {
            watch_aram_writes = true;
            watch_aram_start = (uint16_t)std::strtoul(argv[++i], nullptr, 0);
            watch_aram_length = (uint16_t)std::strtoul(argv[++i], nullptr, 0);
        }
        else if (strcmp(argv[i], "--headless-frames") == 0 && (i+1) < argc)
            headless_frames = std::max(0, std::atoi(argv[++i]));
        else if (strcmp(argv[i], "--pad1-script") == 0 && (i+1) < argc)
            pad1_script_spec = argv[++i];
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            PrintUsage();
            return 0;
        } else if (argv[i][0] != '-' && rom_path.empty()) {
            rom_path = argv[i];
        } else {
            std::cerr << "Unknown argument: " << argv[i] << "\n";
            PrintUsage();
            return 1;
        }
    }

    if (rom_path.empty() && headless_frames > 0) {
        std::cerr << "Headless mode requires a ROM path.\n";
        PrintUsage();
        return 1;
    }

    std::vector<PadScriptRange> pad1_script;
    if (!ParsePadScript(pad1_script_spec, pad1_script)) {
        std::cerr << "[INPUT] Invalid --pad1-script format. Use frame[-frame]:mask, e.g. 0-15:1000,60-63:8000\n";
        return 1;
    }

    System snes;
    g_system_for_crash = &snes;
    g_start_ticks = SDL_GetTicks();
    std::signal(SIGSEGV, CrashHandler);
    std::signal(SIGABRT, CrashHandler);
    std::signal(SIGILL, CrashHandler);
    std::signal(SIGFPE, CrashHandler);

    if (do_trace) { snes.enableTrace("trace.log"); std::cout << "[TRACE] Writing to trace.log\n"; }
    InputConfig input_config = DefaultInputConfig();
    const std::string controls_path = "controls.cfg";
    LoadInputConfig(controls_path, input_config);

    if (headless_frames > 0) {
        if (!rom_path.empty() && !snes.loadROM(rom_path)) {
            return 1;
        }
        if (watch_aram_writes) {
            snes.getAPU()->configureWriteWatch(watch_aram_start, watch_aram_length);
        }
        for (int i = 0; i < headless_frames && !snes.isCPUHalted(); i++) {
            snes.setJoypad(PadScriptMaskForFrame(pad1_script, i));
            snes.stepFrame();
        }
        if (!dump_frame_path.empty()) {
            if (!WriteFramebufferBMP(dump_frame_path, snes.getFramebuffer())) {
                std::cerr << "[FRAME] Failed to write " << dump_frame_path << "\n";
            }
        }
        if (!dump_aram_path.empty()) {
            if (!WriteBinaryFile(dump_aram_path, snes.getAPU()->getRAMData(), 64 * 1024)) {
                std::cerr << "[APU] Failed to write " << dump_aram_path << "\n";
            }
        }
        if (dump_state) snes.dumpDebugState(std::cout);
        return snes.isCPUHalted() ? 2 : 0;
    }

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
        return 1;
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    ResetRuntimeCrashStats();

    SDL_Window* window = SDL_CreateWindow("Super Furamicom",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256*3, 224*3, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }
    InstallMainWindowDragRenderer(window);

    bool renderer_vsync = false;
    SDL_Renderer* renderer = CreateMainRenderer(window, renderer_vsync);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        RestoreMainWindowDragRenderer();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RendererInfo renderer_info{};
    if (SDL_GetRendererInfo(renderer, &renderer_info) == 0) {
        std::snprintf(g_renderer_name_for_crash, sizeof(g_renderer_name_for_crash), "%s",
            renderer_info.name ? renderer_info.name : "unknown");
        g_runtime_crash_stats.renderer_flags.store(renderer_info.flags, std::memory_order_relaxed);
    } else {
        std::snprintf(g_renderer_name_for_crash, sizeof(g_renderer_name_for_crash), "unknown");
    }
    g_runtime_crash_stats.renderer_vsync.store(renderer_vsync, std::memory_order_relaxed);

    SDL_RenderSetLogicalSize(renderer, 256, 224);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 224);
    if (!texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        RestoreMainWindowDragRenderer();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    uint32_t main_wid = SDL_GetWindowID(window);

    auto refreshMainOutputStats = [&]() {
        int window_w = 0;
        int window_h = 0;
        SDL_GetWindowSize(window, &window_w, &window_h);
        g_runtime_crash_stats.window_w.store((uint32_t)std::max(0, window_w), std::memory_order_relaxed);
        g_runtime_crash_stats.window_h.store((uint32_t)std::max(0, window_h), std::memory_order_relaxed);

        int output_w = window_w;
        int output_h = window_h;
        if (SDL_GetRendererOutputSize(renderer, &output_w, &output_h) != 0) {
            output_w = window_w;
            output_h = window_h;
        }
        g_runtime_crash_stats.output_w.store((uint32_t)std::max(0, output_w), std::memory_order_relaxed);
        g_runtime_crash_stats.output_h.store((uint32_t)std::max(0, output_h), std::memory_order_relaxed);
    };

    refreshMainOutputStats();

    const uint64_t perf_freq = SDL_GetPerformanceFrequency();
    const uint64_t target_ticks = (uint64_t)((double)perf_freq / EmuTiming::kNtscFrameRate);
    bool app_running = true;
    std::string active_rom_path = rom_path;

    while (app_running) {
        if (active_rom_path.empty()) {
            g_rom_path_for_crash = nullptr;
            std::vector<std::string> rom_paths;
            AppendUniquePaths(rom_paths, DiscoverROMs("roms"));
            AppendUniquePaths(rom_paths, DiscoverROMs("."));

            LauncherResult launcher = RunLauncher(
                window, renderer, texture, rom_paths, controls_path, input_config,
                do_visualize, do_manipulator, do_integer_scaling);
            if (!launcher.launch_requested) {
                break;
            }

            input_config = launcher.input_config;
            do_visualize = launcher.visualize;
            do_manipulator = launcher.manipulator;
            do_integer_scaling = launcher.integer_scaling;
            active_rom_path = launcher.rom_path;
        }

        if (!snes.loadROM(active_rom_path)) {
            app_running = false;
            break;
        }
        if (watch_aram_writes) {
            snes.getAPU()->configureWriteWatch(watch_aram_start, watch_aram_length);
        }

        ResetRuntimeCrashStats();
        g_runtime_crash_stats.renderer_flags.store(renderer_info.flags, std::memory_order_relaxed);
        g_runtime_crash_stats.renderer_vsync.store(renderer_vsync, std::memory_order_relaxed);
        g_runtime_crash_stats.integer_scale.store(do_integer_scaling, std::memory_order_relaxed);
        g_runtime_crash_stats.main_fullscreen.store(
            (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0, std::memory_order_relaxed);
        refreshMainOutputStats();

        SDL_SetWindowTitle(window, "Super Furamicom");
        g_rom_path_for_crash = active_rom_path.c_str();
        SDL_RenderSetIntegerScale(renderer, do_integer_scaling ? SDL_TRUE : SDL_FALSE);

        SDL_AudioSpec desired = {};
        SDL_AudioSpec obtained = {};
        desired.freq = APU::kOutputHz;
        desired.format = AUDIO_S16SYS;
        desired.channels = 2;
        desired.samples = 512;
        desired.callback = SDLAudioCallback;
        desired.userdata = snes.getAPU();

        SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
        const char* audio_driver = SDL_GetCurrentAudioDriver();
        std::snprintf(g_audio_driver_for_crash, sizeof(g_audio_driver_for_crash), "%s",
            audio_driver ? audio_driver : "none");
        g_runtime_crash_stats.audio_freq.store(audio_device ? (uint32_t)obtained.freq : 0, std::memory_order_relaxed);
        g_runtime_crash_stats.audio_samples.store(audio_device ? (uint32_t)obtained.samples : 0, std::memory_order_relaxed);
        g_runtime_crash_stats.audio_channels.store(audio_device ? (uint32_t)obtained.channels : 0, std::memory_order_relaxed);
        const std::size_t callback_frames = audio_device
            ? std::max<std::size_t>(128, obtained.samples != 0 ? obtained.samples : desired.samples)
            : 256;
        const std::size_t audio_frames_per_emu_frame =
            (std::size_t)std::ceil((double)APU::kOutputHz / EmuTiming::kNtscFrameRate);
        const std::size_t target_queued_audio_frames = audio_device
            ? callback_frames + audio_frames_per_emu_frame * 2
            : 0;

        Visualizer* viz = nullptr;
        Manipulator* manip = nullptr;
        auto refreshVisualizerStats = [&]() {
            g_runtime_crash_stats.visualizer_open.store(viz != nullptr, std::memory_order_relaxed);
            g_runtime_crash_stats.visualizer_fullscreen.store(viz && viz->isFullscreen(), std::memory_order_relaxed);
        };
        auto openVisualizer = [&]() -> bool {
            if (viz) return true;
            viz = new Visualizer();
            if (!viz->init()) {
                delete viz;
                viz = nullptr;
                refreshVisualizerStats();
                return false;
            }
            viz->placeBeside(window);
            viz->show();
            SDL_RaiseWindow(window);
            if (manip && manip->isOpen()) {
                manip->placeBeside(viz->getWindowHandle());
            }
            refreshVisualizerStats();
            return true;
        };
        auto closeVisualizer = [&]() {
            if (!viz) {
                refreshVisualizerStats();
                return;
            }
            viz->destroy();
            delete viz;
            viz = nullptr;
            if (manip && manip->isOpen()) {
                manip->placeBeside(window);
            }
            refreshVisualizerStats();
        };
        auto openManipulator = [&]() -> bool {
            if (manip) return true;
            manip = new Manipulator();
            if (!manip->init()) {
                delete manip;
                manip = nullptr;
                return false;
            }
            SDL_Window* anchor = viz ? viz->getWindowHandle() : window;
            manip->placeBeside(anchor);
            manip->show();
            SDL_RaiseWindow(window);
            return true;
        };
        auto closeManipulator = [&]() {
            if (!manip) return;
            manip->destroy();
            delete manip;
            manip = nullptr;
        };
        if (do_visualize && openVisualizer()) {
            std::cout << "[VIZ] Visualizer active (1=CPU 2=PPU 3=APU 4=MEM)\n";
        }
        if (do_manipulator && openManipulator()) {
            std::cout << "[MANIP] Manipulator active\n";
        }
        refreshVisualizerStats();

        bool running = true;
        bool return_to_launcher = false;
        bool main_fullscreen = (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN_DESKTOP) != 0;
        SDL_Rect main_windowed_bounds{};
        SDL_GetWindowPosition(window, &main_windowed_bounds.x, &main_windowed_bounds.y);
        SDL_GetWindowSize(window, &main_windowed_bounds.w, &main_windowed_bounds.h);

        auto toggleMainFullscreen = [&]() {
            if (!main_fullscreen) {
                SDL_GetWindowPosition(window, &main_windowed_bounds.x, &main_windowed_bounds.y);
                SDL_GetWindowSize(window, &main_windowed_bounds.w, &main_windowed_bounds.h);
                if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
                    main_fullscreen = true;
                }
            } else {
                if (SDL_SetWindowFullscreen(window, 0) == 0) {
                    main_fullscreen = false;
                    SDL_SetWindowPosition(window, main_windowed_bounds.x, main_windowed_bounds.y);
                    SDL_SetWindowSize(window, main_windowed_bounds.w, main_windowed_bounds.h);
                }
            }
            g_runtime_crash_stats.main_fullscreen.store(main_fullscreen, std::memory_order_relaxed);
            refreshMainOutputStats();
        };

        SDL_Event event;
        uint32_t frame_count = 0;
        uint32_t presented_total_frames = 0;
        uint32_t last_fps_time = SDL_GetTicks();
        uint32_t next_viz_update_ms = 0;
        uint32_t viz_resume_ms = 0;
        uint32_t next_manip_update_ms = 0;
        uint32_t manip_resume_ms = 0;
        uint32_t esc_armed_until_ms = 0;
        uint64_t last_present_counter = 0;
        bool reported_halt = false;
        bool manual_report_requested = false;
        bool manual_report_hotkey_latched = false;
        g_frame_counter_for_crash = &presented_total_frames;

        std::array<uint32_t, 256 * 224> present_frame{};
        std::memcpy(present_frame.data(), snes.getFramebuffer(), present_frame.size() * sizeof(uint32_t));

        std::mutex frame_mutex;
        std::mutex system_mutex;
        std::atomic<bool> live_drag_render_active{true};
        std::atomic<uint16_t> pending_pad1{0};
        std::atomic<uint16_t> pending_pad2{0};
        std::atomic<bool> emulation_running{true};
        std::atomic<bool> cpu_halted{snes.isCPUHalted()};
        struct StallWatchdog {
            uint32_t last_emulated_frames = 0;
            uint64_t last_emulation_progress_ms = 0;
            uint64_t last_state_change_ms = 0;
            uint64_t last_visual_change_ms = 0;
            uint64_t last_cpu_history_change_ms = 0;
            uint64_t last_input_change_ms = 0;
            uint64_t last_state_signature = 0;
            uint32_t last_visual_signature = 0;
            uint32_t last_cpu_history_signature = 0;
            uint16_t last_input_mask = 0;
            uint32_t input_changes_since_state = 0;
            uint32_t input_changes_since_visual = 0;
            uint32_t input_changes_since_cpu_history = 0;
            bool episode_reported = false;
        } stall_watchdog;

        {
            std::lock_guard<std::mutex> system_lock(system_mutex);
            for (int warmup = 0;
                 warmup < 6 &&
                 !snes.isCPUHalted() &&
                 (!audio_device || snes.getAPU()->getQueuedAudioFrames() < target_queued_audio_frames);
                 warmup++) {
                snes.setJoypad(0, 0);
                snes.stepFrame();
            }
            cpu_halted.store(snes.isCPUHalted(), std::memory_order_relaxed);
            std::memcpy(present_frame.data(), snes.getFramebuffer(), present_frame.size() * sizeof(uint32_t));
            PublishRuntimeStateStats(snes, present_frame.data());
        }
        const uint64_t session_start_ms = SDL_GetTicks64();
        stall_watchdog.last_emulated_frames =
            g_runtime_crash_stats.emulated_frames.load(std::memory_order_relaxed);
        stall_watchdog.last_emulation_progress_ms = session_start_ms;
        stall_watchdog.last_state_signature =
            g_runtime_crash_stats.state_signature.load(std::memory_order_relaxed);
        stall_watchdog.last_state_change_ms = session_start_ms;
        stall_watchdog.last_visual_signature =
            g_runtime_crash_stats.framebuffer_signature.load(std::memory_order_relaxed);
        stall_watchdog.last_visual_change_ms = session_start_ms;
        stall_watchdog.last_cpu_history_signature =
            g_runtime_crash_stats.cpu_history_signature.load(std::memory_order_relaxed);
        stall_watchdog.last_cpu_history_change_ms = session_start_ms;
        stall_watchdog.last_input_change_ms = session_start_ms;
        SetLiveMainWindowRenderState(
            window,
            renderer,
            texture,
            &present_frame,
            &frame_mutex,
            &live_drag_render_active);

        std::thread emulation_thread([&]() {
            using clock = std::chrono::steady_clock;
            const auto frame_interval = std::chrono::nanoseconds(
                (long long)std::llround(1000000000.0 / EmuTiming::kNtscFrameRate));
            auto next_frame = clock::now();

            while (emulation_running.load(std::memory_order_acquire)) {
                const auto now = clock::now();
                if (now > next_frame + std::chrono::milliseconds(250)) {
                    next_frame = now;
                }
                const bool behind_video = now >= next_frame;
                const std::size_t queued_audio_frames = audio_device
                    ? snes.getAPU()->getQueuedAudioFrames()
                    : 0;
                const bool need_audio = audio_device &&
                    queued_audio_frames < target_queued_audio_frames;
                if (!behind_video && !need_audio) {
                    const auto short_sleep_until = std::min(next_frame, now + std::chrono::milliseconds(2));
                    std::this_thread::sleep_until(short_sleep_until);
                    continue;
                }
                const auto frame_begin = clock::now();

                if (!cpu_halted.load(std::memory_order_relaxed)) {
                    std::lock_guard<std::mutex> system_lock(system_mutex);
                    snes.setJoypad(pending_pad1.load(std::memory_order_relaxed),
                                   pending_pad2.load(std::memory_order_relaxed));
                    snes.stepFrame();
                    cpu_halted.store(snes.isCPUHalted(), std::memory_order_relaxed);

                    const uint32_t* frame = snes.getFramebuffer();
                    std::lock_guard<std::mutex> frame_lock(frame_mutex);
                    std::memcpy(present_frame.data(), frame, present_frame.size() * sizeof(uint32_t));
                    PublishRuntimeStateStats(snes, frame);
                    g_runtime_crash_stats.emulated_frames.fetch_add(1, std::memory_order_relaxed);
                }

                const auto frame_end = clock::now();
                UpdateTimingStats(
                    g_runtime_crash_stats.emulation,
                    (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(frame_end - frame_begin).count());
                if (behind_video || !audio_device) {
                    next_frame += frame_interval;
                }
            }
        });
        if (audio_device) {
            SDL_PauseAudioDevice(audio_device, 0);
        }

        while (running) {
            const uint64_t frame_start = SDL_GetPerformanceCounter();

            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_QUIT) {
                    running = false;
                    break;
                }

                uint32_t event_wid = 0;
                if (event.type == SDL_WINDOWEVENT) event_wid = event.window.windowID;
                else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP) event_wid = event.key.windowID;
                else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP) event_wid = event.button.windowID;
                else if (event.type == SDL_MOUSEMOTION) event_wid = event.motion.windowID;
                else if (event.type == SDL_MOUSEWHEEL) event_wid = event.wheel.windowID;

                if (viz && viz->isOpen() && event_wid == viz->getWindowID()) {
                    if (event.type == SDL_WINDOWEVENT) {
                        switch (event.window.event) {
                        case SDL_WINDOWEVENT_MOVED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                        case SDL_WINDOWEVENT_EXPOSED:
                            viz_resume_ms = SDL_GetTicks() + 200;
                            break;
                        default:
                            break;
                        }
                    }
                    if (!viz->processEvent(event)) {
                        closeVisualizer();
                        do_visualize = false;
                    } else {
                        refreshVisualizerStats();
                    }
                    continue;
                }
                if (manip && manip->isOpen() && event_wid == manip->getWindowID()) {
                    if (event.type == SDL_WINDOWEVENT) {
                        switch (event.window.event) {
                        case SDL_WINDOWEVENT_MOVED:
                        case SDL_WINDOWEVENT_RESIZED:
                        case SDL_WINDOWEVENT_SIZE_CHANGED:
                        case SDL_WINDOWEVENT_EXPOSED:
                            manip_resume_ms = SDL_GetTicks() + 200;
                            break;
                        default:
                            break;
                        }
                    }
                    if (!manip->processEvent(event)) {
                        closeManipulator();
                        do_manipulator = false;
                    }
                    continue;
                }

                if (event.type == SDL_WINDOWEVENT && event_wid == main_wid) {
                    if (event.window.event == SDL_WINDOWEVENT_CLOSE) {
                        running = false;
                    }
                    switch (event.window.event) {
                    case SDL_WINDOWEVENT_FOCUS_LOST:
                        pending_pad1.store(0, std::memory_order_relaxed);
                        pending_pad2.store(0, std::memory_order_relaxed);
                        break;
                    case SDL_WINDOWEVENT_MOVED:
                    case SDL_WINDOWEVENT_RESIZED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_EXPOSED:
                        viz_resume_ms = SDL_GetTicks() + 200;
                        manip_resume_ms = SDL_GetTicks() + 200;
                        refreshMainOutputStats();
                        break;
                    default:
                        break;
                    }
                }

                if (event.type == SDL_KEYDOWN && event_wid == main_wid) {
                    const SDL_Keymod keymods =
                        (SDL_Keymod)(event.key.keysym.mod | SDL_GetModState());
                    if (event.key.keysym.sym == SDLK_ESCAPE) {
                        const uint32_t now_ms = SDL_GetTicks();
                        if (now_ms <= esc_armed_until_ms) {
                            return_to_launcher = true;
                            running = false;
                            break;
                        }
                        esc_armed_until_ms = now_ms + 900;
                        std::cout << "[SYSTEM] Press ESC again to return to launcher\n";
                    }
                    if (event.key.keysym.sym == SDLK_p &&
                        event.key.repeat == 0 &&
                        (keymods & KMOD_CTRL)) {
                        manual_report_requested = true;
                    }
                    if (event.key.keysym.sym == SDLK_F5) {
                        if (!viz) {
                            if (openVisualizer()) {
                                next_viz_update_ms = 0;
                                viz_resume_ms = SDL_GetTicks() + 200;
                                std::cout << "[VIZ] Opened\n";
                            }
                        } else {
                            closeVisualizer();
                            std::cout << "[VIZ] Closed\n";
                        }
                        do_visualize = (viz != nullptr);
                        refreshVisualizerStats();
                    }
                    if (event.key.keysym.sym == SDLK_F6) {
                        if (!manip) {
                            if (openManipulator()) {
                                next_manip_update_ms = 0;
                                manip_resume_ms = SDL_GetTicks() + 200;
                                std::cout << "[MANIP] Opened\n";
                            }
                        } else {
                            closeManipulator();
                            std::cout << "[MANIP] Closed\n";
                        }
                        do_manipulator = (manip != nullptr);
                    }
                    if (event.key.keysym.sym == SDLK_F11) {
                        toggleMainFullscreen();
                    }
                    if (event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT)) {
                        toggleMainFullscreen();
                    }
                }
            }

            const uint8_t* keys = SDL_GetKeyboardState(NULL);
            const uint16_t current_pad1 = MapInputConfigToPad(keys, input_config);
            const uint16_t current_pad2 = 0;
            const bool manual_hotkey_down =
                (keys[SDL_SCANCODE_LCTRL] || keys[SDL_SCANCODE_RCTRL]) &&
                keys[SDL_SCANCODE_P];
            if (manual_hotkey_down) {
                if (!manual_report_hotkey_latched) {
                    manual_report_requested = true;
                }
                manual_report_hotkey_latched = true;
            } else {
                manual_report_hotkey_latched = false;
            }
            pending_pad1.store(current_pad1, std::memory_order_relaxed);
            pending_pad2.store(current_pad2, std::memory_order_relaxed);
            g_runtime_crash_stats.last_pad1.store(current_pad1, std::memory_order_relaxed);

            {
                std::lock_guard<std::mutex> frame_lock(frame_mutex);
                SDL_UpdateTexture(texture, NULL, present_frame.data(), 256 * sizeof(uint32_t));
            }
            PresentMainTexture(renderer, texture);

            const uint64_t after_present = SDL_GetPerformanceCounter();
            UpdateTimingStats(
                g_runtime_crash_stats.render_work,
                ((after_present - frame_start) * 1000000ull) / perf_freq);
            if (last_present_counter != 0) {
                UpdateTimingStats(
                    g_runtime_crash_stats.present,
                    ((after_present - last_present_counter) * 1000000ull) / perf_freq);
            }
            last_present_counter = after_present;
            presented_total_frames++;
            g_runtime_crash_stats.presented_frames.store(presented_total_frames, std::memory_order_relaxed);

            const uint32_t now = SDL_GetTicks();
            const uint64_t now64 = SDL_GetTicks64();
            const bool window_focused = SDL_GetKeyboardFocus() == window;
            g_runtime_crash_stats.last_pad2.store(current_pad2, std::memory_order_relaxed);
            g_runtime_crash_stats.window_focused.store(window_focused, std::memory_order_relaxed);
            g_runtime_crash_stats.cpu_halted.store(cpu_halted.load(std::memory_order_relaxed), std::memory_order_relaxed);
            if (current_pad1 != stall_watchdog.last_input_mask) {
                if (window_focused && (current_pad1 != 0 || stall_watchdog.last_input_mask != 0)) {
                    stall_watchdog.input_changes_since_state++;
                    stall_watchdog.input_changes_since_visual++;
                    stall_watchdog.input_changes_since_cpu_history++;
                    stall_watchdog.last_input_change_ms = now64;
                }
                stall_watchdog.last_input_mask = current_pad1;
            }

            const uint32_t emulated_frames =
                g_runtime_crash_stats.emulated_frames.load(std::memory_order_relaxed);
            if (emulated_frames != stall_watchdog.last_emulated_frames) {
                stall_watchdog.last_emulated_frames = emulated_frames;
                stall_watchdog.last_emulation_progress_ms = now64;
            }

            const uint64_t current_state_signature =
                g_runtime_crash_stats.state_signature.load(std::memory_order_relaxed);
            if (current_state_signature != stall_watchdog.last_state_signature) {
                stall_watchdog.last_state_signature = current_state_signature;
                stall_watchdog.last_state_change_ms = now64;
                stall_watchdog.input_changes_since_state = 0;
                stall_watchdog.episode_reported = false;
            }
            const uint32_t current_visual_signature =
                g_runtime_crash_stats.framebuffer_signature.load(std::memory_order_relaxed);
            if (current_visual_signature != stall_watchdog.last_visual_signature) {
                stall_watchdog.last_visual_signature = current_visual_signature;
                stall_watchdog.last_visual_change_ms = now64;
                stall_watchdog.input_changes_since_visual = 0;
                stall_watchdog.episode_reported = false;
            }
            const uint32_t current_cpu_history_signature =
                g_runtime_crash_stats.cpu_history_signature.load(std::memory_order_relaxed);
            if (current_cpu_history_signature != stall_watchdog.last_cpu_history_signature) {
                stall_watchdog.last_cpu_history_signature = current_cpu_history_signature;
                stall_watchdog.last_cpu_history_change_ms = now64;
                stall_watchdog.input_changes_since_cpu_history = 0;
                stall_watchdog.episode_reported = false;
            }

            const uint64_t emulation_stall_ms = now64 - stall_watchdog.last_emulation_progress_ms;
            const uint64_t state_stall_ms = now64 - stall_watchdog.last_state_change_ms;
            const uint64_t visual_stall_ms = now64 - stall_watchdog.last_visual_change_ms;
            const uint64_t cpu_history_stall_ms = now64 - stall_watchdog.last_cpu_history_change_ms;
            if (!stall_watchdog.episode_reported &&
                window_focused &&
                now64 >= session_start_ms + 5000 &&
                !cpu_halted.load(std::memory_order_relaxed)) {
                const bool hard_stall = emulation_stall_ms >= 4000;
                const bool input_locked_stall =
                    state_stall_ms >= 10000 &&
                    stall_watchdog.input_changes_since_state >= 6 &&
                    now64 - stall_watchdog.last_input_change_ms <= 2500;
                const bool visual_locked_stall =
                    visual_stall_ms >= 15000 &&
                    stall_watchdog.input_changes_since_visual >= 10 &&
                    now64 - stall_watchdog.last_input_change_ms <= 3000;
                const bool cpu_loop_spin_stall =
                    cpu_history_stall_ms >= 8000 &&
                    visual_stall_ms >= 3000 &&
                    stall_watchdog.input_changes_since_cpu_history >= 6 &&
                    now64 - stall_watchdog.last_input_change_ms <= 3000;
                if (hard_stall || input_locked_stall || visual_locked_stall || cpu_loop_spin_stall) {
                    std::array<uint32_t, 256 * 224> stall_frame_snapshot{};
                    {
                        std::lock_guard<std::mutex> frame_lock(frame_mutex);
                        stall_frame_snapshot = present_frame;
                    }
                    RuntimeReportOptions stall_options;
                    stall_options.filename_prefix = "stall";
                    stall_options.header = "STALL REPORT";
                    stall_options.stall_type = hard_stall
                        ? "no_emulation_progress"
                        : (input_locked_stall
                            ? "input_locked_state"
                            : (visual_locked_stall ? "input_locked_visual" : "input_locked_cpu_loop"));
                    stall_options.emulation_stall_ms = emulation_stall_ms;
                    stall_options.state_stall_ms = state_stall_ms;
                    stall_options.visual_stall_ms = visual_stall_ms;
                    stall_options.cpu_history_stall_ms = cpu_history_stall_ms;
                    stall_options.input_changes = stall_watchdog.input_changes_since_state;
                    stall_options.visual_input_changes = stall_watchdog.input_changes_since_visual;
                    stall_options.cpu_history_input_changes = stall_watchdog.input_changes_since_cpu_history;
                    stall_options.last_pad1 = current_pad1;
                    stall_options.last_pad2 = current_pad2;
                    stall_options.window_focused = window_focused;
                    stall_options.cpu_halted = cpu_halted.load(std::memory_order_relaxed);
                    stall_options.framebuffer_override = stall_frame_snapshot.data();
                    {
                        std::lock_guard<std::mutex> system_lock(system_mutex);
                        WriteDiagnosticReport(stall_options);
                    }
                    std::cout << "[STALL] Diagnostic report written to output/\n";
                    stall_watchdog.episode_reported = true;
                }
            }

            if (manual_report_requested) {
                std::array<uint32_t, 256 * 224> manual_frame_snapshot{};
                {
                    std::lock_guard<std::mutex> frame_lock(frame_mutex);
                    manual_frame_snapshot = present_frame;
                }
                RuntimeReportOptions manual_options;
                manual_options.filename_prefix = "manual";
                manual_options.header = "MANUAL REPORT";
                manual_options.stall_type = "manual_snapshot";
                manual_options.emulation_stall_ms = emulation_stall_ms;
                manual_options.state_stall_ms = state_stall_ms;
                manual_options.visual_stall_ms = visual_stall_ms;
                manual_options.cpu_history_stall_ms = cpu_history_stall_ms;
                manual_options.input_changes = stall_watchdog.input_changes_since_state;
                manual_options.visual_input_changes = stall_watchdog.input_changes_since_visual;
                manual_options.cpu_history_input_changes = stall_watchdog.input_changes_since_cpu_history;
                manual_options.last_pad1 = current_pad1;
                manual_options.last_pad2 = current_pad2;
                manual_options.window_focused = window_focused;
                manual_options.cpu_halted = cpu_halted.load(std::memory_order_relaxed);
                manual_options.framebuffer_override = manual_frame_snapshot.data();
                {
                    std::lock_guard<std::mutex> system_lock(system_mutex);
                    WriteDiagnosticReport(manual_options);
                }
                std::cout << "[REPORT] Manual diagnostics written to output/\n";
                manual_report_requested = false;
            }

            if (viz && viz->isOpen() &&
                now >= next_viz_update_ms &&
                now >= viz_resume_ms) {
                std::unique_lock<std::mutex> system_lock(system_mutex, std::try_to_lock);
                if (system_lock.owns_lock()) {
                    viz->update(snes.getCPU(), snes.getPPU(), *snes.getAPU());
                    next_viz_update_ms = now + 66;
                    g_runtime_crash_stats.visualizer_fullscreen.store(viz->isFullscreen(), std::memory_order_relaxed);
                }
            }
            if (manip && manip->isOpen() &&
                now >= next_manip_update_ms &&
                now >= manip_resume_ms) {
                std::unique_lock<std::mutex> system_lock(system_mutex, std::try_to_lock);
                if (system_lock.owns_lock()) {
                    manip->update(*snes.getAPU(), snes.getPPU());
                    next_manip_update_ms = now + 66;
                }
            }

            if (!renderer_vsync) {
                const uint64_t elapsed = SDL_GetPerformanceCounter() - frame_start;
                if (elapsed < target_ticks) {
                    const uint32_t delay = (uint32_t)(((target_ticks - elapsed) * 1000ull) / perf_freq);
                    if (delay > 0 && delay < 50) SDL_Delay(delay);
                }
            }

            frame_count++;
            if (now - last_fps_time >= 1000) {
                char title[128];
                std::snprintf(title, sizeof(title), "Super Furamicom | %u FPS", frame_count);
                SDL_SetWindowTitle(window, title);
                frame_count = 0;
                last_fps_time = now;
            }

            if (cpu_halted.load(std::memory_order_relaxed) && !reported_halt) {
                reported_halt = true;
                std::cout << "[SYSTEM] CPU halted\n";
            }
        }

        live_drag_render_active.store(false, std::memory_order_relaxed);
        ClearLiveMainWindowRenderState();
        emulation_running.store(false, std::memory_order_release);
        if (emulation_thread.joinable()) emulation_thread.join();

        do_visualize = (viz != nullptr);
        do_manipulator = (manip != nullptr);
        g_runtime_crash_stats.visualizer_open.store(false, std::memory_order_relaxed);
        g_runtime_crash_stats.visualizer_fullscreen.store(false, std::memory_order_relaxed);
        closeVisualizer();
        closeManipulator();
        if (audio_device) {
            SDL_CloseAudioDevice(audio_device);
        }

        snes.saveSRAM();
        g_frame_counter_for_crash = nullptr;
        active_rom_path.clear();
        if (!return_to_launcher) {
            app_running = false;
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    RestoreMainWindowDragRenderer();
    SDL_DestroyWindow(window);
    SDL_Quit();
    if (dump_state) snes.dumpDebugState(std::cout);
    return 0;

#if 0

    if (rom_path.empty()) {
        std::vector<std::string> rom_paths;
        AppendUniquePaths(rom_paths, DiscoverROMs("roms"));
        AppendUniquePaths(rom_paths, DiscoverROMs("."));
        LauncherResult launcher = RunLauncher(window, renderer, texture, rom_paths, controls_path, input_config, do_visualize, do_integer_scaling);
        if (!launcher.launch_requested) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }

        input_config = launcher.input_config;
        do_visualize = launcher.visualize;
        do_integer_scaling = launcher.integer_scaling;
        rom_path = launcher.rom_path;
        if (!snes.loadROM(rom_path)) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 1;
        }
        SDL_SetWindowTitle(window, "Super Furamicom");
    }

    g_rom_path_for_crash = rom_path.c_str();
    SDL_RenderSetIntegerScale(renderer, do_integer_scaling ? SDL_TRUE : SDL_FALSE);

    SDL_AudioSpec desired = {};
    SDL_AudioSpec obtained = {};
    desired.freq = APU::kOutputHz; desired.format = AUDIO_S16SYS;
    desired.channels = 2; desired.samples = 256;
    desired.callback = SDLAudioCallback; desired.userdata = snes.getAPU();
    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, &obtained, 0);
    if (audio_device) SDL_PauseAudioDevice(audio_device, 0);

    Visualizer* viz = nullptr;
    if (do_visualize) {
        viz = new Visualizer();
        if (!viz->init()) { delete viz; viz = nullptr; }
        else {
            viz->placeBeside(window);
            viz->show();
            SDL_RaiseWindow(window);
            std::cout << "[VIZ] Visualizer active (1=CPU 2=PPU 3=APU 4=MEM)\n";
        }
    }

    bool running = true;
    bool main_fullscreen = false;
    SDL_Rect main_windowed_bounds{};
    SDL_GetWindowPosition(window, &main_windowed_bounds.x, &main_windowed_bounds.y);
    SDL_GetWindowSize(window, &main_windowed_bounds.w, &main_windowed_bounds.h);

    auto toggleMainFullscreen = [&]() {
        if (!main_fullscreen) {
            SDL_GetWindowPosition(window, &main_windowed_bounds.x, &main_windowed_bounds.y);
            SDL_GetWindowSize(window, &main_windowed_bounds.w, &main_windowed_bounds.h);
            if (SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP) == 0)
                main_fullscreen = true;
        } else {
            if (SDL_SetWindowFullscreen(window, 0) == 0) {
                main_fullscreen = false;
                SDL_SetWindowPosition(window, main_windowed_bounds.x, main_windowed_bounds.y);
                SDL_SetWindowSize(window, main_windowed_bounds.w, main_windowed_bounds.h);
            }
        }
    };

    SDL_Event event;
    uint32_t frame_count = 0, total_frames = 0, last_fps_time = SDL_GetTicks();
    g_frame_counter_for_crash = &total_frames;
    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    const uint64_t target_ticks = perf_freq / 60;
    bool reported_halt = false;

    std::array<uint32_t, 256 * 224> present_frame{};
    std::memcpy(present_frame.data(), snes.getFramebuffer(), present_frame.size() * sizeof(uint32_t));

    std::mutex frame_mutex;
    std::mutex system_mutex;
    std::atomic<uint16_t> pending_pad1{0};
    std::atomic<uint16_t> pending_pad2{0};
    std::atomic<bool> emulation_running{true};
    std::atomic<bool> cpu_halted{snes.isCPUHalted()};
    uint32_t next_viz_update_ms = 0;
    uint32_t viz_resume_ms = 0;

    std::thread emulation_thread([&]() {
        using clock = std::chrono::steady_clock;
        auto next_frame = clock::now();
        const std::size_t callback_frames = audio_device
            ? std::max<std::size_t>(128, obtained.samples != 0 ? obtained.samples : desired.samples)
            : 256;
        const std::size_t audio_frames_per_emu_frame = (APU::kOutputHz + 59) / 60;
        const std::size_t kTargetQueuedAudioFrames = callback_frames + audio_frames_per_emu_frame;

        while (emulation_running.load(std::memory_order_acquire)) {
            const auto now = clock::now();
            const bool behind_video = now >= next_frame;
            const bool need_audio = snes.getAPU()->getQueuedAudioFrames() < kTargetQueuedAudioFrames;

            if (!behind_video && !need_audio) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }

            if (behind_video) {
                next_frame += std::chrono::microseconds(16667);
            }

            if (!cpu_halted.load(std::memory_order_relaxed)) {
                std::lock_guard<std::mutex> system_lock(system_mutex);
                snes.setJoypad(pending_pad1.load(std::memory_order_relaxed),
                               pending_pad2.load(std::memory_order_relaxed));
                snes.stepFrame();
                cpu_halted.store(snes.isCPUHalted(), std::memory_order_relaxed);

                const uint32_t* frame = snes.getFramebuffer();
                std::lock_guard<std::mutex> frame_lock(frame_mutex);
                std::memcpy(present_frame.data(), frame, present_frame.size() * sizeof(uint32_t));
            }

            if (next_frame + std::chrono::milliseconds(250) < now) {
                next_frame = now;
            }
        }
    });

    while (running) {
        uint64_t frame_start = SDL_GetPerformanceCounter();

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) { running = false; break; }

            uint32_t event_wid = 0;
            if (event.type == SDL_WINDOWEVENT)  event_wid = event.window.windowID;
            else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
                event_wid = event.key.windowID;
            else if (event.type == SDL_MOUSEBUTTONDOWN || event.type == SDL_MOUSEBUTTONUP)
                event_wid = event.button.windowID;
            else if (event.type == SDL_MOUSEMOTION)
                event_wid = event.motion.windowID;
            else if (event.type == SDL_MOUSEWHEEL)
                event_wid = event.wheel.windowID;

            if (viz && viz->isOpen() && event_wid == viz->getWindowID()) {
                if (event.type == SDL_WINDOWEVENT) {
                    switch (event.window.event) {
                    case SDL_WINDOWEVENT_MOVED:
                    case SDL_WINDOWEVENT_RESIZED:
                    case SDL_WINDOWEVENT_SIZE_CHANGED:
                    case SDL_WINDOWEVENT_EXPOSED:
                        viz_resume_ms = SDL_GetTicks() + 200;
                        break;
                    default:
                        break;
                    }
                }
                if (!viz->processEvent(event)) {
                    delete viz; viz = nullptr;
                }
                continue;
            }

            if (event.type == SDL_WINDOWEVENT && event_wid == main_wid) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
                switch (event.window.event) {
                case SDL_WINDOWEVENT_FOCUS_LOST:
                    pending_pad1.store(0, std::memory_order_relaxed);
                    pending_pad2.store(0, std::memory_order_relaxed);
                    break;
                case SDL_WINDOWEVENT_MOVED:
                case SDL_WINDOWEVENT_RESIZED:
                case SDL_WINDOWEVENT_SIZE_CHANGED:
                case SDL_WINDOWEVENT_EXPOSED:
                    viz_resume_ms = SDL_GetTicks() + 200;
                    break;
                default:
                    break;
                }
            }

            if (event.type == SDL_KEYDOWN) {
                if (event.key.keysym.sym == SDLK_ESCAPE) running = false;
                if (event.key.keysym.sym == SDLK_F5) {
                    if (!viz) {
                        viz = new Visualizer();
                        if (!viz->init()) { delete viz; viz = nullptr; }
                        else {
                            viz->placeBeside(window);
                            viz->show();
                            next_viz_update_ms = 0;
                            viz_resume_ms = SDL_GetTicks() + 200;
                            SDL_RaiseWindow(window);
                            std::cout << "[VIZ] Opened\n";
                        }
                    } else {
                        viz->destroy(); delete viz; viz = nullptr;
                        std::cout << "[VIZ] Closed\n";
                    }
                }
                if (event.key.keysym.sym == SDLK_F11) {
                    toggleMainFullscreen();
                }
                if (event.key.keysym.sym == SDLK_RETURN && (event.key.keysym.mod & KMOD_ALT)) {
                    toggleMainFullscreen();
                }
            }
        }

        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        pending_pad1.store(MapInputConfigToPad(keys, input_config), std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex);
            SDL_UpdateTexture(texture, NULL, present_frame.data(), 256 * sizeof(uint32_t));
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        const uint32_t now = SDL_GetTicks();
        if (viz && viz->isOpen() &&
            now >= next_viz_update_ms &&
            now >= viz_resume_ms) {
            std::unique_lock<std::mutex> system_lock(system_mutex, std::try_to_lock);
            if (system_lock.owns_lock()) {
                viz->update(snes.getCPU(), snes.getPPU(), *snes.getAPU());
                next_viz_update_ms = now + 66;
            }
        }

        uint64_t elapsed = SDL_GetPerformanceCounter() - frame_start;
        if (elapsed < target_ticks) {
            uint32_t delay = (uint32_t)(((target_ticks - elapsed)*1000) / perf_freq);
            if (delay > 0 && delay < 50) SDL_Delay(delay);
        }

        frame_count++;
        total_frames++;
        if (now - last_fps_time >= 1000) {
            char title[128];
            snprintf(title, sizeof(title), "Super Furamicom | %u FPS", frame_count);
            SDL_SetWindowTitle(window, title);
            frame_count = 0; last_fps_time = now;
        }

        if (cpu_halted.load(std::memory_order_relaxed) && !reported_halt) {
            reported_halt = true;
            std::cout << "[SYSTEM] CPU halted\n";
        }
    }

    emulation_running.store(false, std::memory_order_release);
    if (emulation_thread.joinable()) emulation_thread.join();

    if (viz) { viz->destroy(); delete viz; }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    SDL_Quit();
    if (dump_state) snes.dumpDebugState(std::cout);
    return 0;
#endif
}
