// The NVN present pass (docs/nvn/ANDROID-PORT.md A2; DEBUG-2.md #79): the window texture drawn into the swapchain
// image by a render pass instead of vkCmdBlitImage.
// - Rotated by the swapchain's preTransform: phones scan out portrait, and a pre-rotated image goes to the display
//   controller as is (the compositor rotates otherwise, on the GPU whenever system UI is on top).
// - Fit with bars: a 16:9 picture on a 19.5:9 screen keeps its shape.
// - Alpha 1: an Android surface may take the image's alpha as transparency (compositeAlpha INHERIT), and the game's
//   final image has no meaningful alpha.
// On by default on Android. NX_PRESENT_PASS=1 turns it on elsewhere (checking it on the Mac), =0 off (the blit).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "nvn_vk.h"
#include "nvn_ultrawide.h"
#include "platform.h"

namespace nx::nvn {

using namespace nx::vk;

namespace {

// glslc -O --target-env=vulkan1.0 of present.vert:
//   #version 450
//   // A triangle covering the target; uv 0..1 across it (y down, as Vulkan framebuffer coordinates).
//   layout(location = 0) out vec2 uv;
//   void main() {
//       vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
//       uv = p;
//       gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
//   }
const uint32_t kVertex[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x0000002f, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000,
    0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000018, 0x00000020, 0x00040047, 0x0000000c, 0x0000000b,
    0x0000002a, 0x00040047, 0x00000018, 0x0000001e, 0x00000000, 0x00030047, 0x0000001e, 0x00000002, 0x00050048,
    0x0000001e, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001e, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000001e, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001e, 0x00000003, 0x0000000b,
    0x00000004, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x0000000a, 0x00000020, 0x00000001, 0x00040020,
    0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000001, 0x0004002b, 0x0000000a,
    0x0000000e, 0x00000001, 0x0004002b, 0x0000000a, 0x00000010, 0x00000002, 0x00040020, 0x00000017, 0x00000003,
    0x00000007, 0x0004003b, 0x00000017, 0x00000018, 0x00000003, 0x00040017, 0x0000001a, 0x00000006, 0x00000004,
    0x00040015, 0x0000001b, 0x00000020, 0x00000000, 0x0004002b, 0x0000001b, 0x0000001c, 0x00000001, 0x0004001c,
    0x0000001d, 0x00000006, 0x0000001c, 0x0006001e, 0x0000001e, 0x0000001a, 0x00000006, 0x0000001d, 0x0000001d,
    0x00040020, 0x0000001f, 0x00000003, 0x0000001e, 0x0004003b, 0x0000001f, 0x00000020, 0x00000003, 0x0004002b,
    0x0000000a, 0x00000021, 0x00000000, 0x0004002b, 0x00000006, 0x00000023, 0x40000000, 0x0004002b, 0x00000006,
    0x00000025, 0x3f800000, 0x0004002b, 0x00000006, 0x00000028, 0x00000000, 0x00040020, 0x0000002c, 0x00000003,
    0x0000001a, 0x0005002c, 0x00000007, 0x0000002e, 0x00000025, 0x00000025, 0x00050036, 0x00000002, 0x00000004,
    0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000a, 0x0000000d, 0x0000000c, 0x000500c4,
    0x0000000a, 0x0000000f, 0x0000000d, 0x0000000e, 0x000500c7, 0x0000000a, 0x00000011, 0x0000000f, 0x00000010,
    0x0004006f, 0x00000006, 0x00000012, 0x00000011, 0x000500c7, 0x0000000a, 0x00000014, 0x0000000d, 0x00000010,
    0x0004006f, 0x00000006, 0x00000015, 0x00000014, 0x00050050, 0x00000007, 0x00000016, 0x00000012, 0x00000015,
    0x0003003e, 0x00000018, 0x00000016, 0x0005008e, 0x00000007, 0x00000024, 0x00000016, 0x00000023, 0x00050083,
    0x00000007, 0x00000027, 0x00000024, 0x0000002e, 0x00050051, 0x00000006, 0x00000029, 0x00000027, 0x00000000,
    0x00050051, 0x00000006, 0x0000002a, 0x00000027, 0x00000001, 0x00070050, 0x0000001a, 0x0000002b, 0x00000029,
    0x0000002a, 0x00000028, 0x00000025, 0x00050041, 0x0000002c, 0x0000002d, 0x00000020, 0x00000021, 0x0003003e,
    0x0000002d, 0x0000002b, 0x000100fd, 0x00010038,
};

// glslc -O --target-env=vulkan1.0 of present.frag:
//   #version 450
//   // Host pixel -> the picture: `rot` turns host uv into upright uv (the swapchain's preTransform undone), `box` is where
//   // the picture sits in the upright target (uv, fit with bars), `crop` the part of the texture shown. Alpha is 1.
//   // Over it, the touch controls: `n` circles (x, y, radius in target heights, opacity) in upright uv.
//   layout(set = 0, binding = 0) uniform sampler2D src;
//   layout(set = 0, binding = 1) uniform Overlay {
//       vec4 shape[32];
//   } ov;
//   layout(push_constant) uniform P {
//       vec4 rot;   // mat2 columns (x, y) (z, w)
//       vec4 off;   // xy: rot offset; z: number of circles; w: target width / height (upright)
//       vec4 box;   // x0 y0 w h
//       vec4 crop;  // u0 v0 du dv
//   } p;
//   layout(location = 0) in vec2 uv;
//   layout(location = 0) out vec4 color;
//   void main() {
//       vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
//       vec2 b = (up - p.box.xy) / p.box.zw;
//       vec3 c = vec3(0.0);
//       if (all(greaterThanEqual(b, vec2(0.0))) && all(lessThanEqual(b, vec2(1.0))))
//           c = texture(src, p.crop.xy + b * p.crop.zw).rgb;
//       int n = int(p.off.z);
//       for (int i = 0; i < n; i++) {
//           vec4 s = ov.shape[i];
//           float d = length(vec2((up.x - s.x) * p.off.w, up.y - s.y));
//           if (d < s.z)
//               c = mix(c, vec3(1.0), s.w);
//           else if (d < s.z + 0.005)
//               c = mix(c, vec3(1.0), min(1.0, s.w * 3.0));
//       }
//       color = vec4(c, 1.0);
//   }
const uint32_t kFragment[] = {
    0x07230203, 0x00010000, 0x000d000b, 0x000000bf, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001,
    0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004,
    0x00000004, 0x6e69616d, 0x00000000, 0x00000022, 0x000000a9, 0x00030010, 0x00000004, 0x00000007, 0x00030047,
    0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x0000000b,
    0x00000001, 0x00000023, 0x00000010, 0x00050048, 0x0000000b, 0x00000002, 0x00000023, 0x00000020, 0x00050048,
    0x0000000b, 0x00000003, 0x00000023, 0x00000030, 0x00040047, 0x00000022, 0x0000001e, 0x00000000, 0x00040047,
    0x0000004b, 0x00000021, 0x00000000, 0x00040047, 0x0000004b, 0x00000022, 0x00000000, 0x00040047, 0x0000006d,
    0x00000006, 0x00000010, 0x00030047, 0x0000006e, 0x00000002, 0x00050048, 0x0000006e, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000070, 0x00000021, 0x00000001, 0x00040047, 0x00000070, 0x00000022, 0x00000000,
    0x00040047, 0x000000a9, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040017, 0x0000000a,
    0x00000006, 0x00000004, 0x0006001e, 0x0000000b, 0x0000000a, 0x0000000a, 0x0000000a, 0x0000000a, 0x00040020,
    0x0000000c, 0x00000009, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000009, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010, 0x00000009,
    0x0000000a, 0x00040018, 0x00000017, 0x00000007, 0x00000002, 0x0004002b, 0x00000006, 0x00000018, 0x3f800000,
    0x0004002b, 0x00000006, 0x00000019, 0x00000000, 0x00040020, 0x00000021, 0x00000001, 0x00000007, 0x0004003b,
    0x00000021, 0x00000022, 0x00000001, 0x0004002b, 0x0000000e, 0x00000025, 0x00000001, 0x0004002b, 0x0000000e,
    0x0000002c, 0x00000002, 0x00040017, 0x00000035, 0x00000006, 0x00000003, 0x0006002c, 0x00000035, 0x00000038,
    0x00000019, 0x00000019, 0x00000019, 0x00020014, 0x00000039, 0x0005002c, 0x00000007, 0x0000003b, 0x00000019,
    0x00000019, 0x00040017, 0x0000003c, 0x00000039, 0x00000002, 0x0005002c, 0x00000007, 0x00000042, 0x00000018,
    0x00000018, 0x00090019, 0x00000048, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001,
    0x00000000, 0x0003001b, 0x00000049, 0x00000048, 0x00040020, 0x0000004a, 0x00000000, 0x00000049, 0x0004003b,
    0x0000004a, 0x0000004b, 0x00000000, 0x0004002b, 0x0000000e, 0x0000004d, 0x00000003, 0x00040015, 0x0000005b,
    0x00000020, 0x00000000, 0x0004002b, 0x0000005b, 0x0000005c, 0x00000002, 0x00040020, 0x0000005d, 0x00000009,
    0x00000006, 0x0004002b, 0x0000005b, 0x0000006c, 0x00000020, 0x0004001c, 0x0000006d, 0x0000000a, 0x0000006c,
    0x0003001e, 0x0000006e, 0x0000006d, 0x00040020, 0x0000006f, 0x00000002, 0x0000006e, 0x0004003b, 0x0000006f,
    0x00000070, 0x00000002, 0x00040020, 0x00000072, 0x00000002, 0x0000000a, 0x0004002b, 0x0000005b, 0x0000007d,
    0x00000003, 0x0006002c, 0x00000035, 0x00000090, 0x00000018, 0x00000018, 0x00000018, 0x0004002b, 0x00000006,
    0x00000099, 0x3ba3d70a, 0x0004002b, 0x00000006, 0x000000a1, 0x40400000, 0x00040020, 0x000000a8, 0x00000003,
    0x0000000a, 0x0004003b, 0x000000a8, 0x000000a9, 0x00000003, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x00050041, 0x00000010, 0x00000011, 0x0000000d, 0x0000000f, 0x0004003d,
    0x0000000a, 0x00000012, 0x00000011, 0x00050051, 0x00000006, 0x0000001a, 0x00000012, 0x00000000, 0x00050051,
    0x00000006, 0x0000001b, 0x00000012, 0x00000001, 0x00050051, 0x00000006, 0x0000001c, 0x00000012, 0x00000002,
    0x00050051, 0x00000006, 0x0000001d, 0x00000012, 0x00000003, 0x00050050, 0x00000007, 0x0000001e, 0x0000001a,
    0x0000001b, 0x00050050, 0x00000007, 0x0000001f, 0x0000001c, 0x0000001d, 0x00050050, 0x00000017, 0x00000020,
    0x0000001e, 0x0000001f, 0x0004003d, 0x00000007, 0x00000023, 0x00000022, 0x00050091, 0x00000007, 0x00000024,
    0x00000020, 0x00000023, 0x00050041, 0x00000010, 0x00000026, 0x0000000d, 0x00000025, 0x0004003d, 0x0000000a,
    0x00000027, 0x00000026, 0x0007004f, 0x00000007, 0x00000028, 0x00000027, 0x00000027, 0x00000000, 0x00000001,
    0x00050081, 0x00000007, 0x00000029, 0x00000024, 0x00000028, 0x00050041, 0x00000010, 0x0000002d, 0x0000000d,
    0x0000002c, 0x0004003d, 0x0000000a, 0x0000002e, 0x0000002d, 0x0007004f, 0x00000007, 0x0000002f, 0x0000002e,
    0x0000002e, 0x00000000, 0x00000001, 0x00050083, 0x00000007, 0x00000030, 0x00000029, 0x0000002f, 0x0007004f,
    0x00000007, 0x00000033, 0x0000002e, 0x0000002e, 0x00000002, 0x00000003, 0x00050088, 0x00000007, 0x00000034,
    0x00000030, 0x00000033, 0x000500be, 0x0000003c, 0x0000003d, 0x00000034, 0x0000003b, 0x0004009b, 0x00000039,
    0x0000003e, 0x0000003d, 0x000300f7, 0x00000040, 0x00000000, 0x000400fa, 0x0000003e, 0x0000003f, 0x00000040,
    0x000200f8, 0x0000003f, 0x000500bc, 0x0000003c, 0x00000043, 0x00000034, 0x00000042, 0x0004009b, 0x00000039,
    0x00000044, 0x00000043, 0x000200f9, 0x00000040, 0x000200f8, 0x00000040, 0x000700f5, 0x00000039, 0x00000045,
    0x0000003e, 0x00000005, 0x00000044, 0x0000003f, 0x000300f7, 0x00000047, 0x00000000, 0x000400fa, 0x00000045,
    0x00000046, 0x00000047, 0x000200f8, 0x00000046, 0x0004003d, 0x00000049, 0x0000004c, 0x0000004b, 0x00050041,
    0x00000010, 0x0000004e, 0x0000000d, 0x0000004d, 0x0004003d, 0x0000000a, 0x0000004f, 0x0000004e, 0x0007004f,
    0x00000007, 0x00000050, 0x0000004f, 0x0000004f, 0x00000000, 0x00000001, 0x0007004f, 0x00000007, 0x00000054,
    0x0000004f, 0x0000004f, 0x00000002, 0x00000003, 0x00050085, 0x00000007, 0x00000055, 0x00000034, 0x00000054,
    0x00050081, 0x00000007, 0x00000056, 0x00000050, 0x00000055, 0x00050057, 0x0000000a, 0x00000057, 0x0000004c,
    0x00000056, 0x0008004f, 0x00000035, 0x00000058, 0x00000057, 0x00000057, 0x00000000, 0x00000001, 0x00000002,
    0x000200f9, 0x00000047, 0x000200f8, 0x00000047, 0x000700f5, 0x00000035, 0x000000b9, 0x00000038, 0x00000040,
    0x00000058, 0x00000046, 0x00060041, 0x0000005d, 0x0000005e, 0x0000000d, 0x00000025, 0x0000005c, 0x0004003d,
    0x00000006, 0x0000005f, 0x0000005e, 0x0004006e, 0x0000000e, 0x00000060, 0x0000005f, 0x000200f9, 0x00000062,
    0x000200f8, 0x00000062, 0x000700f5, 0x00000035, 0x000000b8, 0x000000b9, 0x00000047, 0x000000bd, 0x00000065,
    0x000700f5, 0x0000000e, 0x000000b7, 0x0000000f, 0x00000047, 0x000000a7, 0x00000065, 0x000500b1, 0x00000039,
    0x00000069, 0x000000b7, 0x00000060, 0x000400f6, 0x00000064, 0x00000065, 0x00000000, 0x000400fa, 0x00000069,
    0x00000063, 0x00000064, 0x000200f8, 0x00000063, 0x00060041, 0x00000072, 0x00000073, 0x00000070, 0x0000000f,
    0x000000b7, 0x0004003d, 0x0000000a, 0x00000074, 0x00000073, 0x00050051, 0x00000006, 0x00000079, 0x00000029,
    0x00000000, 0x00050051, 0x00000006, 0x0000007b, 0x00000074, 0x00000000, 0x00050083, 0x00000006, 0x0000007c,
    0x00000079, 0x0000007b, 0x00060041, 0x0000005d, 0x0000007e, 0x0000000d, 0x00000025, 0x0000007d, 0x0004003d,
    0x00000006, 0x0000007f, 0x0000007e, 0x00050085, 0x00000006, 0x00000080, 0x0000007c, 0x0000007f, 0x00050051,
    0x00000006, 0x00000083, 0x00000029, 0x00000001, 0x00050051, 0x00000006, 0x00000085, 0x00000074, 0x00000001,
    0x00050083, 0x00000006, 0x00000086, 0x00000083, 0x00000085, 0x00050050, 0x00000007, 0x00000087, 0x00000080,
    0x00000086, 0x0006000c, 0x00000006, 0x00000088, 0x00000001, 0x00000042, 0x00000087, 0x00050051, 0x00000006,
    0x0000008b, 0x00000074, 0x00000002, 0x000500b8, 0x00000039, 0x0000008c, 0x00000088, 0x0000008b, 0x000300f7,
    0x0000008e, 0x00000000, 0x000400fa, 0x0000008c, 0x0000008d, 0x00000095, 0x000200f8, 0x0000008d, 0x00050051,
    0x00000006, 0x00000092, 0x00000074, 0x00000003, 0x00060050, 0x00000035, 0x00000093, 0x00000092, 0x00000092,
    0x00000092, 0x0008000c, 0x00000035, 0x00000094, 0x00000001, 0x0000002e, 0x000000b8, 0x00000090, 0x00000093,
    0x000200f9, 0x0000008e, 0x000200f8, 0x00000095, 0x00050081, 0x00000006, 0x0000009a, 0x0000008b, 0x00000099,
    0x000500b8, 0x00000039, 0x0000009b, 0x00000088, 0x0000009a, 0x000300f7, 0x0000009d, 0x00000000, 0x000400fa,
    0x0000009b, 0x0000009c, 0x0000009d, 0x000200f8, 0x0000009c, 0x00050051, 0x00000006, 0x000000a0, 0x00000074,
    0x00000003, 0x00050085, 0x00000006, 0x000000a2, 0x000000a0, 0x000000a1, 0x0007000c, 0x00000006, 0x000000a3,
    0x00000001, 0x00000025, 0x00000018, 0x000000a2, 0x00060050, 0x00000035, 0x000000a4, 0x000000a3, 0x000000a3,
    0x000000a3, 0x0008000c, 0x00000035, 0x000000a5, 0x00000001, 0x0000002e, 0x000000b8, 0x00000090, 0x000000a4,
    0x000200f9, 0x0000009d, 0x000200f8, 0x0000009d, 0x000700f5, 0x00000035, 0x000000be, 0x000000b8, 0x00000095,
    0x000000a5, 0x0000009c, 0x000200f9, 0x0000008e, 0x000200f8, 0x0000008e, 0x000700f5, 0x00000035, 0x000000bd,
    0x00000094, 0x0000008d, 0x000000be, 0x0000009d, 0x000200f9, 0x00000065, 0x000200f8, 0x00000065, 0x00050080,
    0x0000000e, 0x000000a7, 0x000000b7, 0x00000025, 0x000200f9, 0x00000062, 0x000200f8, 0x00000064, 0x00050051,
    0x00000006, 0x000000ab, 0x000000b8, 0x00000000, 0x00050051, 0x00000006, 0x000000ac, 0x000000b8, 0x00000001,
    0x00050051, 0x00000006, 0x000000ad, 0x000000b8, 0x00000002, 0x00070050, 0x0000000a, 0x000000ae, 0x000000ab,
    0x000000ac, 0x000000ad, 0x00000018, 0x0003003e, 0x000000a9, 0x000000ae, 0x000100fd, 0x00010038,
};

constexpr size_t kOverlayMax = 32;  // Overlay.shape[32] in present.frag

struct Push {
    float rot[4], off[4], box[4], crop[4];
};

struct Pass {
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    VkExtent2D extent{};
    VkSurfaceTransformFlagBitsKHR transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    bool failed = false;
} g;

VkShaderModule module(const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule m = VK_NULL_HANDLE;
    p_vkCreateShaderModule(vk_device(), &ci, nullptr, &m);
    return m;
}

void destroy_targets() {
    VkDevice d = vk_device();
    for (VkFramebuffer f : g.framebuffers) p_vkDestroyFramebuffer(d, f, nullptr);
    for (VkImageView v : g.views) p_vkDestroyImageView(d, v, nullptr);
    g.framebuffers.clear(), g.views.clear();
}

bool make_pipeline(VkFormat format) {
    VkDevice d = vk_device();
    VkAttachmentDescription color{};
    color.format = format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1, rpi.pAttachments = &color;
    rpi.subpassCount = 1, rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1, rpi.pDependencies = &dep;
    if (p_vkCreateRenderPass(d, &rpi, nullptr, &g.render_pass) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (p_vkCreateSampler(d, &si, nullptr, &g.sampler) != VK_SUCCESS) return false;
    VkDescriptorSetLayoutBinding b[2] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &g.sampler},
        {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dli.bindingCount = 2, dli.pBindings = b;
    if (p_vkCreateDescriptorSetLayout(d, &dli, nullptr, &g.set_layout) != VK_SUCCESS) return false;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1, pli.pSetLayouts = &g.set_layout;
    pli.pushConstantRangeCount = 1, pli.pPushConstantRanges = &push;
    if (p_vkCreatePipelineLayout(d, &pli, nullptr, &g.layout) != VK_SUCCESS) return false;

    VkShaderModule vs = module(kVertex, sizeof kVertex), fs = module(kFragment, sizeof kFragment);
    VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
                                                 {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT, stages[0].module = vs, stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT, stages[1].module = fs, stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL, rs.cullMode = VK_CULL_MODE_NONE, rs.lineWidth = 1;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState blend{};
    blend.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1, cb.pAttachments = &blend;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2, ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2, gp.pStages = stages;
    gp.pVertexInputState = &vin, gp.pInputAssemblyState = &ia, gp.pViewportState = &vp;
    gp.pRasterizationState = &rs, gp.pMultisampleState = &ms, gp.pColorBlendState = &cb, gp.pDynamicState = &ds;
    gp.layout = g.layout, gp.renderPass = g.render_pass;
    VkResult r = p_vkCreateGraphicsPipelines(d, VK_NULL_HANDLE, 1, &gp, nullptr, &g.pipeline);
    p_vkDestroyShaderModule(d, vs, nullptr);
    p_vkDestroyShaderModule(d, fs, nullptr);
    g.format = format;
    return r == VK_SUCCESS;
}

}  // namespace

bool present_pass_wanted() {
    static const bool on = [] {
        const char* e = std::getenv("NX_PRESENT_PASS");
#if defined(__ANDROID__)
        return !e || e[0] != '0';
#else
        return e && e[0] == '1';
#endif
    }();
    return on;
}

void present_pass_release() { destroy_targets(); }  // before the swapchain whose images they view is destroyed
bool present_pass_failed() { return g.failed; }

bool present_pass_targets(VkFormat format, const std::vector<VkImage>& images, VkExtent2D extent,
                          VkSurfaceTransformFlagBitsKHR transform) {
    if (!present_pass_wanted() || g.failed) return false;
    destroy_targets();
    if (g.format != format && g.pipeline) {  // a new swapchain format: a new render pass and pipeline
        VkDevice d = vk_device();
        p_vkDestroyPipeline(d, g.pipeline, nullptr);
        p_vkDestroyRenderPass(d, g.render_pass, nullptr);
        g.pipeline = VK_NULL_HANDLE, g.render_pass = VK_NULL_HANDLE;
    }
    if (!g.pipeline && !make_pipeline(format)) {
        log("nvn/vk: present pass unavailable: frames are blitted");
        g.failed = true;
        return false;
    }
    VkDevice d = vk_device();
    for (VkImage img : images) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = img, vi.viewType = VK_IMAGE_VIEW_TYPE_2D, vi.format = format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView v = VK_NULL_HANDLE;
        p_vkCreateImageView(d, &vi, nullptr, &v);
        g.views.push_back(v);
        VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fi.renderPass = g.render_pass, fi.attachmentCount = 1, fi.pAttachments = &v;
        fi.width = extent.width, fi.height = extent.height, fi.layers = 1;
        VkFramebuffer fb = VK_NULL_HANDLE;
        if (p_vkCreateFramebuffer(d, &fi, nullptr, &fb) != VK_SUCCESS) return false;
        g.framebuffers.push_back(fb);
    }
    g.extent = extent, g.transform = transform;
    return true;
}

bool present_pass_record(VkCommandBuffer cb, GpuImage& src, const int32_t* crop, uint32_t index) {
    if (!g.pipeline || index >= g.framebuffers.size() || !src.view || src.levels != 1 || src.layers != 1 ||
        src.view_type != VK_IMAGE_VIEW_TYPE_2D || !(src.aspect & VK_IMAGE_ASPECT_COLOR_BIT))
        return false;
    transition(cb, src, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The set and the circles' uniform data come from this submit's slot (descriptor pool and upload ring, reset when
    // the slot is retired): nothing a frame still in flight reads is written. Per swapchain image they were not safe
    // (MAILBOX can hand an image back before the frame that last drew into it finished; review of DEBUG-2.md #88).
    VkDescriptorSet set = descriptor_set(g.set_layout);
    Upload ubo = upload_alloc(sizeof(float) * 4 * kOverlayMax, 256);
    std::memset(ubo.ptr, 0, sizeof(float) * 4 * kOverlayMax);
    VkDescriptorImageInfo ii{g.sampler, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bi{ubo.buffer, ubo.offset, sizeof(float) * 4 * kOverlayMax};
    VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
    w[0].dstSet = set, w[0].descriptorCount = 1, w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pImageInfo = &ii;
    w[1].dstSet = set, w[1].dstBinding = 1, w[1].descriptorCount = 1, w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[1].pBufferInfo = &bi;
    p_vkUpdateDescriptorSets(vk_device(), 2, w, 0, nullptr);

    Push p{};
    // Host uv -> upright uv, undoing the clockwise turn the display applies (as vk_present.cpp).
    static const float kRot[4][6] = {{1, 0, 0, 1, 0, 0}, {0, -1, 1, 0, 0, 1}, {-1, 0, 0, -1, 1, 1}, {0, 1, -1, 0, 1, 0}};
    int r = g.transform == VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR    ? 1
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR ? 2
            : g.transform == VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR ? 3
                                                                     : 0;
    std::memcpy(p.rot, kRot[r], 4 * sizeof(float));
    p.off[0] = kRot[r][4], p.off[1] = kRot[r][5];
    // nvnWindowSetCrop is in guest pixels; use the logical extent even when the host image is upscaled.
    float logical_width = float(src.logical_width ? src.logical_width : src.width);
    float logical_height = float(src.logical_height ? src.logical_height : src.height);
    float cx = 0, cy = 0, cw = logical_width, ch = logical_height;
    if (crop && crop[2] > 0 && crop[3] > 0 && crop[0] + crop[2] <= int32_t(logical_width) &&
        crop[1] + crop[3] <= int32_t(logical_height))
        cx = float(crop[0]), cy = float(crop[1]), cw = float(crop[2]), ch = float(crop[3]);
    p.crop[0] = cx / logical_width, p.crop[1] = cy / logical_height;
    p.crop[2] = cw / logical_width, p.crop[3] = ch / logical_height;
    bool sideways = r == 1 || r == 3;
    float tw = float(sideways ? g.extent.height : g.extent.width), th = float(sideways ? g.extent.width : g.extent.height);
    const float display_cw = cw * (src.uw ? uw_factor() : 1.0f);
    float scale = std::min(tw / display_cw, th / ch), bw = display_cw * scale / tw, bh = ch * scale / th;
    p.box[0] = (1 - bw) / 2, p.box[1] = (1 - bh) / 2, p.box[2] = bw, p.box[3] = bh;
    // The touch controls are drawn with labels by overlay.cpp on Android; elsewhere only NX_OVERLAY_DEMO uses shader circles.
    plat::OverlayShape shapes[kOverlayMax];
#if defined(__ANDROID__)
    size_t count = 0;
#else
    size_t count = plat::touch_overlay(shapes, kOverlayMax);
#endif
    auto* dst = reinterpret_cast<float*>(ubo.ptr);
    for (size_t k = 0; k < count; k++)
        dst[4 * k] = shapes[k].x, dst[4 * k + 1] = shapes[k].y, dst[4 * k + 2] = shapes[k].r, dst[4 * k + 3] = shapes[k].alpha;
    p.off[2] = float(count), p.off[3] = tw / th;

    VkRenderPassBeginInfo rb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rb.renderPass = g.render_pass, rb.framebuffer = g.framebuffers[index], rb.renderArea = {{0, 0}, g.extent};
    p_vkCmdBeginRenderPass(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport v{0, 0, float(g.extent.width), float(g.extent.height), 0, 1};
    VkRect2D sc{{0, 0}, g.extent};
    p_vkCmdSetViewport(cb, 0, 1, &v);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.pipeline);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, g.layout, 0, 1, &set, 0, nullptr);
    p_vkCmdPushConstants(cb, g.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof p, &p);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);
    return true;
}

// NX_PRESENT_TEST=<w>x<h>[:<90|180|270>] (headless, with NX_DUMP_FRAMES): the pass draws into an offscreen image
// of that size and transform, and that image is dumped in place of the window texture. It checks the rotation, the
// fit and the alpha without a phone (a Galaxy S23 held sideways: 1080x2340:90).
GpuImage* present_pass_test_target() {
    static GpuImage img;
    static bool tried = false;
    if (tried) return img.image ? &img : nullptr;
    tried = true;
    const char* e = std::getenv("NX_PRESENT_TEST");
    uint32_t w = 0, h = 0, deg = 0;
    if (!e || std::sscanf(e, "%ux%u:%u", &w, &h, &deg) < 2 || !w || !h) return nullptr;
    VkDevice d = vk_device();
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R8G8B8A8_UNORM, ci.extent = {w, h, 1};
    ci.mipLevels = 1, ci.arrayLayers = 1, ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (p_vkCreateImage(d, &ci, nullptr, &img.image) != VK_SUCCESS) return nullptr;
    VkMemoryRequirements req{};
    p_vkGetImageMemoryRequirements(d, img.image, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size, ai.memoryTypeIndex = memory_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    p_vkAllocateMemory(d, &ai, nullptr, &img.memory);
    p_vkBindImageMemory(d, img.image, img.memory, 0);
    img.format = VK_FORMAT_R8G8B8A8_UNORM, img.width = w, img.height = h;
    VkSurfaceTransformFlagBitsKHR t = deg == 90    ? VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR
                                      : deg == 180 ? VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR
                                      : deg == 270 ? VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR
                                                   : VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (!present_pass_targets(img.format, {img.image}, {w, h}, t)) {
        log("nvn/vk: present test: the pass is off (NX_PRESENT_PASS=1)");
        return nullptr;
    }
    log("nvn/vk: present test: %ux%u, turned %u degrees", w, h, deg);
    return &img;
}

}  // namespace nx::nvn
