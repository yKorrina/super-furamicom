#include "cpu.hpp"
#include "bus.hpp"
#include "apu.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include "launcher.hpp"
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
#include <SDL2/SDL.h>

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
            apu.reset();
            cpu.reset();
            std::cout << "=== Super Furamicom Powered On ===\n";
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
        constexpr int CYCLES_PER_FRAME = 29781;
        constexpr int VBLANK_CYCLES = 4510;
        constexpr int VISIBLE_CYCLES = CYCLES_PER_FRAME - VBLANK_CYCLES;

        ppu.setVBlank(false);
        bus.endVBlank();
        ppu.beginFrame();
        ppu.setHBlank(true);
        bus.beginFrame();
        ppu.setHBlank(false);

        int visible_elapsed = 0;
        auto runVisibleTo = [&](int target_cycles) {
            while (visible_elapsed < target_cycles) {
                int used = cpu.step();
                visible_elapsed += used;
                apu.tickCPUCycles(used);
                if (bus.consumeIRQ()) cpu.irq();
                if (cpu.isHalted()) break;
            }
        };
        runVisibleTo(VISIBLE_CYCLES / 225);

        for (int scanline = 0; scanline < 224 && !cpu.isHalted(); scanline++) {
            bus.beginScanline(scanline);
            if (bus.consumeIRQ()) cpu.irq();
            ppu.renderScanline(scanline);

            if (scanline + 1 < 224) {
                ppu.setHBlank(true);
                bus.beginHBlank(scanline);
                if (bus.consumeIRQ()) cpu.irq();
                bus.stepHDMA();
                ppu.setHBlank(false);
            }

            const int target_cycles = ((scanline + 2) * VISIBLE_CYCLES) / 225;
            runVisibleTo(target_cycles);
        }

        ppu.endFrame();
        bus.startVBlank();
        if (bus.consumeNMI()) cpu.nmi();

        int c = 0;
        while (c < VBLANK_CYCLES) {
            int used = cpu.step();
            c += used;
            apu.tickCPUCycles(used);
            if (bus.consumeIRQ()) cpu.irq();
            if (cpu.isHalted()) break;
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
    const PPU& getPPU() const { return ppu; }
    APU* getAPU() { return &apu; }

    void dumpDebugState(std::ostream& out) const {
        const auto& cpu_ports = apu.getCpuToSPCPorts();
        const auto& spc_ports = apu.getSPCToCpuPorts();
        const auto& cpu_port_writes = apu.getCpuPortWriteCounts();
        const auto& spc_port_writes = apu.getSpcPortWriteCounts();
        const auto& last_cpu_writes = apu.getLastCpuPortWrites();
        const auto& last_spc_writes = apu.getLastSpcPortWrites();
        out << "[CPU] PC=" << std::hex << cpu.getProgramCounter()
            << " A=" << cpu.getA() << " X=" << cpu.getX() << " Y=" << cpu.getY()
            << " SP=" << cpu.getSP() << " P=" << (unsigned)cpu.getP()
            << " E=" << cpu.isEmulationMode() << std::dec << "\n";
        out << "[PPU] INIDISP=$" << std::hex << (unsigned)ppu.getINIDISP()
            << " BGMODE=$" << (unsigned)ppu.getBGMode()
            << " TM=$" << (unsigned)ppu.getTMMain()
            << " TS=$" << (unsigned)ppu.getTMSub()
            << " BG1SC=$" << (unsigned)ppu.getBGSC(0)
            << " BG2SC=$" << (unsigned)ppu.getBGSC(1)
            << " BG3SC=$" << (unsigned)ppu.getBGSC(2)
            << " BG12NBA=$" << (unsigned)ppu.getBGNBA(0)
            << " BG34NBA=$" << (unsigned)ppu.getBGNBA(1)
            << " HOFS1=$" << ppu.getBGHOFS(0)
            << " VOFS1=$" << ppu.getBGVOFS(0)
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
            << " VBL=" << ppu.getVBlank() << std::dec << "\n";
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
            << " audio_frames=" << std::dec << apu.getGeneratedAudioFrames()
            << " audio_nonzero=" << apu.getNonZeroAudioFrames()
            << " audio_peak=" << apu.getAudioPeakSample()
            << " audio_queue=" << apu.getQueuedAudioFrames()
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
                out << "[APU-BRR" << i << "]"
                    << " start_hdr=$" << std::hex << (unsigned)aram[start]
                    << " start_b1=$" << (unsigned)aram[(uint16_t)(start + 1)]
                    << " start_b2=$" << (unsigned)aram[(uint16_t)(start + 2)]
                    << " loop_hdr=$" << (unsigned)aram[loop]
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
              << "  --headless-frames N  Run N frames and exit\n"
              << "  --pad1-script SPEC   Headless pad script, e.g. 0-15:1000,60-63:8000\n"
              << "  --dump-frame PATH    Save the final frame as a BMP\n"
              << "  --dump-state         Print CPU/PPU/APU state on exit\n\n"
              << "When no ROM path is provided, the launcher scans ./roms and ./ for .sfc/.smc files.\n"
              << "Controls can be remapped from the launcher, and F5 still toggles the visualizer in-game.\n";
}

static void AppendUniquePaths(std::vector<std::string>& target, const std::vector<std::string>& source) {
    for (const auto& path : source) {
        if (std::find(target.begin(), target.end(), path) == target.end()) {
            target.push_back(path);
        }
    }
}

int main(int argc, char* argv[]) {
    bool do_trace = false, do_visualize = false, dump_state = false;
    int headless_frames = 0;
    std::string rom_path;
    std::string dump_frame_path;
    std::string pad1_script_spec;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) do_trace = true;
        else if (strcmp(argv[i], "--visualize") == 0) do_visualize = true;
        else if (strcmp(argv[i], "--dump-state") == 0) dump_state = true;
        else if (strcmp(argv[i], "--dump-frame") == 0 && (i+1) < argc)
            dump_frame_path = argv[++i];
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
    if (do_trace) { snes.enableTrace("trace.log"); std::cout << "[TRACE] Writing to trace.log\n"; }
    InputConfig input_config = DefaultInputConfig();
    const std::string controls_path = "controls.cfg";
    LoadInputConfig(controls_path, input_config);

    if (!rom_path.empty() && !snes.loadROM(rom_path)) {
        return 1;
    }

    if (headless_frames > 0) {
        for (int i = 0; i < headless_frames && !snes.isCPUHalted(); i++) {
            snes.setJoypad(PadScriptMaskForFrame(pad1_script, i));
            snes.stepFrame();
        }
        if (!dump_frame_path.empty()) {
            if (!WriteFramebufferBMP(dump_frame_path, snes.getFramebuffer())) {
                std::cerr << "[FRAME] Failed to write " << dump_frame_path << "\n";
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

    SDL_Window* window = SDL_CreateWindow("Super Furamicom",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256*3, 224*3, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!window) {
        std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
        SDL_Quit();
        return 1;
    }

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::cerr << "SDL_CreateRenderer failed: " << SDL_GetError() << "\n";
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }

    SDL_RenderSetLogicalSize(renderer, 256, 224);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 224);
    if (!texture) {
        std::cerr << "SDL_CreateTexture failed: " << SDL_GetError() << "\n";
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    uint32_t main_wid = SDL_GetWindowID(window);

    if (rom_path.empty()) {
        std::vector<std::string> rom_paths;
        AppendUniquePaths(rom_paths, DiscoverROMs("roms"));
        AppendUniquePaths(rom_paths, DiscoverROMs("."));
        LauncherResult launcher = RunLauncher(window, renderer, texture, rom_paths, controls_path, input_config, do_visualize);
        if (!launcher.launch_requested) {
            SDL_DestroyTexture(texture);
            SDL_DestroyRenderer(renderer);
            SDL_DestroyWindow(window);
            SDL_Quit();
            return 0;
        }

        input_config = launcher.input_config;
        do_visualize = launcher.visualize;
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
            std::cout << "[VIZ] Visualizer active (1=CPU 2=PPU 3=APU)\n";
        }
    }

    bool running = true;
    SDL_Event event;
    uint32_t frame_count = 0, last_fps_time = SDL_GetTicks();
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

        // ═════════════════════════════════════════════════════════════════
        //  SINGLE event loop — route events to the right window
        // ═════════════════════════════════════════════════════════════════
        while (SDL_PollEvent(&event)) {
            // Global quit
            if (event.type == SDL_QUIT) { running = false; break; }

            // Figure out which window this event belongs to
            uint32_t event_wid = 0;
            if (event.type == SDL_WINDOWEVENT)  event_wid = event.window.windowID;
            else if (event.type == SDL_KEYDOWN || event.type == SDL_KEYUP)
                event_wid = event.key.windowID;

            // Route to visualizer if it's for that window
            if (viz && viz->isOpen() && event_wid == viz->getWindowID()) {
                if (!viz->processEvent(event)) {
                    delete viz; viz = nullptr;
                }
                continue;  // Don't process this event in main window
            }

            // Main window events
            if (event.type == SDL_WINDOWEVENT && event_wid == main_wid) {
                if (event.window.event == SDL_WINDOWEVENT_CLOSE) running = false;
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
                            SDL_RaiseWindow(window);
                            std::cout << "[VIZ] Opened\n";
                        }
                    } else {
                        viz->destroy(); delete viz; viz = nullptr;
                        std::cout << "[VIZ] Closed\n";
                    }
                }
            }
        }

        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        pending_pad1.store(MapInputConfigToPad(keys, input_config), std::memory_order_relaxed);

        {
            std::lock_guard<std::mutex> frame_lock(frame_mutex);
            SDL_UpdateTexture(texture, NULL, present_frame.data(), 256 * sizeof(uint32_t));
        }
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        if (viz && viz->isOpen()) {
            std::unique_lock<std::mutex> system_lock(system_mutex, std::try_to_lock);
            if (system_lock.owns_lock()) {
                viz->update(snes.getCPU(), snes.getPPU(), *snes.getAPU());
            }
        }

        // Frame timing
        uint64_t elapsed = SDL_GetPerformanceCounter() - frame_start;
        if (elapsed < target_ticks) {
            uint32_t delay = (uint32_t)(((target_ticks - elapsed)*1000) / perf_freq);
            if (delay > 0 && delay < 50) SDL_Delay(delay);
        }

        frame_count++;
        uint32_t now = SDL_GetTicks();
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
}
