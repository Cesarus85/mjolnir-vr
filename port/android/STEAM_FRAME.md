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
| 1 | Base: the fixes above on the current main | done |
| 2 | The game's data where Lepton keeps it | done: `/home/steamos/Documents/HaloCE` |
| 3 | Menus, HUD and text visible (quad layers) | done: their alpha from their colour |
| 4 | The freeze at the cryo bay's calibration | not seen again (2026-10-04) |
| 5 | The Frame's controllers | done: their own profile, the Xbox's layout |
| 6 | Frame rate, resolution | done: 72 Hz steady, multiview on |
| 7 | Multiplayer, voice, cross-play with the Quest | |
| 8 | A button in the Steam library | done: `steam_frame_launch.sh` |

## Starting it on the Frame

A button in the Steam library ("Halo VR", a non-Steam game) runs
`~/Documents/HaloCE/halo-vr.sh` (`port/android/steam_frame_launch.sh`),
which starts the game as Steam starts an Android title of its own:
`lepton waitforexitandrun -- ~/Documents/HaloCE/HaloVR.apk`, in a container
of its own (`steamlaunch-4290000001`) that Lepton installs the APK into
anew whenever the file is newer. Updating the game is replacing that APK.

What the script does that Steam would for an Android title:
- a compat data path (`steamapps/compatdata/4290000001/internal`): after an
  install Lepton links the app's data to it, a dangling link without it;
  and the shader path, which Lepton reads Steam's shader dump path against;
- a container id of its own: with the shortcut's id, SteamVR's client in the
  container stalled in xrCreateInstance;
- a few starts: a new APK's first start only installs it, and a start that
  ends within 30 s is tried again (up to three times), unless the game was
  quit (it writes `quit_requested.txt`).

Steam's "Exit game" does not reach the game (its process is the container's,
not Steam's): quit it with the VR options' "Quit Halo" (Y in the main or
pause menu, twice). A container ended within 30 s of its start makes
Lepton drop the app's install, so the next start installs it again.

The shortcut is in `userdata/<id>/config/shortcuts.vdf` (Steam reads it when
it starts; appid 3151454348 for this name and path), its pictures in
`userdata/<id>/config/grid/` (from `port/android/steam_frame_grid/`). From
the Mac, over USB the Frame's SteamOS is an adb device of its own, `frame`,
beside Lepton's `<frame-ip>:5555`.

## The game's data

Lepton uninstalls the app and empties its folders whenever it starts again.
The game's data lives in the SteamOS user's Documents instead: when
`/home/steamos/Documents/HaloCE` holds `maps/ui.map` and is writable, the
launcher and the game take it as their data folder (config.toml, saves and
debug.txt go there too). Once:

    adb -s <frame-ip>:5555 shell mkdir -p /home/steamos/Documents/HaloCE/maps
    adb -s <frame-ip>:5555 push assets/maps/. /home/steamos/Documents/HaloCE/maps/
    adb -s <frame-ip>:5555 shell chmod -R 777 /home/steamos/Documents/HaloCE

After a Lepton restart only the APK is installed again. (Use the path under
/home, not /sdcard/Documents: Android 11's scoped storage stands between the
app and the latter.)

## The controllers

SteamVR offers the Frame's controllers as themselves
(`XR_VALVE_frame_controller_interaction`,
`/interaction_profiles/valve/frame_controller_valve`; Valve's "Steam Frame
Input" page). Without bindings for it, SteamVR maps the Quest's onto them,
and the left Touch controller's X and Y land on the D-pad. With them the
game gets the Xbox's layout, as the right controller has it: A jump, B
flashlight (held: switch grenades), X action and reload, Y switch weapon,
menu pause; the left one's View is Back (scores), its D-pad the D-pad, the
bumpers switch grenades (left) and the flashlight (right). The VR options'
page of buttons shows this layout on the Frame.

## Findings

- 2026-10-02: runs flat and in VR (GLES 3.2 by Mesa zink on Turnip, eyes
  1728x1728, 72 Hz, XR_FB_display_refresh_rate absent). Quad layers
  (menus, HUD, instructions) are not shown; the projection layer is. Froze
  at the cryo bay's look calibration; its log was lost to Lepton's wipe.
- 2026-10-04: SteamVR draws a quad's texels by their alpha alone; the
  game's menus and HUD have none in their additive parts, so nothing showed.
  On SteamVR (by the runtime's name) the overlays' alpha now comes from their
  colour (host_xr.c; `HALO_XR_QUAD_ALPHA=colour|keep` overrides). With it,
  menus, HUD and the campaign ran well (the user: "lief 1a"); no freeze.
- 2026-10-04: no online games found: the public MQTT brokers had banned the
  home connection for a while (too many connections from tests that day);
  broker.emqx.io came back within minutes. Lepton's network is the Frame's
  own (same IP), DNS works.
- 2026-10-04: the refresh rate is SteamOS's setting; the runtime offers
  only that one (the game asks for the highest at or below
  `vr.refresh_rate`, and logs the rates offered). At 72 Hz the game holds
  72 fps (about 11.6 ms a frame with multiview, eyes 7.6 ms; without it
  eyes about 9 ms). At 90 Hz it cannot (11.1 ms a frame), and SteamVR's
  making up of the missed frames showed as a doubled picture when turning
  the head. Keep the Frame at 72 Hz, or lower `vr.resolution_scale` (about
  0.85) for 90.
- 2026-10-05: doubled pictures when turning at 72 Hz too, the game at the
  edge of its frame (12.5 of 13.9 ms; SteamVR reprojecting 2-8% of the
  frames). The time was not the drawing's but waits: the layer copies read
  the GL state back (glGet*) and released each image at once, and with
  Mesa's GL thread (zink) every read waits for the thread to run all that
  is queued, every release for the GPU (the left eye's copy took 4.5-7 ms
  of the frame, the right one's, just after it, 0.7). On SteamVR the copies
  are now deferred (host_xr.c): no state read back (the game sets its own
  again), the images released together as the frame ends, and the eyes'
  work flushed to the GPU as soon as they are copied.
  `debug.xr_deferred_copies` = 0 brings back the old copies for comparing,
  1 defers them on any runtime; `vr.log_performance` logs the frame's parts
  and, with the compositor's counters, each copy's time.
- 2026-10-05 later: the doubling was the eyes not fusing (each eye sharp
  alone) with multiview, and gone drawing the eyes one by one, which is now
  SteamVR's default (`debug.multiview_steamvr` for multiview). The per-eye
  corrections were checked against the projection (x, y from the top after
  the flip, the eyes' offsets in clip units) and agree with the Quest, where
  multiview fuses; the Frame's eyes are wider and differ from each other
  more, so a driver giving the right eye the left one's correction (zink on
  Turnip: ViewIndex, Turnip's multi-position output) would show there. For
  finding it: a self-test of the driver's views when the layers are made
  (logcat "multiview: self-test"), a frame whose views are turned apart
  drawn one by one, the views' shape in the log as it changes ("VR views:"),
  and `debug.gl_environment` ("TU_DEBUG=nomultipos", "ZINK_DEBUG=nobgc",
  "MESA_SHADER_CACHE_DISABLE=true") set before the driver starts.
