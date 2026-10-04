# Steam Frame

The same APK as for the Quest runs on the Steam Frame, in Lepton (SteamOS's
Android, a Waydroid fork: Android 11, arm64), with SteamVR's OpenXR runtime.
What differs is found at run time, not by a build of its own.

## The device

- Steam Settings > System > Enable Developer Mode; then start "Lepton
  Development" from the library. While the headset sleeps Lepton stops, and
  adb with it.
- adb over the network: `adb connect <frame-ip>:5555`; over USB:
  `adb forward tcp:5555 tcp:5555` then `adb connect localhost:5555`.
  Always name the device (`adb -s <frame-ip>:5555 ...`): a Quest may be
  connected as well.
- Install with `adb install -r --no-streaming`. It prints "Lepton post
  installation failed due to APP_PATH being unset", but installs. Start the
  game with `am start -n com.halo.decomp/.LauncherActivity`.
- Lepton empties `/sdcard/Android/data` whenever it starts again: the game's
  data must live elsewhere (stage 2). `/sdcard/Documents` and
  `/sdcard/Download` are the SteamOS user's folders and are kept.

## What the port does for it

- SDL3's clipboard handler takes a clipboard service for granted, which
  Lepton has not: `tools/android_build.py` patches it to do without.
- SteamVR lists an implicit OpenXR layer for Vulkan apps whose library an
  app cannot load: the APK carries a pass-through layer under its name
  (`port/android/xr_layer/fdm_injection_stub.c`).

## Stages

| # | Stage | State |
| --- | --- | --- |
| 1 | Base: the fixes above on the current main | in progress |
| 2 | The game's data where Lepton keeps it | |
| 3 | Menus, HUD and text visible (quad layers) | |
| 4 | The freeze at the cryo bay's calibration | |
| 5 | The Frame's controllers | |
| 6 | Frame rate, resolution | |
| 7 | Multiplayer, voice, cross-play with the Quest | |
| 8 | Installing it, for others | |

## Findings

- 2026-10-02: runs flat and in VR (GLES 3.2 by Mesa zink on Turnip, eyes
  1728x1728, 72 Hz, XR_FB_display_refresh_rate absent). Quad layers
  (menus, HUD, instructions) are not shown; the projection layer is. Froze
  at the cryo bay's look calibration; its log was lost to Lepton's wipe.
