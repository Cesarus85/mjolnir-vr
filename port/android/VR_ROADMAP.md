# VR roadmap

What Ringfall: Mjolnir VR (Quest 3, Steam Frame) has, and what comes next, in the
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
| Headsets | Quest 3; Steam Frame (Lepton): its controllers' layout, a Steam library tile that installs, starts, updates and quits the game, data kept in `~/Documents/RingfallMjolnirVR` |
| Rendering | The eyes drawn one by one (both at once, multiview, only with `debug.multiview`: its eyes did not always fuse, see the open points); lens flares at their depth in stereo; HUD, menus, scope, crosshair and names as layers of their own |
| Aiming | The weapon in the hand, its shots from the muzzle, laser and crosshair options; each weapon held so that its model's hand lands on the real one, as the free hand's (it learns where each weapon's animations hold the hand; the pistol's had been 15 cm before the real hand, the assault rifle's 8 cm aside), the scope's disc moved with it; `vr.weapon_hand_*` to adjust (2026-10-17) |
| Body | Physical crouch (`vr.crouch_limit`) beside the crouch button, leaning (`vr.lean_limit`), melee by swinging, grenades thrown with the left hand, two-handed weapons |
| Vehicles | The Warthog's wheel turned with the hands, right trigger throttle, left brake; first-person seats; turrets with handles |
| Scores | The menu button held shows the scores on the Quest (a press pauses, as it is let go); the Steam Frame's View button as before; the text chat's lines where the desktop has them (2026-10-18) |
| Comfort | Snap or smooth turning, a comfort vignette in vehicles, VR options in the main and pause menus (Y), a page of every controller button |
| Menus | The PC version's menus in VR (`vr.menus`), clicked with a laser; text typed on the on-screen keyboard |
| Multiplayer | Up to date with OpenCE (network version 29, 2026-10-14): cross-play, its server browser, co-op campaign, password lobbies; join codes and the VR online list |
| Bots | Bots in multiplayer: on foot (paths, items, objectives), in vehicles and at turrets; they make room for a joining person, on the LAN too (a lobby they fill is advertised open, 2026-10-18) |
| Voice chat | Push to talk on any button (`vr.button_talk`, BUTTONS page; the free hand's grip by default, sending nothing while it reaches for the foregrip or the wheel), open mic, off (2026-10-13) |
| Builds | The Android build's assembly conversion (`tools/android_asm_convert.py`) keeps string data as it is: strings and tables starting with "_" or holding \" lost a "_" (Opus's pulse cache among them: the Steam Frame's crashes in online games while talking), 2026-10-13 |
| Languages | The European disc in Spanish and Italian (a maps folder for each, `maps_es` and `maps_it`, none called `maps`) imported on the Quest and the Steam Frame: the headset's language's, its language the game's and the menus' (`maps/language.txt`, unless `game.language` is set), 2026-10-17; not yet tried with that disc |
| Controls | Left-handed mode (`vr.handedness`): the weapon in the left hand, the arms and grips mirrored, sticks and buttons mirrored on both controllers (`vr.mirror_controls`), the assault rifle's counter read the right way round (2026-10-06) |
| Graphics | Presets low / medium / high / max (`vr.graphics`), refresh rate and resolution in the VR options, all kept over a restart (2026-10-06) |
| Arms and hands | Both arms by IK from shoulders that follow the torso, the elbow led by the hand's roll; fingers from trigger, grip and touch; `vr.body` arms / hands / off, `vr.fingers` (2026-10-06) |

## Next, in order

| # | Step | State |
| --- | --- | --- |
| 1 | Steam Frame CPU time: Mesa's trace markers off (after astromaddie/HaloCE-VR), no `glGet` stalls in the visibility counters' copies | **done** (in `main` 2026-10-06): markers quieted, no problems; the frame's work about unchanged (11-12.5 ms of 13.9 in a bot game) |
| 3a | Visible arms and hands: the arms by IK to the controllers, fingers by trigger and grip | **done** (in `main` 2026-10-06; Quest 3 and Steam Frame good; 0.02 ms a frame) |
| 2 | Controls and comfort: left-handed mode, buttons remapped, flashlight by tapping the head, finer turning and vignette choices, graphics presets and refresh rate in the VR options | first part **done** (in `main` 2026-10-06; Quest 3 and Steam Frame good): left-handed mode, graphics presets, refresh rate and resolution in the VR options. Second part **done** (in `main` 2026-10-06; Quest 3 good): buttons remapped, finer turning and vignette choices; the flashlight by tapping the head taken out (did not work) |
| 3b | The whole body: torso from head and hands, legs and feet following in the room; full / arms only / hands only | **done** (in `main` 2026-10-10; Quest 3 good): `vr.body` "full" ("Full body" in the VR options; "arms" still the default), legs walking the way the player moves, feet on the ground. Also in: voice chat's push to talk on the free hand's grip. Steam Frame: its cost still to measure |
| 3c | Weapons held physically, holsters at shoulder and hip (the buttons stay) | |
| 3d | Other VR players shown with their heads, hands and bodies (today their bodies move as flat players' do: only their aim and throws come from their hands) | |
| 4 | Later: disc image import in the launcher; moving by swinging the arms (in place of the stick, never faster than it: online too), and a sprint by swinging them harder (faster than the game allows: offline only) | import **done** (in `main` 2026-10-06): the app's import (tried on the Quest 3: all 24 maps; on the Frame kept in `~/Documents/RingfallMjolnirVR`), a Steam Frame installer from Desktop Mode (`STEAM_FRAME.md`; tried on the Frame: the old folder renamed, the shortcut added by Steam), a release signing key (CI and local builds the same). Arm swinging and its offline sprint still open |
| 5 | Later: multiview again (both eyes in one pass, half the drawing's CPU cost): find why its eyes do not fuse in big online games on the Quest 3 (and now and then on the Steam Frame), then turn it back on | open: off by default since 2026-10-16 (`debug.multiview`); findings in the open points |
| R | Release as **Ringfall: Mjolnir VR** (no "Halo" in the product's name): app name, id `io.github.cesarus85.ringfall`, icon and library pictures (`tools/ringfall_art.py`), Frame folder and installer, release assets, README; the repository renamed | **done** (in `main` 2026-10-06; Quest 3 and Steam Frame good) |

## Step 2, first part (done)

Left-handed mode and graphics presets, after HaloCE-Quest-VR (CC0): its
mirrored first-person model, its graphics switches.

Left-handed (`vr.handedness = "left"`):
- The weapon in the left hand: aim, shots, muzzle, laser, crosshair,
  scope disc, turret aim. The first-person model is posed as the right
  hand's in a mirror image of the room ("model space", `halo_vr.h`) and
  mirrored back; a model whose node matrices mirror has its triangles'
  winding turned over (`rasterizer_set_model_skinning`, `d3d8_gl.c`). The
  arms reach to the other shoulders, the foregrip is gripped with the
  right hand, the right hand's fingers follow its controller.
- Grenades held and thrown with the right trigger and hand; melee by the
  left grip (a swing of either hand, as before); the motion tracker on the
  right wrist (`halo_xr_submit.wrist_hand`); the rumble's light motor in
  the weapon hand.
- The left trigger fires, the right one throws; at the Warthog's wheel the
  left trigger accelerates, the right one brakes.
- With `vr.mirror_controls` (default): the right stick moves, the left
  turns and steers; their clicks, the bumpers and the face buttons change
  sides (Quest: X jumps, Y light / held grenades, A action and reload, B
  switch weapon; Steam Frame: the left D-pad does A, B, X, Y as their
  mirror image, down jump, left light, right action, up switch weapon,
  and A, B, X, Y do the D-pad's). The menus keep their buttons; the
  laser pointer works from either hand as before.
- Other machines see the shots leave the left hand (the network message
  is unchanged: aim and muzzle from the camera).
- The assault rifle's ammo counter and compass read the right way round
  on the mirrored model (2026-10-06): their geometry stays mirrored, each
  digit's bitmap is drawn turned over in u and the digits' order across
  the display swapped (numeric shaders drawn mirrored,
  `rasterizer_xbox_transparent_geometry.c`). Checked flat in the emulator
  with `debug.mirror_first_person` (the first-person weapon mirrored off
  the headset): 60, 48, 36 ... 06 read correctly. Steam Frame good
  (2026-10-06).
- Kept as they are (accepted 2026-10-06): the small static icons under the
  counter (round and "x") stay mirror images (a texture atlas, not
  numeric). The game's prompts still name the Xbox's buttons.
- Settings kept: on Android `config.toml` is written beside itself and
  renamed over (`port_config.c`), so one pushed by adb (the shell's, which
  the app could not open for writing) no longer loses the VR options'
  changes at a restart.
- Headset tests 2026-10-06: Quest 3 and Steam Frame good (left hand,
  controls mirrored and not, counter, presets, refresh rates, resolution,
  settings kept over a restart).

Graphics (`vr.graphics`, `port/linux/game/vr_graphics.c`), set each frame
on the game's own switches:

| Preset | Leaves out |
| --- | --- |
| `"high"` (default) | nothing: as before |
| `"medium"` | object shadows, environment reflections, lens flares |
| `"low"` | also specular light, bump maps, grass and other detail objects, the screen's fog layer, contrails, weather, camouflage's second pass |
| `"max"` | nothing; models lit for each pixel, shadows at 512x512 |

A client in another's game keeps grass, lens flares and the fog layer as
the host's rules have them (`cheats.c`). Shadows, specular passes,
reflections and detail objects are draws (CPU, which the Frame's GL
driver is short of); bump maps, fog, camouflage and per-pixel lighting
the GPU's fill.

VR options (Y): two pages, PLAY (the comfort rows, "Weapon hand",
"Mirrored controls") and MORE ("Graphics", "Refresh rate", "Resolution",
the controller buttons page, Quit); four since the second part (below). The refresh rate steps through what
the runtime offers and changes at once; on the Steam Frame the row shows
SteamOS's rate. The resolution (`vr.resolution_scale`, 70-120%) takes a
restart. The controller buttons page shows the layout as set.

| Setting | Values |
| --- | --- |
| `vr.handedness` | `"right"` (default), `"left"` |
| `vr.mirror_controls` | `true` (default): sticks and buttons mirrored when left-handed; `false`: as right-handed (triggers and grips still follow the hands) |
| `vr.graphics` | `"auto"` (default, branch `fix/vr-frame-hitches`), `"low"`, `"medium"`, `"high"`, `"max"` |
| `vr.foveation` | `"off"` (default), `"low"`, `"medium"` (the Quest only; branch `fix/vr-frame-hitches`) |
| `vr.refresh_rate` | Hz, as offered (Quest 3: 72, 80, 90, 120) |
| `vr.resolution_scale` | 0.7 to 1.2, on restart |

To try in the headset (Quest 3 and Steam Frame):
1. Left-handed: aim, laser, crosshair, scope of sniper and rocket
   launcher; weapon held right in the left hand, arms from the right
   shoulders, nothing inside out (faces culled the wrong way would show
   the inside of the gun or the glove).
2. Two-handed grip with the right hand; grenades in the right hand (held
   and thrown); melee by grip and by swing; motion tracker on the right
   wrist.
3. Buttons and sticks mirrored on the Touch and the Frame controllers
   (every function reachable), then `Mirrored controls: Off`.
4. Vehicles: wheel, throttle and brake, steering stick, a gunner's aim;
   menus and the laser pointer; a network game (shots from the left hand
   on the other machine).
5. Graphics: each preset's look and smoothness (`vr.log_performance`),
   in a bot game; Max on the Quest at 90 Hz; refresh rates 72/80/90/120
   on the Quest; the Frame's row.

## Step 2, second part (done)

In `main` 2026-10-06; tried on the Quest 3 (with the rename): good. The flashlight by tapping the head (after HaloCE-Quest-VR)
did not work in the headset and was taken out again; the button stays.
The VR
options (Y) have four pages: PLAY ("Weapon hand", "Mirrored controls",
laser, crosshair, motion tracker, body, fingers, the Warthog's wheel),
COMFORT (turning, the vignettes), BUTTONS (the actions' buttons,
"Reset buttons to defaults", the controller buttons page) and MORE (graphics, refresh rate, resolution, Quit).

Buttons remapped (BUTTONS page; `vr.c` `controls_remapped`):
- Jump, the flashlight (held: switch grenades), action/reload, switch
  weapon, crouch, zoom, melee and push to talk each go on one of eight
  buttons: A, B, X, Y, the move stick's click, the turning stick's, the
  weapon hand's grip, the free hand's grip. Moving an action to a button
  swaps it with the action there, so none is ever left without one.
- The free hand's grip as a button (`free_grip_down`): pressed once held
  0.15 s at the foregrip's threshold, and not at all while it holds the
  weapon's foregrip or the Warthog's wheel (taken in that time, the press
  is theirs until the grip opens).
- The triggers, the menu button, the Steam Frame's
  bumpers, D-pad and View button keep theirs; the menus keep the buttons
  as they are.
- Applied after the left-handed mirroring, on the gamepad's sides: the
  same on the Touch and the Frame controllers, and a left-handed player's
  buttons are the mirror image of the right-handed ones (on the Frame,
  mirrored, the left D-pad's). The defaults are the old layout, the
  host's bindings and the ABI unchanged.
- The melee's aim and jab follow its button; the grips on a Warthog's
  wheel press nothing, whatever the grip does.
- The controller buttons page shows each action where its button is.

Comfort (COMFORT page): snap turns 10 to 45 degrees by 5, 60, 75, 90;
smooth turning 30 to 150 degrees a second by 15, then to 300 by 30; the
vehicle vignette Off, 15 to 100%; the on-foot vignette (off by default)
while the stick turns smoothly, or also while the move stick moves, as
far as the sticks are pushed, with its own strength. The old values are
among the steps; defaults unchanged.

Transparent geometry: room for 2048 groups a window (the Xbox's 384,
below).

| Setting | Values |
| --- | --- |
| `vr.button_jump` | `"a"` (default), `"b"`, `"x"`, `"y"`, `"move_stick"`, `"turn_stick"`, `"grip"`: as right-handed, mirrored with `vr.mirror_controls` |
| `vr.button_light` | the flashlight, held switch grenades; `"b"` (default) |
| `vr.button_action` | action, reload; `"x"` (default) |
| `vr.button_weapon` | switch weapon; `"y"` (default) |
| `vr.button_crouch` | `"move_stick"` (default) |
| `vr.button_zoom` | `"turn_stick"` (default) |
| `vr.button_melee` | `"grip"` (default) |
| `vr.foot_vignette` | `"off"` (default), `"turning"` (smooth turns), `"moving"` (and moving) |
| `vr.foot_vignette_strength` | 0.15 to 1, `0.7` (default) |
| `debug.vr_options` | `false` (default); `true` shows the VR options off the headset (the emulator) |

Checked in the emulator (`debug.vr_options`): the four pages without
overlaps, each row's values, swaps and reset, the controller buttons page
right- and left-handed after swaps, the settings kept over a restart, a
15-bot game as before.

To try in the headset (Quest 3 and Steam Frame):
1. Defaults: every button as before (jump, light/held grenades, action,
   switch weapon, crouch, zoom, melee by grip and by swing), right- and
   left-handed, mirrored and not; the menus' A and B.
2. BUTTONS: swap jump and melee (jump on the grip, melee on A): both work,
   the melee jabs and aims where the head looks; light on a stick click
   (tap: light, hold: grenades); the controller buttons page says so.
   At the Warthog's wheel with jump on the grip: no jump while holding
   the wheel. Reset: all back.
3. The same left-handed with mirrored controls (on the Frame the left
   D-pad), and with `Mirrored controls: Off`.
4. COMFORT: snap 10 and 90, smooth 30 and 300; vehicle vignette 15% and
   100%; on-foot vignette "Smooth turns" (nothing with snap turns),
   "Moving, turning", strength 15% and 100%; no vignette in a vehicle
   from the on-foot one, none in the menus.
5. A game of 30 and more players: effects, glass and water stay; the log
   (`adb logcat -s halo`) says `rasterizer: N transparent geometry groups
   in a window` once a window passes 384; frame time as before.

## Step 3a (done)

Tried on the Quest 3 and the Steam Frame, in `main`. The arm IK,
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

## Step 3b (done)

On `feature/vr-voice-and-body`, not yet tried in the headset. The neck
pivot, the planted feet with their steps and the leg's clamped reach follow
HaloCE-Quest-VR (CC0), its full body, rebuilt in
`port/linux/game/vr_body.c` on this port's skeleton (`vr_arms.c`).

- `vr.body = "full"` (VR options, Body: "Full body") draws the player's own
  biped under the first-person arms and weapon: its torso, hips and legs, in
  the map's own model and colours. "arms" stays the default until the
  headsets have tried it.
- Drawn among the world's objects, so it sorts against the world by depth;
  the first-person weapon and arms, drawn before the world into the
  stencil, stay over it, as before. In both eyes (multiview on the Quest,
  each eye on SteamVR), posed once a frame for every eye and the scope.
- The torso: the torso's heading (the head's beyond 15 degrees, drawn
  towards the hands, the stick's turns at once), upright from the hips to a
  neck 14 cm behind and 20 cm below the eyes as the head turns about it
  (looking down, the eyes go forward and the chest stays behind them). The
  aiming's twist and bend of the third-person torso are taken out.
- The hips: down with the physical crouch (and back a little, the torso
  leaning forward), a third of the way with a lean (the spine tilts the
  rest). The body's model has its neck higher than the eyes' pivot: it is
  moved down to it, so the shoulders stay below the eyes, crouched by the
  button too.
- The legs: the game's own animation (walking, running, jumping, crouched
  by the button). Moving, the hips and legs stay as the game faces them, so
  its own choice of animation (forward, back, aside, as for any player,
  by the way it moves against the way it faces) runs along the way the
  player moves, and the spine turns the torso; standing, the hips turn to
  the torso's heading (eased, 0.12 s). Walking off, the feet ease from
  where they stood into the walk (0.2 s); stopping, they stay where the
  walk left them and step into place. Standing still, the feet stay
  planted and step, one at a time (0.22 s, 6 cm lifted), when the body has
  turned 35 degrees from them or their place moved 18 cm; the knees bend by
  IK to them as the hips go down. Moved 80 cm at once (a teleporter), they
  are put down there.
- The ground: put down, each foot rests on the ground under it (a slope, a
  stair), its height above the ground as the animation has it, tilted with
  the slope (no steeper than 45 degrees), up to 45 cm from the ground under
  the body; a foot's ground below it lowers the hips as far. Rays straight
  down, read only, cast only as the feet are put down (three: under the
  body and each foot) and as one steps (one), none while walking, where
  the animation's feet are kept; eased (0.08 s) in and out. Nothing found
  (a ledge): level.
- Hidden: the head and neck (shrunk onto the neck's root: no head or
  collar seen from inside), the third-person arms (shrunk onto their
  shoulders), the third-person weapon and all the biped carries.
- The first-person arms hang from the body's shoulders: beside its neck,
  as wide as its shoulder joints within 14 to 24 cm (the aiming's shrug
  left out). Left-handed, the shoulders go into the mirror with the arms
  (the room's left one is the model's right): the body itself is the
  room's, not mirrored.
- Seats: a passenger in an open vehicle (the Warthog's back) sits as the
  seat's animation has it, the head and arms shrunk alike; at a wheel or a
  gun the body is drawn whole as before (its hands on them, no
  first-person weapon); in a closed seat (Ghost, Banshee, tank) none.
- Not drawn when dead, in cutscenes, in third person, in a mirror's view;
  camouflage as the game has it.
- Only drawn, on this machine: the game's own pose, hit boxes, markers and
  network messages are unchanged.
- Cost: `vr.log_performance` adds `body ... ms a frame in ... poses` to the
  `first person:` line. In the emulator 0.006 to 0.025 ms a pose standing
  (as busy as the emulator was), up to 0.1 ms averaged while turning with
  the feet's rays (at most a few a second); the drawing is one more biped
  model (as one more player close by).

| Setting | Values |
| --- | --- |
| `vr.body` | `"full"` (the whole body), `"arms"` (default), `"hands"`, `"off"` |
| `debug.vr_body` | `"off"` (default); off the headset (the emulator) the body posed from the flat camera: `"eye"`, `"down"` (looking down 60 degrees, 90 degrees wide), `"front"`, `"side"`, `"back"` (the camera 2.6 m away); after a colon the head's motion: `"cycle"` (turning, crouching, leaning, 5 s each), `"still"`, `"turn"`, `"crouch"`, `"lean"`, `"raw"` (the game's pose, only shrunk). Logs its pose once a second |

Checked in the emulator (`debug.vr_body`): the cyborg's 19 nodes (bip01
pelvis, spine, spine1, neck, head, clavicles, arms, thighs, calves, feet);
the body standing, crouched physically and by the button, leaning,
turning with the feet stepping; looking down, the shoulders below the eyes
and no head or collar; two-minute bot games on Blood Gulch with it on and
off, deaths and respawns, nothing in the log. Then the legs by the way of
moving (strafing, backwards, diagonally with the touch stick: the game's
sideways and backwards walk) and the feet on the ground: on the ramp of a
Blood Gulch base, the upper foot 15 cm up, the lower 10 cm down, both on
it; stairs not reached in the emulator (Prisoner's spawn was level). Three
rays when the feet are put down, one a step, none walking. Screenshots in
`build/body-shots/` (local).

Push to talk is built on the same branch: voice chat's talking on the free
hand's grip, while it holds neither the weapon's foregrip nor the Warthog's
wheel (`audio.voice_chat`, VR options PLAY page "Voice chat": push to talk
(grip), open mic, off).

To try in the headset (Quest 3 and Steam Frame), with `vr.body = "full"`
(VR options, Body: Full body) and `vr.log_performance = true`:
1. Standing, looking down: torso, hips, legs and feet, the shoulders below
   the eyes, no head, collar or helmet in view, the chest not in the way;
   the arms from the shoulders, lined up with the body, the weapon as
   before.
2. Turning the head alone: the body stays within about 15 degrees, then
   follows; turning further, the feet step after it, one at a time, and
   settle. Snap and smooth turns: the body turns, the feet step.
3. Physical crouch: the knees bend, the feet stay; kneeling deep, nothing
   of the body in the eyes. Leaning forward and aside: the torso tilts, the
   feet stay. Crouch button: the game's crouch, shoulders below the eyes.
4. Walking, running, jumping by the stick: the legs walk with the game's
   animation, no sliding when starting or stopping. Strafing left and
   right, walking backwards and diagonally, also with the head turned away
   from the way of walking: the legs step the way the body moves (sideways,
   backwards), the torso facing as the head and hands have it; stopping,
   the hips turn back under the torso and the feet step into place.
4a. Ground: standing still on a slope (Blood Gulch's hills and the bases'
   ramps), across it and along it: both feet on the ground, tilted with
   it, the lower knee straighter, the hips down with the lower foot; on
   stairs (Prisoner, Chill Out, Damnation), one foot a step higher than
   the other, turning in place there: each step lands on its stair. At a
   ledge, one foot over it: level, nothing odd.
5. Left-handed (`vr.handedness = left`, mirrored or not): the arms from
   the body's shoulders, the weapon in the left hand, the body as
   right-handed.
6. Vehicles: Warthog driver and gunner as before (body with its hands on
   the wheel or gun); Warthog passenger: seated legs and torso, the
   first-person arms from its shoulders; Ghost, Banshee, Scorpion: no body.
7. Dying and respawning, a teleporter, a cutscene (campaign), the menus
   and pause menu, the scope, camouflage, a game with 15 bots: nothing
   broken, no body where it should not be.
8. Body "Arms and hands", "Hands only", "Weapon only": as before.
9. Performance: `first person: ... body ... ms` in `adb logcat -s halo`;
   the Quest holds 90 Hz in a bot game; the Steam Frame's working time
   against "arms" (about 12 ms of 13.9 before).
10. Push to talk (Voice chat: push to talk), in a network game with
    another player: holding the free grip away from the weapon talks;
    nothing while the free hand holds the foregrip or the Warthog's wheel;
    "Open mic" talks without the grip; "Off" sends nothing (others still
    heard).

## Frame hitches and GPU time (built: branch `fix/vr-frame-hitches`)

Measured on the Quest 3 (2026-10-12; 90 Hz, multiview, graphics "high",
full body, a 15-bot game on Blood Gulch): the game's CPU work 5-6 ms a
frame; the app's GPU time 5.5-6.4 ms (`xr: metrics`), the same with
foveation off, low, medium and high: **the GPU is not the bottleneck**.
The doubled pictures come from single long frames: shader programs compiled
in a frame (29 new ones in a 794 ms frame 22 s into a match, single ones
18-64 ms), draws of 254 ms (the first frame in a map) and 74 ms with
nothing compiled (most likely the driver making a program's variants at
its first draw), and the menus' PNG pictures (7 in 108 ms).

What the branch has:

| Part | What |
| --- | --- |
| Long frames | `VR: long frame N ms: input, ticks, camera, draw, present; shaders compiled or linked, programs from the cache; textures uploaded, menu pictures, render targets; draw calls N ms, the slowest N ms (vertex program, program key); N draws left out waiting for their programs`. The 5 s report counts the draws left out |
| GPU time | XR_META_performance_metrics on with `vr.log_performance`: `xr: metrics (mean/most) app/gpu_frametime=... compositor/dropped_frame_count=...` |
| Foveation | `vr.foveation` "off" (default) / "low" / "medium" on the MORE page (GL_QCOM_texture_foveated, the eyes only, lens centres); "high" taken out (it drew a grey rectangle below the player). Kept for later, no gain measured |
| Shader cache | `display.shader_cache`: programs kept in `shader_cache/` (binary, driver, sources); each map's list (`maps/<map>.txt`) and every map's (`all.txt`) |
| Worker | `display.shader_worker` (default true): programs made on a thread with a GL context sharing the game's (`gl worker: running`), from the cache's binary or compiled; a draw whose program is not made yet is left out for those frames, the frame never waits. As a map loads, its programs and then every map's are asked for |
| Warm-up | `debug.shader_warm_up`: "worker" (default: each program drawn with once on the worker into a small target of the eyes' kinds), "load" (the map's programs drawn with on the game's thread as it loads), "off" |
| Menus | The PC menus' pictures decoded on a thread of their own, drawn transparent for the frames that takes |

Second Quest test (latest build): the worker works, no long frame after the
map's first frames (245 ms and 72 ms there, each one draw of 67-75 ms with
vertex program 4). But doubled pictures in spells of seconds with the GPU
at 9-13 ms a frame (GPU% 0.91-0.94, 4-37 stale frames a second), 5-7 ms in
between: "high" is too much for 90 Hz in heavy scenes, "medium" holds.
Hence:

| Part | What |
| --- | --- |
| Graphics "auto" | `vr.graphics = "auto"`, the new default (Graphics row: Auto): "high" while the GPU keeps up. Past 4/5 of the frame (8.9 ms at 90 Hz) for a quarter second it leaves out the next steps (shield glow, shadows, reflections, specular light, lens flares, bump maps, the rest of "low"), as many as their learnt costs say; brings one back once GPU time + its cost stays under 7/10 of the frame for 3 s (longer if it had to go again within 10 s). Logs `VR graphics auto: shadows left out (step 2): the GPU 10.2 ms a frame of 11.1`, `... saved 1.9 ms`, `... back`. On the Steam Frame (no GPU time from SteamVR) "auto" is "high" |
| Measuring | `debug.vr_graphics_measure = true`: each effect left out in turn (1.5 s on, 1.5 s off), `VR graphics measure: shadows 2.10 ms (...)` and means each round |

What the effects cost, from the code (to be measured with the option above):
object shadows are drawn once a frame with multiview (the scene is drawn
once), twice one eye at a time; each shadow-casting object (every bot, its
weapon, vehicles) renders its map, blurs it and projects it onto the level
beneath: a pass of fill on both eyes per object. Reflections and specular
light are passes over the level's geometry. The shield's glow
(`display.shield_glow`, off by default) is a dynamic light per flaring unit
(at most 8), each lighting the level's surfaces in its reach once more, not
a pass per object; upstream's shield colour on the arms only changes a
colour. The whole body adds the player's own model and its shadow.

Not done, and why:
- Drawing the eyes straight into the swapchain: moot now (GPU not the
  bottleneck); it needs GL_EXT_sRGB_write_control, the vignette as a pass
  of its own and array swapchains acquired mid-frame.
- Decoding all menu pictures ahead: 165 pictures, about 350 MB decoded with
  their mipmaps.
- Later: XR_KHR_visibility_mask.

### Headset test, graphics "auto" (Quest 3)

1. VR options, MORE: Graphics **Auto** (a `config.toml` written before
   still says "high"), Foveation Off; `vr.log_performance = true`.
2. Log at start: `xr: performance metrics on (... the app's GPU time read)`,
   `VR: graphics auto`.
3. 15-bot match on Blood Gulch, a few minutes, also into the busy middle:
   - `VR graphics auto: ... left out (step N): the GPU X ms` within a
     quarter to half a second of a heavy spell, `... saved Y ms`, and
     `... back` a few seconds after it.
   - VrApi: `Stale=` 0 nearly every second (at most a few in the second a
     spell begins), no `ASW=1` spells; App GPU under about 9 ms.
   - Look for effects that pop: shadows going and coming back. If they
     change more than every ten seconds or so, note the log lines.
4. Costs: a second match with `debug.vr_graphics_measure = true` (Graphics
   setting ignored meanwhile; stand somewhere busy, a few minutes):
   `VR graphics measure: means (ms of GPU a frame): shadows ...;
   reflections ...`. The steps' order should follow the largest costs.
5. The first frames of a map (245/72 ms, vertex program 4): try
   `debug.shader_warm_up = "load"` once and compare those lines.

### Headset test, shader worker (Quest 3)

Before: `adb logcat -G 16M` (again after any reboot); in `config.toml`
`vr.log_performance = true`, 90 Hz, graphics "high", multiview, full body;
Foveation Off (VR options, MORE: the config may still say "medium"). After
each test: `adb logcat -d -s halo VrApi > test.txt`. The shader cache from
the earlier tests stays.

1. Start: `gl worker: running (ES 3, surfaceless, programs warmed up)` and
   `shader cache: ... N programs kept (... N listed) ...; programs made on
   the worker, warmed up there`. As the menus load: `shader cache:
   levels_ui_ui: N programs listed; N asked of the worker ...; N of other
   maps to ask for`.
2. Bot match on Blood Gulch: as it loads, `... bloodgulch: N listed; ...
   N there already` (made while in the menus). In play no `VR: long frame`
   with shaders compiled; at most a few `draws left out` (objects appearing
   a frame or two late), and VrApi `Stale=0` most seconds, no `ASW=1`.
   Any long frame left: its `draw calls ... the slowest N ms (vertex program
   N, program K)` says whether a draw (a driver variant) is the cause.
3. A map not played yet (Damnation): as it loads, `N programs listed` is 0
   but `N there already` should not matter: its programs mostly come from
   every map's list (made in the menus). New ones are compiled on the worker
   (`draws left out` for a moment, no long frame).
4. If draws of 70-250 ms remain with nothing compiled: `debug.shader_warm_up
   = "load"` (the log: `shader cache: N programs drawn with once as the map
   loaded, in N ms`), then compare; and `"off"` for the baseline.
5. Menus: open new screens; pictures may appear a frame late, no long
   frames with `menu pictures`.
6. If anything breaks (missing objects for good, a crash):
   `display.shader_worker = false` brings the game's-thread compiling back.

Steam Frame: the worker also runs there if its EGL shares contexts (the log
says `gl worker: running` or why not); no foveation. A short bot game to
see that nothing is missing or slower.

## Open points outside the order

- Multiview's doubled pictures (2026-10-16): on the developer's Quest 3,
  doubled and shifted pictures from the start of big online games (26-35
  players) as the head turned, nauseating; none in a local game with 48
  bots; none online either with the eyes drawn one by one. Not the frame
  rate: one by one, online at 72 Hz, the compositor repeated as many
  frames (about 4 a second) and nothing was seen. Not the facing jumps
  (respawns only) nor the camera (it moves every frame). Before: the
  Steam Frame's (2026-10-05), each eye alone sharp. So the eyes are drawn
  one by one by default now (`debug.multiview` for both at once). To do:
  find what the shared pass draws wrongly online, e.g. by saving both
  eyes' pictures in a big online game and comparing what is doubled
  (sprites and effects projected by the game itself, `mv_depth`; programs
  made on the shader worker; the sky rows), then turn it back on: it
  halves the drawing's CPU cost.
- Big online games (2026-10-16): doubled pictures on a 32-player server,
  none in a local game with 48 bots. Not the GPU (60–80 %): the ticks cost
  3.3–4.6 ms as a client there (1.3–1.9 ms with bots or 16 players), so
  47–70 of 150 frames with a tick missed 90 Hz's 11.1 ms (the doubling
  itself was multiview's, above). In `main` 2026-10-16 (Quest 3 good):
  `vr.refresh_rate_auto`
  (VR options MORE, "Lower it when frames are late", on by default) steps
  the rate down a step (90, 80, 72 Hz) while more than one frame in 32
  comes late, and back up once they would keep up; "VR: refresh rate" in
  the log. First try: 90 to 80 Hz a few seconds into a 30-player game, no
  doubled picture seen; 2-5 stale frames a second left at 80, hence one
  in 32 (was 16). Later:
  why a client's ticks cost that much per remote player.

- Doubled pictures when turning on the developer's Quest 3 (2026-10-12):
  with every build tried, back to 2026-10-06's, at 90 and at 72 Hz, even
  in seconds the compositor reported no stale frame. That Quest runs
  Horizon OS from the Public Test Channel (PTC; its build 2026-10-08),
  the likeliest cause. Released with `vr.graphics` "medium" as the
  default (smooth there). To do: try this and later builds with "high" on
  a Quest 3 outside PTC (the developer's wife's), and set the default
  back to "high" or "auto" if it holds.
  Resolved 2026-10-16: multiview (above), not the Horizon OS PTC.

- Multiview on the Steam Frame: why the eyes do not fuse there (suspected
  Mesa zink/Turnip); tools in branch `frame/doubled-images` (86335828),
  not merged.
- Transparent geometry groups: the Xbox's 384 a window overflowed in
  games of 30 and more players (the last submitted, glass, water,
  particles, contrails, vanished). Raised to 2048 (320 KB), in `main`;
  still to check in a game of 30 and more players; upstream raised them
  to 2048 too (2026-10-12).

## Headset tests (2026-10-06, all three in `main`)

Quest 3, 2026-10-06: menus, laser, VR options, head turning, arms and
hands, fingers, body modes, scope, vehicles, a bot game (bots spread and
leave their bases) and a large online game: all good.
Steam Frame, 2026-10-06: buttons, picture (no doubled image), arms, hands,
fingers, wheel: all good; SteamVR reprojected 18% of the frames, unseen.
What follows is left for the two headsets together.

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
