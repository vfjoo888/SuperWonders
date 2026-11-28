# SuperWonders — Super Mario Bros. Wonder PC Recompilation

> [!NOTE]
> **Nintendo Switch 2 Edition Port:** A port for the Nintendo Switch 2 version of *Super Mario Bros. Wonder* is currently being developed over at **[wondershot](https://github.com/Kendoismebutmaybenot/wondershot)** — go check it out!
> Please note that **this is not my repository/project**; for any issues, bug reports, or questions regarding it, please open an issue directly on [their repository](https://github.com/Kendoismebutmaybenot/wondershot/issues).

I'm working on SuperWonders, an experimental Windows and Linux x86-64 static recompilation of *Super Mario Bros. Wonder* (Nintendo Switch, base version 1.0.0 and update 1.2.1) for native execution on PC. I started from the DKB/TOTK recompilation framework and have been adapting its runtime and graphics support for Wonder. The graphics backend uses Vulkan.

This repository contains source code and project documentation. It does not contain the builder package, the game, game assets, a game dump, firmware, or a compiled game. The separate Windows GUI Builder is distributed through [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases).

## What I've done

- I got a Windows build running and tested it by playing in the game's tutorial area.
- I added a first-launch shader preparation screen. It compiles the known shader program catalog before the first game frame; the current catalog contains about 12,000 programs. The Vulkan pipeline cache is saved locally for later launches. Shader variants outside the catalog can still compile the first time they are encountered, so some first-use stutter may remain.
- I fixed the misaligned and flickering shadows I saw when changing the internal resolution, and checked the result at multiple internal resolutions in the tutorial area.
- I added an in-game menu opened with **F1**, organized into Video, Audio, Controls, Overlay, and System sections.
- The Video section includes window/output size, fullscreen, always-on-top, and internal-resolution options.
- The Audio section lets me adjust game volume and select the game language. The settings menu itself is available in English, Italian, German, and French.
- The Controls section has Xbox/DualSense and Nintendo button-layout presets. It also lets me remap keyboard keys and controller buttons. The Windows runtime includes keyboard, XInput, and DualSense input support.
- On Windows, Switch 2 controllers (Pro Controller 2 and Joy-Con 2) connect over Bluetooth: hold the Sync button while the game runs. They play the game's HD rumble, as does a DualSense through its voice coils (see [docs/JOYCONS.md](docs/JOYCONS.md)). Switch 1 and Switch 2 Joy-Cons pair as one controller, or play alone held sideways (see [docs/JOYCONS.md](docs/JOYCONS.md)).
- The Overlay section has separate toggles for FPS, 1% lows, game speed, and a frametime graph, with four corner-position options.
- The System section includes a configurable memory cleanup threshold and a Low VRAM profile. Crossing the threshold asks Vulkan to release unused reusable images; it does not close the game or cap memory used by live resources, and it is not a dedicated VRAM limit. The Low VRAM profile can store decoded ASTC textures as BC3 instead of expanding them to RGBA8 when the GPU does not support ASTC. This can reduce texture memory use, with a possible image-quality tradeoff.
- I prepared a separate Windows GUI Builder package. Users provide their own extracted ExeFS/RomFS folders.
- I changed the Windows build so the game can be started directly from its executable, with an orange-flower application icon.
- I added Linux x86-64 support and a launcher that can prepare the game from the user's own extracted files.
- I added frame-rate caps up to 240 FPS, FSR 1 upscaling, FXAA, anisotropic filtering, game-language selection, and controller rumble support.
- The recomp supports every language supported by version 1.0.0 of the game.

## Testing and compatibility

I have tested the project on a Windows and Linux PC with an **NVIDIA GeForce RTX 5060 Ti**. A contributor tested on his AMD RX 9070XT on both Windows and Linux. 

**Support on intel graphic cards is still at early stages. It boots, but it may display graphical glitches**. Other systems and GPUs may behave differently. Linux support is available.

The recompilation targets the game's base version **1.0.0** and the **1.2.1** update.

### Community benchmark reports

- **NVIDIA GeForce 930M (2 GB DDR3 VRAM) / Intel Core i7-6600U / 8 GB DDR4-2133 / Kubuntu 26.04 LTS (AppImage)** ([#152](https://github.com/vfjoo888/SuperWonders/issues/152)):
  - **Settings:** 720p, Bilinear upscaling, **Low VRAM profile enabled** (required to avoid crashing on 2 GB VRAM when loading), Anti-Aliasing disabled, default texture filtering.
  - **Result:** Playable (~30 FPS mostly, with occasional drops; ~2 hours for first-run shader compilation; tested through Intro and World 1).

## Minimum hardware (provisional)

I have not established a formally tested minimum yet. The current Windows build expects:

- **CPU:** a 64-bit x86 processor with AVX2 support.
- **GPU:** a graphics card and driver supporting Vulkan 1.3. The minimum amount of video memory has not been verified.
- **System RAM:** 16 GB as a practical baseline. 32 GB is recommended when building the project, especially with a higher worker count; lowering the worker count can reduce build memory use.

These are preliminary requirements inferred from the build and runtime, not a guarantee that every compatible-looking PC will run the game well. Testing is still limited to the hardware and game area described above.

## Future features

Android is substantially more complicated and may be considered later.

## AI assistance

I used AI tools extensively to help write and revise the code, build scripts, and documentation. I directed the work and ran the builds and gameplay checks described above. AI-assisted code can still have bugs; the limited testing coverage is part of the project's current status.

## Contributor credits

Thanks to [devall96](https://github.com/devall96) for the work shared in pull requests #20–#33. I reviewed and incorporated selected ideas and implementations into my local source, including asynchronous pipeline preparation and cache saving, Vulkan validation fixes, Linux support, graphics pipeline library support, graphics and frame-rate options, launcher and release tooling, and performance/replay diagnostics. The published source is my integrated project state; the pull requests themselves were not merged as a batch. Pull request #25's feedback-loop copy change was not included.

## Getting a local build

Download the Windows GUI Builder ZIP from [GitHub Releases](https://github.com/vfjoo888/SuperWonders/releases), extract it to a writable folder, and run `Start WonderBuilder.cmd`.

In the builder, select your own extracted ExeFS folder (containing `main` and `sdk`) and RomFS folder. Choose an output folder and click **Build game**; when it finishes, start `wonder.exe` from that folder.

The builder ZIP includes its portable Windows build tools, but no game files. You must provide your own legally obtained extracted game data. I don't provide instructions for obtaining game files or firmware. Do not share the generated game output, because it contains material from your own game dump.

To update a build, keep the builder folder and output folder, extract the newer builder package over the old builder files, and build again with the same inputs and output path. The builder can reuse unchanged extraction and compilation caches.

## Project layout

- `recomp/` — AArch64 analysis and C++ code-generation tools.
- `runtime/` — native runtime, platform support, audio, input, and graphics implementation.
- `runtime/src/nvn/` — NVN-to-Vulkan graphics implementation.
- `games/wonder/` — Wonder-specific integration code.
- `tools/` — shader, packaging, and development utilities.
- `docs/` — technical notes, findings, and project history.

## Credits and licensing

Project source is licensed under the MIT License in [LICENSE](LICENSE). Third-party components and artwork are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). The project's license covers only its own code; it grants no rights to Nintendo's game or trademarks. *Super Mario Bros. Wonder* and *Nintendo Switch* are trademarks of Nintendo. This is an unofficial fan project and is not affiliated with or endorsed by Nintendo.
