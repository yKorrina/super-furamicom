#include "cpu.hpp"
#include "bus.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include <iostream>
#include <cstring>
#include <SDL2/SDL.h>

class APU {}; 

class System {
public:
    System() : bus(&cpu, &ppu, &apu) {}

    void loadROM(const std::string& filepath) {
        cartridge = new Cartridge(filepath);
        if (cartridge->isLoaded()) {
            bus.insertCartridge(cartridge);
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
        // Run ~1 frame worth of CPU cycles (~21477 master cycles / ~1364 scanlines)
        // Each CPU step returns cycles consumed; accumulate until a frame's worth
        int cycles_this_frame = 0;
        const int CYCLES_PER_FRAME = 29781;  // ~29781 CPU cycles per NTSC frame
        
        while (cycles_this_frame < CYCLES_PER_FRAME) {
            uint8_t c = cpu.step();
            cycles_this_frame += c;
            
            if (cpu.isHalted()) {
                std::cout << "[SYSTEM] CPU halted — check trace log\n";
                break;
            }
        }
        
        // Fire NMI if enabled
        if (bus.isNMIEnabled()) {
            cpu.nmi();
        }
        
        // Render
        ppu.renderFrame();
    }

    void setJoypad(uint16_t pad1, uint16_t pad2 = 0) {
        bus.setJoypadState(pad1, pad2);
    }

    bool isCPUHalted() const { return cpu.isHalted(); }
    const uint32_t* getFramebuffer() const { return ppu.getFramebuffer(); }
    ~System() { delete cartridge; }

private:
    CPU cpu;
    PPU ppu; 
    APU apu; 
    Bus bus;
    Cartridge* cartridge = nullptr;
};

// ─────────────────────────────────────────────────────────────────────────────
//  SNES Joypad bit layout (active HIGH for auto-read):
//    Bit 15: B       Bit 11: R       Bit 7: A        Bit 3: Up
//    Bit 14: Y       Bit 10: L       Bit 6: X        Bit 2: Down
//    Bit 13: Select  Bit  9: -       Bit 5: -        Bit 1: Left
//    Bit 12: Start   Bit  8: -       Bit 4: -        Bit 0: Right
// ─────────────────────────────────────────────────────────────────────────────

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
        std::cout << "Usage: snes_emu <rom.sfc> [--trace]\n";
        std::cout << "  --trace   Write CPU trace to trace.log (first 500K instructions)\n";
        return 1;
    }

    bool do_trace = false;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--trace") == 0) do_trace = true;
    }

    SDL_Init(SDL_INIT_VIDEO);
    
    SDL_Window* window = SDL_CreateWindow(
        "Super Furamicom",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * 3, 224 * 3,
        SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, 
        SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    SDL_RenderSetLogicalSize(renderer, 256, 224);
    
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, 
        SDL_TEXTUREACCESS_STREAMING, 256, 224
    );

    System snes;
    if (do_trace) {
        snes.enableTrace("trace.log");
        std::cout << "[TRACE] Writing CPU trace to trace.log\n";
    }
    snes.loadROM(argv[1]);
    
    bool running = true;
    SDL_Event event;
    uint32_t frame_count = 0;
    uint32_t last_fps_time = SDL_GetTicks();

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
            if (event.type == SDL_KEYDOWN && event.key.keysym.sym == SDLK_ESCAPE)
                running = false;
        }

        // Read keyboard and map to SNES pad
        const uint8_t* keys = SDL_GetKeyboardState(NULL);
        snes.setJoypad(mapKeyboard(keys));

        snes.stepFrame();

        SDL_UpdateTexture(texture, NULL, snes.getFramebuffer(), 256 * sizeof(uint32_t));
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);

        // FPS counter in title bar
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
            // Keep the window open so the user can see the last frame
            while (running) {
                while (SDL_PollEvent(&event)) {
                    if (event.type == SDL_QUIT) running = false;
                    if (event.type == SDL_KEYDOWN) running = false;
                }
                SDL_Delay(100);
            }
        }
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}
