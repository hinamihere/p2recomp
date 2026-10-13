// Histogram of instructions PSPRecomp's decoder marks Unsupported/Vfpu-generic.
#include "psprecomp/decoder.hpp"
#include "psprecomp/elf32.hpp"
#include "psprecomp/guest_memory.hpp"
#include <cstdio>
#include <map>
#include <string>

int main(int argc, char **argv) {
    auto elf = psprecomp::Elf32Image::from_file(argv[1]);
    psprecomp::GuestMemory mem;
    (void)elf.load_and_relocate(mem);
    std::uint32_t start = std::stoul(argv[2], nullptr, 0), end = std::stoul(argv[3], nullptr, 0);
    std::map<std::string, std::pair<unsigned, std::uint32_t>> hist;
    unsigned total = 0;
    for (std::uint32_t pc = start; pc < end; pc += 4) {
        auto w = mem.load32(pc);
        auto d = psprecomp::decode_allegrex(w);
        if (d.kind == psprecomp::OpcodeKind::Unsupported || d.kind == psprecomp::OpcodeKind::Vfpu) {
            auto &h = hist[d.mnemonic + (d.kind == psprecomp::OpcodeKind::Vfpu ? " [vfpu]" : "")];
            if (!h.first) h.second = pc;
            ++h.first; ++total;
        }
    }
    for (auto &[k, v] : hist) std::printf("%8u %-20s first=%08x\n", v.first, k.c_str(), v.second);
    std::printf("total %u\n", total);
}
