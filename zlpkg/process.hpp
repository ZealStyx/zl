#pragma once

#include <string>

namespace zlpkg {

struct ProcessResult {
    int exitCode{-1};
    std::string output;
};

// Quote one command-line argument for the platform shell used by zlpkg.
std::string shellQuote(const std::string& arg);

// Run a command and capture combined stdout/stderr.
ProcessResult runCaptured(const std::string& command);

// Run a command inheriting the parent's stdio.
int run(const std::string& command);

} // namespace zlpkg
