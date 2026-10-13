// Shim: decoder diagnostics go to stderr only at error level.
#pragma once
#include <cstdio>
namespace Log { enum { ME }; }
#define DEBUG_LOG(ch, ...) do { } while (0)
#define INFO_LOG(ch, ...) do { } while (0)
#define WARN_LOG(ch, ...) do { } while (0)
#define ERROR_LOG(ch, ...) do { std::fprintf(stderr, __VA_ARGS__); std::fputc('\n', stderr); } while (0)
