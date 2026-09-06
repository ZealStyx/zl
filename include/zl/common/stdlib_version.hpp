#pragma once

#include <filesystem>
#include <string>

namespace zl::common {

struct StdlibVersionCheck {
    bool compatible{false};
    std::string version;
    std::string error;
};

StdlibVersionCheck checkStdlibVersion(const std::filesystem::path& root, const std::string& runtimeVersion);

} // namespace zl::common
