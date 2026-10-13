#pragma once

#include "hle.hpp"

#include <filesystem>

namespace p2 {

void savedata_configure(const std::filesystem::path &ms_root);
void register_savedata_imports();

} // namespace p2
