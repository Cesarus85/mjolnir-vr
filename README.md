<p align="center">
  <img src="port/android/steam_frame_grid/library.png" width="640" alt="Ringfall: Mjolnir VR">
</p>

# Ringfall: Mjolnir VR

**A VR mod for Halo: Combat Evolved** (the original Xbox game) on the
**Meta Quest 3 / 3S** and the **Steam Frame**. Play the campaign and
multiplayer in full 6DoF virtual reality, with your hands in the game.

Ringfall: Mjolnir VR runs on your headset by itself, without a PC. It
brings no game files: it uses a disc image of **your own, purchased copy**
of the game.

> **Unofficial fan project.** Ringfall: Mjolnir VR is a free,
> non-commercial fan project. It is not affiliated with, endorsed,
> sponsored or approved by Microsoft, Xbox Game Studios, Halo Studios or
> Bungie. See [Legal](#legal).

## Contents

- [What you get](#what-you-get)
- [What you can play](#what-you-can-play)
- [What you need](#what-you-need)
- [Install on Meta Quest 3 / 3S](#install-on-meta-quest-3--3s)
- [Install on Steam Frame](#install-on-steam-frame)
- [Playing](#playing)
- [Problems and questions](#problems-and-questions)
- [Credits](#credits)
- [Legal](#legal)

## What you get

- **Full VR**: stereo 6DoF at your headset's refresh rate (72 to 120 Hz);
  the HUD, menus, scope and crosshair float as layers of their own.
- **Your hands in the game**: the weapon sits in your hand and fires
  where you point it; arms and fingers follow your controllers;
  two-handed weapons, melee by swinging, grenades thrown with your hand,
  crouching and leaning with your body.
- **Vehicles**: steer the Warthog by turning its wheel with both hands;
  turrets with handles.
- **Comfort**: snap or smooth turning in fine steps, comfort vignettes,
  left-handed mode, buttons remapped freely, graphics presets, refresh
  rate and resolution, all in the in-game VR options (**Y**).
- **The PC version's menus**, clicked with a laser pointer, and an
  on-screen keyboard for names, codes and passwords.

## What you can play

- **The whole campaign**: all ten missions, on every difficulty, saved as
  you go.
- **Co-op campaign** with friends, online or on your local network, with
  settings for friendly fire and extra enemies per player.
- **LAN multiplayer** (system link) with everyone on your network.
- **Online multiplayer**, no server of your own needed: host a game and
  share its join code, find public games in the server browser, or protect
  a lobby with a password. Up to 128 players.
- **Bots** to fill a game, on foot, in vehicles and at turrets; they make
  room when a person joins.
- **All 13 multiplayer maps** and the Xbox game's game types: Slayer,
  Capture the Flag, King of the Hill, Oddball and Race.
- **Cross-play**: VR players on the Quest and the Steam Frame, together with
  players of the PC and phone builds of
  [OpenCE](https://github.com/OpenCommunityEdition/OpenCE).

## What you need

- A **Meta Quest 3 / 3S** or a **Steam Frame**.
- A **disc image** (`.iso` or `.xiso`) of **Halo: Combat Evolved for the
  original Xbox**, made from **a disc you own**. Any region and version
  works.

This project does not include, link to or help find game files. Please
do not ask for or share disc images in issues or discussions; such posts
are deleted.

## Install on Meta Quest 3 / 3S

You install the game once with a computer (Windows, macOS or Linux); after
that it updates itself.

1. **Turn on Developer Mode** for your Quest, once: in the Meta Horizon
   app on your phone, open your headset's settings, then *Developer
   Mode* (Meta asks you to create a free developer account first).
2. **Install [SideQuest](https://sidequestvr.com/setup-howto)** (the
   "Advanced Installer") on your computer and connect the Quest with a
   USB cable. In the headset, allow USB debugging for this computer.
3. **Download `RingfallMjolnirVR.apk`** from the
   [latest release](../../releases/latest) and drag it onto the SideQuest
   window. It installs the game.
4. **Copy your disc image** to the Quest's `Download` folder: in
   SideQuest, open the file manager (the folder icon), go to `Download`
   and drag the `.iso` file in.
5. **In the headset**, open your *Library*, choose *Unknown Sources* and
   start **Ringfall: Mjolnir VR**. Select **Choose disc image** and pick
   your file in `Download`. The game copies what it needs (about 1.8 GB,
   a minute or two) and starts.

You can delete the disc image from `Download` afterwards. When a new
version comes out, the game asks you at start whether to update.

## Install on Steam Frame

No computer and no keyboard needed.

1. **Switch to Desktop Mode**: press the Steam button, then *Power* >
   *Switch to Desktop*.
2. **Put your disc image** in the `Downloads` folder (for example from a
   USB stick or memory card with the file manager, Dolphin).
3. **Download `Install-Ringfall-Mjolnir-VR.desktop`** from the
   [latest release](../../releases/latest) with the web browser.
4. **Open it**: in Dolphin, open `Downloads` and double-click the file.
   If you are asked whether to run it, choose to continue.
5. **Answer the dialogs.** The installer finds your disc image, copies
   what the game needs, adds **Ringfall: Mjolnir VR** to your Steam
   library and offers to switch back to Gaming Mode.
6. Start **Ringfall: Mjolnir VR** from your library (under *Non-Steam*).

To update, open the same file in Desktop Mode again; your settings,
saves and game data stay. More about the Steam Frame:
[port/android/STEAM_FRAME.md](port/android/STEAM_FRAME.md).

## Playing

- **Y** opens the VR options in the main and pause menus: turning,
  comfort, weapon hand, buttons, graphics, refresh rate and resolution.
  *Quit the game* is on its last page.
- In the menus, point with a controller and pull the trigger to click.
- The controller layout is on the VR options' *Controller buttons* page.

## Problems and questions

Please open an [issue](../../issues) and include:

- your headset and the build number of the release you installed;
- what you did and what happened;
- the game's log, `debug.txt`:
  - Quest: `Android/data/io.github.cesarus85.ringfall/files/debug.txt`
    (SideQuest's file manager);
  - Steam Frame: `Documents/RingfallMjolnirVR/debug.txt` (and
    `logcat.txt` and `install.log` beside it).

What is done and what comes next: [port/android/VR_ROADMAP.md](port/android/VR_ROADMAP.md).

**Network use.** The game connects to the internet only for what you
use: the update check (GitHub), online games (to other players
directly), and join codes and lobby lists through public MQTT brokers
(listed in [brokers.txt](port/assets/network/brokers.txt)). There is no
telemetry.

## Credits

Ringfall: Mjolnir VR stands on the work of others:

- **[OpenCE](https://github.com/OpenCommunityEdition/OpenCE)** (Open
  Community Edition): the port of the decompiled game to Linux, Windows
  and Android, with its renderer, netcode, PC menus and multiplayer, that
  this project builds on and follows.
- **The decompilation**: [bnunu/halo-1](https://github.com/bnunu/halo-1),
  a fork of [punpckhdq/halo](https://github.com/punpckhdq/halo).
- **[HaloCE-Quest-VR](https://github.com/moistman42069/HaloCE-Quest-VR)**
  by moistman42069 (CC0): the arm IK, the forearm's twist and the
  fingers' poses are taken from it (`port/linux/game/vr_arms.c`); the
  left-handed mode and the graphics presets follow its ideas.
- **[HaloCE-VR](https://github.com/astromaddie/HaloCE-VR)** by astromaddie
  (CC0): the Steam Frame work, whose findings (such as quieting Mesa's
  trace markers) are used here.
- **[extract-xiso](https://github.com/XboxDev/extract-xiso)**: the disc
  image readers follow its format handling.
- The libraries and fonts in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Thank you all.

## Building it

`python3 configure.py --release`, then `ninja android_apk` (Android SDK
and NDK needed). See [port/android/README.md](port/android/README.md) for
the details and [docs/OPENCE_README.md](docs/OPENCE_README.md) for the
port it is built on.

## Legal

Ringfall: Mjolnir VR is an unofficial, free, non-commercial fan project.
It is not affiliated with, endorsed, sponsored or approved by Microsoft
Corporation, Xbox Game Studios, Halo Studios or Bungie. Halo, Halo: Combat
Evolved and Xbox are trademarks of Microsoft Corporation; they are named
here only to say which game this mod works with. Meta Quest is a trademark
of Meta Platforms, Inc.; Steam and Steam Frame are trademarks of Valve
Corporation.

No game data, art, music or other content of the game is included or
distributed. You need your own, legally purchased copy of the game. If
you enjoy this mod, please support the creators of the original by buying
their games.

This project's own code is released under [CC0 1.0](LICENSE.md), as the
decompilation it builds on. Third-party code and fonts keep their own
licenses: see [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
