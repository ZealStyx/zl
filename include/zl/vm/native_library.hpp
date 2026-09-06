#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

namespace zl {

class NativeLibrary final {
public:
    NativeLibrary() = default;
    explicit NativeLibrary(const std::string& path) { open(path); }
    NativeLibrary(const NativeLibrary&) = delete;
    NativeLibrary& operator=(const NativeLibrary&) = delete;
    NativeLibrary(NativeLibrary&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }
    NativeLibrary& operator=(NativeLibrary&& other) noexcept {
        if (this != &other) { close(); handle_ = other.handle_; other.handle_ = nullptr; }
        return *this;
    }
    ~NativeLibrary() { close(); }

    void open(const std::string& path);
    void close() noexcept;
    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    [[nodiscard]] void* symbol(const std::string& name) const;

private:
    void* handle_{nullptr};
};

} // namespace zl
