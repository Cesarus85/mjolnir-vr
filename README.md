<p align="center">
  <img src="port/android/steam_frame_grid/library.png" width="640" alt="Ringfall: Mjolnir VR">
</p>

# Ringfall: Mjolnir VR

Ringfall: Mjolnir VR plays **Halo: Combat Evolved** for the original Xbox in
virtual reality, on the **Meta Quest 3 / 3S** and the **Steam Frame**. You
play your own copy: the game data comes from your own disc image of the
game, and this project includes none of it.

> **Unofficial fan project.** Ringfall: Mjolnir VR is not affiliated with,
> endorsed or sponsored by Microsoft, Xbox Game Studios, Bungie or Halo
> Studios. Halo is a trademark of Microsoft Corporation; it is named here
> only to say which game the project works with. No game data, art or
> music of the game is distributed.

## What it does

- **Stereo 6DoF VR** at the headset's refresh rate (72 to 120 Hz), the HUD,
  menus, scope and crosshair as layers of their own.
- **Your hands and arms**: the weapon in the hand, its shots from the
  muzzle, arms by inverse kinematics, fingers from the controllers;
  two-handed weapons, melee by swinging, grenades thrown with the hand,
  physical crouch and lean.
- **Vehicles**: the Warthog's wheel turned with your hands, turrets with
  handles, first-person seats.
- **Comfort**: snap or smooth turning in fine steps, comfort vignettes,
  left-handed mode with mirrored controls, buttons remapped freely,
  graphics presets, refresh rate and resolution in the VR options.
- **Multiplayer**: the PC version's menus and server browser, join codes,
  password lobbies, co-op campaign, bots that make room for people, up to
  128 players; cross-play with the desktop and phone builds of
  [OpenCE](https://github.com/OpenCommunityEdition/OpenCE).

## Install

You need your own Xbox disc image of Halo: Combat Evolved (`.iso` or
`.xiso`, any version).

### Meta Quest 3 / 3S

1. Download `ringfall-mjolnir-vr-android-release.zip` from the
   [latest release](../../releases/latest) and unpack it.
2. Install the APK with [SideQuest](https://sidequestvr.com/) or
   `adb install`.
3. Copy your disc image to the Quest's `Download` folder (USB cable, then
   the Quest's storage on your computer).
4. Start **Ringfall: Mjolnir VR** (Library > Unknown Sources), select
   **Choose disc image** and pick it. The game copies its maps out of the
   image (about 1.8 GB) and starts. You can delete the image afterwards.

The game looks for new releases when it starts and offers to update itself.

### Steam Frame

1. In Desktop Mode, put your disc image in `Downloads`.
2. Download `Install-Ringfall-Mjolnir-VR.desktop` from the
   [latest release](../../releases/latest) and open it.
3. Answer the dialogs: the installer finds the disc image, copies the game
   data out of it, adds **Ringfall: Mjolnir VR** to your Steam library and
   switches back to Gaming Mode.

Open the same file again to update. Details:
[port/android/STEAM_FRAME.md](port/android/STEAM_FRAME.md).

## Documentation

| Topic | Where |
| --- | --- |
| The Android/VR app: build, controls, settings | [port/android/README.md](port/android/README.md) |
| The Steam Frame | [port/android/STEAM_FRAME.md](port/android/STEAM_FRAME.md) |
| What is done and what comes next | [port/android/VR_ROADMAP.md](port/android/VR_ROADMAP.md) |
| The port it is built on (desktop builds, netcode, build options) | [docs/OPENCE_README.md](docs/OPENCE_README.md) |

To build it: `python3 configure.py --release`, then `ninja android_apk`
(Android SDK and NDK needed; see the Android README).

## Credits

- [OpenCE](https://github.com/OpenCommunityEdition/OpenCE), the port of the
  decompilation to Linux, Windows and Android that this project follows.
- The decompilation: [bnunu/halo-1](https://github.com/bnunu/halo-1), a fork
  of [punpckhdq/halo](https://github.com/punpckhdq/halo).
- Ideas for the arms, hands and left-handed mode from
  [HaloCE-Quest-VR](https://github.com/moistman42069/HaloCE-Quest-VR) and
  [astromaddie/HaloCE-VR](https://github.com/astromaddie/HaloCE-VR) (CC0).
- The disc image readers follow [extract-xiso](https://github.com/XboxDev/extract-xiso)
  (its license: [port/third_party/extract-xiso](port/third_party/extract-xiso/LICENSE.TXT)).
- Fonts: Overpass (SIL Open Font License).

The project's own code is released under [CC0](LICENSE.md), as the
decompilation it builds on.
