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

private:
    std::vector<uint8_t> vram;   
    std::vector<uint8_t> cgram;  
    std::vector<uint8_t> oam;    

    uint16_t vram_address;       
    uint8_t  vmain;              
    uint8_t  cgram_address;       
    bool     cgram_latch;
    uint8_t  cgram_buffer;

    // OAM address and write latch
    uint16_t oam_address;
    uint16_t oam_write_address;  // internal address that auto-increments
    bool     oam_latch;
    uint8_t  oam_buffer;

    uint8_t  bgmode;
    uint8_t  bg_sc[4];
    uint8_t  bg_nba[2];
    uint16_t bg_hofs[4];
    uint16_t bg_vofs[4];
    uint8_t  bg_scroll_latch;    
    uint8_t  tm_main;
    uint8_t  tm_sub;

    // Object (sprite) settings
    uint8_t  obsel;  // $2101 — OBJ size select & name base address

    // Screen settings
    uint8_t  inidisp;  // $2100 — forced blank & brightness

    // VRAM read prefetch
    uint16_t vram_prefetch;

    bool vblank_flag = false;

    std::array<uint32_t, 256 * 224> framebuffer;

    // Rendering helpers
    void renderBG(int bg_num, int bpp);
    void renderSprites();
    uint32_t colorFromCGRAM(uint16_t palette_offset);
};

#endif
