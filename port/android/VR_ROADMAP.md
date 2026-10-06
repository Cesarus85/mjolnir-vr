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
| 3a | Visible arms and hands: the arms by IK to the controllers, fingers by trigger and grip | tried on the Quest 3 (good, 2026-10-06); Steam Frame to try; branch `feature/vr-arms` |
| 2 | Controls and comfort: left-handed mode, buttons remapped, flashlight by tapping the head, finer turning and vignette choices, graphics presets and refresh rate in the VR options | |
| 3b | The whole body: torso from head and hands, legs and feet following in the room; full / arms only / hands only | |
| 3c | Weapons held physically, holsters at shoulder and hip (the buttons stay) | |
| 3d | Other VR players shown with their heads, hands and bodies (today their bodies move as flat players' do: only their aim and throws come from their hands) | |
| 4 | Later: disc image import in the launcher, sprint by arm swing (offline) | |

## Step 3a in the branch `feature/vr-arms`

Built on `frame/performance-2`, not yet tried in the headset. The arm IK,
the forearm's twist and the fingers' poses follow HaloCE-Quest-VR (CC0),
in `port/linux/game/vr_arms.c`.

- The first-person pose (weapon, arms) is built once a frame instead of
  per eye and scope view (SteamVR: two to three times before).
- `vr.log_performance` reports its cost (`VR:   first person: ... ms a
  frame in ... builds (arms ... ms), ... kept`) and, once, how the hands'
  skin hangs from its bones (none from the root: no triangle filter
  needed).
- Both arms bend from shoulders below the neck, square to the torso. The
  torso follows the head beyond 15 degrees and turns towards the hands;
  snap and smooth turns carry it at once.
- The free left arm bends at the elbow to the controller (before: carried
  rigidly). The hand's roll is shared along forearm and upper arm.
- The free hand's fingers follow the controller: index from the trigger,
  the others from the grip, the thumb from where it rests (touch on the
  Quest's, Index and Steam Frame controllers; half curled where unknown),
  closed round a held grenade. The weapon hand keeps its grip; its index
  finger lifts off the trigger when the controller says it is off.
- host_xr: touch actions for thumb and index; `suggest()` takes 64
  bindings (was 32, silently cut) and retries a profile without the
  touch paths should the runtime refuse them.

Settings (`[vr]` in config.toml; the first two also in the VR options,
Y on the main or pause menu):

| Setting | Values |
| --- | --- |
| `vr.body` | `"arms"` (default), `"hands"` (hands alone, the arms shrunk behind the wrists), `"off"` (the weapon alone) |
| `vr.fingers` | `true` (default): fingers follow the controller; `false`: as the weapon's animation has them |
| `vr.arm_ik` | `true` (default): the free arm bends at the elbow; `false`: rigid as before, should its skin tear |

After the first Quest tries (2026-10-06):
- The shoulders 13 cm behind the eyes and 20 cm below the neck: the open
  end of the sleeve was in view. Looking hard down to the side one still
  sees into the arm (the model ends at the shoulder): left as it is.
- The elbow bends in the arm's plane (`vr_align_elbow`): it had turned in
  with the elbow out to the side; the upper arm takes none of the hand's
  roll.
- The free arm is solved first and its hand turned after (as
  HaloCE-Quest-VR does): turned first, its skin stretched like a rubber
  band.
- The elbow follows the hand's roll (lifted out, the controller rolls in):
  before, it bent only towards a fixed direction.
- Arm length, shoulder width, fingers, grenade grip, body modes, scope,
  vehicles: fine.

Not in it (3a.7): the glove fitted to each weapon's grip (gun anchor) and
the left hand animated through reloads.

## Open points outside the order

- Multiview on the Steam Frame: why the eyes do not fuse there (suspected
  Mesa zink/Turnip); tools in branch `frame/doubled-images` (86335828),
  not merged.
- Transparent geometry groups: the Xbox's 384 a window overflow in games
  of 30 and more players.
- The VR on-screen keyboard shows a password in the clear while it is
  typed.

## Headset tests pending (build `feature/vr-arms`, which holds all three)

Upstream merge (`merge/upstream-2026-10-06`):
- Menus, a bot game, head turning (no doubled image), co-op briefly.
- A password game hosted on the Frame, joined from the Quest.
- A bot making room for a person joining a full lobby.

Performance (`frame/performance-2`, and the merge's upstream speed-ups),
with `vr.log_performance = true`:
- Frame: the log says `trace markers: N descriptors to /dev/null`; frame
  time ("working") against before (about 12.5 ms in a bot game on Blood
  Gulch); SteamVR's reprojected share (vrcompositor.txt) against before
  (2 to 17 %); no doubled image.
- Lens flares still appear and hide as before (their visibility counters'
  copies changed).
- The pose cache (3a.1): `first person: ... builds` about one a frame;
  scope, crosshair, laser and the weapon in the hand as before.
- Quest: 90 Hz held as before in a bot game and in a large online game;
  nothing slower than the last build.

Arms and hands (3a): on the Steam Frame, as on the Quest (fingers on its
controllers, every button still working).
