#pragma once

#include "hle.hpp"

#include <filesystem>

namespace p2 {

void savedata_configure(const std::filesystem::path &ms_root);
// Save slot picker (PSP list load/save/delete dialogs), see savedata.cpp.
bool savedata_dialog_active();
void savedata_dialog_input(u32 buttons);
void savedata_dialog_draw(u32 *frame); // 480x272, 0xAABBGGRR
void register_savedata_imports();

} // namespace p2
