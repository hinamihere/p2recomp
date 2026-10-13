// Persona 2: Innocent Sin (PSP, ULES-01557) — native host for the statically
// recompiled EBOOT.
#include "atrac.hpp"
#include "audio.hpp"
#include "display.hpp"
#include "ge.hpp"
#include "hle.hpp"
#include "host.hpp"
#include "io.hpp"
#include "kernel.hpp"
#include "sas.hpp"

#include "psprecomp/common.hpp"
#include "psprecomp/elf32.hpp"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <memory>

namespace psprecomp {
void register_generated_functions(Runtime &runtime);
}

namespace p2 {

namespace {
std::unique_ptr<Runtime> g_runtime;

void on_post_dispatch(Runtime &, AllegrexContext &c, std::uint32_t, std::int32_t) { g_kernel.on_dispatch(c); }
void on_starvation(Runtime &, AllegrexContext &c) { g_kernel.process_events(c); }
void on_break(Runtime &r, AllegrexContext &c, std::uint32_t pc, std::uint32_t code) {
    LOG_E("cpu", "guest break %u at %08x (ra=%08x)", code, pc, c.gpr[31]);
    r.stop("guest break");
}

struct Options {
    std::filesystem::path eboot{"bin/EBOOT.dec"};
    std::filesystem::path disc{"extracted"};
    std::filesystem::path iso;
    std::filesystem::path ms{"memstick"};
    int scale{2};
    bool headless{};
    u32 dump_every{};
};

Options parse_options(int argc, char **argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : std::string(); };
        if (a == "--eboot") o.eboot = next();
        else if (a == "--disc") o.disc = next();
        else if (a == "--iso") o.iso = next();
        else if (a == "--ms") o.ms = next();
        else if (a == "--scale") o.scale = std::atoi(next().c_str());
        else if (a == "--headless") o.headless = true;
        else if (a == "--dump-frames") o.dump_every = static_cast<u32>(std::atoi(next().c_str()));
        else if (a == "-v") g_log_level = LogLevel::Debug;
        else if (a == "-vv") g_log_level = LogLevel::Trace;
        else if (a == "-q") g_log_level = LogLevel::Warn;
    }
    if (o.iso.empty()) {
        // Default: the first .iso next to the extracted tree's parent.
        std::error_code ec;
        for (const auto &entry : std::filesystem::recursive_directory_iterator(".", ec)) {
            if (entry.path().extension() == ".iso") { o.iso = entry.path(); break; }
        }
    }
    return o;
}
} // namespace

Runtime &rt() { return *g_runtime; }

int run(int argc, char **argv) {
    const Options opt = parse_options(argc, argv);
    g_runtime = std::make_unique<Runtime>(32u * 1024u * 1024u);
    Runtime &r = *g_runtime;

    const auto elf = psprecomp::Elf32Image::from_file(opt.eboot);
    constexpr u32 kLoadBase = psprecomp::kDefaultPspUserLoadBase;
    const auto relocs = elf.load_and_relocate(r.memory(), kLoadBase);
    const auto module = elf.find_module_info(r.memory(), kLoadBase);
    if (!module) {
        LOG_E("boot", "no module info in %s", opt.eboot.string().c_str());
        return 1;
    }
    u32 module_end = kLoadBase;
    for (std::size_t i = 0; i < elf.segments().size(); ++i) {
        const auto &seg = elf.segments()[i];
        if (seg.type == 1u) module_end = std::max(module_end, elf.segment_runtime_address(i, kLoadBase) + seg.memory_size);
    }
    LOG_I("boot", "module '%s' gp=%08x end=%08x entry=%08x relocs=%llu", module->name.c_str(), module->gp,
          module_end, elf.runtime_entry(kLoadBase), static_cast<unsigned long long>(relocs.total));

    psprecomp::register_generated_functions(r);
    r.register_function(kThreadReturnAddr, [](Runtime &, AllegrexContext &c) { g_kernel.thread_return(c); }, "hle_thread_return");
    r.register_function(kNestedReturnAddr, [](Runtime &, AllegrexContext &c) { g_kernel.nested_return(c); }, "hle_nested_return");
    r.register_function(kIdleAddr, [](Runtime &, AllegrexContext &c) { g_kernel.idle(c); }, "hle_idle");

    io_configure({opt.disc, opt.iso, opt.ms});
    register_kernel_imports();
    register_io_imports();
    register_display_imports();
    display_set_frame_dump(opt.dump_every);
    register_ge_imports();
    register_audio_imports();
    register_stub_imports();
    register_sas_imports();
    register_atrac_imports();
    report_unimplemented_imports();

    psprecomp::set_runtime_post_dispatch_hook(on_post_dispatch);
    psprecomp::set_runtime_starvation_hook(on_starvation, 4096u);
    r.set_break_handler(on_break);

    if (!opt.headless) {
        if (!host::init("Persona 2: Innocent Sin", opt.scale)) return 1;
        audio_start();
    }

    g_kernel.init(module->gp, module_end);
    g_kernel.boot_main_thread(elf.runtime_entry(kLoadBase), 0x20u, 0x4000u, 0x80000000u,
                              "disc0:/PSP_GAME/SYSDIR/EBOOT.BIN");

    int status = 0;
    try {
        r.run(r.cpu().pc, ~0ull);
        LOG_I("boot", "stopped: %s", r.stop_reason().c_str());
    } catch (const std::exception &e) {
        LOG_E("boot", "fatal: %s", e.what());
        status = 2;
    }
    if (r.stop_reason() != "window closed" && r.stop_reason() != "sceKernelExitGame") {
        LOG_E("boot", "thread state at stop (current uid=%x):", g_kernel.current_uid());
        g_kernel.dump_threads();
        if (status == 0) status = 3;
    }
    if (!opt.headless) host::shutdown();
    return status;
}

} // namespace p2

int main(int argc, char **argv) { return p2::run(argc, argv); }
