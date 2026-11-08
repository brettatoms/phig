#pragma once

#include <sys/wait.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
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

// What one run of the binary did: its exit status, and stdout and stderr
// interleaved as the terminal would show them. A refusal writes its reason to
// stderr and its progress to stdout, so a test that read only one of the two
// would miss half of what it is asserting about.
struct PhigRun {
    int exit_code;
    std::string output;
};

// Run `phig <args>` with HOME and all four XDG directories pointed inside
// `scratch`.
//
// Redirecting them is not tidiness. Without it the child reads the developer's
// ~/.config/phig/config.ini — which may set db, models_dir or thumbs_dir and
// silently decide what the test measured — takes its advisory lock on the real
// collection's sidecar, and writes thumbnails and log lines into the real
// cache. It also fixes where the child looks for the face models
// (models_dir() is cache_dir() + "/models"), which is what lets a test drive
// the models-missing path on a machine that has them installed.
//
// `args` is appended to the binary path as a shell string, so quote anything
// holding a space at the call site. The caller supplies `binary` from
// phig_binary_path() and has already skipped if it came back empty.
inline PhigRun run_phig(const std::filesystem::path& binary, const std::filesystem::path& scratch,
                        const std::string& args) {
    namespace fs = std::filesystem;
    fs::path env_root = scratch / "xdg";
    for (const char* sub : {"config", "data", "cache", "state"}) {
        fs::create_directories(env_root / sub);
    }
    fs::path out_path = scratch / "phig-run-output.txt";

    std::string cmd =
        "HOME='" + env_root.string() + "' XDG_CONFIG_HOME='" + (env_root / "config").string() +
        "' XDG_DATA_HOME='" + (env_root / "data").string() + "' XDG_CACHE_HOME='" +
        (env_root / "cache").string() + "' XDG_STATE_HOME='" + (env_root / "state").string() +
        "' " + binary.string() + " " + args + " > '" + out_path.string() + "' 2>&1";
    int rc = std::system(cmd.c_str());

    std::ifstream in(out_path);
    std::string output((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::error_code ec;
    fs::remove(out_path, ec);

    return PhigRun{WIFEXITED(rc) ? WEXITSTATUS(rc) : -1, output};
}

// Where a child of run_phig() takes its advisory writer lock when it is given no
// --catalog: the default catalog is data_dir(), inside the redirected XDG_DATA_HOME.
inline std::filesystem::path phig_scratch_lock_path(const std::filesystem::path& scratch) {
    return scratch / "xdg" / "data" / "phig" / "phig.db.lock";
}

// The database a child of run_phig() uses when it is given no --catalog.
inline std::filesystem::path phig_scratch_db_path(const std::filesystem::path& scratch) {
    return scratch / "xdg" / "data" / "phig" / "phig.db";
}
