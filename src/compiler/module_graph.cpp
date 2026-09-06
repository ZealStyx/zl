#include "zl/compiler/module_graph.hpp"

namespace zl {

bool ModuleGraph::isResolved(const std::string& dottedName) const {
    return resolvedByDottedName_.find(dottedName) != resolvedByDottedName_.end();
}

void ModuleGraph::recordResolved(const std::string& dottedName,
                                 const std::filesystem::path& filePath) {
    resolvedByDottedName_.emplace(dottedName, filePath);
}

bool ModuleGraph::beginLoading(const std::string& dottedName) {
    return loading_.insert(dottedName).second;
}

bool ModuleGraph::isLoading(const std::string& dottedName) const {
    return loading_.find(dottedName) != loading_.end();
}

void ModuleGraph::endLoading(const std::string& dottedName) {
    loading_.erase(dottedName);
}

} // namespace zl
