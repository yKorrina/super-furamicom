#ifndef LAUNCHER_HPP
#define LAUNCHER_HPP

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL2/SDL.h>

struct InputBindingDefinition {
    const char* label;
    uint16_t mask;
    SDL_Scancode default_scancode;
};

constexpr std::size_t kInputBindingCount = 12;

struct InputConfig {
    std::array<SDL_Scancode, kInputBindingCount> scancodes{};
};

const std::array<InputBindingDefinition, kInputBindingCount>& GetInputBindingDefinitions();
InputConfig DefaultInputConfig();
bool LoadInputConfig(const std::string& path, InputConfig& config);
bool SaveInputConfig(const std::string& path, const InputConfig& config);
uint16_t MapInputConfigToPad(const uint8_t* keys, const InputConfig& config);
std::vector<std::string> DiscoverROMs(const std::string& root_dir);

struct LauncherResult {
    bool launch_requested = false;
    std::string rom_path;
    bool visualize = false;
    bool manipulator = false;
    bool integer_scaling = false;
    InputConfig input_config;
};

LauncherResult RunLauncher(SDL_Window* window,
                           SDL_Renderer* renderer,
                           SDL_Texture* texture,
                           const std::vector<std::string>& rom_paths,
                           const std::string& controls_path,
                           const InputConfig& initial_config,
                           bool visualize_default,
                           bool manipulator_default,
                           bool integer_scaling_default);

#endif
