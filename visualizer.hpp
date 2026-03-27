#ifndef VISUALIZER_HPP
#define VISUALIZER_HPP

#include <cstdint>
#include <SDL2/SDL.h>

class PPU;

class Visualizer {
public:
    Visualizer();
    ~Visualizer();

    bool init();
    void update(const PPU& ppu);
    void destroy();
    bool isOpen() const { return window != nullptr; }

    // Process a single SDL event — return false if window should close
    bool processEvent(const SDL_Event& e);
    
    // Get the window ID so the main loop can route events
    uint32_t getWindowID() const;

private:
    static constexpr int WIN_W = 512;
    static constexpr int WIN_H = 640;

    SDL_Window*   window   = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture*  texture  = nullptr;

    uint32_t pixels[512 * 640];
    int active_panel = 0;

    void decodeTile(const uint8_t* vram, size_t vram_size,
                    uint32_t tile_word_addr, int bpp,
                    const uint8_t* cgram, int palette,
                    int dest_x, int dest_y);
    void drawPalette(const uint8_t* cgram, size_t cgram_size);
    void drawTileGrid(const uint8_t* vram, size_t vram_size,
                      const uint8_t* cgram, int bpp);
    void drawBGMap(const PPU& ppu, int bg_num);

    uint32_t snesColorToARGB(uint16_t c15);
    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawText(int x, int y, const char* text, uint32_t color);
    void putPixel(int x, int y, uint32_t color);
};

#endif
