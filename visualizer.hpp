#ifndef VISUALIZER_HPP
#define VISUALIZER_HPP

#include <array>
#include <cstdint>

#include <SDL2/SDL.h>

class CPU;
class PPU;
class APU;

class Visualizer {
public:
    Visualizer();
    ~Visualizer();

    bool init();
    void show();
    void placeBeside(SDL_Window* anchor, int gap = 24);
    void update(const CPU& cpu, const PPU& ppu, const APU& apu);
    void destroy();
    bool isOpen() const { return window != nullptr; }
    bool processEvent(const SDL_Event& e);
    uint32_t getWindowID() const;

private:
    static constexpr int WIN_W = 960;
    static constexpr int WIN_H = 720;
    static constexpr int SPECTROGRAM_W = 320;
    static constexpr int SPECTROGRAM_H = 180;

    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    SDL_Texture* texture = nullptr;

    std::array<uint32_t, WIN_W * WIN_H> pixels{};
    std::array<uint32_t, SPECTROGRAM_W * SPECTROGRAM_H> spectrogram{};
    int active_panel = 2;

    uint64_t last_audio_frame = 0;

    void clear(uint32_t color);
    void drawBackground();
    void drawTabs();
    void drawPanel(int x, int y, int w, int h, uint32_t fill, uint32_t border);
    void drawPanelTitle(int x, int y, const char* text, uint32_t color);
    void drawPPUView(const PPU& ppu);
    void drawCPUView(const CPU& cpu);
    void drawAPUView(const APU& apu);
    void drawFramePreview(const PPU& ppu, int x, int y, int scale);
    void drawPaletteStrip(const PPU& ppu, int x, int y, int swatch, int cols);
    void drawCPUHistoryPlot(const CPU& cpu, int x, int y, int w, int h);
    void drawWaveform(const APU& apu, int x, int y, int w, int h);
    void updateSpectrogram(const APU& apu);
    void drawSpectrogram(int x, int y, int w, int h);
    void drawVoiceMeters(const APU& apu, int x, int y, int w, int h);

    static uint32_t snesColorToARGB(uint16_t c15);
    static uint32_t lerpColor(uint32_t a, uint32_t b, float t);
    static uint32_t heatColor(float t);
    static float clamp01(float value);

    void drawRect(int x, int y, int w, int h, uint32_t color);
    void drawRectOutline(int x, int y, int w, int h, uint32_t color);
    void drawText(int x, int y, const char* text, uint32_t color);
    void putPixel(int x, int y, uint32_t color);
};

#endif
