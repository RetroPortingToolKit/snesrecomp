#pragma once
// Shared building blocks for game-owned semantic JSON adapters.
#include <cstddef>
#include <filesystem>
#include <rapidjson/document.h>
#include <string>
namespace snesrecomp::data_pack {
std::filesystem::path inside(const std::filesystem::path &root, const std::string &relative);
std::string read(const std::filesystem::path &path, size_t limit = 4 * 1024 * 1024);
void validate_json(const rapidjson::Value &value);
std::string string(const rapidjson::Value &value, const char *key);
}
