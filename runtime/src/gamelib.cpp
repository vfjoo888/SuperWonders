// The program's side of the game code library (nx/gamelib.h): find it, load it, check it was built for this program,
// hand it the runtime's functions and take the module tables.
//
// Where it is, first found:
//   1. NX_GAMELIB=<path>                           (tests, a library kept elsewhere)
//   2. next to the program                         (a developer's build folder: its own library, never a stale one
//                                                   left in a data root that several builds share)
//   3. <data root>/game/<file>                     (what the player's build makes: it holds their game, so it lives
//                                                   with their data, not with the program an update replaces; a
//                                                   release has no library next to the program)
// <file> is wonder_game.dll on Windows, libwonder_game.so on Linux.
#include <cstdlib>
#include <string>
#include <vector>

#include <nx/gamelib.h>
#include <nx/runtime.h>

#include "platform.h"

#if !NX_GAME_LIBRARY
extern const nx::ModuleInfo nx_module_main;
extern const nx::ModuleInfo nx_module_sdk;
#elif defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace nx {

#if defined(__ANDROID__)
namespace plat { const char* android_internal_data_dir(); }
#endif

#if !NX_GAME_LIBRARY

const GameLib& game_lib() {
    static const GameLib lib = {gamelib_layout(), NX_GAME_NAME, &nx_module_main, &nx_module_sdk};
    return lib;
}

#else

namespace {

bool exists(const std::string& path) {
    FILE* f = plat::fopen_utf8(path, "rb");
    if (!f) return false;
    std::fclose(f);
    return true;
}

}  // namespace

std::string program_dir() {
#if defined(_WIN32)
    std::wstring w(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, w.data(), DWORD(w.size()));
    if (!n || n >= w.size()) return ".";
    w.resize(n);
    w.resize(w.find_last_of(L"\\/") == std::wstring::npos ? 0 : w.find_last_of(L"\\/"));
    int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), int(w.size()), s.data(), len, nullptr, nullptr);
    return s.empty() ? "." : s;
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    std::string s(buf, size_t(n));
    size_t slash = s.find_last_of('/');
    return slash == std::string::npos ? "." : s.substr(0, slash);
#endif
}

namespace {

// The library's handle and bind function, or null with why filled in.
#if defined(_WIN32)
using LibHandle = HMODULE;
void close_library(LibHandle h) { if (h) FreeLibrary(h); }
#else
using LibHandle = void*;
void close_library(LibHandle h) { if (h) dlclose(h); }
#endif

NxGameLibBind open_library(const std::string& path, std::string& why, LibHandle* out_handle = nullptr) {
#if defined(_WIN32)
    int len = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(size_t(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, w.data(), len);
    // Its own dependencies (libc++.dll, libunwind.dll) come from the program's folder, which Windows searches first.
    HMODULE m = LoadLibraryW(w.c_str());
    if (!m) {
        why = "Windows could not load it (error " + std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    auto bind = reinterpret_cast<NxGameLibBind>(reinterpret_cast<void*>(GetProcAddress(m, NX_GAMELIB_BIND_NAME)));
#else
    void* m = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!m) {
        const char* e = dlerror();
        why = e ? e : "dlopen failed";
        return nullptr;
    }
    auto bind = reinterpret_cast<NxGameLibBind>(dlsym(m, NX_GAMELIB_BIND_NAME));
#endif
    if (!bind) {
        why = "it is not a game code library (no " NX_GAMELIB_BIND_NAME ")";
        close_library(m);
        return nullptr;
    }
    if (out_handle) *out_handle = m;
    return bind;
}

#if defined(__ANDROID__)
std::string stage_android_library(const std::string& src) {
    const char* internal = plat::android_internal_data_dir();
    if (!internal || !*internal) return src;
    const std::string dst = std::string(internal) + "/" NX_GAMELIB_FILE;
    struct stat st_src{}, st_dst{};
    if (stat(src.c_str(), &st_src) != 0) return src;
    if (stat(dst.c_str(), &st_dst) == 0 && st_dst.st_size == st_src.st_size && st_dst.st_mtime >= st_src.st_mtime)
        return dst;
    FILE* in = std::fopen(src.c_str(), "rb");
    if (!in) return src;
    const std::string tmp = dst + ".tmp";
    FILE* out = std::fopen(tmp.c_str(), "wb");
    if (!out) { std::fclose(in); return src; }
    char buf[65536];
    size_t n = 0;
    bool ok = true;
    while ((n = std::fread(buf, 1, sizeof buf, in)) > 0) {
        if (std::fwrite(buf, 1, n, out) != n) { ok = false; break; }
    }
    std::fclose(in);
    if (std::fflush(out) != 0) ok = false;
    std::fclose(out);
    if (!ok || std::rename(tmp.c_str(), dst.c_str()) != 0) return src;
    return dst;
}
#endif

std::vector<std::string> places() {
    std::vector<std::string> places;
    if (const char* p = std::getenv("NX_GAMELIB"); p && *p) places.push_back(p);
#if defined(__ANDROID__)
    places.push_back(NX_GAMELIB_FILE);  // nativeLibraryDir inside the APK if bundled
    places.push_back(plat::data_root() + "/game/" NX_GAMELIB_FILE);
#else
    places.push_back(program_dir() + "/" NX_GAMELIB_FILE);
    places.push_back(plat::data_root() + "/game/" NX_GAMELIB_FILE);
#endif
    return places;
}

const GameLib* load() {

    std::string tried;
    for (const std::string& candidate : places()) {
        std::string path = candidate;
#if defined(__ANDROID__)
        if (path.find('/') != std::string::npos) {
            if (!exists(path)) {
                tried += "\n  " + path + ": not there";
                continue;
            }
            path = stage_android_library(path);
        }
#else
        if (!exists(path)) {
            tried += "\n  " + path + ": not there";
            continue;
        }
#endif
        std::string why;
        NxGameLibBind bind = open_library(path, why);
#if defined(__ANDROID__)
        if (!bind && path.find('/') == std::string::npos) {
            tried += "\n  " + path + ": " + why;
            continue;
        }
#endif
        if (!bind) fatal("The game code library %s can't be used: %s.", path.c_str(), why.c_str());
        static const GameLibHost host = {gamelib_layout(), &call, &unimpl, &svc, &mrs, &msr, &trace_sp};
        const GameLib* lib = bind(&host);
        if (!lib || lib->layout != gamelib_layout())
            fatal("The game code in %s was built for another version of this program (layout %llx, this one %llx). "
                  "Build the game code again.",
                  path.c_str(), lib ? (unsigned long long)lib->layout : 0ull, (unsigned long long)gamelib_layout());
        if (std::string(lib->game) != NX_GAME_NAME)
            fatal("%s is the game code of \"%s\", not \"%s\".", path.c_str(), lib->game, NX_GAME_NAME);
        if (!lib->main || !lib->sdk) fatal("%s has no module tables.", path.c_str());
        log("game code: %s (%zu + %zu functions)", path.c_str(), lib->main->func_count, lib->sdk->func_count);
        return lib;
    }
    fatal("The game code isn't built yet: no " NX_GAMELIB_FILE " was found.%s\nBuild it from your own game files first.",
          tried.c_str());
}

}  // namespace

const GameLib& game_lib() {
    static const GameLib* lib = load();
    return *lib;
}

std::string game_lib_path() {
    for (const std::string& path : places())
        if (exists(path)) return path;
    return {};
}

bool probe_game_lib(const std::string& path, std::string& why) {
    if (!exists(path)) {
        why = "The game code library is missing.";
        return false;
    }
    LibHandle handle = nullptr;
    NxGameLibBind bind = open_library(path, why, &handle);
    if (!bind) return false;
    const GameLib* lib = bind(nullptr);
    bool ok = true;
    if (!lib || lib->layout != gamelib_layout()) {
        why = "The game code library was built for another ABI version of SuperWonders.";
        ok = false;
    } else if (!lib->game || std::string(lib->game) != NX_GAME_NAME) {
        why = "The game code library belongs to another game.";
        ok = false;
    } else if (!lib->main || !lib->sdk) {
        why = "The game code library has no module tables.";
        ok = false;
    }
    close_library(handle);
    return ok;
}

#endif

}  // namespace nx
