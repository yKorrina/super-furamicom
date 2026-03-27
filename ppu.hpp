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
    
    void setVBlank(bool v) { vblank_flag = v; }
    bool getVBlank() const { return vblank_flag; }

    const uint32_t* getFramebuffer() const { return framebuffer.data(); }

    // ── Accessors for debug / visualizer ──────────────────────────────────
    uint8_t  getINIDISP()      const { return inidisp; }
    uint8_t  getBGMode()       const { return bgmode; }
    uint8_t  getTMMain()       const { return tm_main; }
    uint8_t  getTMSub()        const { return tm_sub; }
    uint8_t  getTMW()          const { return tmw_main; }
    uint8_t  getTSW()          const { return tsw_sub; }
    uint8_t  getVMAIN()        const { return vmain; }
    uint8_t  getCGWSEL()       const { return cgwsel; }
    uint8_t  getCGADSUB()      const { return cgadsub; }
    uint16_t getFixedColor()   const { return fixed_color; }
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

    // Raw data access for visualizer
    const uint8_t* getVRAMData()  const { return vram.data(); }
    const uint8_t* getCGRAMData() const { return cgram.data(); }
    const uint8_t* getOAMData()   const { return oam.data(); }
    size_t getVRAMSize()  const { return vram.size(); }
    size_t getCGRAMSize() const { return cgram.size(); }
    size_t getOAMSize()   const { return oam.size(); }

    // BG register access for visualizer
    uint8_t  getBGSC(int n)   const { return (n >= 0 && n < 4) ? bg_sc[n] : 0; }
    uint8_t  getBGNBA(int n)  const { return (n >= 0 && n < 2) ? bg_nba[n] : 0; }
    uint16_t getBGHOFS(int n) const { return (n >= 0 && n < 4) ? bg_hofs[n] : 0; }
    uint16_t getBGVOFS(int n) const { return (n >= 0 && n < 4) ? bg_vofs[n] : 0; }

    // Debug counters
    size_t countNonZeroVRAM()    const;
    size_t countNonZeroCGRAM()   const;
    size_t countNonZeroOAM()     const;
    size_t countNonBlackPixels() const;

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
    uint32_t mpy_result;

    uint16_t ophct, opvct;
    bool     ophct_latch, opvct_latch;

    bool vblank_flag = false;

    std::array<uint32_t, 256 * 224> framebuffer;

    uint16_t translateVRAMAddress(uint16_t addr);
    bool layerWindowMasked(int layer_index, bool main_screen, int x) const;
    bool colorWindowActive(int x) const;
    bool colorWindowRegionEnabled(uint8_t region_mode, bool window_active) const;
    bool evaluateWindowMask(uint8_t select, uint8_t logic, int x) const;
    void renderBG(int bg_num, int bpp, bool high_priority, bool main_screen, uint32_t* target, uint8_t* source);
    void renderSprites(int priority, bool main_screen, uint32_t* target, uint8_t* source);
    uint32_t colorFromCGRAM(uint16_t palette_offset);
};

#endif
