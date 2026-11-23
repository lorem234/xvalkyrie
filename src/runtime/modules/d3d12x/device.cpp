#include "backends/graphics/backend.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/console_shader.hpp"
#include "runtime/modules/d3d12x.hpp"
#include "runtime/modules/d3d12x/internal.hpp"
#include "runtime/modules/placement.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <d3d12.h>
#include <deque>
#include <dxgi1_6.h>
#include <intrin.h>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <wrl/implements.h>

// d3d12_x.dll and d3d12_xs.dll on the host's Direct3D 12. Both consoles have the same
// interfaces up to the methods each added last; `console` says which the title was built for.
namespace xv::runtime::modules::d3d12x {
namespace {
Target console = Target::durango;
AdapterWrapper adapter_wrapper = nullptr;
template <class... Interfaces> bool is_any(REFIID iid) {
    return ((iid == __uuidof(Interfaces)) || ...);
}
// The title's first direct queue: the console presents for the device, the host for a queue
std::atomic<ID3D12CommandQueue *> first_direct_queue{nullptr};
// d3d12_x.dll: root signatures are the host's format as well; the device is not there yet
template <class Function> Function host_d3d12(const char *name) {
    static const auto library = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return library ? reinterpret_cast<Function>(GetProcAddress(library, name)) : nullptr;
}
HRESULT WINAPI serialize_root_signature(const void *desc, UINT version, void **blob,
                                        void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, UINT, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeRootSignature");
    return serialize ? serialize(desc, version, blob, errors) : E_NOTIMPL;
}
HRESULT WINAPI serialize_versioned_root_signature(const void *desc, void **blob, void **errors) {
    using Serialize = HRESULT(WINAPI *)(const void *, void **, void **);
    static const auto serialize = host_d3d12<Serialize>("D3D12SerializeVersionedRootSignature");
    return serialize ? serialize(desc, blob, errors) : E_NOTIMPL;
}
HRESULT WINAPI root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                           void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI versioned_root_signature_deserializer(const void *blob, SIZE_T size, REFIID iid,
                                                     void **reader) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    return create ? create(blob, size, iid, reader) : E_NOTIMPL;
}
HRESULT WINAPI create_device_12(void *adapter, UINT level, REFIID iid, void **device) {
    if (device)
        *device = nullptr;
    std::fprintf(stderr, "[Runtime] D3D12CreateDevice(%p, 0x%x): Direct3D 12.X is not there yet\n",
                 adapter, level);
    std::fflush(stderr);
    return E_NOTIMPL;
}
// The Series console's IDXGIOutput goes on past the desktop's methods with three of its own,
// and a title asks it for the display's modes. The host's output answers the rest.
struct ConsoleMode {
    UINT width, height;
    DXGI_RATIONAL refresh;
    DXGI_FORMAT format;
    DXGI_MODE_SCANLINE_ORDER scanline;
    DXGI_MODE_SCALING scaling;
    UINT flags;
};
// (No virtual destructor: it would take the slot right after the desktop's methods.)
class ConsoleOutput final : public IDXGIOutput {
  public:
    explicit ConsoleOutput(IDXGIOutput *host) : host_(host) {}
    ~ConsoleOutput() { host_->Release(); }
    STDMETHODIMP QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (!is_any<IUnknown, IDXGIObject, IDXGIOutput>(iid)) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return ++references_; }
    STDMETHODIMP_(ULONG) Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }
    STDMETHODIMP SetPrivateData(REFGUID id, UINT size, const void *data) override {
        return host_->SetPrivateData(id, size, data);
    }
    STDMETHODIMP SetPrivateDataInterface(REFGUID id, const IUnknown *object) override {
        return host_->SetPrivateDataInterface(id, object);
    }
    STDMETHODIMP GetPrivateData(REFGUID id, UINT *size, void *data) override {
        return host_->GetPrivateData(id, size, data);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        return host_->GetParent(iid, parent);
    }
    STDMETHODIMP GetDesc(DXGI_OUTPUT_DESC *desc) override { return host_->GetDesc(desc); }
    STDMETHODIMP GetDisplayModeList(DXGI_FORMAT format, UINT flags, UINT *count,
                                    DXGI_MODE_DESC *modes) override {
        return host_->GetDisplayModeList(format, flags, count, modes);
    }
    STDMETHODIMP FindClosestMatchingMode(const DXGI_MODE_DESC *wanted, DXGI_MODE_DESC *closest,
                                         IUnknown *device) override {
        return host_->FindClosestMatchingMode(wanted, closest, device);
    }
    STDMETHODIMP WaitForVBlank() override { return host_->WaitForVBlank(); }
    STDMETHODIMP TakeOwnership(IUnknown *, BOOL) override { return S_OK; }
    STDMETHODIMP_(void) ReleaseOwnership() override {}
    STDMETHODIMP GetGammaControlCapabilities(DXGI_GAMMA_CONTROL_CAPABILITIES *caps) override {
        return host_->GetGammaControlCapabilities(caps);
    }
    STDMETHODIMP SetGammaControl(const DXGI_GAMMA_CONTROL *) override { return S_OK; }
    STDMETHODIMP GetGammaControl(DXGI_GAMMA_CONTROL *control) override {
        return host_->GetGammaControl(control);
    }
    STDMETHODIMP SetDisplaySurface(IDXGISurface *) override { return S_OK; }
    STDMETHODIMP GetDisplaySurfaceData(IDXGISurface *) override { return E_NOTIMPL; }
    STDMETHODIMP GetFrameStatistics(DXGI_FRAME_STATISTICS *statistics) override {
        return host_->GetFrameStatistics(statistics);
    }
    virtual HRESULT STDMETHODCALLTYPE GetRefreshRate(DXGI_RATIONAL *rate) {
        if (!rate)
            return E_POINTER;
        *rate = {60, 1};
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE GetAspectRatio(DXGI_RATIONAL *ratio) {
        if (!ratio)
            return E_POINTER;
        *ratio = {16, 9};
        return S_OK;
    }
    // GetDisplayModeListX: the modes a television gives the console, at 60 Hz
    virtual HRESULT STDMETHODCALLTYPE GetDisplayModeListX(DXGI_FORMAT format, UINT, UINT *count,
                                                          ConsoleMode *modes) {
        if (!count)
            return E_POINTER;
        static constexpr UINT sizes[][2] = {{1920, 1080}, {2560, 1440}, {3840, 2160}};
        if (!modes) {
            *count = static_cast<UINT>(std::size(sizes));
            return S_OK;
        }
        const auto given = std::min<UINT>(*count, static_cast<UINT>(std::size(sizes)));
        for (UINT i = 0; i < given; ++i)
            modes[i] = {sizes[i][0],
                        sizes[i][1],
                        {60, 1},
                        format,
                        DXGI_MODE_SCANLINE_ORDER_PROGRESSIVE,
                        DXGI_MODE_SCALING_UNSPECIFIED,
                        0};
        const bool all = given == std::size(sizes);
        *count = given;
        return all ? S_OK : DXGI_ERROR_MORE_DATA;
    }

  private:
    IDXGIOutput *host_;
    std::atomic<ULONG> references_{1};
};
// the host's adapter, but for the outputs it gives
class ConsoleAdapter final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          Microsoft::WRL::ChainInterfaces<IDXGIAdapter2, IDXGIAdapter1, IDXGIAdapter>> {
  public:
    // takes over `host`
    static HRESULT make(IDXGIAdapter *host, IDXGIAdapter **adapter) {
        IDXGIAdapter2 *latest = nullptr;
        const auto result = host->QueryInterface(&latest);
        host->Release();
        if (SUCCEEDED(result))
            *adapter = Microsoft::WRL::Make<ConsoleAdapter>(latest).Detach();
        return result;
    }
    explicit ConsoleAdapter(IDXGIAdapter2 *host) : host_(host) {}
    ~ConsoleAdapter() override { host_->Release(); }
    STDMETHODIMP SetPrivateData(REFGUID id, UINT size, const void *data) override {
        return host_->SetPrivateData(id, size, data);
    }
    STDMETHODIMP SetPrivateDataInterface(REFGUID id, const IUnknown *object) override {
        return host_->SetPrivateDataInterface(id, object);
    }
    STDMETHODIMP GetPrivateData(REFGUID id, UINT *size, void *data) override {
        return host_->GetPrivateData(id, size, data);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        return host_->GetParent(iid, parent);
    }
    STDMETHODIMP EnumOutputs(UINT index, IDXGIOutput **output) override {
        if (!output)
            return E_POINTER;
        IDXGIOutput *host = nullptr;
        const auto result = host_->EnumOutputs(index, &host);
        *output = SUCCEEDED(result) ? new ConsoleOutput(host) : nullptr;
        return result;
    }
    STDMETHODIMP GetDesc(DXGI_ADAPTER_DESC *desc) override { return host_->GetDesc(desc); }
    STDMETHODIMP CheckInterfaceSupport(REFGUID name, LARGE_INTEGER *version) override {
        return host_->CheckInterfaceSupport(name, version);
    }
    STDMETHODIMP GetDesc1(DXGI_ADAPTER_DESC1 *desc) override { return host_->GetDesc1(desc); }
    STDMETHODIMP GetDesc2(DXGI_ADAPTER_DESC2 *desc) override { return host_->GetDesc2(desc); }

  private:
    IDXGIAdapter2 *host_;
};
// The console's Direct3D 12 device answers for IDXGIDevice1, which the host's does not: this
// one leads to the adapter the device was made on, and from there to our factory.
class D3D12DxgiDevice final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IDXGIDevice1> {
  public:
    explicit D3D12DxgiDevice(LUID luid) : luid_(luid) {}
    STDMETHODIMP GetAdapter(IDXGIAdapter **adapter) override {
        if (!adapter)
            return E_POINTER;
        *adapter = nullptr;
        IDXGIFactory4 *factory = nullptr;
        auto result = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
        if (FAILED(result))
            return result;
        IDXGIAdapter *host = nullptr;
        result = factory->EnumAdapterByLuid(luid_, IID_PPV_ARGS(&host));
        factory->Release();
        if (SUCCEEDED(result) && !adapter_wrapper && console == Target::scarlett)
            return ConsoleAdapter::make(host, adapter);
        if (FAILED(result) || !adapter_wrapper) {
            *adapter = host;
            return result;
        }
        return adapter_wrapper(host, adapter);
    }
    STDMETHODIMP GetParent(REFIID iid, void **parent) override {
        if (!is_any<IDXGIAdapter, IDXGIAdapter1, IDXGIAdapter2, IDXGIAdapter3, IDXGIAdapter4>(iid))
            return E_NOINTERFACE;
        return GetAdapter(reinterpret_cast<IDXGIAdapter **>(parent));
    }
    STDMETHODIMP SetPrivateData(REFGUID, UINT, const void *) override { return S_OK; }
    STDMETHODIMP SetPrivateDataInterface(REFGUID, const IUnknown *) override { return S_OK; }
    STDMETHODIMP GetPrivateData(REFGUID, UINT *, void *) override { return DXGI_ERROR_NOT_FOUND; }
    STDMETHODIMP CreateSurface(const DXGI_SURFACE_DESC *, UINT, DXGI_USAGE,
                               const DXGI_SHARED_RESOURCE *, IDXGISurface **) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP QueryResourceResidency(IUnknown *const *, DXGI_RESIDENCY *, UINT) override {
        return E_NOTIMPL;
    }
    STDMETHODIMP SetGPUThreadPriority(INT) override { return S_OK; }
    STDMETHODIMP GetGPUThreadPriority(INT *priority) override {
        *priority = 0;
        return S_OK;
    }
    STDMETHODIMP SetMaximumFrameLatency(UINT) override { return S_OK; }
    STDMETHODIMP GetMaximumFrameLatency(UINT *latency) override {
        *latency = 3;
        return S_OK;
    }

  private:
    LUID luid_;
};
// The console's Direct3D 12 objects are the host's with a vtable of their own, one slot
// before which is the host's: being the host's objects, they go back to the host's API as
// they are, whichever way the title got hold of them.
using Slot = void *;
Slot *original_vtable(void *object) {
    return static_cast<Slot *>((*static_cast<Slot **>(object))[-1]);
}
template <class Method> Method original(void *object, std::size_t index) {
    return reinterpret_cast<Method>(original_vtable(object)[index]);
}
// The host's runtime calls its own objects through their vtable too: a slot the console has
// differently runs the console's method only when the title calls it, and the host's
// otherwise (its callers pass the object first; the title's, for methods that return a
// structure, second). The console's methods are one table per kind of object.
enum class Family { device, list, heap, queue, resource };
constexpr std::size_t max_slots = 160;
template <Family Kind> std::array<Slot, max_slots> console_table{};
using Raw = std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t, std::uint64_t, std::uint64_t,
                                               std::uint64_t);
// Whether the call that returns to `address` is the title's own call of this slot: the
// instruction before it is `call [reg + Index * 8]`. The host's runtime also gets here with
// the title's return address, when one of its methods jumps on to another slot.
bool title_called_slot(const void *address, std::size_t index) {
    if (!detail::in_title(address))
        return false;
    const auto *code = static_cast<const std::uint8_t *>(address);
    const auto offset = static_cast<std::uint32_t>(index * 8);
    std::uint32_t disp32;
    std::memcpy(&disp32, code - 4, 4);
    // FF /2 with a 32-bit displacement: FF 90+r, or FF 94 and a SIB byte
    if (disp32 == offset &&
        ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94)))
        return true;
    // slot 0, no displacement: FF 10+r
    if (offset == 0 && code[-2] == 0xff && (code[-1] & 0xf8) == 0x10)
        return true;
    // with an 8-bit displacement: FF 50+r d8
    if (offset < 0x80 && code[-1] == offset && code[-3] == 0xff && (code[-2] & 0xf8) == 0x50)
        return true;
    // `call reg` (FF D0+r, with a prefix for r8 to r15): the slot was loaded into the register
    // first, with `mov reg, [reg + Index * 8]` a few instructions back. Titles built against
    // the GDK's header call this way.
    if (code[-2] == 0xff && (code[-1] & 0xf8) == 0xd0) {
        for (const auto *at = code - 3; at > code - 160; --at) {
            if (at[0] != 0x8b)
                continue;
            const auto mode = at[1] >> 6;
            const std::size_t skip = (at[1] & 7) == 4 ? 3 : 2; // a SIB byte follows
            std::uint32_t loaded = ~0u;
            if (mode == 2)
                std::memcpy(&loaded, at + skip, 4);
            else if (mode == 1)
                loaded = at[skip];
            else if (mode == 0)
                loaded = 0;
            if (loaded == offset && (mode != 0 || offset == 0))
                return true;
        }
        return false;
    }
    // Another slot's call is the host's runtime going on from one method to the next. A call
    // of a function of the title's own is the title's: the function ends in a jump through
    // the slot (the out-of-line side of the device's inline methods does).
    if ((code[-6] == 0xff && (code[-5] & 0xf8) == 0x90) || (code[-7] == 0xff && code[-6] == 0x94) ||
        (code[-3] == 0xff && (code[-2] & 0xf8) == 0x50) ||
        (code[-2] == 0xff && (code[-1] & 0xf8) == 0x10))
        return false;
    std::int32_t relative;
    std::memcpy(&relative, code - 4, 4);
    return code[-5] == 0xe8 && detail::in_title(code + relative);
}
template <Family Kind, std::size_t Index>
std::uint64_t STDMETHODCALLTYPE by_caller(void *first, std::uint64_t a, std::uint64_t b,
                                          std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                          std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                          std::uint64_t i, std::uint64_t j) {
    const auto method = title_called_slot(_ReturnAddress(), Index) ? console_table<Kind>[Index]
                                                                   : original_vtable(first)[Index];
    return reinterpret_cast<Raw>(method)(first, a, b, c, d, e, f, g, h, i, j);
}
template <Family Kind, std::size_t... Index>
std::array<Slot, max_slots> by_caller_slots(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&by_caller<Kind, Index>)...};
}
template <Family Kind>
const auto dispatchers = by_caller_slots<Kind>(std::make_index_sequence<max_slots>{});
// One console vtable per host vtable (one per class of the host's runtime); an object that
// has one already keeps it
template <Family Kind>
void console_vtable(void *object, std::size_t host_slots, std::size_t slots,
                    const std::vector<std::pair<std::size_t, Slot>> &overrides) {
    static std::mutex mutex;
    static std::map<Slot *, Slot *> blocks; // host vtable -> console vtable
    const std::lock_guard lock(mutex);
    auto *const host = *static_cast<Slot **>(object);
    for (const auto &[from, to] : blocks)
        if (to == host)
            return;
    auto &block = blocks[host];
    if (!block) {
        // The host's class has methods of its own after the interface's, which its runtime
        // calls on the object: they are copied as well, as far as its vtable's memory goes.
        std::size_t copied = host_slots;
        if (MEMORY_BASIC_INFORMATION region{}; VirtualQuery(host, &region, sizeof region))
            copied = std::min<std::size_t>(
                max_slots, (static_cast<const std::uint8_t *>(region.BaseAddress) +
                            region.RegionSize - reinterpret_cast<const std::uint8_t *>(host)) /
                               sizeof(Slot));
        copied = std::max(copied, host_slots);
        auto *const memory = new Slot[std::max(slots, copied) + 1]{};
        memory[0] = host;
        std::copy(host, host + copied, memory + 1);
        // a console method where the host has one of its own: by who calls
        for (std::size_t index = host_slots; index < slots; ++index)
            memory[1 + index] = dispatchers<Kind>[index];
        for (const auto &[index, function] : overrides) {
            console_table<Kind>[index] = function;
            memory[1 + index] = dispatchers<Kind>[index];
        }
        block = memory + 1;
    }
    *static_cast<Slot **>(object) = block;
}
// Methods that return an 8-byte structure: the console's return it in RAX, where the
// desktop's C++ methods write it through a hidden pointer
template <std::size_t Index> std::uint64_t STDMETHODCALLTYPE returned_in_rax(void *self) {
    std::uint64_t value = 0;
    original<void *(STDMETHODCALLTYPE *)(void *, std::uint64_t *)>(self, Index)(self, &value);
    return value;
}

// Methods that return a larger structure: the console's take the place for it first and the
// object second, the desktop's C++ methods the other way round
D3D12_RESOURCE_ALLOCATION_INFO *STDMETHODCALLTYPE
resource_allocation_info(D3D12_RESOURCE_ALLOCATION_INFO *info, ID3D12Device *self, UINT mask,
                         UINT count, const D3D12_RESOURCE_DESC *descs) {
    std::vector<D3D12_RESOURCE_DESC> host(descs, descs + (descs ? count : 0));
    for (auto &desc : host)
        desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(desc.Flags & 0xff);
    using Get = D3D12_RESOURCE_ALLOCATION_INFO *(
        STDMETHODCALLTYPE *)(void *, D3D12_RESOURCE_ALLOCATION_INFO *, UINT, UINT,
                             const D3D12_RESOURCE_DESC *);
    return original<Get>(self, 25)(self, info, mask, count, host.data());
}
D3D12_HEAP_PROPERTIES *STDMETHODCALLTYPE custom_heap_properties(D3D12_HEAP_PROPERTIES *properties,
                                                                ID3D12Device *self, UINT node,
                                                                D3D12_HEAP_TYPE type) {
    using Get = D3D12_HEAP_PROPERTIES *(STDMETHODCALLTYPE *)(void *, D3D12_HEAP_PROPERTIES *, UINT,
                                                             D3D12_HEAP_TYPE);
    return original<Get>(self, 26)(self, properties, node, type);
}
// a console method that is a host one at another slot, with the same arguments
template <std::size_t Host>
std::uint64_t STDMETHODCALLTYPE moved_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    return reinterpret_cast<Raw>(original_vtable(self)[Host])(self, a, b, c, d, e, f, g, h, i, j);
}
// ID3D12Resource as the console has it: the desktop's methods, then GetPlacementDescX and
// GetOpaqueResourceTypeX, which nothing here has
HRESULT STDMETHODCALLTYPE no_console_method(void *) { return E_NOTIMPL; }
void console_resource(void *resource) {
    if (resource && std::getenv("XV_PATCH_RESOURCES"))
        console_vtable<Family::resource>(resource, 15, 17,
                                         {{15, reinterpret_cast<Slot>(&no_console_method)},
                                          {16, reinterpret_cast<Slot>(&no_console_method)}});
}
// Map (8) of a resource the host does not map, and the address of one that has none. All of the
// console's memory is the CPU's as well, and a title maps a texture of the default heap to fill it
// (one clears its back buffers that way). The title gets memory of ours of the resource's size
// instead, and what it writes there goes nowhere.
std::mutex scratch_mutex;
struct Scratch {
    void *memory = nullptr;
    std::uint64_t size = 0;
};
std::map<ID3D12Resource *, Scratch> scratch_maps;
void *scratch_memory(ID3D12Resource *self) {
    ID3D12Device *device = nullptr;
    if (FAILED(self->GetDevice(IID_PPV_ARGS(&device))))
        return nullptr;
    const auto desc = self->GetDesc();
    const auto size = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
    device->Release();
    if (size == ~0ull)
        return nullptr;
    const std::lock_guard lock(scratch_mutex);
    // a resource made where a released one was finds that one's memory, which may be smaller
    auto &scratch = scratch_maps[self];
    if (scratch.size < size) {
        if (scratch.memory)
            VirtualFree(scratch.memory, 0, MEM_RELEASE);
        scratch = {VirtualAlloc(nullptr, static_cast<SIZE_T>(size), MEM_RESERVE | MEM_COMMIT,
                                PAGE_READWRITE),
                   size};
        if (static std::atomic<int> shown{0}; shown++ < 4) {
            std::fprintf(stderr,
                         "[Runtime] the title writes a %llux%u texture's memory, which the host "
                         "does not give: what it writes there is lost\n",
                         desc.Width, desc.Height);
            std::fflush(stderr);
        }
    }
    return scratch.memory;
}
HRESULT STDMETHODCALLTYPE map_resource(ID3D12Resource *self, UINT subresource,
                                       const D3D12_RANGE *range, void **data) {
    using Map = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *, void **);
    const auto result = original<Map>(self, 8)(self, subresource, range, data);
    if (SUCCEEDED(result) || !data)
        return result;
    *data = scratch_memory(self);
    if (!*data) {
        const auto desc = self->GetDesc();
        std::fprintf(stderr,
                     "[Runtime] Map of subresource %u of a resource of dimension %d %llux%u "
                     "format %d flags 0x%x: 0x%08lx\n",
                     subresource, desc.Dimension, desc.Width, desc.Height, desc.Format, desc.Flags,
                     static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return *data ? S_OK : result;
}
void STDMETHODCALLTYPE unmap_resource(ID3D12Resource *self, UINT subresource,
                                      const D3D12_RANGE *range) {
    {
        const std::lock_guard lock(scratch_mutex);
        if (scratch_maps.contains(self))
            return;
    }
    using Unmap = void(STDMETHODCALLTYPE *)(void *, UINT, const D3D12_RANGE *);
    original<Unmap>(self, 9)(self, subresource, range);
}
// GetGPUVirtualAddress (11): see track_buffer
bool mapped_address(const void *resource, std::uint64_t &address);
std::uint64_t STDMETHODCALLTYPE resource_address(ID3D12Resource *self) {
    if (std::uint64_t address = 0; mapped_address(self, address))
        return address;
    const auto address = original<std::uint64_t(STDMETHODCALLTYPE *)(void *)>(self, 11)(self);
    // the host has no address for a texture; the console's is its memory
    return address ? address : reinterpret_cast<std::uint64_t>(scratch_memory(self));
}
void mappable_resource(void *resource) {
    console_vtable<Family::resource>(resource, 15, 17,
                                     {{8, reinterpret_cast<Slot>(&map_resource)},
                                      {9, reinterpret_cast<Slot>(&unmap_resource)},
                                      {11, reinterpret_cast<Slot>(&resource_address)}});
}
// ID3D12Device::QueryInterface: the console's device is also its DXGI device
HRESULT STDMETHODCALLTYPE device_query(ID3D12Device *self, REFIID iid, void **object) {
    if (object && is_any<IDXGIObject, IDXGIDevice, IDXGIDevice1>(iid)) {
        *object = static_cast<IDXGIDevice1 *>(
            Microsoft::WRL::Make<D3D12DxgiDevice>(self->GetAdapterLuid()).Detach());
        return S_OK;
    }
    // ID3D12Device8 had another interface ID in the kits than the one the desktop shipped with
    static constexpr GUID kit_device8{
        0x728f1fa2, 0xea11, 0x4a9f, {0x9b, 0x52, 0x9b, 0x42, 0xa4, 0x84, 0xd5, 0xc9}};
    return original<HRESULT(STDMETHODCALLTYPE *)(void *, REFIID, void **)>(self, 0)(
        self, iid == kit_device8 ? __uuidof(ID3D12Device8) : iid, object);
}
// CheckFeatureSupport: some of the console's structures are larger than the desktop's
// (D3D12_OPTIONS is 64 bytes there): the host fills its part, the rest is zero
HRESULT STDMETHODCALLTYPE check_feature_support(ID3D12Device *self, D3D12_FEATURE feature,
                                                void *data, UINT size) {
    using Check = HRESULT(STDMETHODCALLTYPE *)(void *, D3D12_FEATURE, void *, UINT);
    const auto check = original<Check>(self, 13);
    auto result = check(self, feature, data, size);
    if (result != E_INVALIDARG || !data)
        return result;
    std::vector<std::uint8_t> input(static_cast<std::uint8_t *>(data),
                                    static_cast<std::uint8_t *>(data) + size);
    for (UINT smaller = size - 4; smaller >= 4 && smaller + 32 >= size; smaller -= 4) {
        std::memcpy(data, input.data(), size);
        if (SUCCEEDED(result = check(self, feature, data, smaller))) {
            std::memset(static_cast<std::uint8_t *>(data) + smaller, 0, size - smaller);
            break;
        }
    }
    return result;
}
// Descriptor heaps as the console has them: a CPU handle is the address of the descriptor,
// below 4 GiB, and titles write descriptors there themselves. Each heap gets such memory of
// its own, a descriptor's worth per host descriptor; the host's descriptor at the same index
// is the one that counts. Descriptors made through the device are the host's already (the
// shadow holds a mark); those the title wrote are hardware descriptors, read when used.
struct ShadowHeap {
    std::uintptr_t base, end;
    D3D12_CPU_DESCRIPTOR_HANDLE host;
    UINT stride;
    D3D12_DESCRIPTOR_HEAP_TYPE type;
    ID3D12Device *device;
    bool shader_visible;
    std::uint64_t gpu; // the host's GPU handle of the start, for a shader-visible heap
    UINT host_stride;
    void *owner; // the heap the title holds
};
// The console's descriptor sizes, which titles also have compiled in: the device's inline
// methods copy descriptors of these sizes themselves
UINT console_stride(D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ? 32
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV       ? 112
           : type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV       ? 128
                                                          : 16;
}
UINT STDMETHODCALLTYPE descriptor_increment(ID3D12Device *, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    return console_stride(type);
}
std::mutex shadow_mutex;
std::map<std::uintptr_t, ShadowHeap> shadow_heaps;         // by base
std::map<void *, std::uintptr_t> shadow_of;                // heap -> base
constexpr std::uint64_t host_mark = 0x5853'4844'4553'4352; // "XSHDESCR"
void *low_memory(std::size_t bytes) {
    MEM_ADDRESS_REQUIREMENTS requirements{};
    requirements.HighestEndingAddress = reinterpret_cast<void *>(0xffff'ffffull);
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &requirements;
    return VirtualAlloc2(nullptr, nullptr, (bytes + 0xffff) & ~std::size_t{0xffff},
                         MEM_RESERVE | MEM_COMMIT | MEM_WRITE_WATCH, PAGE_READWRITE, &parameter, 1);
}
const ShadowHeap *shadow_heap(std::uintptr_t address) {
    auto found = shadow_heaps.upper_bound(address);
    if (found == shadow_heaps.begin())
        return nullptr;
    --found;
    return address < found->second.end ? &found->second : nullptr;
}
// the host's handle for a console one; others are the host's already
D3D12_CPU_DESCRIPTOR_HANDLE host_handle(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        return {heap->host.ptr + (handle.ptr - heap->base) / heap->stride * heap->host_stride};
    return handle;
}
D3D12_CPU_DESCRIPTOR_HANDLE host_slot(const ShadowHeap &heap, std::uintptr_t at) {
    return {heap.host.ptr + (at - heap.base) / heap.stride * heap.host_stride};
}
// The mark of a host descriptor, and the host's handle of the slot that holds it: a title
// that copies descriptors itself (the device's inline CopyDescriptorsSimple) carries both
// along, which tells where the host's descriptor of the copy is to come from.
void set_mark(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    const std::uint64_t words[2]{host_mark, host_slot(heap, at).ptr};
    std::memcpy(reinterpret_cast<void *>(at), words, sizeof words);
}
void mark_host(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr))
        set_mark(*heap, handle.ptr);
}
// whether a host handle is a slot of a heap the title still has
bool live_slot(D3D12_DESCRIPTOR_HEAP_TYPE type, std::uint64_t handle) {
    const std::lock_guard lock(shadow_mutex);
    for (const auto &[base, heap] : shadow_heaps) {
        const auto size = (heap.end - heap.base) / heap.stride * heap.host_stride;
        if (heap.type == type && handle >= heap.host.ptr && handle - heap.host.ptr < size)
            return (handle - heap.host.ptr) % heap.host_stride == 0;
    }
    return false;
}
// A marked descriptor that names another slot is a copy the title made itself: the host's
// descriptor follows it
void resolve(const ShadowHeap &heap, std::uintptr_t at) {
    at = heap.base + (at - heap.base) / heap.stride * heap.stride;
    std::uint64_t words[2];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    const auto own = host_slot(heap, at);
    if (words[0] != host_mark || words[1] == own.ptr || words[1] == 0)
        return;
    // the slot it names may have gone with its heap, or the title may be writing here now
    if (!live_slot(heap.type, words[1]))
        return;
    using Copy = void(STDMETHODCALLTYPE *)(void *, UINT, D3D12_CPU_DESCRIPTOR_HANDLE,
                                           D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(heap.device, 24)(heap.device, 1, own, D3D12_CPU_DESCRIPTOR_HANDLE{words[1]},
                                    heap.type);
    set_mark(heap, at);
}
// the host's handle for a console one whose descriptor is about to be read
D3D12_CPU_DESCRIPTOR_HANDLE host_view(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    ShadowHeap heap;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(handle.ptr);
        if (!found)
            return handle;
        heap = *found;
    }
    resolve(heap, handle.ptr);
    return host_slot(heap, handle.ptr);
}
// a constant buffer view of nothing, as the device's inline CreateConstantBufferView writes it
bool null_buffer(const std::uint32_t *words) { return words[2] == 1 && words[3] == 0x0002403c; }
// A descriptor the title wrote: made on the host from what the hardware descriptor says.
// Buffers (V#: 48-bit address, stride in bits 48-61, record count in the third dword) become
// constant buffer views.
void decode_written(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    ShadowHeap found{};
    {
        const std::lock_guard lock(shadow_mutex);
        if (const auto *const live = shadow_heap(handle.ptr))
            found = *live;
    }
    const auto *const heap = found.base ? &found : nullptr;
    if (!heap || heap->type != D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
        return;
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(handle.ptr), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark)
        resolve(*heap, handle.ptr);
    if (first == host_mark || first == 0 || null_buffer(words))
        return;
    // what kinds of descriptors titles write, by the type in the last dword (buffers 0, images
    // 8 and up), once each
    if (static std::atomic<std::uint32_t> seen{0}; std::getenv("XV_D3D_TRACE")) {
        const std::uint32_t bit = 1u << (words[3] >> 28);
        if (!(seen.fetch_or(bit) & bit))
            std::fprintf(stderr, "[Runtime] descriptor written by the title: %08x %08x %08x %08x\n",
                         words[0], words[1], words[2], words[3]);
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const std::uint32_t stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t size = std::uint64_t{words[2]} * (stride ? stride : 1);
    const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
        address, static_cast<UINT>(std::min<std::uint64_t>((size + 255) & ~255ull, 65536))};
    using Create = void(STDMETHODCALLTYPE *)(void *, const D3D12_CONSTANT_BUFFER_VIEW_DESC *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(heap->device, 17)(heap->device, &view, host_handle(handle));
}
void register_shadow(ID3D12Device *device, void *heap, const D3D12_DESCRIPTOR_HEAP_DESC &desc,
                     D3D12_CPU_DESCRIPTOR_HANDLE host, std::uint64_t gpu) {
    const UINT stride = console_stride(desc.Type);
    auto *const memory = low_memory(std::size_t{desc.NumDescriptors} * stride);
    if (!memory) {
        std::fprintf(stderr, "[Runtime] no memory below 4 GiB for a descriptor heap\n");
        return;
    }
    const auto base = reinterpret_cast<std::uintptr_t>(memory);
    const std::lock_guard lock(shadow_mutex);
    shadow_heaps[base] = {base,
                          base + std::size_t{desc.NumDescriptors} * stride,
                          host,
                          stride,
                          desc.Type,
                          device,
                          (desc.Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) != 0,
                          gpu,
                          device->GetDescriptorHandleIncrementSize(desc.Type),
                          heap};
    shadow_of[heap] = base;
}
// Before the GPU runs: what the title wrote straight into a shader-visible heap since last
// time, read from the pages written to
void decode_written_heaps() {
    std::vector<ShadowHeap> heaps;
    {
        const std::lock_guard lock(shadow_mutex);
        for (const auto &[base, heap] : shadow_heaps)
            if (heap.shader_visible && heap.type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
                heaps.push_back(heap);
    }
    std::vector<void *> pages(4096);
    for (const auto &heap : heaps) {
        ULONG_PTR count = pages.size();
        ULONG size = 0;
        const auto bytes = ((heap.end - heap.base) + 0xffff) & ~std::uintptr_t{0xffff};
        if (GetWriteWatch(WRITE_WATCH_FLAG_RESET, reinterpret_cast<void *>(heap.base), bytes,
                          pages.data(), &count, &size) != 0)
            continue;
        for (ULONG_PTR i = 0; i < count; ++i) {
            const auto page = reinterpret_cast<std::uintptr_t>(pages[i]);
            const auto first = std::max(heap.base, page);
            const auto last = std::min(heap.end, page + size);
            for (auto at =
                     heap.base + (first - heap.base + heap.stride - 1) / heap.stride * heap.stride;
                 at + heap.stride <= last; at += heap.stride)
                decode_written({at});
        }
    }
}
// ID3D12DescriptorHeap::GetCPUDescriptorHandleForHeapStart: the shadow's start
// The title's descriptor heap: a proxy of the host's, which the host's runtime keeps as its
// own (it knows its objects by their vtable). The proxy's handles are the console's: the CPU
// one the shadow's start, both in RAX.
struct HeapProxy {
    Slot *vtable;
    ID3D12DescriptorHeap *host;
    std::atomic<ULONG> references;
};
std::mutex heap_proxy_mutex;
std::atomic<unsigned> shadows_released{0};
std::set<const void *> heap_proxies;
ID3D12DescriptorHeap *host_heap(void *heap) {
    const std::lock_guard lock(heap_proxy_mutex);
    return heap_proxies.contains(heap) ? static_cast<HeapProxy *>(heap)->host
                                       : static_cast<ID3D12DescriptorHeap *>(heap);
}
std::uint64_t STDMETHODCALLTYPE cpu_heap_start(void *self) {
    const std::lock_guard lock(shadow_mutex);
    const auto found = shadow_of.find(self);
    return found != shadow_of.end()
               ? found->second
               : static_cast<HeapProxy *>(self)->host->GetCPUDescriptorHandleForHeapStart().ptr;
}
// the console's GPU handles are its CPU ones: the shadow's start as well
std::uint64_t STDMETHODCALLTYPE gpu_heap_start(void *self) {
    {
        const std::lock_guard lock(shadow_mutex);
        const auto found = shadow_of.find(self);
        if (found != shadow_of.end() && shadow_heaps[found->second].shader_visible)
            return found->second;
    }
    return static_cast<HeapProxy *>(self)->host->GetGPUDescriptorHandleForHeapStart().ptr;
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE heap_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d) {
    auto *const host = static_cast<HeapProxy *>(self)->host;
    return reinterpret_cast<std::uint64_t(STDMETHODCALLTYPE *)(void *, std::uint64_t, std::uint64_t,
                                                               std::uint64_t, std::uint64_t)>(
        (*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d);
}
HRESULT STDMETHODCALLTYPE heap_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12DescriptorHeap>(
            iid)) {
        ++static_cast<HeapProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return static_cast<HeapProxy *>(self)->host->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE heap_add_ref(void *self) {
    return ++static_cast<HeapProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE heap_release(void *self) {
    auto *const proxy = static_cast<HeapProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(heap_proxy_mutex);
            heap_proxies.erase(proxy);
        }
        // its shadow goes with it: the host's descriptors it stood for are no more
        {
            const std::lock_guard lock(shadow_mutex);
            if (const auto found = shadow_of.find(proxy); found != shadow_of.end()) {
                VirtualFree(reinterpret_cast<void *>(found->second), 0, MEM_RELEASE);
                shadow_heaps.erase(found->second);
                shadow_of.erase(found);
                ++shadows_released;
            }
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
template <std::size_t... Index> std::array<Slot, 11> heap_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 11> vtable{reinterpret_cast<Slot>(&heap_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&heap_query);
    vtable[1] = reinterpret_cast<Slot>(&heap_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&heap_release);
    vtable[9] = reinterpret_cast<Slot>(&cpu_heap_start);
    vtable[10] = reinterpret_cast<Slot>(&gpu_heap_start);
    return vtable;
}
std::array<Slot, 11> heap_vtable = heap_vtable_of(std::make_index_sequence<11>{});
// The buffers the title has, by GPU address: a view of one names the resource, where a
// hardware descriptor names an address
struct BufferRange {
    std::uint64_t end;
    ID3D12Resource *resource;
};
std::mutex buffer_mutex;
std::map<std::uint64_t, BufferRange> buffers_at; // by start
// On the Series console a buffer's GPU address is where its memory is for the CPU as well,
// and a title writes there without mapping anything. Such a buffer is kept mapped here and
// the title is given that address as its GPU address; an address that comes back from the
// title is turned into the host's wherever the host takes one.
struct MappedBuffer {
    std::uint64_t end, gpu;
};
std::map<std::uint64_t, MappedBuffer> mapped_at;    // by the address the title has
std::map<const void *, std::uint64_t> mapped_start; // by resource
bool mapped_address(const void *resource, std::uint64_t &address) {
    const std::lock_guard lock(buffer_mutex);
    const auto found = mapped_start.find(resource);
    if (found == mapped_start.end())
        return false;
    address = found->second;
    return true;
}
std::uint64_t host_address(std::uint64_t address) {
    const std::lock_guard lock(buffer_mutex);
    auto found = mapped_at.upper_bound(address);
    if (found == mapped_at.begin())
        return address;
    --found;
    return address < found->second.end ? found->second.gpu + (address - found->first) : address;
}
void track_buffer(ID3D12Resource *resource, bool mapped = false) {
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
        return;
    auto start = resource->GetGPUVirtualAddress();
    void *memory = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    const bool kept = mapped && SUCCEEDED(resource->Map(0, &nothing_read, &memory)) && memory;
    const std::lock_guard lock(buffer_mutex);
    if (kept) {
        const auto address = reinterpret_cast<std::uint64_t>(memory);
        // what was mapped here before is gone
        for (auto old = mapped_at.lower_bound(address);
             old != mapped_at.end() && old->first < address + desc.Width;)
            old = mapped_at.erase(old);
        mapped_at[address] = {address + desc.Width, start};
        mapped_start[resource] = address;
        start = address;
    } else {
        mapped_start.erase(resource);
    }
    buffers_at[start] = {start + desc.Width, resource};
}
bool buffer_at(std::uint64_t address, ID3D12Resource *&resource, std::uint64_t &offset,
               std::uint64_t &size) {
    const std::lock_guard lock(buffer_mutex);
    auto found = buffers_at.upper_bound(address);
    if (found == buffers_at.begin())
        return false;
    --found;
    if (address >= found->second.end)
        return false;
    resource = found->second.resource;
    offset = address - found->first;
    size = found->second.end - address;
    return true;
}

// An address the title gives for a vertex or index buffer that no buffer here answers for:
// worth a line, since the host's GPU faults on it
void check_address(const char *what, std::uint64_t address, std::uint64_t bytes) {
    if (!address)
        return;
    ID3D12Resource *resource = nullptr;
    std::uint64_t offset = 0, size = 0;
    {
        const std::lock_guard lock(buffer_mutex);
        auto found = mapped_at.upper_bound(address);
        if (found != mapped_at.begin() && address < std::prev(found)->second.end)
            return;
    }
    if (buffer_at(address, resource, offset, size) && size >= bytes)
        return;
    if (static std::atomic<int> shown{0}; shown++ < 8) {
        MEMORY_BASIC_INFORMATION info{};
        const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                         info.State == MEM_COMMIT;
        std::fprintf(stderr, "[Runtime] %s at %llx (%llu bytes) is in no buffer known here%s\n",
                     what, static_cast<unsigned long long>(address),
                     static_cast<unsigned long long>(bytes),
                     cpu ? ": it is memory of the title's" : "");
        std::fflush(stderr);
    }
}
// The descriptor tables of each root signature: the type of the descriptors at each offset
// from a table's start, for the descriptors the title writes itself
struct TableRange {
    D3D12_DESCRIPTOR_RANGE_TYPE type;
    UINT offset, count;
};
std::mutex signature_mutex;
std::map<const void *, std::vector<std::vector<TableRange>>> tables_of; // per parameter
void read_root_signature(const void *signature, const void *blob, SIZE_T size) {
    using Create = HRESULT(WINAPI *)(const void *, SIZE_T, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateVersionedRootSignatureDeserializer");
    ID3D12VersionedRootSignatureDeserializer *reader = nullptr;
    const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc = nullptr;
    if (!create || FAILED(create(blob, size, IID_PPV_ARGS(&reader))) ||
        FAILED(reader->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &desc))) {
        if (reader)
            reader->Release();
        return;
    }
    std::vector<std::vector<TableRange>> tables(desc->Desc_1_1.NumParameters);
    for (UINT i = 0; i < desc->Desc_1_1.NumParameters; ++i) {
        const auto &parameter = desc->Desc_1_1.pParameters[i];
        if (parameter.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            continue;
        UINT next = 0;
        for (UINT r = 0; r < parameter.DescriptorTable.NumDescriptorRanges; ++r) {
            const auto &range = parameter.DescriptorTable.pDescriptorRanges[r];
            const UINT offset =
                range.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND
                    ? next
                    : range.OffsetInDescriptorsFromTableStart;
            // an unbounded range: as many as a table here reads at most
            const UINT count = range.NumDescriptors == UINT_MAX ? 64 : range.NumDescriptors;
            tables[i].push_back({range.RangeType, offset, count});
            next = offset + count;
        }
    }
    reader->Release();
    const std::lock_guard lock(signature_mutex);
    tables_of[signature] = std::move(tables);
}

// A descriptor the title wrote, made on the host as what the table it is bound in says it is.
// A buffer descriptor (V#) has a 48-bit address, the stride in bits 48-61 and the record
// count in its third dword.
std::atomic<std::uint64_t> decoded[4];
void decode_as(const ShadowHeap &heap, std::uintptr_t at, D3D12_DESCRIPTOR_RANGE_TYPE type) {
    std::uint32_t words[4];
    std::memcpy(words, reinterpret_cast<const void *>(at), sizeof words);
    std::uint64_t first;
    std::memcpy(&first, words, sizeof first);
    if (first == host_mark) {
        resolve(heap, at);
        return;
    }
    ++decoded[type & 3];
    if (static std::atomic<std::uint32_t> shown{0};
        std::getenv("XV_D3D_TRACE") && first != 0 && !(shown.fetch_or(1u << type) & (1u << type))) {
        std::uint32_t more[4]{};
        if (heap.stride >= 32)
            std::memcpy(more, reinterpret_cast<const void *>(at + 16), sizeof more);
        std::fprintf(stderr,
                     "[Runtime] title %s descriptor: %08x %08x %08x %08x | %08x %08x %08x %08x"
                     "\n",
                     type == 0   ? "SRV"
                     : type == 1 ? "UAV"
                     : type == 2 ? "CBV"
                                 : "sampler",
                     words[0], words[1], words[2], words[3], more[0], more[1], more[2], more[3]);
        std::fflush(stderr);
    }
    // A sampler the title wrote (S#): the hardware's four words of sampler state. Clamp
    // modes in bits 0 to 8 of the first, the comparison in 12 to 14, the anisotropy in 9 to
    // 11; the LOD range in the second (4.8 fixed point each); the bias in the low 14 bits of
    // the third (signed 5.8), then the filters; the border colour's kind in the top of the
    // last. A sampler slot the host is left without hangs the GPU when it is sampled with.
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER) {
        static constexpr D3D12_TEXTURE_ADDRESS_MODE modes[] = {
            D3D12_TEXTURE_ADDRESS_MODE_WRAP,   D3D12_TEXTURE_ADDRESS_MODE_MIRROR,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP,  D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE,
            D3D12_TEXTURE_ADDRESS_MODE_BORDER, D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE};
        const auto magnify = (words[2] >> 20) & 3, minify = (words[2] >> 22) & 3,
                   mip = (words[2] >> 26) & 3, compare = (words[0] >> 12) & 7,
                   ratio = (words[0] >> 9) & 7, border = words[3] >> 30;
        const bool anisotropic = magnify >= 2 || minify >= 2;
        const bool compares = compare != 0 && compare != 7;
        D3D12_SAMPLER_DESC desc{};
        desc.Filter = static_cast<D3D12_FILTER>(
            (anisotropic ? 0x55u : (minify & 1) << 4 | (magnify & 1) << 2 | (mip == 2 ? 1u : 0u)) |
            (compares ? 0x80u : 0u));
        desc.AddressU = modes[words[0] & 7];
        desc.AddressV = modes[(words[0] >> 3) & 7];
        desc.AddressW = modes[(words[0] >> 6) & 7];
        desc.MipLODBias =
            static_cast<float>(static_cast<std::int32_t>(words[2] << 18) >> 18) / 256.0f;
        desc.MaxAnisotropy = anisotropic ? 1u << ratio : 1;
        desc.ComparisonFunc = compares ? static_cast<D3D12_COMPARISON_FUNC>(compare + 1)
                                       : D3D12_COMPARISON_FUNC_NEVER;
        const float colour = border == 2 ? 1.0f : 0.0f;
        desc.BorderColor[0] = desc.BorderColor[1] = desc.BorderColor[2] = colour;
        desc.BorderColor[3] = border == 0 ? 0.0f : 1.0f;
        desc.MinLOD = static_cast<float>(words[1] & 0xfff) / 256.0f;
        desc.MaxLOD = mip ? static_cast<float>((words[1] >> 12) & 0xfff) / 256.0f : desc.MinLOD;
        auto *const device = heap.device;
        original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
            device, 22)(device, &desc, host_slot(heap, at));
        set_mark(heap, at);
        return;
    }
    // a descriptor the title cleared is a null one, whose reads give 0: the host's slot may
    // hold anything until it is made so
    if ((first == 0 && words[2] == 0 && words[3] == 0) || null_buffer(words)) {
        const auto host = host_slot(heap, at);
        auto *const device = heap.device;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
            const D3D12_CONSTANT_BUFFER_VIEW_DESC view{};
            original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
                device, 17)(device, &view, host);
        } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Texture2D.MipLevels = 1;
            original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                device, nullptr, &view, host);
        } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_R32_UINT;
            view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                device, nullptr, nullptr, &view, host);
        } else {
            return;
        }
        set_mark(heap, at);
        return;
    }
    const std::uint64_t address = first & 0xffff'ffff'ffffull;
    const UINT stride = (words[1] >> 16) & 0x3fff;
    const std::uint64_t bytes = std::uint64_t{words[2]} * (stride ? stride : 1);
    const auto host = host_slot(heap, at);
    auto *const device = heap.device;
    if (type == D3D12_DESCRIPTOR_RANGE_TYPE_CBV) {
        // the address is the one the title has for the buffer, which on Series is where
        // its memory is: the host is given its own for it, and no more than the buffer has
        ID3D12Resource *resource = nullptr;
        std::uint64_t offset = 0, room = 0;
        if (!buffer_at(address, resource, offset, room))
            return;
        const auto wanted = std::max<std::uint64_t>((bytes + 255) & ~255ull, 256);
        const D3D12_CONSTANT_BUFFER_VIEW_DESC view{
            host_address(address),
            static_cast<UINT>(std::min<std::uint64_t>({wanted, 65536, room & ~255ull}))};
        if (!view.SizeInBytes)
            return;
        original<void(STDMETHODCALLTYPE *)(void *, const void *, D3D12_CPU_DESCRIPTOR_HANDLE)>(
            device, 17)(device, &view, host);
    } else if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV || type == D3D12_DESCRIPTOR_RANGE_TYPE_UAV) {
        ID3D12Resource *resource = nullptr;
        std::uint64_t offset = 0, size = 0;
        if (!buffer_at(address, resource, offset, size))
            return;
        size = std::min(size, bytes ? bytes : size);
        // structured when the descriptor has a stride that is not a format's, raw otherwise
        const bool structured = stride > 4 && offset % stride == 0;
        const UINT element = structured ? stride : 4;
        if (type == D3D12_DESCRIPTOR_RANGE_TYPE_SRV) {
            D3D12_SHADER_RESOURCE_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0,
                           structured ? D3D12_BUFFER_SRV_FLAG_NONE : D3D12_BUFFER_SRV_FLAG_RAW};
            original<void(STDMETHODCALLTYPE *)(void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 18)(
                device, resource, &view, host);
        } else {
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format = structured ? DXGI_FORMAT_UNKNOWN : DXGI_FORMAT_R32_TYPELESS;
            view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            view.Buffer = {offset / element, static_cast<UINT>(size / element),
                           structured ? stride : 0, 0,
                           structured ? D3D12_BUFFER_UAV_FLAG_NONE : D3D12_BUFFER_UAV_FLAG_RAW};
            original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *,
                                               D3D12_CPU_DESCRIPTOR_HANDLE)>(device, 19)(
                device, resource, nullptr, &view, host);
        }
    } else {
        return;
    }
    set_mark(heap, at);
}
// A table bound at a GPU handle: what the title wrote in it, by the root signature's ranges
void decode_table(const void *signature, UINT parameter, std::uint64_t gpu) {
    std::vector<TableRange> ranges;
    {
        const std::lock_guard lock(signature_mutex);
        const auto found = tables_of.find(signature);
        if (found == tables_of.end() || parameter >= found->second.size())
            return;
        ranges = found->second[parameter];
    }
    ShadowHeap heap{};
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *found = shadow_heap(gpu);
        if (!found || !found->shader_visible)
            return;
        heap = *found;
    }
    const std::uintptr_t table = gpu;
    for (const auto &range : ranges)
        for (UINT i = 0; i < range.count; ++i) {
            const auto at = table + std::uintptr_t{range.offset + i} * heap.stride;
            if (at + heap.stride > heap.end)
                break;
            decode_as(heap, at, range.type);
        }
}

// The device's methods that take CPU handles: the host's handle, and the shadow marked
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
void STDMETHODCALLTYPE create_cbv(ID3D12Device *self, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
                                  Handle handle) {
    D3D12_CONSTANT_BUFFER_VIEW_DESC host{};
    if (desc) {
        host = *desc;
        // the console takes a view of any size; the host wants multiples of 256 bytes, and
        // removes the device over one that is not
        ID3D12Resource *buffer = nullptr;
        std::uint64_t offset = 0, room = 0;
        const auto rounded = (host.SizeInBytes + 255u) & ~255u;
        if (rounded != host.SizeInBytes)
            host.SizeInBytes = buffer_at(host.BufferLocation, buffer, offset, room) &&
                                       room < rounded && room >= 256
                                   ? static_cast<UINT>(room & ~255ull)
                                   : rounded;
        host.BufferLocation = host_address(host.BufferLocation);
    }
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 17)(
        self, desc ? &host : nullptr, host_handle(handle));
    mark_host(handle);
}
// What a view without a format reads a typeless texture as: the console takes the number
// format from elsewhere, the host wants it in the view. `srgb` picks the gamma twin.
DXGI_FORMAT typed_format(DXGI_FORMAT format, bool srgb = false) {
    switch (format) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_BC1_TYPELESS:
        return srgb ? DXGI_FORMAT_BC1_UNORM_SRGB : DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS:
        return srgb ? DXGI_FORMAT_BC2_UNORM_SRGB : DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS:
        return srgb ? DXGI_FORMAT_BC3_UNORM_SRGB : DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS:
        return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS:
        return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return srgb ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return srgb ? DXGI_FORMAT_B8G8R8X8_UNORM_SRGB : DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS:
        return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS:
        return srgb ? DXGI_FORMAT_BC7_UNORM_SRGB : DXGI_FORMAT_BC7_UNORM;
    default:
        return format;
    }
}
void STDMETHODCALLTYPE create_srv(ID3D12Device *self, ID3D12Resource *resource, const void *desc,
                                  Handle handle) {
    // The console views a multisampled texture as a plain one and a typeless one without a
    // format; the host removes the device over either
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    if (resource && desc) {
        std::memcpy(&view, desc, sizeof view);
        const auto of = resource->GetDesc();
        if (of.SampleDesc.Count > 1 && view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) {
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
            desc = &view;
        } else if (of.SampleDesc.Count > 1 &&
                   view.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2DARRAY) {
            const auto slices = view.Texture2DArray;
            view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMSARRAY;
            view.Texture2DMSArray = {slices.FirstArraySlice, slices.ArraySize};
            desc = &view;
        }
        if (view.Format == DXGI_FORMAT_UNKNOWN && of.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER &&
            typed_format(of.Format) != of.Format) {
            view.Format = typed_format(of.Format);
            desc = &view;
        }
    }
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, 18)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
}
void STDMETHODCALLTYPE create_uav(ID3D12Device *self, ID3D12Resource *resource,
                                  ID3D12Resource *counter, const void *desc, Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, void *, const void *, Handle)>(self, 19)(
        self, resource, counter, desc, host_handle(handle));
    mark_host(handle);
}
// Xbox One's CreatePlacedShaderResourceViewX (46) and CreatePlacedUnorderedAccessViewX (47):
// the console's descriptions begin as the host's do and go on with where the memory is and
// how the hardware reads it, which the host's resource already says
void STDMETHODCALLTYPE create_placed_srv(ID3D12Device *self, ID3D12Resource *resource,
                                         const void *desc, Handle handle) {
    if (!resource)
        return;
    // the number format after the host's fields and the memory's address: 9 is sRGB
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    if (desc) {
        std::memcpy(&view, desc, sizeof view);
        std::uint32_t number = 0;
        std::memcpy(&number, static_cast<const std::uint8_t *>(desc) + sizeof view + 12,
                    sizeof number);
        const auto of = resource->GetDesc();
        if (view.Format == DXGI_FORMAT_UNKNOWN && of.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
            view.Format = typed_format(of.Format, number == 9);
    }
    create_srv(self, resource, desc ? &view : nullptr, handle);
}
void STDMETHODCALLTYPE create_placed_uav(ID3D12Device *self, ID3D12Resource *resource,
                                         const void *desc, Handle handle) {
    if (resource)
        create_uav(self, resource, nullptr, desc, handle);
}
// GetDefaultMSAAParametersX (59): the standard pattern of the sample count, in sixteenths of
// a pixel from its centre, the same for the four pixels of a quad
HRESULT STDMETHODCALLTYPE default_msaa_parameters(void *, const DXGI_SAMPLE_DESC *samples,
                                                  std::int8_t (*locations)[16][2],
                                                  std::uint8_t *priorities,
                                                  std::uint32_t *parameters) {
    if (!samples || !std::has_single_bit(samples->Count) || samples->Count > 16)
        return E_INVALIDARG;
    static constexpr std::int8_t one[][2]{{0, 0}};
    static constexpr std::int8_t two[][2]{{4, 4}, {-4, -4}};
    static constexpr std::int8_t four[][2]{{-2, -6}, {6, -2}, {-6, 2}, {2, 6}};
    static constexpr std::int8_t eight[][2]{{1, -3}, {-1, 3},  {5, 1}, {-3, -5},
                                            {-5, 5}, {-7, -1}, {3, 7}, {7, -7}};
    static constexpr std::int8_t sixteen[][2]{
        {1, 1},  {-1, -3}, {-3, 2},  {4, -1}, {-5, -2}, {2, 5},  {5, 3}, {3, -5},
        {-2, 6}, {0, -7},  {-4, -6}, {-6, 4}, {-8, 0},  {7, -4}, {6, 7}, {-7, -8}};
    const std::int8_t(*const pattern)[2] = samples->Count == 1   ? one
                                           : samples->Count == 2 ? two
                                           : samples->Count == 4 ? four
                                           : samples->Count == 8 ? eight
                                                                 : sixteen;
    if (locations)
        for (int pixel = 0; pixel < 4; ++pixel) {
            std::memset(locations[pixel], 0, sizeof locations[pixel]);
            std::memcpy(locations[pixel], pattern, samples->Count * 2);
        }
    if (priorities)
        for (std::uint8_t i = 0; i < 16; ++i)
            priorities[i] = static_cast<std::uint8_t>(i % samples->Count);
    if (parameters) {
        // every sample is a colour sample the pixel shader sees
        const auto log2 = static_cast<std::uint32_t>(std::countr_zero(samples->Count));
        parameters[0] = log2 | 8u << 3 | log2 << 7;
        parameters[1] = log2 | log2 << 3 | log2 << 6 | log2 << 9;
    }
    return S_OK;
}
// the resource behind each render target view, for XV_D3D_PROBE
std::mutex target_mutex;
std::map<std::uint64_t, ID3D12Resource *> target_of;
std::atomic<ID3D12Resource *> last_target{nullptr};
std::vector<ID3D12Resource *> bound_targets; // the first of each size, under target_mutex
template <std::size_t Index>
void STDMETHODCALLTYPE create_target_view(ID3D12Device *self, ID3D12Resource *resource,
                                          const void *desc, Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, void *, const void *, Handle)>(self, Index)(
        self, resource, desc, host_handle(handle));
    mark_host(handle);
    if (Index == 20) {
        const std::lock_guard lock(target_mutex);
        target_of[host_handle(handle).ptr] = resource;
    }
}
void STDMETHODCALLTYPE create_sampler(ID3D12Device *self, const D3D12_SAMPLER_DESC *desc,
                                      Handle handle) {
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, desc, host_handle(handle));
    mark_host(handle);
}
// CreateSamplerX (61): a sampler as the console's GPU has it, with each filter on its own.
// What the host has no word for (the filter along Z, the second bias, the anisotropy bias
// and threshold, the performance settings) is left out.
struct ConsoleSampler {
    UINT flags, filter_mag, filter_min, filter_mip, filter_z, filter_mode;
    D3D12_TEXTURE_ADDRESS_MODE address_u, address_v, address_w;
    FLOAT min_lod, max_lod, lod_bias, lod_bias_secondary;
    UINT max_anisotropy;
    FLOAT anisotropy_bias;
    UINT anisotropy_threshold;
    D3D12_COMPARISON_FUNC comparison;
    FLOAT border[4];
};
void STDMETHODCALLTYPE create_sampler_x(ID3D12Device *self, const ConsoleSampler *desc,
                                        Handle handle) {
    if (!desc)
        return;
    // filters: 0 point, 1 bilinear, 2 and 3 the same with anisotropy; mips: 0 none, 1 point,
    // 2 linear; mode: 0 a blend, 1 the minimum, 2 the maximum
    const bool compares = desc->comparison > D3D12_COMPARISON_FUNC_NEVER &&
                          desc->comparison < D3D12_COMPARISON_FUNC_ALWAYS;
    const UINT reduction = compares                 ? 1
                           : desc->filter_mode == 1 ? 2
                           : desc->filter_mode == 2 ? 3
                                                    : 0;
    const bool anisotropic = desc->filter_mag >= 2 || desc->filter_min >= 2;
    const UINT basic = (desc->filter_min & 1) << 4 | (desc->filter_mag & 1) << 2 |
                       (desc->filter_mip == 2 ? 1u : 0u);
    const auto address = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
        return mode >= 1 && mode <= 5 ? mode : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    };
    D3D12_SAMPLER_DESC host{};
    host.Filter = static_cast<D3D12_FILTER>((anisotropic ? 0x55u : basic) | reduction << 7);
    host.AddressU = address(desc->address_u);
    host.AddressV = address(desc->address_v);
    host.AddressW = address(desc->address_w);
    host.MipLODBias = desc->lod_bias;
    host.MaxAnisotropy = anisotropic ? std::clamp(desc->max_anisotropy, 1u, 16u) : 1;
    host.ComparisonFunc = compares ? desc->comparison : D3D12_COMPARISON_FUNC_NEVER;
    std::copy(desc->border, desc->border + 4, host.BorderColor);
    host.MinLOD = desc->min_lod;
    host.MaxLOD = desc->filter_mip ? desc->max_lod : desc->min_lod;
    original<void(STDMETHODCALLTYPE *)(void *, const void *, Handle)>(self, 22)(
        self, &host, host_handle(handle));
    mark_host(handle);
}
// A copied descriptor: the host's goes along with the host's copy; one the title wrote goes
// along as the title wrote it, read where it is bound
void carry(Handle destination, Handle source, UINT stride) {
    host_view(source);
    const std::lock_guard lock(shadow_mutex);
    const auto *const heap = shadow_heap(destination.ptr);
    if (!heap)
        return;
    std::uint64_t first = host_mark;
    if (shadow_heap(source.ptr))
        std::memcpy(&first, reinterpret_cast<const void *>(source.ptr), sizeof first);
    if (first == host_mark || first == 0)
        set_mark(*heap, destination.ptr);
    else
        std::memcpy(reinterpret_cast<void *>(destination.ptr),
                    reinterpret_cast<const void *>(source.ptr), stride);
}
void STDMETHODCALLTYPE copy_descriptors(ID3D12Device *self, UINT destination_ranges,
                                        const Handle *destinations, const UINT *destination_sizes,
                                        UINT source_ranges, const Handle *sources,
                                        const UINT *source_sizes, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    std::vector<Handle> host_destinations(destinations, destinations + destination_ranges);
    std::vector<Handle> host_sources(sources, sources + source_ranges);
    const UINT stride = console_stride(type);
    // the descriptors one by one, in the order both lists of ranges give them
    UINT source_range = 0, source_at = 0;
    for (UINT range = 0; range < destination_ranges; ++range) {
        const UINT count = destination_sizes ? destination_sizes[range] : 1;
        for (UINT i = 0; i < count && source_range < source_ranges; ++i) {
            carry({destinations[range].ptr + std::size_t{i} * stride},
                  {sources[source_range].ptr + std::size_t{source_at} * stride}, stride);
            if (++source_at >= (source_sizes ? source_sizes[source_range] : 1))
                ++source_range, source_at = 0;
        }
        host_destinations[range] = host_handle(destinations[range]);
    }
    for (UINT range = 0; range < source_ranges; ++range)
        host_sources[range] = host_handle(sources[range]);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, const Handle *, const UINT *, UINT, const Handle *,
                                  const UINT *, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 23)(self, destination_ranges, host_destinations.data(), destination_sizes,
                             source_ranges, host_sources.data(), source_sizes, type);
}
void STDMETHODCALLTYPE copy_descriptors_simple(ID3D12Device *self, UINT count, Handle destination,
                                               Handle source, D3D12_DESCRIPTOR_HEAP_TYPE type) {
    const UINT stride = console_stride(type);
    for (UINT i = 0; i < count; ++i)
        carry({destination.ptr + std::size_t{i} * stride}, {source.ptr + std::size_t{i} * stride},
              stride);
    using Copy =
        void(STDMETHODCALLTYPE *)(void *, UINT, Handle, Handle, D3D12_DESCRIPTOR_HEAP_TYPE);
    original<Copy>(self, 24)(self, count, host_handle(destination), host_handle(source), type);
}
// CreateDescriptorHeap: the heap's CPU handles are the console's (shadow memory), its GPU
// handles the host's, returned in RAX (9, 10)
HRESULT STDMETHODCALLTYPE create_descriptor_heap(ID3D12Device *self,
                                                 const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID iid,
                                                 void **heap) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *, REFIID, void **);
    // The Series console's sampler heap without a bound (type 4) is a sampler heap here, as
    // large as the host makes one: 2048 samplers, which is more than titles keep
    D3D12_DESCRIPTOR_HEAP_DESC changed{};
    if (desc && static_cast<int>(desc->Type) == 4) {
        changed = *desc;
        changed.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        changed.NumDescriptors = std::min(changed.NumDescriptors, 2048u);
        desc = &changed;
    }
    // and a heap of target views is never one shaders see, whatever the title says of it
    if (desc &&
        (desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_RTV ||
         desc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_DSV) &&
        desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE) {
        changed = *desc;
        changed.Flags &= ~D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        desc = &changed;
    }
    const auto result = original<Create>(self, 14)(self, desc, iid, heap);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateDescriptorHeap type %d, %u descriptors, flags 0x%x, node "
                     "0x%x: 0x%08lx%c",
                     desc->Type, desc->NumDescriptors, desc->Flags, desc->NodeMask,
                     static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    if (FAILED(result) || !heap || !*heap || !desc)
        return result;
    auto *const host_heap_object = static_cast<ID3D12DescriptorHeap *>(*heap);
    const auto host = host_heap_object->GetCPUDescriptorHandleForHeapStart();
    const auto gpu = desc->Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                         ? host_heap_object->GetGPUDescriptorHandleForHeapStart().ptr
                         : 0;
    auto *const proxy = new HeapProxy{heap_vtable.data(), host_heap_object, 1};
    {
        const std::lock_guard lock(heap_proxy_mutex);
        heap_proxies.insert(proxy);
    }
    *heap = proxy;
    register_shadow(self, proxy, *desc, host, gpu);
    return result;
}
HRESULT STDMETHODCALLTYPE create_committed_resource(ID3D12Device *, const D3D12_HEAP_PROPERTIES *,
                                                    D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
                                                    D3D12_RESOURCE_STATES,
                                                    const D3D12_CLEAR_VALUE *, REFIID, void **);
void report_messages();
// CreateRootSignature (16): its descriptor tables are kept, to know what the descriptors the
// title writes are
// The root signatures the title made, newest last, each with a reference of ours and
// whether it allows an input layout (which tells one for drawing from one for computing)
struct TitleSignature {
    ID3D12RootSignature *signature;
    bool input_layout;
};
std::mutex title_signature_mutex;
std::vector<TitleSignature> title_signatures;
void remember_signature(ID3D12RootSignature *signature, const void *blob, SIZE_T size) {
    const auto *const bytes = static_cast<const std::uint8_t *>(blob);
    std::uint32_t flags = 0;
    if (size >= 32 && !std::memcmp(bytes, "DXBC", 4)) {
        std::uint32_t count = 0;
        std::memcpy(&count, bytes + 28, 4);
        for (std::uint32_t i = 0; i < count && 36 + 4 * i <= size; ++i) {
            std::uint32_t offset = 0;
            std::memcpy(&offset, bytes + 32 + 4 * i, 4);
            if (offset + 8 + 24 <= size && !std::memcmp(bytes + offset, "RTS0", 4))
                std::memcpy(&flags, bytes + offset + 8 + 20, 4);
        }
    }
    const std::lock_guard lock(title_signature_mutex);
    for (const auto &known : title_signatures)
        if (known.signature == signature)
            return;
    signature->AddRef();
    title_signatures.push_back(
        {signature, (flags & D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT) != 0});
}
HRESULT STDMETHODCALLTYPE create_root_signature(ID3D12Device *self, UINT node, const void *blob,
                                                SIZE_T size, REFIID iid, void **signature) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, UINT, const void *, SIZE_T, REFIID, void **);
    // A shader container says how long it is (the dword at 24). The host turns down a size
    // that differs from it; titles get away with a wrong one on the console (a sample passes
    // one shader with the size of another), so the container's own is what the host is given.
    std::uint32_t contained = 0;
    if (blob && size >= 32 && !std::memcmp(blob, "DXBC", 4)) {
        std::memcpy(&contained, static_cast<const std::uint8_t *>(blob) + 24, 4);
        MEMORY_BASIC_INFORMATION info{};
        const auto *const end = static_cast<const std::uint8_t *>(blob) + contained;
        if (contained >= 32 && contained != size && VirtualQuery(blob, &info, sizeof info) &&
            info.State == MEM_COMMIT &&
            end <= static_cast<const std::uint8_t *>(info.BaseAddress) + info.RegionSize)
            size = contained;
    }
    const auto result = original<Create>(self, 16)(self, node, blob, size, iid, signature);
    if (SUCCEEDED(result) && signature && *signature) {
        read_root_signature(*signature, blob, size);
        remember_signature(static_cast<ID3D12RootSignature *>(*signature), blob, size);
    }
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateRootSignature from %zu bytes: 0x%08lx\n",
                     static_cast<std::size_t>(size), static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// slot 44: CreatePlacedResourceX(address, desc, state, clear, iid, resource), a resource on
// memory of the title's (Unreal's XboxOneD3D12Texture.cpp names it). The host cannot place a
// resource at an address: a committed one in GPU memory stands for it.
HRESULT STDMETHODCALLTYPE create_placed_resource_x(ID3D12Device *self, std::uint64_t address,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    if (static std::atomic<int> shown{0};
        desc && std::getenv("XV_D3D_TRACE") &&
        (desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER || shown < 4) && shown++ < 40) {
        std::uint64_t first = 0;
        MEMORY_BASIC_INFORMATION info{};
        if (VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
            info.State == MEM_COMMIT)
            std::memcpy(&first, reinterpret_cast<const void *>(address), sizeof first);
        std::fprintf(stderr,
                     "[Runtime] CreatePlacedResourceX at %llx: dimension %d %llux%ux%u mips %u "
                     "format %d layout %d flags 0x%x state 0x%x, memory %s %llx\n",
                     static_cast<unsigned long long>(address), desc->Dimension, desc->Width,
                     desc->Height, desc->DepthOrArraySize, desc->MipLevels, desc->Format,
                     desc->Layout, desc->Flags, state,
                     info.State == MEM_COMMIT ? "committed" : "not committed",
                     static_cast<unsigned long long>(first));
        std::fflush(stderr);
    }
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const auto result = create_committed_resource(self, &heap, D3D12_HEAP_FLAG_NONE, desc, state,
                                                  clear, iid, resource);
    // a texture the title fills in its memory: the host's copy follows it
    if (SUCCEEDED(result) && resource && *resource && desc &&
        !(desc->Flags &
          (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)))
        track_placement(static_cast<ID3D12Resource *>(*resource),
                        reinterpret_cast<const void *>(address),
                        console == Target::scarlett ? desc : nullptr);
    return result;
}

// The 0x20004 Scarlett ABI also has an address-based resource creation method at slot 133.
// Its caller can leave the initial state unspecified (-1). Create the host buffer in COMMON
// in that case, so it can be promoted on first use.
HRESULT STDMETHODCALLTYPE create_placed_resource_later(ID3D12Device *self, std::uint64_t address,
                                                       const D3D12_RESOURCE_DESC *desc, UINT state,
                                                       const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                       void **resource) {
    const auto initial =
        state == ~UINT{0} ? D3D12_RESOURCE_STATE_COMMON : static_cast<D3D12_RESOURCE_STATES>(state);
    return create_placed_resource_x(self, address, desc, initial, clear, iid, resource);
}
// The console's GPU draws rectangle lists and quad lists (topology types 19 and 32, the
// topologies 17 and 19). Here both are triangles: a quad list through indices of ours (see
// draw_instanced), a rectangle list through a geometry shader of ours (see
// rectangle_pipeline).
constexpr D3D12_PRIMITIVE_TOPOLOGY_TYPE rectangle_type =
    static_cast<D3D12_PRIMITIVE_TOPOLOGY_TYPE>(19);
D3D12_PRIMITIVE_TOPOLOGY_TYPE host_topology_type(D3D12_PRIMITIVE_TOPOLOGY_TYPE type) {
    return type == 19 || type == 32 ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE : type;
}
// What a shader a pipeline was turned down with is made of: its container's parts by name
// and size. A console build may carry only code for the console's GPU. XV_SHADER_DUMP=<file>
// writes the first one there.
void describe_shader(const char *stage, const D3D12_SHADER_BYTECODE &code) {
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    if (!bytes || code.BytecodeLength < 32)
        return;
    std::string parts;
    std::uint32_t count = 0;
    if (!std::memcmp(bytes, "DXBC", 4)) {
        std::memcpy(&count, bytes + 28, 4);
        for (std::uint32_t i = 0; i < std::min(count, 32u); ++i) {
            std::uint32_t offset = 0, size = 0;
            std::memcpy(&offset, bytes + 32 + 4 * i, 4);
            if (offset + 8 > code.BytecodeLength)
                break;
            std::memcpy(&size, bytes + offset + 4, 4);
            parts += ' ' + std::string(reinterpret_cast<const char *>(bytes + offset), 4) + ':' +
                     std::to_string(size);
        }
    }
    std::fprintf(stderr, "[Runtime]   %s shader, %zu bytes, starts %.4s, parts:%s\n", stage,
                 static_cast<std::size_t>(code.BytecodeLength), bytes, parts.c_str());
    if (static std::atomic<bool> dumped{false};
        std::getenv("XV_SHADER_DUMP") && !dumped.exchange(true))
        if (auto *const file = std::fopen(std::getenv("XV_SHADER_DUMP"), "wb")) {
            std::fwrite(bytes, 1, code.BytecodeLength, file);
            std::fclose(file);
        }
}
// A shader of the title's with only console GPU code is given to the host as the shader
// made for it (see console_shader.hpp)
std::span<const std::uint8_t> shader_bytes(const D3D12_SHADER_BYTECODE &code) {
    if (!code.pShaderBytecode)
        return {};
    return {static_cast<const std::uint8_t *>(code.pShaderBytecode), code.BytecodeLength};
}
bool host_bytecode(modules::ShaderStage stage, D3D12_SHADER_BYTECODE &code,
                   modules::HostShaderModel model = modules::HostShaderModel::sm5) {
    const auto container = shader_bytes(code);
    if (!code.pShaderBytecode || !modules::console_only_shader(container))
        return false;
    const auto made = modules::host_shader(stage, container, model);
    if (made.empty())
        return false;
    code = {made.data(), made.size()};
    return true;
}

bool host_graphics_pair(ID3D12Device *device, D3D12_SHADER_BYTECODE &vs, D3D12_SHADER_BYTECODE &ps,
                        bool allow_sm6) {
    using modules::HostShaderModel;
    using modules::ShaderStage;
    if (allow_sm6 && vs.pShaderBytecode && ps.pShaderBytecode) {
        auto pixel = ps;
        if (host_bytecode(ShaderStage::pixel, pixel, HostShaderModel::automatic)) {
            if (!modules::shader_has_dxil(shader_bytes(pixel))) {
                ps = pixel;
                host_bytecode(ShaderStage::vertex, vs);
                return true;
            }
            // A graphics pipeline cannot mix DXBC and DXIL on all supported drivers.
            D3D12_FEATURE_DATA_D3D12_OPTIONS1 options{};
            D3D12_FEATURE_DATA_SHADER_MODEL model{D3D_SHADER_MODEL_6_6};
            auto vertex = vs;
            if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1, &options,
                                                      sizeof options)) &&
                options.WaveOps &&
                SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL, &model,
                                                      sizeof model)) &&
                model.HighestShaderModel >= D3D_SHADER_MODEL_6_6 &&
                (modules::shader_has_dxil(shader_bytes(vertex)) ||
                 host_bytecode(ShaderStage::vertex, vertex, HostShaderModel::sm6))) {
                vs = vertex;
                ps = pixel;
                return true;
            }
        }
    }
    const bool vertex = host_bytecode(ShaderStage::vertex, vs);
    const bool pixel = host_bytecode(ShaderStage::pixel, ps);
    return vertex || pixel;
}
// A pipeline without a root signature has it in its shaders, and the shader made for the
// host does not carry it. On the host a pipeline is bound to the root signature it was made
// with, and the title sets its own on its lists: the pipeline gets the newest one the title
// made of its kind (for drawing or for computing), or failing that the one in the shader.
ID3D12RootSignature *shader_root_signature(ID3D12Device *self, bool drawing,
                                           std::initializer_list<D3D12_SHADER_BYTECODE> shaders) {
    {
        const std::lock_guard lock(title_signature_mutex);
        for (auto known = title_signatures.rbegin(); known != title_signatures.rend(); ++known)
            if (known->input_layout == drawing) {
                known->signature->AddRef();
                return known->signature;
            }
    }
    for (const auto &shader : shaders) {
        ID3D12RootSignature *signature = nullptr;
        if (shader.pShaderBytecode &&
            SUCCEEDED(self->CreateRootSignature(0, shader.pShaderBytecode, shader.BytecodeLength,
                                                IID_PPV_ARGS(&signature))))
            return signature;
    }
    return nullptr;
}
// What each graphics pipeline was made from, as the host was given it: the console derives
// a pipeline from another with some of its state changed, and the host makes one whole.
// Shaders are kept once each, however many pipelines use them.
struct KeptPipeline {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::deque<std::string> names;
};
std::mutex kept_mutex;
std::map<const void *, KeptPipeline> kept_pipelines;
std::map<std::uint64_t, std::vector<std::uint8_t>> kept_shaders;
D3D12_SHADER_BYTECODE keep_shader(const D3D12_SHADER_BYTECODE &code) {
    if (!code.pShaderBytecode || !code.BytecodeLength)
        return {};
    const auto *const bytes = static_cast<const std::uint8_t *>(code.pShaderBytecode);
    std::uint64_t hash = 0xcbf29ce484222325ull ^ code.BytecodeLength;
    for (SIZE_T i = 0; i < code.BytecodeLength; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    auto &kept = kept_shaders[hash];
    if (kept.empty())
        kept.assign(bytes, bytes + code.BytecodeLength);
    return {kept.data(), kept.size()};
}
void keep_pipeline(const void *pipeline, const D3D12_GRAPHICS_PIPELINE_STATE_DESC &desc) {
    const std::lock_guard lock(kept_mutex);
    auto &kept = kept_pipelines[pipeline];
    kept = {};
    kept.desc = desc;
    for (auto *const shader :
         {&kept.desc.VS, &kept.desc.PS, &kept.desc.DS, &kept.desc.HS, &kept.desc.GS})
        *shader = keep_shader(*shader);
    kept.desc.StreamOutput = {};
    kept.desc.CachedPSO = {};
    for (UINT i = 0; i < desc.InputLayout.NumElements && desc.InputLayout.pInputElementDescs; ++i) {
        auto element = desc.InputLayout.pInputElementDescs[i];
        element.SemanticName = kept.names.emplace_back(element.SemanticName).c_str();
        kept.elements.push_back(element);
    }
    kept.desc.InputLayout.pInputElementDescs = kept.elements.data();
    if (kept.desc.pRootSignature)
        kept.desc.pRootSignature->AddRef(); // for as long as it may be derived from
}
// CreateDerivedGraphicsPipelineStateX (55)
struct DerivedState {
    UINT type; // 0 depth and stencil, 1 rasterizer, 2 blend, 3 sample mask, 4 and up: what
               // only the console's GPU has a use for
    union {
        D3D12_DEPTH_STENCIL_DESC depth_stencil;
        D3D12_RASTERIZER_DESC rasterizer;
        D3D12_BLEND_DESC blend;
        UINT sample_mask;
    };
};
HRESULT STDMETHODCALLTYPE create_derived_graphics_pipeline(ID3D12Device *self,
                                                           ID3D12PipelineState *source, UINT count,
                                                           const DerivedState *states, REFIID iid,
                                                           void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    if (!source || !state || (count && !states))
        return E_INVALIDARG;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc;
    {
        const std::lock_guard lock(kept_mutex);
        const auto found = kept_pipelines.find(source);
        if (found == kept_pipelines.end()) {
            std::fprintf(stderr, "[Runtime] CreateDerivedGraphicsPipelineStateX from a pipeline "
                                 "that was not made here\n");
            return E_INVALIDARG;
        }
        desc = found->second.desc;
    }
    for (UINT i = 0; i < count; ++i)
        if (states[i].type == 0)
            desc.DepthStencilState = states[i].depth_stencil;
        else if (states[i].type == 1)
            desc.RasterizerState = states[i].rasterizer;
        else if (states[i].type == 2)
            desc.BlendState = states[i].blend;
        else if (states[i].type == 3)
            desc.SampleMask = states[i].sample_mask;
    const auto result = original<Create>(self, 10)(self, &desc, iid, state);
    if (SUCCEEDED(result) && *state)
        keep_pipeline(*state, desc);
    else {
        std::fprintf(stderr, "[Runtime] CreateDerivedGraphicsPipelineStateX: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// A pipeline that draws rectangle lists gets the geometry shader that makes the fourth
// corner of each, unless it has one of the title's. A rectangle faces no way: it is not
// culled. Without the shader the triangle of the three corners is drawn, half the rectangle.
bool rectangle_pipeline(D3D12_GRAPHICS_PIPELINE_STATE_DESC &host) {
    if (!host.GS.pShaderBytecode && host.VS.pShaderBytecode) {
        const auto corner = modules::rectangle_shader(
            {static_cast<const std::uint8_t *>(host.VS.pShaderBytecode), host.VS.BytecodeLength});
        if (!corner.empty()) {
            host.GS = {corner.data(), corner.size()};
            host.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            return true;
        }
    }
    return false;
}
void report_half_rectangles() {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a rectangle list is drawn as the triangles of its three "
                             "corners: half of each rectangle is missing\n");
        std::fflush(stderr);
    }
}
// CreateGraphicsPipelineState (10): the console's flags, as for compute pipelines
HRESULT STDMETHODCALLTYPE create_graphics_pipeline(ID3D12Device *self,
                                                   const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                   REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC host{};
    ID3D12RootSignature *from_shader = nullptr;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        // The console's R10G10B10_SNORM_A2_UNORM (189), what packed normals are in, is no
        // vertex format of the host's: its bits are read unsigned there
        if (host.InputLayout.pInputElementDescs) {
            elements.assign(host.InputLayout.pInputElementDescs,
                            host.InputLayout.pInputElementDescs + host.InputLayout.NumElements);
            for (auto &element : elements)
                if (element.Format == static_cast<DXGI_FORMAT>(189))
                    element.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            host.InputLayout.pInputElementDescs = elements.data();
        }
        host.PrimitiveTopologyType = host_topology_type(host.PrimitiveTopologyType);
        const auto geometry = host.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT
                                  ? modules::ShaderStage::geometry_points
                              : host.PrimitiveTopologyType == D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE
                                  ? modules::ShaderStage::geometry_lines
                                  : modules::ShaderStage::geometry_triangles;
        const bool simple = desc->PrimitiveTopologyType != rectangle_type &&
                            !host.GS.pShaderBytecode && !host.HS.pShaderBytecode &&
                            !host.DS.pShaderBytecode;
        const bool pair = host_graphics_pair(self, host.VS, host.PS, simple);
        const bool made = host_bytecode(geometry, host.GS) || pair;
        if (made && !host.pRootSignature)
            host.pRootSignature = from_shader =
                shader_root_signature(self, true, {desc->VS, desc->PS, desc->GS});
    }
    const bool rectangles = desc && desc->PrimitiveTopologyType == rectangle_type;
    const auto cull = host.RasterizerState.CullMode;
    const bool cornered = rectangles && rectangle_pipeline(host);
    auto result = original<Create>(self, 10)(self, desc ? &host : nullptr, iid, state);
    // the host's compiler may lay the shader's inputs out another way than the title's
    // vertex shader its outputs: the pipeline is then made without it
    if (FAILED(result) && cornered) {
        host.GS = {};
        host.RasterizerState.CullMode = cull;
        result = original<Create>(self, 10)(self, &host, iid, state);
    }
    if (rectangles && (!cornered || !host.GS.pShaderBytecode))
        report_half_rectangles();
    if (SUCCEEDED(result) && desc && state && *state && console == Target::scarlett)
        keep_pipeline(*state, host);
    if (from_shader)
        from_shader->Release();
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreateGraphicsPipelineState: 0x%08lx\n",
                     static_cast<unsigned long>(result));
        // the device went before this: what the host knows of why
        if (static std::atomic<bool> told{false};
            result == DXGI_ERROR_DEVICE_REMOVED && !told.exchange(true))
            report_failure();
        if (static std::atomic<int> shown{0}; desc && shown++ < 2) {
            describe_shader("vertex", desc->VS);
            describe_shader("pixel", desc->PS);
            const auto bytes = [](const D3D12_SHADER_BYTECODE &code) {
                return std::span<const std::uint8_t>(
                    static_cast<const std::uint8_t *>(code.pShaderBytecode), code.BytecodeLength);
            };
            std::fprintf(stderr, "[Runtime]   vertex %s\n[Runtime]   pixel %s\n",
                         modules::shader_signature(bytes(desc->VS)).c_str(),
                         modules::shader_signature(bytes(desc->PS)).c_str());
            if (desc->GS.pShaderBytecode)
                std::fprintf(stderr, "[Runtime]   geometry %s\n",
                             modules::shader_signature(bytes(desc->GS)).c_str());
            std::fprintf(stderr,
                         "[Runtime]   topology %d, %u targets of format %d, depth %d, "
                         "samples %u, layout:",
                         host.PrimitiveTopologyType, host.NumRenderTargets, host.RTVFormats[0],
                         host.DSVFormat, host.SampleDesc.Count);
            for (UINT i = 0; i < host.InputLayout.NumElements; ++i)
                std::fprintf(stderr, " %s%u/%d",
                             host.InputLayout.pInputElementDescs[i].SemanticName,
                             host.InputLayout.pInputElementDescs[i].SemanticIndex,
                             host.InputLayout.pInputElementDescs[i].Format);
            std::fprintf(stderr, "\n");
            describe_shader("geometry", desc->GS);
            describe_shader("hull", desc->HS);
            describe_shader("domain", desc->DS);
        }
        report_messages();
    }
    return result;
}
// slot 74: RegisterPagePoolX(address, page count, handle): the pages are the title's memory,
// mapped for the GPU by later page mapping calls; here the pool is only a handle
HRESULT STDMETHODCALLTYPE register_page_pool(ID3D12Device *, std::uint64_t address, UINT,
                                             HANDLE *handle) {
    if (!handle)
        return E_POINTER;
    *handle = reinterpret_cast<HANDLE>(address);
    return S_OK;
}
// CreateComputePipelineState (11)
HRESULT STDMETHODCALLTYPE create_compute_pipeline(ID3D12Device *self,
                                                  const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                  REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMPUTE_PIPELINE_STATE_DESC *,
                                                REFIID, void **);
    // the console's pipeline flags mean nothing to the host, which takes none of them
    D3D12_COMPUTE_PIPELINE_STATE_DESC host{};
    ID3D12RootSignature *from_shader = nullptr;
    if (desc) {
        host = *desc;
        host.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
        if (host_bytecode(modules::ShaderStage::compute, host.CS) && !host.pRootSignature)
            host.pRootSignature = from_shader = shader_root_signature(self, false, {desc->CS});
    }
    const auto result = original<Create>(self, 11)(self, desc ? &host : nullptr, iid, state);
    if (from_shader)
        from_shader->Release();
    if (FAILED(result) && desc) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->CS.pShaderBytecode);
        std::fprintf(stderr,
                     "[Runtime] CreateComputePipelineState: 0x%08lx, shader of %zu bytes "
                     "starting %02x %02x %02x %02x %02x %02x %02x %02x\n",
                     static_cast<unsigned long>(result), desc->CS.BytecodeLength,
                     bytes ? bytes[0] : 0, bytes ? bytes[1] : 0, bytes ? bytes[2] : 0,
                     bytes ? bytes[3] : 0, bytes ? bytes[4] : 0, bytes ? bytes[5] : 0,
                     bytes ? bytes[6] : 0, bytes ? bytes[7] : 0);
        // DXBC: magic, digest, version, size, chunk count, chunk offsets; each chunk a tag
        if (bytes && desc->CS.BytecodeLength >= 32) {
            std::uint32_t chunks = 0;
            std::memcpy(&chunks, bytes + 28, 4);
            std::string tags;
            for (std::uint32_t i = 0; i < chunks && 32 + i * 4 + 4 <= desc->CS.BytecodeLength;
                 ++i) {
                std::uint32_t offset = 0;
                std::memcpy(&offset, bytes + 32 + i * 4, 4);
                if (offset + 8 <= desc->CS.BytecodeLength) {
                    std::uint32_t size = 0;
                    std::memcpy(&size, bytes + offset + 4, 4);
                    tags += ' ';
                    tags.append(reinterpret_cast<const char *>(bytes + offset), 4);
                    tags += ':' + std::to_string(size);
                }
            }
            std::fprintf(stderr, "[Runtime] its chunks:%s\n", tags.c_str());
        }
        // XV_D3D_TRACE: the first shaders the host turned down, to look at
        if (static std::atomic<int> dumped{0};
            bytes && std::getenv("XV_D3D_TRACE") && std::getenv("TEMP") && dumped < 4)
            if (auto *const file = std::fopen((std::string(std::getenv("TEMP")) + "\\xv_shader_" +
                                               std::to_string(dumped++) + ".bin")
                                                  .c_str(),
                                              "wb")) {
                std::fwrite(bytes, 1, desc->CS.BytecodeLength, file);
                std::fclose(file);
            }
        std::fflush(stderr);
        report_messages();
    }
    return result;
}
// ID3D12CommandQueue goes on past the desktop's 19 methods with the console's own
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_method(void *, std::uint64_t a, std::uint64_t b,
                                                     std::uint64_t c) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx, %llx) from %s\n",
                     Index, a, b, c, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists);
HRESULT STDMETHODCALLTYPE ignored_queue_method(void *) { return S_OK; }
// slot 23: SignalX(fence, value, flush flags), by Unreal's message; the host's caches need no
// flushing asked for
HRESULT STDMETHODCALLTYPE signal_x(ID3D12CommandQueue *self, ID3D12Fence *fence, UINT64 value,
                                   UINT) {
    return self->Signal(fence, value);
}
constexpr std::size_t host_queue_slots = 19, queue_slots = 48;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_queue_methods(std::index_sequence<Index...>) {
    return {{host_queue_slots + Index,
             reinterpret_cast<Slot>(&unknown_queue_method<host_queue_slots + Index>)}...};
}
// slot 62: CreateComputePipelineState as the console has it (Unreal's message names it): two
// more arguments, 0 at its call site, before the interface and the state
HRESULT STDMETHODCALLTYPE create_compute_pipeline_x(ID3D12Device *self,
                                                    const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
                                                    void *, void *, REFIID iid, void **state) {
    return create_compute_pipeline(self, desc, iid, state);
}
// The title's command queue: a proxy of the host's, which the host's runtime and DXGI keep as
// their own. The console's queue has the desktop's methods to Wait (15), then PIX captures,
// SuspendX, ResumeX, SignalX (23), ..., GetDesc (26) and its page mapping methods.
struct QueueProxy {
    Slot *vtable;
    ID3D12CommandQueue *host;
    std::atomic<ULONG> references;
};
ID3D12CommandQueue *queue_host(void *self) { return static_cast<QueueProxy *>(self)->host; }
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE queue_method(void *self, std::uint64_t a, std::uint64_t b,
                                             std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                             std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                             std::uint64_t i, std::uint64_t j) {
    auto *const host = queue_host(self);
    return reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[Index])(host, a, b, c, d, e, f,
                                                                            g, h, i, j);
}
HRESULT STDMETHODCALLTYPE queue_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    if (is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12Pageable, ID3D12CommandQueue>(
            iid)) {
        ++static_cast<QueueProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return queue_host(self)->QueryInterface(iid, object);
}
ULONG STDMETHODCALLTYPE queue_add_ref(void *self) {
    return ++static_cast<QueueProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE queue_release(void *self) {
    auto *const proxy = static_cast<QueueProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        proxy->host->Release();
        delete proxy;
    }
    return left;
}
void STDMETHODCALLTYPE queue_execute(void *self, UINT count, ID3D12CommandList *const *lists) {
    execute_command_lists(queue_host(self), count, lists);
    // what the debug layer has to say of the draws, which no call's result shows
    report_messages();
}
HRESULT STDMETHODCALLTYPE queue_signal_x(void *self, ID3D12Fence *fence, UINT64 value, UINT) {
    return queue_host(self)->Signal(fence, value);
}
D3D12_COMMAND_QUEUE_DESC *STDMETHODCALLTYPE queue_get_desc(void *self,
                                                           D3D12_COMMAND_QUEUE_DESC *desc) {
    using Get = D3D12_COMMAND_QUEUE_DESC *(STDMETHODCALLTYPE *)(void *, D3D12_COMMAND_QUEUE_DESC *);
    auto *const host = queue_host(self);
    return reinterpret_cast<Get>((*reinterpret_cast<Slot **>(host))[18])(host, desc);
}
// slot 30: PresentX. The console shows the title's own resources; there is no swap chain.
HRESULT STDMETHODCALLTYPE queue_present_x(void *self, UINT planes, const void *plane_parameters,
                                          const void *present_parameters) {
    return present_x(queue_host(self), planes, plane_parameters, present_parameters);
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE unknown_queue_slot(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: command queue slot %zu (%llx, "
                     "%llx) from %s\n",
                     Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
template <std::size_t... Index>
std::array<Slot, 48> queue_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, 48> vtable{(Index < 18
                                     ? reinterpret_cast<Slot>(&queue_method<Index>)
                                     : reinterpret_cast<Slot>(&unknown_queue_slot<Index>))...};
    vtable[0] = reinterpret_cast<Slot>(&queue_query);
    vtable[1] = reinterpret_cast<Slot>(&queue_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&queue_release);
    vtable[10] = reinterpret_cast<Slot>(&queue_execute);
    for (const std::size_t ignored : {18, 19, 20, 21, 22, 24, 25})
        vtable[ignored] = reinterpret_cast<Slot>(&ignored_queue_method);
    vtable[23] = reinterpret_cast<Slot>(&queue_signal_x);
    vtable[26] = reinterpret_cast<Slot>(&queue_get_desc);
    vtable[30] = reinterpret_cast<Slot>(&queue_present_x);
    // SetHDRToneMapperX: the picture goes to the host's display as the title made it
    vtable[31] = reinterpret_cast<Slot>(&ignored_queue_method);
    return vtable;
}
std::array<Slot, 48> queue_vtable = queue_vtable_of(std::make_index_sequence<48>{});
// Titles built with later headers have two more methods before Signal: it is at 16 there
// and PresentX at 32. Which two is not known; they lie among the event markers, which do
// nothing here. Neither the version a title creates its device with nor its system DLLs
// tell the two layouts apart, so the calls do: Signal and Wait are given a fence, which
// nothing else at their places is, and only the later layout has a method at 32.
std::atomic<bool> later_queue{false};
bool is_object(std::uint64_t address) {
    MEMORY_BASIC_INFORMATION region{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &region, sizeof region) ||
        region.State != MEM_COMMIT || (region.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return false;
    const auto vtable = *reinterpret_cast<const void *const *>(address);
    return vtable && VirtualQuery(vtable, &region, sizeof region) && region.State == MEM_COMMIT &&
           region.Type == MEM_IMAGE;
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE queue_by_layout(void *self, std::uint64_t a, std::uint64_t b,
                                                std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                                std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                                std::uint64_t i, std::uint64_t j) {
    const bool fence = (Index >= 14 && Index <= 17) && is_object(a);
    if (Index >= 32 || (Index >= 16 && fence))
        later_queue = true;
    std::size_t slot = Index;
    if (later_queue) {
        if (Index < 16)
            return 0; // markers, and the two methods that are not known
        slot = Index - 2;
    } else if (Index < 16 && !fence) {
        return 0; // a marker of a later title that has not signalled yet
    }
    return reinterpret_cast<Raw>(queue_vtable[slot])(self, a, b, c, d, e, f, g, h, i, j);
}
template <std::size_t... Index>
std::array<Slot, 48> layout_queue_vtable_of(std::index_sequence<Index...>) {
    auto vtable = queue_vtable;
    ((vtable[14 + Index] = reinterpret_cast<Slot>(&queue_by_layout<14 + Index>)), ...);
    return vtable;
}
std::array<Slot, 48> layout_queue_vtable = layout_queue_vtable_of(std::make_index_sequence<34>{});
void *make_queue_proxy(ID3D12CommandQueue *host) {
    return new QueueProxy{layout_queue_vtable.data(), host, 1};
}
// slot 48: CreateCommandQueue(desc, iid, queue), by its arguments at Unreal's call site
HRESULT STDMETHODCALLTYPE create_command_queue(ID3D12Device *self,
                                               const D3D12_COMMAND_QUEUE_DESC *desc, REFIID iid,
                                               void **queue) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_QUEUE_DESC *, REFIID, void **);
    const auto result = original<Create>(self, 8)(self, desc, iid, queue);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommandQueue type %d priority %d flags 0x%x node 0x%x: "
                     "0x%08lx%c",
                     desc->Type, desc->Priority, desc->Flags, desc->NodeMask,
                     static_cast<unsigned long>(result), 10);
        std::fflush(stderr);
    }
    auto *const host =
        SUCCEEDED(result) && queue ? static_cast<ID3D12CommandQueue *>(*queue) : nullptr;
    if (host)
        *queue = make_queue_proxy(host);
    if (SUCCEEDED(result) && queue && *queue && desc &&
        desc->Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
        ID3D12CommandQueue *expected = nullptr;
        if (first_direct_queue.compare_exchange_strong(expected, host))
            first_direct_queue.load()->AddRef();
    }
    return result;
}
// CreateCommandQueueX (48): the type, the flags, and which engine and queue of the hardware.
// The copy engine's queue (type 16) is a direct queue here, whose lists take any transition.
HRESULT STDMETHODCALLTYPE create_command_queue_x(ID3D12Device *self, const UINT *desc, REFIID iid,
                                                 void **queue) {
    if (!desc)
        return E_INVALIDARG;
    const D3D12_COMMAND_QUEUE_DESC host{desc[0] == 16
                                            ? D3D12_COMMAND_LIST_TYPE_DIRECT
                                            : static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]),
                                        0, static_cast<D3D12_COMMAND_QUEUE_FLAGS>(desc[1] & 1), 0};
    return create_command_queue(self, &host, iid, queue);
}
// What the host insists on where the console takes anything: a resource of an upload heap
// starts as one to read from and one of a readback heap as one to copy to, and only a depth
// buffer is kept from shaders.
void host_rules(const D3D12_HEAP_PROPERTIES *heap, D3D12_RESOURCE_FLAGS &flags,
                D3D12_RESOURCE_STATES &state) {
    // 0x100000, which later kits give a display target and the host has no state for
    state &= ~static_cast<D3D12_RESOURCE_STATES>(0x100000);
    if (heap && heap->Type == D3D12_HEAP_TYPE_UPLOAD)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    else if (heap && heap->Type == D3D12_HEAP_TYPE_READBACK)
        state = D3D12_RESOURCE_STATE_COPY_DEST;
    if (!(flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
        flags &= ~D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE;
}
// CreateCommittedResource, reported when the host turns it down
HRESULT STDMETHODCALLTYPE create_committed_resource(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, REFIID, void **);
    // the console's D3D12_RESOURCE_STATE_GENERIC_READ (0x8c3) has no indirect argument bit, the
    // desktop's (0xac3), which upload heaps require, does
    constexpr auto console_generic_read = static_cast<D3D12_RESOURCE_STATES>(0x8c3);
    if (state == console_generic_read)
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    // resource flags above the desktop's (0x100000, 0x200000 on Unreal's render targets) are
    // the console's own
    D3D12_RESOURCE_DESC host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        // a console layout names a tile mode (0x100 | mode): the host picks its own
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        // the console takes any alignment for a buffer (a title asks for 64 bytes); the
        // host only its own
        // and a constant buffer view of 256 bytes on a smaller buffer, which the host drops
        // the device for: the buffer is made to hold it
        if (host_desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            host_desc.Alignment = 0;
            host_desc.Width = (host_desc.Width + 255) & ~std::uint64_t{255};
        }
        host_rules(heap, host_desc.Flags, state);
    }
    // All of the console's memory is the CPU's as well: a title maps a buffer of the default
    // heap and writes to it. The host only maps what is in a heap the CPU can reach, so such
    // a buffer goes in one (unless it is written by the GPU, which those heaps do not take).
    D3D12_HEAP_PROPERTIES mappable{};
    const auto *const asked = heap;
    if (console == Target::scarlett && heap && desc && heap->Type == D3D12_HEAP_TYPE_DEFAULT &&
        desc->Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
        mappable = {D3D12_HEAP_TYPE_CUSTOM, D3D12_CPU_PAGE_PROPERTY_WRITE_BACK,
                    D3D12_MEMORY_POOL_L0, heap->CreationNodeMask, heap->VisibleNodeMask};
        heap = &mappable;
    }
    if (static std::atomic<int> shown{0};
        std::getenv("XV_D3D_TRACE") && heap && desc && shown++ < 40)
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource heap %d flags 0x%x dimension %d %llux%u "
                     "format %d flags 0x%x state 0x%x\n",
                     heap->Type, flags, desc->Dimension, desc->Width, desc->Height, desc->Format,
                     desc->Flags, state);
    auto result = original<Create>(self, 27)(self, heap, flags, desc ? &host_desc : nullptr, state,
                                             clear, iid, resource);
    // a buffer the host does not make there is made where the title asked
    if (FAILED(result) && heap != asked) {
        heap = asked;
        result =
            original<Create>(self, 27)(self, heap, flags, &host_desc, state, clear, iid, resource);
    }
    if (SUCCEEDED(result) && resource && *resource) {
        console_resource(*resource);
        track_buffer(static_cast<ID3D12Resource *>(*resource), console == Target::scarlett);
        if (console == Target::scarlett)
            mappable_resource(*resource);
    }
    if (FAILED(result) && heap && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource heap %d/%d/%d flags 0x%x dimension %d "
                     "alignment %llu %llux%ux%u mips %u format %d samples %u layout %d flags "
                     "0x%x state 0x%x: 0x%08lx\n",
                     heap->Type, heap->CPUPageProperty, heap->MemoryPoolPreference, flags,
                     desc->Dimension, desc->Alignment, desc->Width, desc->Height,
                     desc->DepthOrArraySize, desc->MipLevels, desc->Format, desc->SampleDesc.Count,
                     desc->Layout, desc->Flags, state, static_cast<unsigned long>(result));
        std::fflush(stderr);
    }
    return result;
}
// XV_D3D_DEBUG: the host's debug layer, whose messages go to the log when something the title
// asked for is turned down
std::atomic<ID3D12InfoQueue *> debug_messages{nullptr};
void report_messages() {
    auto *const queue = debug_messages.load();
    if (!queue)
        return;
    for (UINT64 i = 0, count = queue->GetNumStoredMessages(); i < count; ++i) {
        SIZE_T size = 0;
        queue->GetMessage(i, nullptr, &size);
        std::vector<std::uint8_t> buffer(size);
        auto *const message = reinterpret_cast<D3D12_MESSAGE *>(buffer.data());
        if (SUCCEEDED(queue->GetMessage(i, message, &size)) &&
            message->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            std::fprintf(stderr, "[Runtime] Direct3D 12 debug layer: %.*s\n",
                         static_cast<int>(message->DescriptionByteLength), message->pDescription);
    }
    queue->ClearStoredMessages();
    std::fflush(stderr);
}

// ID3D12GraphicsCommandList as the console has it. The title reads the object itself: a
// write pointer (at +0x38 on Xbox One, +0x18 on Xbox Series) and its limits after it, between
// which it records commands inline when there is room. The title gets a proxy of the
// console's size whose inline space is always full, so that every command comes through a
// method, and the proxy hands those to the host's list. On Xbox Series the inline
// SetPipelineState also copies 32 bytes of the pipeline state into the list at +0x540, before
// it looks at the write pointer: the proxy has the room, 0x560 bytes being the list's size
// there.
struct ListProxy {
    Slot *vtable;
    std::uint8_t fields[0x600 - sizeof(Slot *)];
    ID3D12GraphicsCommandList *host;
    std::atomic<ULONG> references;
    ID3D12Resource *target = nullptr; // bound last, for XV_D3D_PROBE's draw counts
    const void *graphics_root = nullptr, *compute_root = nullptr;
    ID3D12DescriptorHeap *heaps[2]{};  // the host's, as bound: views, then samplers
    UINT topology = 0;                 // as the title set it, the console's own included
    D3D12_INDEX_BUFFER_VIEW indices{}; // the title's, to put back after a quad draw
};
std::mutex proxy_mutex;
std::set<const void *> list_proxies;
// the host's list for a title's one, which may be either
ID3D12CommandList *host_list(void *list) {
    const std::lock_guard lock(proxy_mutex);
    return list_proxies.contains(list) ? static_cast<ListProxy *>(list)->host
                                       : static_cast<ID3D12CommandList *>(list);
}
using List = ID3D12GraphicsCommandList;
List *host_of(void *self) { return static_cast<ListProxy *>(self)->host; }

// Each console slot calls this host slot, the arguments going on as they came (integer
// registers and stack). The console has the desktop's methods in another order: the queries
// (BeginQuery, EndQuery, ResolveQueryData, SetPredication; the desktop's 52 to 55) come right
// after ExecuteBundle, at 28 to 31, and push SetDescriptorHeaps to ClearUnorderedAccessView-
// Float 4 slots later; from SetMarker (56) on they agree again. Unreal's call sites show it:
// Close, Reset, CopyBufferRegion and ResourceBarrier in place, SetDescriptorHeaps at 32, its
// timestamps' EndQuery and ResolveQueryData at 29 and 30.
//
// Past the first interface the two consoles differ. Xbox One has ID3D12GraphicsCommandList1
// and 2 at 104 to 110. Xbox Series has two more methods of its own first (to 104), then
// those at 105 to 111 and ID3D12GraphicsCommandList3 to 6 at 112 to 128, with four console
// methods among them (122, 123, 126, 128). The host has all of them from 60 on, in the
// desktop's order.
constexpr std::size_t host_list_slots = 60, list_slots = 136, unmapped = ~std::size_t{0};
std::array<std::size_t, list_slots> list_map{};
void map_list_slots() {
    for (std::size_t slot = 0; slot < list_slots; ++slot)
        list_map[slot] = slot <= 27               ? slot
                         : slot < 32              ? slot + 24
                         : slot < 56              ? slot - 4
                         : slot < host_list_slots ? slot
                                                  : unmapped;
    // CopyTextureRegion with one more argument, which the host's has not
    list_map[83] = 16;
    // DispatchX and CopyResourceX likewise: Dispatch and CopyResource, and flags
    list_map[78] = 14;
    list_map[84] = 17;
    if (console == Target::durango) {
        for (std::size_t slot = 104; slot <= 110; ++slot)
            list_map[slot] = slot - 44;
        return;
    }
    for (std::size_t slot = 105; slot <= 121; ++slot)
        list_map[slot] = slot - 45;
    list_map[124] = 77; // RSSetShadingRate
    list_map[125] = 78; // RSSetShadingRateImage
    list_map[127] = 79; // DispatchMesh
}
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE list_method(void *self, std::uint64_t a, std::uint64_t b,
                                            std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                            std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                            std::uint64_t i, std::uint64_t j) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && calls++ < 2) {
        std::fprintf(
            stderr,
            "[Runtime] Direct3D 12 command list slot %zu (%llx %llx %llx %llx %llx) from %s\n",
            Index, a, b, c, d, e, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    const auto target = list_map[Index];
    if (target == unmapped) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
            std::fprintf(stderr,
                         "[Runtime] unimplemented Direct3D 12 call: command list slot %zu (%llx, "
                         "%llx) from %s\n",
                         Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
            std::fflush(stderr);
        }
        return E_NOTIMPL;
    }
    auto *const host = host_of(self);
    return reinterpret_cast<Raw>((*reinterpret_cast<Slot **>(host))[target])(host, a, b, c, d, e, f,
                                                                             g, h, i, j);
}
HRESULT STDMETHODCALLTYPE list_query(void *self, REFIID iid, void **object) {
    if (!object)
        return E_POINTER;
    // The later interfaces are the same object on the console, and on the host: the proxy
    // answers for those the host's list has. ID3D12GraphicsCommandList5 and 6 had other
    // interface IDs in the kits than the ones the desktop shipped with.
    static constexpr GUID kit_list5{
        0x55050859, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    static constexpr GUID kit_list6{
        0xc3827890, 0x4024, 0x474c, {0x98, 0xcf, 0x64, 0x5b, 0x51, 0xdc, 0x48, 0x74}};
    const auto &host_iid = iid == kit_list5   ? __uuidof(ID3D12GraphicsCommandList5)
                           : iid == kit_list6 ? __uuidof(ID3D12GraphicsCommandList6)
                                              : iid;
    bool ours = is_any<IUnknown, ID3D12Object, ID3D12DeviceChild, ID3D12CommandList,
                       ID3D12GraphicsCommandList>(host_iid);
    if (!ours &&
        is_any<ID3D12GraphicsCommandList1, ID3D12GraphicsCommandList2, ID3D12GraphicsCommandList3,
               ID3D12GraphicsCommandList4, ID3D12GraphicsCommandList5, ID3D12GraphicsCommandList6>(
            host_iid)) {
        IUnknown *later = nullptr;
        if (SUCCEEDED(host_of(self)->QueryInterface(host_iid, reinterpret_cast<void **>(&later)))) {
            later->Release();
            ours = true;
        }
    }
    if (ours) {
        ++static_cast<ListProxy *>(self)->references;
        *object = self;
        return S_OK;
    }
    return host_of(self)->QueryInterface(host_iid, object);
}
ULONG STDMETHODCALLTYPE list_add_ref(void *self) {
    return ++static_cast<ListProxy *>(self)->references;
}
ULONG STDMETHODCALLTYPE list_release(void *self) {
    auto *const proxy = static_cast<ListProxy *>(self);
    const auto left = --proxy->references;
    if (left == 0) {
        {
            const std::lock_guard lock(proxy_mutex);
            list_proxies.erase(proxy);
        }
        proxy->host->Release();
        delete proxy;
    }
    return left;
}

// The methods that take CPU handles, at their console slots (the desktop's + 4): the host's
// handles. The depth clear has a float in a register the generic path does not carry.
using Handle = D3D12_CPU_DESCRIPTOR_HANDLE;
std::atomic<ID3D12Device *> d3d12_device{nullptr};
HRESULT STDMETHODCALLTYPE close_list(void *self) {
    const auto result = host_of(self)->Close();
    if (FAILED(result))
        report_messages();
    return result;
}
// ResourceBarrier: transitions name states the console's way. GENERIC_READ is 0x8c3 there,
// and bit 0x400000 goes with the states shaders read in; on the host that bit is a ray
// tracing acceleration structure, which is no state to be combined with another. Xbox One
// has more of its own from there up (what stays compressed or expanded across the
// transition), where the host has video and shading rate states.
D3D12_RESOURCE_STATES host_state(D3D12_RESOURCE_STATES state) {
    if (state == static_cast<D3D12_RESOURCE_STATES>(0x8c3))
        return D3D12_RESOURCE_STATE_GENERIC_READ;
    if (console == Target::durango)
        return state & ~static_cast<D3D12_RESOURCE_STATES>(0x0fd00000);
    constexpr auto structure = D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE;
    state &= ~static_cast<D3D12_RESOURCE_STATES>(0x100000);
    return state & structure && state != structure ? state & ~structure : state;
}
// The state the title left each render target in, by its last transition: the console shows
// a resource whatever its state, the host copies it out of one it has to be told
std::mutex state_mutex;
std::map<ID3D12Resource *, D3D12_RESOURCE_STATES> target_states;
void STDMETHODCALLTYPE resource_barrier(void *self, UINT count,
                                        const D3D12_RESOURCE_BARRIER *barriers) {
    std::vector<D3D12_RESOURCE_BARRIER> host(barriers, barriers + (barriers ? count : 0));
    for (auto &barrier : host)
        if (barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) {
            barrier.Transition.StateBefore = host_state(barrier.Transition.StateBefore);
            barrier.Transition.StateAfter = host_state(barrier.Transition.StateAfter);
            if (barrier.Transition.StateAfter & D3D12_RESOURCE_STATE_RENDER_TARGET ||
                barrier.Transition.StateBefore & D3D12_RESOURCE_STATE_RENDER_TARGET) {
                const std::lock_guard lock(state_mutex);
                target_states[barrier.Transition.pResource] = barrier.Transition.StateAfter;
            }
        }
    // The title's placed resources are committed ones here, each with memory of its own:
    // none aliases another, and the host turns a list down for saying so.
    std::erase_if(host, [](const D3D12_RESOURCE_BARRIER &barrier) {
        return barrier.Type == D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
    });
    // The console takes two transitions of one subresource in a call, one after the other;
    // the host takes a subresource once per call, so the call is split where one comes again.
    const auto again = [&](std::size_t first, std::size_t at) {
        if (host[at].Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION)
            return false;
        const auto &now = host[at].Transition;
        for (auto i = first; i < at; ++i)
            if (host[i].Type == D3D12_RESOURCE_BARRIER_TYPE_TRANSITION &&
                host[i].Transition.pResource == now.pResource &&
                (host[i].Transition.Subresource == now.Subresource ||
                 host[i].Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
                 now.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES))
                return true;
        return false;
    };
    for (std::size_t first = 0, at = 0; at <= host.size(); ++at)
        if (at == host.size() || again(first, at)) {
            if (at > first)
                host_of(self)->ResourceBarrier(static_cast<UINT>(at - first), host.data() + first);
            first = at;
        }
}
// CopyTextureRegion from a buffer at an offset the host does not take: the console reads a
// texture's data from anywhere in a buffer, the host from multiples of 512 bytes. The data
// goes through a buffer of ours first, where it starts at 0. Those buffers are let go a
// while after they were used, when the GPU is long past them.
struct Staging {
    ID3D12Resource *buffer;
    ULONGLONG made;
};
std::mutex staging_mutex;
std::deque<Staging> staging;
// CopyTextureRegion from a sampler feedback map to a buffer: the console decodes in the copy,
// the host needs ResolveSubresourceRegion(DECODE) into an R8_UINT texture first. Bytes are
// whole mip levels (255: none); the console's 5.3 fixed point for R8_TYPELESS is not done.
struct DecodedFeedback {
    ID3D12Resource *texture;
    UINT64 width;
    UINT height;
};
std::mutex decoded_feedback_mutex;
std::map<ID3D12Resource *, DecodedFeedback> decoded_feedback; // by map
bool copy_feedback_map(List *host, const D3D12_TEXTURE_COPY_LOCATION *target,
                       const D3D12_TEXTURE_COPY_LOCATION *source) {
    if (!target || !source || !target->pResource || !source->pResource ||
        target->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        source->Type != D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX ||
        target->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT)
        return false;
    ID3D12Resource2 *map = nullptr;
    if (FAILED(source->pResource->QueryInterface(IID_PPV_ARGS(&map))))
        return false;
    const auto desc = map->GetDesc1();
    map->Release();
    if (desc.Format != DXGI_FORMAT_SAMPLER_FEEDBACK_MIN_MIP_OPAQUE)
        return false;
    const auto &region = desc.SamplerFeedbackMipRegion;
    const auto width = std::max<UINT64>(desc.Width / std::max<UINT>(region.Width, 1), 1);
    const auto height = std::max<UINT>(desc.Height / std::max<UINT>(region.Height, 1), 1);
    ID3D12GraphicsCommandList1 *list = nullptr;
    if (FAILED(host->QueryInterface(IID_PPV_ARGS(&list))))
        return false;
    ID3D12Resource *texture = nullptr;
    {
        const std::lock_guard lock(decoded_feedback_mutex);
        auto &decoded = decoded_feedback[source->pResource];
        if (decoded.texture && (decoded.width != width || decoded.height != height)) {
            decoded.texture->Release();
            decoded.texture = nullptr;
        }
        ID3D12Device *device = nullptr;
        if (!decoded.texture && SUCCEEDED(host->GetDevice(IID_PPV_ARGS(&device)))) {
            const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
            D3D12_RESOURCE_DESC bytes{};
            bytes.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            bytes.Width = width;
            bytes.Height = height;
            bytes.DepthOrArraySize = desc.DepthOrArraySize;
            bytes.MipLevels = 1;
            bytes.Format = DXGI_FORMAT_R8_UINT;
            bytes.SampleDesc.Count = 1;
            device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bytes,
                                            D3D12_RESOURCE_STATE_RESOLVE_DEST, nullptr,
                                            IID_PPV_ARGS(&decoded.texture));
            device->Release();
            decoded.width = width;
            decoded.height = height;
        }
        texture = decoded.texture;
    }
    if (!texture) {
        list->Release();
        return false;
    }
    const auto transition = [&](ID3D12Resource *resource, D3D12_RESOURCE_STATES from,
                                D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
        host->ResourceBarrier(1, &barrier);
    };
    transition(source->pResource, D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    list->ResolveSubresourceRegion(texture, ~0u, 0, 0, source->pResource, ~0u, nullptr,
                                   DXGI_FORMAT_R8_UINT, D3D12_RESOLVE_MODE_DECODE_SAMPLER_FEEDBACK);
    list->Release();
    transition(source->pResource, D3D12_RESOURCE_STATE_RESOLVE_SOURCE,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(texture, D3D12_RESOURCE_STATE_RESOLVE_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    auto placed = *target;
    auto &footprint = placed.PlacedFootprint.Footprint;
    footprint.Format = DXGI_FORMAT_R8_UINT;
    D3D12_TEXTURE_COPY_LOCATION from{};
    from.pResource = texture;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.SubresourceIndex =
        source->SubresourceIndex < desc.DepthOrArraySize ? source->SubresourceIndex : 0;
    const D3D12_BOX part{0,
                         0,
                         0,
                         static_cast<UINT>(std::min<UINT64>(width, footprint.Width)),
                         std::min(height, footprint.Height),
                         1};
    host->CopyTextureRegion(&placed, 0, 0, 0, &from, &part);
    transition(texture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    if (static std::atomic<bool> shown{false};
        target->PlacedFootprint.Footprint.Format == DXGI_FORMAT_R8_TYPELESS &&
        !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] a sampler feedback map is read back as whole mip levels, "
                             "where the title asks for the console's 5.3 fixed point\n");
        std::fflush(stderr);
    }
    return true;
}
void STDMETHODCALLTYPE copy_texture_region(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                           UINT x, UINT y, UINT z,
                                           const D3D12_TEXTURE_COPY_LOCATION *source,
                                           const D3D12_BOX *box) {
    auto *const host = host_of(self);
    if (!x && !y && !z && copy_feedback_map(host, target, source))
        return;
    ID3D12Device *device = nullptr;
    // the other way, a texture read into a buffer at such an offset: into a buffer of ours
    // at 0, and from there to where the title wants it
    if (target && source && target->Type == D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT &&
        target->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT &&
        target->pResource && !x && !y && !z && SUCCEEDED(host->GetDevice(IID_PPV_ARGS(&device)))) {
        const auto &footprint = target->PlacedFootprint.Footprint;
        const auto offset = target->PlacedFootprint.Offset;
        const auto room = target->pResource->GetDesc().Width;
        const auto rows = footprint.Height; // block formats have fewer; the room caps it
        const auto bytes = std::min<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth,
                                            room > offset ? room - offset : 0);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        const D3D12_RESOURCE_DESC desc{
            D3D12_RESOURCE_DIMENSION_BUFFER,
            0,
            std::max<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth, 1),
            1,
            1,
            1,
            DXGI_FORMAT_UNKNOWN,
            {1, 0},
            D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
            D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *buffer = nullptr;
        const auto made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                          IID_PPV_ARGS(&buffer));
        device->Release();
        if (SUCCEEDED(made) && bytes) {
            auto staged = *target;
            staged.pResource = buffer;
            staged.PlacedFootprint.Offset = 0;
            host->CopyTextureRegion(&staged, 0, 0, 0, source, box);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Transition = {buffer, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE};
            host->ResourceBarrier(1, &barrier);
            host->CopyBufferRegion(target->pResource, offset, buffer, 0, bytes);
            const std::lock_guard lock(staging_mutex);
            staging.push_back({buffer, GetTickCount64()});
            return;
        }
        if (buffer)
            buffer->Release();
        device = nullptr;
    }
    if (!source || source->Type != D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT ||
        source->PlacedFootprint.Offset % D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT == 0 ||
        !source->pResource || FAILED(host->GetDevice(IID_PPV_ARGS(&device)))) {
        host->CopyTextureRegion(target, x, y, z, source, box);
        return;
    }
    const auto &footprint = source->PlacedFootprint.Footprint;
    D3D12_RESOURCE_DESC texture{};
    texture.Dimension = footprint.Depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D
                                            : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texture.Width = footprint.Width;
    texture.Height = footprint.Height;
    texture.DepthOrArraySize = static_cast<UINT16>(footprint.Depth);
    texture.MipLevels = 1;
    texture.Format = footprint.Format;
    texture.SampleDesc.Count = 1;
    UINT rows = footprint.Height;
    device->GetCopyableFootprints(&texture, 0, 1, 0, nullptr, &rows, nullptr, nullptr);
    const auto offset = source->PlacedFootprint.Offset;
    const auto available = source->pResource->GetDesc().Width;
    const auto bytes = std::min<UINT64>(UINT64{footprint.RowPitch} * rows * footprint.Depth,
                                        available > offset ? available - offset : 0);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                   0,
                                   std::max<UINT64>(bytes, 1),
                                   1,
                                   1,
                                   1,
                                   DXGI_FORMAT_UNKNOWN,
                                   {1, 0},
                                   D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                   D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    const auto made = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                      IID_PPV_ARGS(&buffer));
    device->Release();
    if (FAILED(made) || !bytes) {
        std::fprintf(stderr,
                     "[Runtime] CopyTextureRegion from offset %llu of a buffer of %llu bytes, "
                     "%ux%ux%u format %d pitch %u: no way round the offset (0x%08lx)\n",
                     offset, available, footprint.Width, footprint.Height, footprint.Depth,
                     footprint.Format, footprint.RowPitch, static_cast<unsigned long>(made));
        host->CopyTextureRegion(target, x, y, z, source, box);
        return;
    }
    host->CopyBufferRegion(buffer, 0, source->pResource, offset, bytes);
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Transition = {buffer, 0, D3D12_RESOURCE_STATE_COPY_DEST,
                          D3D12_RESOURCE_STATE_COPY_SOURCE};
    host->ResourceBarrier(1, &barrier);
    auto staged = *source;
    staged.pResource = buffer;
    staged.PlacedFootprint.Offset = 0;
    host->CopyTextureRegion(target, x, y, z, &staged, box);
    const std::lock_guard lock(staging_mutex);
    const auto now = GetTickCount64();
    staging.push_back({buffer, now});
    while (!staging.empty() && now - staging.front().made > 20000) {
        staging.front().buffer->Release();
        staging.pop_front();
    }
}
// CopyTextureRegionX (83): the same copy with flags for the console's compressed surfaces
void STDMETHODCALLTYPE copy_texture_region_x(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                             UINT x, UINT y, UINT z,
                                             const D3D12_TEXTURE_COPY_LOCATION *source,
                                             const D3D12_BOX *box, UINT) {
    copy_texture_region(self, target, x, y, z, source, box);
}
void STDMETHODCALLTYPE execute_bundle(void *self, ID3D12GraphicsCommandList *bundle) {
    host_of(self)->ExecuteBundle(static_cast<List *>(host_list(bundle)));
}
// XV_D3D_TRACE: the first calls of a method, with a handle and the host's for it
void trace_handle(const char *method, Handle handle) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (tracing && calls++ < 24) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx -> %llx)\n", method,
                     static_cast<unsigned long long>(handle.ptr),
                     static_cast<unsigned long long>(host_handle(handle).ptr));
        std::fflush(stderr);
    }
}
// SetDescriptorHeaps: the host's heaps for the title's proxies
void STDMETHODCALLTYPE set_descriptor_heaps(void *self, UINT count,
                                            ID3D12DescriptorHeap *const *heaps) {
    auto *const list = static_cast<ListProxy *>(self);
    ID3D12DescriptorHeap *host[2]{};
    list->heaps[0] = list->heaps[1] = nullptr;
    for (UINT i = 0; i < count && i < 2; ++i) {
        host[i] = host_heap(heaps[i]);
        list->heaps[host[i]->GetDesc().Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER] = host[i];
    }
    host_of(self)->SetDescriptorHeaps(std::min(count, 2u), host);
}
// A console GPU handle is the descriptor's address, good whatever heaps are set, and titles
// use tables of heaps they never set. The host reads a table from the heap that is bound,
// so the heap a handle is in gets bound first. It replaces the one of its kind: tables of
// two view heaps cannot be bound for one draw here.
void bind_heap_of(void *self, std::uint64_t handle) {
    void *owner = nullptr;
    bool samplers = false;
    {
        const std::lock_guard lock(shadow_mutex);
        const auto *heap = shadow_heap(handle);
        if (!heap || !heap->shader_visible)
            return;
        owner = heap->owner;
        samplers = heap->type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    }
    auto *const list = static_cast<ListProxy *>(self);
    auto *const host = host_heap(owner);
    if (list->heaps[samplers] == host)
        return;
    list->heaps[samplers] = host;
    ID3D12DescriptorHeap *bound[2]{};
    UINT count = 0;
    for (auto *heap : list->heaps)
        if (heap)
            bound[count++] = heap;
    host_of(self)->SetDescriptorHeaps(count, bound);
}
// Reset: the host's list starts again with no heaps
HRESULT STDMETHODCALLTYPE reset_list(void *self, ID3D12CommandAllocator *allocator,
                                     ID3D12PipelineState *state) {
    auto *const list = static_cast<ListProxy *>(self);
    list->heaps[0] = list->heaps[1] = nullptr;
    list->topology = 0;
    list->indices = {};
    return host_of(self)->Reset(allocator, state);
}
// the host's GPU handle for a console one, which is the descriptor's address in the shadow
D3D12_GPU_DESCRIPTOR_HANDLE host_gpu(D3D12_GPU_DESCRIPTOR_HANDLE handle) {
    const std::lock_guard lock(shadow_mutex);
    if (const auto *heap = shadow_heap(handle.ptr); heap && heap->shader_visible)
        return {heap->gpu + (handle.ptr - heap->base) / heap->stride * heap->host_stride};
    return handle;
}
// The root signatures and descriptor tables: the title's own descriptors in a table are read
// as the table says, once bound
void STDMETHODCALLTYPE set_compute_root_signature(void *self, ID3D12RootSignature *signature) {
    static_cast<ListProxy *>(self)->compute_root = signature;
    host_of(self)->SetComputeRootSignature(signature);
}
void STDMETHODCALLTYPE set_graphics_root_signature(void *self, ID3D12RootSignature *signature) {
    static_cast<ListProxy *>(self)->graphics_root = signature;
    host_of(self)->SetGraphicsRootSignature(signature);
}
void STDMETHODCALLTYPE set_compute_root_table(void *self, UINT parameter,
                                              D3D12_GPU_DESCRIPTOR_HANDLE table) {
    decode_table(static_cast<ListProxy *>(self)->compute_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    host_of(self)->SetComputeRootDescriptorTable(parameter, host_gpu(table));
}
void STDMETHODCALLTYPE set_graphics_root_table(void *self, UINT parameter,
                                               D3D12_GPU_DESCRIPTOR_HANDLE table) {
    decode_table(static_cast<ListProxy *>(self)->graphics_root, parameter, table.ptr);
    bind_heap_of(self, table.ptr);
    host_of(self)->SetGraphicsRootDescriptorTable(parameter, host_gpu(table));
}
// XV_D3D_PROBE: draws per render target
std::mutex draw_mutex;
std::map<ID3D12Resource *, std::uint64_t> draws_into;
void count_draw(void *self) {
    static const bool probing = std::getenv("XV_D3D_PROBE") != nullptr;
    if (!probing)
        return;
    const std::lock_guard lock(draw_mutex);
    ++draws_into[static_cast<ListProxy *>(self)->target];
}
constexpr UINT rectangle_list = 17, quad_list = 19;
void STDMETHODCALLTYPE set_topology(void *self, UINT topology) {
    static_cast<ListProxy *>(self)->topology = topology;
    host_of(self)->IASetPrimitiveTopology(topology == rectangle_list || topology == quad_list
                                              ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST
                                              : static_cast<D3D_PRIMITIVE_TOPOLOGY>(topology));
}
// Indices that draw quads as two triangles each, for at least `quads` of them: 0 1 2, 0 2 3,
// then the same four vertices on. One buffer that only grows; the ones it outgrew stay
// alive, since lists already recorded may still use them.
D3D12_INDEX_BUFFER_VIEW quad_indices(UINT quads) {
    static std::mutex mutex;
    static ID3D12Resource *buffer = nullptr;
    static UINT capacity = 0;
    const std::lock_guard lock(mutex);
    auto *const device = d3d12_device.load();
    if (quads > capacity && device) {
        const UINT wanted = std::max(quads, std::max(capacity * 2, 4096u));
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                       0,
                                       std::uint64_t{wanted} * 6 * sizeof(std::uint32_t),
                                       1,
                                       1,
                                       1,
                                       DXGI_FORMAT_UNKNOWN,
                                       {1, 0},
                                       D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                       D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *larger = nullptr;
        void *mapped = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&larger))) &&
            SUCCEEDED(larger->Map(0, nullptr, &mapped))) {
            auto *const index = static_cast<std::uint32_t *>(mapped);
            constexpr UINT corners[] = {0, 1, 2, 0, 2, 3};
            for (UINT quad = 0; quad < wanted; ++quad)
                for (UINT corner = 0; corner < 6; ++corner)
                    index[quad * 6 + corner] = quad * 4 + corners[corner];
            larger->Unmap(0, nullptr);
            buffer = larger;
            capacity = wanted;
        }
    }
    if (!buffer || quads > capacity)
        return {};
    return {buffer->GetGPUVirtualAddress(), quads * 6 * static_cast<UINT>(sizeof(std::uint32_t)),
            DXGI_FORMAT_R32_UINT};
}
// A quad list is drawn with indices of ours over the title's vertices. A rectangle list
// gives three corners of each rectangle and the GPU makes the fourth: here the pipeline's
// geometry shader does (see rectangle_pipeline).
void STDMETHODCALLTYPE draw_instanced(void *self, UINT vertices, UINT instances, UINT first,
                                      UINT first_instance) {
    count_draw(self);
    auto *const list = static_cast<ListProxy *>(self);
    if (list->topology == quad_list) {
        const auto view = quad_indices(vertices / 4);
        if (!view.BufferLocation)
            return;
        list->host->IASetIndexBuffer(&view);
        list->host->DrawIndexedInstanced(vertices / 4 * 6, instances, 0, static_cast<INT>(first),
                                         first_instance);
        list->host->IASetIndexBuffer(list->indices.BufferLocation ? &list->indices : nullptr);
        return;
    }
    host_of(self)->DrawInstanced(vertices, instances, first, first_instance);
}
void STDMETHODCALLTYPE draw_indexed_instanced(void *self, UINT indices, UINT instances, UINT first,
                                              INT base, UINT first_instance) {
    count_draw(self);
    if (static std::atomic<bool> shown{false};
        static_cast<ListProxy *>(self)->topology == quad_list && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] an indexed quad list is drawn as a triangle list, which "
                             "is not the same picture\n");
        std::fflush(stderr);
    }
    host_of(self)->DrawIndexedInstanced(indices, instances, first, base, first_instance);
}
// XV_D3D_TRACE: a GPU address the title gives, and whether it is CPU memory of the process
void trace_address(const char *method, std::uint64_t address) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    static std::atomic<int> calls{0};
    if (!tracing || calls++ >= 24)
        return;
    MEMORY_BASIC_INFORMATION info{};
    const bool cpu = VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) &&
                     info.State == MEM_COMMIT;
    std::fprintf(stderr, "[Runtime] Direct3D 12 %s(%llx)%s\n", method,
                 static_cast<unsigned long long>(address), cpu ? " in the title's memory" : "");
    std::fflush(stderr);
}
void STDMETHODCALLTYPE set_vertex_buffers(void *self, UINT start, UINT count,
                                          const D3D12_VERTEX_BUFFER_VIEW *views) {
    if (views && count)
        trace_address("IASetVertexBuffers", views->BufferLocation);
    std::vector<D3D12_VERTEX_BUFFER_VIEW> host;
    if (views) {
        host.assign(views, views + count);
        for (auto &view : host) {
            check_address("a vertex buffer", view.BufferLocation, view.SizeInBytes);
            view.BufferLocation = host_address(view.BufferLocation);
        }
    }
    host_of(self)->IASetVertexBuffers(start, count, views ? host.data() : nullptr);
}
void STDMETHODCALLTYPE set_index_buffer(void *self, const D3D12_INDEX_BUFFER_VIEW *view) {
    if (view)
        trace_address("IASetIndexBuffer", view->BufferLocation);
    auto host = view ? *view : D3D12_INDEX_BUFFER_VIEW{};
    check_address("an index buffer", host.BufferLocation, host.SizeInBytes);
    host.BufferLocation = host_address(host.BufferLocation);
    static_cast<ListProxy *>(self)->indices = host;
    host_of(self)->IASetIndexBuffer(view ? &host : nullptr);
}
// On the Series console any memory the GPU can see may be given as a root descriptor, a
// buffer or not. An address that is memory of the title's and in no buffer here is copied
// into a buffer of ours, which is what the host is given: 64 KiB of it, or as much as is
// there. The copies go round a ring, which a frame does not come near filling.
constexpr std::uint64_t most_copied = 64 << 10;
// the GPU address of a copy of `bytes` (most_copied at most) at `source`, 0 if there is no ring
std::uint64_t ring_copy(const void *source, std::uint64_t bytes) {
    constexpr std::uint64_t ring_size = 256ull << 20, most = most_copied;
    static std::mutex mutex;
    static ID3D12Resource *ring = nullptr;
    static std::uint8_t *memory = nullptr;
    static std::uint64_t next = 0;
    const std::lock_guard lock(mutex);
    if (!ring) {
        auto *const device = d3d12_device.load();
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = ring_size;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        void *mapped = nullptr;
        if (!device ||
            FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                   IID_PPV_ARGS(&ring))) ||
            FAILED(ring->Map(0, nullptr, &mapped))) {
            ring = nullptr;
            return 0;
        }
        memory = static_cast<std::uint8_t *>(mapped);
        std::fprintf(stderr, "[Runtime] root descriptors the host cannot take where they are are "
                             "copied into a buffer of its own\n");
        std::fflush(stderr);
    }
    bytes = std::min(bytes, most);
    if (next + most > ring_size)
        next = 0;
    const auto at = next;
    next += (bytes + 255) & ~255ull;
    std::memcpy(memory + at, source, bytes);
    return ring->GetGPUVirtualAddress() + at;
}
std::uint64_t root_address(std::uint64_t address) {
    if (!address)
        return 0;
    {
        const std::lock_guard lock(buffer_mutex);
        auto found = mapped_at.upper_bound(address);
        if (found != mapped_at.begin() && address < std::prev(found)->second.end)
            return std::prev(found)->second.gpu + (address - std::prev(found)->first);
        auto buffer = buffers_at.upper_bound(address);
        if (buffer != buffers_at.begin() && address < std::prev(buffer)->second.end)
            return address;
    }
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) ||
        info.State != MEM_COMMIT || (info.Protect & (PAGE_NOACCESS | PAGE_GUARD)))
        return address;
    const auto region_end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    const auto copy = ring_copy(reinterpret_cast<const void *>(address), region_end - address);
    return copy ? copy : address;
}
// A constant buffer starts anywhere on the console and on a multiple of 256 bytes on the
// host, which turns the whole list down for one that does not. What a buffer the CPU can
// read holds from such an address on is copied to where the host takes it.
std::uint64_t root_constants(std::uint64_t address) {
    const auto host = root_address(address);
    // only Xbox One titles have been seen to need it, and a Series title that binds its
    // constants this way for every draw would have 64 KiB copied each time
    if (!(host & 255) || console != Target::durango)
        return host;
    ID3D12Resource *buffer = nullptr;
    std::uint64_t start = 0, end = 0;
    {
        const std::lock_guard lock(buffer_mutex);
        const auto found = buffers_at.upper_bound(host);
        if (found == buffers_at.begin() || host >= std::prev(found)->second.end)
            return host;
        start = std::prev(found)->first;
        end = std::prev(found)->second.end;
        buffer = std::prev(found)->second.resource;
    }
    void *memory = nullptr;
    const D3D12_RANGE nothing_read{0, 0};
    if (FAILED(buffer->Map(0, &nothing_read, &memory)) || !memory)
        return host;
    const auto copy =
        ring_copy(static_cast<const std::uint8_t *>(memory) + (host - start), end - host);
    buffer->Unmap(0, &nothing_read);
    return copy ? copy : host;
}
void STDMETHODCALLTYPE set_graphics_root_cbv(void *self, UINT index, std::uint64_t address) {
    trace_address("SetGraphicsRootConstantBufferView", address);
    host_of(self)->SetGraphicsRootConstantBufferView(index, root_constants(address));
}
void STDMETHODCALLTYPE set_compute_root_cbv(void *self, UINT index, std::uint64_t address) {
    host_of(self)->SetComputeRootConstantBufferView(index, root_constants(address));
}
void STDMETHODCALLTYPE set_compute_root_srv(void *self, UINT index, std::uint64_t address) {
    host_of(self)->SetComputeRootShaderResourceView(index, root_address(address));
}
void STDMETHODCALLTYPE set_graphics_root_srv(void *self, UINT index, std::uint64_t address) {
    host_of(self)->SetGraphicsRootShaderResourceView(index, root_address(address));
}
void STDMETHODCALLTYPE set_compute_root_uav(void *self, UINT index, std::uint64_t address) {
    host_of(self)->SetComputeRootUnorderedAccessView(index, host_address(address));
}
void STDMETHODCALLTYPE set_graphics_root_uav(void *self, UINT index, std::uint64_t address) {
    host_of(self)->SetGraphicsRootUnorderedAccessView(index, host_address(address));
}
void STDMETHODCALLTYPE so_set_targets(void *self, UINT start, UINT count,
                                      const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views) {
    std::vector<D3D12_STREAM_OUTPUT_BUFFER_VIEW> host;
    if (views) {
        host.assign(views, views + count);
        for (auto &view : host) {
            view.BufferLocation = host_address(view.BufferLocation);
            view.BufferFilledSizeLocation = host_address(view.BufferFilledSizeLocation);
        }
    }
    host_of(self)->SOSetTargets(start, count, views ? host.data() : nullptr);
}
void STDMETHODCALLTYPE set_viewports(void *self, UINT count, const D3D12_VIEWPORT *viewports) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && viewports && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetViewports(%u: %g %g %g %g %g %g)\n", count,
                     viewports->TopLeftX, viewports->TopLeftY, viewports->Width, viewports->Height,
                     viewports->MinDepth, viewports->MaxDepth);
        std::fflush(stderr);
    }
    host_of(self)->RSSetViewports(count, viewports);
}
void STDMETHODCALLTYPE set_scissors(void *self, UINT count, const D3D12_RECT *rects) {
    static const bool tracing = std::getenv("XV_D3D_TRACE") != nullptr;
    if (static std::atomic<int> calls{0}; tracing && rects && count && calls++ < 6) {
        std::fprintf(stderr, "[Runtime] Direct3D 12 RSSetScissorRects(%u: %ld %ld %ld %ld)\n",
                     count, rects->left, rects->top, rects->right, rects->bottom);
        std::fflush(stderr);
    }
    host_of(self)->RSSetScissorRects(count, rects);
}
void STDMETHODCALLTYPE om_set_render_targets(void *self, UINT count, const Handle *targets,
                                             BOOL single, const Handle *depth) {
    if (targets && count)
        trace_handle("OMSetRenderTargets", targets[0]);
    std::vector<Handle> host(targets ? (single ? 1 : count) : 0);
    for (std::size_t i = 0; i < host.size(); ++i)
        host[i] = host_view(targets[i]);
    // one handle for a run of descriptors: the rest follow it in the heap
    const auto stride = console_stride(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for (UINT i = 1; targets && single && i < count; ++i)
        host_view({targets[0].ptr + std::size_t{i} * stride});
    if (!host.empty()) {
        const std::lock_guard lock(target_mutex);
        if (const auto found = target_of.find(host[0].ptr); found != target_of.end()) {
            last_target = found->second;
            static_cast<ListProxy *>(self)->target = found->second;
            const auto desc = found->second->GetDesc();
            if (bound_targets.size() < 12 &&
                std::none_of(bound_targets.begin(), bound_targets.end(), [&](auto *other) {
                    const auto known = other->GetDesc();
                    return known.Width == desc.Width && known.Height == desc.Height &&
                           known.Format == desc.Format;
                }))
                bound_targets.push_back(found->second);
        }
    }
    Handle host_depth{};
    if (depth)
        host_depth = host_view(*depth);
    host_of(self)->OMSetRenderTargets(count, targets ? host.data() : nullptr, single,
                                      depth ? &host_depth : nullptr);
}
void STDMETHODCALLTYPE clear_depth_stencil(void *self, Handle view, D3D12_CLEAR_FLAGS flags,
                                           FLOAT depth, UINT8 stencil, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearDepthStencilView(host_view(view), flags, depth, stencil, count, rects);
}
void STDMETHODCALLTYPE clear_render_target(void *self, Handle view, const FLOAT *color, UINT count,
                                           const D3D12_RECT *rects) {
    host_of(self)->ClearRenderTargetView(host_view(view), color, count, rects);
}
// ClearUnorderedAccessView needs its CPU handle in a non-shader-visible heap; a title passes
// one of its shader-visible heap. The feedback map's view is remade in a heap of ours.
std::mutex feedback_mutex;
std::map<ID3D12Resource *, ID3D12Resource *> feedback_texture; // by map: what it is paired with
struct FeedbackViews {
    ID3D12DescriptorHeap *heap = nullptr;
    UINT stride = 0, next = 0;
} feedback_views;
Handle feedback_clear_view(List *host, Handle view, ID3D12Resource *map) {
    constexpr UINT slots = 64;
    const std::lock_guard lock(feedback_mutex);
    const auto paired = feedback_texture.find(map);
    if (paired == feedback_texture.end())
        return view;
    ID3D12Device *device = nullptr;
    if (FAILED(host->GetDevice(IID_PPV_ARGS(&device))))
        return view;
    auto &views = feedback_views;
    if (!views.heap) {
        const D3D12_DESCRIPTOR_HEAP_DESC desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, slots};
        using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_DESCRIPTOR_HEAP_DESC *,
                                                    REFIID, void **);
        original<Create>(device, 14)(device, &desc, IID_PPV_ARGS(&views.heap));
        views.stride = device->GetDescriptorHandleIncrementSize(desc.Type);
    }
    if (views.heap) {
        view = {views.heap->GetCPUDescriptorHandleForHeapStart().ptr +
                std::size_t{views.next++ % slots} * views.stride};
        using Create = void(STDMETHODCALLTYPE *)(void *, ID3D12Resource *, ID3D12Resource *,
                                                 D3D12_CPU_DESCRIPTOR_HANDLE);
        original<Create>(device, 71)(device, paired->second, map, view);
    }
    device->Release();
    return view;
}
void STDMETHODCALLTYPE clear_uav_uint(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                      ID3D12Resource *resource, const UINT *values, UINT count,
                                      const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewUint(
        host_gpu(gpu), feedback_clear_view(host_of(self), host_view(cpu), resource), resource,
        values, count, rects);
}
void STDMETHODCALLTYPE clear_uav_float(void *self, D3D12_GPU_DESCRIPTOR_HANDLE gpu, Handle cpu,
                                       ID3D12Resource *resource, const FLOAT *values, UINT count,
                                       const D3D12_RECT *rects) {
    bind_heap_of(self, gpu.ptr);
    host_of(self)->ClearUnorderedAccessViewFloat(host_gpu(gpu), host_view(cpu), resource, values,
                                                 count, rects);
}
void STDMETHODCALLTYPE ignored_list_method(void *) {}
template <std::size_t... Index>
std::array<Slot, list_slots> list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{reinterpret_cast<Slot>(&list_method<Index>)...};
    vtable[0] = reinterpret_cast<Slot>(&list_query);
    vtable[1] = reinterpret_cast<Slot>(&list_add_ref);
    vtable[2] = reinterpret_cast<Slot>(&list_release);
    vtable[9] = reinterpret_cast<Slot>(&close_list);
    vtable[10] = reinterpret_cast<Slot>(&reset_list);
    vtable[21] = reinterpret_cast<Slot>(&set_viewports);
    vtable[22] = reinterpret_cast<Slot>(&set_scissors);
    vtable[26] = reinterpret_cast<Slot>(&resource_barrier);
    vtable[27] = reinterpret_cast<Slot>(&execute_bundle);
    vtable[20] = reinterpret_cast<Slot>(&set_topology);
    vtable[16] = reinterpret_cast<Slot>(&copy_texture_region);
    vtable[12] = reinterpret_cast<Slot>(&draw_instanced);
    vtable[13] = reinterpret_cast<Slot>(&draw_indexed_instanced);
    vtable[32] = reinterpret_cast<Slot>(&set_descriptor_heaps);
    vtable[33] = reinterpret_cast<Slot>(&set_compute_root_signature);
    vtable[34] = reinterpret_cast<Slot>(&set_graphics_root_signature);
    vtable[35] = reinterpret_cast<Slot>(&set_compute_root_table);
    vtable[36] = reinterpret_cast<Slot>(&set_graphics_root_table);
    vtable[41] = reinterpret_cast<Slot>(&set_compute_root_cbv);
    vtable[42] = reinterpret_cast<Slot>(&set_graphics_root_cbv);
    vtable[43] = reinterpret_cast<Slot>(&set_compute_root_srv);
    vtable[44] = reinterpret_cast<Slot>(&set_graphics_root_srv);
    vtable[45] = reinterpret_cast<Slot>(&set_compute_root_uav);
    vtable[46] = reinterpret_cast<Slot>(&set_graphics_root_uav);
    vtable[49] = reinterpret_cast<Slot>(&so_set_targets);
    vtable[47] = reinterpret_cast<Slot>(&set_index_buffer);
    vtable[48] = reinterpret_cast<Slot>(&set_vertex_buffers);
    vtable[50] = reinterpret_cast<Slot>(&om_set_render_targets);
    vtable[51] = reinterpret_cast<Slot>(&clear_depth_stencil);
    vtable[52] = reinterpret_cast<Slot>(&clear_render_target);
    vtable[53] = reinterpret_cast<Slot>(&clear_uav_uint);
    vtable[54] = reinterpret_cast<Slot>(&clear_uav_float);
    // FlushPipelineX and OMSetDepthBoundsX: cache flushes the host makes itself, and a depth
    // bounds test no host pipeline here enables
    vtable[67] = reinterpret_cast<Slot>(&ignored_list_method);
    vtable[68] = reinterpret_cast<Slot>(&ignored_list_method);
    // what tunes the console's GPU and changes no picture: occlusion query and hierarchical
    // stencil controls, NopX, KickoffX, shader limits and compression state
    for (const std::size_t hint : {64, 65, 66, 73, 79, 82, 93, 95, 96, 97})
        vtable[hint] = reinterpret_cast<Slot>(&ignored_list_method);
    return vtable;
}
std::array<Slot, list_slots> list_vtable = list_vtable_of(std::make_index_sequence<list_slots>{});
// Titles with the later queue layout (see later_queue) have two more methods in their lists
// as well, at 59 and 60: every method from there on is two slots further. What the two are
// is not known. A list is made before the layout shows, so each call looks.
template <std::size_t Index>
std::uint64_t STDMETHODCALLTYPE list_by_layout(void *self, std::uint64_t a, std::uint64_t b,
                                               std::uint64_t c, std::uint64_t d, std::uint64_t e,
                                               std::uint64_t f, std::uint64_t g, std::uint64_t h,
                                               std::uint64_t i, std::uint64_t j) {
    if (later_queue && Index < 61)
        return 0;
    return reinterpret_cast<Raw>(list_vtable[later_queue ? Index - 2 : Index])(self, a, b, c, d, e,
                                                                               f, g, h, i, j);
}
template <std::size_t... Index>
std::array<Slot, list_slots> layout_list_vtable_of(std::index_sequence<Index...>) {
    std::array<Slot, list_slots> vtable{};
    ((vtable[59 + Index] = reinterpret_cast<Slot>(&list_by_layout<59 + Index>)), ...);
    return vtable;
}
std::array<Slot, list_slots> layout_list_vtable =
    layout_list_vtable_of(std::make_index_sequence<list_slots - 59>{});
// BeginRenderPass: the views in its descriptions are console handles
void STDMETHODCALLTYPE begin_render_pass(void *self, UINT count,
                                         const D3D12_RENDER_PASS_RENDER_TARGET_DESC *targets,
                                         const D3D12_RENDER_PASS_DEPTH_STENCIL_DESC *depth,
                                         D3D12_RENDER_PASS_FLAGS flags) {
    ID3D12GraphicsCommandList4 *host = nullptr;
    if (FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host))))
        return;
    std::vector<D3D12_RENDER_PASS_RENDER_TARGET_DESC> host_targets(targets,
                                                                   targets + (targets ? count : 0));
    for (auto &target : host_targets)
        target.cpuDescriptor = host_view(target.cpuDescriptor);
    D3D12_RENDER_PASS_DEPTH_STENCIL_DESC host_depth{};
    if (depth) {
        host_depth = *depth;
        host_depth.cpuDescriptor = host_view(depth->cpuDescriptor);
    }
    host->BeginRenderPass(count, host_targets.data(), depth ? &host_depth : nullptr, flags);
    host->Release();
}
// DispatchMeshX (group count, flags, indirect arguments, payload): only the plain form, with
// a count and nothing else, has a host method
void STDMETHODCALLTYPE dispatch_mesh_x(void *self, UINT groups, UINT64 flags, UINT64 arguments,
                                       UINT64 payload) {
    ID3D12GraphicsCommandList6 *host = nullptr;
    if (flags || arguments || payload ||
        FAILED(host_of(self)->QueryInterface(IID_PPV_ARGS(&host)))) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DispatchMeshX(%u, flags 0x%llx, arguments %llx, payload "
                         "%llx) has no host form\n",
                         groups, flags, arguments, payload);
        return;
    }
    host->DispatchMesh(groups, 1, 1);
    host->Release();
}
// The methods of a list that work on memory by its address (the console header's slots 60
// to 63, 69 to 72 and 88 to 90). On the console a GPU address is the title's own memory and
// the GPU does the work when it gets there; titles time their frames with the values it
// writes. Here the CPU does it when the list is recorded, and what waits for a value to be
// there has nothing left to wait for.
bool title_memory(std::uint64_t address, std::uint64_t size, bool write);
template <class Value> void write_value(std::uint64_t destination, Value value) {
    if (title_memory(destination, sizeof value, true))
        std::memcpy(reinterpret_cast<void *>(destination), &value, sizeof value);
}
std::uint64_t host_clock() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return static_cast<std::uint64_t>(counter.QuadPart);
}
// D3D12XBOX_WRITE_VALUE_BOP_FLAGS: 1 the GPU's timestamp, 2 the system's
void STDMETHODCALLTYPE write_64_bottom_of_pipe(void *, std::uint64_t destination,
                                               std::uint64_t value, UINT, UINT flags) {
    write_value(destination, flags & 3 ? host_clock() : value);
}
void STDMETHODCALLTYPE write_32_bottom_of_pipe(void *, std::uint64_t destination, UINT value, UINT,
                                               UINT) {
    write_value(destination, value);
}
// D3D12XBOX_WRITE_VALUE_TOP_FLAGS: 4 the GPU's timestamp, 8 the system's
void STDMETHODCALLTYPE write_64_top_of_pipe(void *, std::uint64_t destination, std::uint64_t value,
                                            UINT flags) {
    write_value(destination, flags & 12 ? host_clock() : value);
}
// Write32BitValueTopOfPipeX and Write32BitValueBottomOfShaderX
void STDMETHODCALLTYPE write_32(void *, std::uint64_t destination, UINT value, UINT) {
    write_value(destination, value);
}
void STDMETHODCALLTYPE copy_memory(void *, std::uint64_t destination, std::uint64_t source,
                                   std::uint64_t size, UINT) {
    if (title_memory(destination, size, true) && title_memory(source, size, false))
        std::memmove(reinterpret_cast<void *>(destination), reinterpret_cast<const void *>(source),
                     static_cast<std::size_t>(size));
}
// FillMemoryWith32BitValueX, 64 and 128: the value over and over, for `size` bytes
void fill_memory(std::uint64_t destination, std::uint64_t size, const void *value,
                 std::size_t width) {
    if (!title_memory(destination, size, true))
        return;
    auto *const bytes = reinterpret_cast<std::uint8_t *>(destination);
    for (std::uint64_t at = 0; at + width <= size; at += width)
        std::memcpy(bytes + at, value, width);
}
void STDMETHODCALLTYPE fill_memory_32(void *, std::uint64_t destination, std::uint64_t size,
                                      UINT value, UINT) {
    fill_memory(destination, size, &value, sizeof value);
}
void STDMETHODCALLTYPE fill_memory_64(void *, std::uint64_t destination, std::uint64_t size,
                                      std::uint64_t value, UINT) {
    fill_memory(destination, size, &value, sizeof value);
}
void STDMETHODCALLTYPE fill_memory_128(void *, std::uint64_t destination, std::uint64_t size,
                                       const UINT *value, UINT) {
    if (value)
        fill_memory(destination, size, value, 4 * sizeof *value);
}
// the slots where the two consoles' lists differ
void configure_lists() {
    map_list_slots();
    list_vtable[60] = reinterpret_cast<Slot>(&copy_memory);
    list_vtable[61] = reinterpret_cast<Slot>(&fill_memory_32);
    list_vtable[62] = reinterpret_cast<Slot>(&fill_memory_64);
    list_vtable[63] = reinterpret_cast<Slot>(&fill_memory_128);
    list_vtable[69] = reinterpret_cast<Slot>(&write_32_bottom_of_pipe);
    list_vtable[70] = reinterpret_cast<Slot>(&write_64_bottom_of_pipe);
    // Wait32BitValueX and Wait64BitValueX
    list_vtable[71] = list_vtable[72] = reinterpret_cast<Slot>(&ignored_list_method);
    list_vtable[88] = list_vtable[90] = reinterpret_cast<Slot>(&write_32);
    list_vtable[89] = reinterpret_cast<Slot>(&write_64_top_of_pipe);
    const auto ignore = [](std::initializer_list<std::size_t> slots) {
        for (const auto slot : slots)
            list_vtable[slot] = reinterpret_cast<Slot>(&ignored_list_method);
    };
    if (console == Target::durango) {
        // PrefetchMemoryX and PrefetchPipelineStateX
        ignore({98, 99});
        return;
    }
    list_vtable[83] = reinterpret_cast<Slot>(&copy_texture_region_x);
    // XV_D3D_NO_QUERIES: no queries (BeginQuery, EndQuery, ResolveQueryData), which the
    // debug layer does not get through with some titles
    if (std::getenv("XV_D3D_NO_QUERIES"))
        ignore({28, 29, 30});
    list_vtable[98] = reinterpret_cast<Slot>(&dispatch_mesh_x);
    // DriverHintX, the counters, and the binning of primitive batches
    ignore({99, 100, 101, 102, 103, 104});
    list_vtable[113] = reinterpret_cast<Slot>(&begin_render_pass);
    // SetPredicationBufferX: nothing is drawn by a predicate here
    ignore({85});
}
void *make_list_proxy(List *host) {
    auto *const proxy = new ListProxy{};
    // the slots both layouts share, as they are now
    static const bool filled = [] {
        std::copy_n(list_vtable.begin(), 59, layout_list_vtable.begin());
        return true;
    }();
    static_cast<void>(filled);
    proxy->vtable = console == Target::scarlett ? layout_list_vtable.data() : list_vtable.data();
    proxy->host = host;
    proxy->references = 1;
    const std::lock_guard lock(proxy_mutex);
    list_proxies.insert(proxy);
    return proxy;
}

// ID3D12XboxDmaCommandList (a list of type 16): the console's hardware copies, fills and
// (de)compresses memory by itself, in the order of a queue. The slots are the ones of the
// console header's table. Resources are copied on a host list of ours; what works on
// memory by address is done by the CPU when it is recorded, since on the console a GPU
// address is the title's own memory. The compression formats are not known: those say so.
constexpr UINT dma_list_type = 16;
constexpr std::size_t dma_slots = 32;
struct DmaProxy : ListProxy {
    ID3D12CommandAllocator *allocator = nullptr;
};
bool title_memory(std::uint64_t address, std::uint64_t size, bool write) {
    MEMORY_BASIC_INFORMATION info{};
    if (!address || !VirtualQuery(reinterpret_cast<void *>(address), &info, sizeof info) ||
        info.State != MEM_COMMIT)
        return false;
    const auto end = reinterpret_cast<std::uint64_t>(info.BaseAddress) + info.RegionSize;
    const DWORD writable = PAGE_READWRITE | PAGE_EXECUTE_READWRITE | PAGE_WRITECOPY;
    const DWORD readable = writable | PAGE_READONLY | PAGE_EXECUTE_READ;
    return address + size <= end && (info.Protect & (write ? writable : readable));
}
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE unknown_dma_method(void *, std::uint64_t a, std::uint64_t b) {
    if (static std::atomic<bool> shown{false}; !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] unimplemented Direct3D 12 call: DMA list slot %zu (%llx, %llx) "
                     "from %s\n",
                     Index, a, b, detail::guest_location(_ReturnAddress()).c_str());
        std::fflush(stderr);
    }
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE dma_ok(void *) { return S_OK; }
UINT STDMETHODCALLTYPE dma_type(void *) { return dma_list_type; }
HRESULT STDMETHODCALLTYPE dma_device(void *self, REFIID iid, void **device) {
    return host_of(self)->GetDevice(iid, device);
}
HRESULT STDMETHODCALLTYPE dma_reset(void *self, ID3D12CommandAllocator *, ID3D12PipelineState *) {
    auto *const list = static_cast<DmaProxy *>(self);
    return list->host->Reset(list->allocator, nullptr);
}
void STDMETHODCALLTYPE dma_copy_buffer(void *self, ID3D12Resource *target, UINT64 target_offset,
                                       ID3D12Resource *source, UINT64 source_offset, UINT64 bytes) {
    host_of(self)->CopyBufferRegion(target, target_offset, source, source_offset, bytes);
}
void STDMETHODCALLTYPE dma_copy_texture(void *self, const D3D12_TEXTURE_COPY_LOCATION *target,
                                        UINT x, UINT y, UINT z,
                                        const D3D12_TEXTURE_COPY_LOCATION *source,
                                        const D3D12_BOX *box) {
    host_of(self)->CopyTextureRegion(target, x, y, z, source, box);
}
void STDMETHODCALLTYPE dma_copy_resource(void *self, ID3D12Resource *target,
                                         ID3D12Resource *source) {
    host_of(self)->CopyResource(target, source);
}
void STDMETHODCALLTYPE dma_copy_memory(void *, std::uint64_t target, std::uint64_t source,
                                       std::uint64_t bytes, UINT) {
    if (title_memory(target, bytes, true) && title_memory(source, bytes, false))
        std::memmove(reinterpret_cast<void *>(target), reinterpret_cast<void *>(source),
                     static_cast<std::size_t>(bytes));
}
void STDMETHODCALLTYPE dma_fill_memory(void *, std::uint64_t target, std::uint64_t bytes,
                                       UINT value, UINT) {
    if (!title_memory(target, bytes, true))
        return;
    auto *const words = reinterpret_cast<std::uint32_t *>(target);
    std::fill(words, words + bytes / 4, value);
}
void STDMETHODCALLTYPE dma_last_error(void *, std::uint64_t target) {
    constexpr std::uint32_t none = 0;
    if (title_memory(target, sizeof none, true))
        std::memcpy(reinterpret_cast<void *>(target), &none, sizeof none);
}
void STDMETHODCALLTYPE dma_nothing(void *) {}
const std::array<Slot, dma_slots> dma_vtable = [] {
    std::array<Slot, dma_slots> vtable{};
    const auto fill = [&]<std::size_t... Index>(std::index_sequence<Index...>) {
        ((vtable[Index] = reinterpret_cast<Slot>(&unknown_dma_method<Index>)), ...);
    };
    fill(std::make_index_sequence<dma_slots>{});
    const auto set = [&](std::size_t slot, auto *function) {
        vtable[slot] = reinterpret_cast<Slot>(function);
    };
    set(0, &list_query);
    set(1, &list_add_ref);
    set(2, &list_release);
    for (const std::size_t named : {3, 4, 5, 6}) // private data and the name
        set(named, &dma_ok);
    set(7, &dma_device);
    set(8, &dma_type);
    set(9, &close_list);
    set(10, &dma_reset);
    set(11, &dma_copy_buffer);
    set(12, &copy_texture_region);
    set(13, &dma_copy_resource);
    set(15, &resource_barrier);
    // queries and markers count nothing here; a wait is over when it is recorded, as the
    // value it waits for was written when that was; packets for the hardware and the
    // kickoff have nothing to go to
    for (const std::size_t ignored : {16, 17, 18, 19, 20, 21, 25, 30, 31})
        set(ignored, &dma_nothing);
    set(22, &dma_copy_memory);
    set(23, &dma_fill_memory);
    set(24, &write_64_bottom_of_pipe);
    set(26, &dma_last_error);
    return vtable;
}();
HRESULT make_dma_list(ID3D12Device *device, void **list) {
    auto *const proxy = new DmaProxy{};
    auto result = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&proxy->allocator));
    if (SUCCEEDED(result))
        result = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, proxy->allocator,
                                           nullptr, IID_PPV_ARGS(&proxy->host));
    if (FAILED(result)) {
        delete proxy;
        return result;
    }
    proxy->vtable = const_cast<Slot *>(dma_vtable.data());
    proxy->references = 1;
    const std::lock_guard lock(proxy_mutex);
    list_proxies.insert(proxy);
    *list = proxy;
    return S_OK;
}

// slot 73: a command list from a description (Unreal's call site: the type, 0, the node mask,
// 0), an allocator, a pipeline state, then the interface and the list
HRESULT STDMETHODCALLTYPE create_command_list(ID3D12Device *self, const UINT *desc,
                                              ID3D12CommandAllocator *allocator,
                                              ID3D12PipelineState *state, REFIID, void **list) {
    if (!desc || !list)
        return E_INVALIDARG;
    if (desc[0] == dma_list_type)
        return make_dma_list(self, list);
    List *host = nullptr;
    const auto result =
        self->CreateCommandList(desc[2], static_cast<D3D12_COMMAND_LIST_TYPE>(desc[0]), allocator,
                                state, IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    if (FAILED(result)) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommandListX type %u flags 0x%x nodes 0x%x allocator %p: "
                     "0x%08lx\n",
                     desc[0], desc[1], desc[2], static_cast<void *>(allocator),
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// The uploads of placement textures the title wrote, on a list of ours that runs before the
// title's on the same queue. Textures are where the title keeps them between its own lists:
// shader resources.
struct Uploader {
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    UINT64 value = 0;
    std::vector<ID3D12Resource *> buffers; // until the GPU is past them
};
std::mutex uploader_mutex;
void upload_placements_d3d12(ID3D12CommandQueue *queue) {
    // shader resource states are a direct queue's: the uploads wait for one
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    auto uploads = take_d3d12_uploads();
    if (uploads.empty())
        return;
    if (static std::atomic<int> shown{0}; std::getenv("XV_D3D_TRACE") && shown++ < 5) {
        std::fprintf(stderr,
                     "[Runtime] uploading %zu placement textures, the first with %zu "
                     "subresources\n",
                     uploads.size(), uploads[0].subresources.size());
        std::fflush(stderr);
    }
    const std::lock_guard lock(uploader_mutex);
    static Uploader uploader;
    ID3D12Device *device = nullptr;
    queue->GetDevice(IID_PPV_ARGS(&device));
    if (!uploader.list &&
        (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                               IID_PPV_ARGS(&uploader.allocator))) ||
         FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, uploader.allocator,
                                          nullptr, IID_PPV_ARGS(&uploader.list))) ||
         FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&uploader.fence))))) {
        device->Release();
        return;
    }
    if (uploader.value) {
        if (uploader.fence->GetCompletedValue() < uploader.value) {
            auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uploader.fence->SetEventOnCompletion(uploader.value, event);
            WaitForSingleObject(event, 5000);
            CloseHandle(event);
        }
        for (auto *buffer : uploader.buffers)
            buffer->Release();
        uploader.buffers.clear();
        uploader.allocator->Reset();
        uploader.list->Reset(uploader.allocator, nullptr);
    }
    for (auto &upload : uploads) {
        const auto desc = upload.resource->GetDesc();
        const auto count = static_cast<UINT>(upload.subresources.size());
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
        std::vector<UINT> rows(count);
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rows.data(), nullptr,
                                      &bytes);
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_UPLOAD};
        const D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                              0,
                                              bytes,
                                              1,
                                              1,
                                              1,
                                              DXGI_FORMAT_UNKNOWN,
                                              {1, 0},
                                              D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                              D3D12_RESOURCE_FLAG_NONE};
        ID3D12Resource *buffer = nullptr;
        void *mapped = nullptr;
        if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                      D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                      IID_PPV_ARGS(&buffer))) &&
            SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
            for (UINT i = 0; i < count; ++i) {
                const auto &source = upload.subresources[i];
                const auto pitch = upload.pitches[i];
                // each slice of a volume after the one before it, in both
                const auto plane = std::size_t{pitch} * rows[i];
                for (UINT slice = 0; slice < footprints[i].Footprint.Depth; ++slice)
                    for (UINT row = 0;
                         row < rows[i] &&
                         slice * plane + std::size_t{row + 1} * pitch <= source.size();
                         ++row)
                        std::memcpy(static_cast<std::uint8_t *>(mapped) + footprints[i].Offset +
                                        (std::size_t{slice} * rows[i] + row) *
                                            footprints[i].Footprint.RowPitch,
                                    source.data() + slice * plane + std::size_t{row} * pitch,
                                    pitch);
            }
            buffer->Unmap(0, nullptr);
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Transition = {upload.resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST};
            uploader.list->ResourceBarrier(1, &barrier);
            for (UINT i = 0; i < count; ++i) {
                const D3D12_TEXTURE_COPY_LOCATION target{upload.resource,
                                                         D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,
                                                         {.SubresourceIndex = i}};
                const D3D12_TEXTURE_COPY_LOCATION source{buffer,
                                                         D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT,
                                                         {.PlacedFootprint = footprints[i]}};
                uploader.list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            }
            std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
            uploader.list->ResourceBarrier(1, &barrier);
            uploader.buffers.push_back(buffer);
        } else if (buffer) {
            buffer->Release();
        }
        upload.resource->Release();
    }
    uploader.list->Close();
    ID3D12CommandList *const lists[] = {uploader.list};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(uploader.fence, ++uploader.value);
    device->Release();
}
// ID3D12CommandQueue::ExecuteCommandLists: the host's lists for the title's proxies
void STDMETHODCALLTYPE execute_command_lists(ID3D12CommandQueue *self, UINT count,
                                             ID3D12CommandList *const *lists) {
    if (const auto *text = std::getenv("XV_D3D_SLOW"))
        Sleep(std::atoi(text));
    decode_written_heaps();
    upload_placements_d3d12(self);
    std::vector<ID3D12CommandList *> host(count);
    for (UINT i = 0; i < count; ++i)
        host[i] = host_list(lists[i]);
    self->ExecuteCommandLists(count, host.data());
    // XV_D3D_SYNC: the GPU done with this work before the title goes on, to tell its waits
    // from ours
    if (std::getenv("XV_D3D_SYNC")) {
        static ID3D12Fence *fence = nullptr;
        static UINT64 value = 0;
        if (!fence) {
            ID3D12Device *device = nullptr;
            self->GetDevice(IID_PPV_ARGS(&device));
            device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            device->Release();
        }
        self->Signal(fence, ++value);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(value, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
    }
}
// CreateCommandAllocator (9): the console's copy engine has a list type of its own (16),
// which is a direct list here, as its queue is
HRESULT STDMETHODCALLTYPE create_command_allocator(ID3D12Device *self, UINT type, REFIID iid,
                                                   void **allocator) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, REFIID, void **);
    return original<Create>(self, 9)(self, type == 16 ? D3D12_COMMAND_LIST_TYPE_DIRECT : type, iid,
                                     allocator);
}
// CreateCommandList (12), the desktop's way: a proxy as well
HRESULT STDMETHODCALLTYPE create_command_list_desktop(ID3D12Device *self, UINT node,
                                                      D3D12_COMMAND_LIST_TYPE type,
                                                      ID3D12CommandAllocator *allocator,
                                                      ID3D12PipelineState *state, REFIID,
                                                      void **list) {
    if (!list)
        return E_POINTER;
    List *host = nullptr;
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, UINT, D3D12_COMMAND_LIST_TYPE,
                                                ID3D12CommandAllocator *, ID3D12PipelineState *,
                                                REFIID, void **);
    const auto result =
        original<Create>(self, 12)(self, node, type, allocator, state, IID_PPV_ARGS(&host));
    *list = SUCCEEDED(result) ? make_list_proxy(host) : nullptr;
    return result;
}
// The console's ID3D12Device goes on past the desktop's 44 methods with its own. Those not
// known yet say which they are and fail.
template <std::size_t Index>
HRESULT STDMETHODCALLTYPE unknown_device_method(void *, void *first, void *second) {
    std::fprintf(stderr,
                 "[Runtime] unimplemented Direct3D 12 call: device slot %zu (%p, %p) from %s\n",
                 Index, first, second, detail::guest_location(_ReturnAddress()).c_str());
    std::fflush(stderr);
    return E_NOTIMPL;
}
// CreatePlacedResource (29) and CreateReservedResource (30): the console's states and flags
D3D12_RESOURCE_DESC host_resource_desc(const D3D12_RESOURCE_DESC &desc) {
    auto host = desc;
    host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xff);
    if (host.Layout >= 0x100)
        host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return host;
}
HRESULT STDMETHODCALLTYPE create_placed_resource(ID3D12Device *self, ID3D12Heap *heap,
                                                 UINT64 offset, const D3D12_RESOURCE_DESC *desc,
                                                 D3D12_RESOURCE_STATES state,
                                                 const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                 void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, ID3D12Heap *, UINT64,
                                                const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                                const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(*desc);
    const auto result = original<Create>(self, 29)(self, heap, offset, desc ? &host : nullptr,
                                                   host_state(state), clear, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        track_buffer(static_cast<ID3D12Resource *>(*resource));
    return result;
}
HRESULT STDMETHODCALLTYPE create_reserved_resource(ID3D12Device *self,
                                                   const D3D12_RESOURCE_DESC *desc,
                                                   D3D12_RESOURCE_STATES state,
                                                   const D3D12_CLEAR_VALUE *clear, REFIID iid,
                                                   void **resource) {
    using Create =
        HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, D3D12_RESOURCE_STATES,
                                     const D3D12_CLEAR_VALUE *, REFIID, void **);
    D3D12_RESOURCE_DESC host{};
    if (desc)
        host = host_resource_desc(*desc);
    return original<Create>(self, 30)(self, desc ? &host : nullptr, host_state(state), clear, iid,
                                      resource);
}
// slot 60: CreateGraphicsPipelineStateX, the pipeline and the console's extensions of it
HRESULT STDMETHODCALLTYPE create_graphics_pipeline_x(ID3D12Device *self,
                                                     const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
                                                     UINT, const void *, REFIID iid, void **state) {
    return create_graphics_pipeline(self, desc, iid, state);
}
// what only the console's driver and debugger have a use for: driver hints, hang and debug
// callbacks, debug flags, memory priority, UnregisterPagePoolX
HRESULT STDMETHODCALLTYPE ignored_device_method(void *) { return S_OK; }
// GetGpuHardwareConfigurationX (68): the GPU's clock and which console it is: the first
// Xbox One, or the Series X
void STDMETHODCALLTYPE gpu_hardware_configuration(void *, std::uint64_t *configuration) {
    if (!configuration)
        return;
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    configuration[0] = static_cast<std::uint64_t>(frequency.QuadPart);
    configuration[1] = console == Target::scarlett ? 5 : 0;
}
constexpr std::size_t host_device_slots = 44, device_slots = 134;
template <std::size_t... Index>
std::vector<std::pair<std::size_t, Slot>> unknown_device_methods(std::index_sequence<Index...>) {
    return {{host_device_slots + Index,
             reinterpret_cast<Slot>(&unknown_device_method<host_device_slots + Index>)}...};
}

// ID3D12Device8::CreateCommittedResource2 and CreateSamplerFeedbackUnorderedAccessView (the
// host's 69 and 71). The first Series kit numbers the two sampler feedback formats 187 and
// 188, which the desktop has at 189 and 190.
HRESULT STDMETHODCALLTYPE create_committed_resource_2(
    ID3D12Device *self, const D3D12_HEAP_PROPERTIES *heap, D3D12_HEAP_FLAGS flags,
    const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE *clear,
    void *session, REFIID iid, void **resource) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(
        void *, const D3D12_HEAP_PROPERTIES *, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC1 *,
        D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE *, void *, REFIID, void **);
    D3D12_RESOURCE_DESC1 host_desc{};
    if (desc) {
        host_desc = *desc;
        host_desc.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host_desc.Flags & 0xff);
        if (host_desc.Layout >= 0x100)
            host_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if (host_desc.Format == 187 || host_desc.Format == 188) {
            host_desc.Format = static_cast<DXGI_FORMAT>(host_desc.Format + 2);
            host_desc.Alignment = 0; // the host has no small alignment for these
            // The console takes a mip region as large as the texture, one value for all of
            // it. The host stops at half: its map is then 2x2 where the title expects 1x1.
            auto &region = host_desc.SamplerFeedbackMipRegion;
            region.Width = std::min<UINT>(region.Width, static_cast<UINT>(host_desc.Width / 2));
            region.Height = std::min(region.Height, host_desc.Height / 2);
            if (host_desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D)
                region.Depth = 0;
            // The host wants as many mips as the texture the map is paired with later, where
            // the console takes one: a full chain, which is what a texture worth feedback has
            if (host_desc.MipLevels == 1)
                host_desc.MipLevels = static_cast<UINT16>(
                    std::bit_width(std::max<std::uint64_t>(host_desc.Width, host_desc.Height)));
        }
    }
    if (state == static_cast<D3D12_RESOURCE_STATES>(0x8c3))
        state = D3D12_RESOURCE_STATE_GENERIC_READ;
    if (desc)
        host_rules(heap, host_desc.Flags, state);
    const auto result = original<Create>(self, 69)(self, heap, flags, desc ? &host_desc : nullptr,
                                                   state, clear, session, iid, resource);
    if (SUCCEEDED(result) && resource && *resource)
        console_resource(*resource);
    if (FAILED(result) && desc) {
        std::fprintf(stderr,
                     "[Runtime] CreateCommittedResource2 dimension %d %llux%ux%u mips %u format "
                     "%d flags 0x%x state 0x%x: 0x%08lx\n",
                     desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
                     desc->MipLevels, desc->Format, desc->Flags, state,
                     static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// The console's takes a view description as well, null for the whole map
void STDMETHODCALLTYPE create_feedback_view(ID3D12Device *self, ID3D12Resource *target,
                                            ID3D12Resource *feedback,
                                            const D3D12_UNORDERED_ACCESS_VIEW_DESC *,
                                            D3D12_CPU_DESCRIPTOR_HANDLE handle) {
    using Create = void(STDMETHODCALLTYPE *)(void *, ID3D12Resource *, ID3D12Resource *,
                                             D3D12_CPU_DESCRIPTOR_HANDLE);
    original<Create>(self, 71)(self, target, feedback, host_handle(handle));
    mark_host(handle);
    const std::lock_guard lock(feedback_mutex);
    feedback_texture[feedback] = target;
}
// GetCopyableFootprints (38): the description is a console one, with the console's flags
// and layouts, which the host answers with offsets of -1. A title adds those to the address
// it copies to.
void STDMETHODCALLTYPE copyable_footprints(ID3D12Device *self, const D3D12_RESOURCE_DESC *desc,
                                           UINT first, UINT count, UINT64 offset,
                                           D3D12_PLACED_SUBRESOURCE_FOOTPRINT *layouts, UINT *rows,
                                           UINT64 *row_sizes, UINT64 *total) {
    using Get =
        void(STDMETHODCALLTYPE *)(void *, const D3D12_RESOURCE_DESC *, UINT, UINT, UINT64,
                                  D3D12_PLACED_SUBRESOURCE_FOOTPRINT *, UINT *, UINT64 *, UINT64 *);
    D3D12_RESOURCE_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = static_cast<D3D12_RESOURCE_FLAGS>(host.Flags & 0xff);
        if (host.Layout >= 0x100)
            host.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        host.Alignment = 0;
    }
    original<Get>(self, 38)(self, desc ? &host : nullptr, first, count, offset, layouts, rows,
                            row_sizes, total);
    if (static std::atomic<bool> shown{false};
        desc && total && *total == ~0ull && !shown.exchange(true)) {
        std::fprintf(stderr,
                     "[Runtime] GetCopyableFootprints: the host has no layout for dimension %d "
                     "%llux%ux%u mips %u format %d samples %u layout %d flags 0x%x\n",
                     desc->Dimension, desc->Width, desc->Height, desc->DepthOrArraySize,
                     desc->MipLevels, desc->Format, desc->SampleDesc.Count, desc->Layout,
                     desc->Flags);
        std::fflush(stderr);
    }
}
// GetDeviceRemovedReason (37): a title asks once the host has dropped the device; why it did
// is worth a line
HRESULT STDMETHODCALLTYPE device_removed_reason(ID3D12Device *self) {
    const auto reason = original<HRESULT(STDMETHODCALLTYPE *)(void *)>(self, 37)(self);
    if (static std::atomic<bool> shown{false}; FAILED(reason) && !shown.exchange(true)) {
        std::fprintf(stderr, "[Runtime] the host removed the Direct3D 12 device: 0x%08lx\n",
                     static_cast<unsigned long>(reason));
        report_failure();
        std::fflush(stderr);
    }
    return reason;
}
// CreateHeap (28): the console's heap flags and alignments. The host knows two alignments
// (64 KiB and 4 MiB) and its own flags; a heap is given the nearest of what it has.
HRESULT STDMETHODCALLTYPE create_heap(ID3D12Device *self, const D3D12_HEAP_DESC *desc, REFIID iid,
                                      void **heap) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_HEAP_DESC *, REFIID, void **);
    D3D12_HEAP_DESC host{};
    if (desc) {
        host = *desc;
        host.Flags = static_cast<D3D12_HEAP_FLAGS>(host.Flags & 0xfff);
        if (host.Alignment != 0 && host.Alignment != 0x10000 && host.Alignment != 0x400000)
            host.Alignment = host.Alignment > 0x10000 ? 0x400000 : 0x10000;
        host.SizeInBytes = (host.SizeInBytes + 0xffff) & ~0xffffull;
    }
    auto result = original<Create>(self, 28)(self, desc ? &host : nullptr, iid, heap);
    if (FAILED(result) && desc) {
        // once more with nothing but the size and the kind of heap
        D3D12_HEAP_DESC plain{};
        plain.SizeInBytes = host.SizeInBytes;
        plain.Properties.Type = host.Properties.Type >= 1 && host.Properties.Type <= 3
                                    ? host.Properties.Type
                                    : D3D12_HEAP_TYPE_DEFAULT;
        result = original<Create>(self, 28)(self, &plain, iid, heap);
        std::fprintf(stderr,
                     "[Runtime] CreateHeap of %llu bytes, type %d page %d pool %d, alignment "
                     "0x%llx, flags 0x%x: as a plain heap 0x%08lx\n",
                     desc->SizeInBytes, desc->Properties.Type, desc->Properties.CPUPageProperty,
                     desc->Properties.MemoryPoolPreference, desc->Alignment, desc->Flags,
                     static_cast<unsigned long>(result));
        if (FAILED(result))
            report_messages();
    }
    return result;
}
// CreateCommandSignature (41): which arguments the host turned down. The console has
// argument types of its own from 100 on.
HRESULT STDMETHODCALLTYPE create_command_signature(ID3D12Device *self,
                                                   const D3D12_COMMAND_SIGNATURE_DESC *desc,
                                                   ID3D12RootSignature *root, REFIID iid,
                                                   void **signature) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_COMMAND_SIGNATURE_DESC *,
                                                ID3D12RootSignature *, REFIID, void **);
    // 104 and 105 are the console's own dispatches (DISPATCHX, DISPATCH_L2): three counts,
    // as the desktop's dispatch has, which is what the host is given. What else sets them
    // apart on the console is not known here.
    D3D12_COMMAND_SIGNATURE_DESC host{};
    std::vector<D3D12_INDIRECT_ARGUMENT_DESC> arguments;
    if (desc && desc->pArgumentDescs) {
        arguments.assign(desc->pArgumentDescs, desc->pArgumentDescs + desc->NumArgumentDescs);
        for (auto &argument : arguments)
            if (argument.Type == 104 || argument.Type == 105)
                argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        host = *desc;
        host.pArgumentDescs = arguments.data();
        desc = &host;
    }
    const auto result = original<Create>(self, 41)(self, desc, root, iid, signature);
    if (FAILED(result) && desc) {
        std::string types;
        for (UINT i = 0; i < desc->NumArgumentDescs && desc->pArgumentDescs; ++i)
            types += ' ' + std::to_string(desc->pArgumentDescs[i].Type);
        std::fprintf(stderr,
                     "[Runtime] CreateCommandSignature stride %u, %u arguments of types%s, root "
                     "signature %p: 0x%08lx\n",
                     desc->ByteStride, desc->NumArgumentDescs, types.c_str(),
                     static_cast<void *>(root), static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// ID3D12Device2::CreatePipelineState, from a stream of subobjects: the host's (47), and why
// when it turns the pipeline down
HRESULT STDMETHODCALLTYPE create_pipeline_state(ID3D12Device *self,
                                                const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
                                                REFIID iid, void **state) {
    using Create = HRESULT(STDMETHODCALLTYPE *)(void *, const D3D12_PIPELINE_STATE_STREAM_DESC *,
                                                REFIID, void **);
    // The stream is subobjects one after another, each a type and its description, aligned
    // to 8 bytes. The Series kit numbers the amplification and mesh shaders 23 and 24, where
    // the desktop has them at 24 and 25, and has subobjects of its own from 25 on.
    static constexpr std::size_t sizes[] = {16, 24, 24, 24, 24, 24, 24, 40, 336, 8,  48, 56, 24,
                                            8,  8,  40, 8,  16, 8,  24, 8,  64,  32, 24, 24};
    std::vector<std::uint8_t> stream;
    D3D12_PIPELINE_STATE_STREAM_DESC host{};
    // the title's vertex, pixel and compute shaders, and whether any had to be made for the
    // host: such a pipeline needs a root signature of its own, as in create_graphics_pipeline
    D3D12_SHADER_BYTECODE shaders[3] = {};
    std::size_t shader_at[3] = {};
    bool simple_graphics = true;
    bool made = false;
    std::size_t signature_at = 0; // where in the stream the root signature's pointer is
    ID3D12RootSignature *from_shader = nullptr;
    if (desc && desc->pPipelineStateSubobjectStream && console == Target::scarlett) {
        const auto *bytes = static_cast<const std::uint8_t *>(desc->pPipelineStateSubobjectStream);
        for (std::size_t at = 0; at + 8 <= desc->SizeInBytes;) {
            std::uint32_t type;
            std::memcpy(&type, bytes + at, 4);
            if (type >= std::size(sizes)) {
                if (static std::atomic<bool> shown{false}; !shown.exchange(true))
                    std::fprintf(stderr,
                                 "[Runtime] CreatePipelineState: console subobject %u, which "
                                 "ends what the host is given of the stream\n",
                                 type);
                break;
            }
            const auto size = std::min(sizes[type], desc->SizeInBytes - at);
            stream.insert(stream.end(), bytes + at, bytes + at + size);
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PRIMITIVE_TOPOLOGY && size >= 8) {
                D3D12_PRIMITIVE_TOPOLOGY_TYPE topology;
                std::memcpy(&topology, stream.data() + stream.size() - size + 4, 4);
                simple_graphics &= topology != rectangle_type;
                topology = host_topology_type(topology);
                std::memcpy(stream.data() + stream.size() - size + 4, &topology, 4);
            }
            if (type == 23 || type == 24) {
                ++type;
                std::memcpy(stream.data() + stream.size() - size, &type, 4);
            }
            const auto value_at = stream.size() - size + 8;
            const int shader = type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_VS   ? 0
                               : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_PS ? 1
                               : type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_CS ? 2
                                                                                : -1;
            if (shader >= 0 && size >= 8 + sizeof(D3D12_SHADER_BYTECODE)) {
                shader_at[shader] = value_at;
                std::memcpy(&shaders[shader], stream.data() + value_at, sizeof shaders[shader]);
            }
            if ((type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_DS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_HS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_GS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_AS ||
                 type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_MS) &&
                size >= 8 + sizeof(D3D12_SHADER_BYTECODE)) {
                D3D12_SHADER_BYTECODE other{};
                std::memcpy(&other, stream.data() + value_at, sizeof other);
                simple_graphics &= !other.pShaderBytecode;
            }
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_ROOT_SIGNATURE && size >= 16)
                signature_at = stream.size() - size + 8;
            // the console's flags
            if (type == D3D12_PIPELINE_STATE_SUBOBJECT_TYPE_FLAGS && size >= 8)
                std::memset(stream.data() + stream.size() - size + 4, 0, 4);
            at += size;
        }
        D3D12_SHADER_BYTECODE vertex = shaders[0], pixel = shaders[1], compute = shaders[2];
        made = host_graphics_pair(self, vertex, pixel, simple_graphics && !compute.pShaderBytecode);
        made |= host_bytecode(modules::ShaderStage::compute, compute);
        const D3D12_SHADER_BYTECODE translated[] = {vertex, pixel, compute};
        for (std::size_t index = 0; index < std::size(shader_at); ++index)
            if (shader_at[index])
                std::memcpy(stream.data() + shader_at[index], &translated[index], sizeof vertex);
        void *signature = nullptr;
        if (signature_at)
            std::memcpy(&signature, stream.data() + signature_at, sizeof signature);
        if (made && !signature &&
            (from_shader = shader_root_signature(self, !shaders[2].pShaderBytecode,
                                                 {shaders[0], shaders[1], shaders[2]}))) {
            if (!signature_at) {
                stream.resize(stream.size() + 16);
                signature_at = stream.size() - 8;
            }
            std::memcpy(stream.data() + signature_at, &from_shader, sizeof from_shader);
        }
        host = {stream.size(), stream.data()};
        desc = &host;
    }
    const auto result = original<Create>(self, 47)(self, desc, iid, state);
    if (from_shader)
        from_shader->Release();
    if (FAILED(result)) {
        std::fprintf(stderr, "[Runtime] CreatePipelineState from a stream of %zu bytes: 0x%08lx\n",
                     desc ? desc->SizeInBytes : 0, static_cast<unsigned long>(result));
        report_messages();
    }
    return result;
}
// the host's method `Index` after its first 44, at whatever slot the console has it
template <std::size_t... Index>
std::array<Slot, sizeof...(Index)> later_device_methods(std::index_sequence<Index...>) {
    return {reinterpret_cast<Slot>(&moved_method<host_device_slots + Index>)...};
}

// D3D12XBOX_CREATE_DEVICE_PARAMETERS sizes the console's rings, which mean nothing here
HRESULT WINAPI xbox_create_device(void *, const UINT *parameters, REFIID iid, void **device) {
    if (!device)
        return E_POINTER;
    *device = nullptr;
    // the first of the parameters is the version of the kit the title was built with
    if (parameters && console == Target::scarlett) {
        std::fprintf(stderr, "[Runtime] the title asks for Direct3D 12 of kit version 0x%x\n",
                     *parameters);
    }
    configure_lists();
    using Create = HRESULT(WINAPI *)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);
    static const auto create = host_d3d12<Create>("D3D12CreateDevice");
    // XV_D3D_DRED: what the GPU was doing when the host drops the device, without the debug
    // layer, which runs the queues one after the other and so hides what they do at once
    if (std::getenv("XV_D3D_DEBUG") || std::getenv("XV_D3D_DRED")) {
        using GetDebug = HRESULT(WINAPI *)(REFIID, void **);
        static const auto get_debug = host_d3d12<GetDebug>("D3D12GetDebugInterface");
        if (ID3D12Debug *debug = nullptr; std::getenv("XV_D3D_DEBUG") && get_debug &&
                                          SUCCEEDED(get_debug(IID_PPV_ARGS(&debug)))) {
            debug->EnableDebugLayer();
            // XV_D3D_DEBUG=2: also what each draw reads, checked on the GPU (slow)
            if (ID3D12Debug1 *gpu = nullptr; std::atoi(std::getenv("XV_D3D_DEBUG")) >= 2 &&
                                             SUCCEEDED(debug->QueryInterface(IID_PPV_ARGS(&gpu)))) {
                // 3: the queues not serialized, as they are without the debug layer
                if (std::atoi(std::getenv("XV_D3D_DEBUG")) == 3)
                    gpu->SetEnableSynchronizedCommandQueueValidation(FALSE);
                else
                    gpu->SetEnableGPUBasedValidation(TRUE);
                gpu->Release();
            }
            debug->Release();
        }
        // The debug layer raises an exception of its own (0x87d) for some messages, for a
        // debugger to see. With none attached and a title's frames below, nobody takes it
        // and the process ends: it is passed over.
        static const auto passed_over =
            AddVectoredExceptionHandler(1, [](EXCEPTION_POINTERS *exception) -> LONG {
                return exception->ExceptionRecord->ExceptionCode == 0x87d
                           ? EXCEPTION_CONTINUE_EXECUTION
                           : EXCEPTION_CONTINUE_SEARCH;
            });
        static_cast<void>(passed_over);
        // what the GPU was doing when the device goes, and where it faulted
        if (ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            get_debug && SUCCEEDED(get_debug(IID_PPV_ARGS(&dred)))) {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
        }
    }
    ID3D12Device *host = nullptr;
    const auto result =
        create ? create(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&host)) : E_NOTIMPL;
    if (SUCCEEDED(result)) {
        d3d12_device = host;
        graphics::drawing_with("Direct3D 12");
    }
    if (ID3D12InfoQueue *queue = nullptr;
        SUCCEEDED(result) && SUCCEEDED(host->QueryInterface(IID_PPV_ARGS(&queue))))
        debug_messages = queue;
    std::fprintf(stderr, "[Runtime] Direct3D 12 device for an %s title on the host: 0x%08lx\n",
                 std::string(target_name(console)).c_str(), static_cast<unsigned long>(result));
    std::fflush(stderr);
    if (FAILED(result))
        return result;
    auto overrides =
        unknown_device_methods(std::make_index_sequence<device_slots - host_device_slots>{});
    overrides.push_back({0, reinterpret_cast<Slot>(&device_query)});
    overrides.push_back({13, reinterpret_cast<Slot>(&check_feature_support)});
    overrides.push_back({14, reinterpret_cast<Slot>(&create_descriptor_heap)});
    overrides.push_back({15, reinterpret_cast<Slot>(&descriptor_increment)});
    overrides.push_back({16, reinterpret_cast<Slot>(&create_root_signature)});
    overrides.push_back({41, reinterpret_cast<Slot>(&create_command_signature)});
    overrides.push_back({38, reinterpret_cast<Slot>(&copyable_footprints)});
    overrides.push_back({37, reinterpret_cast<Slot>(&device_removed_reason)});
    if (console == Target::scarlett)
        overrides.push_back({28, reinterpret_cast<Slot>(&create_heap)});
    overrides.push_back({17, reinterpret_cast<Slot>(&create_cbv)});
    overrides.push_back({18, reinterpret_cast<Slot>(&create_srv)});
    overrides.push_back({19, reinterpret_cast<Slot>(&create_uav)});
    overrides.push_back({20, reinterpret_cast<Slot>(&create_target_view<20>)});
    overrides.push_back({21, reinterpret_cast<Slot>(&create_target_view<21>)});
    overrides.push_back({22, reinterpret_cast<Slot>(&create_sampler)});
    if (console == Target::scarlett)
        overrides.push_back({61, reinterpret_cast<Slot>(&create_sampler_x)});
    overrides.push_back({55, reinterpret_cast<Slot>(&create_derived_graphics_pipeline)});
    overrides.push_back({23, reinterpret_cast<Slot>(&copy_descriptors)});
    overrides.push_back({24, reinterpret_cast<Slot>(&copy_descriptors_simple)});
    overrides.push_back({25, reinterpret_cast<Slot>(&resource_allocation_info)});
    // GetAdapterLuid returns its 8 bytes in RAX
    overrides.push_back({43, reinterpret_cast<Slot>(&returned_in_rax<43>)});
    overrides.push_back({26, reinterpret_cast<Slot>(&custom_heap_properties)});
    overrides.push_back({27, reinterpret_cast<Slot>(&create_committed_resource)});
    overrides.push_back({44, reinterpret_cast<Slot>(&create_placed_resource_x)});
    overrides.push_back({8, reinterpret_cast<Slot>(&create_command_queue)});
    overrides.push_back({62, reinterpret_cast<Slot>(&create_compute_pipeline_x)});
    overrides.push_back({74, reinterpret_cast<Slot>(&register_page_pool)});
    overrides.push_back({10, reinterpret_cast<Slot>(&create_graphics_pipeline)});
    overrides.push_back({11, reinterpret_cast<Slot>(&create_compute_pipeline)});
    overrides.push_back({48, reinterpret_cast<Slot>(&create_command_queue_x)});
    overrides.push_back({73, reinterpret_cast<Slot>(&create_command_list)});
    overrides.push_back({12, reinterpret_cast<Slot>(&create_command_list_desktop)});
    overrides.push_back({9, reinterpret_cast<Slot>(&create_command_allocator)});
    overrides.push_back({29, reinterpret_cast<Slot>(&create_placed_resource)});
    overrides.push_back({30, reinterpret_cast<Slot>(&create_reserved_resource)});
    overrides.push_back({60, reinterpret_cast<Slot>(&create_graphics_pipeline_x)});
    for (const std::size_t ignored : {49, 50, 51, 52, 53, 54, 58, 69, 72, 75})
        overrides.push_back({ignored, reinterpret_cast<Slot>(&ignored_device_method)});
    overrides.push_back({68, reinterpret_cast<Slot>(&gpu_hardware_configuration)});
    // how a GDK title paces its frames: there is no swap chain to wait for
    overrides.push_back({78, reinterpret_cast<Slot>(&set_frame_interval)});
    overrides.push_back({79, reinterpret_cast<Slot>(&schedule_frame_event)});
    overrides.push_back({80, reinterpret_cast<Slot>(&wait_frame_event)});
    overrides.push_back({81, reinterpret_cast<Slot>(&frame_statistics)});
    // The later device interfaces follow the console's own methods, where the host has them
    // right after the first interface's 44: ID3D12Device1 to 3 on Xbox One (87 to 92), and up
    // to ID3D12Device8 on Xbox Series (83 to 112, with a console method at 104).
    const auto later = later_device_methods(std::make_index_sequence<32>{});
    if (console == Target::durango) {
        for (std::size_t slot = 87; slot <= 92; ++slot)
            overrides.push_back({slot, later[slot - 87]});
        overrides.push_back({90, reinterpret_cast<Slot>(&create_pipeline_state)});
        overrides.push_back({46, reinterpret_cast<Slot>(&create_placed_srv)});
        overrides.push_back({47, reinterpret_cast<Slot>(&create_placed_uav)});
        overrides.push_back({59, reinterpret_cast<Slot>(&default_msaa_parameters)});
    } else {
        for (std::size_t slot = 83; slot <= 103; ++slot)
            overrides.push_back({slot, later[slot - 83]});
        for (std::size_t slot = 105; slot <= 112; ++slot)
            overrides.push_back({slot, later[slot - 84]});
        overrides.push_back({86, reinterpret_cast<Slot>(&create_pipeline_state)});
        overrides.push_back({109, reinterpret_cast<Slot>(&create_committed_resource_2)});
        overrides.push_back({111, reinterpret_cast<Slot>(&create_feedback_view)});
        if (parameters && *parameters == 0x20004)
            overrides.push_back({133, reinterpret_cast<Slot>(&create_placed_resource_later)});
    }
    // all of the host's vtable, which its runtime calls past the desktop's first 44 too
    console_vtable<Family::device>(host, 96, device_slots, overrides);
    // through the console's QueryInterface, which knows the kits' interface IDs
    const auto queried = device_query(host, iid, device);
    host->Release();
    return queried;
}
} // namespace

void console_d3d12_resource(void *resource) { console_resource(resource); }
void report_failure() {
    auto *const device = d3d12_device.load();
    if (!device)
        return;
    {
        const std::lock_guard lock(signature_mutex);
        std::fprintf(stderr,
                     "[Runtime] root signatures %zu; title descriptors read by their tables: SRV "
                     "%llu, UAV %llu, CBV %llu, sampler %llu\n",
                     tables_of.size(), static_cast<unsigned long long>(decoded[0].load()),
                     static_cast<unsigned long long>(decoded[1].load()),
                     static_cast<unsigned long long>(decoded[2].load()),
                     static_cast<unsigned long long>(decoded[3].load()));
    }
    std::fprintf(stderr,
                 "[Runtime] Direct3D 12 device removed: 0x%08lx; descriptor heaps the title "
                 "let go of: %u\n",
                 static_cast<unsigned long>(device->GetDeviceRemovedReason()),
                 shadows_released.load());
    report_messages();
    ID3D12DeviceRemovedExtendedData *dred = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
        return;
    if (D3D12_DRED_PAGE_FAULT_OUTPUT fault{}; SUCCEEDED(dred->GetPageFaultAllocationOutput(&fault)))
        std::fprintf(stderr, "[Runtime] GPU page fault at 0x%llx\n",
                     static_cast<unsigned long long>(fault.PageFaultVA));
    if (D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs{};
        SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)))
        for (auto *node = crumbs.pHeadAutoBreadcrumbNode; node; node = node->pNext) {
            const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            if (done >= node->BreadcrumbCount)
                continue;
            std::fprintf(stderr, "[Runtime] GPU stopped after %u of %u operations:", done,
                         node->BreadcrumbCount);
            for (UINT i = done > 3 ? done - 3 : 0; i < std::min(done + 3, node->BreadcrumbCount);
                 ++i)
                std::fprintf(stderr, " %s%d", i == done ? ">" : "",
                             static_cast<int>(node->pCommandHistory[i]));
            std::fprintf(stderr, "\n");
        }
    dred->Release();
    std::fflush(stderr);
}
ID3D12Resource *last_render_target() { return last_target.load(); }
std::uint64_t draws_into_target(ID3D12Resource *target) {
    const std::lock_guard lock(draw_mutex);
    const auto found = draws_into.find(target);
    return found == draws_into.end() ? 0 : found->second;
}
std::vector<ID3D12Resource *> bound_render_targets() {
    const std::lock_guard lock(target_mutex);
    return bound_targets;
}
bool read_back(ID3D12Resource *resource, std::vector<std::uint8_t> &data, unsigned &pitch,
               unsigned &width, unsigned &height, DXGI_FORMAT &format,
               D3D12_RESOURCE_STATES state) {
    auto *const queue = first_direct_queue.load();
    ID3D12Device *device = nullptr;
    if (!queue || FAILED(resource->GetDevice(IID_PPV_ARGS(&device))))
        return false;
    const auto desc = resource->GetDesc();
    // a multisampled texture cannot be copied to a buffer
    if (desc.SampleDesc.Count > 1) {
        device->Release();
        return false;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT64 bytes = 0;
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer_desc{D3D12_RESOURCE_DIMENSION_BUFFER,
                                    0,
                                    bytes,
                                    1,
                                    1,
                                    1,
                                    DXGI_FORMAT_UNKNOWN,
                                    {1, 0},
                                    D3D12_TEXTURE_LAYOUT_ROW_MAJOR,
                                    D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource *buffer = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *list = nullptr;
    ID3D12Fence *fence = nullptr;
    bool done = false;
    if (SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&buffer))) &&
        SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                 IID_PPV_ARGS(&allocator))) &&
        SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
                                            IID_PPV_ARGS(&list))) &&
        SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Transition = {resource, 0, state, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        const D3D12_TEXTURE_COPY_LOCATION target{
            buffer, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {.PlacedFootprint = footprint}};
        const D3D12_TEXTURE_COPY_LOCATION source{
            resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {.SubresourceIndex = 0}};
        list->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        list->ResourceBarrier(1, &barrier);
        list->Close();
        ID3D12CommandList *const lists[] = {list};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(fence, 1);
        auto *const event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        fence->SetEventOnCompletion(1, event);
        WaitForSingleObject(event, 5000);
        CloseHandle(event);
        void *mapped = nullptr;
        if (SUCCEEDED(buffer->Map(0, nullptr, &mapped))) {
            const auto *start = static_cast<const std::uint8_t *>(mapped) + footprint.Offset;
            data.assign(start, start + std::size_t{footprint.Footprint.RowPitch} *
                                           footprint.Footprint.Height);
            buffer->Unmap(0, nullptr);
            pitch = footprint.Footprint.RowPitch;
            width = footprint.Footprint.Width;
            height = footprint.Footprint.Height;
            format = desc.Format;
            done = true;
        }
    }
    for (IUnknown *object : {static_cast<IUnknown *>(fence), static_cast<IUnknown *>(list),
                             static_cast<IUnknown *>(allocator), static_cast<IUnknown *>(buffer),
                             static_cast<IUnknown *>(device)})
        if (object)
            object->Release();
    return done;
}
ID3D12CommandQueue *direct_queue() { return first_direct_queue.load(); }
D3D12_RESOURCE_STATES tracked_state(ID3D12Resource *resource, D3D12_RESOURCE_STATES unknown) {
    const std::lock_guard lock(state_mutex);
    const auto found = target_states.find(resource);
    return found == target_states.end() ? unknown : found->second;
}
void set_adapter_wrapper(AdapterWrapper wrapper) { adapter_wrapper = wrapper; }

namespace {
HRESULT WINAPI succeeded() { return S_OK; }
// the console's debug interface is its own; titles go on without one
HRESULT WINAPI no_debug_interface(REFIID, void **debug) {
    if (debug)
        *debug = nullptr;
    return E_NOINTERFACE;
}
// Interface IDs the DLL exports as data, for titles built without their own copies
constexpr GUID graphics_unknown{
    0xaceeea63, 0xe0a9, 0x4a1c, {0xbb, 0xec, 0x71, 0xb2, 0xf4, 0x85, 0xf7, 0x58}};
constexpr GUID dma_command_list{
    0x30bc77b9, 0x8379, 0x4b25, {0x95, 0xde, 0xb8, 0x58, 0x8c, 0x42, 0x27, 0x5d}};
constexpr GUID debug_object_name{
    0x4cca5fd8, 0x921f, 0x42c8, {0x85, 0x66, 0x70, 0xca, 0xf2, 0xa9, 0xb7, 0x41}};
template <class Interface> void *iid_of() {
    static const GUID iid = __uuidof(Interface);
    return const_cast<GUID *>(&iid);
}
} // namespace

void install(Registry &registry, std::string_view module, Target target) {
    console = target;
    const std::pair<const char *, void *> functions[] = {
        {"D3D12XboxCreateDevice", reinterpret_cast<void *>(&xbox_create_device)},
        {"D3D12CreateDevice", reinterpret_cast<void *>(&create_device_12)},
        {"D3D12GetDebugInterface", reinterpret_cast<void *>(&no_debug_interface)},
        {"D3D12XboxSetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxGetProcessDebugFlags", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxEnableMemoryProfiling", reinterpret_cast<void *>(&succeeded)},
        {"D3D12XboxUnloadDriverModule", reinterpret_cast<void *>(&succeeded)},
        {"D3D12ConfigureVirtualMemoryX", reinterpret_cast<void *>(&succeeded)},
        {"D3DConfigureVirtualMemory", reinterpret_cast<void *>(&succeeded)},
        {"D3DFlushEntireCpuCache", reinterpret_cast<void *>(&succeeded)},
        {"D3D12CreateRootSignatureDeserializer",
         reinterpret_cast<void *>(&root_signature_deserializer)},
        {"D3D12CreateVersionedRootSignatureDeserializer",
         reinterpret_cast<void *>(&versioned_root_signature_deserializer)},
        {"D3D12SerializeRootSignature", reinterpret_cast<void *>(&serialize_root_signature)},
        {"D3D12SerializeVersionedRootSignature",
         reinterpret_cast<void *>(&serialize_versioned_root_signature)},
        {"IID_IGraphicsUnknown", const_cast<GUID *>(&graphics_unknown)},
        {"IID_ID3D12XboxDmaCommandList", const_cast<GUID *>(&dma_command_list)},
        {"WKPDID_D3DDebugObjectNameW", const_cast<GUID *>(&debug_object_name)},
        {"IID_ID3D12CommandAllocator", iid_of<ID3D12CommandAllocator>()},
        {"IID_ID3D12CommandList", iid_of<ID3D12CommandList>()},
        {"IID_ID3D12CommandQueue", iid_of<ID3D12CommandQueue>()},
        {"IID_ID3D12Debug", iid_of<ID3D12Debug>()},
        {"IID_ID3D12DescriptorHeap", iid_of<ID3D12DescriptorHeap>()},
        {"IID_ID3D12Device", iid_of<ID3D12Device>()},
        {"IID_ID3D12DeviceChild", iid_of<ID3D12DeviceChild>()},
        {"IID_ID3D12Fence", iid_of<ID3D12Fence>()},
        {"IID_ID3D12GraphicsCommandList", iid_of<ID3D12GraphicsCommandList>()},
        {"IID_ID3D12Heap", iid_of<ID3D12Heap>()},
        {"IID_ID3D12Object", iid_of<ID3D12Object>()},
        {"IID_ID3D12Pageable", iid_of<ID3D12Pageable>()},
        {"IID_ID3D12PipelineState", iid_of<ID3D12PipelineState>()},
        {"IID_ID3D12Resource", iid_of<ID3D12Resource>()},
        {"IID_ID3D12RootSignature", iid_of<ID3D12RootSignature>()},
        {"IID_ID3D12RootSignatureDeserializer", iid_of<ID3D12RootSignatureDeserializer>()},
        {"IID_IDXGIAdapter", iid_of<IDXGIAdapter>()},
        {"IID_IDXGIAdapter1", iid_of<IDXGIAdapter1>()},
        {"IID_IDXGIAdapter2", iid_of<IDXGIAdapter2>()},
        {"IID_IDXGIDevice", iid_of<IDXGIDevice>()},
        {"IID_IDXGIDevice1", iid_of<IDXGIDevice1>()},
        {"IID_IDXGIDevice2", iid_of<IDXGIDevice2>()},
        {"IID_IDXGIDeviceSubObject", iid_of<IDXGIDeviceSubObject>()},
        {"IID_IDXGIFactory", iid_of<IDXGIFactory>()},
        {"IID_IDXGIFactory1", iid_of<IDXGIFactory1>()},
        {"IID_IDXGIFactory2", iid_of<IDXGIFactory2>()},
        {"IID_IDXGIObject", iid_of<IDXGIObject>()},
        {"IID_IDXGISwapChain", iid_of<IDXGISwapChain>()},
        {"IID_IDXGISwapChain1", iid_of<IDXGISwapChain1>()}};
    for (const auto &[name, function] : functions)
        registry.add(module, name, function);
}
} // namespace xv::runtime::modules::d3d12x
