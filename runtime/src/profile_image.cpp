#include "profile_image.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STB_IMAGE_IMPLEMENTATION
#include "../../third_party/stb/stb_image.h"
#define STBI_WRITE_NO_STDIO
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../../third_party/stb/stb_image_write.h"
namespace nx::profiles {
namespace {
void append(void* ctx, void* data, int n) {
    auto& out = *static_cast<std::vector<uint8_t>*>(ctx);
    auto* bytes = static_cast<uint8_t*>(data);
    out.insert(out.end(), bytes, bytes + n);
}
std::vector<uint8_t> jpeg(const std::vector<uint8_t>& rgb) {
    std::vector<uint8_t> out;
    if (!stbi_write_jpg_to_func(append, &out, 256, 256, 3, rgb.data(), 88) || out.size() > 0x20000) out.clear();
    return out;
}
}
std::vector<uint8_t> builtin_icon(int index) {
    static constexpr uint8_t colors[][3] = {{70,135,230},{239,120,95},{90,180,140},{165,112,207},{232,173,70},{65,180,190}};
    if (index < 0 || index >= 6) return {};
    std::vector<uint8_t> rgb(256 * 256 * 3);
    for (int y=0; y<256; ++y) for (int x=0; x<256; ++x) {
        const int dx=x-128, dy=y-128;
        const bool symbol = index % 3 == 0 ? (std::abs(dx)+std::abs(dy)<80) :
                            index % 3 == 1 ? (dx*dx+dy*dy<72*72) :
                            (std::abs(dx)<24 || std::abs(dy)<24) && std::abs(dx)<80 && std::abs(dy)<80;
        for (int c=0; c<3; ++c) rgb[(y*256+x)*3+c] = symbol ? 245 : colors[index][c];
    }
    return jpeg(rgb);
}
std::vector<uint8_t> import_icon(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto len = f.tellg();
    if (len <= 0 || len > 16*1024*1024) return {};
    std::vector<uint8_t> bytes(size_t(len), 0);
    f.seekg(0); if (!f.read(reinterpret_cast<char*>(bytes.data()), len)) return {};
    int w=0,h=0,n=0;
    if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w,&h,&n) || w<1 || h<1 || w>8192 || h>8192 || int64_t(w)*h>32000000) return {};
    unsigned char* src=stbi_load_from_memory(bytes.data(), int(bytes.size()), &w,&h,&n,3);
    if (!src) return {};
    // Centre-crop to a square; sample without changing the source image.
    const int side=std::min(w,h), ox=(w-side)/2, oy=(h-side)/2;
    std::vector<uint8_t> rgb(256*256*3);
    for(int y=0;y<256;++y) for(int x=0;x<256;++x) {
        const int sx=ox+std::min(side-1,int((x+0.5)*side/256));
        const int sy=oy+std::min(side-1,int((y+0.5)*side/256));
        for(int c=0;c<3;++c) rgb[(y*256+x)*3+c]=src[(sy*w+sx)*3+c];
    }
    stbi_image_free(src);
    return jpeg(rgb);
}
std::vector<uint8_t> decode_icon_rgba(const std::filesystem::path& path, int out_size) {
    if (out_size <= 0) return {};
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    auto len = f.tellg();
    if (len <= 0 || len > 16 * 1024 * 1024) return {};
    std::vector<uint8_t> bytes(size_t(len), 0);
    f.seekg(0);
    if (!f.read(reinterpret_cast<char*>(bytes.data()), len)) return {};
    int w = 0, h = 0, n = 0;
    if (!stbi_info_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n) || w < 1 || h < 1 || w > 8192 ||
        h > 8192 || int64_t(w) * h > 32000000)
        return {};
    unsigned char* src = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!src) return {};
    const int side = std::min(w, h), ox = (w - side) / 2, oy = (h - side) / 2;
    std::vector<uint8_t> rgba(size_t(out_size) * size_t(out_size) * 4);
    for (int y = 0; y < out_size; ++y) {
        const int sy = oy + std::min(side - 1, int((y + 0.5) * side / out_size));
        for (int x = 0; x < out_size; ++x) {
            const int sx = ox + std::min(side - 1, int((x + 0.5) * side / out_size));
            const size_t di = (size_t(y) * size_t(out_size) + size_t(x)) * 4;
            const size_t si = (size_t(sy) * size_t(w) + size_t(sx)) * 4;
            rgba[di + 0] = src[si + 0];
            rgba[di + 1] = src[si + 1];
            rgba[di + 2] = src[si + 2];
            rgba[di + 3] = src[si + 3];
        }
    }
    stbi_image_free(src);
    return rgba;
}
}
