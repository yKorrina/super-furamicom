#include "cpu.hpp"
#include "bus.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include <iostream>
#include <cstring>
#include <cstdlib>
#include <cstdint>
#include <SDL2/SDL.h>

class APU {
public:
    static constexpr int kOutputHz = 32040;

    void mix(int16_t* stream, int stereo_frames) {
        std::memset(stream, 0, stereo_frames * 2 * sizeof(int16_t));
    }
};

static void SDLAudioCallback(void* userdata, Uint8* stream, int len) {
    auto* apu = static_cast<APU*>(userdata);
    if (!apu) {
        std::memset(stream, 0, len);
        return;
    }

    apu->mix(reinterpret_cast<int16_t*>(stream), len / (int)sizeof(int16_t) / 2);
}

class System {
public:
    System() : bus(&cpu, &ppu, &apu) {}

    void loadROM(const std::string& filepath) {
        rom_filepath = filepath;
        cartridge = new Cartridge(filepath);
        if (cartridge->isLoaded()) {
            bus.insertCartridge(cartridge);
            
            // Load SRAM from disk if it exists
            std::string srm = filepath + ".srm";
            cartridge->loadSRAM(srm);
            
            cpu.reset(); 
            std::cout << "=== Super Furamicom Powered On ===\n";
        } else {
            std::cerr << "Failed to load ROM: " << filepath << "\n";
        }
    }

    void enableTrace(const std::string& path) {
        cpu.enableTrace(path);
    }

    void stepFrame() {
        constexpr int CYCLES_PER_FRAME = 29781;
        constexpr int VBLANK_CYCLES = 4510;
        constexpr int VISIBLE_CYCLES = CYCLES_PER_FRAME - VBLANK_CYCLES;

        ppu.setVBlank(false);

        int cycles_this_phase = 0;
        while (cycles_this_phase < VISIBLE_CYCLES) {
            uint8_t c = cpu.step();
            cycles_this_phase += c;

            if (cpu.isHalted()) {
                std::cout << "[SYSTEM] CPU halted — check trace log\n";
                break;
            }
        }

        ppu.renderFrame();

        if (bus.isNMIEnabled()) {
            cpu.nmi();
        }

        cycles_this_phase = 0;
        while (cycles_this_phase < VBLANK_CYCLES) {
            uint8_t c = cpu.step();
            cycles_this_phase += c;

            if (cpu.isHalted()) {
                std::cout << "[SYSTEM] CPU halted — check trace log\n";
                break;
            }
        }
    }

    void setJoypad(uint16_t pad1, uint16_t pad2 = 0) {
        bus.setJoypadState(pad1, pad2);
    }

    void saveSRAM() {
        if (cartridge && cartridge->hasSRAM()) {
            std::string srm = rom_filepath + ".srm";
            cartridge->saveSRAM(srm);
        }
    }

    bool isCPUHalted() const { return cpu.isHalted(); }
    const uint32_t* getFramebuffer() const { return ppu.getFramebuffer(); }
    APU* getAPU() { return &apu; }
    void dumpDebugState(std::ostream& out) const {
        out << "[CPU] PC=" << std::hex << cpu.getProgramCounter()
            << " A=" << cpu.getA()
            << " X=" << cpu.getX()
            << " Y=" << cpu.getY()
            << " SP=" << cpu.getSP()
            << " P=" << (unsigned)cpu.getP()
            << " D=" << cpu.getD()
            << " DB=" << (unsigned)cpu.getDB()
            << " E=" << cpu.isEmulationMode()
            << std::dec << "\n";
        out << "[PPU] INIDISP=$" << std::hex << (unsigned)ppu.getINIDISP()
            << " BGMODE=$" << (unsigned)ppu.getBGMode()
            << " TM=$" << (unsigned)ppu.getTMMain()
            << " TS=$" << (unsigned)ppu.getTMSub()
            << " VMAIN=$" << (unsigned)ppu.getVMAIN()
            << " VADDR=$" << ppu.getVRAMAddress()
            << " CGADDR=$" << (unsigned)ppu.getCGRAMAddress()
            << " OAMADDR=$" << ppu.getOAMAddress()
            << " VBL=" << ppu.getVBlank()
            << std::dec << "\n";
        out << "[PPU] nonzero_vram=" << ppu.countNonZeroVRAM()
            << " nonzero_cgram=" << ppu.countNonZeroCGRAM()
            << " nonzero_oam=" << ppu.countNonZeroOAM()
            << " nonblack_pixels=" << ppu.countNonBlackPixels()
            << "\n";
    }
    ~System() { saveSRAM(); delete cartridge; }

private:
    CPU cpu;
    PPU ppu; 
    APU apu; 
    Bus bus;
    Cartridge* cartridge = nullptr;
    std::string rom_filepath;
};

uint16_t mapKeyboard(const uint8_t* keys) {
    uint16_t pad = 0;
    if (keys[SDL_SCANCODE_X])      pad |= 0x8000;  // B
    if (keys[SDL_SCANCODE_Z])      pad |= 0x4000;  // Y
    if (keys[SDL_SCANCODE_RSHIFT]
     || keys[SDL_SCANCODE_BACKSPACE]) pad |= 0x2000; // Select
    if (keys[SDL_SCANCODE_RETURN]) pad |= 0x1000;  // Start
    if (keys[SDL_SCANCODE_UP])     pad |= 0x0008;  // Up
    if (keys[SDL_SCANCODE_DOWN])   pad |= 0x0004;  // Down
    if (keys[SDL_SCANCODE_LEFT])   pad |= 0x0002;  // Left
    if (keys[SDL_SCANCODE_RIGHT])  pad |= 0x0001;  // Right
    if (keys[SDL_SCANCODE_S])      pad |= 0x0080;  // A
    if (keys[SDL_SCANCODE_A])      pad |= 0x0040;  // X
    if (keys[SDL_SCANCODE_Q])      pad |= 0x0800;  // R shoulder
    if (keys[SDL_SCANCODE_W])      pad |= 0x0400;  // L shoulder
    return pad;
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: snes_emu <rom.sfc> [--trace] [--headless-frames N] [--dump-state]\n";
        std::cout << "  --trace   Write CPU trace to trace.log (first 500K instructions)\n";
        std::cout << "  --headless-frames N   Run N frames without opening SDL and exit\n";
        std::cout << "  --dump-state          Print CPU/PPU state before exit\n";
        std::cout << "\nControls:\n";
        std::cout << "  Arrows    D-Pad        Z/X    Y/B buttons\n";
        std::cout << "  A/S       X/A buttons  Q/W    L/R shoulders\n";
        std::cout << "  Enter     Start        Backspace  Select\n";
        std::cout << "  Escape    Quit\n";
        return 1;
    }

    bool do_trace = false;
    bool dump_state = false;
    int headless_frames = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) do_trace = true;
        else if (strcmp(argv[i], "--dump-state") == 0) dump_state = true;
        else if (strcmp(argv[i], "--headless-frames") == 0 && (i + 1) < argc) {
            headless_frames = std::max(0, std::atoi(argv[++i]));
        }
    }

    System snes;
    if (do_trace) {
        snes.enableTrace("trace.log");
        std::cout << "[TRACE] Writing CPU trace to trace.log\n";
    }
    snes.loadROM(argv[1]);

    if (headless_frames > 0) {
        for (int i = 0; i < headless_frames && !snes.isCPUHalted(); i++) {
            snes.stepFrame();
        }
        if (dump_state) {
            snes.dumpDebugState(std::cout);
        }
        return snes.isCPUHalted() ? 2 : 0;
    }

    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO);
    
    SDL_Window* window = SDL_CreateWindow(
        "Super Furamicom",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * 3, 224 * 3,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_SOFTWARE);
    }
    SDL_RenderSetLogicalSize(renderer, 256, 224);
    
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, 
        SDL_TEXTUREACCESS_STREAMING, 256, 224
    );

    SDL_AudioSpec desired = {};
    desired.freq = APU::kOutputHz;
    desired.format = AUDIO_S16SYS;
    desired.channels = 2;
    desired.samples = 1024;
    desired.callback = SDLAudioCallback;
    desired.userdata = snes.getAPU();
    SDL_AudioDeviceID audio_device = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
    if (audio_device != 0) {
        SDL_PauseAudioDevice(audio_device, 0);
    } else {
        std::cerr << "[AUDIO] SDL audio init failed: " << SDL_GetError() << "\n";
    }
    
    bool running = true;
    SDL_Event event;
    uint32_t frame_count = 0;
    uint32_t last_fps_time = SDL_GetTicks();
    uint64_t perf_freq = SDL_GetPerformanceFrequency();
    const uint64_t target_frame_ticks = perf_freq / 60;

    while (running) {
        uint64_t frame_start = SDL_GetPerformanceCounter();

        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        snes.setJoypad(mapKeyboard(keys));

        snes.stepFrame();

        SDL_UpdateTexture(texture, NULL, snes.getFramebuffer(), 256 * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        uint64_t frame_end = SDL_GetPerformanceCounter();
        uint64_t elapsed_ticks = frame_end - frame_start;
        if (elapsed_ticks < target_frame_ticks) {
            uint32_t delay_ms = (uint32_t)(((target_frame_ticks - elapsed_ticks) * 1000) / perf_freq);
            if (delay_ms > 0) {
                SDL_Delay(delay_ms);
            }
        }

        frame_count++;
        uint32_t now = SDL_GetTicks();
        if (now - last_fps_time >= 1000) {
            char title[128];
            snprintf(title, sizeof(title), "Super Furamicom | %u FPS", frame_count);
            SDL_SetWindowTitle(window, title);
            frame_count = 0;
            last_fps_time = now;
        }

        if (snes.isCPUHalted()) {
            std::cout << "[SYSTEM] CPU halted. Check trace.log for diagnostics.\n";
            while (running) {
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) running = false;
                    if (event.type == SDL_KEYDOWN) running = false;
                }
                SDL_Delay(100);
            }
        }
    }

    // SRAM auto-saves in System destructor
    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    if (audio_device != 0) {
        SDL_CloseAudioDevice(audio_device);
    }
    SDL_Quit();

    if (dump_state) {
        snes.dumpDebugState(std::cout);
    }
    
    return 0;
}
