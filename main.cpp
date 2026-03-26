#include "cpu.hpp"
#include "bus.hpp"
#include "cartridge.hpp"
#include <iostream>

class PPU {};
class APU {};

class System {
public:
    System() : bus(&cpu, &ppu, &apu), total_master_cycles(0) {}

    void loadROM(const std::string& filepath) {
        cartridge = new Cartridge(filepath);
        if (cartridge->isLoaded()) {
            bus.insertCartridge(cartridge);
            cpu.reset(); 
            std::cout << "ROM Loaded. Boot vector fetched. Executing...\n";
            std::cout << "------------------------------------------\n";
        } else {
            std::cerr << "Failed to load ROM.\n";
            exit(1);
        }
    }

void step() {
        cpu.printState(); // Debug output before execution
        uint8_t cpu_cycles = cpu.step();
        total_master_cycles += cpu_cycles * 6;
    }

    ~System() { delete cartridge; }

private:
    CPU cpu;
    PPU ppu; 
    APU apu; 
    Bus bus;
    Cartridge* cartridge = nullptr;
    uint64_t total_master_cycles;

    void catchUpPPU(uint32_t master_cycles) {}
    void catchUpAPU(uint32_t master_cycles) {}
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: ./snes_emu <rom_file.sfc>\n";
        return 1;
    }

    System snes;
    snes.loadROM(argv[1]);
    
    // Execute the first 20 instructions to verify the boot sequence
    for(int i = 0; i < 200; i++) {
        snes.step();
    }
    
    return 0;
}