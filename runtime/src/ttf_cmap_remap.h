#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace nx {

// Remaps Switch controller button PUA codepoints in `shared_5.ttf` to match `style`
// (1 Nintendo, 2 Xbox, 3 PlayStation), `action_to_physical` (16 entries mapping
// overlay::Control -> overlay::Physical 0..15), and `title_quit_action` (mapped to 0xE0F8).
// Leaves `ttf` untouched and returns false when the font does not contain the 0xE200..0xE22F
// physical glyph set or is invalid.
bool remap_button_font_cmap(std::vector<uint8_t>& ttf, int style, const std::array<int, 16>& action_to_physical,
                            int title_quit_action = 9);

}  // namespace nx
