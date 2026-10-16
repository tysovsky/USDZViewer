// Small file system helpers (UTF-8 paths in, Win32 wide-char APIs underneath).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace usd {

std::wstring Utf8ToWide(const std::string& s);
std::string WideToUtf8(const std::wstring& s);

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, std::string& err);
bool FileExists(const std::wstring& path);

// Joins a directory and a relative path and normalizes "." / ".." segments (UTF-8, '/' or '\\').
std::string JoinFsPath(const std::string& dir, const std::string& rel);
std::string FsDirName(const std::string& p);
bool IsAbsoluteFsPath(const std::string& p);
std::string FileExtensionLower(const std::string& p);

}  // namespace usd
