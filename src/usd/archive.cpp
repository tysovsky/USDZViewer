#include "archive.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace usd {

namespace {

uint16_t Rd16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
uint32_t Rd32(const uint8_t* p) { return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24); }
uint64_t Rd64(const uint8_t* p) { return uint64_t(Rd32(p)) | (uint64_t(Rd32(p + 4)) << 32); }

char Lower(char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; }

bool EqualsNoCase(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (Lower(a[i]) != Lower(b[i])) return false;
    return true;
}

// ---- inflate (after Mark Adler's puff.c) -------------------------------------------------------

struct InflateError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct BitReader {
    const uint8_t* src;
    size_t len;
    size_t pos = 0;
    uint32_t bitbuf = 0;
    int bitcnt = 0;

    int Bits(int need) {
        uint32_t val = bitbuf;
        while (bitcnt < need) {
            if (pos >= len) throw InflateError("unexpected end of deflate stream");
            val |= uint32_t(src[pos++]) << bitcnt;
            bitcnt += 8;
        }
        bitbuf = need < 32 ? (val >> need) : 0;
        bitcnt -= need;
        return int(val & ((1u << need) - 1));
    }
};

struct Huffman {
    short count[16];
    short symbol[320];
};

int Decode(BitReader& s, const Huffman& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= s.Bits(1);
        int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
    }
    throw InflateError("invalid Huffman code");
}

int Construct(Huffman& h, const short* length, int n) {
    for (int len = 0; len < 16; ++len) h.count[len] = 0;
    for (int sym = 0; sym < n; ++sym) h.count[length[sym]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len < 16; ++len) {
        left <<= 1;
        left -= h.count[len];
        if (left < 0) return left;
    }
    short offs[16];
    offs[1] = 0;
    for (int len = 1; len < 15; ++len) offs[len + 1] = short(offs[len] + h.count[len]);
    for (int sym = 0; sym < n; ++sym)
        if (length[sym] != 0) h.symbol[offs[length[sym]]++] = short(sym);
    return left;
}

const short kLenBase[29] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
                            35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
const short kLenExtra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
const short kDistBase[30] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129,
                             193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
const short kDistExtra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void Codes(BitReader& s, std::vector<uint8_t>& out, const Huffman& lencode, const Huffman& distcode) {
    int symbol;
    do {
        symbol = Decode(s, lencode);
        if (symbol < 256) {
            out.push_back(uint8_t(symbol));
        } else if (symbol > 256) {
            symbol -= 257;
            if (symbol >= 29) throw InflateError("invalid length symbol");
            size_t len = size_t(kLenBase[symbol] + s.Bits(kLenExtra[symbol]));
            int dsym = Decode(s, distcode);
            if (dsym < 0 || dsym >= 30) throw InflateError("invalid distance symbol");
            size_t dist = size_t(kDistBase[dsym] + s.Bits(kDistExtra[dsym]));
            if (dist > out.size()) throw InflateError("distance too far back");
            size_t from = out.size() - dist;
            for (size_t i = 0; i < len; ++i) out.push_back(out[from + i]);
        }
    } while (symbol != 256);
}

void Stored(BitReader& s, std::vector<uint8_t>& out) {
    s.bitbuf = 0;
    s.bitcnt = 0;
    if (s.pos + 4 > s.len) throw InflateError("truncated stored block");
    unsigned len = Rd16(s.src + s.pos);
    unsigned nlen = Rd16(s.src + s.pos + 2);
    s.pos += 4;
    if (len != (~nlen & 0xffff)) throw InflateError("stored block length mismatch");
    if (s.pos + len > s.len) throw InflateError("truncated stored block");
    out.insert(out.end(), s.src + s.pos, s.src + s.pos + len);
    s.pos += len;
}

void Fixed(BitReader& s, std::vector<uint8_t>& out) {
    static Huffman lencode, distcode;
    static bool built = false;
    if (!built) {
        short lengths[288];
        int sym = 0;
        for (; sym < 144; ++sym) lengths[sym] = 8;
        for (; sym < 256; ++sym) lengths[sym] = 9;
        for (; sym < 280; ++sym) lengths[sym] = 7;
        for (; sym < 288; ++sym) lengths[sym] = 8;
        Construct(lencode, lengths, 288);
        for (sym = 0; sym < 30; ++sym) lengths[sym] = 5;
        Construct(distcode, lengths, 30);
        built = true;
    }
    Codes(s, out, lencode, distcode);
}

void Dynamic(BitReader& s, std::vector<uint8_t>& out) {
    static const short order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    short lengths[320];
    int nlen = s.Bits(5) + 257;
    int ndist = s.Bits(5) + 1;
    int ncode = s.Bits(4) + 4;
    if (nlen > 286 || ndist > 30) throw InflateError("bad dynamic block counts");
    int index = 0;
    for (; index < ncode; ++index) lengths[order[index]] = short(s.Bits(3));
    for (; index < 19; ++index) lengths[order[index]] = 0;
    Huffman lencode, distcode;
    if (Construct(lencode, lengths, 19) != 0) throw InflateError("incomplete code length code");
    index = 0;
    while (index < nlen + ndist) {
        int symbol = Decode(s, lencode);
        if (symbol < 16) {
            lengths[index++] = short(symbol);
        } else {
            short len = 0;
            int repeat;
            if (symbol == 16) {
                if (index == 0) throw InflateError("repeat with no previous length");
                len = lengths[index - 1];
                repeat = 3 + s.Bits(2);
            } else if (symbol == 17) {
                repeat = 3 + s.Bits(3);
            } else {
                repeat = 11 + s.Bits(7);
            }
            if (index + repeat > nlen + ndist) throw InflateError("too many lengths");
            while (repeat--) lengths[index++] = len;
        }
    }
    if (lengths[256] == 0) throw InflateError("no end-of-block code");
    int err = Construct(lencode, lengths, nlen);
    if (err < 0 || (err > 0 && nlen - lencode.count[0] != 1)) throw InflateError("bad literal/length code");
    err = Construct(distcode, lengths + nlen, ndist);
    if (err < 0 || (err > 0 && ndist - distcode.count[0] != 1)) throw InflateError("bad distance code");
    Codes(s, out, lencode, distcode);
}

}  // namespace

bool Inflate(const uint8_t* src, size_t srcLen, std::vector<uint8_t>& out, size_t expectedSize) {
    out.clear();
    out.reserve(expectedSize);
    BitReader s{src, srcLen};
    try {
        int last;
        do {
            last = s.Bits(1);
            int type = s.Bits(2);
            if (type == 0) Stored(s, out);
            else if (type == 1) Fixed(s, out);
            else if (type == 2) Dynamic(s, out);
            else return false;
        } while (!last);
    } catch (const InflateError&) {
        return false;
    }
    return true;
}

std::string NormalizeRelativePath(std::string_view p) {
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else parts.push_back(cur);
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (char c : p) {
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    std::string r;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += '/';
        r += parts[i];
    }
    return r;
}

std::shared_ptr<Archive> Archive::Load(std::vector<uint8_t> bytes, std::string displayPath, std::string& err) {
    auto a = std::shared_ptr<Archive>(new Archive());
    a->bytes_ = std::move(bytes);
    a->displayPath_ = std::move(displayPath);
    const uint8_t* d = a->bytes_.data();
    const size_t n = a->bytes_.size();
    if (n < 22) {
        err = "file is too small to be a zip archive";
        return nullptr;
    }

    // Find the end-of-central-directory record.
    size_t eocd = SIZE_MAX;
    size_t minPos = n > 22 + 65535 ? n - 22 - 65535 : 0;
    for (size_t pos = n - 22 + 1; pos-- > minPos;) {
        if (Rd32(d + pos) == 0x06054b50) {
            eocd = pos;
            break;
        }
    }
    if (eocd == SIZE_MAX) {
        err = "not a zip archive (no end of central directory record)";
        return nullptr;
    }
    uint64_t count = Rd16(d + eocd + 10);
    uint64_t cdSize = Rd32(d + eocd + 12);
    uint64_t cdOffset = Rd32(d + eocd + 16);

    // ZIP64 end of central directory locator.
    if ((count == 0xffff || cdOffset == 0xffffffffu) && eocd >= 20 && Rd32(d + eocd - 20) == 0x07064b50) {
        uint64_t z64 = Rd64(d + eocd - 20 + 8);
        if (z64 + 56 <= n && Rd32(d + z64) == 0x06064b50) {
            count = Rd64(d + z64 + 32);
            cdSize = Rd64(d + z64 + 40);
            cdOffset = Rd64(d + z64 + 48);
        }
    }
    if (cdOffset + cdSize > n) {
        err = "zip central directory is out of range";
        return nullptr;
    }

    size_t pos = size_t(cdOffset);
    for (uint64_t i = 0; i < count; ++i) {
        if (pos + 46 > n || Rd32(d + pos) != 0x02014b50) {
            err = "corrupt zip central directory";
            return nullptr;
        }
        Entry e;
        e.method = Rd16(d + pos + 10);
        e.compressedSize = Rd32(d + pos + 20);
        e.size = Rd32(d + pos + 24);
        uint16_t nameLen = Rd16(d + pos + 28);
        uint16_t extraLen = Rd16(d + pos + 30);
        uint16_t commentLen = Rd16(d + pos + 32);
        uint64_t localOffset = Rd32(d + pos + 42);
        if (pos + 46 + nameLen + extraLen + commentLen > n) {
            err = "corrupt zip central directory entry";
            return nullptr;
        }
        e.name.assign(reinterpret_cast<const char*>(d + pos + 46), nameLen);
        // ZIP64 extended information.
        const uint8_t* extra = d + pos + 46 + nameLen;
        for (size_t x = 0; x + 4 <= extraLen;) {
            uint16_t id = Rd16(extra + x), sz = Rd16(extra + x + 2);
            if (id == 0x0001) {
                const uint8_t* f = extra + x + 4;
                size_t off = 0;
                if (e.size == 0xffffffffu && off + 8 <= sz) { e.size = Rd64(f + off); off += 8; }
                if (e.compressedSize == 0xffffffffu && off + 8 <= sz) { e.compressedSize = Rd64(f + off); off += 8; }
                if (localOffset == 0xffffffffu && off + 8 <= sz) { localOffset = Rd64(f + off); off += 8; }
            }
            x += 4 + size_t(sz);
        }
        pos += 46 + size_t(nameLen) + extraLen + commentLen;

        if (localOffset + 30 > n || Rd32(d + localOffset) != 0x04034b50) continue;
        uint16_t localNameLen = Rd16(d + localOffset + 26);
        uint16_t localExtraLen = Rd16(d + localOffset + 28);
        e.dataOffset = localOffset + 30 + localNameLen + localExtraLen;
        if (e.dataOffset + e.compressedSize > n) continue;
        if (!e.name.empty() && e.name.back() == '/') continue;  // directory
        a->entries_.push_back(std::move(e));
    }
    if (a->entries_.empty()) {
        err = "zip archive contains no files";
        return nullptr;
    }
    return a;
}

const Archive::Entry* Archive::Find(std::string_view path) const {
    std::string norm = NormalizeRelativePath(path);
    for (auto& e : entries_)
        if (e.name == norm) return &e;
    for (auto& e : entries_)
        if (EqualsNoCase(NormalizeRelativePath(e.name), norm)) return &e;
    return nullptr;
}

bool Archive::Extract(const Entry& e, std::vector<uint8_t>& out, std::string& err) const {
    const uint8_t* src = bytes_.data() + e.dataOffset;
    if (e.method == 0) {
        out.assign(src, src + e.compressedSize);
        return true;
    }
    if (e.method == 8) {
        if (!Inflate(src, size_t(e.compressedSize), out, size_t(e.size))) {
            err = "failed to inflate '" + e.name + "'";
            return false;
        }
        return true;
    }
    err = "unsupported zip compression method " + std::to_string(e.method) + " for '" + e.name + "'";
    return false;
}

}  // namespace usd
