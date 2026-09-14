#pragma once

#include <string>
#include <vector>

namespace zlpkg {

struct ProcessResult {
    int exitCode{-1};
    std::string output;
};

// Run argv[0] with arguments argv[1..] and capture combined stdout/stderr.
//
// The child is launched with an argv array (execvp / CreateProcess with a
// quoted command line) - NEVER through a shell. Manifest-controlled strings
// (git URLs, refs, package names, paths, program arguments) are passed as
// single argv elements, so no quoting or escaping rules can turn them into
// command syntax. Do not reintroduce a string-command API here: a shell
// string is exactly how command injection happens in a package manager.
ProcessResult runCaptured(const std::vector<std::string>& argv);

// Run argv[0] with arguments argv[1..], inheriting the parent's stdio.
int run(const std::vector<std::string>& argv);

} // namespace zlpkg
