#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>
namespace nx::profiles {
std::vector<uint8_t> builtin_icon(int index);
std::vector<uint8_t> import_icon(const std::filesystem::path& path);
std::vector<uint8_t> decode_icon_rgba(const std::filesystem::path& path, int out_size = 128);
}
