#pragma once
#include "runtime/modules/kernel.hpp"
#include <cstddef>
#include <cstdint>
#include <span>
#include <windows.h>

// Console memory: reservations in the console's page sizes, and the title's physical pages,
// which are views of one section. ERA titles reach it through kernelx.dll, GDK titles
// through xmem.dll and the API sets.
namespace xv::runtime {
class Registry;
namespace modules {
// by the names kernelx.dll has for them
std::span<const ApiFunction> memory_functions();
// xmem.dll, and VirtualAlloc and its family for every API set
void install_xmem(Registry &registry);
void *allocate_aligned(std::size_t size, std::uint64_t alignment, void *address);
// VirtualAlloc(MEM_COMMIT) as a title's own call does it, physical page reservations included
void *commit_title_memory(void *address, std::size_t size);
struct XMemAllocationHooks {
    void *(WINAPI *allocate)(SIZE_T, ULONGLONG);
    void(WINAPI *release)(void *, ULONGLONG);
};
XMemAllocationHooks title_allocation_hooks();
// D3DAllocateGraphicsMemory and D3DFreeGraphicsMemory
HRESULT WINAPI allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT flags,
                                        void **result);
HRESULT WINAPI free_graphics_memory(void *address);
} // namespace modules
} // namespace xv::runtime
