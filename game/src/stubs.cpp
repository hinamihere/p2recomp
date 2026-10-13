// Placeholder HLE for libraries that are not implemented yet. Each stub logs
// its first call so missing behaviour is visible while bringing the game up.
#include "audio.hpp"
#include "kernel.hpp"

#include <set>
#include <string>

namespace p2 {
namespace {

void note(const char *name, AllegrexContext &c) {
    static std::set<std::string> seen;
    if (seen.insert(name).second)
        LOG_W("stub", "%s(%08x, %08x, %08x, %08x) first call", name, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
}

#define STUB(fn, value)                       \
    void fn(AllegrexContext &c) {             \
        note(#fn, c);                         \
        ret(c, value);                        \
    }

// --- sceMpeg ---
STUB(MpegInit, 0u)
STUB(MpegFinish, 0u)
STUB(MpegQueryMemSize, 0x10000u)
STUB(MpegRingbufferQueryMemSize, 0x10000u)
STUB(MpegRingbufferConstruct, 0u)
STUB(MpegRingbufferDestruct, 0u)
STUB(MpegRingbufferPut, 0u)
STUB(MpegRingbufferAvailableSize, 0u)
STUB(MpegCreate, 0u)
STUB(MpegDelete, 0u)
STUB(MpegQueryStreamOffset, 0x80618001u)
STUB(MpegQueryStreamSize, 0x80618001u)
STUB(MpegRegistStream, 0u)
STUB(MpegUnRegistStream, 0u)
STUB(MpegFlushAllStream, 0u)
STUB(MpegMallocAvcEsBuf, 1u)
STUB(MpegFreeAvcEsBuf, 0u)
STUB(MpegInitAu, 0u)
STUB(MpegQueryAtracEsSize, 0u)
STUB(MpegGetAvcAu, 0x80618001u)
STUB(MpegGetAtracAu, 0x80618001u)
STUB(MpegAtracDecode, 0u)
STUB(MpegAvcDecodeYCbCr, 0x80618001u)
STUB(MpegAvcDecodeStopYCbCr, 0u)
STUB(MpegAvcDecodeFlush, 0u)
STUB(MpegAvcQueryYCbCrSize, 0u)
STUB(MpegAvcInitYCbCr, 0u)
STUB(MpegAvcCsc, 0u)

// --- sceUtility dialogs: complete immediately ---
struct Dialog {
    u32 status{};
    u32 param{};
};
Dialog g_msg, g_shot, g_install;

void dialog_init(Dialog &d, AllegrexContext &c, const char *name) {
    note(name, c);
    d.param = arg(c, 0);
    d.status = 1u;
    ret(c, 0u);
}
void dialog_status(Dialog &d, AllegrexContext &c) {
    const u32 s = d.status;
    if (d.status == 1u) d.status = 2u;
    else if (d.status == 2u) d.status = 3u;
    else if (d.status == 4u) d.status = 0u;
    ret(c, s);
}
void dialog_shutdown(Dialog &d, AllegrexContext &c) { d.status = 4u; ret(c, 0u); }

void MsgDialogInitStart(AllegrexContext &c) {
    dialog_init(g_msg, c, "sceUtilityMsgDialogInitStart");
    const u32 p = g_msg.param;
    LOG_I("utility", "msg dialog: %s", guest_string(p + 0x3Cu, 512).c_str());
    mem().store32(p + 0x1Cu, 0u);   // result
    mem().store32(p + 0x240u, 1u);  // button pressed: yes
}
void MsgDialogGetStatus(AllegrexContext &c) { dialog_status(g_msg, c); }
void MsgDialogUpdate(AllegrexContext &c) { ret(c, 0u); }
void MsgDialogShutdownStart(AllegrexContext &c) { dialog_shutdown(g_msg, c); }

void ScreenshotInitStart(AllegrexContext &c) { dialog_init(g_shot, c, "sceUtilityScreenshotInitStart"); }
void ScreenshotGetStatus(AllegrexContext &c) { dialog_status(g_shot, c); }
void ScreenshotUpdate(AllegrexContext &c) { ret(c, 0u); }
void ScreenshotShutdownStart(AllegrexContext &c) { dialog_shutdown(g_shot, c); }

void GamedataInstallInitStart(AllegrexContext &c) {
    dialog_init(g_install, c, "sceUtilityGamedataInstallInitStart");
    mem().store32(g_install.param + 0x1Cu, 0x80020001u);
}
void GamedataInstallGetStatus(AllegrexContext &c) { dialog_status(g_install, c); }
void GamedataInstallUpdate(AllegrexContext &c) { ret(c, 0u); }
void GamedataInstallShutdownStart(AllegrexContext &c) { dialog_shutdown(g_install, c); }
void GamedataInstallAbort(AllegrexContext &c) { g_install.status = 3u; ret(c, 0u); }

STUB(UtilityLoadModule, 0u)
STUB(UtilityUnloadModule, 0u)

} // namespace

void register_stub_imports() {
    const char *M = "sceMpeg";
    hle(M, "sceMpegInit", MpegInit);
    hle(M, "sceMpegFinish", MpegFinish);
    hle(M, "sceMpegQueryMemSize", MpegQueryMemSize);
    hle(M, "sceMpegRingbufferQueryMemSize", MpegRingbufferQueryMemSize);
    hle(M, "sceMpegRingbufferConstruct", MpegRingbufferConstruct);
    hle(M, "sceMpegRingbufferDestruct", MpegRingbufferDestruct);
    hle(M, "sceMpegRingbufferPut", MpegRingbufferPut);
    hle(M, "sceMpegRingbufferAvailableSize", MpegRingbufferAvailableSize);
    hle(M, "sceMpegCreate", MpegCreate);
    hle(M, "sceMpegDelete", MpegDelete);
    hle(M, "sceMpegQueryStreamOffset", MpegQueryStreamOffset);
    hle(M, "sceMpegQueryStreamSize", MpegQueryStreamSize);
    hle(M, "sceMpegRegistStream", MpegRegistStream);
    hle(M, "sceMpegUnRegistStream", MpegUnRegistStream);
    hle(M, "sceMpegFlushAllStream", MpegFlushAllStream);
    hle(M, "sceMpegMallocAvcEsBuf", MpegMallocAvcEsBuf);
    hle(M, "sceMpegFreeAvcEsBuf", MpegFreeAvcEsBuf);
    hle(M, "sceMpegInitAu", MpegInitAu);
    hle(M, "sceMpegQueryAtracEsSize", MpegQueryAtracEsSize);
    hle(M, "sceMpegGetAvcAu", MpegGetAvcAu);
    hle(M, "sceMpegGetAtracAu", MpegGetAtracAu);
    hle(M, "sceMpegAtracDecode", MpegAtracDecode);
    hle(M, "sceMpegAvcDecodeYCbCr", MpegAvcDecodeYCbCr);
    hle(M, "sceMpegAvcDecodeStopYCbCr", MpegAvcDecodeStopYCbCr);
    hle(M, "sceMpegAvcDecodeFlush", MpegAvcDecodeFlush);
    hle(M, "sceMpegAvcQueryYCbCrSize", MpegAvcQueryYCbCrSize);
    hle(M, "sceMpegAvcInitYCbCr", MpegAvcInitYCbCr);
    hle(M, "sceMpegAvcCsc", MpegAvcCsc);

    const char *U = "sceUtility";
    hle(U, "sceUtilityMsgDialogInitStart", MsgDialogInitStart);
    hle(U, "sceUtilityMsgDialogGetStatus", MsgDialogGetStatus);
    hle(U, "sceUtilityMsgDialogUpdate", MsgDialogUpdate);
    hle(U, "sceUtilityMsgDialogShutdownStart", MsgDialogShutdownStart);
    hle(U, "sceUtilityScreenshotInitStart", ScreenshotInitStart);
    hle(U, "sceUtilityScreenshotGetStatus", ScreenshotGetStatus);
    hle(U, "sceUtilityScreenshotUpdate", ScreenshotUpdate);
    hle(U, "sceUtilityScreenshotShutdownStart", ScreenshotShutdownStart);
    hle(U, "sceUtilityGamedataInstallInitStart", GamedataInstallInitStart);
    hle(U, "sceUtilityGamedataInstallGetStatus", GamedataInstallGetStatus);
    hle(U, "sceUtilityGamedataInstallUpdate", GamedataInstallUpdate);
    hle(U, "sceUtilityGamedataInstallShutdownStart", GamedataInstallShutdownStart);
    hle(U, "sceUtilityGamedataInstallAbort", GamedataInstallAbort);
    hle(U, "sceUtilityLoadModule", UtilityLoadModule);
    hle(U, "sceUtilityUnloadModule", UtilityUnloadModule);
}

} // namespace p2
