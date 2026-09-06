#include "zl/vm/native_resource.hpp"

#include <cstdio>

namespace zl {

void NativeResourceOwner::reset() noexcept {
    const Handle handle = std::exchange(handle_, 0);
    if (handle != 0 && destroy_) {
        try { destroy_(handle); } catch (...) {}
    }
    if (lifetime_) lifetime_->alive.store(false, std::memory_order_release);
    destroy_ = {};
}

NativeHandleRef NativeResourceRegistry::insert(NativeResourceOwner owner) {
    if (!owner.valid()) throw std::runtime_error("cannot register an invalid native resource");
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t id = nextId_++;
    if (id == 0) id = nextId_++;
    owners_.emplace(id, std::move(owner));
    return NativeHandleRef{id};
}

bool NativeResourceRegistry::contains(NativeHandleRef ref) const {
    if (!ref.valid()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return owners_.find(ref.id) != owners_.end();
}

NativeResourceBorrow NativeResourceRegistry::borrow(NativeHandleRef ref) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owners_.find(ref.id);
    if (it == owners_.end()) throw std::runtime_error("invalid native handle");
    return NativeResourceBorrow(it->second);
}

NativeResourceOwner NativeResourceRegistry::consume(NativeHandleRef ref) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = owners_.find(ref.id);
    if (it == owners_.end()) throw std::runtime_error("invalid native handle");
    NativeResourceOwner owner = std::move(it->second);
    owners_.erase(it);
    return owner;
}

void NativeResourceRegistry::close(NativeHandleRef ref) {
    auto owner = consume(ref);
    (void)owner;
}


NativeCallbackRef NativeCallbackRegistry::insert(InvokeFn invoke) {
    if (!invoke) throw std::runtime_error("cannot register an empty native callback");
    std::lock_guard<std::mutex> lock(mutex_);
    std::uint64_t id = nextId_++;
    if (id == 0) id = nextId_++;
    auto lifetime = makeNativeCallbackLifetime();
    entries_.emplace(id, Entry{std::move(invoke), std::move(lifetime)});
    return NativeCallbackRef{id};
}

bool NativeCallbackRegistry::contains(NativeCallbackRef ref) const {
    if (!ref.valid()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.find(ref.id) != entries_.end();
}

void NativeCallbackRegistry::close(NativeCallbackRef ref) {
    std::shared_ptr<NativeCallbackLifetime> lifetime;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(ref.id);
        if (it == entries_.end()) throw std::runtime_error("invalid native callback");
        lifetime = it->second.lifetime;
    }
    lifetime->close();
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(ref.id);
}

std::int32_t NativeCallbackRegistry::invoke(NativeCallbackRef ref,
                                            const ZlNativeValue* args,
                                            std::uint32_t argc,
                                            ZlNativeValue* result,
                                            char* errorMessage,
                                            std::uint32_t errorMessageCapacity) {
    Entry entry;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = entries_.find(ref.id);
        if (it == entries_.end()) {
            if (errorMessage && errorMessageCapacity) {
                std::snprintf(errorMessage, errorMessageCapacity, "%s", "invalid native callback");
            }
            return -1;
        }
        entry = it->second;
    }
    try {
        auto lease = acquireNativeCallbackLease(entry.lifetime);
        return entry.invoke(args, argc, result, errorMessage, errorMessageCapacity);
    } catch (const std::exception& e) {
        if (errorMessage && errorMessageCapacity) {
            std::snprintf(errorMessage, errorMessageCapacity, "%s", e.what());
        }
        return -1;
    }
}

NativeCallbackRegistry& nativeCallbackRegistry() {
    static NativeCallbackRegistry registry;
    return registry;
}

NativeResourceRegistry& nativeResourceRegistry() {
    static NativeResourceRegistry registry;
    return registry;
}

} // namespace zl

namespace zl {

void NativeCallbackLifetime::close() noexcept {
    std::unique_lock<std::mutex> lock(mutex_);
    closed_ = true;
    cv_.wait(lock, [&] { return active_ == 0; });
}

bool NativeCallbackLifetime::tryAcquire() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) return false;
    ++active_;
    return true;
}

void NativeCallbackLifetime::release() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    if (active_ == 0) return;
    --active_;
    if (active_ == 0) cv_.notify_all();
}

bool NativeCallbackLifetime::closed() const noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
}

NativeCallbackLease::NativeCallbackLease(std::shared_ptr<NativeCallbackLifetime> lifetime) noexcept
    : lifetime_(std::move(lifetime)) {}

NativeCallbackLease::~NativeCallbackLease() {
    if (lifetime_) lifetime_->release();
}

NativeCallbackLease::NativeCallbackLease(NativeCallbackLease&& other) noexcept
    : lifetime_(std::move(other.lifetime_)) {}

NativeCallbackLease& NativeCallbackLease::operator=(NativeCallbackLease&& other) noexcept {
    if (this != &other) {
        if (lifetime_) lifetime_->release();
        lifetime_ = std::move(other.lifetime_);
    }
    return *this;
}

std::shared_ptr<NativeCallbackLifetime> makeNativeCallbackLifetime() {
    return std::make_shared<NativeCallbackLifetime>();
}

NativeCallbackLease acquireNativeCallbackLease(const std::shared_ptr<NativeCallbackLifetime>& lifetime) {
    if (!lifetime || !lifetime->tryAcquire())
        throw std::runtime_error("native callback lifetime is closed");
    return NativeCallbackLease(lifetime);
}

} // namespace zl
