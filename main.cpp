#include "cpu.hpp"
#include "bus.hpp"
#include "apu.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include "visualizer.hpp"
#include <iostream>
#include <fstream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
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

class System {
public:
    System() : bus(&cpu, &ppu, &apu) {}

    void loadROM(const std::string& filepath) {
        rom_filepath = filepath;
        cartridge = new Cartridge(filepath);
        if (cartridge->isLoaded()) {
            bus.insertCartridge(cartridge);
            std::string srm = filepath + ".srm";
            cartridge->loadSRAM(srm);
            apu.reset();
            cpu.reset(); 
            std::cout << "=== Super Furamicom Powered On ===\n";
        } else {
            std::cerr << "Failed to load ROM: " << filepath << "\n";
        }
    }

    void enableTrace(const std::string& path) { cpu.enableTrace(path); }

    void stepFrame() {
        constexpr int CYCLES_PER_FRAME = 29781;
        constexpr int VBLANK_CYCLES = 4510;
        constexpr int VISIBLE_CYCLES = CYCLES_PER_FRAME - VBLANK_CYCLES;

        ppu.setVBlank(false);
        int c = 0;
        while (c < VISIBLE_CYCLES) {
            int used = cpu.step();
            c += used;
            apu.tickCPUCycles(used);
            if (cpu.isHalted()) break;
        }

        ppu.renderFrame();
        if (bus.isNMIEnabled()) cpu.nmi();

        c = 0;
        while (c < VBLANK_CYCLES) {
            int used = cpu.step();
            c += used;
            apu.tickCPUCycles(used);
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
    const PPU& getPPU() const { return ppu; }
    APU* getAPU() { return &apu; }

    void dumpDebugState(std::ostream& out) const {
        const auto& cpu_ports = apu.getCpuToSPCPorts();
        const auto& spc_ports = apu.getSPCToCpuPorts();
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
            << " DSP_KON=$" << (unsigned)apu.getDSPRegister(0x4C)
            << " audio_frames=" << std::dec << apu.getGeneratedAudioFrames()
            << " audio_nonzero=" << apu.getNonZeroAudioFrames()
            << " audio_peak=" << apu.getAudioPeakSample()
            << " cpu_ports="
            << std::hex
            << (unsigned)cpu_ports[0] << "," << (unsigned)cpu_ports[1] << ","
            << (unsigned)cpu_ports[2] << "," << (unsigned)cpu_ports[3]
            << " spc_ports="
            << (unsigned)spc_ports[0] << "," << (unsigned)spc_ports[1] << ","
            << (unsigned)spc_ports[2] << "," << (unsigned)spc_ports[3]
            << " unsupported=" << std::dec << apu.hasUnsupportedOpcode();
        if (apu.hasUnsupportedOpcode()) {
            out << " bad_op=$" << std::hex << (unsigned)apu.getUnsupportedOpcode()
                << " bad_pc=$" << apu.getUnsupportedPC();
        }
        out
            << std::dec << "\n";
    }

    ~System() { saveSRAM(); delete cartridge; }

private:
    CPU cpu; PPU ppu; APU apu; Bus bus;
    Cartridge* cartridge = nullptr;
    std::string rom_filepath;
};

uint16_t mapKeyboard(const uint8_t* keys) {
    uint16_t pad = 0;
    if (keys[SDL_SCANCODE_X])      pad |= 0x8000;
    if (keys[SDL_SCANCODE_Z])      pad |= 0x4000;
    if (keys[SDL_SCANCODE_RSHIFT] || keys[SDL_SCANCODE_BACKSPACE]) pad |= 0x2000;
    if (keys[SDL_SCANCODE_RETURN]) pad |= 0x1000;
    if (keys[SDL_SCANCODE_UP])     pad |= 0x0008;
    if (keys[SDL_SCANCODE_DOWN])   pad |= 0x0004;
    if (keys[SDL_SCANCODE_LEFT])   pad |= 0x0002;
    if (keys[SDL_SCANCODE_RIGHT])  pad |= 0x0001;
    if (keys[SDL_SCANCODE_S])      pad |= 0x0080;
    if (keys[SDL_SCANCODE_A])      pad |= 0x0040;
    if (keys[SDL_SCANCODE_Q])      pad |= 0x0800;
    if (keys[SDL_SCANCODE_W])      pad |= 0x0400;
    return pad;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Super Furamicom — SNES Emulator\n\n"
                  << "Usage: snes_emu <rom.sfc> [options]\n"
                  << "  --trace              CPU trace to trace.log\n"
                  << "  --visualize          Open PPU debug window\n"
                  << "  --headless-frames N  Run N frames and exit\n"
                  << "  --dump-state         Print CPU/PPU state\n\n"
                  << "Controls:  Arrows=D-Pad  Z/X=Y/B  A/S=X/A  Q/W=L/R\n"
                  << "  Enter=Start  Backspace=Select  Escape=Quit  F5=Visualizer\n";
        return 1;
    }

    bool do_trace = false, do_visualize = false, dump_state = false;
    int headless_frames = 0;
    std::string dump_frame_path;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) do_trace = true;
        else if (strcmp(argv[i], "--visualize") == 0) do_visualize = true;
        else if (strcmp(argv[i], "--dump-state") == 0) dump_state = true;
        else if (strcmp(argv[i], "--dump-frame") == 0 && (i+1) < argc)
            dump_frame_path = argv[++i];
        else if (strcmp(argv[i], "--headless-frames") == 0 && (i+1) < argc)
            headless_frames = std::max(0, std::atoi(argv[++i]));
    }

    System snes;
    if (do_trace) { snes.enableTrace("trace.log"); std::cout << "[TRACE] Writing to trace.log\n"; }
    snes.loadROM(argv[1]);

    if (headless_frames > 0) {
        for (int i = 0; i < headless_frames && !snes.isCPUHalted(); i++) snes.stepFrame();
        if (!dump_frame_path.empty()) {
            if (!WriteFramebufferBMP(dump_frame_path, snes.getFramebuffer())) {
                std::cerr << "[FRAME] Failed to write " << dump_frame_path << "\n";
            }
        }
        if (dump_state) snes.dumpDebugState(std::cout);
        return snes.isCPUHalted() ? 2 : 0;
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);

    SDL_Window* window = SDL_CreateWindow("Super Furamicom",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256*3, 224*3, SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    SDL_RenderSetLogicalSize(renderer, 256, 224);
    SDL_Texture* texture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888,
        SDL_TEXTUREACCESS_STREAMING, 256, 224);
    uint32_t main_wid = SDL_GetWindowID(window);

    SDL_AudioSpec desired = {};
    desired.freq = APU::kOutputHz; desired.format = AUDIO_S16SYS;
    desired.channels = 2; desired.samples = 1024;
    desired.callback = SDLAudioCallback; desired.userdata = snes.getAPU();
    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device) SDL_PauseAudioDevice(audio_device, 0);

    Visualizer* viz = nullptr;
    if (do_visualize) {
        viz = new Visualizer();
        if (!viz->init()) { delete viz; viz = nullptr; }
        else std::cout << "[VIZ] PPU Visualizer active (1=Tiles 2=Palette 3=BG Map)\n";
    }

    bool running = true;
    SDL_Event event;
    uint32_t frame_count = 0, last_fps_time = SDL_GetTicks();
    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    const uint64_t target_ticks = perf_freq / 60;

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
                        else std::cout << "[VIZ] Opened\n";
                    } else {
                        viz->destroy(); delete viz; viz = nullptr;
                        std::cout << "[VIZ] Closed\n";
                    }
                }
            }
        }

        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        snes.setJoypad(mapKeyboard(keys));
        snes.stepFrame();

        SDL_UpdateTexture(texture, NULL, snes.getFramebuffer(), 256*sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        if (viz && viz->isOpen()) viz->update(snes.getPPU());

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

        if (snes.isCPUHalted()) {
            std::cout << "[SYSTEM] CPU halted\n";
            while (running) {
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT || event.type == SDL_KEYDOWN) running = false;
                }
                SDL_Delay(100);
            }
        }
    }

    if (viz) { viz->destroy(); delete viz; }
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (audio_device) SDL_CloseAudioDevice(audio_device);
    SDL_Quit();
    if (dump_state) snes.dumpDebugState(std::cout);
    return 0;
}
