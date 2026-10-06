/*
HALO_VR.H

VR for the native ports (port/linux/src/vr.c; implemented on Android, where
the host runs an OpenXR session, port/android/host/host_xr.c). The game's
sources call these under #ifdef HALO_ANDROID; the types are plain C so that
both the game (MSVC-era C) and the platform layer can include this.

Coordinates here are the game's: world units (10 feet, 3.048 metres), +z up,
yaw counter-clockwise from +x.

A frame in VR:
- halo_vr_frame_begin (the platform layer, before input is read) waits for
  the headset's next frame and takes its predicted poses;
- the player's facing follows the head (halo_vr_aim, player_control.c);
- render.c draws the player's view once per eye (halo_vr_eye_camera), each
  copied to its eye image (halo_vr_capture_eye), then clears the back buffer
  (halo_vr_begin_overlay) and draws the HUD, menus and overlays over nothing;
- Present (d3d8_gl.c) copies the back buffer to the overlay layer and
  submits the frame (halo_vr_present): the overlay follows the head over the
  eyes' images, or, when no eyes were drawn (menus, loading, cinematics),
  becomes a screen fixed in the room.
*/

#ifndef __HALO_VR_H
#define __HALO_VR_H

#ifdef __cplusplus
extern "C" {
#endif

/* the game runs in VR (a headset was found and its session created) */
int halo_vr_active(void);

/* this frame can draw the eyes: the session is shown and the views known */
int halo_vr_stereo_frame(void);

/* whether the given local player's view is drawn once per eye this frame:
cinematics and the like are shown on the flat screen instead */
int halo_vr_stereo_player(short local_player_index, int cinematic);

/* the camera of an eye, from the game's camera position: the eye's position
and orientation, the vertical field of view (radians) and the frustum
bounds (x0, x1, y0, y1; render_camera_build_frustum) for the game's
640x480 viewport */
void halo_vr_eye_camera(int eye, const float game_position[3],
	float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4]);

/* copies the back buffer, just drawn for an eye, to that eye's image */
void halo_vr_capture_eye(int eye);

/* Both eyes in one draw (vr.multiview, where GL_OVR_multiview2 is): whether
this frame draws so; the camera between the eyes whose view takes in both
(as halo_vr_eye_camera), which also starts drawing into both eyes' layers;
and after the draw, the layers taken for the eyes' images. */
int halo_vr_multiview(void);
void halo_vr_center_camera(const float game_position[3], float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4]);
void halo_vr_capture_multiview(void);
/* the sky, drawn about the camera, is being drawn: infinitely far */
void halo_vr_multiview_sky(int sky);
/* the shared camera's projection's [2][2] and [3][2], for what the game
projects itself (sprites, lens flare tests) */
void halo_vr_multiview_projection(float projection_22, float projection_32);
/* lens flares are being drawn: placed by their depth, though not depth
tested */
void halo_vr_multiview_lens_flares(int drawing);

/* clears the back buffer to transparent black for the overlay, after the
eyes; menus_active chooses where the overlay is shown */
void halo_vr_begin_overlay(int menus_active);

/* the player's facing from the headset. Given the game's current desired
facing, returns nonzero with the facing to use instead. */
int halo_vr_aim(short local_player_index, float current_yaw, float current_pitch,
	float *yaw, float *pitch);

/* where the head looks, in the game's axes; 0 when not in VR play */
int halo_vr_look_vector(short local_player_index, float vector[3]);

/* vr.log_performance: logs the unit's looking vector against the head's
every second (players.c) */
void halo_vr_debug_look(short local_player_index, const float unit_looking[3], int in_vehicle);

/* how far the view has turned since this was last asked, once past a few
degrees on an axis (the other axis 0); nonzero if it has. The game counts
it as looking around (player_control.c's action tests). */
int halo_vr_look_delta(float *yaw, float *pitch);

/* The weapon in the right hand (vr.aim_with = "controller"; each returns 0,
and the game does as before, when the hand is not tracked):
- the direction the player's weapon aims, where the controller points (the
  body and the view keep facing where the head looks);
- where its shots start, as an offset from the unit's camera (world units);
- the camera the first-person weapon model is posed from, so that it lies
  in the hand;
- the aiming laser's start and direction; */
int halo_vr_aim_vector(short local_player_index, float vector[3]);
int halo_vr_weapon_origin_offset(short local_player_index, float offset[3]);
/* - and the same as a point in the world, where the crosshair's line starts
  (as the drawn frame had it): shots start there too, not from an offset to
  the unit's own eye point, which need not be the drawn camera's */
int halo_vr_weapon_origin(short local_player_index, float origin[3]);
int halo_vr_weapon_camera(short local_player_index, float position[3], float forward[3], float up[3]);
int halo_vr_laser(short local_player_index, float start[3], float direction[3]);
/* the laser for this frame, along the barrel (first_person_weapons.c: while
an animation turns the weapon, as in a reload), and the laser at the ready,
where the controller aims */
void halo_vr_set_laser(short local_player_index, const float start[3], const float direction[3]);
int halo_vr_ready_laser(short local_player_index, float start[3], float direction[3]);
/* The arms (interface/first_person_weapons.c):
- the body: the eyes' position (world units), the neck they turn about,
  and the torso's flat heading (which follows the head beyond a few
  degrees, and the hands; the stick's turns turn it at once), from which
  the shoulders hang;
- the left hand's pose;
- the left hand on the weapon: given where the model's left hand holds it
  (world units) and the line from its right hand to its left in the
  weapon's forward, right and up, whether the left hand grips it; then the
  weapon aims along both hands; */
int halo_vr_body_frame(short local_player_index, float head[3], float neck[3], float forward[3]);
int halo_vr_left_hand(short local_player_index, float position[3], float forward[3], float up[3]);
int halo_vr_two_hand(short local_player_index, const float foregrip[3], const float reference[3]);
/* - the fingers of a hand (0 the left, 1 the right), from the controller's trigger,
  grip and touch: the thumb, index, middle finger and the last two, each
  curled 0 (straight) to 1; 0 when the hand is not tracked or vr.fingers
  is off, else HALO_VR_FINGERS_CURLED, and HALO_VR_FINGERS_TOUCH too when
  the controller tells where the fingers rest (else a finger off the
  trigger reads as one on it, unpressed); */
enum
{
	HALO_VR_FINGERS_CURLED = 1,
	HALO_VR_FINGERS_TOUCH = 2,
};
int halo_vr_finger_curls(short local_player_index, int hand, float curls[4]);
/* - whether the free hand's arm bends at the elbow to reach it from the
  shoulder (vr.arm_ik); else it keeps the shape it has on the foregrip,
  pointed at the shoulder;
- the time of the headset's frame (seconds, steady through the frame), by
  which the arms' bending is eased */
int halo_vr_free_arm_bends(void);
double halo_vr_pose_time(void);
/* a hand has just been thrust forward: a melee attack (xinput_sdl.c) */
int halo_vr_melee(void);
/* the weight of the melee attack under way, from the swing's speed (0..1;
0 with none): the game scales melee damage by the player's run instead,
and in VR a player punches standing (units.c) */
float halo_vr_melee_strength(void);
/* The grenade (units.c): whether the left trigger holds it in the hand; and
as it let go, whether the hand threw it: from where (world units), which
way (the game's axes) and how hard (0..1 of the game's throw) */
int halo_vr_grenade_held(short local_player_index);
int halo_vr_grenade_throw(short local_player_index, float origin[3], float direction[3], float *strength);
/* where the held grenade is drawn (render_objects.c): in the left palm, and
at what size */
int halo_vr_grenade_in_hand(short local_player_index, float position[3], float forward[3], float up[3],
	float *scale);
/* The scope (render.c): zoomed in by magnification, the camera for the
view on a disc over the weapon, in the game's axes; then that view, drawn
into the given part of the back buffer (left, top, right, bottom, 0..1),
is taken for the disc. Between the two, the first-person weapon is left
out (halo_vr_scope_pass). */
int halo_vr_scope(short local_player_index, float magnification, float position[3], float forward[3], float up[3],
	float *vertical_field_of_view);
int halo_vr_scope_pass(void);
/* The menus' pointer (d3d8_gl.c's halo_ui_pointer_update): where a
controller points on the menus' screen, in their 640x480, whether that
moved since asked, and the trigger's clicks since (nonzero while it points
at the screen); and where it points this frame, to draw it (render.c) */
int halo_vr_menu_pointer(int *x, int *y, int *click_x, int *click_y, int *moved, int *clicks);
int halo_vr_menu_pointer_at(int *x, int *y);
/* Players' names over their heads (hud.c, render.c): whether they are drawn
apart this frame; the slot a name is drawn into (left, top, right, bottom
of the game's 640x480); a name's place (world units, over the head),
giving its slot (-1: no more); and the slots, drawn on the cleared back
buffer, taken (it is cleared again) */
int halo_vr_names_apart(void);
void halo_vr_name_slot(int slot, short bounds[4]);
int halo_vr_add_name(const float point[3]);
void halo_vr_capture_names(void);
/* how much higher than a rifle's the weapon in hand has its scope's disc
(metres; render.c, before halo_vr_scope) */
void halo_vr_set_scope_raise(float metres);
void halo_vr_capture_scope(float left, float top, float right, float bottom);
/* where the weapon's gun node is as drawn, at the ready (world units;
first_person_weapons.c, each frame): shots, the crosshair and the laser
start there; and the direction its barrel points in the weapon camera's
frame (forward, left, up), which the camera is turned to aim */
void halo_vr_set_barrel(short local_player_index, const float world[3], const float direction[3]);
/* how far, in world units, the weapon's aim runs before it hits something
(render.c, each frame); the crosshair floats there */
void halo_vr_set_aim_distance(float world_units);
/* - and whether the HUD's crosshair, which marks where the head looks, is
  left out */
int halo_vr_hide_crosshair(void);
/* whether the crosshair is drawn apart this frame (render.c), and taking
it, drawn alone on the cleared back buffer, which is cleared again */
int halo_vr_crosshair_apart(void);
void halo_vr_capture_crosshair(void);

/* Vehicles (player_control.c, each tick): the seat the player sits in and
its vehicle's heading (yaw, radians). Seated, the body turns with the
vehicle; a driver who steers by facing (HALO_VR_SEAT_STEER) steers with the
right stick, else the right controller aims (HALO_VR_SEAT_AIM). And each
frame (render.c) the heading as drawn, between ticks. */
enum
{
	HALO_VR_SEAT_NONE = 0,
	HALO_VR_SEAT_STEER = 1,
	HALO_VR_SEAT_AIM = 2,
};
/* (armed: HALO_VR_SEAT_ARMED if the seat fires the vehicle's gun, as a
Ghost's driver does, and HALO_VR_SEAT_TURRET too if all it does is man the
gun, as the Warthog's gunner) */
enum
{
	HALO_VR_SEAT_ARMED = 1,
	HALO_VR_SEAT_TURRET = 2,
};
void halo_vr_set_seat(short local_player_index, int seat, int armed, float vehicle_yaw);
void halo_vr_set_seat_frame(short local_player_index, float vehicle_yaw);
/* At an open vehicle's wheel (a Warthog's), each frame (render.c): the
driver's hands on it as drawn (world units, from the game's camera), or
NULL; the player's own hands turn it. And whether they hold it. */
void halo_vr_set_wheel(short local_player_index, const float left[3], const float right[3]);
int halo_vr_wheel_held(void);
/* the controllers' layout (HALO_XR_LAYOUT_*: 0 the Quest's, 1 the Steam
Frame's), for what the game tells the player */
int halo_vr_controller_layout(void);
/* whether a seated player's view stays in first person (director.c) */
int halo_vr_vehicle_first_person(void);
/* VR test aid (debug.vehicle_spawn): both sticks held in for a moment asks
for a vehicle; nonzero once per ask (player_control.c spawns it) */
int halo_vr_take_vehicle_spawn(short local_player_index);
/* how far a gunner's view is moved back from the gun (world units) */
float halo_vr_gunner_camera_back(void);
/* how far forward an open vehicle's driver's view is moved (world units) */
float halo_vr_driver_camera_forward(void);
/* how far the eyes of a rider who sits down in the hull rise above his
head (world units), by the kind of hull */
enum
{
	HALO_VR_HULL_OPEN = 0,
	HALO_VR_HULL_SCOUT,    /* a Ghost: its hood arches over the rider */
	HALO_VR_HULL_FIGHTER,  /* a Banshee: the pilot lies in it */
	HALO_VR_HULL_TURRET,   /* a Shade: its shield is at the eyes */
	HALO_VR_HULL_TANK,     /* a Scorpion: its driver sits in the hull */
};
float halo_vr_enclosed_seat_raise(int hull);
/* how strongly glass tints and reflects (1 as the game has it) */
float halo_vr_glass_strength(void);

/* VR players of other machines in a network game (port/linux/game/
network_vr.c): their hands, as their machines send them, while they keep
coming (each returns 0 otherwise, and for this machine's own players, and
the game does as before):
- where the weapon aims, on foot;
- where its shots start (world units), from the unit's camera;
- whether the left hand holds the grenade;
- the hand's throw, once, as the grenade goes (as halo_vr_grenade_throw);
and this machine's own VR player's throw, to send (units.c). */
int halo_vr_network_aim(long player_index, float vector[3]);
int halo_vr_network_weapon_origin(long player_index, long unit_index, float origin[3]);
int halo_vr_network_grenade_held(long player_index);
int halo_vr_network_grenade_throw(long player_index, long unit_index, float origin[3], float direction[3],
	float *strength);
void halo_vr_network_grenade_thrown(short local_player_index, long unit_index, const float origin[3],
	const float direction[3], float strength);

/* the platform layer's side (vr.c) */
void halo_vr_initialize(void);
void halo_vr_frame_begin(void);
/* vr.log_performance: the end of a part of the frame, for the report: the
input and network, the game's ticks (how many), the camera and director,
the drawing; the rest is presenting */
enum
{
	HALO_VR_PHASE_INPUT,
	HALO_VR_PHASE_TICKS,
	HALO_VR_PHASE_CAMERA,
	HALO_VR_PHASE_DRAW,
	HALO_VR_PHASE_COUNT
};
void halo_vr_phase(int phase, int ticks);
/* vr.log_performance: time taken by a part of the drawing, for the report
(first_person_weapons.c): its start (0 when not logged), and its end. The
first-person pose (its animation and the arms), the arms alone, and the
pose taken as built already this frame (counted) */
enum
{
	HALO_VR_TIMING_FIRST_PERSON,
	HALO_VR_TIMING_ARMS,
	HALO_VR_TIMING_FIRST_PERSON_KEPT,
	HALO_VR_TIMING_COUNT
};
unsigned long long halo_vr_timing_start(void);
void halo_vr_timing_end(int part, unsigned long long start);
/* the headset's frames, counted (0 before the first): what is drawn the same
for every eye, as the first-person pose posed from the controller, is built
once a frame */
unsigned long halo_vr_frame_serial(void);
void halo_vr_present(unsigned int back_buffer_texture);
/* the eye image size: the back buffer's size in pixels in VR */
int halo_vr_eye_size(int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
