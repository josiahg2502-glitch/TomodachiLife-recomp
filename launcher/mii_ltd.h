// Importing a Mii from a ShareMii .ltd file into a resident's slot of the save (launcher: SAVE DATA > IMPORT A MII).
// The .ltd format and the import are those of Star-F0rce's ShareMii (https://github.com/Star-F0rce/ShareMii, GPL-3.0,
// ShareMii.py), ported here; TomodachiShare and the ltdsave.app editor use the same files. Version 3 layout:
//   [0] version 1..3, [1] embedded face paint canvas, [2] embedded face paint texture, [3] 0
//   [4..160)   the Mii's appearance record (Mii.sav 881CA27A: u32 size 152 + 152 bytes)
//   [160..232) 18 u32: personality, voice, gender / pronoun / clothing style, birthday (the hashes in kPersonal)
//   [232..296) name (UTF-16, 32 units), [296..424) its pronunciation (64 units), [424..427) whom it can fall for (bits)
//   then A3A3A3A3 + UgcFacePaint canvas (.zs) + A4A4A4A4 + its texture (.zs), when it has face paint.
// A save's fields are found through its entry table (hash -> heap offset of the array, whose u32 count comes first).
#pragma once
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace ltd {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

inline uint32_t u32(const Bytes& b, size_t o) { uint32_t v = 0; if (o + 4 <= b.size()) memcpy(&v, b.data() + o, 4); return v; }
inline bool read_file(const fs::path& p, Bytes& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return true;
}
inline bool write_file(const fs::path& p, const Bytes& b) {
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    return f && f.write((const char*)b.data(), (std::streamsize)b.size()) && f.flush();
}

// The payload (after its u32 count) of the array with this hash, 0 when the save has no such entry.
inline size_t field(const Bytes& sav, uint32_t hash) {
    if (sav.size() < 0x20 || u32(sav, 0) != 0x01020304) return 0;
    const size_t heap = u32(sav, 8);
    for (size_t o = 0x20; o + 8 <= heap && o + 8 <= sav.size(); o += 8)
        if (u32(sav, o) == hash) { const size_t at = u32(sav, o + 4); return at + 4 <= sav.size() ? at + 4 : 0; }
    return 0;
}

constexpr int kSlots = 70;
constexpr uint32_t kRaw = 0x881CA27A, kNames = 0x2499BFDA, kPronounce = 0x3A5EDA05, kFacePaintIndex = 0x5E32ADF4,
                   kLove = 0xDFC82223, kLevel = 0x9999B7D9;
constexpr uint32_t kPersonal[18] = {0x43CD364F, 0xCD8DBAF8, 0x25B48224, 0x607BA160, 0x68E1134E,                   // CharacterParam
                                    0x4913AE1A, 0x141EE086, 0x07B9D175, 0x81CF470A, 0x4D78E262, 0xFBC3FFB0,       // Voice
                                    0x236E2D73, 0xF3C3DE59, 0x660C5247,                                           // gender, pronoun, cloth style
                                    0x5D7D3F45, 0xAB8AE08B, 0x2545E583, 0x6CF484F4};                              // birthday
// Player.sav UGC.Facepaint.{Price, TextureSourceType, State, Unknown, Hash}: 71 u32 entries (70 Miis + the editor's)
constexpr uint32_t kPaint[5] = {0x4C9819E4, 0xDECC8954, 0x23135BC5, 0xFFC750B6, 0xA56E42EC};

inline std::string utf8(const uint8_t* p, int units) {
    std::string s;
    for (int i = 0; i < units; i++) {
        uint32_t c = p[2 * i] | p[2 * i + 1] << 8;
        if (!c) break;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < units) { const uint32_t lo = p[2 * i + 2] | p[2 * i + 3] << 8; c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00); i++; }
        if (c < 0x80) s += char(c);
        else if (c < 0x800) { s += char(0xC0 | c >> 6); s += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { s += char(0xE0 | c >> 12); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
        else { s += char(0xF0 | c >> 18); s += char(0x80 | (c >> 12 & 63)); s += char(0x80 | (c >> 6 & 63)); s += char(0x80 | (c & 63)); }
    }
    return s;
}

inline bool slot_used(const Bytes& mii, size_t raw, int slot) {      // ShareMii: an empty record is 152, 0, 0, ... (byte sum 152)
    unsigned sum = 0;
    for (int i = 0; i < 156; i++) sum += mii[raw + 156 * slot + i];
    return sum != 152;
}

// The residents: slot and name, in slot order.
inline std::vector<std::pair<int, std::string>> residents(const fs::path& save) {
    std::vector<std::pair<int, std::string>> out;
    Bytes mii;
    if (!read_file(save / "Mii.sav", mii)) return out;
    const size_t raw = field(mii, kRaw), names = field(mii, kNames);
    if (!raw || !names || raw + 156 * kSlots > mii.size() || names + 64 * kSlots > mii.size()) return out;
    for (int s = 0; s < kSlots; s++)
        if (slot_used(mii, raw, s)) { std::string n = utf8(&mii[names + 64 * s], 32); out.push_back({s, n.empty() ? "Mii " + std::to_string(s + 1) : n}); }
    return out;
}

inline size_t find(const Bytes& b, const uint8_t* pat, size_t n, bool last = false) {
    size_t hit = std::string::npos;
    for (size_t i = 0; i + n <= b.size(); i++) if (!memcmp(&b[i], pat, n)) { hit = i; if (!last) break; }
    return hit;
}

// A .ltd brought to version 3 (ShareMii's conversion of versions 1 and 2), or "" + error.
inline std::string load(const fs::path& p, Bytes& m) {
    if (!read_file(p, m) || m.empty()) return "That file is empty or cannot be read.";
    if (m[0] < 1 || m[0] > 3) return "Not a Mii file (.ltd versions 1 to 3).";
    if (m[0] < 3) {
        m.erase(m.begin() + 4);
        if (m[0] == 2 && m.size() > 427) {
            static const uint8_t a3[3] = {0xA3, 0xA3, 0xA3};
            m.insert(m.begin() + 427, 0);
            const size_t c = find(m, a3, 3), u = find(m, a3, 3, true);
            if (c != std::string::npos && u != std::string::npos) {
                m.insert(m.begin() + c + 3, 0xA3);
                for (int k = 1; k < 4 && u + k < m.size(); k++) m[u + k] = 0xA4;
                m.insert(m.begin() + u + 3, 0xA4);
            }
        }
    }
    if (m.size() < 160 || (m[0] >= 2 && m.size() < 427)) return "That Mii file is cut short.";
    return "";
}
inline std::string name_of(const Bytes& m) { return m[0] >= 2 && m.size() >= 296 ? utf8(&m[232], 32) : ""; }

// Imports the .ltd m (from load) over the Mii in slot of the save folder; "" or what went wrong. Nothing is written
// unless every field is found. ltd_path: for face paint kept beside the file (<file>.canvas.zs / .ugctex.zs).
inline std::string import(const fs::path& save, int slot, Bytes m, const fs::path& ltd_path) {
    Bytes mii, player;
    if (!read_file(save / "Mii.sav", mii) || !read_file(save / "Player.sav", player)) return "No save to import into (Mii.sav, Player.sav).";
    const size_t raw = field(mii, kRaw), names = field(mii, kNames), pron = field(mii, kPronounce), fpi = field(mii, kFacePaintIndex),
                 love = field(mii, kLove), level = field(mii, kLevel);
    size_t pers[18], paint[5];
    bool ok = raw && names && pron && fpi && love && level;
    for (int i = 0; i < 18; i++) ok = ok && (pers[i] = field(mii, kPersonal[i])) != 0;
    for (int i = 0; i < 5; i++) ok = ok && (paint[i] = field(player, kPaint[i])) != 0;
    if (!ok || raw + 156 * kSlots > mii.size()) return "This save's layout is not the one Mii files are made for.";
    if (slot < 0 || slot >= kSlots || !slot_used(mii, raw, slot)) return "There is no Mii in that slot.";

    static const uint8_t A3[4] = {0xA3, 0xA3, 0xA3, 0xA3}, A4[4] = {0xA4, 0xA4, 0xA4, 0xA4};
    int paint_kind = 0;      // 1: in the file, 2: beside it
    Bytes canvas, tex;
    if (m[1] == 1 && m[2] == 1) {
        const size_t c = find(m, A3, 4), u = find(m, A4, 4, true);
        if (c == std::string::npos || u == std::string::npos || u < c + 4) return "The Mii file's face paint is damaged.";
        canvas.assign(m.begin() + c + 4, m.begin() + u);
        tex.assign(m.begin() + u + 4, m.end());
        paint_kind = 1;
    } else if (read_file(fs::path(ltd_path).concat(".canvas.zs"), canvas) && read_file(fs::path(ltd_path).concat(".ugctex.zs"), tex))
        paint_kind = 2;
    if (paint_kind) m[47] = 1;
    memcpy(&mii[raw + 156 * slot], &m[4], 156);

    int id = mii[fpi + 4 * slot];
    const auto set_paint = [&](int k, uint32_t v) { memcpy(&player[paint[k] + 4 * id], &v, 4); };
    if (paint_kind) {
        if (id == 255) {      // the first face paint number no Mii uses
            bool used[256] = {};
            for (int s = 0; s < kSlots; s++) used[mii[fpi + 4 * s]] = true;
            id = 0;
            while (id < kSlots && used[id]) id++;
            if (id == kSlots) return "Every face paint slot is in use.";
        }
        const uint32_t v = (uint32_t)id;
        memcpy(&mii[fpi + 4 * slot], &v, 4);
        set_paint(0, 0x000001F4); set_paint(1, 0x56934941); set_paint(2, 0x1D7FADF4); set_paint(3, 0x00008000); set_paint(4, (uint32_t)id | 0x00080000);
        char file[64];
        snprintf(file, sizeof file, "UgcFacePaint%03d", id);
        std::error_code e;
        fs::create_directories(save / "Ugc", e);
        if (!write_file(save / "Ugc" / (std::string(file) + ".canvas.zs"), canvas) || !write_file(save / "Ugc" / (std::string(file) + ".ugctex.zs"), tex))
            return "The face paint could not be written.";
    } else if (id != 255) {      // the new Mii has none: its old face paint is let go
        memset(&mii[fpi + 4 * slot], 0xFF, 4);
        set_paint(0, 0); set_paint(1, 0xB6EEDE09); set_paint(2, 0xAFFF8AA5); set_paint(3, 0); set_paint(4, 0);
    }

    if (m[0] >= 2) {
        for (int i = 0; i < 18; i++) memcpy(&mii[pers[i] + 4 * slot], &m[160 + 4 * i], 4);
        memcpy(&mii[names + 64 * slot], &m[232], 64);
        memcpy(&mii[pron + 128 * slot], &m[296], 128);
        if (mii[level + 4 * slot] < 2)      // ShareMii: a Mii past level 1 keeps whom it can fall for
            for (int k = 0; k < 3; k++) {
                const int bit = slot * 3 + k;
                uint8_t& byte = mii[love + bit / 8];
                byte = m[424 + k] ? byte | (1 << (bit % 8)) : byte & ~(1 << (bit % 8));
            }
    }
    if (!write_file(save / "Mii.sav", mii) || !write_file(save / "Player.sav", player)) return "The save could not be written.";
    return "";
}

}  // namespace ltd
