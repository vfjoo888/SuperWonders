#include "ttf_cmap_remap.h"

#include <algorithm>
#include <cstring>
#include <map>

namespace nx {

namespace {

uint16_t rd16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) << 8 | uint16_t(p[1]));
}

uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}

void wr16(uint8_t* p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

void wr32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}

void push16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void push32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

uint32_t table_checksum(const uint8_t* data, size_t len) {
    uint32_t sum = 0;
    const size_t full = len & ~size_t(3);
    for (size_t i = 0; i < full; i += 4) sum += rd32(data + i);
    if (full < len) {
        uint8_t tail[4]{};
        std::memcpy(tail, data + full, len - full);
        sum += rd32(tail);
    }
    return sum;
}

bool parse_format4(const uint8_t* sub, size_t avail, std::map<uint32_t, uint16_t>& out) {
    if (avail < 16) return false;
    const uint16_t len = rd16(sub + 2);
    if (len > avail || len < 16) return false;
    const uint16_t seg_x2 = rd16(sub + 6);
    if ((seg_x2 & 1u) || seg_x2 < 2) return false;
    const size_t seg_count = seg_x2 / 2;
    const size_t arrays_end = 16 + 8 * seg_count;
    if (arrays_end > len) return false;

    const size_t end_off = 14;
    const size_t start_off = 16 + 2 * seg_count;
    const size_t delta_off = 16 + 4 * seg_count;
    const size_t range_off = 16 + 6 * seg_count;

    out.clear();
    for (size_t s = 0; s < seg_count; ++s) {
        const uint16_t ec = rd16(sub + end_off + 2 * s);
        const uint16_t sc = rd16(sub + start_off + 2 * s);
        const uint16_t delta = rd16(sub + delta_off + 2 * s);
        const uint16_t iro = rd16(sub + range_off + 2 * s);
        if (sc > ec) return false;
        if (sc == 0xFFFF && ec == 0xFFFF) break;
        for (uint32_t cp = sc; cp <= ec; ++cp) {
            uint16_t gid = 0;
            if (iro == 0) {
                gid = uint16_t((cp + delta) & 0xFFFFu);
            } else {
                const size_t idx_off = range_off + 2 * s + iro + 2 * (cp - sc);
                if (idx_off + 2 > len) return false;
                const uint16_t raw = rd16(sub + idx_off);
                if (raw != 0) gid = uint16_t((raw + delta) & 0xFFFFu);
            }
            if (gid != 0) out[cp] = gid;
        }
    }
    return !out.empty();
}

bool parse_format12(const uint8_t* sub, size_t avail, std::map<uint32_t, uint16_t>& out) {
    if (avail < 16) return false;
    const uint32_t len = rd32(sub + 4);
    if (len > avail || len < 16) return false;
    const uint32_t groups = rd32(sub + 12);
    if (16ull + uint64_t(groups) * 12ull > len) return false;

    out.clear();
    for (uint32_t g = 0; g < groups; ++g) {
        const uint8_t* p = sub + 16 + size_t(g) * 12;
        const uint32_t sc = rd32(p);
        const uint32_t ec = rd32(p + 4);
        const uint32_t sg = rd32(p + 8);
        if (ec < sc || ec - sc > 0x10FFFFu) return false;
        for (uint32_t cp = sc; cp <= ec; ++cp) {
            const uint32_t gid = sg + (cp - sc);
            if (gid != 0 && gid <= 0xFFFFu) out[cp] = uint16_t(gid);
        }
    }
    return !out.empty();
}

bool parse_unicode_cmap(const uint8_t* table, size_t len, std::map<uint32_t, uint16_t>& out) {
    if (len < 4) return false;
    const uint16_t num_sub = rd16(table + 2);
    if (4ull + uint64_t(num_sub) * 8ull > len) return false;

    int best_rank = -1;
    std::map<uint32_t, uint16_t> best_map;
    for (uint16_t i = 0; i < num_sub; ++i) {
        const uint8_t* rec = table + 4 + size_t(i) * 8;
        const uint16_t pid = rd16(rec);
        const uint16_t eid = rd16(rec + 2);
        const uint32_t off = rd32(rec + 4);
        if (off + 2 > len) continue;
        const bool is_unicode = (pid == 0) || (pid == 3 && (eid == 1 || eid == 10));
        if (!is_unicode) continue;
        const uint16_t fmt = rd16(table + off);
        int rank = -1;
        std::map<uint32_t, uint16_t> candidate;
        if (fmt == 12 && parse_format12(table + off, len - off, candidate)) {
            rank = (pid == 3 && eid == 10) ? 4 : 3;
        } else if (fmt == 4 && parse_format4(table + off, len - off, candidate)) {
            rank = (pid == 3 && eid == 1) ? 2 : 1;
        }
        if (rank > best_rank) {
            best_rank = rank;
            best_map = std::move(candidate);
        }
    }
    if (best_rank < 0) return false;
    out = std::move(best_map);
    return true;
}

bool build_format4(const std::map<uint32_t, uint16_t>& cmap, std::vector<uint8_t>& out) {
    struct Seg {
        uint16_t start, end, delta;
    };
    std::vector<Seg> segs;
    for (const auto& [cp32, gid] : cmap) {
        if (cp32 >= 0xFFFFu || gid == 0) continue;
        const uint16_t cp = uint16_t(cp32);
        if (!segs.empty() && uint32_t(segs.back().end) + 1u == cp &&
            uint16_t((uint32_t(cp) + segs.back().delta) & 0xFFFFu) == gid) {
            segs.back().end = cp;
        } else {
            segs.push_back({cp, cp, uint16_t((uint32_t(gid) - uint32_t(cp)) & 0xFFFFu)});
        }
    }
    segs.push_back({0xFFFFu, 0xFFFFu, 1u});

    const size_t seg_count = segs.size();
    const size_t total = 16 + 8 * seg_count;
    if (total > 0xFFFFu || seg_count > 0x3FFFu) return false;

    uint16_t max_pow2 = 1;
    uint16_t entry_selector = 0;
    while (size_t(max_pow2 << 1) <= seg_count) {
        max_pow2 <<= 1;
        ++entry_selector;
    }
    const uint16_t seg_x2 = uint16_t(seg_count * 2);
    const uint16_t search_range = uint16_t(2 * max_pow2);
    const uint16_t range_shift = uint16_t(seg_x2 - search_range);

    out.clear();
    out.reserve(total);
    push16(out, 4);
    push16(out, uint16_t(total));
    push16(out, 0);
    push16(out, seg_x2);
    push16(out, search_range);
    push16(out, entry_selector);
    push16(out, range_shift);
    for (const auto& s : segs) push16(out, s.end);
    push16(out, 0);  // reservedPad
    for (const auto& s : segs) push16(out, s.start);
    for (const auto& s : segs) push16(out, s.delta);
    for (size_t i = 0; i < seg_count; ++i) push16(out, 0);  // idRangeOffset
    return out.size() == total;
}

void build_format12(const std::map<uint32_t, uint16_t>& cmap, std::vector<uint8_t>& out) {
    struct Group {
        uint32_t start, end, start_gid;
    };
    std::vector<Group> groups;
    for (const auto& [cp, gid] : cmap) {
        if (gid == 0) continue;
        if (!groups.empty() && groups.back().end + 1u == cp &&
            groups.back().start_gid + (cp - groups.back().start) == gid) {
            groups.back().end = cp;
        } else {
            groups.push_back({cp, cp, gid});
        }
    }

    const uint32_t total = uint32_t(16 + groups.size() * 12);
    out.clear();
    out.reserve(total);
    push16(out, 12);
    push16(out, 0);
    push32(out, total);
    push32(out, 0);
    push32(out, uint32_t(groups.size()));
    for (const auto& g : groups) {
        push32(out, g.start);
        push32(out, g.end);
        push32(out, g.start_gid);
    }
}

}  // namespace

bool remap_button_font_cmap(std::vector<uint8_t>& ttf, int style, const std::array<int, 16>& action_to_physical,
                            int title_quit_action) {
    if (ttf.size() < 12) return false;
    const uint16_t num_tables = rd16(ttf.data() + 4);
    if (12ull + uint64_t(num_tables) * 16ull > ttf.size()) return false;

    size_t cmap_rec = 0, head_rec = 0;
    bool has_cmap = false, has_head = false;
    for (uint16_t i = 0; i < num_tables; ++i) {
        const size_t rec = 12 + size_t(i) * 16;
        if (std::memcmp(ttf.data() + rec, "cmap", 4) == 0) cmap_rec = rec, has_cmap = true;
        else if (std::memcmp(ttf.data() + rec, "head", 4) == 0) head_rec = rec, has_head = true;
    }
    if (!has_cmap || !has_head) return false;

    const uint32_t cmap_off = rd32(ttf.data() + cmap_rec + 8);
    const uint32_t cmap_len = rd32(ttf.data() + cmap_rec + 12);
    const uint32_t head_off = rd32(ttf.data() + head_rec + 8);
    const uint32_t head_len = rd32(ttf.data() + head_rec + 12);
    if (uint64_t(cmap_off) + cmap_len > ttf.size() || uint64_t(head_off) + head_len > ttf.size() || head_len < 12)
        return false;

    std::map<uint32_t, uint16_t> cmap;
    if (!parse_unicode_cmap(ttf.data() + cmap_off, cmap_len, cmap)) return false;

    const int clamped_style = std::clamp(style, 1, 3);
    const uint32_t base = 0xE200u + uint32_t(clamped_style - 1) * 16u;
    std::array<uint16_t, 16> phys_gids{};
    for (size_t i = 0; i < 16; ++i) {
        auto it = cmap.find(base + uint32_t(i));
        if (it == cmap.end() || it->second == 0) return false;
        phys_gids[i] = it->second;
    }

    struct Mapping {
        size_t action;
        uint32_t codepoint;
    };
    static constexpr Mapping kSwitchGlyphs[] = {
        {0, 0xE0E0},  {0, 0xE0A0},  // A
        {1, 0xE0E1},  {1, 0xE0A1},  // B
        {2, 0xE0E2},  {2, 0xE0A2},  // X
        {3, 0xE0E3},  {3, 0xE0A3},  // Y
        {4, 0xE0E4},  {4, 0xE0A4},  // L
        {5, 0xE0E5},  {5, 0xE0A5},  // R
        {6, 0xE0E6},  {6, 0xE0A6},  // ZL
        {7, 0xE0E7},  {7, 0xE0A7},  // ZR
        {8, 0xE0EF},                // +
        {9, 0xE0F0},                // -
        {12, 0xE0AE},               // D-pad Left
        {13, 0xE0AD},               // D-pad Up
        {14, 0xE0AB},               // D-pad Right
        {15, 0xE0AC},               // D-pad Down
    };
    for (const auto& m : kSwitchGlyphs) {
        const int phys = std::clamp(action_to_physical[m.action], 0, 15);
        cmap[m.codepoint] = phys_gids[size_t(phys)];
    }
    const int quit_action = std::clamp(title_quit_action, 0, 15);
    const int quit_phys = std::clamp(action_to_physical[size_t(quit_action)], 0, 15);
    cmap[0xE0F8u] = phys_gids[size_t(quit_phys)];

    std::vector<uint8_t> f4, f12;
    if (!build_format4(cmap, f4)) return false;
    build_format12(cmap, f12);

    const uint32_t f4_off = 4 + 3 * 8;
    const uint32_t f4_padded = uint32_t((f4_off + f4.size() + 3u) & ~3u);
    std::vector<uint8_t> new_cmap;
    new_cmap.reserve(size_t(f4_padded) + f12.size());
    push16(new_cmap, 0);
    push16(new_cmap, 3);
    // Sorted encoding records: (0, 3), (3, 1), (3, 10)
    push16(new_cmap, 0); push16(new_cmap, 3); push32(new_cmap, f4_off);
    push16(new_cmap, 3); push16(new_cmap, 1); push32(new_cmap, f4_off);
    push16(new_cmap, 3); push16(new_cmap, 10); push32(new_cmap, f4_padded);
    new_cmap.insert(new_cmap.end(), f4.begin(), f4.end());
    while (new_cmap.size() < f4_padded) new_cmap.push_back(0);
    new_cmap.insert(new_cmap.end(), f12.begin(), f12.end());

    // Zero old cmap bytes, align file end to 4 bytes, and append new cmap.
    std::memset(ttf.data() + cmap_off, 0, cmap_len);
    while (ttf.size() % 4 != 0) ttf.push_back(0);
    const uint32_t new_cmap_off = uint32_t(ttf.size());
    const uint32_t new_cmap_len = uint32_t(new_cmap.size());
    ttf.insert(ttf.end(), new_cmap.begin(), new_cmap.end());
    while (ttf.size() % 4 != 0) ttf.push_back(0);

    wr32(ttf.data() + cmap_rec + 4, table_checksum(new_cmap.data(), new_cmap.size()));
    wr32(ttf.data() + cmap_rec + 8, new_cmap_off);
    wr32(ttf.data() + cmap_rec + 12, new_cmap_len);

    // Zero checkSumAdjustment, update head checksum, then write whole-file adjustment.
    wr32(ttf.data() + head_off + 8, 0);
    wr32(ttf.data() + head_rec + 4, table_checksum(ttf.data() + head_off, head_len));
    const uint32_t file_sum = table_checksum(ttf.data(), ttf.size());
    wr32(ttf.data() + head_off + 8, 0xB1B0AFBAu - file_sum);
    return true;
}

}  // namespace nx
