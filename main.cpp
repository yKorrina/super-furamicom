#include "cpu.hpp"
#include "bus.hpp"
#include "ppu.hpp"
#include "cartridge.hpp"
#include <iostream>
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
            std::cout << "Super Furamicom Powered On.\n";
        }
    }

    void stepFrame() {
        // 1. Run CPU logic for the duration of one screen draw
        for(int i = 0; i < 35000; i++) {
            cpu.step();
        }
        
        // 2. Only interrupt the CPU with a Screen Draw if the game actually wants it!
        if (bus.isNMIEnabled()) {
            cpu.nmi();
        }
        
        // 3. Render the VRAM Viewer
        ppu.renderFrame();
    }

    const uint32_t* getFramebuffer() const { return ppu.getFramebuffer(); }
    ~System() { delete cartridge; }

private:
    CPU cpu;
    PPU ppu; 
    APU apu; 
    Bus bus;
    Cartridge* cartridge = nullptr;
};

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;

    SDL_Init(SDL_INIT_VIDEO);
    
    SDL_Window* window = SDL_CreateWindow(
        "Super Furamicom: Dev Viewer",
        SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        256 * 3, 224 * 3, // Scaled 3x
        SDL_WINDOW_SHOWN
    );

    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    
    SDL_Texture* texture = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_ARGB8888, 
        SDL_TEXTUREACCESS_STREAMING, 256, 224
    );

    System snes;
    snes.loadROM(argv[1]);
    
    bool running = true;
    SDL_Event event;

    while (running) {
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_QUIT) running = false;
        }

        snes.stepFrame();

        SDL_UpdateTexture(texture, NULL, snes.getFramebuffer(), 256 * sizeof(uint32_t));
        
        SDL_RenderClear(renderer);
        SDL_RenderCopy(renderer, texture, NULL, NULL);
        SDL_RenderPresent(renderer);
    }

    SDL_DestroyTexture(texture);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    
    return 0;
}