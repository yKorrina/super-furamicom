#include "ppu.hpp"
#include <iostream>

PPU::PPU() {
    vram.resize(64 * 1024, 0);
    cgram.resize(512, 0);
    oam.resize(544, 0);
    vram_address = 0; cgram_address = 0; oam_address = 0;
    vmain = 0; cgram_latch = false; cgram_buffer = 0;
}

uint8_t PPU::readRegister(uint16_t address) { return 0x00; }

void PPU::writeRegister(uint16_t address, uint8_t data) {
    switch (address) {
        case 0x2115: vmain = data; break; // VMAIN configuration
        case 0x2116: vram_address = (vram_address & 0xFF00) | data; break; 
        case 0x2117: vram_address = (data << 8) | (vram_address & 0x00FF); break; 
        
        case 0x2118: 
            vram[vram_address * 2] = data; 
            // Increment if VMAIN bit 7 is 0 (increment on low byte write)
            if ((vmain & 0x80) == 0) vram_address += ((vmain & 0x03) == 1) ? 32 : 1;
            break; 
            
        case 0x2119: 
            vram[(vram_address * 2) + 1] = data; 
            // Increment if VMAIN bit 7 is 1 (increment on high byte write)
            if ((vmain & 0x80) != 0) vram_address += ((vmain & 0x03) == 1) ? 32 : 1;
            break; 
        
        case 0x2121: 
            cgram_address = data; 
            cgram_latch = false; 
            break; 
            
        case 0x2122: 
            if (!cgram_latch) {
                cgram_buffer = data; 
                cgram_latch = true;
            } else {
                cgram[cgram_address * 2] = cgram_buffer; 
                cgram[cgram_address * 2 + 1] = data;     
                cgram_address++; 
                cgram_latch = false;
            }
            break;
    }
}

void PPU::renderFrame() {
    uint32_t bg_color32 = (255 << 24) | (0 << 16) | (0 << 8) | 50; 
    for (int i = 0; i < 256 * 224; i++) framebuffer[i] = bg_color32;

    int tile_index = 0;
    for (int ty = 0; ty < 28; ty++) {       
        for (int tx = 0; tx < 32; tx++) {   
            uint32_t offset = tile_index * 32; 
            if (offset + 32 <= vram.size()) {
                for (int py = 0; py < 8; py++) { 
                    uint8_t bp1 = vram[offset + py * 2];
                    uint8_t bp2 = vram[offset + py * 2 + 1];
                    uint8_t bp3 = vram[offset + 16 + py * 2];
                    uint8_t bp4 = vram[offset + 16 + py * 2 + 1];

                    for (int px = 0; px < 8; px++) { 
                        uint8_t bit = 7 - px;
                        uint8_t color_idx = (((bp1 >> bit) & 1) << 0) |
                                            (((bp2 >> bit) & 1) << 1) |
                                            (((bp3 >> bit) & 1) << 2) |
                                            (((bp4 >> bit) & 1) << 3);

                        if (color_idx != 0) {
                            uint16_t c16 = cgram[color_idx * 2] | (cgram[color_idx * 2 + 1] << 8);
                            uint32_t c32;
                            if (c16 == 0) {
                                uint8_t intensity = (color_idx * 16) + 15; 
                                c32 = (255 << 24) | (intensity << 16) | (intensity << 8) | intensity;
                            } else {
                                uint8_t r = (c16 & 0x1F) << 3;
                                uint8_t g = ((c16 >> 5) & 0x1F) << 3;
                                uint8_t b = ((c16 >> 10) & 0x1F) << 3;
                                c32 = (255 << 24) | (r << 16) | (g << 8) | b;
                            }
                            framebuffer[(ty * 8 + py) * 256 + (tx * 8 + px)] = c32;
                        }
                    }
                }
            }
            tile_index++;
        }
    }
}