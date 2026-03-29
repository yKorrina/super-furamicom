#ifndef MANIPULATOR_HPP
#define MANIPULATOR_HPP

#include <array>
#include <cstdint>

#include <SDL2/SDL.h>

class APU;
class PPU;

class Manipulator {
public:
    Manipulator();
    ~Manipulator();

    bool init();
    void show();
    void placeBeside(SDL_Window* anchor, int gap = 24);
    void update(APU& apu, PPU& ppu);
    void destroy();
    bool isOpen() const { return window != nullptr; }
    bool processEvent(const SDL_Event& e);
    uint32_t getWindowID() const;
    bool isFullscreen() const { return fullscreen; }
    SDL_Window* getWindowHandle() const { return window; }

private:
    static constexpr int WIN_W = 640;
    static constexpr int WIN_H = 480;
    static constexpr int MIN_W = 480;
    static constexpr int MIN_H = 320;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;
    bool fullscreen = false;
    SDL_Rect windowed_bounds{SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, WIN_W, WIN_H};

    std::array<uint32_t, WIN_W * WIN_H> pixels{};
    uint8_t desired_voice_mask = 0xFF;
    uint8_t desired_main_layer_mask = 0x1F;
    uint8_t desired_sub_layer_mask = 0x1F;
    bool synced_masks = false;

    void clear(uint32_t color);
    void drawBackground();
    void drawPanel(int x, int y, int w, int h, uint32_t fill, uint32_t border);
    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawRectOutline(int x, int y, int w, int h, uint32_t color);
    void drawText(int x, int y, const char* text, uint32_t color);
    void drawHeader();
    void drawVoicePanel(const APU& apu);
    void drawLayerPanel(const PPU& ppu);
    void drawStatusPanel(const APU& apu, const PPU& ppu);
    void drawCheckbox(const SDL_Rect& rect, bool checked, bool highlighted);
    void syncMasksFromCore(const APU& apu, const PPU& ppu);
    void applyMasks(APU& apu, PPU& ppu);
    void refreshScaleMode();
    void setFullscreen(bool enable);
    SDL_Point toLogicalPoint(int window_x, int window_y) const;
    void handleClick(int x, int y);
};

#endif
