# Third-party notices

Mjolnir VR is released under the GNU Affero General Public
License, version 3 or later ([LICENSE](LICENSE)); releases before build 312
were released under [CC0 1.0](LICENSES/CC0-1.0.txt). It includes or builds
on the following, under their own licenses; each license's full text is in
the file named.

## Code

| Component | Used for | License | Text |
| --- | --- | --- | --- |
| [OpenCE](https://github.com/OpenCommunityEdition/OpenCE) and the decompilation ([bnunu/halo-1](https://github.com/bnunu/halo-1), [punpckhdq/halo](https://github.com/punpckhdq/halo)) | the game's code, the port | AGPL 3.0 or later (OpenCE from 19 October 2026, punpckhdq/halo from 17 October 2026; CC0 1.0 before); bnunu/halo-1: CC0 1.0 | [LICENSE](LICENSE), [LICENSES/CC0-1.0.txt](LICENSES/CC0-1.0.txt) |
| [HaloCE-Quest-VR](https://github.com/moistman42069/HaloCE-Quest-VR) | the arm IK, forearm twist and finger poses (`port/linux/game/vr_arms.c`) | CC0 1.0 | (its repository) |
| [HaloCE-VR](https://github.com/astromaddie/HaloCE-VR) | Steam Frame findings | CC0 1.0 | (its repository) |
| [SDL 3](https://libsdl.org) | windows, input, sound | zlib | (in SDL's source, `LICENSE.txt`) |
| [Khronos OpenXR loader](https://github.com/KhronosGroup/OpenXR-SDK) | VR runtime access | Apache 2.0 | (in the loader's package) |
| [musl](https://musl.libc.org) | the C library of the game's code on Android, its maths | MIT | [port/third_party/musl-math](port/third_party/musl-math/README.md) |
| [Expat](https://libexpat.github.io) | the PC menus' definitions | MIT | [port/third_party/expat/COPYING](port/third_party/expat/COPYING) |
| [Mbed TLS](https://www.trustedfirmware.org/projects/mbed-tls/) | HTTPS for the desktop builds' updater (not in the Android app) | Apache 2.0 | [port/third_party/mbedtls/LICENSE](port/third_party/mbedtls/LICENSE) |
| [miniupnpc](https://miniupnp.tuxfamily.org) | router port forwarding for online games | BSD 3-Clause | [port/third_party/miniupnpc/LICENSE](port/third_party/miniupnpc/LICENSE) |
| [KCP](https://github.com/skywind3000/kcp) | reliable transport for online games | MIT | [port/third_party/kcp/LICENSE](port/third_party/kcp/LICENSE) |
| [Monocypher](https://monocypher.org) | signing public games' listings (Ed25519); online play's other cryptography is the port's own | CC0 1.0 or BSD 2-Clause | [port/third_party/monocypher/LICENCE.md](port/third_party/monocypher/LICENCE.md) |
| [tomlc17](https://github.com/cktan/tomlc17) | reading `config.toml` | MIT | [port/third_party/tomlc17/LICENSE](port/third_party/tomlc17/LICENSE) |
| [stb](https://github.com/nothings/stb) | drawing the game's text sharply (stb_truetype) | MIT or public domain | [port/third_party/stb/LICENSE](port/third_party/stb/LICENSE) |
| [SMAA](https://www.iryoku.com/smaa/) | anti-aliasing (flat screens; off in VR) | MIT | [port/third_party/smaa/LICENSE](port/third_party/smaa/LICENSE) |

The disc image readers (`XisoExtractor.java`, `steam_frame_xiso.py`,
`xiso.c`) are written from a description of the Xbox's disc file system
([xboxdevwiki's XDVDFS page](https://xboxdevwiki.net/XDVDFS)), not from
extract-xiso, which earlier releases followed.

## Fonts

| Font | Used for | License | Text |
| --- | --- | --- | --- |
| Overpass (Red Hat, Delve Withrington) | the game's text, the library pictures | SIL Open Font License 1.1 | [port/assets/fonts/Overpass-OFL.txt](port/assets/fonts/Overpass-OFL.txt) |
| OpenCE (after Newtown) | the menus' titles | SIL Open Font License 1.1 | [port/assets/fonts/OpenCE-OFL.txt](port/assets/fonts/OpenCE-OFL.txt) |
| Newtown (Roger White) | OpenCE's source | public domain | [port/assets/fonts/Newtown-LICENSE.txt](port/assets/fonts/Newtown-LICENSE.txt) |

See [port/assets/fonts/README.md](port/assets/fonts/README.md) for where
each font is used.

## Not included

No file of Halo: Combat Evolved (maps, sounds, art, music, fonts or
executables) is part of this repository or its releases. The game reads
them from the player's own disc image.
