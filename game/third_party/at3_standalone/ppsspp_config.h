// Shim for the standalone decoder extracted from PPSSPP: only the arch macro is used.
#pragma once
#define PPSSPP_ARCH(x) PPSSPP_ARCH_##x
#if defined(__x86_64__) || defined(_M_X64)
#define PPSSPP_ARCH_AMD64 1
#define PPSSPP_ARCH_X86 0
#else
#define PPSSPP_ARCH_AMD64 0
#define PPSSPP_ARCH_X86 0
#endif
#define PPSSPP_ARCH_ARM_NEON 0
#define PPSSPP_ARCH_ARM64 0
