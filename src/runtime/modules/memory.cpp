#include "runtime/modules/memory.hpp"
#include "runtime/common/runtime.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>
#include <windows.h>

#include <psapi.h> // after windows.h

// Physical pages: a 16 GiB section mapped in 64 KiB views over placeholders.
namespace xv::runtime::modules {
namespace {
constexpr DWORD mem_large_pages = 0x20000000;
constexpr DWORD mem_4mb_pages = 0x80000000;
constexpr DWORD page_sizes = mem_large_pages | mem_4mb_pages;
constexpr std::uintptr_t page = 1 << 16;
constexpr std::uint64_t physical_size = 16ull << 30;
constexpr DWORD allocation_types =
    MEM_COMMIT | MEM_RESERVE | MEM_RESET | MEM_TOP_DOWN | MEM_WRITE_WATCH | MEM_RESET_UNDO;
constexpr DWORD placeholder_types = MEM_RESERVE_PLACEHOLDER | MEM_REPLACE_PLACEHOLDER;
constexpr DWORD protections = 0xff | PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE;

struct Mapping {
    ULONG_PTR page;
    bool owned; // taken by a plain commit
};
// large or 4 MiB pages: one placeholder per 64 KiB
struct Reservation {
    std::uintptr_t size;
    std::map<std::uintptr_t, Mapping> views;
};
struct Physical {
    std::mutex mutex;
    HANDLE section =
        CreateFileMapping2(INVALID_HANDLE_VALUE, nullptr, FILE_MAP_READ | FILE_MAP_WRITE,
                           PAGE_READWRITE, SEC_RESERVE, physical_size, nullptr, nullptr, 0);
    std::vector<bool> used = std::vector<bool>(physical_size / page),
                      dirty = std::vector<bool>(physical_size / page);
    std::map<std::uintptr_t, Reservation> reservations;
};
Physical &physical() {
    static Physical state;
    return state;
}
Reservation *reservation_at(Physical &state, std::uintptr_t address, std::uintptr_t *base) {
    auto entry = state.reservations.upper_bound(address);
    if (entry == state.reservations.begin())
        return nullptr;
    --entry;
    if (address - entry->first >= entry->second.size)
        return nullptr;
    *base = entry->first;
    return &entry->second;
}
bool take_pages(Physical &state, std::uintptr_t count, std::uintptr_t run, ULONG_PTR *pages) {
    std::uintptr_t taken = 0;
    for (std::uintptr_t i = 0; i + run <= state.used.size() && taken < count;) {
        std::uintptr_t free = 0;
        while (free < run && !state.used[i + free])
            ++free;
        if (free < run) {
            i += free + 1;
            continue;
        }
        for (std::uintptr_t j = 0; j < run; ++j) {
            state.used[i + j] = true;
            pages[taken++] = i + j;
        }
        i += run;
    }
    if (taken == count)
        return true;
    for (std::uintptr_t j = 0; j < taken; ++j)
        state.used[pages[j]] = false;
    return false;
}
void release_page(Physical &state, ULONG_PTR number) {
    state.used[number] = false;
    state.dirty[number] = true;
}
void unmap(Physical &state, Reservation &reservation, std::uintptr_t address) {
    const auto view = reservation.views.find(address);
    if (view == reservation.views.end())
        return;
    UnmapViewOfFile2(GetCurrentProcess(), reinterpret_cast<void *>(address),
                     MEM_PRESERVE_PLACEHOLDER);
    if (view->second.owned)
        release_page(state, view->second.page);
    reservation.views.erase(view);
}
bool map(Physical &state, Reservation &reservation, std::uintptr_t address, ULONG_PTR number,
         DWORD protect, bool owned) {
    unmap(state, reservation, address);
    auto *const at = reinterpret_cast<void *>(address);
    if (!MapViewOfFile3(state.section, nullptr, at, number * page, page, MEM_REPLACE_PLACEHOLDER,
                        PAGE_READWRITE, nullptr, 0) ||
        !VirtualAlloc(at, page, MEM_COMMIT, PAGE_READWRITE))
        return false;
    if (owned && state.dirty[number])
        std::memset(at, 0, page);
    state.dirty[number] = false;
    DWORD previous;
    if ((protect & protections) != PAGE_READWRITE)
        VirtualProtect(at, page, protect & protections, &previous);
    reservation.views[address] = {number, owned};
    return true;
}
void *reserve(void *address, std::uintptr_t size, DWORD type) {
    auto &state = physical();
    if (!state.section)
        return nullptr;
    if (!size || size > std::numeric_limits<std::uintptr_t>::max() - (page - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    const bool replacing = (type & MEM_REPLACE_PLACEHOLDER) != 0;
    if (replacing &&
        (!address || (reinterpret_cast<std::uintptr_t>(address) | size) & (page - 1))) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    size = (size + page - 1) & ~(page - 1);
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = type & mem_4mb_pages && !address ? 4u << 20 : 0;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    const std::lock_guard lock(state.mutex);
    if (replacing) {
        std::uintptr_t existing_base = 0;
        if (reservation_at(state, reinterpret_cast<std::uintptr_t>(address), &existing_base)) {
            SetLastError(ERROR_INVALID_ADDRESS);
            return nullptr;
        }
    }
    // A physical reservation replaces a guest placeholder with 64 KiB placeholder pieces so
    // MapViewOfFile3 can map physical pages into them later. Let Windows validate that the
    // supplied address and size describe one whole placeholder before splitting it.
    const DWORD flags =
        MEM_RESERVE | (replacing ? MEM_REPLACE_PLACEHOLDER : MEM_RESERVE_PLACEHOLDER);
    auto *const base = VirtualAlloc2(GetCurrentProcess(), address, size, flags, PAGE_NOACCESS,
                                     replacing ? nullptr : &parameter, replacing ? 0 : 1);
    if (!base)
        return nullptr;
    if (replacing && !VirtualFree(base, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
        const auto error = GetLastError();
        VirtualFree(base, 0, MEM_RELEASE);
        SetLastError(error);
        return nullptr;
    }
    const auto start = reinterpret_cast<std::uintptr_t>(base);
    for (std::uintptr_t offset = 0; offset + page < size; offset += page) {
        if (VirtualFree(reinterpret_cast<void *>(start + offset), page,
                        MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER))
            continue;
        const auto error = GetLastError();
        if (replacing && offset)
            VirtualFree(base, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS);
        if (!replacing)
            for (std::uintptr_t piece = 0; piece <= offset; piece += page)
                VirtualFree(reinterpret_cast<void *>(start + piece), 0, MEM_RELEASE);
        SetLastError(error);
        return nullptr;
    }
    state.reservations[start] = {size, {}};
    return base;
}
bool commit(std::uintptr_t address, std::uintptr_t size, DWORD protect) {
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    std::uintptr_t base = 0;
    auto *const reservation = reservation_at(state, address, &base);
    if (!reservation)
        return false;
    const auto end = std::min(address + size, base + reservation->size);
    for (auto at = address & ~(page - 1); at < end; at += page) {
        if (reservation->views.contains(at)) {
            DWORD previous;
            VirtualProtect(reinterpret_cast<void *>(at), page, protect & protections, &previous);
            continue;
        }
        ULONG_PTR number;
        if (!take_pages(state, 1, 1, &number) ||
            !map(state, *reservation, at, number, protect, true)) {
            SetLastError(ERROR_NOT_ENOUGH_MEMORY);
            return false;
        }
    }
    return true;
}

BOOL WINAPI virtual_free_ex(HANDLE process, void *address, SIZE_T size, DWORD type);

void *WINAPI virtual_alloc_ex(HANDLE process, void *address, SIZE_T size, DWORD type,
                              DWORD protect) {
    if (GetProcessId(process) == GetCurrentProcessId()) {
        if (type & MEM_RESERVE && type & page_sizes && !(type & MEM_RESERVE_PLACEHOLDER)) {
            auto *const base = reserve(address, size, type);
            if (base && type & MEM_COMMIT &&
                !commit(reinterpret_cast<std::uintptr_t>(base), size, protect)) {
                const auto error = GetLastError();
                const DWORD preserve =
                    type & MEM_REPLACE_PLACEHOLDER ? MEM_PRESERVE_PLACEHOLDER : 0;
                virtual_free_ex(process, base, preserve ? size : 0, MEM_RELEASE | preserve);
                SetLastError(error);
                return nullptr;
            }
            return base;
        }
        if (!(type & placeholder_types) && type & MEM_COMMIT &&
            commit(reinterpret_cast<std::uintptr_t>(address), size, protect))
            return address;
    }
    const auto result =
        type & placeholder_types
            ? VirtualAlloc2(process, address, size, type & (allocation_types | placeholder_types),
                            protect & protections, nullptr, 0)
            : VirtualAllocEx(process, address, size, type & allocation_types,
                             protect & protections);
    if (!result && (type & ~(allocation_types | page_sizes) || protect & ~protections)) {
        const auto error = GetLastError();
        std::fprintf(stderr,
                     "[Runtime] VirtualAlloc of %zu bytes, type 0x%lx protect 0x%lx failed: %lu\n",
                     static_cast<std::size_t>(size), type, protect, error);
        std::fflush(stderr);
        SetLastError(error);
    }
    return result;
}
void *WINAPI virtual_alloc(void *address, SIZE_T size, DWORD type, DWORD protect) {
    return virtual_alloc_ex(GetCurrentProcess(), address, size, type, protect);
}
BOOL WINAPI virtual_free_ex(HANDLE process, void *address, SIZE_T size, DWORD type) {
    if (GetProcessId(process) == GetCurrentProcessId()) {
        auto &state = physical();
        const std::lock_guard lock(state.mutex);
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        std::uintptr_t base = 0;
        if (auto *const reservation = reservation_at(state, at, &base)) {
            const bool release = type & MEM_RELEASE;
            const bool preserve = release && (type & MEM_PRESERVE_PLACEHOLDER);
            if (release && (at != base || type & MEM_COALESCE_PLACEHOLDERS ||
                            (preserve && size != reservation->size))) {
                SetLastError(ERROR_INVALID_PARAMETER);
                return FALSE;
            }
            const auto end = release || !size ? base + reservation->size : at + size;
            for (auto view = at & ~(page - 1); view < end; view += page)
                unmap(state, *reservation, view);
            if (release) {
                if (preserve) {
                    if (reservation->size > page &&
                        !VirtualFree(address, reservation->size,
                                     MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS))
                        return FALSE;
                } else {
                    for (auto piece = base; piece < end; piece += page)
                        VirtualFree(reinterpret_cast<void *>(piece), 0, MEM_RELEASE);
                }
                state.reservations.erase(base);
            }
            return TRUE;
        }
    }
    return VirtualFreeEx(process, address, size, type);
}
BOOL WINAPI virtual_free(void *address, SIZE_T size, DWORD type) {
    return virtual_free_ex(GetCurrentProcess(), address, size, type);
}
BOOL WINAPI virtual_protect(void *address, SIZE_T size, DWORD protect, DWORD *previous) {
    return VirtualProtect(address, size, protect & protections, previous);
}

BOOL WINAPI allocate_title_physical_pages(HANDLE, DWORD type, ULONG_PTR *count, ULONG_PTR *pages) {
    const auto sizes = type & page_sizes;
    if (!count || !pages || !sizes || sizes == page_sizes ||
        (type & mem_4mb_pages && *count & 63)) {
        if (count)
            *count = 0;
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    if (!state.section || !take_pages(state, *count, type & mem_4mb_pages ? 64 : 1, pages)) {
        *count = 0;
        SetLastError(ERROR_OUTOFMEMORY);
        return FALSE;
    }
    return TRUE;
}
BOOL WINAPI free_title_physical_pages(HANDLE, ULONG_PTR count, ULONG_PTR *pages) {
    if (count && !pages) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    auto &state = physical();
    const std::lock_guard lock(state.mutex);
    for (ULONG_PTR i = 0; i < count; ++i)
        if (pages[i] < state.used.size())
            release_page(state, pages[i]);
    return TRUE;
}
void *WINAPI map_title_physical_pages(void *address, ULONG_PTR count, DWORD type, DWORD protect,
                                      ULONG_PTR *pages) {
    const auto sizes = type & page_sizes;
    if (!pages || !sizes || sizes == page_sizes || (type & mem_4mb_pages && count & 63) ||
        reinterpret_cast<std::uintptr_t>(address) & (page - 1)) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return nullptr;
    }
    auto &state = physical();
    if (!address && !(address = reserve(nullptr, count * page, type)))
        return nullptr;
    const std::lock_guard lock(state.mutex);
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    std::uintptr_t base = 0;
    auto *const reservation = reservation_at(state, start, &base);
    if (!reservation || start + count * page > base + reservation->size) {
        SetLastError(ERROR_INVALID_ADDRESS);
        return nullptr;
    }
    for (ULONG_PTR i = 0; i < count; ++i)
        if (pages[i] >= state.used.size() ||
            !map(state, *reservation, start + i * page, pages[i], protect, false)) {
            SetLastError(ERROR_INVALID_PARAMETER);
            return nullptr;
        }
    return address;
}

// TOOLINGMEMORYSTATUS: none on retail
struct ToolingMemory {
    DWORD length, reserved;
    ULONGLONG total, available, peak, page_tables;
};
BOOL WINAPI tooling_memory_status(ToolingMemory *status) {
    if (!status || status->length < sizeof *status) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    *status = {sizeof *status, 0, 0, 0, 0, 0};
    return TRUE;
}

// a fresh 64K-aligned region satisfies any attributes
void *WINAPI xmem_alloc_default(SIZE_T size, ULONGLONG) {
    return VirtualAlloc(nullptr, size ? size : 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
}
void WINAPI xmem_free_default(void *address, ULONGLONG) {
    if (address)
        VirtualFree(address, 0, MEM_RELEASE);
}
using XMemAlloc = void *(WINAPI *)(SIZE_T, ULONGLONG);
using XMemFree = void(WINAPI *)(void *, ULONGLONG);
std::atomic<XMemAlloc> xmem_alloc_hook{&xmem_alloc_default};
std::atomic<XMemFree> xmem_free_hook{&xmem_free_default};
void *WINAPI xmem_alloc(SIZE_T size, ULONGLONG attributes) {
    return xmem_alloc_hook.load()(size, attributes);
}
void WINAPI xmem_free(void *address, ULONGLONG attributes) {
    xmem_free_hook.load()(address, attributes);
}
void WINAPI xmem_set_allocation_hooks(XMemAlloc alloc, XMemFree free) {
    xmem_alloc_hook = alloc ? alloc : &xmem_alloc_default;
    xmem_free_hook = alloc ? free : &xmem_free_default;
}
void WINAPI xmem_check_default_heaps() {}
BOOL WINAPI not_implemented() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return FALSE;
}
SIZE_T WINAPI xmem_get_allocation_hysteresis() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return static_cast<SIZE_T>(-1);
}
HANDLE WINAPI xmem_get_auxiliary_title_memory() {
    SetLastError(ERROR_CALL_NOT_IMPLEMENTED);
    return nullptr;
}
void WINAPI xmem_release_auxiliary_title_memory(HANDLE) {}

} // namespace

void *commit_title_memory(void *address, std::size_t size) {
    return virtual_alloc(address, size, MEM_COMMIT, PAGE_READWRITE);
}

void *allocate_aligned(std::size_t size, std::uint64_t alignment, void *address) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.Alignment = address || alignment <= page ? 0 : alignment;
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE, &parameter, 1);
}

namespace {
HRESULT WINAPI d3d_allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT,
                                            void **result) {
    if (!result || alignment > 0x20000)
        return E_INVALIDARG;
    *result = allocate_aligned(size, alignment, reinterpret_cast<void *>(address));
    return *result ? S_OK : E_OUTOFMEMORY;
}
HRESULT WINAPI d3d_free_graphics_memory(void *address) {
    return VirtualFree(address, 0, MEM_RELEASE) ? S_OK : E_INVALIDARG;
}

// xmem.dll, the GDK's names for the same memory. XMemVirtualAlloc is VirtualAlloc with
// attributes the host has no use for; the physical page functions take no process.
void *WINAPI xmem_virtual_alloc(void *address, SIZE_T size, DWORD type, ULONGLONG, DWORD protect) {
    return virtual_alloc(address, size, type, protect);
}
BOOL WINAPI xmem_allocate_physical_pages(ULONGLONG flags, ULONG_PTR *count, ULONG_PTR *pages) {
    const auto sizes = static_cast<DWORD>(flags) & page_sizes;
    return allocate_title_physical_pages(nullptr, sizes ? sizes : mem_large_pages, count, pages);
}
BOOL WINAPI xmem_free_physical_pages(ULONG_PTR count, ULONG_PTR *pages) {
    return free_title_physical_pages(nullptr, count, pages);
}
// the address comes back, which a title goes on to use; null is failure either way
void *WINAPI xmem_map_physical_pages(void *address, ULONG_PTR count, ULONG_PTR *pages) {
    return map_title_physical_pages(address, count, mem_large_pages, PAGE_READWRITE, pages);
}
// XMEM_WORKING_SET_STATISTICS: what a title may use, from what the host has
struct WorkingSet {
    SIZE_T game_limit, game_used, tools_limit, tools_used;
    UINT32 stack_used, kernel_pool_used;
    SIZE_T gpu_limit, gpu_used, tools_gpu_limit, tools_gpu_used;
};
BOOL WINAPI xmem_working_set_statistics(DWORD, WorkingSet *statistics) {
    if (!statistics) {
        SetLastError(ERROR_INVALID_PARAMETER);
        return FALSE;
    }
    MEMORYSTATUSEX host{sizeof host};
    PROCESS_MEMORY_COUNTERS_EX process{};
    process.cb = sizeof process;
    if (!GlobalMemoryStatusEx(&host) ||
        !K32GetProcessMemoryInfo(GetCurrentProcess(),
                                 reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&process),
                                 sizeof process))
        return FALSE;
    // What is used is what the title has taken, not what the rest of the host has: a title
    // sizes its pools and what it streams in by what is left of its limit.
    const SIZE_T limit = std::min<ULONGLONG>(13ull << 30, host.ullTotalPhys);
    const SIZE_T used = std::min<ULONGLONG>(process.PrivateUsage, limit);
    *statistics = {limit, used, 0, 0, 0, 0, limit, used, 0, 0};
    return TRUE;
}
BOOL WINAPI no() { return FALSE; }
// XMemTransferMemory moves memory between the title's and the system's budgets, which the
// host does not keep apart
BOOL WINAPI xmem_transfer_memory(UINT, SIZE_T) { return TRUE; }
ULONGLONG WINAPI xmem_make_tag(UINT32 tag) { return ULONGLONG{tag} << 32; }

template <class F> void *erase(F *function) { return reinterpret_cast<void *>(function); }
const ApiFunction functions[] = {
    {"VirtualAlloc", erase(virtual_alloc)},
    {"VirtualAllocEx", erase(virtual_alloc_ex)},
    {"VirtualFree", erase(virtual_free)},
    {"VirtualFreeEx", erase(virtual_free_ex)},
    {"VirtualProtect", erase(virtual_protect)},
    {"AllocateTitlePhysicalPages", erase(allocate_title_physical_pages)},
    {"FreeTitlePhysicalPages", erase(free_title_physical_pages)},
    {"MapTitlePhysicalPages", erase(map_title_physical_pages)},
    {"ToolingMemoryStatus", erase(tooling_memory_status)},
    {"XMemAllocDefault", erase(xmem_alloc_default)},
    {"XMemFreeDefault", erase(xmem_free_default)},
    {"XMemAlloc", erase(xmem_alloc)},
    {"XMemFree", erase(xmem_free)},
    {"XMemSetAllocationHooks", erase(xmem_set_allocation_hooks)},
    {"XMemCheckDefaultHeaps", erase(xmem_check_default_heaps)},
    {"XMemSetAllocationHysteresis", erase(not_implemented)},
    {"XMemGetAllocationHysteresis", erase(xmem_get_allocation_hysteresis)},
    {"XMemPreallocateFreeSpace", erase(not_implemented)},
    {"XMemGetAllocationStatistics", erase(not_implemented)},
    {"XMemGetAuxiliaryTitleMemory", erase(xmem_get_auxiliary_title_memory)},
    {"XMemReleaseAuxiliaryTitleMemory", erase(xmem_release_auxiliary_title_memory)}};
} // namespace

std::span<const ApiFunction> memory_functions() { return functions; }
HRESULT WINAPI allocate_graphics_memory(SIZE_T size, UINT64 alignment, UINT64 address, UINT flags,
                                        void **result) {
    return d3d_allocate_graphics_memory(size, alignment, address, flags, result);
}
HRESULT WINAPI free_graphics_memory(void *address) { return d3d_free_graphics_memory(address); }

namespace {
// XMemSetOption(option, value): tuning of the console's allocator, which is not this one
HRESULT WINAPI xmem_set_option(UINT, std::uint64_t) { return S_OK; }
} // namespace

void install_xmem(Registry &registry) {
    for (const auto &[name, function] : functions)
        if (std::string_view(name).starts_with("XMem") &&
            !std::string_view(name).ends_with("AuxiliaryTitleMemory") &&
            std::string_view(name) != "XMemCheckDefaultHeaps")
            registry.add("xmem.dll", name, function);
    const std::pair<const char *, void *> more[] = {
        {"XMemVirtualAlloc", erase(xmem_virtual_alloc)},
        {"XMemAllocatePhysicalPages", erase(xmem_allocate_physical_pages)},
        {"XMemFreePhysicalPages", erase(xmem_free_physical_pages)},
        {"XMemMapPhysicalPages", erase(xmem_map_physical_pages)},
        {"XMemGetWorkingSetStatistics", erase(xmem_working_set_statistics)},
        {"XMemGetWin32HeapStatistics", erase(not_implemented)},
        {"XMemIsToolsMemAvailable", erase(no)},
        {"XMemSetOption", erase(xmem_set_option)},
        {"XMemTransferMemory", erase(xmem_transfer_memory)},
        {"XMemMakeTag", erase(xmem_make_tag)}};
    for (const auto &[name, function] : more)
        registry.add("xmem.dll", name, function);
    // a GDK title asks the API sets for memory in the console's page sizes
    for (const auto *name :
         {"VirtualAlloc", "VirtualAllocEx", "VirtualFree", "VirtualFreeEx", "VirtualProtect"})
        for (const auto &[known, function] : functions)
            if (std::string_view(known) == name)
                registry.add_api(name, function);
}
} // namespace xv::runtime::modules
