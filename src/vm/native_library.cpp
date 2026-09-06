#include "zl/vm/native_library.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace zl {

void NativeLibrary::open(const std::string& path) {
    if (path.empty()) throw std::invalid_argument("native library path is empty");
    close();
#ifdef _WIN32
    handle_ = reinterpret_cast<void*>(LoadLibraryA(path.c_str()));
#else
    handle_ = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
    if (!handle_) throw std::runtime_error("failed to load native library: " + path);
}

void NativeLibrary::close() noexcept {
    if (!handle_) return;
#ifdef _WIN32
    FreeLibrary(reinterpret_cast<HMODULE>(handle_));
#else
    dlclose(handle_);
#endif
    handle_ = nullptr;
}

void* NativeLibrary::symbol(const std::string& name) const {
    if (!handle_) throw std::runtime_error("native library is not open");
    if (name.empty()) throw std::invalid_argument("native symbol name is empty");
#ifdef _WIN32
    void* p = reinterpret_cast<void*>(GetProcAddress(reinterpret_cast<HMODULE>(handle_), name.c_str()));
#else
    dlerror();
    void* p = dlsym(handle_, name.c_str());
#endif
    if (!p) throw std::runtime_error("native symbol not found: " + name);
    return p;
}

} // namespace zl
