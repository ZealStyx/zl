#include "zl/common/stdlib_version.hpp"

#include <fstream>
#include <sstream>

namespace zl::common {

StdlibVersionCheck checkStdlibVersion(const std::filesystem::path& root, const std::string& runtimeVersion) {
    const auto metadata = root / "VERSION";
    std::ifstream in(metadata);
    if (!in) {
        return {false, {}, "missing stdlib version metadata: " + metadata.string()};
    }

    std::string version;
    std::getline(in, version);
    if (!version.empty() && version.back() == '\r') version.pop_back();
    if (version.empty()) return {false, {}, "empty stdlib version metadata: " + metadata.string()};
    if (version != runtimeVersion) {
        return {false, version, "stdlib version " + version + " is incompatible with runtime " + runtimeVersion};
    }
    return {true, version, {}};
}

} // namespace zl::common
