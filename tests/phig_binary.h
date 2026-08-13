#pragma once

#include <cstdint>
#include <filesystem>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

// The built `phig` binary, or an empty path when it is not there.
//
// cli/main.cpp is not linked into phig-tests, so a guard that lives in argv
// parsing can only be exercised by running the binary as a subprocess. Looked
// for beside the running test binary first — /proc/self/exe exists only on
// Linux, macOS needs _NSGetExecutablePath — then at ./build/phig for a run
// started from the source root. Callers skip when this comes back empty.
inline std::filesystem::path phig_binary_path() {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir;
#if defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        fs::path self = fs::canonical(buf, ec);
        if (!ec) dir = self.parent_path();
    }
#else
    fs::path self = fs::canonical("/proc/self/exe", ec);
    if (!ec) dir = self.parent_path();
#endif
    if (!dir.empty() && fs::exists(dir / "phig")) return dir / "phig";
    if (fs::exists("./build/phig")) return "./build/phig";
    return {};
}
