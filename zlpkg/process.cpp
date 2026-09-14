#include "process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace zlpkg {
namespace {

#ifdef _WIN32
// Quotes one argument for a CreateProcess command line (which the child's
// CRT parses back into argv with the standard MS rules). Only backslashes
// that immediately precede a double quote (or the closing quote) need
// doubling; a trailing backslash run must be doubled so it cannot escape
// the closing quote.
std::string quoteWindowsArg(const std::string& arg) {
    std::string out = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
            continue;
        }
        out.append(backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string windowsCommandLine(const std::vector<std::string>& argv) {
    std::string line;
    for (const auto& arg : argv) {
        if (!line.empty()) line += ' ';
        line += quoteWindowsArg(arg);
    }
    return line;
}
#endif

#ifndef _WIN32
[[noreturn]] void execChild(char* const argvPtr[]) {
    ::execvp(argvPtr[0], argvPtr);
    // execvp only returns on failure; nothing has executed yet, so the exit
    // status tells the parent which problem it was (127 = not found).
    ::_exit(errno == ENOENT ? 127 : 126);
}

std::vector<char*> argvForExec(const std::vector<std::string>& argv) {
    std::vector<char*> ptrs;
    ptrs.reserve(argv.size() + 1);
    for (const auto& arg : argv) ptrs.push_back(const_cast<char*>(arg.c_str()));
    ptrs.push_back(nullptr);
    return ptrs;
}
#endif

} // namespace

ProcessResult runCaptured(const std::vector<std::string>& argv) {
    if (argv.empty()) return {-1, "zlpkg: runCaptured: empty argv"};

#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readHandle = nullptr;
    HANDLE writeHandle = nullptr;
    if (!CreatePipe(&readHandle, &writeHandle, &sa, 0)) {
        return {-1, "failed to create output pipe"};
    }
    if (!SetHandleInformation(readHandle, HANDLE_FLAG_INHERIT, 0)) {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        return {-1, "failed to configure output pipe"};
    }

    std::string commandLine = windowsCommandLine(argv);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeHandle;
    si.hStdError = writeHandle;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &si, &pi)) {
        CloseHandle(readHandle);
        CloseHandle(writeHandle);
        return {-1, "failed to launch: " + commandLine};
    }

    CloseHandle(writeHandle);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(readHandle, nullptr, 0, nullptr, &available, nullptr)) break;
        if (available == 0) {
            DWORD wait = WaitForSingleObject(pi.hProcess, 25);
            if (wait == WAIT_OBJECT_0) {
                while (true) {
                    DWORD n = 0;
                    if (!ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) || n == 0) break;
                    output.append(buffer.data(), n);
                }
                break;
            }
            continue;
        }
        DWORD n = 0;
        if (!ReadFile(readHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &n, nullptr) || n == 0) break;
        output.append(buffer.data(), n);
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(readHandle);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return {static_cast<int>(exitCode), output};
#else
    int pipeFds[2];
    if (::pipe(pipeFds) != 0) return {-1, "failed to create output pipe"};

    std::vector<char*> argvPtr = argvForExec(argv);
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(pipeFds[0]);
        ::close(pipeFds[1]);
        return {-1, "failed to fork"};
    }
    if (pid == 0) {
        // Child: both stdout and stderr feed the pipe the parent reads.
        ::close(pipeFds[0]);
        if (::dup2(pipeFds[1], STDOUT_FILENO) < 0) ::_exit(126);
        if (::dup2(pipeFds[1], STDERR_FILENO) < 0) ::_exit(126);
        if (pipeFds[1] != STDOUT_FILENO && pipeFds[1] != STDERR_FILENO) ::close(pipeFds[1]);
        execChild(argvPtr.data());
    }

    ::close(pipeFds[1]);
    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const ssize_t n = ::read(pipeFds[0], buffer.data(), buffer.size());
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        output.append(buffer.data(), static_cast<std::size_t>(n));
    }
    ::close(pipeFds[0]);

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return {-1, "failed to wait for child"};
    }
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output};
#endif
}

int run(const std::vector<std::string>& argv) {
    if (argv.empty()) return -1;

#ifdef _WIN32
    std::string commandLine = windowsCommandLine(argv);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr, TRUE, 0, nullptr,
                        nullptr, &si, &pi)) {
        return -1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return static_cast<int>(exitCode);
#else
    std::vector<char*> argvPtr = argvForExec(argv);
    const pid_t pid = ::fork();
    if (pid < 0) return -1;
    if (pid == 0) execChild(argvPtr.data());

    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return 1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} // namespace zlpkg
