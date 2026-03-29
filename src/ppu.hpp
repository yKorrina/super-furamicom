#ifndef PPU_HPP
#define PPU_HPP

#include <cstdint>
#include <vector>
#include <array>

class PPU {
public:
    PPU();

    uint8_t readRegister (uint16_t address);
    void    writeRegister(uint16_t address, uint8_t data);
    void    renderFrame  ();
    void    beginFrame   ();
    void    renderScanline(int scanline);
    void    endFrame     ();
    void    renderDebugFrame(uint8_t main_mask, uint8_t sub_mask, bool disable_color_math);

    void setVBlank(bool v) { vblank_flag = v; }
    bool getVBlank() const { return vblank_flag; }
    void setHBlank(bool v) { hblank_flag = v; }
    bool getHBlank() const { return hblank_flag; }

    const uint32_t* getFramebuffer() const { return framebuffer.data(); }
    uint32_t* getFramebufferMutable() { return framebuffer.data(); }

    uint8_t  getINIDISP()      const { return inidisp; }
    uint8_t  getBGMode()       const { return bgmode; }
    uint16_t getM7A()          const { return m7a; }
    uint16_t getM7B()          const { return m7b; }
    uint16_t getM7C()          const { return m7c; }
    uint16_t getM7D()          const { return m7d; }
    uint16_t getM7X()          const { return m7x; }
    uint16_t getM7Y()          const { return m7y; }
    uint16_t getM7HOFS()       const { return m7hofs; }
    uint16_t getM7VOFS()       const { return m7vofs; }
    uint8_t  getM7SEL()        const { return m7sel; }
    uint8_t  getTMMain()       const { return tm_main; }
    uint8_t  getTMSub()        const { return tm_sub; }
    uint8_t  getLiveMainLayerMask() const { return live_main_layer_mask; }
    uint8_t  getLiveSubLayerMask() const { return live_sub_layer_mask; }
    uint8_t  getEffectiveTMMain() const { return tm_main & live_main_layer_mask; }
    uint8_t  getEffectiveTMSub() const { return tm_sub & live_sub_layer_mask; }
    uint8_t  getTMW()          const { return tmw_main; }
    uint8_t  getTSW()          const { return tsw_sub; }
    uint8_t  getVMAIN()        const { return vmain; }
    uint8_t  getCGWSEL()       const { return cgwsel; }
    uint8_t  getCGADSUB()      const { return cgadsub; }
    uint16_t getFixedColor()   const { return fixed_color; }
    uint8_t  getSETINI()       const { return setini; }
    uint8_t  getW12SEL()       const { return w12sel; }
    uint8_t  getW34SEL()       const { return w34sel; }
    uint8_t  getWOBJSEL()      const { return wobjsel; }
    uint8_t  getWBGLOG()       const { return wbglog; }
    uint8_t  getWOBJLOG()      const { return wobjlog; }
    uint8_t  getWH0()          const { return wh0; }
    uint8_t  getWH1()          const { return wh1; }
    uint8_t  getWH2()          const { return wh2; }
    uint8_t  getWH3()          const { return wh3; }
    uint16_t getVRAMAddress()  const { return vram_address; }
    uint8_t  getCGRAMAddress() const { return cgram_address; }
    uint16_t getOAMAddress()   const { return oam_address; }

    const uint8_t* getVRAMData()  const { return vram.data(); }
    const uint8_t* getCGRAMData() const { return cgram.data(); }
    const uint8_t* getOAMData()   const { return oam.data(); }
    size_t getVRAMSize()  const { return vram.size(); }
    size_t getCGRAMSize() const { return cgram.size(); }
    size_t getOAMSize()   const { return oam.size(); }

    uint8_t  getBGSC(int n)   const { return (n >= 0 && n < 4) ? bg_sc[n] : 0; }
    uint8_t  getBGNBA(int n)  const { return (n >= 0 && n < 2) ? bg_nba[n] : 0; }
    uint16_t getBGHOFS(int n) const { return (n >= 0 && n < 4) ? bg_hofs[n] : 0; }
    uint16_t getBGVOFS(int n) const { return (n >= 0 && n < 4) ? bg_vofs[n] : 0; }

    size_t countNonZeroVRAM()    const;
    size_t countNonZeroCGRAM()   const;
    size_t countNonZeroOAM()     const;
    size_t countNonBlackPixels() const;
    const std::array<uint32_t, 7>& getFrameMainSourceCounts() const { return frame_main_source_counts; }
    const std::array<uint32_t, 7>& getFrameSubSourceCounts() const { return frame_sub_source_counts; }
    uint32_t getFrameColorMathPixels() const { return frame_color_math_pixels; }
    uint32_t getFrameBlackWindowPixels() const { return frame_black_window_pixels; }
    void setLiveLayerMasks(uint8_t main_mask, uint8_t sub_mask) {
        live_main_layer_mask = main_mask & 0x1F;
        live_sub_layer_mask = sub_mask & 0x1F;
    }
    void resetLiveLayerMasks() {
        live_main_layer_mask = 0x1F;
        live_sub_layer_mask = 0x1F;
    }

private:
    std::vector<uint8_t> vram;
    std::vector<uint8_t> cgram;
    std::vector<uint8_t> oam;

    uint16_t vram_address;
    uint8_t  vmain;
    uint8_t  cgram_address;
    bool     cgram_latch;
    uint8_t  cgram_buffer;

    uint16_t oam_address;
    uint16_t oam_write_address;
    bool     oam_latch;
    uint8_t  oam_buffer;

    uint8_t  bgmode;
    uint8_t  bg_sc[4];
    uint8_t  bg_nba[2];
    uint16_t bg_hofs[4];
    uint16_t bg_vofs[4];
    uint8_t  bg_scroll_latch;
    uint8_t  bg_hofs_latch;
    uint8_t  tm_main;
    uint8_t  tm_sub;
    uint8_t  tmw_main;
    uint8_t  tsw_sub;
    uint8_t  cgwsel;
    uint8_t  cgadsub;
    uint16_t fixed_color;
    uint8_t  setini;
    uint8_t  w12sel;
    uint8_t  w34sel;
    uint8_t  wobjsel;
    uint8_t  wbglog;
    uint8_t  wobjlog;
    uint8_t  wh0;
    uint8_t  wh1;
    uint8_t  wh2;
    uint8_t  wh3;

    uint8_t  obsel;
    uint8_t  inidisp;

    uint16_t vram_prefetch;

    uint16_t m7a, m7b, m7c, m7d;
    uint16_t m7x, m7y;
    uint16_t m7hofs, m7vofs;
    uint8_t  m7sel;
    uint8_t  m7byte;
    uint32_t mpy_result;

    uint16_t ophct, opvct;
    bool     ophct_latch, opvct_latch;
    uint16_t current_hcounter;
    uint16_t current_vcounter;

    bool vblank_flag = false;
    bool hblank_flag = false;
    uint8_t live_main_layer_mask = 0x1F;
    uint8_t live_sub_layer_mask = 0x1F;
    bool debug_override_masks = false;
    bool debug_disable_color_math = false;
    uint8_t debug_tm_main = 0;
    uint8_t debug_tm_sub = 0;
    std::array<uint32_t, 7> frame_main_source_counts{};
    std::array<uint32_t, 7> frame_sub_source_counts{};
    uint32_t frame_color_math_pixels = 0;
    uint32_t frame_black_window_pixels = 0;

    std::array<uint32_t, 256 * 224> framebuffer;

    uint16_t translateVRAMAddress(uint16_t addr);
    bool layerWindowMasked(int layer_index, bool main_screen, int x) const;
    bool colorWindowActive(int x) const;
    bool colorWindowRegionEnabled(uint8_t region_mode, bool window_active) const;
    bool evaluateWindowMask(uint8_t select, uint8_t logic, int x) const;
    bool cpuCanAccessVRAM() const;
    bool cpuCanAccessOAM() const;
    bool cpuCanAccessCGRAM() const;
    void renderMode7(int scanline, bool main_screen, uint16_t* target, uint8_t* source);
    void renderBG(int scanline, int bg_num, int bpp, bool high_priority, bool main_screen, uint16_t* target, uint8_t* source);
    void renderSprites(int scanline, int priority, bool main_screen, uint16_t* target, uint8_t* source);
    uint16_t colorFromCGRAM(uint16_t palette_offset) const;
    uint16_t directColorFromMode7(uint8_t color_byte) const;
    uint16_t directColorFromBG(uint8_t color_byte, uint8_t palette_bits) const;
};

#endif
