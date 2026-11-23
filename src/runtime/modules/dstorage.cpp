#include "runtime/modules/dstorage.hpp"
#include "runtime/common/runtime.hpp"
#include "runtime/modules/inflate.hpp"
#include "runtime/modules/kernel.hpp"
#include "runtime/modules/placement.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <intrin.h>
#include <mutex>
#include <string>
#include <variant>
#include <vector>
#include <windows.h>

#include <d3d12.h>
#include <unknwn.h>

// DirectStorage as the Series console has it (dstorage_xs.dll, and XCRDAPI.dll for the
// factory): queues of read requests, with marks in between that set a status or signal a
// fence once everything before them is done. Here a queue does its work when it is
// submitted, on the thread that submits it, in order. Reads may be inflated (zlib); the
// console's texture compression (BCPack) is not known here, and such a request fails.
namespace xv::runtime::modules {
namespace {
constexpr GUID file_iid{
    0x43704f9f, 0x27c6, 0x4a9f, {0x88, 0x15, 0xd1, 0x65, 0x27, 0x89, 0x4d, 0xf4}};
constexpr GUID factory_iid{
    0xfb3b54ac, 0xbf82, 0x463a, {0xb7, 0x77, 0x73, 0x19, 0x58, 0x37, 0x86, 0x4d}};
constexpr GUID factory_x2_iid{
    0x62959d11, 0xd45f, 0x4884, {0xa0, 0xa3, 0xf7, 0x86, 0x2e, 0xa2, 0x43, 0xae}};
constexpr GUID status_iid{
    0xcd66d752, 0x2786, 0x44f7, {0x90, 0x70, 0xe2, 0xa6, 0xfd, 0xad, 0x3f, 0x1b}};
constexpr GUID queue_iid{
    0xa3f6649a, 0x8c2c, 0x4b3a, {0x8b, 0x6e, 0x10, 0x18, 0x6a, 0x04, 0xb9, 0xbc}};
constexpr GUID queue_x2_iid{
    0x3c02cb0c, 0xc050, 0x4195, {0x99, 0xa1, 0xa1, 0xce, 0x94, 0xba, 0x33, 0xc5}};

template <class Interface> class Object : public Interface {
  public:
    explicit Object(const GUID &iid) : iid_(iid) {}
    virtual ~Object() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        if (!object)
            return E_POINTER;
        if (iid != IID_IUnknown && iid != iid_) {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        *object = this;
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const auto left = --references_;
        if (!left)
            delete this;
        return left;
    }

  private:
    GUID iid_;
    std::atomic<ULONG> references_{1};
};

// the interfaces, with their methods in the order the console's header has them
struct FileInterface : IUnknown {
    virtual void Close() = 0;
    virtual HANDLE GetHandle() = 0;
};
struct StatusInterface : IUnknown {
    virtual bool IsComplete(UINT32 index) = 0;
    virtual HRESULT GetHResult(UINT32 index) = 0;
};
struct QueueDesc {
    UINT64 source_type;
    UINT16 capacity;
    INT8 priority;
    const char *name;
};
struct QueueInfo {
    QueueDesc desc;
    UINT16 empty_slots, until_submit;
};
// IDStorageFactoryX2 adds a descriptor with one 64-bit options word after the base fields.
constexpr UINT64 disable_auto_submit = 1;
struct QueueDesc1 {
    QueueDesc base;
    UINT64 options;
};
struct QueueInfo1 {
    QueueDesc1 desc;
    UINT16 empty_slots, until_submit;
};
static_assert(offsetof(QueueDesc1, options) == 24 && offsetof(QueueInfo1, empty_slots) == 32);

struct PriorityStats {
    UINT32 current_queues, max_queues, total_queues, total_status, total_fences, pending_requests,
        total_requests;
    UINT64 source_bytes, destination_bytes;
};
struct StorageStats {
    PriorityStats priorities[4];
    UINT8 current_disks, max_disks;
    UINT32 total_disks, pending_reads, total_reads, read_retries, read_errors,
        pending_decompressions, total_decompressions, decompression_errors, staging_stalls,
        current_files, max_files, total_files, current_memory, max_memory, errors;
};
static_assert(sizeof(PriorityStats) == 48 && sizeof(StorageStats) == 256);
struct Request {
    UINT64 options; // bit 0 zlib, bits 1 to 4 BCPack, bit 21 destination in pages, bit 22
                    // source in memory, bit 23 source in pages
    void *destination;
    UINT64 destination_page_offset; // 16 bits of it, in a union as large as two pointers
    UINT32 destination_size;
    void *source; // the file, or the memory
    UINT64 file_offset;
    UINT32 source_size, intermediate_size;
    UINT64 tag;
    const char *name;
};
static_assert(offsetof(Request, destination_size) == 24 && offsetof(Request, source) == 32 &&
              offsetof(Request, source_size) == 48 && offsetof(Request, tag) == 56);
struct ErrorRecord {
    UINT32 failures;
    HRESULT first;
    std::uint8_t rest[1024];
};
struct QueueInterface : IUnknown {
    virtual void EnqueueRequest(const Request *request) = 0;
    virtual void EnqueueStatus(StatusInterface *status, UINT32 index) = 0;
    virtual void EnqueueSignal(ID3D12Fence *fence, UINT64 value) = 0;
    virtual void Submit() = 0;
    virtual void CancelRequestsWithTag(UINT64 mask, UINT64 value) = 0;
    virtual void Close() = 0;
    virtual HANDLE GetErrorEvent() = 0;
    virtual void RetrieveErrorRecord(ErrorRecord *record) = 0;
    virtual void Query(QueueInfo *info) = 0;
    virtual void EnqueueSetEvent(HANDLE event) = 0;
    virtual void Query1(QueueInfo1 *info) = 0;
};
struct FactoryInterface : IUnknown {
    virtual HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) = 0;
    virtual HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) = 0;
    virtual HRESULT CreateStatusArray(UINT32 capacity, const char *name, REFIID iid,
                                      void **status) = 0;
    virtual HRESULT SetCpuAffinity(UINT64 affinity) = 0;
    virtual void SetDebugFlags(UINT32 flags) = 0;
    virtual HRESULT SetStagingBufferSize(UINT32 size) = 0;
    virtual void GetStats(StorageStats *stats) = 0;
    virtual HRESULT CreateQueue1(const QueueDesc1 *desc, REFIID iid, void **queue) = 0;
};

class File final : public Object<FileInterface> {
  public:
    explicit File(HANDLE handle) : Object(file_iid), handle_(handle) {}
    ~File() override { Close(); }
    void Close() override {
        if (const auto handle = handle_.exchange(INVALID_HANDLE_VALUE);
            handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
    }
    HANDLE GetHandle() override { return handle_; }

  private:
    std::atomic<HANDLE> handle_;
};

class Status final : public Object<StatusInterface> {
  public:
    explicit Status(UINT32 capacity) : Object(status_iid), entries_(capacity) {}
    bool IsComplete(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        return index < entries_.size() && entries_[index].done;
    }
    HRESULT GetHResult(UINT32 index) override {
        const std::lock_guard lock(mutex_);
        if (index >= entries_.size())
            return E_INVALIDARG;
        return entries_[index].done ? entries_[index].result : E_PENDING;
    }
    void set(UINT32 index, HRESULT result) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {true, result};
    }
    void pending(UINT32 index) {
        const std::lock_guard lock(mutex_);
        if (index < entries_.size())
            entries_[index] = {};
    }

  private:
    struct Entry {
        bool done = false;
        HRESULT result = S_OK;
    };
    std::mutex mutex_;
    std::vector<Entry> entries_;
};

HRESULT read(const Request &request) {
    constexpr UINT64 zlib = 1, bcpack = 0xf << 1, destination_pages = 1ull << 21,
                     from_memory = 1ull << 22, source_pages = 1ull << 23;
    if (request.options & (bcpack | destination_pages | source_pages)) {
        if (static std::atomic<bool> shown{false}; !shown.exchange(true))
            std::fprintf(stderr,
                         "[Runtime] DirectStorage: a request with options 0x%llx (texture "
                         "compression or physical pages), which is not done\n",
                         request.options);
        return E_NOTIMPL;
    }
    if (!request.destination || !request.source)
        return E_INVALIDARG;
    const bool inflated = request.options & zlib;
    std::vector<std::uint8_t> packed;
    const std::uint8_t *data = nullptr;
    if (request.options & from_memory) {
        data = static_cast<const std::uint8_t *>(request.source);
    } else {
        // read straight to where it goes unless it has to be inflated first
        auto *const target = inflated ? (packed.resize(request.source_size), packed.data())
                                      : static_cast<std::uint8_t *>(request.destination);
        if (!inflated) {
            if (request.source_size > request.destination_size)
                return E_INVALIDARG;
            prepare_title_write(target, request.source_size);
        }
        OVERLAPPED at{};
        at.Offset = static_cast<DWORD>(request.file_offset);
        at.OffsetHigh = static_cast<DWORD>(request.file_offset >> 32);
        DWORD done = 0;
        if (!ReadFile(static_cast<FileInterface *>(request.source)->GetHandle(), target,
                      request.source_size, &done, &at) ||
            done != request.source_size)
            return HRESULT_FROM_WIN32(GetLastError() ? GetLastError() : ERROR_HANDLE_EOF);
        if (!inflated)
            return S_OK;
        data = packed.data();
    }
    prepare_title_write(request.destination, request.destination_size);
    if (!inflated) {
        if (request.source_size > request.destination_size)
            return E_INVALIDARG;
        std::memcpy(request.destination, data, request.source_size);
        return S_OK;
    }
    std::vector<std::uint8_t> plain;
    if (!inflate_zlib({data, request.source_size}, plain) ||
        plain.size() > request.destination_size)
        return E_FAIL;
    std::memcpy(request.destination, plain.data(), plain.size());
    return S_OK;
}

class Queue final : public Object<QueueInterface> {
  public:
    Queue(const QueueDesc &desc, UINT64 options)
        : Object(queue_iid), desc_(desc), options_(options), name_(desc.name ? desc.name : ""),
          error_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        desc_.name = name_.c_str();
    }
    ~Queue() override {
        for (auto &entry : pending_) {
            if (const auto *request = std::get_if<Request>(&entry)) {
                if (!(request->options & 1ull << 22) && request->source)
                    static_cast<IUnknown *>(request->source)->Release();
            } else if (const auto *mark = std::get_if<Mark>(&entry)) {
                mark->status->Release();
            } else if (const auto *signal = std::get_if<Signal>(&entry)) {
                signal->fence->Release();
            } else if (const auto *event = std::get_if<Event>(&entry)) {
                CloseHandle(event->handle);
            }
        }
        CloseHandle(error_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        return Object::QueryInterface(iid == queue_x2_iid ? queue_iid : iid, object);
    }
    void EnqueueRequest(const Request *request) override {
        if (!request)
            return;
        {
            std::unique_lock lock(mutex_);
            wait_for_space(lock);
            // the file is held until the request is done
            if (!(request->options & 1ull << 22) && request->source)
                static_cast<IUnknown *>(request->source)->AddRef();
            pending_.emplace_back(*request);
        }
        submit_when_full();
    }
    void EnqueueStatus(StatusInterface *status, UINT32 index) override {
        if (!status)
            return;
        status->AddRef();
        {
            std::unique_lock lock(mutex_);
            wait_for_space(lock);
            static_cast<Status *>(status)->pending(index);
            pending_.emplace_back(Mark{static_cast<Status *>(status), index});
        }
        submit_when_full();
    }
    void EnqueueSignal(ID3D12Fence *fence, UINT64 value) override {
        if (!fence)
            return;
        fence->AddRef();
        {
            std::unique_lock lock(mutex_);
            wait_for_space(lock);
            pending_.emplace_back(Signal{fence, value});
        }
        submit_when_full();
    }
    void Submit() override {
        // Take submissions in queue order, including when more than one thread submits.
        const std::lock_guard running(running_);
        std::vector<Entry> work;
        {
            const std::lock_guard lock(mutex_);
            work.swap(pending_);
        }
        space_.notify_all();
        HRESULT since_mark = S_OK;
        for (auto &entry : work) {
            if (auto *const request = std::get_if<Request>(&entry)) {
                const auto result = read(*request);
                // XV_FILE_TRACE: the first requests, and every one that fails
                if (static std::atomic<int> shown{0};
                    std::getenv("XV_FILE_TRACE") && (FAILED(result) || shown < 24) && shown++ < 200)
                    std::fprintf(stderr,
                                 "[Runtime] DirectStorage request: options 0x%llx, %u bytes at "
                                 "%llu to %u bytes (intermediate %u): 0x%08lx\n",
                                 request->options, request->source_size, request->file_offset,
                                 request->destination_size, request->intermediate_size,
                                 static_cast<unsigned long>(result));
                if (!(request->options & 1ull << 22) && request->source)
                    static_cast<IUnknown *>(request->source)->Release();
                if (FAILED(result)) {
                    if (SUCCEEDED(since_mark))
                        since_mark = result;
                    const std::lock_guard lock(mutex_);
                    if (!failures_++)
                        first_failure_ = result;
                    SetEvent(error_);
                }
            } else if (auto *const mark = std::get_if<Mark>(&entry)) {
                mark->status->set(mark->index, since_mark);
                mark->status->Release();
                since_mark = S_OK;
            } else if (auto *const signal = std::get_if<Signal>(&entry)) {
                signal->fence->Signal(signal->value);
                signal->fence->Release();
            } else if (auto *const event = std::get_if<Event>(&entry)) {
                SetEvent(event->handle);
                CloseHandle(event->handle);
            }
        }
    }
    void CancelRequestsWithTag(UINT64 mask, UINT64 value) override {
        const std::lock_guard lock(mutex_);
        std::erase_if(pending_, [&](Entry &entry) {
            auto *const request = std::get_if<Request>(&entry);
            if (!request || (request->tag & mask) != value)
                return false;
            if (!(request->options & 1ull << 22) && request->source)
                static_cast<IUnknown *>(request->source)->Release();
            return true;
        });
        space_.notify_all();
    }
    void Close() override { Submit(); }
    HANDLE GetErrorEvent() override { return error_; }
    void RetrieveErrorRecord(ErrorRecord *record) override {
        if (!record)
            return;
        const std::lock_guard lock(mutex_);
        *record = {};
        record->failures = failures_;
        record->first = first_failure_;
        failures_ = 0;
        first_failure_ = S_OK;
        ResetEvent(error_);
    }
    void Query(QueueInfo *info) override {
        if (!info)
            return;
        const std::lock_guard lock(mutex_);
        const auto used =
            static_cast<UINT16>(std::min<std::size_t>(pending_.size(), desc_.capacity));
        *info = {desc_, static_cast<UINT16>(desc_.capacity - used),
                 static_cast<UINT16>(desc_.capacity / 2 > used ? desc_.capacity / 2 - used : 0)};
    }
    void EnqueueSetEvent(HANDLE event) override {
        HANDLE copy = nullptr;
        if (!DuplicateHandle(GetCurrentProcess(), event, GetCurrentProcess(), &copy, 0, FALSE,
                             DUPLICATE_SAME_ACCESS))
            return;
        {
            std::unique_lock lock(mutex_);
            wait_for_space(lock);
            pending_.emplace_back(Event{copy});
        }
        submit_when_full();
    }
    void Query1(QueueInfo1 *info) override {
        if (!info)
            return;
        const std::lock_guard lock(mutex_);
        const auto used = std::min(pending_.size(), usable_slots());
        const auto threshold = static_cast<std::size_t>(desc_.capacity / 2);
        const auto until_submit =
            options_ & disable_auto_submit || used >= threshold ? 0 : threshold - used;
        *info = {{desc_, options_},
                 static_cast<UINT16>(usable_slots() - used),
                 static_cast<UINT16>(until_submit)};
    }

  private:
    struct Mark {
        Status *status;
        UINT32 index;
    };
    struct Signal {
        ID3D12Fence *fence;
        UINT64 value;
    };
    struct Event {
        HANDLE handle;
    };
    using Entry = std::variant<Request, Mark, Signal, Event>;
    std::size_t usable_slots() const { return desc_.capacity > 1 ? desc_.capacity - 1 : 0; }
    void wait_for_space(std::unique_lock<std::mutex> &lock) {
        if (options_ & disable_auto_submit)
            space_.wait(lock, [this] { return pending_.size() < usable_slots(); });
    }
    // a queue submits itself when half of it is taken
    void submit_when_full() {
        if (options_ & disable_auto_submit)
            return;
        bool full;
        {
            const std::lock_guard lock(mutex_);
            full = pending_.size() >= std::max<std::size_t>(desc_.capacity / 2, 1);
        }
        if (full)
            Submit();
    }
    QueueDesc desc_;
    UINT64 options_;
    std::string name_;
    HANDLE error_;
    std::mutex mutex_, running_;
    std::condition_variable space_;
    std::vector<Entry> pending_;
    UINT32 failures_ = 0;
    HRESULT first_failure_ = S_OK;
};

class Factory final : public Object<FactoryInterface> {
  public:
    Factory() : Object(factory_iid) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void **object) override {
        return Object::QueryInterface(iid == factory_x2_iid ? factory_iid : iid, object);
    }
    HRESULT CreateQueue(const QueueDesc *desc, REFIID iid, void **queue) override {
        if (!desc || !queue)
            return E_POINTER;
        return make_queue(*desc, 0, iid, queue);
    }
    HRESULT OpenFile(const WCHAR *path, REFIID iid, void **file) override {
        if (!path || !file)
            return E_POINTER;
        *file = nullptr;
        const auto handle =
            CreateFileW(console_file_path(path).c_str(), GENERIC_READ,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS,
                        nullptr); // a title opens the root of its package as well
        if (handle == INVALID_HANDLE_VALUE) {
            const auto error = GetLastError();
            if (static std::atomic<int> shown{0}; shown++ < 6)
                std::fprintf(stderr,
                             "[Runtime] DirectStorage: cannot open %ls (as %ls): error %lu\n", path,
                             console_file_path(path).c_str(), error);
            return HRESULT_FROM_WIN32(error);
        }
        if (static std::atomic<int> shown{0}; std::getenv("XV_FILE_TRACE") && shown++ < 40)
            std::fprintf(stderr, "[Runtime] DirectStorage: opened %ls (%zu characters) from %s\n",
                         path, wcslen(path), detail::guest_location(_ReturnAddress()).c_str());
        auto *const made = new File(handle);
        const auto result = made->QueryInterface(iid, file);
        made->Release();
        return result;
    }
    HRESULT CreateStatusArray(UINT32 capacity, const char *, REFIID iid, void **status) override {
        if (!status)
            return E_POINTER;
        auto *const made = new Status(capacity);
        const auto result = made->QueryInterface(iid, status);
        made->Release();
        return result;
    }
    HRESULT SetCpuAffinity(UINT64) override { return S_OK; }
    void SetDebugFlags(UINT32) override {}
    HRESULT SetStagingBufferSize(UINT32) override { return S_OK; }
    void GetStats(StorageStats *stats) override {
        // The shim does its work with Win32 I/O and has no console storage telemetry.
        if (stats)
            *stats = {};
    }
    HRESULT CreateQueue1(const QueueDesc1 *desc, REFIID iid, void **queue) override {
        if (!desc || !queue)
            return E_POINTER;
        *queue = nullptr;
        if (desc->base.capacity < 2 || (desc->options & ~disable_auto_submit))
            return E_INVALIDARG;
        return make_queue(desc->base, desc->options, iid, queue);
    }

  private:
    static HRESULT make_queue(const QueueDesc &desc, UINT64 options, REFIID iid, void **queue) {
        auto *const made = new Queue(desc, options);
        const auto result = made->QueryInterface(iid, queue);
        made->Release();
        return result;
    }
};

HRESULT WINAPI get_factory(const GUID *iid, void **factory) {
    if (!iid || !factory)
        return E_POINTER;
    static Factory *const one = new Factory;
    return one->QueryInterface(*iid, factory);
}
} // namespace

void install_dstorage(Registry &registry) {
    for (const auto *module : {"dstorage_xs.dll", "xcrdapi.dll"})
        registry.add(module, "DStorageGetFactory", reinterpret_cast<void *>(&get_factory));
}
} // namespace xv::runtime::modules
