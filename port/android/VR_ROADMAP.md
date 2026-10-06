# VR roadmap

What the VR port (Quest 3, Steam Frame) has, and what comes next, in the
agreed order. Updated with every step: a step is **done** once the user has
tried it in the headset and it is merged into `main`; **built** means in a
branch, not yet tried in the headset.

Several of the next steps follow
[HaloCE-Quest-VR](https://github.com/moistman42069/HaloCE-Quest-VR) and the
Steam Frame work it grew from,
[astromaddie/HaloCE-VR](https://github.com/astromaddie/HaloCE-VR) (both
CC0): their ideas, rebuilt into this port's own structure, with the source
named where code is taken over.

## Done (in `main`)

| Area | What |
| --- | --- |
| Headsets | Quest 3; Steam Frame (Lepton): its controllers' layout, a Steam library tile that installs, starts, updates and quits the game, data kept in `~/Documents/HaloCE` |
| Rendering | Both eyes in one pass (multiview) on the Quest; one by one on SteamVR, where multiview's eyes did not fuse; lens flares at their depth in stereo; HUD, menus, scope, crosshair and names as layers of their own |
| Aiming | The weapon in the hand, its shots from the muzzle, laser and crosshair options |
| Body | Physical crouch (`vr.crouch_limit`) beside the crouch button, leaning (`vr.lean_limit`), melee by swinging, grenades thrown with the left hand, two-handed weapons |
| Vehicles | The Warthog's wheel turned with the hands, right trigger throttle, left brake; first-person seats; turrets with handles |
| Comfort | Snap or smooth turning, a comfort vignette in vehicles, VR options in the main and pause menus (Y), a page of every controller button |
| Menus | The PC version's menus in VR (`vr.menus`), clicked with a laser; text typed on the on-screen keyboard |
| Multiplayer | Up to date with OpenCE (network version 20): cross-play, its server browser, co-op campaign, password lobbies; join codes and the VR online list |
| Bots | Bots in multiplayer: on foot (paths, items, objectives), in vehicles and at turrets; they make room for a joining person |

## Next, in order

| # | Step | State |
| --- | --- | --- |
| 1 | Steam Frame CPU time: Mesa's trace markers off (after astromaddie/HaloCE-VR), no `glGet` stalls in the visibility counters' copies; then measured in the headset | built (branch `frame/performance-2`) |
| 3a | Visible arms and hands: the arms by IK to the controllers, fingers by trigger and grip | |
| 2 | Controls and comfort: left-handed mode, buttons remapped, flashlight by tapping the head, finer turning and vignette choices, graphics presets and refresh rate in the VR options | |
| 3b | The whole body: torso from head and hands, legs and feet following in the room; full / arms only / hands only | |
| 3c | Weapons held physically, holsters at shoulder and hip (the buttons stay) | |
| 3d | Other VR players shown with their heads, hands and bodies (today their bodies move as flat players' do: only their aim and throws come from their hands) | |
| 4 | Later: disc image import in the launcher, sprint by arm swing (offline) | |

## Open points outside the order

- Multiview on the Steam Frame: why the eyes do not fuse there (suspected
  Mesa zink/Turnip); tools in branch `frame/doubled-images` (86335828),
  not merged.
- Transparent geometry groups: the Xbox's 384 a window overflow in games
  of 30 and more players.
- The VR on-screen keyboard shows a password in the clear while it is
  typed.
- Headset tests of the latest merge (`merge/upstream-2026-10-06`): menus,
  bots, co-op, a password game between Frame and Quest, a bot making room
  for a joining person.
