#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include <catch2/catch.hpp>
#include <memory>
#include <unknwn.h>

namespace {
constexpr GUID factory_x2{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID queue_x2{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};
struct QueueDescription {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
    UINT64 options;
};
struct QueueInfo {
    QueueDescription desc;
    UINT16 empty_slots, until_submit;
};

template <class Function> Function *slot(IUnknown *object, unsigned index) {
    return reinterpret_cast<Function *>((*reinterpret_cast<void ***>(object))[index]);
}
void release(IUnknown *object) { object->Release(); }
} // namespace

TEST_CASE("DirectStorage X2 queues honor the manual submit option") {
    xv::runtime::Registry registry;
    xv::runtime::modules::install_dstorage(registry);
    using GetFactory = HRESULT WINAPI(const GUID *, void **);
    const auto get_factory =
        reinterpret_cast<GetFactory *>(registry.find("xcrdapi.dll", "DStorageGetFactory"));
    REQUIRE(get_factory);

    void *result = nullptr;
    REQUIRE(get_factory(&factory_x2, &result) == S_OK);
    REQUIRE(result);
    const std::unique_ptr<IUnknown, decltype(&release)> factory(static_cast<IUnknown *>(result),
                                                                &release);
    const GUID unknown{};
    CHECK(get_factory(&unknown, &result) == E_NOINTERFACE);
    CHECK(result == nullptr);

    // Use the documented X2 methods at their console ABI slots.
    using CreateQueue1 = HRESULT WINAPI(IUnknown *, const QueueDescription *, REFIID, void **);
    const auto create_queue = slot<CreateQueue1>(factory.get(), 10);
    const QueueDescription manual{0, 128, 0, "manual", 1};
    REQUIRE(create_queue(factory.get(), &manual, queue_x2, &result) == S_OK);
    REQUIRE(result);
    const std::unique_ptr<IUnknown, decltype(&release)> queue(static_cast<IUnknown *>(result),
                                                              &release);
    const std::unique_ptr<void, decltype(&CloseHandle)> event(
        CreateEventW(nullptr, TRUE, FALSE, nullptr), &CloseHandle);
    REQUIRE(event);

    using EnqueueSetEvent = void WINAPI(IUnknown *, HANDLE);
    using Query1 = void WINAPI(IUnknown *, QueueInfo *);
    using Submit = void WINAPI(IUnknown *);
    const auto enqueue_event = slot<EnqueueSetEvent>(queue.get(), 12);
    const auto query = slot<Query1>(queue.get(), 13);
    for (int i = 0; i < 64; ++i)
        enqueue_event(queue.get(), event.get());
    CHECK(WaitForSingleObject(event.get(), 0) == WAIT_TIMEOUT);
    QueueInfo info{};
    query(queue.get(), &info);
    CHECK(info.desc.options == 1);
    CHECK(info.empty_slots == 63);
    CHECK(info.until_submit == 0);

    slot<Submit>(queue.get(), 6)(queue.get());
    CHECK(WaitForSingleObject(event.get(), 0) == WAIT_OBJECT_0);
    query(queue.get(), &info);
    CHECK(info.empty_slots == 127);
}
