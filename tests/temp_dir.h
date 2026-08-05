#pragma once

#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

// A unique temp directory per caller, created empty.
//
// ctest runs each test case as its own process, so fixtures sharing a fixed
// path race under `ctest -j`: one case's TearDown deletes the directory another
// case is still reading. The PID separates concurrent cases; the counter
// separates multiple directories within one case.
//
// Callers own the returned directory and should fs::remove_all() it in
// TearDown. Never remove its parent — that is the shared temp root.
//
// `root` names the filesystem to create under; tests needing a second device
// (EXDEV paths) pass e.g. "/dev/shm" and still get a per-process directory.
inline std::filesystem::path make_temp_dir(const std::string& label,
                                           const std::filesystem::path& root = {}) {
    static std::atomic<unsigned> counter{0};
    auto unique = std::to_string(::getpid()) + "-" + std::to_string(counter++);
    auto base = root.empty() ? std::filesystem::temp_directory_path() : root;
    auto dir = base / ("phig-" + label + "-" + unique);
    std::filesystem::create_directories(dir);
    return dir;
}
