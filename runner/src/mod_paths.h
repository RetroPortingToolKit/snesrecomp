#ifndef SNESRECOMP_MOD_PATHS_H
#define SNESRECOMP_MOD_PATHS_H

#include <filesystem>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace SNESRecomp {
namespace ModPaths {

// Use an absolute, extended path only at Windows filesystem boundaries. Keep
// logical paths in the catalog and state so callers retain portable paths.
inline std::filesystem::path for_io(const std::filesystem::path& path) {
#ifdef _WIN32
    if (path.empty()) return path;
    auto preferred = path;
    preferred.make_preferred();
    const auto& original = preferred.native();
    if (original.compare(0, 4, L"\\\\?\\") == 0 ||
        original.compare(0, 4, L"\\\\.\\") == 0)
        return preferred;
    // MinGW's absolute()/lexically_normal() can collapse a UNC root into a
    // current-drive path. Preserve server/share and normalize only its tail.
    if (original.compare(0, 2, L"\\\\") == 0) {
        const auto server_end = original.find(L'\\', 2);
        const auto share_end = server_end == std::wstring::npos
            ? std::wstring::npos : original.find(L'\\', server_end + 1);
        const auto tail = share_end == std::wstring::npos ? std::wstring()
            : std::filesystem::path(original.substr(share_end)).lexically_normal().native();
        return std::filesystem::path(L"\\\\?\\UNC\\" + original.substr(2, share_end - 2) + tail);
    }
    auto absolute = std::filesystem::absolute(preferred).lexically_normal();
    absolute.make_preferred();
    const auto& native = absolute.native();
    return std::filesystem::path(L"\\\\?\\" + native);
#else
    return path;
#endif
}

// Some MinGW directory iterators still use a MAX_PATH-limited CRT even with
// extended paths. Enumerate the catalog with the wide Windows API instead.
inline std::vector<std::filesystem::path> entries(
    const std::filesystem::path& root, std::error_code& ec) {
    std::vector<std::filesystem::path> result;
    ec.clear();
#ifdef _WIN32
    WIN32_FIND_DATAW data;
    const auto pattern = for_io(root) / L"*";
    const HANDLE handle = FindFirstFileW(pattern.c_str(), &data);
    if (handle == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND)
            ec = std::error_code(error, std::system_category());
        return result;
    }
    struct CloseSearch {
        HANDLE handle;
        ~CloseSearch() { FindClose(handle); }
    } close{handle};
    do {
        const std::filesystem::path name(data.cFileName);
        if (name != L"." && name != L"..") result.push_back(root / name);
    } while (FindNextFileW(handle, &data));
    const DWORD error = GetLastError();
    if (error != ERROR_NO_MORE_FILES)
        ec = std::error_code(error, std::system_category());
#else
    for (std::filesystem::directory_iterator it(root, ec), end;
         !ec && it != end; it.increment(ec))
        result.push_back(it->path());
#endif
    return result;
}

}  // namespace ModPaths
}  // namespace SNESRecomp

#endif
