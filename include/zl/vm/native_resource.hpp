#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include "zl/compiler/native_abi.hpp"

namespace zl {

class NativeResourceBorrow;
class NativeBufferView;
class NativeStructView;

// Move-only ownership of an opaque native resource. The resource itself is not
// a GC object: ownership is deterministic and release occurs exactly once when
// the final owner is destroyed or explicitly reset/released.
class NativeResourceOwner {
public:
    using Handle = std::uintptr_t;
    using DestroyFn = std::function<void(Handle)>;

    NativeResourceOwner() = default;
    NativeResourceOwner(Handle handle, DestroyFn destroy)
        : handle_(handle), destroy_(std::move(destroy)), lifetime_(std::make_shared<Lifetime>()) {
        lifetime_->alive.store(handle != 0, std::memory_order_release);
    }

    NativeResourceOwner(const NativeResourceOwner&) = delete;
    NativeResourceOwner& operator=(const NativeResourceOwner&) = delete;

    NativeResourceOwner(NativeResourceOwner&& other) noexcept
        : handle_(std::exchange(other.handle_, 0)),
          destroy_(std::move(other.destroy_)),
          lifetime_(std::move(other.lifetime_)) {
        other.lifetime_ = std::make_shared<Lifetime>();
    }

    NativeResourceOwner& operator=(NativeResourceOwner&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = std::exchange(other.handle_, 0);
            destroy_ = std::move(other.destroy_);
            lifetime_ = std::move(other.lifetime_);
            other.lifetime_ = std::make_shared<Lifetime>();
        }
        return *this;
    }

    ~NativeResourceOwner() { reset(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != 0; }
    [[nodiscard]] Handle get() const {
        if (!valid()) throw std::runtime_error("native resource handle is not valid");
        return handle_;
    }

    // Transfers ownership out of this owner without destroying the resource.
    // Existing borrows are invalidated because this owner no longer controls
    // the resource lifetime.
    [[nodiscard]] Handle release() noexcept {
        const Handle result = std::exchange(handle_, 0);
        if (lifetime_) lifetime_->alive.store(false, std::memory_order_release);
        return result;
    }

    void reset() noexcept;

private:
    struct Lifetime { std::atomic<bool> alive{false}; };
    Handle handle_{0};
    DestroyFn destroy_;
    std::shared_ptr<Lifetime> lifetime_{std::make_shared<Lifetime>()};

    friend class NativeResourceBorrow;
    friend class NativeBufferView;
    friend class NativeStructView;
};

// A deliberately non-owning native handle view. It follows move transfer of
// its owner, and becomes invalid when that ownership lifetime ends.
class NativeResourceBorrow {
public:
    NativeResourceBorrow() = default;
    // Legacy raw-handle construction is intentionally untracked. New code
    // should prefer the owner-based constructor so lifetime is checked.
    explicit NativeResourceBorrow(NativeResourceOwner::Handle handle)
        : handle_(handle), tracked_(false) {}
    explicit NativeResourceBorrow(const NativeResourceOwner& owner)
        : handle_(owner.handle_), lifetime_(owner.lifetime_), tracked_(true) {}

    [[nodiscard]] bool valid() const noexcept {
        if (handle_ == 0) return false;
        if (!tracked_) return true;
        const auto life = lifetime_.lock();
        return life && life->alive.load(std::memory_order_acquire);
    }

    [[nodiscard]] NativeResourceOwner::Handle get() const {
        if (!valid()) throw std::runtime_error("native resource borrow is not valid or owner is destroyed");
        return handle_;
    }

private:
    NativeResourceOwner::Handle handle_{0};
    std::weak_ptr<NativeResourceOwner::Lifetime> lifetime_;
    bool tracked_{false};

    friend class NativeBufferView;
    friend class NativeStructView;
};

// A borrowed byte view tied to a NativeResourceOwner. It never owns or frees
// the memory; it becomes invalid automatically when the owner is destroyed.
class NativeBufferView {
public:
    NativeBufferView() = default;
    NativeBufferView(const NativeResourceOwner& owner, const void* data, std::size_t size)
        : data_(data), size_(size), lifetime_(owner.lifetime_) {
        if (!owner.valid()) throw std::runtime_error("cannot borrow buffer from invalid native resource");
        if (data == nullptr && size != 0) throw std::runtime_error("native buffer view has null data with non-zero size");
    }
    NativeBufferView(const NativeResourceBorrow& borrow, const void* data, std::size_t size)
        : data_(data), size_(size), lifetime_(borrow.lifetime_) {
        if (!borrow.valid()) throw std::runtime_error("cannot borrow buffer from invalid native resource");
        if (data == nullptr && size != 0) throw std::runtime_error("native buffer view has null data with non-zero size");
    }

    [[nodiscard]] bool valid() const noexcept {
        const auto life = lifetime_.lock();
        return (data_ != nullptr || size_ == 0) && life && life->alive.load(std::memory_order_acquire);
    }

    [[nodiscard]] const void* data() const {
        if (!valid()) throw std::runtime_error("native buffer borrow is not valid or owner is destroyed");
        return data_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    friend bool operator==(const NativeBufferView& a, const NativeBufferView& b) noexcept {
        return a.data_ == b.data_ && a.size_ == b.size_;
    }
    friend bool operator!=(const NativeBufferView& a, const NativeBufferView& b) noexcept { return !(a == b); }

private:
    const void* data_{nullptr};
    std::size_t size_{0};
    std::weak_ptr<NativeResourceOwner::Lifetime> lifetime_;
};


// Borrowed bytes representing a native C-ABI struct. The view carries no
// ownership and is valid only while the associated native resource owner is
// alive and the synchronous native call is active.
class NativeStructView {
public:
    NativeStructView() = default;
    NativeStructView(const NativeResourceOwner& owner, const void* data, std::size_t size, std::size_t alignment)
        : data_(data), size_(size), alignment_(alignment), lifetime_(owner.lifetime_) {
        if (!owner.valid()) throw std::runtime_error("cannot borrow struct from invalid native resource");
        if (data == nullptr && size != 0) throw std::runtime_error("native struct view has null data with non-zero size");
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::runtime_error("native struct view alignment must be a power of two");
        if (data != nullptr && (reinterpret_cast<std::uintptr_t>(data) % alignment) != 0)
            throw std::runtime_error("native struct view data is not aligned");
    }
    NativeStructView(const NativeResourceBorrow& borrow, const void* data, std::size_t size, std::size_t alignment)
        : data_(data), size_(size), alignment_(alignment), lifetime_(borrow.lifetime_) {
        if (!borrow.valid()) throw std::runtime_error("cannot borrow struct from invalid native resource");
        if (data == nullptr && size != 0) throw std::runtime_error("native struct view has null data with non-zero size");
        if (alignment == 0 || (alignment & (alignment - 1)) != 0) throw std::runtime_error("native struct view alignment must be a power of two");
        if (data != nullptr && (reinterpret_cast<std::uintptr_t>(data) % alignment) != 0)
            throw std::runtime_error("native struct view data is not aligned");
    }
    [[nodiscard]] bool valid() const noexcept {
        const auto life = lifetime_.lock();
        return (data_ != nullptr || size_ == 0) && life && life->alive.load(std::memory_order_acquire);
    }
    [[nodiscard]] const void* data() const {
        if (!valid()) throw std::runtime_error("native struct view is not valid or owner is destroyed");
        return data_;
    }
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    [[nodiscard]] std::size_t alignment() const noexcept { return alignment_; }
    friend bool operator==(const NativeStructView& a, const NativeStructView& b) noexcept { return a.data_ == b.data_ && a.size_ == b.size_ && a.alignment_ == b.alignment_; }
    friend bool operator!=(const NativeStructView& a, const NativeStructView& b) noexcept { return !(a == b); }
private:
    const void* data_{nullptr};
    std::size_t size_{0};
    std::size_t alignment_{1};
    std::weak_ptr<NativeResourceOwner::Lifetime> lifetime_;
};

// Stable runtime representation for an opaque FFI handle. The token is never
// a raw pointer in the ZL value model; it is only a boundary identifier.
struct NativeHandleRef {
    std::uint64_t id{0};
    [[nodiscard]] bool valid() const noexcept { return id != 0; }
    friend bool operator==(NativeHandleRef a, NativeHandleRef b) noexcept { return a.id == b.id; }
    friend bool operator!=(NativeHandleRef a, NativeHandleRef b) noexcept { return !(a == b); }
};

// Process-local registry for opaque handles. The registry owns resource
// objects; exported values contain only stable integer IDs.

// A host-to-ZL callback lifetime token. Registration owns the state; each
// NativeCallbackLease represents one in-flight invocation. Closing waits for
// existing leases to drain and prevents new invocations, so a native thread
// cannot call into an expired callback context.
class NativeCallbackLifetime final {
public:
    NativeCallbackLifetime() = default;
    NativeCallbackLifetime(const NativeCallbackLifetime&) = delete;
    NativeCallbackLifetime& operator=(const NativeCallbackLifetime&) = delete;

    void close() noexcept;
    [[nodiscard]] bool tryAcquire() noexcept;
    void release() noexcept;
    [[nodiscard]] bool closed() const noexcept;

private:
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::size_t active_{0};
    bool closed_{false};
};

class NativeCallbackLease final {
public:
    NativeCallbackLease() = default;
    explicit NativeCallbackLease(std::shared_ptr<NativeCallbackLifetime> lifetime) noexcept;
    ~NativeCallbackLease();

    NativeCallbackLease(const NativeCallbackLease&) = delete;
    NativeCallbackLease& operator=(const NativeCallbackLease&) = delete;
    NativeCallbackLease(NativeCallbackLease&& other) noexcept;
    NativeCallbackLease& operator=(NativeCallbackLease&& other) noexcept;

    [[nodiscard]] bool valid() const noexcept { return lifetime_ != nullptr; }

private:
    std::shared_ptr<NativeCallbackLifetime> lifetime_;
};

[[nodiscard]] std::shared_ptr<NativeCallbackLifetime> makeNativeCallbackLifetime();
[[nodiscard]] NativeCallbackLease acquireNativeCallbackLease(const std::shared_ptr<NativeCallbackLifetime>& lifetime);


// Stable process-local callback token. The callable itself remains owned by
// the callback registry; ZL and native code exchange only this integer id.
struct NativeCallbackRef {
    std::uint64_t id{0};
    [[nodiscard]] bool valid() const noexcept { return id != 0; }
    friend bool operator==(NativeCallbackRef a, NativeCallbackRef b) noexcept { return a.id == b.id; }
    friend bool operator!=(NativeCallbackRef a, NativeCallbackRef b) noexcept { return !(a == b); }
};

// A registry entry couples a callback target to its lifetime lease. Native
// callers must acquire a lease for every invocation, so close() deterministically
// waits for in-flight callbacks and rejects late calls.
class NativeCallbackRegistry {
public:
    using InvokeFn = std::function<std::int32_t(const ZlNativeValue*, std::uint32_t, ZlNativeValue*, char*, std::uint32_t)>;

    NativeCallbackRegistry() = default;
    NativeCallbackRegistry(const NativeCallbackRegistry&) = delete;
    NativeCallbackRegistry& operator=(const NativeCallbackRegistry&) = delete;

    [[nodiscard]] NativeCallbackRef insert(InvokeFn invoke);
    [[nodiscard]] bool contains(NativeCallbackRef ref) const;
    void close(NativeCallbackRef ref);
    [[nodiscard]] std::int32_t invoke(NativeCallbackRef ref, const ZlNativeValue* args, std::uint32_t argc, ZlNativeValue* result, char* errorMessage, std::uint32_t errorMessageCapacity);

private:
    struct Entry {
        InvokeFn invoke;
        std::shared_ptr<NativeCallbackLifetime> lifetime;
    };
    mutable std::mutex mutex_;
    std::uint64_t nextId_{1};
    std::unordered_map<std::uint64_t, Entry> entries_;
};

NativeCallbackRegistry& nativeCallbackRegistry();

class NativeResourceRegistry {
public:
    NativeResourceRegistry() = default;
    NativeResourceRegistry(const NativeResourceRegistry&) = delete;
    NativeResourceRegistry& operator=(const NativeResourceRegistry&) = delete;

    [[nodiscard]] NativeHandleRef insert(NativeResourceOwner owner);
    [[nodiscard]] bool contains(NativeHandleRef ref) const;
    [[nodiscard]] NativeResourceBorrow borrow(NativeHandleRef ref) const;
    [[nodiscard]] NativeResourceOwner consume(NativeHandleRef ref);
    void close(NativeHandleRef ref);

private:
    mutable std::mutex mutex_;
    std::uint64_t nextId_{1};
    std::unordered_map<std::uint64_t, NativeResourceOwner> owners_;
};

NativeResourceRegistry& nativeResourceRegistry();

} // namespace zl
