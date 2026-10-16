#include "fileio.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace usd {

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}

std::string WideToUtf8(const std::wstring& s) {
    if (s.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0, nullptr, nullptr);
    std::string r(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), int(s.size()), r.data(), n, nullptr, nullptr);
    return r;
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, std::string& err) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        err = "cannot open '" + WideToUtf8(path) + "'";
        return false;
    }
    LARGE_INTEGER size;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (1ll << 34)) {
        CloseHandle(h);
        err = "cannot read '" + WideToUtf8(path) + "'";
        return false;
    }
    out.resize(size_t(size.QuadPart));
    size_t done = 0;
    while (done < out.size()) {
        DWORD chunk = DWORD(std::min<size_t>(out.size() - done, 1u << 30));
        DWORD got = 0;
        if (!ReadFile(h, out.data() + done, chunk, &got, nullptr) || got == 0) {
            CloseHandle(h);
            err = "error reading '" + WideToUtf8(path) + "'";
            return false;
        }
        done += got;
    }
    CloseHandle(h);
    return true;
}

bool FileExists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool IsAbsoluteFsPath(const std::string& p) {
    if (p.size() >= 2 && p[1] == ':') return true;
    return !p.empty() && (p[0] == '/' || p[0] == '\\');
}

std::string FsDirName(const std::string& p) {
    size_t cut = p.find_last_of("/\\");
    if (cut == std::string::npos) return std::string();
    return p.substr(0, cut);
}

std::string JoinFsPath(const std::string& dir, const std::string& rel) {
    std::string full = IsAbsoluteFsPath(rel) || dir.empty() ? rel : dir + "\\" + rel;
    // Normalize separators and dot segments, keeping a drive / UNC prefix intact.
    std::string prefix;
    size_t start = 0;
    if (full.size() >= 2 && full[1] == ':') {
        prefix = full.substr(0, 2);
        start = 2;
    } else if (full.size() >= 2 && (full[0] == '\\' || full[0] == '/') && (full[1] == '\\' || full[1] == '/')) {
        prefix = "\\\\";
        start = 2;
    }
    bool rooted = start < full.size() && (full[start] == '\\' || full[start] == '/');
    std::vector<std::string> parts;
    std::string cur;
    auto flush = [&]() {
        if (cur.empty() || cur == ".") {
        } else if (cur == "..") {
            if (!parts.empty() && parts.back() != "..") parts.pop_back();
            else if (!rooted) parts.push_back(cur);
        } else {
            parts.push_back(cur);
        }
        cur.clear();
    };
    for (size_t i = start; i < full.size(); ++i) {
        char c = full[i];
        if (c == '/' || c == '\\') flush();
        else cur += c;
    }
    flush();
    std::string r = prefix;
    if (rooted) r += '\\';
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) r += '\\';
        r += parts[i];
    }
    return r;
}

std::string FileExtensionLower(const std::string& p) {
    size_t slash = p.find_last_of("/\\");
    size_t dot = p.rfind('.');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return std::string();
    std::string ext = p.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](char c) { return char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c); });
    return ext;
}

}  // namespace usd
