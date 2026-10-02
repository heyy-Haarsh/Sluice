#include <cstdlib>
#include <iostream>
#include <string>
#include <filesystem>

#include "config.hpp"

int main(int argc, char** argv) {
    std::string config_path;
    config_path = DEFAULT_CONFIG_PATH;
    if (!std::filesystem::exists(config_path)) {
        create_default_config();
        if (!std::filesystem::exists(config_path)) {
            std::cerr << "Couldn't make default config file!\n";
            return -1;
        }
    }
    return 0;
}
