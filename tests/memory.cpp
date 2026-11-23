#include "runtime/modules/memory.hpp"
#include "runtime/common/runtime.hpp"
#include <array>
#include <catch2/catch.hpp>
#include <memory>

TEST_CASE("a split Xbox placeholder can hold physical pages and be restored") {
    xv::runtime::Registry registry;
    xv::runtime::modules::install_xmem(registry);
    using Allocate = void *WINAPI(void *, SIZE_T, DWORD, ULONGLONG, DWORD);
    using Free = BOOL WINAPI(void *, SIZE_T, DWORD);
    using TakePages = BOOL WINAPI(ULONGLONG, ULONG_PTR *, ULONG_PTR *);
    using FreePages = BOOL WINAPI(ULONG_PTR, ULONG_PTR *);
    using MapPages = void *WINAPI(void *, ULONG_PTR, ULONG_PTR *);
    const auto allocate =
        reinterpret_cast<Allocate *>(registry.find("xmem.dll", "XMemVirtualAlloc"));
    const auto release = reinterpret_cast<Free *>(registry.find_api("VirtualFree"));
    const auto take_pages =
        reinterpret_cast<TakePages *>(registry.find("xmem.dll", "XMemAllocatePhysicalPages"));
    const auto free_pages =
        reinterpret_cast<FreePages *>(registry.find("xmem.dll", "XMemFreePhysicalPages"));
    const auto map_pages =
        reinterpret_cast<MapPages *>(registry.find("xmem.dll", "XMemMapPhysicalPages"));
    REQUIRE(allocate);
    REQUIRE(release);
    REQUIRE(take_pages);
    REQUIRE(free_pages);
    REQUIRE(map_pages);

    constexpr SIZE_T page = 64 * 1024;
    constexpr DWORD xbox_64k_pages = 0x20400000;
    constexpr DWORD reserve_physical = MEM_RESERVE | MEM_REPLACE_PLACEHOLDER | xbox_64k_pages;
    std::array<ULONG_PTR, 2> pages{};
    ULONG_PTR count = pages.size();
    REQUIRE(take_pages(0, &count, pages.data()));
    const auto free_physical = [free_pages, count](ULONG_PTR *at) { free_pages(count, at); };
    const std::unique_ptr<ULONG_PTR, decltype(free_physical)> physical(pages.data(), free_physical);
    REQUIRE(count == pages.size());

    const auto free_region = [release](std::byte *at) { release(at, 0, MEM_RELEASE); };
    const auto placeholder =
        allocate(nullptr, 3 * page, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER, 0, PAGE_NOACCESS);
    std::unique_ptr<std::byte, decltype(free_region)> first(static_cast<std::byte *>(placeholder),
                                                            free_region);
    REQUIRE(first);
    REQUIRE(release(first.get(), 2 * page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    std::unique_ptr<std::byte, decltype(free_region)> last(first.get() + 2 * page, free_region);
    CHECK(allocate(first.get(), page, reserve_physical, 0, PAGE_READWRITE) == nullptr);
    REQUIRE(allocate(first.get(), 2 * page, reserve_physical, 0, PAGE_READWRITE) == first.get());
    REQUIRE(map_pages(first.get(), count, pages.data()) == first.get());
    first.get()[page] = std::byte{0x42};
    CHECK(first.get()[page] == std::byte{0x42});

    REQUIRE(release(first.get(), 2 * page, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER));
    REQUIRE(release(first.get(), 3 * page, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS));
    last.release(); // The coalesced reservation is now released as a whole by first.
}
