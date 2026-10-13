#pragma once

#include "hle.hpp"

#include <filesystem>

namespace p2 {

struct IoConfig {
    std::filesystem::path disc_root; // extracted ISO tree (contains PSP_GAME/)
    std::filesystem::path iso_path;  // raw ISO, for sce_lbn paths
    std::filesystem::path ms_root;   // memory stick root
};

void io_configure(const IoConfig &config);
void register_io_imports();

} // namespace p2
