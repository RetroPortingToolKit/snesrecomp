// Folder/ZIP transport distilled from F-Zero's pack loader. Semantic formats,
// title rendering, record keys, physics and audio routing belong to the game.
#include "data_pack.h"
#include "data_pack_io.hpp"
#include "sha256.h"
#include <archive.h>
#include <archive_entry.h>
#include <rapidjson/document.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
constexpr size_t PayloadLimit = 128u * 1024 * 1024;
constexpr size_t CatalogLimit = 512u * 1024 * 1024;
using Blob = std::vector<uint8_t>;
using Archive = std::unique_ptr<archive, decltype(&archive_read_free)>;
void check(bool ok, const std::string &why) {
  if (!ok) throw std::runtime_error(why);
}
std::string lower(std::string s) {
  for (char &c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
  return s;
}
void relativePath(const std::string &name, bool directory = false) {
  check(!name.empty() && name.size() < 512 && name.front() != '/' &&
        name.find_first_of("\\:\0", 0, 3) == std::string::npos,
        "Expected a relative portable pack path");
  size_t start = 0;
  while (start < name.size()) {
    auto end = name.find('/', start);
    auto part = name.substr(start, end == std::string::npos ? end : end - start);
    check(!part.empty() && part != "." && part != ".." && part.back() != '.' &&
          part.back() != ' ' && part.find_first_of("<>\"|?*") == std::string::npos,
          "Invalid pack path component");
    for (unsigned char c : part) check(c >= 32, "Control character in pack path");
    auto stem = lower(part.substr(0, part.find('.')));
    check(stem != "con" && stem != "nul" && stem != "prn" && stem != "aux" &&
          !(stem.size() == 4 && (stem.substr(0,3) == "com" || stem.substr(0,3) == "lpt") &&
            stem[3] >= '1' && stem[3] <= '9'), "Reserved pack filename");
    if (end == std::string::npos) break;
    start = end + 1;
    check(start != name.size() || directory, "File path ends in slash");
  }
}
fs::path inside(const fs::path &root, const std::string &name) {
  relativePath(name);
  auto base = fs::canonical(root);
  auto file = fs::weakly_canonical(base / fs::u8path(name));
  auto rel = file.lexically_relative(base);
  check(!rel.empty() && !rel.is_absolute() && *rel.begin() != "..",
        "Pack symlink escapes its directory");
  return file;
}
Blob readFile(const fs::path &file, size_t limit) {
  check(fs::is_regular_file(file), "Missing pack file");
  auto size = fs::file_size(file);
  check(size <= limit, "Pack file exceeds size limit");
  std::ifstream input(file, std::ios::binary);
  Blob b(static_cast<size_t>(size));
  check(bool(input) && bool(input.read(reinterpret_cast<char *>(b.data()), b.size())),
        "Cannot read complete pack file");
  check(input.peek() == std::char_traits<char>::eof(), "Pack file changed during read");
  return b;
}
Archive openZip(const fs::path &file) {
  Archive a(archive_read_new(), archive_read_free);
  check(bool(a), "Cannot allocate ZIP reader");
  archive_read_support_format_zip(a.get());
  archive_read_support_filter_none(a.get());
#ifdef _WIN32
  auto ok = archive_read_open_filename_w(a.get(), file.c_str(), 65536);
#else
  auto ok = archive_read_open_filename(a.get(), file.c_str(), 65536);
#endif
  check(ok == ARCHIVE_OK, "Cannot open ZIP");
  return a;
}
std::string entryName(archive_entry *entry) {
  const char *name = archive_entry_pathname_utf8(entry);
  check(name != nullptr, "ZIP filename must be UTF-8");
  relativePath(name, archive_entry_filetype(entry) == AE_IFDIR);
  check(!archive_entry_symlink(entry) && !archive_entry_hardlink(entry) &&
        (archive_entry_filetype(entry) == AE_IFREG || archive_entry_filetype(entry) == AE_IFDIR),
        "ZIP links and special files are unsupported");
  check(archive_entry_is_encrypted(entry) == 0, "Encrypted ZIP entries are unsupported");
  return name;
}
// Validate the entire directory, not only the entry being requested.
// Optional disk materialization uses the same validation and content identity.
std::string zipRoot(const fs::path &source) {
  auto a = openZip(source);
  archive_entry *entry;
  std::set<std::string> names;
  std::vector<std::string> manifests;
  uint64_t total = 0;
  unsigned count = 0;
  int status;
  while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    check(++count <= 20000, "Too many ZIP entries");
    auto name = entryName(entry);
    if (!name.empty() && name.back() == '/') name.pop_back();
    check(names.insert(lower(name)).second, "Duplicate ZIP path");
    auto size = archive_entry_size(entry);
    check(size >= 0 && size <= INT64_C(2147483648), "ZIP entry exceeds size limit");
    total += uint64_t(size);
    check(total <= UINT64_C(8589934592), "ZIP exceeds total size limit");
    if (archive_entry_filetype(entry) == AE_IFREG &&
        fs::u8path(name).filename() == "pack.json") manifests.push_back(name);
  }
  check(status == ARCHIVE_EOF, "Damaged ZIP directory");
  check(manifests.size() == 1, "ZIP needs exactly one pack.json");
  auto root = manifests[0].substr(0, manifests[0].size() - 9);
  check(std::count(root.begin(), root.end(), '/') <= 1,
        "pack.json must be at ZIP root or in one enclosing folder");
  return root;
}
Blob readZip(const fs::path &source, const std::string &name, size_t limit) {
  auto a = openZip(source);
  archive_entry *entry;
  int status;
  while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
    if (entryName(entry) != name) continue;
    auto size = archive_entry_size(entry);
    check(archive_entry_filetype(entry) == AE_IFREG && size >= 0 && uint64_t(size) <= limit,
          "Missing or oversized ZIP asset");
    Blob b(static_cast<size_t>(size));
    size_t at = 0;
    while (at < b.size()) {
      auto n = archive_read_data(a.get(), b.data() + at, b.size() - at);
      check(n > 0, "Damaged ZIP asset");
      at += static_cast<size_t>(n);
    }
    char tail;
    check(archive_read_data(a.get(), &tail, 1) == 0, "Damaged ZIP asset or checksum");
    return b;
  }
  check(status == ARCHIVE_EOF, "Damaged ZIP directory");
  throw std::runtime_error("Missing ZIP asset: " + name);
}
void validateJson(const rapidjson::Value &v, unsigned depth = 0) {
  check(depth <= 32, "JSON nesting exceeds limit");
  if (v.IsObject()) {
    std::set<std::string> names;
    for (auto i = v.MemberBegin(); i != v.MemberEnd(); ++i) {
      std::string name(i->name.GetString(), i->name.GetStringLength());
      check(name.find('\0') == std::string::npos && names.insert(name).second,
            "Duplicate or invalid JSON key");
      validateJson(i->value, depth + 1);
    }
  } else if (v.IsArray()) for (const auto &item : v.GetArray()) validateJson(item, depth + 1);
}
std::string str(const rapidjson::Value &v, const char *key) {
  check(v.IsObject() && v.HasMember(key) && v[key].IsString(), std::string("Missing string: ") + key);
  std::string s(v[key].GetString(), v[key].GetStringLength());
  check(s.find('\0') == std::string::npos, "NUL in metadata");
  return s;
}
std::string hex(const uint8_t *digest) {
  std::string s;
  for (unsigned i = 0; i < 32; ++i) {
    s += "0123456789abcdef"[digest[i] >> 4];
    s += "0123456789abcdef"[digest[i] & 15];
  }
  return s;
}
struct Entry {
  std::string id, title, format, source, prefix, disk_root, payload_file;
  fs::path path;
  bool zip = false;
  Blob payload;
  SnesDataPack view{};
  Blob read(const std::string &name, size_t limit) const {
    relativePath(name);
    return zip ? readZip(path, prefix + name, limit) : readFile(inside(path, name), limit);
  }
};
Entry load(const fs::path &path, const char *game, const char *format,
           const uint8_t *base, const std::set<std::string> &capabilities) {
  Entry p;
  p.path = fs::absolute(path);
  p.source = p.path.u8string();
  p.zip = !fs::is_directory(path);
  if (p.zip) p.prefix = zipRoot(path);
  auto bytes = p.read("pack.json", 65536);
  check(!bytes.empty(), "Empty pack.json");
  rapidjson::Document m;
  m.Parse<rapidjson::kParseValidateEncodingFlag | rapidjson::kParseIterativeFlag>(
      reinterpret_cast<const char *>(bytes.data()), bytes.size());
  check(!m.HasParseError() && m.IsObject(), "Invalid pack.json");
  validateJson(m);
  check(str(m, "format") == "snesrecomp.data-pack" && m.HasMember("version") &&
        m["version"].IsInt() && m["version"].GetInt() == 1, "Unsupported pack format/version");
  check(str(m, "game") == game, "Pack targets a different game");
  check(str(m, "base_rom_sha256") == hex(base), "Pack targets a different base ROM");
  p.id = str(m, "id"); p.title = str(m, "title");
  check(!p.id.empty() && p.id.size() < 64 && p.id.front() != '.' &&
        p.id.front() != '-' && p.id.front() != '_' &&
        p.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789._-") == std::string::npos,
        "Invalid stable pack ID");
  relativePath(p.id);
  check(!p.title.empty() && p.title.size() < 96, "Invalid pack title");
  for (unsigned char c : p.title) check(c >= 32 && c != 127, "Control character in title");
  if (m.HasMember("requires")) {
    check(m["requires"].IsArray(), "requires must be an array");
    for (const auto &v : m["requires"].GetArray()) {
      check(v.IsString(), "Invalid capability");
      std::string c(v.GetString(), v.GetStringLength());
      check(capabilities.count(c) != 0, "Unsupported required capability: " + c);
    }
  }
  check(m.HasMember("payload"), "Missing payload");
  const auto &b = m["payload"];
  p.format = str(b, "format");
  check(p.format == format, "Unsupported game payload format");
  p.payload_file = str(b, "file");
  p.payload = p.read(p.payload_file, PayloadLimit);
  uint8_t digest[32];
  sha256_compute(p.payload.data(), p.payload.size(), digest);
  check(hex(digest) == str(b, "sha256"), "Payload SHA-256 mismatch");
  return p;
}
// An archive-content fingerprint, not a standard SHA-256 digest. Its purpose is
// a disposable cache namespace. Payload authenticity still uses standard SHA-256.
std::string cacheKey(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  check(bool(input), "Cannot hash pack archive");
  std::array<uint8_t, 65536 + 32> block{};
  uint8_t digest[32]{};
  while (input) {
    input.read(reinterpret_cast<char *>(block.data() + 32), 65536);
    auto n = input.gcount();
    if (n > 0) {
      memcpy(block.data(), digest, 32);
      sha256_compute(block.data(), static_cast<size_t>(n) + 32, digest);
    }
  }
  check(input.eof(), "Cannot hash complete archive");
  return hex(digest);
}
fs::path materialize(const Entry &p, const fs::path &cache_directory) {
  if (!p.zip) return fs::canonical(p.path);
  // Revalidate archive directory even on cache reuse. A live archive must never
  // acquire new links/ambiguous names after admission.
  check(zipRoot(p.path) == p.prefix, "Archive changed after discovery");
  fs::create_directories(cache_directory);
  auto base = fs::canonical(cache_directory);
  auto cache = inside(base, cacheKey(p.path));
  auto ready = cache; ready += ".ready";
  if (!fs::is_regular_file(ready)) {
    fs::create_directories(cache);
    auto a = openZip(p.path);
    archive_entry *entry;
    uint64_t total = 0;
    unsigned count = 0;
    std::set<std::string> names;
    int status;
    while ((status = archive_read_next_header(a.get(), &entry)) == ARCHIVE_OK) {
      check(++count <= 20000, "Too many ZIP entries");
      auto name = entryName(entry);
      if (name.back() == '/') name.pop_back();
      check(names.insert(lower(name)).second, "Duplicate ZIP path");
      auto target = inside(cache, name);
      if (archive_entry_filetype(entry) == AE_IFDIR) {
        fs::create_directories(target); continue;
      }
      auto size = archive_entry_size(entry);
      check(size >= 0 && size <= INT64_C(2147483648), "ZIP entry exceeds size limit");
      total += uint64_t(size);
      check(total <= UINT64_C(8589934592), "ZIP exceeds total size limit");
      fs::create_directories(target.parent_path());
      std::ofstream out(target, std::ios::binary | std::ios::trunc);
      check(bool(out), "Cannot create cached asset");
      char buffer[65536];
      int64_t written = 0;
      la_ssize_t n;
      while ((n = archive_read_data(a.get(), buffer, sizeof(buffer))) > 0) {
        written += n;
        check(written <= size, "ZIP entry exceeds declared size");
        out.write(buffer, n);
        check(bool(out), "Cannot write cached asset");
      }
      check(n == 0 && written == size, "Damaged ZIP asset or checksum");
      out.close(); check(bool(out), "Cannot finish cached asset");
    }
    check(status == ARCHIVE_EOF, "Damaged ZIP directory");
    std::ofstream marker(ready, std::ios::binary | std::ios::trunc);
    marker << "1\n"; marker.close();
    check(bool(marker), "Cannot complete ZIP cache");
  }
  auto root = p.prefix.empty() ? cache : inside(cache, p.prefix.substr(0, p.prefix.size()-1));
  check(readFile(inside(root, p.payload_file), PayloadLimit) == p.payload,
        "Cached payload differs from admitted pack");
  return root;
}
}
struct SnesDataPacks { std::vector<Entry> entries; };
namespace snesrecomp::data_pack {
fs::path inside(const fs::path &root, const std::string &relative) { return ::inside(root, relative); }
std::string read(const fs::path &path, size_t limit) {
  auto b = readFile(path, limit);
  return std::string(b.begin(), b.end());
}
void validate_json(const rapidjson::Value &v) { validateJson(v); }
std::string string(const rapidjson::Value &v, const char *key) { return str(v, key); }
}
extern "C" SnesDataPacks *snes_data_packs_scan(const char *directory, const char *game,
    const char *format, const uint8_t base[32], const char *const *caps, size_t ncaps,
    SnesDataPackError error, void *user) {
  try {
    check(directory && game && format && base && (!ncaps || caps), "Invalid catalog arguments");
    auto result = std::make_unique<SnesDataPacks>();
    if (!fs::exists(fs::u8path(directory))) return result.release();
    std::vector<fs::path> sources;
    for (const auto &item : fs::directory_iterator(fs::u8path(directory))) {
      if (item.path().filename().u8string().front() == '.') continue;
      if (item.is_directory() && fs::exists(item.path() / "pack.json")) sources.push_back(item.path());
      else if (item.is_regular_file() && lower(item.path().extension().u8string()) == ".zip") sources.push_back(item.path());
    }
    check(sources.size() <= 128, "Too many pack candidates");
    std::sort(sources.begin(), sources.end());
    std::set<std::string> capabilities;
    for (size_t i = 0; i < ncaps; ++i) { check(caps[i], "Null capability"); capabilities.insert(caps[i]); }
    size_t total = 0;
    for (const auto &source : sources) {
      try {
        auto p = load(source, game, format, base, capabilities);
        check(total + p.payload.size() <= CatalogLimit, "Catalog payload limit exceeded");
        total += p.payload.size();
        result->entries.push_back(std::move(p));
      } catch (const std::exception &e) { if (error) error(user, source.u8string().c_str(), e.what()); }
    }
    std::map<std::string, unsigned> ids;
    for (const auto &p : result->entries) ++ids[p.id];
    auto &entries = result->entries;
    entries.erase(std::remove_if(entries.begin(), entries.end(), [&](const Entry &p) {
      if (ids[p.id] == 1) return false;
      if (error) error(user, p.source.c_str(), "Duplicate pack ID: remove one copy");
      return true;
    }), entries.end());
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.id < b.id; });
    for (auto &p : entries) p.view = {p.id.c_str(), p.title.c_str(), p.source.c_str(), p.format.c_str(), p.payload.data(), p.payload.size()};
    return result.release();
  } catch (const std::exception &e) { if (error) error(user, directory ? directory : "", e.what()); return nullptr; }
}
extern "C" size_t snes_data_packs_count(const SnesDataPacks *p) { return p ? p->entries.size() : 0; }
extern "C" const SnesDataPack *snes_data_packs_get(const SnesDataPacks *p, size_t i) {
  return p && i < p->entries.size() ? &p->entries[i].view : nullptr;
}
extern "C" void snes_data_packs_destroy(SnesDataPacks *p) { delete p; }
extern "C" const char *snes_data_pack_directory(SnesDataPacks *p, size_t i,
    const char *cache, SnesDataPackError error, void *user) {
  try {
    check(p && i < p->entries.size() && cache, "Invalid directory request");
    auto &entry = p->entries[i];
    if (entry.disk_root.empty()) entry.disk_root = materialize(entry, fs::u8path(cache)).u8string();
    return entry.disk_root.c_str();
  } catch (const std::exception &e) {
    if (error) error(user, p && i < p->entries.size() ? p->entries[i].source.c_str() : "", e.what());
    return nullptr;
  }
}
extern "C" int snes_data_pack_read(const SnesDataPacks *p, size_t i, const char *name,
    size_t limit, uint8_t **bytes, size_t *size, SnesDataPackError error, void *user) {
  if (bytes) *bytes = nullptr;
  if (size) *size = 0;
  try {
    check(p && i < p->entries.size() && name && bytes && size, "Invalid asset read arguments");
    auto b = p->entries[i].read(name, limit);
    auto out = static_cast<uint8_t *>(std::malloc(b.empty() ? 1 : b.size()));
    check(out != nullptr, "Cannot allocate asset buffer");
    if (!b.empty()) std::memcpy(out, b.data(), b.size());
    *bytes = out; *size = b.size(); return 1;
  } catch (const std::exception &e) { if (error) error(user, name ? name : "", e.what()); return 0; }
}
