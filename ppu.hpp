#ifndef PPU_HPP
#define PPU_HPP

#include <cstdint>
#include <vector>

class PPU {
public:
    PPU();

    uint8_t readRegister(uint16_t address);
    void writeRegister(uint16_t address, uint8_t data);
    void renderFrame();
    const uint32_t* getFramebuffer() const { return framebuffer; }

private:
    std::vector<uint8_t> vram; 
    std::vector<uint8_t> cgram; 
    std::vector<uint8_t> oam;   

    uint16_t vram_address;
    uint8_t cgram_address;
    uint16_t oam_address;
    
    uint8_t vmain; // Added VMAIN register (0x2115) to control VRAM writes
    bool cgram_latch;       
    uint8_t cgram_buffer;   

    uint32_t framebuffer[256 * 224]; 
};

#endif // PPU_HPP