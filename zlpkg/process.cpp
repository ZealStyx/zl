#include "process.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace zlpkg {

std::string shellQuote(const std::string& arg) {
#ifdef _WIN32
    // zlpkg launches child processes through the Windows command shell
    // (`system`/`_popen`). Backslashes are ordinary Windows path separators
    // and must not be escaped merely because the argument is quoted. The old
    // implementation escaped every backslash, corrupting paths such as
    // `D:\zl_language\build\zl_language.exe` and causing cmd.exe to report
    // the misleading "The filename, directory name, or volume label syntax
    // is incorrect." error.
    //
    // Quote according to the Windows command-line parsing rules: only
    // backslashes immediately preceding a literal quote (or the closing
    // quote) need doubling.
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
    // A trailing run of backslashes would otherwise escape the closing quote.
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
#else
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"' || c == '\\') out += '\\';
        out += c;
    }
    out += '"';
    return out;
#endif
}

ProcessResult runCaptured(const std::string& command) {
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

    std::string commandLine = command;
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
        return {-1, "failed to launch: " + command};
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
    std::string fullCommand = command + " 2>&1";
    FILE* pipe = popen(fullCommand.c_str(), "r");
    if (!pipe) return {-1, "failed to launch: " + command};

    std::string output;
    std::array<char, 4096> buffer{};
    while (true) {
        const std::size_t n = std::fread(buffer.data(), 1, buffer.size(), pipe);
        if (n == 0) break;
        output.append(buffer.data(), n);
    }

    const int status = pclose(pipe);
    const int exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {exitCode, output};
#endif
}

int run(const std::string& command) {
#ifdef _WIN32
    std::string commandLine = command;
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
    const int status = std::system(command.c_str());
    if (status == -1) return 1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : 1;
#endif
}

} // namespace zlpkg
