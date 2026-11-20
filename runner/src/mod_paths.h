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

// MinGW's std::filesystem status calls (exists, is_directory, is_regular_file,
// is_symlink, create_directories) go through the CRT's stat, which rejects
// extended paths with ENOENT or EINVAL. Query and create through the wide
// Windows API instead; elsewhere these are the std::filesystem calls.
enum class Kind { missing, file, directory, other };

inline Kind kind(const std::filesystem::path& path, std::error_code& ec,
                 bool* link = nullptr) {
    ec.clear();
    if (link) *link = false;
#ifdef _WIN32
    const DWORD attributes = GetFileAttributesW(for_io(path).c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            ec = std::error_code(error, std::system_category());
        return Kind::missing;
    }
    if (link) *link = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (attributes & FILE_ATTRIBUTE_DIRECTORY) return Kind::directory;
    if (attributes & FILE_ATTRIBUTE_DEVICE) return Kind::other;
    return Kind::file;
#else
    const auto status = std::filesystem::status(path, ec);
    if (link) {
        std::error_code link_ec;
        *link = std::filesystem::is_symlink(path, link_ec);
    }
    if (status.type() == std::filesystem::file_type::not_found) {
        ec.clear();
        return Kind::missing;
    }
    if (ec) return Kind::missing;
    if (std::filesystem::is_directory(status)) return Kind::directory;
    if (std::filesystem::is_regular_file(status)) return Kind::file;
    return Kind::other;
#endif
}

inline bool exists(const std::filesystem::path& path) {
    std::error_code ec;
    return kind(path, ec) != Kind::missing;
}
inline bool is_directory(const std::filesystem::path& path, std::error_code& ec) {
    return kind(path, ec) == Kind::directory;
}
inline bool is_directory(const std::filesystem::path& path) {
    std::error_code ec;
    return ModPaths::is_directory(path, ec);
}
inline bool is_regular_file(const std::filesystem::path& path, std::error_code& ec) {
    return kind(path, ec) == Kind::file;
}
inline bool is_regular_file(const std::filesystem::path& path) {
    std::error_code ec;
    return ModPaths::is_regular_file(path, ec);
}
inline bool is_symlink(const std::filesystem::path& path, std::error_code& ec) {
    bool link = false;
    kind(path, ec, &link);
    return link;
}

// Removes a file or an empty directory; false without an error when nothing
// was there (std::filesystem::remove on MinGW reports that for any extended path).
inline bool remove(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    const Kind found = kind(path, ec);
    if (ec || found == Kind::missing) return false;
    const auto io = for_io(path);
    if (!(found == Kind::directory ? RemoveDirectoryW(io.c_str()) : DeleteFileW(io.c_str()))) {
        ec = std::error_code(GetLastError(), std::system_category());
        return false;
    }
    return true;
#else
    return std::filesystem::remove(path, ec);
#endif
}

// Creates path and its missing parents; true when path is a directory after.
inline bool create_directories(const std::filesystem::path& path, std::error_code& ec) {
    ec.clear();
#ifdef _WIN32
    // Walk the plain absolute form, create each missing level by its extended path.
    std::wstring text = for_io(path).native();
    if (text.compare(0, 8, L"\\\\?\\UNC\\") == 0) text = L"\\\\" + text.substr(8);
    else if (text.compare(0, 4, L"\\\\?\\") == 0) text = text.substr(4);
    const std::filesystem::path plain(text);
    std::filesystem::path level = plain.root_path();
    for (const auto& part : plain.relative_path()) {
        level /= part;
        const Kind found = kind(level, ec);
        if (ec) return false;
        if (found == Kind::directory) continue;
        if (found != Kind::missing) {
            ec = std::make_error_code(std::errc::not_a_directory);
            return false;
        }
        if (!CreateDirectoryW(for_io(level).c_str(), nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_ALREADY_EXISTS || kind(level, ec) != Kind::directory) {
                if (!ec) ec = std::error_code(error, std::system_category());
                return false;
            }
        }
    }
    return true;
#else
    std::filesystem::create_directories(path, ec);
    return !ec && std::filesystem::is_directory(path, ec);
#endif
}

}  // namespace ModPaths
}  // namespace SNESRecomp

#endif
