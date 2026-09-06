#pragma once

#include <filesystem>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#endif

namespace zl::common {

inline std::filesystem::path executableDir(const char* argv0) {
#ifdef _WIN32
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) {
        return std::filesystem::path(buf).parent_path();
    }
#else
    std::error_code readEc;
    auto self = std::filesystem::read_symlink("/proc/self/exe", readEc);
    if (!readEc) return self.parent_path();
#endif

    std::error_code canonicalEc;
    auto resolved = std::filesystem::weakly_canonical(
        std::filesystem::path(argv0 ? argv0 : ""), canonicalEc);
    if (!canonicalEc) return resolved.parent_path();
    return std::filesystem::current_path();
}

} // namespace zl::common
