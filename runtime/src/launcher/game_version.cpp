// Game version and compatibility detection for Super Mario Bros. Wonder ExeFS, RomFS, and output folders.
#include "launcher/game_version.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace nx::launcher {
namespace {

constexpr uint64_t kWonderProgramId = 0x010015100B514000ull;
constexpr uintmax_t kMain100FlatSize = 0x359cc28ull;
constexpr uintmax_t kMain121FlatSize = 0x3728df8ull;

struct KnownExefsBuild {
    const char* id;
    const char* version;
    int expected_romfs_product;
    bool compatible;
};

constexpr KnownExefsBuild kKnownBuilds[] = {
    {"CD6E42AEE7934F4D8393CD43893AE25EDC83D338", "1.0.0", 100, true},
    {"FF773E90972D544EB79406EAA65396D53C43EFB9", "1.2.1", 120, true},
    {"F91868B88F60D3D5", "1.0.1", 101, false},
    {"B37398A1B68747B7", "1.2.0", 120, false},
};

bool is_file(const fs::path& p) {
    std::error_code ec;
    return fs::is_regular_file(p, ec);
}

bool is_dir(const fs::path& p) {
    std::error_code ec;
    return fs::is_directory(p, ec);
}

std::string to_hex_upper(const uint8_t* data, size_t n) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out(n * 2, '\0');
    for (size_t i = 0; i < n; ++i) {
        out[i * 2] = kHex[(data[i] >> 4) & 0xF];
        out[i * 2 + 1] = kHex[data[i] & 0xF];
    }
    return out;
}

std::string hex_u64_upper(uint64_t v) {
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(v));
    return std::string(buf);
}

bool read_nso_header(const fs::path& p, std::array<uint8_t, 0x60>& hdr) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.read(reinterpret_cast<char*>(hdr.data()), std::streamsize(hdr.size()));
    return f.gcount() == std::streamsize(hdr.size()) && std::memcmp(hdr.data(), "NSO0", 4) == 0;
}

bool read_npdm_program_id(const fs::path& npdm, uint64_t& out_pid) {
    std::ifstream f(npdm, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const auto sz = f.tellg();
    if (sz <= 0 || sz > (1 << 20)) return false;
    std::vector<uint8_t> raw(static_cast<size_t>(sz), 0);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(raw.data()), sz)) return false;
    for (size_t i = 0; i + 0x18 <= raw.size(); ++i) {
        if (std::memcmp(raw.data() + i, "ACI0", 4) == 0) {
            uint64_t pid = 0;
            std::memcpy(&pid, raw.data() + i + 0x10, sizeof pid);
            out_pid = pid;
            return true;
        }
    }
    return false;
}

bool parse_product_rstb_version(const std::string& filename, int& out_ver) {
    constexpr const char* kPrefix = "ResourceSizeTable.Product.";
    constexpr const char* kSuffix = ".rsizetable.zs";
    const size_t p_len = std::strlen(kPrefix);
    const size_t s_len = std::strlen(kSuffix);
    if (filename.size() <= p_len + s_len) return false;
    if (filename.compare(0, p_len, kPrefix) != 0) return false;
    if (filename.compare(filename.size() - s_len, s_len, kSuffix) != 0) return false;
    const std::string digits = filename.substr(p_len, filename.size() - p_len - s_len);
    if (digits.empty()) return false;
    int val = 0;
    for (char c : digits) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
        val = val * 10 + (c - '0');
    }
    out_ver = val;
    return true;
}

std::string read_key(const fs::path& p, const char* key) {
    std::ifstream f(p);
    std::string line;
    const std::string prefix = std::string(key) + " ";
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind(prefix, 0) == 0) return line.substr(prefix.size());
    }
    return {};
}

std::string str_u8(const fs::path& p) {
    const auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

fs::path path_u8(const std::string& s) {
    return fs::path(reinterpret_cast<const char8_t*>(s.c_str()));
}

}  // namespace

ExefsInfo inspect_exefs(const fs::path& dir) {
    ExefsInfo info;
    if (dir.empty()) {
        info.summary = "Not selected";
        return info;
    }
    info.present = true;
    if (!is_dir(dir)) {
        info.summary = "Folder not found";
        return info;
    }
    const fs::path main_path = dir / "main";
    const fs::path sdk_path = dir / "sdk";
    if (!is_file(main_path) || !is_file(sdk_path)) {
        info.summary = "Missing main or sdk";
        return info;
    }
    std::array<uint8_t, 0x60> main_hdr{}, sdk_hdr{};
    if (!read_nso_header(main_path, main_hdr) || !read_nso_header(sdk_path, sdk_hdr)) {
        info.summary = "Invalid NSO header (expected NSO0)";
        return info;
    }
    info.valid_nso = true;
    info.build_id = to_hex_upper(main_hdr.data() + 0x40, 20);

    uint64_t pid = 0;
    if (read_npdm_program_id(dir / "main.npdm", pid)) {
        info.program_id = hex_u64_upper(pid);
        if (pid != kWonderProgramId) {
            info.wrong_game = true;
            info.summary = "Wrong game (Title ID " + info.program_id + ", expected 010015100B514000)";
            return info;
        }
    }

    for (const KnownExefsBuild& b : kKnownBuilds) {
        const size_t blen = std::strlen(b.id);
        const bool match = blen == info.build_id.size()
                               ? (info.build_id == b.id)
                               : (info.build_id.compare(0, blen, b.id) == 0);
        if (match) {
            info.version = b.version;
            info.expected_romfs_product = b.expected_romfs_product;
            info.compatible = b.compatible;
            info.summary = b.compatible
                               ? (info.version + " (Compatible)")
                               : (info.version + " (Not compatible - supported: 1.0.0, 1.2.1)");
            return info;
        }
    }

    info.summary = "Unknown version (Build ID " + info.build_id.substr(0, 16) + " - supported: 1.0.0, 1.2.1)";
    return info;
}

RomfsInfo inspect_romfs(const fs::path& dir) {
    RomfsInfo info;
    if (dir.empty()) {
        info.summary = "Not selected";
        return info;
    }
    info.present = true;
    if (!is_dir(dir)) {
        info.summary = "Folder not found";
        return info;
    }
    const fs::path shader_dir = dir / "Shader";
    bool has_shader_zs = false;
    std::error_code ec;
    if (is_dir(shader_dir)) {
        for (auto& e : fs::directory_iterator(shader_dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            if (e.path().extension() == ".zs") {
                has_shader_zs = true;
                break;
            }
        }
    }
    if (!has_shader_zs) {
        info.summary = "Not a Wonder RomFS (no Shader/*.zs)";
        return info;
    }

    int best_product = -1;
    const fs::path res_dir = dir / "System" / "Resource";
    if (is_dir(res_dir)) {
        for (auto& e : fs::directory_iterator(res_dir, fs::directory_options::skip_permission_denied, ec)) {
            if (!e.is_regular_file(ec)) continue;
            int v = -1;
            if (parse_product_rstb_version(str_u8(e.path().filename()), v)) best_product = std::max(best_product, v);
        }
    }
    if (best_product < 0) {
        info.summary = "Missing ResourceSizeTable.Product.*.rsizetable.zs";
        return info;
    }

    info.valid = true;
    info.product = best_product;
    if (best_product == 100) {
        info.compatible = true;
        info.version_label = "1.0.0";
        info.summary = "1.0.0 (Product.100)";
    } else if (best_product == 120) {
        info.compatible = true;
        info.version_label = "1.2.0 / 1.2.1";
        info.summary = "1.2.0 / 1.2.1 (Product.120)";
    } else {
        info.compatible = false;
        info.version_label = "Product." + std::to_string(best_product);
        info.summary = info.version_label + " (Not compatible)";
    }
    return info;
}

PairInfo evaluate_pair(const ExefsInfo& ex, const RomfsInfo& rom) {
    PairInfo out;
    if (!ex.present || !rom.present) {
        out.state = PairState::Incomplete;
        if (!ex.present && !rom.present) {
            out.badge = "Not selected";
            out.detail = "Pick your ExeFS and RomFS folders to check game version and compatibility (1.0.0 or 1.2.1).";
        } else if (ex.present) {
            out.badge = ex.valid_nso ? ((ex.version.empty() ? "ExeFS" : ex.version) + " - pick RomFS")
                                     : ("ExeFS: " + ex.summary);
            out.detail = "ExeFS: " + ex.summary + "\nRomFS: Not selected";
        } else {
            out.badge = rom.valid ? (rom.version_label + " - pick ExeFS") : ("RomFS: " + rom.summary);
            out.detail = "ExeFS: Not selected\nRomFS: " + rom.summary;
        }
        return out;
    }
    if (ex.wrong_game) {
        out.state = PairState::WrongGame;
        out.badge = "Wrong game";
        out.detail = ex.summary;
        return out;
    }
    if (!ex.valid_nso || !rom.valid) {
        out.state = PairState::Invalid;
        out.badge = "Invalid files";
        out.detail = "ExeFS: " + ex.summary + "\nRomFS: " + rom.summary;
        return out;
    }
    if (!ex.compatible) {
        out.state = PairState::Unsupported;
        out.badge = (ex.version.empty() ? std::string("Unknown") : ex.version) + " (Not compatible)";
        out.detail = ex.summary;
        return out;
    }
    if (!rom.compatible) {
        out.state = PairState::Unsupported;
        out.badge = rom.version_label + " (Not compatible)";
        out.detail = rom.summary;
        return out;
    }
    if (ex.expected_romfs_product != rom.product) {
        out.state = PairState::Mismatch;
        out.badge = ex.version + " + " + rom.version_label + " (Mismatch)";
        out.detail = "Game version mismatch: ExeFS is " + ex.version + " (expects RomFS Product." +
                     std::to_string(ex.expected_romfs_product) + "), while RomFS is " + rom.version_label +
                     " (Product." + std::to_string(rom.product) + ").";
        return out;
    }
    out.state = PairState::Compatible;
    out.badge = ex.version + " (Compatible)";
    out.detail = "Detected Super Mario Bros. Wonder " + ex.version +
                 " (Build ID " + ex.build_id.substr(0, 16) + "). Compatible and ready to set up.";
    return out;
}

OutputInfo inspect_output_dir(const fs::path& dir, const std::string& gamelib_filename,
                              const std::string& fallback_gamelib_path) {
    OutputInfo out;
    if (dir.empty() || !is_dir(dir)) {
        out.missing_reason = "The game files aren't set up yet.";
        return out;
    }

    const fs::path local_lib = dir / "game" / path_u8(gamelib_filename);
    out.has_gamelib = is_file(local_lib) || (!fallback_gamelib_path.empty() && is_file(path_u8(fallback_gamelib_path)));
    out.has_analysis = is_file(dir / "analysis" / "main.bin") && is_file(dir / "analysis" / "sdk.bin");
#if defined(__ANDROID__)
    out.has_shaders = is_file(dir / "spirv" / "results.tsv");
#else
    out.has_shaders = is_file(dir / "shader_programs.bin") && is_file(dir / "spirv" / "results.tsv");
#endif
    out.has_romfs = is_dir(dir / "extract" / "romfs" / "Shader");

    out.installed_version = read_key(dir / "game" / "build-id.txt", "version");
    if (out.installed_version.empty() && is_file(dir / "analysis" / "main.bin")) {
        std::error_code ec;
        const uintmax_t sz = fs::file_size(dir / "analysis" / "main.bin", ec);
        if (!ec) {
            if (sz == kMain100FlatSize) out.installed_version = "1.0.0";
            else if (sz == kMain121FlatSize) out.installed_version = "1.2.1";
        }
    }
    if (out.installed_version.empty() && out.has_romfs) {
        const RomfsInfo r = inspect_romfs(dir / "extract" / "romfs");
        if (r.valid) out.installed_version = r.version_label;
    }

    if (!out.has_gamelib) out.missing_reason = "The game code isn't built yet.";
    else if (!out.has_analysis) out.missing_reason = "The game's executable isn't read yet.";
    else if (!out.has_shaders) out.missing_reason = "The shaders aren't translated yet.";
    else if (!out.has_romfs) out.missing_reason = "The game's files aren't set up yet.";
    else out.ready = true;
    return out;
}

}  // namespace nx::launcher
