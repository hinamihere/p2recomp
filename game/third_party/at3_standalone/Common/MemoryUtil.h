// Shim: aligned allocation used by mem.cpp.
#pragma once
#include <cstdlib>
#include <malloc.h>
inline void *AllocateAlignedMemory(size_t size, size_t alignment) { return _aligned_malloc(size, alignment); }
inline void FreeAlignedMemory(void *ptr) { _aligned_free(ptr); }
