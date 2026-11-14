# Super Mario Bros. Wonder: what the game is made of

Facts measured from the base game version 1.0.0 (title ID 010015100B514000). Analysis used local extracted modules;
all game-derived data stays in `private/wonder/` and is never committed.

## Modules

| Module | Size | Imports | Exports | Notes |
|---|---|---|---|---|
| `rtld` | 0x32a0 | 18 | 0 | not loaded (the runtime does its job) |
| `main` | 0x359cc28 (53.6 MB) | 1,107 | 88 | the game |
| `sdk` | 0xd4d8d0 (13.3 MB) | 13 | 27,619 | NintendoSDK 16.2.0 + NVN 16.2.0 driver |

No `subsdk*`. Load bases: `main` 0x40_0000_0000, `sdk` 0x40_1000_0000 (as for Hades).

SDK and middleware markers: `NintendoSdk_nnSdk-16_2_0`, `NintendoSDK_NVN-16_2_0` (in `sdk`); NintendoWare
`G3d2`, `Ui2d`, `Vfx2`, `Font` 16.2.0; Pia 6.30 (Clone, Common, Lan, Local, Npln, Session, Sync, Transport);
NPLN 1.16.4 with gRPC, OpenSSL, libcurl, Google APIs (online play); libz. The engine is sead (Nintendo EPD) with agl
on top of a statically linked nn::gfx.

## Imports of `main` by namespace

244 libc, 197 nn::audio, 119 nn::os, 118 C++, 53 nn::hid, 35 nn::util, 33 nn::async, 30 nn::socket, 29 nn::fs,
27 nn::prepo, 25 nn::ldn, 24 nn::fontll, 24 nn::account, 20 nn::oe, 18 nn::image, 14 nn::time, 12 nn::swkbd,
11 nn::friends, 9 nn::mem, 9 nn::err, 9 nn::crypto, 8 nn::nifm, 8 nn::ssl, 5 nn::diag, 5 nn::vi, 5 nn::ngc, 3 nn::pl,
3 nv::, 3 nn::lmem, 2 nn::nsd, **2 nvn** (`nvnBootstrapLoader`, `nvnPrivate0`), 1 each nn::codec/settings/detail.

791 of these are not imported by Hades (`private/wonder/imports_new_vs_hades.txt`). The imports resolve to `sdk`
exports, which are recompiled; what the runtime has to provide are the **services** behind them (IPC), not the
functions, except where we choose HLE (NVN, audio).

### Audio

Hades used AudioOut. Wonder uses the **AudioRenderer** (`nn::audio::*` over `audren:u`): the game builds a
renderer configuration (voices, wave buffers, ADPCM/PCM/Opus, mixes, effects: delay, reverb, I3DL2 reverb, aux,
biquad) and the service, on hardware the audio DSP, mixes it.

The SDK 16 `sdk` also carries the renderer's **server** side: `nn::audio::server::AudioRendererManagerImpl` (with
`AudioRendererImpl`, `AudioDeviceImpl`, `ExecuteAudioRendererRendering`), reachable in process through
`nn::audio::CreateAudioRendererManagerByDfc` ("direct function call") next to the usual
`CreateAudioRendererManagerByHipc`. If the DFC path can be selected, the update parsing, command generation and
possibly the mixing come recompiled for free, and only the final output needs the host. To verify during boot.

## NVN

`main` imports `nvnBootstrapLoader` from `sdk` and loads the whole C API by name (536 `nvn*` strings in `main`,
`private/wonder/nvn_names_main.txt`). By object:

149 CommandBuffer, 71 Texture, 40 Sampler, 33 Device, 33 Queue, 24 Window, 23 MultisampleState, 18 Buffer,
18 MemoryPool, 15 BlendState, 14 TextureView, 13 DepthStencilState, 11 Event, 9 PolygonState, 8 Program,
8 SamplerPool, 8 TexturePool, 7 ColorState, 5 Sync, 5 VertexAttribState, 5 VertexStreamState, 3 ChannelMaskState,
2 CommandBufferMulti, plus debug and texel read/write helpers.

Loading every name does not mean using every function: the N2 tracer tells which are called.

## Shaders

All RomFS files ending in `.zs` are plain zstd (no dictionary): `zstd -d`.

| Where | What |
|---|---|
| `Shader/SecredUber.Product.100.product.Nin_NX_NVN.bfsha.zs` | 42 MB BFSHA (`FSHA`): the G3d2 uber shader with its variations |
| `Shader/Secred.Nin_NX_NVN.release.sarc.zs` | agl shaders (`GameAglShader.sharcb`), gsys configuration |
| `Effect/static.Nin_NX_NVN.esetb.byml.zs` | 102 MB Vfx2 effect set with its shaders; 33 more per-effect `esetb` |
| `Layout/*.Nin_NX_NVN.blarc.zs` | Ui2d layouts (234) |

Shader code is Maxwell machine code inside BNSH containers (N5).

## RomFS at a glance

10,181 `bwav`, 2,934 `pack.zs`, 1,270 `bfres.zs` (models), 557 `bars.zs`, 531 `bntx.zs` (textures), 447 `bcett.byml.zs`,
447 + 91 Havok physics (`bphsc`/`bphsh`), BYML/BGYML game parameters.

## Recompilation

| | `main` | `sdk` |
|---|---|---|
| Functions | 209,629 | 40,395 |
| Instructions | 16.3M | 1.5M |
| Jump tables | 1,742 | 661 |
| Unresolved `br` (go through `nx::call`) | 11,092 | |
| Generated C++ | ~630 MB, 1,198 files | ~140 MB |

```sh
private/venv/bin/python -m recomp private/wonder/exefs/main private/wonder/gen/main --name main --base 0x4000000000 --wonder-realtime
private/venv/bin/python -m recomp private/wonder/exefs/sdk  private/wonder/gen/sdk  --name sdk  --base 0x4010000000
```

The recompiler peaks at ~2.4 GB of memory on `main` (about 6 minutes on the M1).

### Frame-rate-independent simulation step

The game's `main+0xc9528` and `main+0xc9588` paths convert the present interval (1 or 2) into Wonder's per-frame
simulation ratio and time step. That makes simulation speed depend on the number of rendered frames. The Wonder 1.0.0
game-build verifies and rewrites those instruction sequences to load the step from an aligned page appended to the
module's BSS. The runtime updates the page from a rolling average of elapsed time between presents (five samples,
capped at 1/15 s to spread brief hitches). `NX_REALTIME_FRAME_STEP=0` restores the old interval-based step for
comparison. The default presentation cap remains 60 FPS. The Video settings offer 30, 60, 90, 120, and 144 FPS;
90/120/144 use a monotonic host-side deadline rather than the 60 Hz VI timer. `NX_FPS_LIMIT=90`, `120`, or `144`
can override the saved cap and takes precedence over `NX_NO_FRAME_LIMIT`. The metrics overlay's Game speed value
reports the applied simulation-time / elapsed-time ratio, rather than FPS divided by 60.

This uses the game's existing VFR ratio and time-step fields; it does not scale the system clock or unrelated waits.

Steps shorter than a 60 Hz frame are something the game never had on the Switch, and a few of its checks race at
them. Known and handled: the Wonder Seed that comes out of a pipe at the end of a Wonder (1-2 and others; also in
plain 0.4.0). Its appearance (AI `ItemAppearNextGoto`, pickup sensor "Target" off) ends when its "Land" animation
does, 0.20 s after it touches down, but its ground contact goes 0.18 s after touchdown and the AI then plays "Fall",
which never ends: at 60 FPS the checks fall at 0.183 s and 0.200 s and it lands; at 120 one falls at 0.175 s and the
seed stays untakeable. `games/wonder/seed_guard.cpp` keeps that seed's ground contact once it has touched down,
until its sensor is back on (only Wonder Seeds, only in that window; it changes nothing at 60 FPS).
`NX_SEED_GUARD=60` holds the frame cap at 60 for that window instead, `NX_SEED_GUARD=0` turns it off. Other items
that come out of pipes use the same AI and may race the same way (not seen yet).
The patch verifies the original instruction words, so another game version fails the game-code build instead of being
patched silently. The addresses and instruction sequence were cross-checked against the Wonder frame-step analysis in
[MacNativeGames](https://github.com/ly05010419/MacNativeGames/blob/main/games/wonder/runtime/game_patches_wonder.c).

## Host: MoltenVK on the Apple M1

`vulkaninfo` (MoltenVK 1.4.2, API 1.4.357). Present: `VK_EXT_external_memory_host`, `extended_dynamic_state`
1/2/3, `VK_KHR_dynamic_rendering`, timeline semaphores, `synchronization2`, descriptor indexing, `robustness2`,
`depth_clip_control`, `fragment_shader_interlock`, `shader_float16_int8`, ASTC LDR and BC textures, tessellation,
depth clamp. Absent: `geometryShader`, `VK_EXT_transform_feedback`, `logicOp`, `vertex_input_dynamic_state`,
`shaderFloat64`.

Consequences for the plan: memory pools can be imported directly (the PC path of N3 works on the Mac); ASTC needs
no decoder; transform feedback, geometry shaders and logic ops, if the trace shows Wonder uses them, need
workarounds (compute emulation / shader rewrites).
