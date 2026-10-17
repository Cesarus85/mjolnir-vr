/*
VR.C

The game's side of VR (port/linux/include/halo_vr.h has the outline). On
Android the host runs the OpenXR session (port/android/host/host_xr.c) and
this file turns its poses into the game's cameras and facing; elsewhere
these are stubs that report VR as off.

Frames of reference. The headset reports poses in its LOCAL space: metres,
+x right, +y up, -z forward, the origin and heading where the user last
recentred. The game's player stands in that space's place: its origin is
the game's camera position, and its forward is the "body" yaw kept here.
The right stick (left-handed, mirrored: the left) turns the body; the head
turns on top of it, so the facing the game uses (halo_vr_aim) is the body's
yaw plus the head's.
*/

#include "halo_vr.h"

/* the actions' settings and the buttons' values in them (halo_vr.h's
HALO_VR_ACTION_* and HALO_VR_INPUT_*), by number */
static const char *const vr_action_settings[HALO_VR_ACTION_COUNT] =
{
	"vr.button_jump", "vr.button_light", "vr.button_action", "vr.button_weapon", "vr.button_crouch",
	"vr.button_zoom", "vr.button_melee", "vr.button_talk",
};
static const char *const vr_input_values[HALO_VR_INPUT_COUNT] =
{
	"a", "b", "x", "y", "move_stick", "turn_stick", "grip", "free_grip",
};

const char *halo_vr_action_setting(int action)
{
	return action >= 0 && action < HALO_VR_ACTION_COUNT ? vr_action_settings[action] : "";
}

const char *halo_vr_input_value(int input)
{
	return input >= 0 && input < HALO_VR_INPUT_COUNT ? vr_input_values[input] : "";
}

#ifdef HALO_ANDROID

#include "platform.h"
#include "port_config.h"
#include "guest_host.h"
#include "halo_xr_abi.h"

#include <SDL3/SDL.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define METRES_PER_WORLD_UNIT 3.048f
#define PI 3.14159265358979f
#define GRENADE_SAMPLES 24
/* the facing the game changes itself by more than this (a respawn, a
teleport, a vehicle's seat) turns the body with it */
#define FACING_JUMP (10.0f * PI / 180.0f)
/* vr.foot_vignette: none on foot, while the stick turns smoothly, and
while a stick turns smoothly or moves */
enum
{
	FOOT_VIGNETTE_OFF = 0,
	FOOT_VIGNETTE_TURNING,
	FOOT_VIGNETTE_MOVING,
};

static struct
{
	int active;
	struct halo_xr_info info;
	struct halo_xr_frame frame;
	int frame_begun;
	/* the hands as last seen with the app in focus (the system menu over
	it reports none): held still meanwhile */
	struct halo_xr_hand focused_hands[HALO_XR_HAND_COUNT];
	int focused_hands_valid;
	int eyes_captured;       /* bit per eye, this frame */
	int overlay_mode;        /* HALO_XR_OVERLAY_*, this frame */
	int overlay_blend;
	int last_overlay_mode;
	int last_frame_stereo;   /* the last frame drew the eyes */

	/* where in LOCAL space the game's camera is: the head's position when
	the view last recentred (vr_recentre) */
	float origin[3];
	int origin_valid;
	float game_camera[3];    /* the game's camera this frame (world units) */
	int recentre_frames;     /* recentre on this many more frames */

	/* vr.log_performance: sums over the frames since the last report */
	int log_performance;
	unsigned long draws_skipped, frames_skipping;
	unsigned long long report_start_us;
	unsigned long long frame_start_us;
	unsigned long long eyes_start_us;
	unsigned long long eyes_us;
	unsigned long long work_us;
	unsigned long long wait_us;
	int report_frames;
	int report_stereo_frames;
	/* (the frame's parts, halo_vr_phase: when each ended, and the game's
	ticks in it; summed apart for frames with ticks and without, with how
	many took longer than the headset's frame) */
	unsigned long long phase_end_us[HALO_VR_PHASE_COUNT];
	int frame_ticks;
	unsigned long long phase_us[2][HALO_VR_PHASE_COUNT + 1];
	int phase_frames[2];
	int phase_late[2];
	/* (parts of the drawing, halo_vr_timing_end: their time and count) */
	unsigned long long timing_us[HALO_VR_TIMING_COUNT];
	int timing_count[HALO_VR_TIMING_COUNT];

	/* the view's turning, gathered for halo_vr_look_delta */
	float look_yaw;
	float look_pitch;
	float look_turned_yaw;
	float look_turned_pitch;
	int look_valid;

	float body_yaw;
	int aim_synced;
	float last_yaw;
	float last_pitch;
	/* Seated (halo_vr_set_seat): the seat's kind, its vehicle's heading as
	the game has it (each tick) and as drawn (each frame), and the body's
	turn from that heading; the body turns with the vehicle. */
	/* the test aid's ask (vehicle_spawn_update) */
	int vehicle_spawn;
	int vehicle_spawn_asked;
	int vehicle_spawn_latched;
	unsigned long long vehicle_spawn_held_us;
	int seat_mode;
	int seat_armed;
	float seat_yaw;
	float seat_frame_yaw;
	int seat_frame_valid;
	float seat_turn;
	int multiview;              /* debug.multiview: both eyes in one draw */
	int multiview_logged;
	int multiview_logged_views;
	/* vr.foveation's level last logged (+1; 0: none yet) */
	int foveation_logged;
	int vehicle_first_person;
	int turret_handles;         /* a gun's barrel rises as its handles go down */
	float gunner_camera_back;   /* metres */
	float driver_camera_forward; /* metres */
	float enclosed_seat_raise[5];  /* metres, by HALO_VR_HULL_* */
	float glass_strength;
	float vehicle_steer_angle;  /* radians */
	float vehicle_gun_steer_angle; /* radians: steering that aims guns too */
	float vehicle_pitch_angle;
	/* the wheel of an open vehicle (a Warthog's: wheel_update) */
	int vehicle_wheel;          /* vr.vehicle_wheel: the hands turn it */
	float vehicle_wheel_angle;  /* radians: turned this far, it steers fully */
	int wheel_valid;            /* where it is, as last drawn */
	float wheel_hands_drawn[2][3]; /* the driver's drawn hands on it, from the camera (world units) */
	int wheel_held[HALO_XR_HAND_COUNT];
	int wheel_hands;            /* the hands that held it last frame (bits) */
	float wheel_reference;      /* their turn about the column last frame */
	float wheel_turn;           /* the wheel's turn (radians, anticlockwise as the driver sees it) */
	int wheel_logged;
	float vehicle_steer_curve;  /* the stick's deflection to this power */
	float vehicle_steer_smoothing; /* seconds */
	float steer[2];             /* the steering as smoothed: aside, up (-1..1) */
	unsigned long long steer_last_us;
	/* the comfort vignette: how strong at most in a vehicle, on foot
	(vr.foot_vignette: FOOT_VIGNETTE_*) and how strong there, now, and the
	heading it last saw */
	float comfort_vignette;
	int foot_vignette;
	float foot_vignette_strength;
	int seated;                 /* in a vehicle's seat, whatever the view */
	float vignette;
	float vignette_last_yaw;
	int vignette_yaw_valid;
	unsigned long long vignette_last_us;
	int snap_latched;
	unsigned long long last_frame_ms;

	/* settings (config.toml, [vr]) */
	unsigned long settings_read_at; /* config_changes() when the live ones were read */
	int smooth_turn;
	float snap_turn_degrees;
	float smooth_turn_degrees_per_second;
	int cinematics_3d;
	float hud_distance;
	float hud_width;
	float screen_distance;
	float screen_width;
	int positional_tracking;
	float scope_size;
	int crosshair;
	int crosshair_captured;  /* this frame (halo_vr_capture_crosshair) */
	/* players' names this frame (halo_vr_add_name), and whether their
	slots were taken (halo_vr_capture_names) */
	int name_count;
	float name_points[HALO_XR_MAXIMUM_NAMES][3];
	int names_captured;
	/* the barrel of the weapon as drawn (halo_vr_set_barrel): where shots,
	the crosshair and the laser start */
	float barrel[3];
	unsigned long barrel_frame;
	/* the barrel's direction in the weapon camera's frame (forward, left,
	up): first-person models point their barrels in towards the screen's
	centre, and the camera is turned so that they point where the
	controller aims */
	float barrel_direction[3];
	int barrel_direction_valid;
	float aim_distance;      /* metres from the muzzle to what the weapon aims at */
	unsigned long aim_distance_frame;
	/* the scope this frame: drawn (halo_vr_scope to halo_vr_capture_scope),
	captured, and where its disc is in LOCAL space */
	int scope_pass;
	int scope_captured;
	struct halo_xr_pose scope_pose;
	int motion_tracker_on_wrist;
	float motion_tracker_size;
	float motion_tracker_turn;
	float motion_tracker_right;
	float motion_tracker_down;
	int aim_with_controller;
	int laser;
	float weapon_adjust[3];  /* forward, right, down, metres: vr.weapon_hand_*, the hand moved on the weapon */
	float weapon_shift[3];   /* forward, right, up, metres: the weapon moved from the old fixed place (the scope) */
	float muzzle_distance;
	/* the free hand on the weapon (halo_vr_two_hand) */
	int two_hand_gripping;
	int two_hand_latched;    /* the grip is held (on the weapon or not) */
	int two_hand_valid;
	float two_hand_reach;    /* metres from the foregrip a closing grip takes the weapon */
	/* the free hand's grip as a button (free_grip_down): closed since (0
	open), and whether it took the weapon or the wheel since */
	unsigned long long free_grip_since_us;
	int free_grip_taken;
	/* the grenade in the free hand (grenade_update, vr.grenade_throw_speed):
	its trigger held, the hand's last positions (LOCAL), and the throw
	as it let go */
	float grenade_throw_speed;
	float grenade_scale;
	float grenade_offset[3];  /* in the grip's axes: right, up, back (metres) */
	float grenade_down;       /* down the knuckles (the aim's -y; metres) */
	float grenade_back;       /* back along the fingers (the aim's +z; metres) */
	int grenade_held;
	float grenade_sample[GRENADE_SAMPLES][3];
	unsigned long long grenade_sample_us[GRENADE_SAMPLES];
	int grenade_sample_count;
	int grenade_thrown;
	float grenade_hand[3];
	float grenade_direction[3];
	float grenade_strength;
	unsigned long long grenade_released_us;
	/* melee by swinging a hand (vr.melee_swing_speed) */
	float melee_swing_speed;
	float hand_last[HALO_XR_HAND_COUNT][3];
	int hand_last_valid[HALO_XR_HAND_COUNT];
	unsigned long long hand_last_us;
	unsigned long long melee_until_us;
	unsigned long long melee_ready_us;
	/* until when a melee attack aims where the head looks: its damage is
	dealt some ticks into the attack, along the unit's aim */
	unsigned long long melee_aim_until_us;
	int melee_button_down;
	float melee_strength;    /* 0..1, the last attack's swing */
	unsigned long long melee_jab_start_us; /* the melee button's attack: the weapon jabs */
	/* the laser along the barrel, for the frame it was set in */
	unsigned long frame_number;
	unsigned long laser_frame;
	float laser_start[3];
	float laser_direction[3];
	float two_hand_reference[3]; /* left wrist from right, in the weapon's forward, right, up */
	float lean_limit;
	float crouch_limit;
	float rise_limit;
	/* the fingers (fingers_update, vr.fingers): each hand's thumb, index,
	middle and the last two, curled 0 (open) to 1, and how fast */
	int fingers;
	int arm_ik;              /* vr.arm_ik: the free arm bends at the elbow */
	int body_mode;           /* vr.body: HALO_VR_BODY_* */
	float finger_curl[HALO_XR_HAND_COUNT][4];
	float finger_velocity[HALO_XR_HAND_COUNT][4];
	unsigned long long fingers_last_us;
	/* the torso's heading (torso_update): in LOCAL space, so that turning
	the body by the stick turns it too; and the head's, level */
	float torso_yaw;
	int torso_valid;
	unsigned long long torso_last_us;
	float head_level_yaw;
	/* the hands (vr.handedness): the weapon in the left one, and with
	vr.mirror_controls the sticks and face buttons mirrored too */
	int left_handed;
	int mirror_controls;
	/* the button each action is on (vr.button_*: HALO_VR_INPUT_* by
	HALO_VR_ACTION_*) */
	int action_inputs[HALO_VR_ACTION_COUNT];
	/* vr.refresh_rate as last asked for (the VR options change it), and
	vr.resolution_scale as the app started with it */
	float refresh_rate_wanted;
	float resolution_scale;
	/* vr.refresh_rate_auto (refresh_governor): the rate asked for now (the
	one wanted, or lower while frames come late), the rates below and above
	it, and the window's frames: all, late at this rate, and those that
	would be late at the one above */
	int refresh_auto;
	float refresh_rate_now, refresh_lower, refresh_higher, refresh_period_ms;
	unsigned long long refresh_window_us, refresh_changed_us;
	unsigned long refresh_frames, refresh_late, refresh_tight;
	int refresh_bad_windows, refresh_good_windows;
} vr;

/* d3d8_gl.c */
unsigned int d3d8_back_buffer_texture(void);
int d3d8_multiview_supported(void);
int d3d8_multiview_begin(const float rows[4][4], const float resolve_rect[2][4]);
void d3d8_multiview_rows(const float rows[4][4]);
void d3d8_multiview_depth(float a, float b);
void d3d8_multiview_projected_depth(int projected);
unsigned int d3d8_multiview_end(void);
void d3d8_clear_back_buffer(float red, float green, float blue, float alpha);
void d3d8_clear_back_buffer_region(float left, float top, float right, float bottom);
void d3d8_host_state_changed(void);
int d3d8_foveation_supported(void);
void d3d8_foveation_parameters(int eye, float focal_x, float focal_y, float gain, float area, float minimum_density);
void d3d8_foveation_eye(int eye);
static void foveation_update(void);
void xgpu_draw_timing(int on);

/* a layer copied (host_xr_copy_layer): 2 is a deferred copy, which left GL
as it drew, for the renderer to set again */
static int vr_copied(int result)
{
	if (result == 2)
		d3d8_host_state_changed();
	return result != 0;
}

/* The motion tracker, the HUD's one element at its bottom left, moves from
the HUD to the left wrist (host_xr.c draws it there): this part of the
640x480 HUD (left, top, right, bottom), square as the HUD shows it, around
its disc (x 49-131, y 362-444) and its range below. */
#define WRIST_REGION_LEFT (38.0f / 640.0f)
#define WRIST_REGION_TOP (352.0f / 480.0f)
#define WRIST_REGION_RIGHT (142.0f / 640.0f)
#define WRIST_REGION_BOTTOM (456.0f / 480.0f)

/* The weapon's crosshair, drawn at the HUD's centre, moves to where the
weapon aims, as far away as what it aims at (so the eyes focus on it with
the target), keeping the size it has in the HUD: this part of the HUD. */
#define CROSSHAIR_REGION_PIXELS 80.0f
#define CROSSHAIR_REGION_LEFT ((320.0f - CROSSHAIR_REGION_PIXELS * 0.5f) / 640.0f)
#define CROSSHAIR_REGION_TOP ((240.0f - CROSSHAIR_REGION_PIXELS * 0.5f) / 480.0f)
#define CROSSHAIR_REGION_RIGHT ((320.0f + CROSSHAIR_REGION_PIXELS * 0.5f) / 640.0f)
#define CROSSHAIR_REGION_BOTTOM ((240.0f + CROSSHAIR_REGION_PIXELS * 0.5f) / 480.0f)
/* how far it floats with nothing in the way, and its nearest, in metres */
#define CROSSHAIR_FARTHEST 60.0f
#define CROSSHAIR_NEAREST 0.5f

/* ---------- math */

static void cross3(const float a[3], const float b[3], float out[3]);
static float normalize3(float v[3]);
static int crosshair_pose(struct halo_xr_pose *pose, float *size);
static int name_pose(const float point[3], struct halo_xr_pose *pose, float *width);
static int weapon_hand_tracked(short local_player_index);
static void controller_axes(float forward[3], float up[3], float right[3]);
static void hand_offset(float out[3]);
static void controls_in_play(struct halo_xr_hand hands[HALO_XR_HAND_COUNT]);

/* the hand that holds the weapon, the free one (the grenade, the
foregrip), and the one whose stick turns the body and steers */
static int weapon_hand(void)
{
	return vr.left_handed ? HALO_XR_HAND_LEFT : HALO_XR_HAND_RIGHT;
}

static int free_hand(void)
{
	return vr.left_handed ? HALO_XR_HAND_RIGHT : HALO_XR_HAND_LEFT;
}

static int turn_hand(void)
{
	return vr.left_handed && vr.mirror_controls ? HALO_XR_HAND_LEFT : HALO_XR_HAND_RIGHT;
}

static void quaternion_rotate(const float q[4], const float v[3], float out[3])
{
	/* out = q v q^-1 */
	float tx = 2.0f * (q[1] * v[2] - q[2] * v[1]);
	float ty = 2.0f * (q[2] * v[0] - q[0] * v[2]);
	float tz = 2.0f * (q[0] * v[1] - q[1] * v[0]);

	out[0] = v[0] + q[3] * tx + (q[1] * tz - q[2] * ty);
	out[1] = v[1] + q[3] * ty + (q[2] * tx - q[0] * tz);
	out[2] = v[2] + q[3] * tz + (q[0] * ty - q[1] * tx);
}

/* a LOCAL-space vector as a game-space one, for the body's yaw */
static void local_to_game(const float v[3], float yaw, float out[3])
{
	float forward = -v[2];
	float left = -v[0];
	float c = cosf(yaw), s = sinf(yaw);

	out[0] = forward * c - left * s;
	out[1] = forward * s + left * c;
	out[2] = v[1];
}

static float wrap_angle(float angle)
{
	while (angle > PI)
		angle -= 2.0f * PI;
	while (angle < -PI)
		angle += 2.0f * PI;
	return angle;
}

/* the head's yaw and pitch in LOCAL space (yaw counter-clockwise from
forward, seen from above, as the game's) */
static void head_angles(float *yaw, float *pitch)
{
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	float direction[3];

	quaternion_rotate(vr.frame.head.orientation, forward, direction);
	*yaw = atan2f(-direction[0], -direction[2]);
	*pitch = atan2f(direction[1], sqrtf(direction[0] * direction[0] + direction[2] * direction[2]));
}

/* seated, the vehicle's heading as last drawn (else as the game has it),
and the body turned with it */
static float seat_heading(void)
{
	return vr.seat_frame_valid ? vr.seat_frame_yaw : vr.seat_yaw;
}

static void seat_body(void)
{
	if (vr.seat_mode != HALO_VR_SEAT_NONE)
		vr.body_yaw = wrap_angle(seat_heading() + vr.seat_turn);
}

/* seated, the way the head looks now is the vehicle's forward */
static void seat_face_forward(void)
{
	float head_yaw, head_pitch;

	head_angles(&head_yaw, &head_pitch);
	vr.seat_turn = -head_yaw;
	seat_body();
}

/* makes the head's position the game camera's, and the way it looks the
game's facing (halo_vr_aim syncs the body's yaw again), or seated, the
vehicle's forward */
static void vr_recentre(void)
{
	if (!(vr.frame.head_flags & HALO_XR_POSE_VALID))
		return;
	memcpy(vr.origin, vr.frame.head.position, sizeof(vr.origin));
	vr.origin_valid = 1;
	vr.aim_synced = 0;
	if (vr.seat_mode != HALO_VR_SEAT_NONE)
		seat_face_forward();
}

/* the offset of a tracked point (an eye, a hand) from the game's camera, in
the game's axes (metres): the head's offset from the origin, kept within the
lean and crouch limits so the view stays near the player's body, plus the
point's own offset from the head */
/* the head's offset from the origin (LOCAL, metres), kept within the lean
and crouch limits */
static void clamped_head(float head[3])
{
	int axis;

	head[0] = head[1] = head[2] = 0.0f;
	if (!vr.origin_valid)
		vr_recentre();
	if (vr.positional_tracking && vr.origin_valid)
	{
		float horizontal;

		for (axis = 0; axis < 3; axis++)
			head[axis] = vr.frame.head.position[axis] - vr.origin[axis];
		horizontal = sqrtf(head[0] * head[0] + head[2] * head[2]);
		if (horizontal > vr.lean_limit && horizontal > 0.0f)
		{
			head[0] *= vr.lean_limit / horizontal;
			head[2] *= vr.lean_limit / horizontal;
		}
		if (head[1] < -vr.crouch_limit)
			head[1] = -vr.crouch_limit;
		if (head[1] > vr.rise_limit)
			head[1] = vr.rise_limit;
	}
}

static void tracked_offset(const float position[3], float out[3])
{
	float head[3], local[3];
	int axis;

	clamped_head(head);
	for (axis = 0; axis < 3; axis++)
		local[axis] = head[axis] + position[axis] - vr.frame.head.position[axis];
	local_to_game(local, vr.body_yaw, out);
}

/* a direction in the game's axes as one in LOCAL space */
static void game_to_local(const float v[3], float out[3])
{
	float c = cosf(vr.body_yaw), s = sinf(vr.body_yaw);
	float forward = v[0] * c + v[1] * s;
	float left = -v[0] * s + v[1] * c;

	out[0] = -left;
	out[1] = v[2];
	out[2] = -forward;
}

/* the inverse: a point in the game's world as one in LOCAL space */
static void world_to_local(const float world[3], float local[3])
{
	float offset[3], head[3], forward, left;
	float c = cosf(vr.body_yaw), s = sinf(vr.body_yaw);
	int axis;

	for (axis = 0; axis < 3; axis++)
		offset[axis] = (world[axis] - vr.game_camera[axis]) * METRES_PER_WORLD_UNIT;
	forward = offset[0] * c + offset[1] * s;
	left = -offset[0] * s + offset[1] * c;
	clamped_head(head);
	local[0] = -left - head[0] + vr.frame.head.position[0];
	local[1] = offset[2] - head[1] + vr.frame.head.position[1];
	local[2] = -forward - head[2] + vr.frame.head.position[2];
}

/* ---------- platform side */

/* the settings the VR options (ui_widget.c) change while the game runs:
read at the start, and again whenever one changes */
static void vr_read_live_settings(void)
{
	vr.settings_read_at = config_changes();
	vr.smooth_turn = config_boolean("vr.smooth_turn");
	vr.snap_turn_degrees = (float)config_real("vr.snap_turn_degrees");
	vr.smooth_turn_degrees_per_second = (float)config_real("vr.smooth_turn_speed");
	vr.motion_tracker_on_wrist = config_boolean("vr.motion_tracker_on_wrist");
	vr.crosshair = config_boolean("vr.crosshair");
	vr.laser = config_boolean("vr.laser");
	vr.vehicle_wheel = config_boolean("vr.vehicle_wheel");
	vr.comfort_vignette = (float)config_real("vr.comfort_vignette");
	{
		const char *foot = config_string("vr.foot_vignette");

		vr.foot_vignette = !strcmp(foot, "moving") ? FOOT_VIGNETTE_MOVING :
			!strcmp(foot, "turning") ? FOOT_VIGNETTE_TURNING : FOOT_VIGNETTE_OFF;
	}
	vr.foot_vignette_strength = (float)config_real("vr.foot_vignette_strength");
	vr.fingers = config_boolean("vr.fingers");
	vr.arm_ik = config_boolean("vr.arm_ik");
	{
		const char *body = config_string("vr.body");

		vr.body_mode = !strcmp(body, "hands") ? HALO_VR_BODY_HANDS :
			!strcmp(body, "off") ? HALO_VR_BODY_OFF :
			!strcmp(body, "full") ? HALO_VR_BODY_FULL : HALO_VR_BODY_ARMS;
	}
	{
		int left_handed = !strcmp(config_string("vr.handedness"), "left");

		if (left_handed != vr.left_handed)
		{
			/* (the hands change places: what the free hand held lets go) */
			vr.two_hand_latched = vr.two_hand_gripping = 0;
			vr.grenade_held = vr.grenade_thrown = 0;
			vr.grenade_sample_count = 0;
			vr.barrel_frame = 0;
			platform_log("VR: the weapon in the %s hand", left_handed ? "left" : "right");
		}
		vr.left_handed = left_handed;
		vr.mirror_controls = config_boolean("vr.mirror_controls");
	}
	{
		int action;

		for (action = 0; action < HALO_VR_ACTION_COUNT; action++)
			vr.action_inputs[action] = halo_vr_action_input(action);
	}
	{
		/* (the refresh rate changes at once where the runtime lets it) */
		float refresh_rate = (float)config_real("vr.refresh_rate");

		int automatic = config_boolean("vr.refresh_rate_auto");

		if (vr.active && (refresh_rate != vr.refresh_rate_wanted ||
			(!automatic && vr.refresh_rate_now != refresh_rate)))
		{
			platform_log("VR: refresh rate %.0f Hz asked for", refresh_rate);
			host_xr_set_refresh_rate(refresh_rate);
		}
		if (refresh_rate != vr.refresh_rate_wanted || automatic != vr.refresh_auto)
		{
			/* (the governor starts over from the rate chosen) */
			vr.refresh_rate_now = refresh_rate;
			vr.refresh_window_us = 0;
			vr.refresh_bad_windows = vr.refresh_good_windows = 0;
		}
		vr.refresh_rate_wanted = refresh_rate;
		vr.refresh_auto = automatic;
	}
}

void halo_vr_initialize(void)
{
	if (vr.active || !getenv("HALO_VR"))
		return;
	vr_read_live_settings();
	vr.cinematics_3d = config_boolean("vr.cinematics_in_3d");
	vr.hud_distance = (float)config_real("vr.hud_distance");
	vr.hud_width = 2.0f * vr.hud_distance * tanf((float)config_real("vr.hud_field_of_view") * PI / 360.0f);
	vr.screen_distance = (float)config_real("vr.screen_distance");
	vr.screen_width = (float)config_real("vr.screen_width");
	vr.positional_tracking = config_boolean("vr.positional_tracking");
	vr.scope_size = (float)config_real("vr.scope_size");
	vr.motion_tracker_size = (float)config_real("vr.motion_tracker_size");
	vr.motion_tracker_turn = (float)config_real("vr.motion_tracker_turn");
	vr.motion_tracker_right = (float)config_real("vr.motion_tracker_right");
	vr.motion_tracker_down = (float)config_real("vr.motion_tracker_down");
	vr.lean_limit = (float)config_real("vr.lean_limit");
	vr.crouch_limit = (float)config_real("vr.crouch_limit");
	vr.rise_limit = 0.25f;
	vr.log_performance = config_boolean("vr.log_performance");
	/* (the draws timed for the long frames' lines) */
	xgpu_draw_timing(vr.log_performance);
	vr.resolution_scale = (float)config_real("vr.resolution_scale");
	vr.aim_with_controller = strcmp(config_string("vr.aim_with"), "head") != 0;
	vr.weapon_adjust[0] = (float)config_real("vr.weapon_hand_forward");
	vr.weapon_adjust[1] = (float)config_real("vr.weapon_hand_right");
	vr.weapon_adjust[2] = (float)config_real("vr.weapon_hand_down");
	vr.muzzle_distance = 0.3f;
	vr.two_hand_reach = (float)config_real("vr.two_hand_reach");
	vr.melee_swing_speed = (float)config_real("vr.melee_swing_speed");
	vr.grenade_throw_speed = (float)config_real("vr.grenade_throw_speed");
	vr.multiview = config_boolean("debug.multiview");
	vr.vehicle_first_person = config_boolean("vr.vehicle_first_person");
	vr.vehicle_spawn = config_boolean("debug.vehicle_spawn");
	vr.turret_handles = config_boolean("vr.turret_handles");
	vr.gunner_camera_back = (float)config_real("vr.gunner_camera_back");
	vr.driver_camera_forward = (float)config_real("vr.driver_camera_forward");
	vr.enclosed_seat_raise[HALO_VR_HULL_SCOUT] = (float)config_real("vr.ghost_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_FIGHTER] = (float)config_real("vr.banshee_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_TURRET] = (float)config_real("vr.turret_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_TANK] = (float)config_real("vr.tank_eye_raise");
	vr.glass_strength = (float)config_real("vr.glass_strength");
	vr.vehicle_steer_angle = (float)config_real("vr.vehicle_steer_degrees") * PI / 180.0f;
	vr.vehicle_gun_steer_angle = (float)config_real("vr.vehicle_gun_steer_degrees") * PI / 180.0f;
	vr.vehicle_pitch_angle = (float)config_real("vr.vehicle_pitch_degrees") * PI / 180.0f;
	vr.vehicle_wheel_angle = (float)config_real("vr.vehicle_wheel_degrees") * PI / 180.0f;
	if (vr.vehicle_wheel_angle < 10.0f * PI / 180.0f)
		vr.vehicle_wheel_angle = 10.0f * PI / 180.0f;
	vr.vehicle_steer_curve = (float)config_real("vr.vehicle_steer_curve");
	vr.vehicle_steer_smoothing = (float)config_real("vr.vehicle_steer_smoothing");
	vr.grenade_scale = (float)config_real("vr.grenade_scale");
	vr.grenade_offset[0] = (float)config_real("vr.grenade_right");
	vr.grenade_offset[1] = 0.0f;
	vr.grenade_down = (float)config_real("vr.grenade_down");
	vr.grenade_back = (float)config_real("vr.grenade_back");
	vr.grenade_offset[2] = -(float)config_real("vr.grenade_forward");
	if (!host_xr_initialize(&vr.info, (float)config_real("vr.refresh_rate")))
	{
		platform_log("VR: the headset's session could not be created; see logcat");
		return;
	}
	vr.active = 1;
	platform_log("VR: %s, eyes %dx%d at %.0f Hz", vr.info.system_name, vr.info.eye_width, vr.info.eye_height,
		vr.info.refresh_rate);
	/* Each eye drawn apart (debug.multiview draws both at once). At once,
	the eyes' pictures did not always fuse: on the Steam Frame now and then
	(each alone sharp, the two together doubled as the head turned,
	2026-10-05; one by one costs it no more there), and on the Quest 3 in
	big online games (doubled from the start as the head turned, none with
	48 bots; one by one, none online either, 2026-10-16) */
	platform_log("VR: the eyes drawn %s (debug.multiview)", vr.multiview ? "at once" : "one by one");
}

int halo_vr_active(void)
{
	return vr.active;
}

int halo_vr_controller_layout(void)
{
	return vr.active ? vr.frame.controller_layout : HALO_XR_LAYOUT_TOUCH;
}

int halo_vr_weapon_hand(void)
{
	return weapon_hand();
}

int halo_vr_action_input(int action)
{
	const char *value;
	int input;

	if (action < 0 || action >= HALO_VR_ACTION_COUNT)
		return HALO_VR_INPUT_A;
	value = config_string(vr_action_settings[action]);
	for (input = 0; input < HALO_VR_INPUT_COUNT; input++)
	{
		if (!strcmp(value, vr_input_values[input]))
			return input;
	}
	return action;
}

int halo_vr_refresh_rates(float *rates, int maximum, float *current)
{
	*current = 0.0f;
	return vr.active ? host_xr_refresh_rates(rates, maximum, current) : 0;
}

float halo_vr_started_resolution_scale(void)
{
	return vr.resolution_scale;
}

int halo_vr_graphics(void)
{
	/* (read as it changes: d3d8_gl.c may ask before vr.c reads the live
	settings again) */
	static unsigned long read_at = (unsigned long)-1;
	static int graphics = HALO_VR_GRAPHICS_HIGH;

	if (!vr.active)
		return HALO_VR_GRAPHICS_HIGH;
	if (read_at != config_changes())
	{
		const char *name = config_string("vr.graphics");

		read_at = config_changes();
		graphics = !strcmp(name, "low") ? HALO_VR_GRAPHICS_LOW :
			!strcmp(name, "medium") ? HALO_VR_GRAPHICS_MEDIUM :
			!strcmp(name, "max") ? HALO_VR_GRAPHICS_MAX :
			!strcmp(name, "auto") ? HALO_VR_GRAPHICS_AUTO :
			!strcmp(name, "high") ? HALO_VR_GRAPHICS_HIGH : HALO_VR_GRAPHICS_MEDIUM;
	}
	return graphics;
}

int halo_vr_eye_size(int *width, int *height)
{
	const char *eye_width = getenv("HALO_VR_EYE_WIDTH");
	const char *eye_height = getenv("HALO_VR_EYE_HEIGHT");

	if (!getenv("HALO_VR") || !eye_width || !eye_height)
		return 0;
	*width = atoi(eye_width);
	*height = atoi(eye_height);
	return *width > 0 && *height > 0;
}

static unsigned long long now_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)now.tv_sec * 1000000ULL + (unsigned long long)now.tv_nsec / 1000ULL;
}

/* vr.log_performance: every five seconds, the frames the game gave the
headset, and where their time went: waiting for the headset (xrWaitFrame),
drawing the eyes, and the rest (the game's update, the overlay, the copies) */
void halo_vr_phase(int phase, int ticks)
{
	if (!vr.active || !vr.log_performance || phase < 0 || phase >= HALO_VR_PHASE_COUNT)
		return;
	vr.phase_end_us[phase] = now_us();
	if (phase == HALO_VR_PHASE_TICKS)
		vr.frame_ticks = ticks;
}

/* debug.vr_body (flat, port/linux/game/vr_body.c): the body's pose timed
off the headset too */
static int debug_body_timing(void)
{
	static int timing = -1;

	if (timing < 0)
	{
		const char *view = config_string("debug.vr_body");

		timing = view && *view && strcmp(view, "off") != 0;
	}
	return timing;
}

unsigned long long halo_vr_timing_start(void)
{
	return (vr.active && vr.log_performance) || (!vr.active && debug_body_timing()) ? now_us() : 0;
}

unsigned long halo_vr_frame_serial(void)
{
	return vr.active ? vr.frame_number : 0;
}

void halo_vr_timing_end(int part, unsigned long long start)
{
	if (!start || part < 0 || part >= HALO_VR_TIMING_COUNT)
		return;
	vr.timing_us[part] += now_us() - start;
	vr.timing_count[part]++;
	if (!vr.active)
	{
		/* (flat, debug.vr_body: the body alone, every five seconds) */
		unsigned long long now = now_us();

		if (!vr.report_start_us)
			vr.report_start_us = now;
		if (now - vr.report_start_us >= 5000000 && vr.timing_count[HALO_VR_TIMING_BODY])
		{
			platform_log("VR body (flat): %.3f ms a pose, %d poses in %.1f s",
				vr.timing_us[HALO_VR_TIMING_BODY] * 1e-3f / vr.timing_count[HALO_VR_TIMING_BODY],
				vr.timing_count[HALO_VR_TIMING_BODY], (now - vr.report_start_us) * 1e-6f);
			memset(vr.timing_us, 0, sizeof(vr.timing_us));
			memset(vr.timing_count, 0, sizeof(vr.timing_count));
			vr.report_start_us = now;
		}
	}
}

/* d3d8_gl.c, xbox_textures.c: what the frame's drawing waited for */
void xgpu_shader_work_take(unsigned long *count, float *milliseconds, unsigned long *loaded,
	float *loaded_milliseconds);
void xgpu_texture_work_take(unsigned long *uploads, unsigned long *kilotexels, unsigned long *decoded,
	unsigned long *pictures, unsigned long *targets, float *milliseconds);
void xgpu_draw_work_take(unsigned long *skipped, float *total_milliseconds, float *slowest_milliseconds,
	unsigned long *slowest_program, unsigned long long *slowest_key);

/* the frame's parts, from the marks halo_vr_phase left (a part not marked
this frame: none) */
static void performance_phases(unsigned long long now)
{
	int kind = vr.frame_ticks > 0;
	unsigned long long last = vr.frame_start_us;
	int phase;

	for (phase = 0; phase < HALO_VR_PHASE_COUNT; phase++)
	{
		unsigned long long end = vr.phase_end_us[phase];

		if (end >= last && end <= now)
		{
			vr.phase_us[kind][phase] += end - last;
			last = end;
		}
	}
	vr.phase_us[kind][HALO_VR_PHASE_COUNT] += now - last;
	vr.phase_frames[kind]++;
	/* (late: longer than the headset's frame, less a millisecond) */
	if (vr.info.refresh_rate > 1.0f && (float)(now - vr.frame_start_us + 1000) > 1e6f / vr.info.refresh_rate)
		vr.phase_late[kind]++;
	{
		/* a frame two of the headset's long, what it spent: the headset
		shows an old frame (and the Quest turns ASW on a while), seen as a
		doubled picture when turning */
		unsigned long shaders, loaded, uploads, kilotexels, decoded, pictures, targets, skipped, slowest_program;
		unsigned long long slowest_key;
		float shader_ms, loaded_ms, texture_ms, draws_ms, slowest_ms;

		xgpu_shader_work_take(&shaders, &shader_ms, &loaded, &loaded_ms);
		xgpu_texture_work_take(&uploads, &kilotexels, &decoded, &pictures, &targets, &texture_ms);
		xgpu_draw_work_take(&skipped, &draws_ms, &slowest_ms, &slowest_program, &slowest_key);
		vr.draws_skipped += skipped;
		if (skipped)
			vr.frames_skipping++;
		if (vr.info.refresh_rate > 1.0f && (float)(now - vr.frame_start_us) > 2e6f / vr.info.refresh_rate)
		{
			unsigned long long mark = vr.frame_start_us;
			float ms[HALO_VR_PHASE_COUNT];

			for (phase = 0; phase < HALO_VR_PHASE_COUNT; phase++)
			{
				unsigned long long end = vr.phase_end_us[phase];

				ms[phase] = end >= mark && end <= now ? (end - mark) * 1e-3f : 0.0f;
				if (end >= mark && end <= now)
					mark = end;
			}
			platform_log("VR: long frame %.1f ms: input %.1f, ticks %.1f, camera %.1f, draw %.1f, present %.1f; "
				"%lu shaders compiled or linked in %.1f ms, %lu programs from the cache in %.1f ms; "
				"%lu textures uploaded (%lu thousand texels, %lu decoded from DXT), %lu menu pictures, "
				"%lu render targets made, in %.1f ms; draw calls %.1f ms, the slowest %.1f ms (vertex program %lu, "
				"program %016llx); %lu draws left out waiting for their programs", (now - vr.frame_start_us) * 1e-3f,
				ms[HALO_VR_PHASE_INPUT], ms[HALO_VR_PHASE_TICKS], ms[HALO_VR_PHASE_CAMERA], ms[HALO_VR_PHASE_DRAW],
				(now - mark) * 1e-3f, shaders, shader_ms, loaded, loaded_ms, uploads, kilotexels, decoded, pictures,
				targets, texture_ms, draws_ms, slowest_ms, slowest_program, slowest_key, skipped);
		}
	}
	memset(vr.phase_end_us, 0, sizeof(vr.phase_end_us));
	vr.frame_ticks = 0;
}

/* vr.refresh_rate_auto: frames late at the headset's rate (a game's big
online games: the ticks cost as a client in step with the players) are
shown twice by the headset, a doubled picture when turning. While more
than one in 32 of them come late, two windows of REFRESH_WINDOW_US
running, the rate steps down to the next the headset offers (the Quest
3's: 90, 80, 72 Hz); while hardly any would be late at the next one up (one in a
hundred), ten windows running, it steps back up, never above
vr.refresh_rate. Frames of a load (REFRESH_LOAD_MS and more) count for
neither; a step waits REFRESH_SETTLE_US (down) or REFRESH_RAISE_US (up)
after the last. */
#define REFRESH_WINDOW_US 2000000ULL
#define REFRESH_SETTLE_US 10000000ULL
#define REFRESH_RAISE_US 30000000ULL
#define REFRESH_LOAD_MS 50.0f

static void refresh_rates_around(void)
{
	float rates[16], current = 0.0f;
	int count = host_xr_refresh_rates(rates, 16, &current);
	float ceiling = vr.refresh_rate_wanted > 1.0f ? vr.refresh_rate_wanted + 0.5f : 1e6f;
	int index;

	vr.refresh_lower = vr.refresh_higher = 0.0f;
	if (current <= 1.0f)
		current = vr.refresh_rate_now;
	vr.refresh_period_ms = current > 1.0f ? 1000.0f / current : 0.0f;
	for (index = 0; index < count; index++)
	{
		if (rates[index] < current - 0.5f && rates[index] > vr.refresh_lower)
			vr.refresh_lower = rates[index];
		if (rates[index] > current + 0.5f && rates[index] <= ceiling &&
			(!vr.refresh_higher || rates[index] < vr.refresh_higher))
			vr.refresh_higher = rates[index];
	}
}

static void refresh_step(float rate, unsigned long long now, const char *why)
{
	platform_log("VR: refresh rate %.0f Hz (from %.0f): %s (%lu of %lu frames late, %lu too slow for the rate above)",
		rate, 1000.0f / vr.refresh_period_ms, why, vr.refresh_late, vr.refresh_frames, vr.refresh_tight);
	host_xr_set_refresh_rate(rate);
	vr.refresh_rate_now = rate;
	vr.refresh_changed_us = now;
	vr.refresh_bad_windows = vr.refresh_good_windows = 0;
}

static void refresh_governor(void)
{
	unsigned long long now = now_us();
	float work_ms = (float)(now - vr.frame_start_us) * 1e-3f;

	if (!vr.refresh_auto)
		return;
	if (!vr.refresh_window_us)
	{
		refresh_rates_around();
		vr.refresh_window_us = now;
		vr.refresh_frames = vr.refresh_late = vr.refresh_tight = 0;
		return;
	}
	if (work_ms < REFRESH_LOAD_MS && vr.refresh_period_ms > 0.0f)
	{
		vr.refresh_frames++;
		if (work_ms + 1.0f > vr.refresh_period_ms)
			vr.refresh_late++;
		if (vr.refresh_higher > 1.0f && work_ms + 1.0f > 1000.0f / vr.refresh_higher)
			vr.refresh_tight++;
	}
	if (now - vr.refresh_window_us < REFRESH_WINDOW_US)
		return;
	if (vr.refresh_frames)
	{
		vr.refresh_bad_windows = vr.refresh_late * 32 > vr.refresh_frames ? vr.refresh_bad_windows + 1 : 0;
		vr.refresh_good_windows = vr.refresh_tight * 100 <= vr.refresh_frames ? vr.refresh_good_windows + 1 : 0;
		if (vr.refresh_lower > 1.0f && vr.refresh_bad_windows >= 2 && now - vr.refresh_changed_us >= REFRESH_SETTLE_US)
			refresh_step(vr.refresh_lower, now, "frames come late");
		else if (vr.refresh_higher > 1.0f && vr.refresh_good_windows >= 10 &&
			now - vr.refresh_changed_us >= REFRESH_RAISE_US)
		{
			refresh_step(vr.refresh_higher, now, "frames would keep up again");
		}
	}
	refresh_rates_around();
	vr.refresh_window_us = now;
	vr.refresh_frames = vr.refresh_late = vr.refresh_tight = 0;
}

static void performance_report(int stereo)
{
	unsigned long long now = now_us();

	performance_phases(now);
	vr.work_us += now - vr.frame_start_us;
	vr.report_frames++;
	vr.report_stereo_frames += stereo != 0;
	if (!vr.report_start_us)
		vr.report_start_us = now;
	if (now - vr.report_start_us < 5000000)
		return;
	{
		/* (the rate the headset runs at now: the VR options change it) */
		float rates[1], current = 0.0f;

		host_xr_refresh_rates(rates, 0, &current);
		if (current > 1.0f)
			vr.info.refresh_rate = current;
	}
	{
		float frames = (float)vr.report_frames;
		float seconds = (float)(now - vr.report_start_us) * 1e-6f;

		platform_log("VR: %.1f fps (%d%% stereo); per frame %.1f ms working (eyes %.1f ms), %.1f ms waiting "
			"for the headset; %lu draws in %lu frames left out waiting for their programs", frames / seconds,
			(int)(100.0f * vr.report_stereo_frames / frames), vr.work_us * 1e-3f / frames, vr.eyes_us * 1e-3f / frames,
			vr.wait_us * 1e-3f / frames, vr.draws_skipped, vr.frames_skipping);
		vr.draws_skipped = vr.frames_skipping = 0;
		{
			int kind;

			for (kind = 1; kind >= 0; kind--)
			{
				float count = (float)(vr.phase_frames[kind] ? vr.phase_frames[kind] : 1);

				platform_log("VR:   %s: %d frames, %d late; ms input %.1f, ticks %.1f, camera %.1f, draw %.1f, "
					"present %.1f", kind ? "with ticks" : "no ticks", vr.phase_frames[kind], vr.phase_late[kind],
					vr.phase_us[kind][HALO_VR_PHASE_INPUT] * 1e-3f / count,
					vr.phase_us[kind][HALO_VR_PHASE_TICKS] * 1e-3f / count,
					vr.phase_us[kind][HALO_VR_PHASE_CAMERA] * 1e-3f / count,
					vr.phase_us[kind][HALO_VR_PHASE_DRAW] * 1e-3f / count,
					vr.phase_us[kind][HALO_VR_PHASE_COUNT] * 1e-3f / count);
			}
		}
		memset(vr.phase_us, 0, sizeof(vr.phase_us));
		memset(vr.phase_frames, 0, sizeof(vr.phase_frames));
		memset(vr.phase_late, 0, sizeof(vr.phase_late));
		/* (the first-person pose: built how often a frame and in how long,
		the arms' part of it, and how often taken as built) */
		platform_log("VR:   first person: %.2f ms a frame in %.2f builds (arms %.3f ms), %.2f kept; "
			"body %.3f ms a frame in %.2f poses",
			vr.timing_us[HALO_VR_TIMING_FIRST_PERSON] * 1e-3f / frames,
			vr.timing_count[HALO_VR_TIMING_FIRST_PERSON] / frames,
			vr.timing_us[HALO_VR_TIMING_ARMS] * 1e-3f / frames,
			vr.timing_count[HALO_VR_TIMING_FIRST_PERSON_KEPT] / frames,
			vr.timing_us[HALO_VR_TIMING_BODY] * 1e-3f / frames,
			vr.timing_count[HALO_VR_TIMING_BODY] / frames);
		memset(vr.timing_us, 0, sizeof(vr.timing_us));
		memset(vr.timing_count, 0, sizeof(vr.timing_count));
	}
	vr.report_start_us = now;
	vr.report_frames = vr.report_stereo_frames = 0;
	vr.work_us = vr.eyes_us = vr.wait_us = 0;
}

/* gathers the view's turning (the head's, and the body's by the stick) for
halo_vr_look_delta, whether or not the game lets the player turn now */
static void look_update(void)
{
	float yaw, pitch;

	if (!(vr.frame.head_flags & HALO_XR_POSE_VALID))
	{
		vr.look_valid = 0;
		return;
	}
	head_angles(&yaw, &pitch);
	yaw = wrap_angle(yaw + vr.body_yaw);
	if (vr.look_valid)
	{
		vr.look_turned_yaw += wrap_angle(yaw - vr.look_yaw);
		vr.look_turned_pitch += pitch - vr.look_pitch;
	}
	vr.look_yaw = yaw;
	vr.look_pitch = pitch;
	vr.look_valid = 1;
}

/* The grenade in the free hand (the left one, or left-handed the right):
while its trigger is held the game keeps it in the hand
(halo_vr_grenade_held); as the trigger lets go, the fastest the hand moved
in its last moments is the throw (halo_vr_grenade_throw). */
static void grenade_update(void)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[free_hand()];
	unsigned long long now = now_us();
	int valid = hand->active && (hand->flags & HALO_XR_POSE_VALID);
	/* (at a wheel the free hand's trigger brakes: xinput_sdl.c) */
	int held = valid && vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN && !vr.wheel_hands &&
		hand->trigger > (vr.grenade_held ? 0.2f : 0.3f);
	int count = vr.grenade_sample_count;

	if (held && !vr.grenade_held)
		count = 0;
	if (valid && (held || vr.grenade_held))
	{
		if (count == GRENADE_SAMPLES)
		{
			memmove(vr.grenade_sample[0], vr.grenade_sample[1], sizeof(vr.grenade_sample[0]) * (GRENADE_SAMPLES - 1));
			memmove(&vr.grenade_sample_us[0], &vr.grenade_sample_us[1],
				sizeof(vr.grenade_sample_us[0]) * (GRENADE_SAMPLES - 1));
			count--;
		}
		memcpy(vr.grenade_sample[count], hand->grip.position, sizeof(vr.grenade_sample[count]));
		vr.grenade_sample_us[count] = now;
		count++;
	}
	vr.grenade_sample_count = count;
	if (vr.grenade_held && !held)
	{
		/* let go: the fastest the hand moved over its last 150 ms, each
		speed over two frames or more (a single frame's is noisy) */
		float best_speed = 0.0f, best_velocity[3] = { 0.0f, 0.0f, 0.0f };
		int best = -1, i;

		vr.grenade_released_us = now;
		for (i = 0; i < count; i++)
		{
			int j;

			if (now - vr.grenade_sample_us[i] > 150000)
				continue;
			for (j = i - 1; j >= 0; j--)
			{
				float seconds = (float)(vr.grenade_sample_us[i] - vr.grenade_sample_us[j]) * 1e-6f;

				if (seconds >= 0.02f)
				{
					float velocity[3], speed;
					int axis;

					if (seconds > 0.1f)
						break;
					for (axis = 0; axis < 3; axis++)
						velocity[axis] = (vr.grenade_sample[i][axis] - vr.grenade_sample[j][axis]) / seconds;
					speed = sqrtf(velocity[0] * velocity[0] + velocity[1] * velocity[1] + velocity[2] * velocity[2]);
					if (speed > best_speed)
					{
						best_speed = speed;
						memcpy(best_velocity, velocity, sizeof(best_velocity));
						best = i;
					}
					break;
				}
			}
		}
		vr.grenade_thrown = 0;
		if (vr.grenade_throw_speed > 0.0f && best >= 0 && best_speed >= vr.grenade_throw_speed)
		{
			int axis;

			vr.grenade_thrown = 1;
			memcpy(vr.grenade_hand, vr.grenade_sample[count - 1], sizeof(vr.grenade_hand));
			for (axis = 0; axis < 3; axis++)
				vr.grenade_direction[axis] = best_velocity[axis] / best_speed;
			/* the throw's speed is its strength: full at 5 m/s */
			vr.grenade_strength = best_speed / 5.0f;
			if (vr.grenade_strength < 0.25f)
				vr.grenade_strength = 0.25f;
			if (vr.grenade_strength > 1.0f)
				vr.grenade_strength = 1.0f;
			host_xr_haptic(free_hand(), 0.6f, 0.06f);
			platform_log("VR: grenade thrown at %.1f m/s", best_speed);
		}
		vr.grenade_sample_count = 0;
	}
	vr.grenade_held = held;
}

int halo_vr_grenade_held(short local_player_index)
{
	return vr.active && vr.aim_with_controller && local_player_index == 0 && vr.grenade_held;
}

int halo_vr_grenade_in_hand(short local_player_index, float position[3], float forward[3], float up[3],
	float *scale)
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const struct halo_xr_hand *hand = &vr.frame.hands[free_hand()];
	float held[3], offset[3], direction[3], grip_offset[3];
	int axis;

	if (!vr.aim_with_controller || local_player_index != 0 || !halo_vr_stereo_frame() || !hand->active ||
		!(hand->flags & HALO_XR_POSE_VALID))
	{
		return 0;
	}
	/* the grip's +x faces out of the left palm, into the right one; the
	drawn hand's knuckles run along the aim's y, and its fingers along the
	aim's -z (first_person_weapons.c, vr_left_hand_free) */
	memcpy(grip_offset, vr.grenade_offset, sizeof(grip_offset));
	if (free_hand() == HALO_XR_HAND_RIGHT)
		grip_offset[0] = -grip_offset[0];
	{
		float knuckles[3], fingers[3];

		quaternion_rotate(hand->grip.orientation, grip_offset, held);
		quaternion_rotate(hand->aim.orientation, local_up, knuckles);
		quaternion_rotate(hand->aim.orientation, local_forward, fingers);
		for (axis = 0; axis < 3; axis++)
			held[axis] += hand->grip.position[axis] - knuckles[axis] * vr.grenade_down -
				fingers[axis] * vr.grenade_back;
	}
	tracked_offset(held, offset);
	for (axis = 0; axis < 3; axis++)
		position[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	quaternion_rotate(hand->grip.orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(hand->grip.orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);
	*scale = vr.grenade_scale;
	return 1;
}

int halo_vr_grenade_throw(short local_player_index, float origin[3], float direction[3], float *strength)
{
	float offset[3];
	int axis;

	if (!vr.active || !vr.aim_with_controller || local_player_index != 0 || !vr.grenade_thrown ||
		now_us() - vr.grenade_released_us > 1000000)
	{
		vr.grenade_thrown = 0;
		return 0;
	}
	vr.grenade_thrown = 0;
	tracked_offset(vr.grenade_hand, offset);
	for (axis = 0; axis < 3; axis++)
		origin[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	local_to_game(vr.grenade_direction, vr.body_yaw, direction);
	*strength = vr.grenade_strength;
	return 1;
}

/* a hand thrust forward fast enough is a melee attack; the game sees its
button for a moment (halo_vr_melee) */
static void melee_update(void)
{
	unsigned long long now = now_us();
	float seconds = vr.hand_last_us ? (float)(now - vr.hand_last_us) * 1e-6f : 0.0f;
	float look[3] = { 0.0f, 0.0f, -1.0f }, facing[3];
	int hand;

	vr.hand_last_us = now;
	{
		/* the melee button's attack (xinput_sdl.c; the weapon hand's grip
		unless vr.button_melee moves it) aims the same way */
		struct halo_xr_hand hands[HALO_XR_HAND_COUNT];
		int button_down;

		memcpy(hands, vr.frame.hands, sizeof(hands));
		controls_in_play(hands);
		button_down = (hands[HALO_XR_HAND_RIGHT].buttons & HALO_XR_BUTTON_SQUEEZE) != 0;
		if (button_down && !vr.melee_button_down)
		{
			vr.melee_aim_until_us = now + 700000;
			vr.melee_strength = 0.75f;
			vr.melee_jab_start_us = now;
		}
		vr.melee_button_down = button_down;
	}
	quaternion_rotate(vr.frame.head.orientation, look, facing);
	facing[1] = 0.0f;
	normalize3(facing);
	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
	{
		const struct halo_xr_hand *state = &vr.frame.hands[hand];

		if (!state->active || !(state->flags & HALO_XR_POSE_VALID))
		{
			vr.hand_last_valid[hand] = 0;
			continue;
		}
		/* (a grenade's throw is not a blow) */
		/* (nor is turning the wheel) */
		if (vr.hand_last_valid[hand] && seconds > 0.0f && seconds < 0.1f && vr.melee_swing_speed > 0.0f &&
			!vr.wheel_held[hand] &&
			vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN && now >= vr.melee_ready_us &&
			!(hand == free_hand() && (vr.grenade_held || now - vr.grenade_released_us < 500000)))
		{
			float velocity[3], forward_speed;
			int axis;

			for (axis = 0; axis < 3; axis++)
				velocity[axis] = (state->grip.position[axis] - vr.hand_last[hand][axis]) / seconds;
			forward_speed = velocity[0] * facing[0] + velocity[2] * facing[2];
			if (forward_speed > vr.melee_swing_speed)
			{
				vr.melee_until_us = now + 150000;
				vr.melee_ready_us = now + 700000;
				vr.melee_aim_until_us = now + 700000;
				/* the swing's speed is the blow's weight: half at 2 m/s, full at 4 */
				vr.melee_strength = forward_speed / 4.0f;
				if (vr.melee_strength < 0.5f)
					vr.melee_strength = 0.5f;
				if (vr.melee_strength > 1.0f)
					vr.melee_strength = 1.0f;
				host_xr_haptic(hand, 0.8f, 0.08f);
				platform_log("VR: melee, %s hand at %.1f m/s", hand ? "right" : "left", forward_speed);
			}
		}
		memcpy(vr.hand_last[hand], state->grip.position, sizeof(vr.hand_last[hand]));
		vr.hand_last_valid[hand] = 1;
	}
}

/* The fingers, from where they rest on the controller (after
HaloCE-Quest-VR, CC0, its vr_frame.c): the thumb curled while it touches
the stick, a button or the rest, half curled where the controller does not
tell; the index finger with the trigger, a little curled on it; the others
with the grip; holding a grenade, the hand closes round it. Each eased
towards that by a critically damped spring (no overshoot however the
frames' lengths vary). */
static void fingers_update(void)
{
	unsigned long long now = now_us();
	float seconds = vr.fingers_last_us ? (float)(now - vr.fingers_last_us) * 1e-6f : 0.0f;
	int hand;

	vr.fingers_last_us = now;
	if (seconds > 0.05f)
		seconds = 0.05f;
	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
	{
		const struct halo_xr_hand *state = &vr.frame.hands[hand];
		float target[4];
		int finger;

		if (!(state->buttons & HALO_XR_BUTTON_TOUCH_KNOWN))
			target[0] = 0.4f;
		else
			target[0] = (state->buttons & HALO_XR_BUTTON_THUMB_TOUCH) ? 0.75f : 0.0f;
		target[1] = fmaxf(state->trigger, (state->buttons & HALO_XR_BUTTON_INDEX_TOUCH) ? 0.22f : 0.0f);
		target[2] = target[3] = state->squeeze;
		if (hand == free_hand() && vr.grenade_held)
		{
			for (finger = 0; finger < 4; finger++)
				target[finger] = 0.8f;
		}
		for (finger = 0; finger < 4; finger++)
		{
			float omega = finger == 0 ? 20.0f : 26.0f;
			float x = vr.finger_curl[hand][finger] - target[finger];
			float v = vr.finger_velocity[hand][finger];
			float j = v + omega * x, decay = expf(-omega * seconds);

			vr.finger_curl[hand][finger] = fmaxf(0.0f, fminf(1.0f, target[finger] + (x + j * seconds) * decay));
			vr.finger_velocity[hand][finger] = (v - omega * j * seconds) * decay;
		}
	}
}

/* The torso's heading (after HaloCE-Quest-VR, CC0, its vr_body_heading):
the head's, steady at any pitch (looking down, the top of the head points
forward; up, the back of it), drawn halfway towards the hands when both
are ahead of it, by at most 45 degrees (turning the body turns both,
looking about only the head). The torso follows it beyond a 15 degree cone,
the turn eased (10 a second, whatever the frame rate). Kept in LOCAL
space, which the stick's turns carry (body_yaw): the shoulders turn with
them at once. */
#define TORSO_CONE (15.0f * PI / 180.0f)

static void torso_update(void)
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const struct halo_xr_hand *hands = vr.frame.hands;
	unsigned long long now = now_us();
	float seconds = vr.torso_valid ? (float)(now - vr.torso_last_us) * 1e-6f : -1.0f;
	float forward[3], up[3], level[3], wanted;
	int axis;

	vr.torso_last_us = now;
	if (!(vr.frame.head_flags & HALO_XR_POSE_VALID))
	{
		vr.torso_valid = 0;
		return;
	}
	quaternion_rotate(vr.frame.head.orientation, local_forward, forward);
	quaternion_rotate(vr.frame.head.orientation, local_up, up);
	for (axis = 0; axis < 3; axis++)
		level[axis] = forward[axis] - forward[1] * up[axis];
	level[1] = 0.0f;
	if (normalize3(level) > 1e-3f)
	{
		wanted = atan2f(-level[0], -level[2]);
	}
	else
	{
		float pitch;

		head_angles(&wanted, &pitch);
	}
	vr.head_level_yaw = wanted;
	if (hands[0].active && (hands[0].flags & HALO_XR_POSE_VALID) && hands[1].active &&
		(hands[1].flags & HALO_XR_POSE_VALID))
	{
		float x = (hands[0].grip.position[0] + hands[1].grip.position[0]) * 0.5f - vr.frame.head.position[0];
		float z = (hands[0].grip.position[2] + hands[1].grip.position[2]) * 0.5f - vr.frame.head.position[2];

		if (sqrtf(x * x + z * z) > 0.12f)
		{
			float difference = wrap_angle(atan2f(-x, -z) - wanted);

			if (fabsf(difference) < 1.745f)
				wanted += 0.5f * fmaxf(-0.785f, fminf(0.785f, difference));
		}
	}
	if (!vr.torso_valid || seconds < 0.0f || seconds > 0.25f || vr.recentre_frames > 0)
	{
		vr.torso_yaw = wrap_angle(wanted);
	}
	else
	{
		float difference = wrap_angle(wanted - vr.torso_yaw);

		if (fabsf(difference) > TORSO_CONE)
		{
			float turn = difference - copysignf(TORSO_CONE, difference);

			vr.torso_yaw = wrap_angle(vr.torso_yaw + turn * (1.0f - expf(-10.0f * fminf(seconds, 0.05f))));
		}
	}
	vr.torso_valid = 1;
}

int halo_vr_free_arm_bends(void)
{
	return vr.active && vr.arm_ik;
}

int halo_vr_body_mode(void)
{
	return vr.active ? vr.body_mode : HALO_VR_BODY_ARMS;
}

double halo_vr_pose_time(void)
{
	return (double)vr.frame_start_us * 1e-6;
}

int halo_vr_finger_curls(short local_player_index, int hand, float curls[4])
{
	const struct halo_xr_hand *state;

	if (!vr.active || !vr.fingers || local_player_index != 0 || hand < 0 || hand >= HALO_XR_HAND_COUNT)
		return 0;
	/* (the model's hands: left-handed, its left is the right controller's) */
	if (vr.left_handed)
		hand = HALO_XR_HAND_COUNT - 1 - hand;
	state = &vr.frame.hands[hand];
	if (!state->active || !(state->flags & HALO_XR_POSE_VALID))
		return 0;
	memcpy(curls, vr.finger_curl[hand], sizeof(vr.finger_curl[hand]));
	return HALO_VR_FINGERS_CURLED | ((state->buttons & HALO_XR_BUTTON_TOUCH_KNOWN) ? HALO_VR_FINGERS_TOUCH : 0);
}

int halo_vr_melee(void)
{
	return vr.active && now_us() < vr.melee_until_us;
}

float halo_vr_melee_strength(void)
{
	return vr.active && now_us() < vr.melee_aim_until_us ? vr.melee_strength : 0.0f;
}

static void turn_update(void)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[turn_hand()];
	unsigned long long now = SDL_GetTicks();
	float seconds = vr.last_frame_ms ? (float)(now - vr.last_frame_ms) * 0.001f : 0.0f;
	float x = hand->active ? hand->thumbstick[0] : 0.0f;

	vr.last_frame_ms = now;
	if (seconds > 0.1f)
		seconds = 0.1f;
	/* no turning in the menus, where the stick moves the cursor */
	if (vr.last_overlay_mode == HALO_XR_OVERLAY_SCREEN || !vr.frame.focused)
	{
		vr.snap_latched = 1;
		return;
	}
	/* driving, the stick steers instead (halo_vr_aim) */
	if (vr.seat_mode == HALO_VR_SEAT_STEER)
	{
		vr.snap_latched = 1;
		seat_body();
		return;
	}
	{
		/* seated, the body turns in the seat */
		float *turned = vr.seat_mode != HALO_VR_SEAT_NONE ? &vr.seat_turn : &vr.body_yaw;

		if (vr.smooth_turn)
		{
			if (fabsf(x) > 0.15f)
				*turned -= x * vr.smooth_turn_degrees_per_second * PI / 180.0f * seconds;
		}
		else if (!vr.snap_latched && fabsf(x) > 0.7f)
		{
			*turned -= (x > 0.0f ? 1.0f : -1.0f) * vr.snap_turn_degrees * PI / 180.0f;
			vr.snap_latched = 1;
		}
		else if (fabsf(x) < 0.3f)
		{
			vr.snap_latched = 0;
		}
		*turned = wrap_angle(*turned);
	}
	seat_body();
}

/* At an open vehicle's wheel (a Warthog's, vr.vehicle_wheel), a hand that
grips near it takes hold of it, and the hands turn it about its column: both
by the line between them, one about the wheel's middle. Where it is, the
driver's hands as drawn hold it (halo_vr_set_wheel); its column runs from
there back towards the driver, tipped up WHEEL_COLUMN_TILT. Held, its turn
steers (halo_vr_aim); let go, the turning stick does. */
#define WHEEL_REACH 0.35f                       /* metres from its middle */
#define WHEEL_COLUMN_TILT (45.0f * PI / 180.0f) /* from the horizontal */

static void wheel_release(void)
{
	int hand;

	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
		vr.wheel_held[hand] = 0;
	vr.wheel_hands = 0;
}

static void wheel_update(void)
{
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	float world[3], centre[3], forward_game[3], forward[3], column[3], up[3], side[3], line[3];
	float heading = seat_heading(), along, angle;
	int hand, axis, hands = 0;

	if (vr.seat_mode != HALO_VR_SEAT_STEER || !vr.vehicle_wheel || !vr.wheel_valid || !vr.frame.focused ||
		vr.last_overlay_mode == HALO_XR_OVERLAY_SCREEN)
	{
		wheel_release();
		return;
	}
	for (axis = 0; axis < 3; axis++)
	{
		world[axis] = vr.game_camera[axis] +
			(vr.wheel_hands_drawn[0][axis] + vr.wheel_hands_drawn[1][axis]) * 0.5f;
	}
	world_to_local(world, centre);
	if (!vr.wheel_logged)
	{
		float apart[3];

		for (axis = 0; axis < 3; axis++)
			apart[axis] = (vr.wheel_hands_drawn[0][axis] - vr.wheel_hands_drawn[1][axis]) * METRES_PER_WORLD_UNIT;
		platform_log("VR: wheel at %.2f %.2f %.2f (LOCAL), the hands %.2f m apart", centre[0], centre[1], centre[2],
			sqrtf(apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2]));
		vr.wheel_logged = 1;
	}
	/* its column, and the wheel's up and left about it */
	forward_game[0] = cosf(heading);
	forward_game[1] = sinf(heading);
	forward_game[2] = 0.0f;
	game_to_local(forward_game, forward);
	for (axis = 0; axis < 3; axis++)
		column[axis] = -forward[axis] * cosf(WHEEL_COLUMN_TILT) + local_up[axis] * sinf(WHEEL_COLUMN_TILT);
	along = local_up[0] * column[0] + local_up[1] * column[1] + local_up[2] * column[2];
	for (axis = 0; axis < 3; axis++)
		up[axis] = local_up[axis] - column[axis] * along;
	normalize3(up);
	cross3(column, up, side);
	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
	{
		const struct halo_xr_hand *state = &vr.frame.hands[hand];
		float apart[3];

		if (!state->active || !(state->flags & HALO_XR_POSE_VALID) || state->squeeze < 0.35f)
		{
			vr.wheel_held[hand] = 0;
			continue;
		}
		if (!vr.wheel_held[hand])
		{
			for (axis = 0; axis < 3; axis++)
				apart[axis] = state->grip.position[axis] - centre[axis];
			if (state->squeeze < 0.6f ||
				sqrtf(apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2]) > WHEEL_REACH)
			{
				continue;
			}
			vr.wheel_held[hand] = 1;
			host_xr_haptic(hand, 0.3f, 0.05f);
		}
		hands |= 1 << hand;
	}
	if (!hands)
	{
		vr.wheel_hands = 0;
		return;
	}
	/* the hands' turn about the column */
	for (axis = 0; axis < 3; axis++)
	{
		line[axis] = hands == ((1 << HALO_XR_HAND_LEFT) | (1 << HALO_XR_HAND_RIGHT)) ?
			vr.frame.hands[HALO_XR_HAND_LEFT].grip.position[axis] -
				vr.frame.hands[HALO_XR_HAND_RIGHT].grip.position[axis] :
			vr.frame.hands[hands & (1 << HALO_XR_HAND_LEFT) ? HALO_XR_HAND_LEFT : HALO_XR_HAND_RIGHT]
				.grip.position[axis] - centre[axis];
	}
	angle = atan2f(line[0] * side[0] + line[1] * side[1] + line[2] * side[2],
		line[0] * up[0] + line[1] * up[1] + line[2] * up[2]);
	if (!vr.wheel_hands)
	{
		/* taken hold of: turned as far as it steers now */
		vr.wheel_turn = -vr.steer[0] * vr.vehicle_wheel_angle;
	}
	else if (hands == vr.wheel_hands)
	{
		vr.wheel_turn += wrap_angle(angle - vr.wheel_reference);
	}
	if (vr.wheel_turn > vr.vehicle_wheel_angle)
		vr.wheel_turn = vr.vehicle_wheel_angle;
	if (vr.wheel_turn < -vr.vehicle_wheel_angle)
		vr.wheel_turn = -vr.vehicle_wheel_angle;
	vr.wheel_reference = angle;
	vr.wheel_hands = hands;
}

void halo_vr_set_wheel(short local_player_index, const float left[3], const float right[3])
{
	if (!vr.active || local_player_index != 0)
		return;
	vr.wheel_valid = left && right;
	if (!vr.wheel_valid)
		return;
	memcpy(vr.wheel_hands_drawn[0], left, sizeof(vr.wheel_hands_drawn[0]));
	memcpy(vr.wheel_hands_drawn[1], right, sizeof(vr.wheel_hands_drawn[1]));
}

int halo_vr_wheel_held(void)
{
	return vr.active && vr.wheel_hands != 0;
}

/* The comfort vignette: while a vehicle turns the player (from 15 degrees a
second, fully at 60), the eye images darken from their edges; on foot
(vr.foot_vignette), while the turning stick turns smoothly and, with
"moving", the move stick moves, as far as they are pushed (fully at three
quarters). It comes in quickly and goes slowly. */
static float foot_vignette_target(void)
{
	const struct halo_xr_hand *turn = &vr.frame.hands[turn_hand()];
	const struct halo_xr_hand *move = &vr.frame.hands[turn_hand() == HALO_XR_HAND_RIGHT ? HALO_XR_HAND_LEFT :
		HALO_XR_HAND_RIGHT];
	float push = 0.0f;

	if (vr.foot_vignette == FOOT_VIGNETTE_OFF || vr.foot_vignette_strength <= 0.0f || vr.seated ||
		!vr.frame.focused || vr.last_overlay_mode == HALO_XR_OVERLAY_SCREEN)
	{
		return 0.0f;
	}
	if (vr.smooth_turn && turn->active)
		push = fabsf(turn->thumbstick[0]);
	if (vr.foot_vignette == FOOT_VIGNETTE_MOVING && move->active)
	{
		float moving = sqrtf(move->thumbstick[0] * move->thumbstick[0] + move->thumbstick[1] * move->thumbstick[1]);

		if (moving > push)
			push = moving;
	}
	/* (beyond the sticks' dead zone, 0.15) */
	push = (push - 0.15f) / 0.6f;
	if (push < 0.0f)
		push = 0.0f;
	if (push > 1.0f)
		push = 1.0f;
	return push * (vr.foot_vignette_strength > 1.0f ? 1.0f : vr.foot_vignette_strength);
}

static void vignette_update(void)
{
	unsigned long long now = now_us();
	float seconds = vr.vignette_last_us ? (float)(now - vr.vignette_last_us) * 1e-6f : 0.0f;
	float target = foot_vignette_target(), rate;

	vr.vignette_last_us = now;
	if (seconds > 0.1f)
		seconds = 0.1f;
	if (vr.seat_mode != HALO_VR_SEAT_NONE && vr.comfort_vignette > 0.0f)
	{
		float yaw = seat_heading();

		if (vr.vignette_yaw_valid && seconds > 0.0f)
		{
			float turning = fabsf(wrap_angle(yaw - vr.vignette_last_yaw)) / seconds;
			float vehicle = (turning - 15.0f * PI / 180.0f) / (45.0f * PI / 180.0f);

			if (vehicle < 0.0f)
				vehicle = 0.0f;
			if (vehicle > 1.0f)
				vehicle = 1.0f;
			vehicle *= vr.comfort_vignette;
			if (vehicle > target)
				target = vehicle;
		}
		vr.vignette_last_yaw = yaw;
		vr.vignette_yaw_valid = 1;
	}
	else
	{
		vr.vignette_yaw_valid = 0;
	}
	rate = (target > vr.vignette ? 6.0f : 1.5f) * seconds;
	if (vr.vignette < target)
		vr.vignette = vr.vignette + rate < target ? vr.vignette + rate : target;
	else
		vr.vignette = vr.vignette - rate > target ? vr.vignette - rate : target;
	host_xr_set_vignette(vr.vignette);
}

void halo_vr_set_seat(short local_player_index, int seat, int armed, float vehicle_yaw)
{
	if (!vr.active || local_player_index != 0)
		return;
	vr.seated = seat != HALO_VR_SEAT_NONE;
	if (!vr.vehicle_first_person)
		seat = HALO_VR_SEAT_NONE;
	vr.seat_yaw = vehicle_yaw;
	vr.seat_armed = armed;
	if (seat == vr.seat_mode)
		return;
	platform_log("VR: seat %d (was %d)", seat, vr.seat_mode);
	if (vr.seat_mode == HALO_VR_SEAT_NONE)
	{
		/* sat down: facing the vehicle's forward where the head looks */
		vr.seat_mode = seat;
		vr.seat_frame_valid = 0;
		seat_face_forward();
		return;
	}
	vr.seat_mode = seat;
	seat_body();
}

void halo_vr_set_seat_frame(short local_player_index, float vehicle_yaw)
{
	if (!vr.active || local_player_index != 0 || vr.seat_mode == HALO_VR_SEAT_NONE)
		return;
	vr.seat_frame_yaw = vehicle_yaw;
	vr.seat_frame_valid = 1;
	seat_body();
}

/* both sticks held in for a second and a half: a vehicle, once */
static void vehicle_spawn_update(void)
{
	int held = vr.vehicle_spawn && vr.frame.focused && vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN &&
		(vr.frame.hands[HALO_XR_HAND_LEFT].buttons & HALO_XR_BUTTON_THUMBSTICK) &&
		(vr.frame.hands[HALO_XR_HAND_RIGHT].buttons & HALO_XR_BUTTON_THUMBSTICK);
	unsigned long long now = now_us();

	if (!held)
	{
		vr.vehicle_spawn_held_us = 0;
		vr.vehicle_spawn_latched = 0;
		return;
	}
	if (!vr.vehicle_spawn_held_us)
		vr.vehicle_spawn_held_us = now;
	if (!vr.vehicle_spawn_latched && now - vr.vehicle_spawn_held_us > 1500000)
	{
		vr.vehicle_spawn_asked = 1;
		vr.vehicle_spawn_latched = 1;
		host_xr_haptic(HALO_XR_HAND_LEFT, 0.5f, 0.1f);
		host_xr_haptic(HALO_XR_HAND_RIGHT, 0.5f, 0.1f);
	}
}

int halo_vr_take_vehicle_spawn(short local_player_index)
{
	if (!vr.active || local_player_index != 0 || !vr.vehicle_spawn_asked)
		return 0;
	vr.vehicle_spawn_asked = 0;
	return 1;
}

int halo_vr_vehicle_first_person(void)
{
	return vr.active && vr.vehicle_first_person;
}

float halo_vr_glass_strength(void)
{
	return vr.active ? vr.glass_strength : 1.0f;
}

float halo_vr_enclosed_seat_raise(int hull)
{
	return hull > HALO_VR_HULL_OPEN && hull <= HALO_VR_HULL_TANK ?
		vr.enclosed_seat_raise[hull] / METRES_PER_WORLD_UNIT : 0.0f;
}

float halo_vr_driver_camera_forward(void)
{
	return vr.driver_camera_forward / METRES_PER_WORLD_UNIT;
}

float halo_vr_gunner_camera_back(void)
{
	return vr.gunner_camera_back / METRES_PER_WORLD_UNIT;
}

/* seated at a gun or a window, where the weapon hand's controller aims it:
at a gun's handles (vr.turret_handles), the barrel rises as the hand tips
down */
static void seat_aim_direction(float direction[3])
{
	float up[3], right[3];

	controller_axes(direction, up, right);
	if ((vr.seat_armed & HALO_VR_SEAT_TURRET) && vr.turret_handles)
		direction[2] = -direction[2];
}

/* seated, where the aim runs from (world units) and along: from the eyes,
where the weapon hand's controller points, or driving, the way the stick
steers */
static int seat_aim(float start[3], float direction[3])
{
	int axis;

	float offset[3];

	if (vr.seat_mode == HALO_VR_SEAT_NONE)
		return 0;
	/* from the eyes, as the game aims a vehicle's gun (aim_assist.c) */
	tracked_offset(vr.frame.head.position, offset);
	for (axis = 0; axis < 3; axis++)
		start[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	if (vr.seat_mode == HALO_VR_SEAT_AIM && weapon_hand_tracked(0))
	{
		seat_aim_direction(direction);
		return 1;
	}
	direction[0] = cosf(vr.last_pitch) * cosf(vr.last_yaw);
	direction[1] = cosf(vr.last_pitch) * sinf(vr.last_yaw);
	direction[2] = sinf(vr.last_pitch);
	return 1;
}

void halo_vr_frame_begin(void)
{
	unsigned long long waited;

	if (!vr.active || vr.frame_begun)
		return;
	waited = now_us();
	if (!host_xr_begin_frame(&vr.frame))
		memset(&vr.frame, 0, sizeof(vr.frame));
	if (vr.frame.focused)
	{
		memcpy(vr.focused_hands, vr.frame.hands, sizeof(vr.focused_hands));
		vr.focused_hands_valid = 1;
	}
	else if (vr.focused_hands_valid && vr.frame.session_running)
	{
		/* the system menu is up: the hands stay where they were, their
		buttons let go (the weapon does not fall back to the head) */
		int hand;

		memcpy(vr.frame.hands, vr.focused_hands, sizeof(vr.frame.hands));
		for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
		{
			vr.frame.hands[hand].trigger = vr.frame.hands[hand].squeeze = 0.0f;
			vr.frame.hands[hand].thumbstick[0] = vr.frame.hands[hand].thumbstick[1] = 0.0f;
			vr.frame.hands[hand].buttons = 0;
		}
	}
	vr.frame_start_us = now_us();
	vr.wait_us += vr.frame_start_us - waited;
	vr.frame_begun = 1;
	vr.frame_number++;
	vr.eyes_captured = 0;
	vr.scope_captured = 0;
	vr.crosshair_captured = 0;
	vr.name_count = 0;
	vr.names_captured = 0;
	vr.overlay_mode = HALO_XR_OVERLAY_SCREEN;
	vr.overlay_blend = 0;
	if (vr.frame.recentred)
	{
		/* the headset recentred (the Meta button held): its LOCAL space
		moves over the next few frames */
		vr.recentre_frames = 10;
	}
	if (vr.recentre_frames > 0)
	{
		vr.recentre_frames--;
		vr_recentre();
	}
	if (vr.settings_read_at != config_changes())
		vr_read_live_settings();
	turn_update();
	wheel_update();
	vignette_update();
	vehicle_spawn_update();
	look_update();
	grenade_update();
	melee_update();
	fingers_update();
	torso_update();
}

void halo_vr_present(unsigned int back_buffer_texture)
{
	struct halo_xr_submit submit;

	if (!vr.active)
		return;
	halo_vr_frame_begin();
	memset(&submit, 0, sizeof(submit));
	submit.eyes = vr.eyes_captured == 3;
	if (!submit.eyes)
	{
		/* nothing drawn per eye: the whole picture goes on the screen */
		vr.overlay_mode = HALO_XR_OVERLAY_SCREEN;
		vr.overlay_blend = 0;
	}
	submit.overlay_mode = vr.overlay_mode;
	submit.overlay_blend = vr.overlay_blend;
	if (vr.overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED)
	{
		submit.overlay_distance = vr.hud_distance;
		submit.overlay_width = vr.hud_width;
	}
	else
	{
		submit.overlay_distance = vr.screen_distance;
		submit.overlay_width = vr.screen_width;
		submit.screen.orientation[3] = 1.0f;
		submit.screen.position[2] = -vr.screen_distance;
	}
	if (vr.overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED && vr.motion_tracker_on_wrist && vr.aim_with_controller)
	{
		/* the motion tracker to the wrist, and out of the HUD */
		submit.wrist = vr_copied(host_xr_copy_layer_region(HALO_XR_LAYER_WRIST, back_buffer_texture,
			HALO_XR_COPY_ALPHA, WRIST_REGION_LEFT, WRIST_REGION_TOP, WRIST_REGION_RIGHT, WRIST_REGION_BOTTOM));
		submit.wrist_size = vr.motion_tracker_size;
		submit.wrist_turn = vr.motion_tracker_turn;
		submit.wrist_right = vr.motion_tracker_right;
		submit.wrist_down = vr.motion_tracker_down;
		submit.wrist_hand = free_hand();
		d3d8_clear_back_buffer_region(WRIST_REGION_LEFT, WRIST_REGION_TOP, WRIST_REGION_RIGHT, WRIST_REGION_BOTTOM);
	}
	/* the crosshair, drawn apart (halo_vr_capture_crosshair), to where the
	weapon aims */
	if (vr.crosshair_captured && vr.overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED)
		submit.crosshair = crosshair_pose(&submit.crosshair_pose, &submit.crosshair_size);
	/* the names, each over its player's head */
	if (vr.names_captured && submit.eyes && vr.overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED)
	{
		int name;

		for (name = 0; name < vr.name_count; name++)
		{
			if (name_pose(vr.name_points[name], &submit.name_poses[submit.name_count],
				&submit.name_widths[submit.name_count]))
			{
				submit.name_count++;
			}
		}
	}
	if (vr.scope_captured && submit.eyes)
	{
		submit.scope = 1;
		submit.scope_size = vr.scope_size;
		submit.scope_pose = vr.scope_pose;
	}
	vr_copied(host_xr_copy_layer(HALO_XR_LAYER_OVERLAY, back_buffer_texture,
		vr.overlay_blend ? HALO_XR_COPY_ALPHA : HALO_XR_COPY_OPAQUE));
	host_xr_end_frame(&submit);
	vr.last_overlay_mode = vr.overlay_mode;
	vr.last_frame_stereo = submit.eyes;
	vr.frame_begun = 0;
	refresh_governor();
	if (vr.log_performance)
		performance_report(submit.eyes);
}

/* ---------- the game's side */

int halo_vr_stereo_frame(void)
{
	return vr.active && vr.frame_begun && vr.frame.session_running && vr.frame.should_render &&
		(vr.frame.views[0].flags & HALO_XR_POSE_VALID) && (vr.frame.views[1].flags & HALO_XR_POSE_VALID);
}

int halo_vr_stereo_player(short local_player_index, int cinematic)
{
	int stereo = halo_vr_stereo_frame() && local_player_index == 0 && (!cinematic || vr.cinematics_3d);

	/* from the menus, a loading screen or a cinematic into play: the game's
	camera is where the head is now, and the player faces where it looks */
	if (stereo && !vr.last_frame_stereo)
		vr_recentre();
	return stereo;
}

void halo_vr_eye_camera(int eye, const float game_position[3],
	float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const struct halo_xr_view *view = &vr.frame.views[eye];
	float offset[3], direction[3];

	float half_tangent;
	int axis;

	if (eye == 0)
		vr.eyes_start_us = now_us();
	/* (this eye's picture foveated while it is drawn) */
	foveation_update();
	d3d8_foveation_eye(eye);

	tracked_offset(view->pose.position, offset);
	memcpy(vr.game_camera, game_position, sizeof(vr.game_camera));
	for (axis = 0; axis < 3; axis++)
		position[axis] = game_position[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	quaternion_rotate(view->pose.orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(view->pose.orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);

	/* The game's viewport is 640x480, stretched to the eye image: its
	projection maps the frustum bounds' [-1, 1] to the vertical field of
	view's tangent, times 4:3 across. The bounds put the eye's own
	(asymmetric) edges there. */
	half_tangent = (view->tangent_up - view->tangent_down) * 0.5f;
	*vertical_field_of_view = 2.0f * atanf(half_tangent);
	frustum_bounds[0] = view->tangent_left / (half_tangent * (4.0f / 3.0f));
	frustum_bounds[1] = view->tangent_right / (half_tangent * (4.0f / 3.0f));
	frustum_bounds[2] = view->tangent_down / half_tangent;
	frustum_bounds[3] = view->tangent_up / half_tangent;
}

/* ---------- foveation (vr.foveation)

The eyes' pictures drawn at full detail about each lens's centre, coarser
towards their edges (d3d8_gl.c, GL_QCOM_texture_foveated: the Quest's). The
centre is where the eye looks straight ahead, tangent 0: off the picture's
middle, as the eyes' views are asymmetric (wider outwards and downwards). In
the picture's normalized device coordinates, rows from its top (row 0 is
the top, -1), it lies at x = -(L + R) / (R - L), y = (U + D) / (U - D) for
the view's edges' tangents. The levels: full detail within sqrt(1 + area)
/ gain of it (0.69, 0.58 of the picture's half width), then halved and
halved again down to the least density. Off by default: on the Quest 3 the
eyes take 5.5-6.4 ms of GPU in a 15-bot game, the same with every level
(2026-10-12), and a stronger level ("high", taken out) drew a grey
rectangle below the player. */

static const struct
{
	const char *name;
	float gain, area, minimum_density;
} foveation_levels[] =
{
	{ "off", 0.0f, 0.0f, 1.0f },
	{ "low", 2.5f, 2.0f, 0.5f },
	{ "medium", 3.0f, 2.0f, 0.25f },
};

#define FOVEATION_LEVEL_COUNT (int)(sizeof(foveation_levels) / sizeof(foveation_levels[0]))

/* vr.foveation, as it changes (the VR options); "off" if unknown */
static int foveation_level(void)
{
	static unsigned long read_at = (unsigned long)-1;
	static int level = 0;

	if (read_at != config_changes())
	{
		const char *name = config_string("vr.foveation");
		int index;

		read_at = config_changes();
		level = 0;
		for (index = 0; index < FOVEATION_LEVEL_COUNT; index++)
		{
			if (name && !strcmp(name, foveation_levels[index].name))
				level = index;
		}
	}
	return level;
}

/* each eye's focal point and falloff, for this frame's eyes' targets */
static void foveation_update(void)
{
	int level = foveation_level();
	float focal[HALO_XR_EYE_COUNT][2];
	int eye;

	if (!d3d8_foveation_supported())
	{
		if (!vr.foveation_logged)
		{
			platform_log("VR: no foveation (the GPU has no GL_QCOM_texture_foveated)");
			vr.foveation_logged = 1;
		}
		return;
	}
	for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
	{
		const struct halo_xr_view *view = &vr.frame.views[eye];
		float width = view->tangent_right - view->tangent_left;
		float height = view->tangent_up - view->tangent_down;

		focal[eye][0] = width > 0.0f ? -(view->tangent_left + view->tangent_right) / width : 0.0f;
		focal[eye][1] = height > 0.0f ? (view->tangent_up + view->tangent_down) / height : 0.0f;
		d3d8_foveation_parameters(eye, focal[eye][0], focal[eye][1], foveation_levels[level].gain,
			foveation_levels[level].area, foveation_levels[level].minimum_density);
	}
	if (vr.foveation_logged != level + 1)
	{
		platform_log("VR: foveation %s (gain %.1f, area %.1f, least density %.3f); lens centres at "
			"(%.2f, %.2f) and (%.2f, %.2f)", foveation_levels[level].name, foveation_levels[level].gain,
			foveation_levels[level].area, foveation_levels[level].minimum_density, focal[0][0], focal[0][1],
			focal[1][0], focal[1][1]);
		vr.foveation_logged = level + 1;
	}
}

int halo_vr_foveation_supported(void)
{
	return vr.active && d3d8_foveation_supported();
}

/* the app's GPU time of a recent frame (ms; 0: the headset does not tell
it), and the headset's refresh rate (Hz; 0: unknown), for vr.graphics
"auto" (port/linux/game/vr_graphics.c) */
float halo_vr_gpu_milliseconds(void)
{
	return vr.active ? host_xr_gpu_time() : 0.0f;
}

float halo_vr_refresh_rate(void)
{
	return vr.active ? vr.info.refresh_rate : 0.0f;
}

/* ---------- both eyes at once (debug.multiview)

The scene is drawn once, from a camera between the eyes whose view takes in
both eyes' (with a margin, for what is near: the eyes stand apart), into two
layers; each layer's positions are moved from that camera's picture to its
eye's (d3d8_multiview_begin, nv2a_vsh.c). With d the depth along the view,
a point's picture across, x, is (2 tx - (L + R)) / (R - L) times d for the
tangent tx = X / d of the view's edges L, R; up, y, likewise, but for rows
from the top. An eye that stands t aside sees X - t: x' = a x + b d + c. */

#define MULTIVIEW_MARGIN 0.1f

static float multiview_bounds[4];     /* the shared view's L, R, D, U */
static int multiview_drawing;
/* the rows for what is drawn in the world, and for the sky, which is drawn
about the camera, as though infinitely far: no eye stands aside of it */
static float multiview_rows[2][4][4];

int halo_vr_multiview(void)
{
	int available = vr.active && vr.multiview && d3d8_multiview_supported();

	if (vr.active && vr.multiview && !vr.multiview_logged)
	{
		platform_log("VR: both eyes %s", available ? "in one draw (multiview)" : "drawn one by one (no multiview)");
		vr.multiview_logged = 1;
	}
	return available && halo_vr_stereo_frame();
}

void halo_vr_center_camera(const float game_position[3], float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	static const float local_right[3] = { 1.0f, 0.0f, 0.0f };
	const struct halo_xr_view *views = vr.frame.views;
	float centre[3], offset[3], direction[3], right_local[3], up_local[3], rows[4][4];
	float left, right, down, top, half_tangent;
	int axis, eye;

	vr.eyes_start_us = now_us();
	for (axis = 0; axis < 3; axis++)
		centre[axis] = (views[0].pose.position[axis] + views[1].pose.position[axis]) * 0.5f;
	tracked_offset(centre, offset);
	memcpy(vr.game_camera, game_position, sizeof(vr.game_camera));
	for (axis = 0; axis < 3; axis++)
		position[axis] = game_position[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	/* the eyes look alike (parallel views): the left one's way */
	quaternion_rotate(views[0].pose.orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(views[0].pose.orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);
	quaternion_rotate(views[0].pose.orientation, local_right, right_local);
	quaternion_rotate(views[0].pose.orientation, local_up, up_local);

	left = fminf(views[0].tangent_left, views[1].tangent_left) - MULTIVIEW_MARGIN;
	right = fmaxf(views[0].tangent_right, views[1].tangent_right) + MULTIVIEW_MARGIN;
	down = fminf(views[0].tangent_down, views[1].tangent_down) - MULTIVIEW_MARGIN;
	top = fmaxf(views[0].tangent_up, views[1].tangent_up) + MULTIVIEW_MARGIN;
	multiview_bounds[0] = left;
	multiview_bounds[1] = right;
	multiview_bounds[2] = down;
	multiview_bounds[3] = top;
	half_tangent = (top - down) * 0.5f;
	*vertical_field_of_view = 2.0f * atanf(half_tangent);
	frustum_bounds[0] = left / (half_tangent * (4.0f / 3.0f));
	frustum_bounds[1] = right / (half_tangent * (4.0f / 3.0f));
	frustum_bounds[2] = down / half_tangent;
	frustum_bounds[3] = top / half_tangent;

	for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
	{
		const struct halo_xr_view *view = &views[eye];
		float aside[3], tx, ty;
		float eye_width = view->tangent_right - view->tangent_left;
		float eye_height = view->tangent_up - view->tangent_down;

		/* the eye's place from the shared camera, in its view's axes (world
		units) */
		for (axis = 0; axis < 3; axis++)
			aside[axis] = view->pose.position[axis] - centre[axis];
		tx = (aside[0] * right_local[0] + aside[1] * right_local[1] + aside[2] * right_local[2]) /
			METRES_PER_WORLD_UNIT;
		ty = (aside[0] * up_local[0] + aside[1] * up_local[1] + aside[2] * up_local[2]) / METRES_PER_WORLD_UNIT;
		rows[eye * 2][0] = (right - left) / eye_width;
		rows[eye * 2][1] = (left + right - view->tangent_left - view->tangent_right) / eye_width;
		rows[eye * 2][2] = -2.0f * tx / eye_width;
		rows[eye * 2][3] = 0.0f;
		/* (rows from the top: y runs against the tangents) */
		rows[eye * 2 + 1][0] = (top - down) / eye_height;
		rows[eye * 2 + 1][1] = -(down + top - view->tangent_down - view->tangent_up) / eye_height;
		rows[eye * 2 + 1][2] = 2.0f * ty / eye_height;
		rows[eye * 2 + 1][3] = 0.0f;
	}
	memcpy(multiview_rows[0], rows, sizeof(rows));
	memcpy(multiview_rows[1], rows, sizeof(rows));
	for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
		multiview_rows[1][eye * 2][2] = multiview_rows[1][eye * 2 + 1][2] = 0.0f;
	if (!vr.multiview_logged_views)
	{
		/* (the correction assumes parallel views: say if they are not) */
		float other[3], apart[3];

		quaternion_rotate(views[1].pose.orientation, local_forward, other);
		quaternion_rotate(views[0].pose.orientation, local_forward, direction);
		for (axis = 0; axis < 3; axis++)
			apart[axis] = views[1].pose.position[axis] - views[0].pose.position[axis];
		platform_log("VR multiview: views %.2f degrees apart, eyes %.1f mm apart; shared view %.2f..%.2f, %.2f..%.2f",
			acosf(fminf(1.0f, direction[0] * other[0] + direction[1] * other[1] + direction[2] * other[2])) * 180.0f / PI,
			sqrtf(apart[0] * apart[0] + apart[1] * apart[1] + apart[2] * apart[2]) * 1000.0f, left, right, down, top);
		vr.multiview_logged_views = 1;
	}
	{
		/* each eye's picture's place in the shared one (rows from the top) */
		float resolve_rect[2][4];

		for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
		{
			resolve_rect[eye][0] = (views[eye].tangent_left - left) / (right - left);
			resolve_rect[eye][1] = (top - views[eye].tangent_up) / (top - down);
			resolve_rect[eye][2] = (views[eye].tangent_right - left) / (right - left);
			resolve_rect[eye][3] = (top - views[eye].tangent_down) / (top - down);
		}
		foveation_update();
		multiview_drawing = d3d8_multiview_begin(rows, resolve_rect);
	}
}

/* the shared camera's projection's depth (render.c: its [2][2] and [3][2]):
what the game projects itself carries z = -[2][2] + [3][2] / depth */
void halo_vr_multiview_projection(float projection_22, float projection_32)
{
	d3d8_multiview_depth(-projection_22, projection_32);
}

/* lens flares are being drawn (rasterizer_lights.c): each eye's by the
flare's depth, as the flare's test was, not at infinity (a weapon's own,
the plasma pistol's at its prongs, half a metre away: a ball each eye saw
elsewhere); the sun's are far enough for infinity either way */
void halo_vr_multiview_lens_flares(int drawing)
{
	d3d8_multiview_projected_depth(drawing && multiview_drawing);
}

/* the sky is being drawn (render.c): as though infinitely far */
void halo_vr_multiview_sky(int sky)
{
	if (multiview_drawing)
		d3d8_multiview_rows(multiview_rows[sky ? 1 : 0]);
}

/* after the shared draw: its layers become the eyes' images */
void halo_vr_capture_multiview(void)
{
	unsigned int layers = d3d8_multiview_end();

	if (!multiview_drawing)
		return;
	multiview_drawing = 0;
	if (vr_copied(host_xr_copy_layer_array(HALO_XR_LAYER_EYE_LEFT, layers, 0)))
		vr.eyes_captured |= 1;
	if (vr_copied(host_xr_copy_layer_array(HALO_XR_LAYER_EYE_RIGHT, layers, 1)))
		vr.eyes_captured |= 2;
	if (vr.eyes_start_us)
		vr.eyes_us += now_us() - vr.eyes_start_us;
}

void halo_vr_capture_eye(int eye)
{
	if (!vr.active || eye < 0 || eye >= HALO_XR_EYE_COUNT)
		return;
	if (vr_copied(host_xr_copy_layer(HALO_XR_LAYER_EYE_LEFT + eye, d3d8_back_buffer_texture(), 1)))
		vr.eyes_captured |= 1 << eye;
	/* (what is drawn into the back buffer next at full detail; after the
	copy, which ends the eye's drawing into it) */
	d3d8_foveation_eye(-1);
	if (eye == HALO_XR_EYE_COUNT - 1 && vr.eyes_start_us)
		vr.eyes_us += now_us() - vr.eyes_start_us;
}

void halo_vr_begin_overlay(int menus_active)
{
	if (!vr.active)
		return;
	d3d8_foveation_eye(-1);
	d3d8_clear_back_buffer(0.0f, 0.0f, 0.0f, 0.0f);
	vr.overlay_mode = menus_active ? HALO_XR_OVERLAY_SCREEN : HALO_XR_OVERLAY_HEAD_LOCKED;
	vr.overlay_blend = 1;
}

int halo_vr_aim(short local_player_index, float current_yaw, float current_pitch,
	float *yaw, float *pitch)
{
	float head_yaw, head_pitch;

	(void)current_pitch;
	if (!halo_vr_stereo_frame() || local_player_index != 0 || !(vr.frame.head_flags & HALO_XR_POSE_VALID))
	{
		vr.aim_synced = 0;
		return 0;
	}
	head_angles(&head_yaw, &head_pitch);
	if (vr.seat_mode == HALO_VR_SEAT_STEER)
	{
		/* driving: the right stick (mirrored, the left) steers off the
		vehicle's heading, and aims above or below the horizon (the body
		turns with the vehicle) */
		const struct halo_xr_hand *hand = &vr.frame.hands[turn_hand()];
		float stick[2];
		unsigned long long now = now_us();
		float seconds = vr.steer_last_us ? (float)(now - vr.steer_last_us) * 1e-6f : 0.0f;
		float blend;
		int axis;

		vr.steer_last_us = now;
		stick[0] = hand->active && vr.frame.focused ? hand->thumbstick[0] : 0.0f;
		stick[1] = hand->active && vr.frame.focused ? hand->thumbstick[1] : 0.0f;
		/* past the dead zone, along a curve (fine aim near the middle, all
		of it at the end), eased in (vr.vehicle_steer_curve, _smoothing) */
		blend = vr.vehicle_steer_smoothing > 0.0f && seconds < 0.2f ?
			1.0f - expf(-seconds / vr.vehicle_steer_smoothing) :
			1.0f;
		for (axis = 0; axis < 2; axis++)
		{
			float amount = (fabsf(stick[axis]) - 0.15f) / 0.85f;

			amount = amount > 0.0f ? powf(amount > 1.0f ? 1.0f : amount, vr.vehicle_steer_curve) : 0.0f;
			if (stick[axis] < 0.0f)
				amount = -amount;
			if (axis == 0 && vr.wheel_hands)
			{
				/* (the hands on the wheel steer, as they turn it: clockwise
				to the right) */
				vr.steer[0] = -vr.wheel_turn / vr.vehicle_wheel_angle;
				continue;
			}
			vr.steer[axis] += (amount - vr.steer[axis]) * blend;
		}
		/* (a Ghost or a Banshee aims its guns as it steers: less, finer) */
		*yaw = wrap_angle(vr.seat_yaw - vr.steer[0] *
			(vr.seat_armed ? vr.vehicle_gun_steer_angle : vr.vehicle_steer_angle));
		*pitch = vr.steer[1] * vr.vehicle_pitch_angle;
		vr.aim_synced = 1;
	}
	else if (vr.seat_mode == HALO_VR_SEAT_AIM && weapon_hand_tracked(local_player_index))
	{
		/* a gunner or passenger: where the weapon hand's controller points */
		float forward[3];

		seat_aim_direction(forward);
		*yaw = atan2f(forward[1], forward[0]);
		*pitch = asinf(forward[2] > 1.0f ? 1.0f : forward[2] < -1.0f ? -1.0f : forward[2]);
		vr.aim_synced = 1;
	}
	else
	{
		if (vr.seat_mode != HALO_VR_SEAT_NONE)
		{
			/* seated: the body is the vehicle's */
			vr.aim_synced = 1;
		}
		else if (!vr.aim_synced)
		{
			/* keep the way the game faces: the body turns to match the head */
			vr.body_yaw = wrap_angle(current_yaw - head_yaw);
			vr.aim_synced = 1;
		}
		else if (fabsf(wrap_angle(current_yaw - vr.last_yaw)) > FACING_JUMP)
		{
			vr.body_yaw = wrap_angle(vr.body_yaw + wrap_angle(current_yaw - vr.last_yaw));
		}
		*yaw = wrap_angle(vr.body_yaw + head_yaw);
		*pitch = head_pitch;
	}
	if (*yaw < 0.0f)
		*yaw += 2.0f * PI;
	vr.last_yaw = *yaw;
	vr.last_pitch = *pitch;
	return 1;
}

/* ---------- vector helpers */

static void cross3(const float a[3], const float b[3], float out[3])
{
	float result[3];

	result[0] = a[1] * b[2] - a[2] * b[1];
	result[1] = a[2] * b[0] - a[0] * b[2];
	result[2] = a[0] * b[1] - a[1] * b[0];
	memcpy(out, result, sizeof(result));
}

static float normalize3(float v[3])
{
	float length = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);

	if (length > 1e-6f)
	{
		v[0] /= length;
		v[1] /= length;
		v[2] /= length;
	}
	return length;
}

/* turns v by the least rotation that takes the unit vector from to to */
static void rotate_between(const float from[3], const float to[3], float v[3])
{
	float axis[3], turned[3], cosine, sine, along;
	int index;

	cross3(from, to, axis);
	sine = normalize3(axis);
	cosine = from[0] * to[0] + from[1] * to[1] + from[2] * to[2];
	if (sine < 1e-6f)
		return;
	cross3(axis, v, turned);
	along = axis[0] * v[0] + axis[1] * v[1] + axis[2] * v[2];
	for (index = 0; index < 3; index++)
		v[index] = v[index] * cosine + turned[index] * sine + axis[index] * along * (1.0f - cosine);
}

/* ---------- the weapon in the hand (vr.aim_with = "controller"): the right
one, or left-handed (vr.handedness) the left */

static int weapon_hand_tracked(short local_player_index)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[weapon_hand()];

	return vr.aim_with_controller && local_player_index == 0 && halo_vr_stereo_frame() &&
		hand->active && (hand->flags & HALO_XR_POSE_VALID);
}

/* Left-handed, the first-person model (first_person_weapons.c) is posed as
the right hand's is, in a mirror image of the room, "model space", and
drawn mirrored back across the same mirror (as HaloCE-Quest-VR mirrors the
whole model across the weapon, CC0): its right hand takes the left
controller, its left hand the right one, its arms the other shoulders. The
mirror is the upright plane through the weapon hand's grip, square to
LOCAL's x: any upright one would do (the model, posed about the vertical
the same, mirrors back the same), and this one leaves the weapon hand
where it is. Right-handed, model space is the room. */
static void model_local_point(float point[3])
{
	if (vr.left_handed)
		point[0] = 2.0f * vr.frame.hands[weapon_hand()].grip.position[0] - point[0];
}

static void model_local_direction(float direction[3])
{
	if (vr.left_handed)
		direction[0] = -direction[0];
}

/* the mirror in the game's world: a point on it (world units) and its
normal, LOCAL's x */
static void mirror_plane(float point[3], float normal[3])
{
	static const float local_x[3] = { 1.0f, 0.0f, 0.0f };
	float offset[3];
	int axis;

	tracked_offset(vr.frame.hands[weapon_hand()].grip.position, offset);
	for (axis = 0; axis < 3; axis++)
		point[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	local_to_game(local_x, vr.body_yaw, normal);
}

/* a point or a direction in the game's world, between the room and model
space (either way: the mirror is its own inverse) */
static void mirror_game_point(float point[3])
{
	float on[3], normal[3], along;
	int axis;

	if (!vr.left_handed)
		return;
	mirror_plane(on, normal);
	along = (point[0] - on[0]) * normal[0] + (point[1] - on[1]) * normal[1] + (point[2] - on[2]) * normal[2];
	for (axis = 0; axis < 3; axis++)
		point[axis] -= 2.0f * along * normal[axis];
}

static void mirror_game_direction(float direction[3])
{
	float on[3], normal[3], along;
	int axis;

	if (!vr.left_handed)
		return;
	mirror_plane(on, normal);
	along = direction[0] * normal[0] + direction[1] * normal[1] + direction[2] * normal[2];
	for (axis = 0; axis < 3; axis++)
		direction[axis] -= 2.0f * along * normal[axis];
}

int halo_vr_first_person_mirror(short local_player_index, float point[3], float normal[3])
{
	if (!vr.left_handed || !weapon_hand_tracked(local_player_index))
		return 0;
	mirror_plane(point, normal);
	return 1;
}

/* The weapon's frames, all in the game's axes:
- the controller's: where it points;
- the model's: the controller's turned so that the first-person model's
  barrel, pointed in towards the screen's centre, points where the
  controller does (barrel_direction);
- with both hands on it, both turned alike so that the line from the
  model's right hand to its left, in the model's frame, runs from the
  weapon hand's controller to the free one's.
The weapon aims along the controller's frame (so turned); the model hangs
from the model's frame. Turned in model space, both; the aim is then
mirrored back. */
static void hand_axes(int hand, int model, float forward[3], float up[3], float right[3])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const float *orientation = vr.frame.hands[hand].aim.orientation;
	float direction[3];

	quaternion_rotate(orientation, local_forward, direction);
	if (model)
		model_local_direction(direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(orientation, local_up, direction);
	if (model)
		model_local_direction(direction);
	local_to_game(direction, vr.body_yaw, up);
	/* the game's axes are right-handed with +z up: forward x up is right
	(in model space too: the mirrored controller's frame is a right hand's) */
	cross3(forward, up, right);
}

/* where the weapon hand's controller points */
static void controller_axes(float forward[3], float up[3], float right[3])
{
	hand_axes(weapon_hand(), 0, forward, up, right);
}

static void model_axes(const float forward[3], const float up[3], const float right[3],
	float model_forward[3], float model_up[3], float model_right[3])
{
	float barrel[3];
	int axis;

	memcpy(model_forward, forward, 3 * sizeof(float));
	memcpy(model_up, up, 3 * sizeof(float));
	memcpy(model_right, right, 3 * sizeof(float));
	if (!vr.barrel_direction_valid)
		return;
	for (axis = 0; axis < 3; axis++)
	{
		barrel[axis] = forward[axis] * vr.barrel_direction[0] - right[axis] * vr.barrel_direction[1] +
			up[axis] * vr.barrel_direction[2];
	}
	if (normalize3(barrel) > 0.0f)
	{
		rotate_between(barrel, forward, model_forward);
		rotate_between(barrel, forward, model_up);
		cross3(model_forward, model_up, model_right);
	}
}

/* with both hands on the weapon, the turn that brings the model's grips in
line with the hands (from, to; model space); 0 without */
static int two_hand_turn(const float model_forward[3], const float model_up[3], const float model_right[3],
	float from[3], float to[3])
{
	float free_grip[3], free_offset[3], weapon_offset[3];
	int axis;

	if (!vr.two_hand_gripping || !vr.two_hand_valid)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		from[axis] = model_forward[axis] * vr.two_hand_reference[0] + model_right[axis] * vr.two_hand_reference[1] +
			model_up[axis] * vr.two_hand_reference[2];
	}
	memcpy(free_grip, vr.frame.hands[free_hand()].grip.position, sizeof(free_grip));
	model_local_point(free_grip);
	tracked_offset(free_grip, free_offset);
	tracked_offset(vr.frame.hands[weapon_hand()].grip.position, weapon_offset);
	for (axis = 0; axis < 3; axis++)
		to[axis] = free_offset[axis] - weapon_offset[axis];
	return normalize3(from) > 0.0f && normalize3(to) > 0.0f;
}

static void turn_axes(const float from[3], const float to[3], float forward[3], float up[3], float right[3])
{
	rotate_between(from, to, forward);
	rotate_between(from, to, up);
	cross3(forward, up, right);
}

/* where the weapon aims (in the room) */
static void weapon_axes(float forward[3], float up[3], float right[3])
{
	float model_forward[3], model_up[3], model_right[3], from[3], to[3];

	hand_axes(weapon_hand(), 1, forward, up, right);
	model_axes(forward, up, right, model_forward, model_up, model_right);
	if (two_hand_turn(model_forward, model_up, model_right, from, to))
		turn_axes(from, to, forward, up, right);
	if (vr.left_handed)
	{
		mirror_game_direction(forward);
		mirror_game_direction(up);
		cross3(forward, up, right);
	}
}

/* how the weapon's model hangs (model space) */
static void weapon_model_axes(float forward[3], float up[3], float right[3])
{
	float controller_forward[3], controller_up[3], controller_right[3], from[3], to[3];

	hand_axes(weapon_hand(), 1, controller_forward, controller_up, controller_right);
	model_axes(controller_forward, controller_up, controller_right, forward, up, right);
	if (two_hand_turn(forward, up, right, from, to))
		turn_axes(from, to, forward, up, right);
}

/* the weapon hand's offset from the game's camera, in world units (the
same in model space) */
static void hand_offset(float out[3])
{
	int axis;

	tracked_offset(vr.frame.hands[weapon_hand()].grip.position, out);
	for (axis = 0; axis < 3; axis++)
		out[axis] /= METRES_PER_WORLD_UNIT;
}

int halo_vr_aim_vector(short local_player_index, float vector[3])
{
	float up[3], right[3];

	/* a punch lands where the player looks, not where the weapon points */
	if (vr.active && now_us() < vr.melee_aim_until_us && halo_vr_look_vector(local_player_index, vector))
		return 1;
	if (!weapon_hand_tracked(local_player_index))
		return 0;
	weapon_axes(vector, up, right);
	return 1;
}

void halo_vr_set_barrel(short local_player_index, const float world[3], const float direction[3])
{
	if (local_player_index != 0)
		return;
	/* (posed in model space: the barrel as drawn, in the room; its
	direction stays the model's, which model_axes turns) */
	memcpy(vr.barrel, world, sizeof(vr.barrel));
	mirror_game_point(vr.barrel);
	vr.barrel_frame = vr.frame_number;
	if (!vr.barrel_direction_valid || memcmp(vr.barrel_direction, direction, sizeof(vr.barrel_direction)))
	{
		float degrees = acosf(direction[0] > 1.0f ? 1.0f : direction[0]) * 180.0f / PI;

		if (!vr.barrel_direction_valid || fabsf(direction[0] - vr.barrel_direction[0]) > 1e-3f)
			platform_log("VR: this weapon's barrel points %.1f degrees off its camera; turned to aim", degrees);
		memcpy(vr.barrel_direction, direction, sizeof(vr.barrel_direction));
		vr.barrel_direction_valid = 1;
	}
}

/* the barrel was drawn this frame or the last */
static int barrel_known(void)
{
	return vr.barrel_frame && vr.frame_number - vr.barrel_frame <= 1;
}

int halo_vr_weapon_origin_offset(short local_player_index, float offset[3])
{
	float forward[3], up[3], right[3];
	int axis;

	if (!weapon_hand_tracked(local_player_index))
		return 0;
	weapon_axes(forward, up, right);
	/* along the weapon as drawn (each weapon's model sits differently in
	the hand), else along the controller */
	if (barrel_known())
	{
		for (axis = 0; axis < 3; axis++)
			offset[axis] = vr.barrel[axis] - vr.game_camera[axis];
	}
	else
	{
		hand_offset(offset);
	}
	for (axis = 0; axis < 3; axis++)
		offset[axis] += forward[axis] * vr.muzzle_distance / METRES_PER_WORLD_UNIT;
	return 1;
}

int halo_vr_weapon_origin(short local_player_index, float origin[3])
{
	float offset[3];
	int axis;

	if (!halo_vr_weapon_origin_offset(local_player_index, offset))
		return 0;
	for (axis = 0; axis < 3; axis++)
		origin[axis] = vr.game_camera[axis] + offset[axis];
	return 1;
}

/* The weapon's hand in its first-person model (halo_vr_weapon_wrist): each
weapon's animations hold the hand elsewhere from the model's camera, the
pistol's some 25 cm further forward than the assault rifle's, so a camera
at one fixed place from the hand (as before: WEAPON_FIXED_*) held the
pistol's hand 15 cm before the real one and the assault rifle's 8 cm aside.
The wrist as the weapon is held ready, eased, is kept for each weapon (a
few at once, the last ones held), and the camera put where it lands as the
free hand's does: WRIST_BEHIND_GRIP behind the controller's grip, along
its aim. */
#define WEAPON_WRISTS 8
#define WRIST_BEHIND_GRIP 0.07f
#define WEAPON_FIXED_FORWARD 0.18f
#define WEAPON_FIXED_RIGHT 0.10f
#define WEAPON_FIXED_DOWN 0.14f

static struct
{
	long graph_index;
	float wrist[3];
	unsigned long used;
} weapon_wrists[WEAPON_WRISTS];
static unsigned long weapon_wrists_used;

static float *weapon_wrist_for(long graph_index, int make)
{
	int index, oldest = 0;

	for (index = 0; index < WEAPON_WRISTS; index++)
	{
		if (weapon_wrists[index].used && weapon_wrists[index].graph_index == graph_index)
		{
			weapon_wrists[index].used = ++weapon_wrists_used;
			return weapon_wrists[index].wrist;
		}
		if (weapon_wrists[index].used < weapon_wrists[oldest].used)
			oldest = index;
	}
	if (!make)
		return NULL;
	weapon_wrists[oldest].graph_index = graph_index;
	weapon_wrists[oldest].used = 0;
	return NULL;
}

void halo_vr_weapon_wrist(long graph_index, const float wrist[3])
{
	float *kept = weapon_wrist_for(graph_index, 1);
	int index, axis;

	if (kept)
	{
		for (axis = 0; axis < 3; axis++)
			kept[axis] += (wrist[axis] - kept[axis]) * 0.1f;
		return;
	}
	for (index = 0; index < WEAPON_WRISTS; index++)
	{
		if (weapon_wrists[index].graph_index == graph_index && !weapon_wrists[index].used)
		{
			memcpy(weapon_wrists[index].wrist, wrist, sizeof(weapon_wrists[index].wrist));
			weapon_wrists[index].used = ++weapon_wrists_used;
			platform_log("VR: weapon hand found %.0f cm forward, %.0f right, %.0f up of its model's camera",
				wrist[0] * METRES_PER_WORLD_UNIT * 100.0f, wrist[1] * METRES_PER_WORLD_UNIT * 100.0f,
				wrist[2] * METRES_PER_WORLD_UNIT * 100.0f);
			return;
		}
	}
}

int halo_vr_weapon_camera(short local_player_index, long graph_index, float position[3], float forward[3],
	float up[3])
{
	float right[3], hand[3], from_hand[3];
	const float *wrist;
	int axis;

	if (!weapon_hand_tracked(local_player_index))
		return 0;
	weapon_model_axes(forward, up, right);
	hand_offset(hand);
	{
		/* the weapon hand's grip's melee attack, which it does not swing: the
		weapon jabs forward and back (the game's own animation is held
		off, first_person_weapons.c) */
		float seconds = vr.melee_jab_start_us ? (float)(now_us() - vr.melee_jab_start_us) * 1e-6f : 1.0f;

		if (seconds < 0.33f)
		{
			float jab = sinf(seconds / 0.33f * PI) * 0.25f / METRES_PER_WORLD_UNIT;

			for (axis = 0; axis < 3; axis++)
				hand[axis] += forward[axis] * jab;
		}
	}
	/* the first-person model hangs from a camera: put that camera where the
	model's hand lands on the real one (the camera from the hand: forward,
	right, up, world units), moved as vr.weapon_hand_* says */
	wrist = graph_index != -1 ? weapon_wrist_for(graph_index, 0) : NULL;
	if (wrist)
	{
		from_hand[0] = -WRIST_BEHIND_GRIP / METRES_PER_WORLD_UNIT - wrist[0];
		from_hand[1] = -wrist[1];
		from_hand[2] = -wrist[2];
	}
	else
	{
		from_hand[0] = -WEAPON_FIXED_FORWARD / METRES_PER_WORLD_UNIT;
		from_hand[1] = -WEAPON_FIXED_RIGHT / METRES_PER_WORLD_UNIT;
		from_hand[2] = WEAPON_FIXED_DOWN / METRES_PER_WORLD_UNIT;
	}
	from_hand[0] -= vr.weapon_adjust[0] / METRES_PER_WORLD_UNIT;
	from_hand[1] -= vr.weapon_adjust[1] / METRES_PER_WORLD_UNIT;
	from_hand[2] += vr.weapon_adjust[2] / METRES_PER_WORLD_UNIT;
	for (axis = 0; axis < 3; axis++)
	{
		position[axis] = vr.game_camera[axis] + hand[axis] +
			forward[axis] * from_hand[0] + right[axis] * from_hand[1] + up[axis] * from_hand[2];
	}
	/* (the scope's disc, placed for the old fixed place, moves with it) */
	vr.weapon_shift[0] = from_hand[0] * METRES_PER_WORLD_UNIT + WEAPON_FIXED_FORWARD;
	vr.weapon_shift[1] = from_hand[1] * METRES_PER_WORLD_UNIT + WEAPON_FIXED_RIGHT;
	vr.weapon_shift[2] = from_hand[2] * METRES_PER_WORLD_UNIT - WEAPON_FIXED_DOWN;
	return 1;
}

int halo_vr_laser(short local_player_index, float start[3], float direction[3])
{
	if (!vr.laser)
		return 0;
	if (local_player_index == 0 && vr.laser_frame == vr.frame_number)
	{
		memcpy(start, vr.laser_start, sizeof(vr.laser_start));
		memcpy(direction, vr.laser_direction, sizeof(vr.laser_direction));
		return 1;
	}
	return halo_vr_ready_laser(local_player_index, start, direction);
}

int halo_vr_ready_laser(short local_player_index, float start[3], float direction[3])
{
	float up[3], right[3], offset[3];
	int axis;

	/* seated, the vehicle's aim (and the crosshair's distance along it) */
	if (local_player_index == 0 && vr.active && seat_aim(start, direction))
		return 1;

	if (!halo_vr_weapon_origin_offset(local_player_index, offset))
		return 0;
	weapon_axes(direction, up, right);
	for (axis = 0; axis < 3; axis++)
		start[axis] = vr.game_camera[axis] + offset[axis];
	return 1;
}

int halo_vr_look_vector(short local_player_index, float vector[3])
{
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	float direction[3];

	if (local_player_index != 0 || !halo_vr_stereo_frame() || !(vr.frame.head_flags & HALO_XR_POSE_VALID))
		return 0;
	quaternion_rotate(vr.frame.head.orientation, forward, direction);
	local_to_game(direction, vr.body_yaw, vector);
	return 1;
}

void halo_vr_debug_look(short local_player_index, const float unit_looking[3], int in_vehicle)
{
	static unsigned long long last_us;
	float head[3] = { 0.0f, 0.0f, 0.0f };
	int valid;
	unsigned long long now = now_us();

	if (!vr.log_performance || local_player_index != 0 || now - last_us < 1000000)
		return;
	last_us = now;
	valid = halo_vr_look_vector(local_player_index, head);
	platform_log("VR look: head %s (%.2f %.2f %.2f), unit looking (%.2f %.2f %.2f)%s", valid ? "valid" : "INVALID",
		head[0], head[1], head[2], unit_looking[0], unit_looking[1], unit_looking[2], in_vehicle ? ", in a vehicle" : "");
}

/* the body: where the head is (world units), the neck it turns about (a
little behind the eyes, at their height) and the torso's heading, flat */
#define NECK_BEHIND_EYES 0.09f

int halo_vr_body_frame(short local_player_index, float head[3], float neck[3], float forward[3])
{
	float offset[3], local[3], back[3];
	int axis;

	if (local_player_index != 0 || !halo_vr_stereo_frame() || !(vr.frame.head_flags & HALO_XR_POSE_VALID) ||
		!vr.torso_valid)
	{
		return 0;
	}
	tracked_offset(vr.frame.head.position, offset);
	for (axis = 0; axis < 3; axis++)
		head[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	local[0] = sinf(vr.head_level_yaw) * NECK_BEHIND_EYES;
	local[1] = 0.0f;
	local[2] = cosf(vr.head_level_yaw) * NECK_BEHIND_EYES;
	local_to_game(local, vr.body_yaw, back);
	for (axis = 0; axis < 3; axis++)
		neck[axis] = head[axis] + back[axis] / METRES_PER_WORLD_UNIT;
	local[0] = -sinf(vr.torso_yaw);
	local[2] = -cosf(vr.torso_yaw);
	local_to_game(local, vr.body_yaw, forward);
	forward[2] = 0.0f;
	normalize3(forward);
	return 1;
}

/* the whole body (vr.body "full", port/linux/game/vr_body.c): the eyes
(world units), the head's forward and up, and the game's camera, where the
eyes are standing straight (as recentred; lower crouched by the button) */
int halo_vr_head_pose(short local_player_index, float eye[3], float forward[3], float up[3], float standing[3])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	float offset[3], direction[3];
	int axis;

	if (local_player_index != 0 || !halo_vr_stereo_frame() || !(vr.frame.head_flags & HALO_XR_POSE_VALID))
		return 0;
	tracked_offset(vr.frame.head.position, offset);
	for (axis = 0; axis < 3; axis++)
		eye[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	quaternion_rotate(vr.frame.head.orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(vr.frame.head.orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);
	memcpy(standing, vr.game_camera, sizeof(vr.game_camera));
	return 1;
}

int halo_vr_free_hand(short local_player_index, float position[3], float forward[3], float up[3])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const struct halo_xr_hand *hand = &vr.frame.hands[free_hand()];
	float grip[3], offset[3], direction[3];
	int axis;

	if (!vr.aim_with_controller || local_player_index != 0 || !halo_vr_stereo_frame() || !hand->active ||
		!(hand->flags & HALO_XR_POSE_VALID))
	{
		return 0;
	}
	/* (in model space: left-handed, the right controller as a left one) */
	memcpy(grip, hand->grip.position, sizeof(grip));
	model_local_point(grip);
	tracked_offset(grip, offset);
	for (axis = 0; axis < 3; axis++)
		position[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	quaternion_rotate(hand->aim.orientation, local_forward, direction);
	model_local_direction(direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(hand->aim.orientation, local_up, direction);
	model_local_direction(direction);
	local_to_game(direction, vr.body_yaw, up);
	return 1;
}

int halo_vr_two_hand(short local_player_index, const float foregrip[3], const float reference[3])
{
	const struct halo_xr_hand *hand = &vr.frame.hands[free_hand()];
	float position[3], forward[3], up[3];
	int squeezed = hand->squeeze > (vr.two_hand_latched ? 0.35f : 0.6f);

	memcpy(vr.two_hand_reference, reference, sizeof(vr.two_hand_reference));
	vr.two_hand_valid = 1;
	if (!squeezed || !halo_vr_free_hand(local_player_index, position, forward, up))
	{
		vr.two_hand_latched = vr.two_hand_gripping = 0;
		return 0;
	}
	if (!vr.two_hand_latched)
	{
		/* the grip closed: on the weapon if the hand is at its foregrip */
		float distance = sqrtf((position[0] - foregrip[0]) * (position[0] - foregrip[0]) +
			(position[1] - foregrip[1]) * (position[1] - foregrip[1]) +
			(position[2] - foregrip[2]) * (position[2] - foregrip[2])) * METRES_PER_WORLD_UNIT;

		vr.two_hand_latched = 1;
		vr.two_hand_gripping = distance < vr.two_hand_reach;
		if (vr.two_hand_gripping)
			host_xr_haptic(free_hand(), 0.4f, 0.04f);
	}
	return vr.two_hand_gripping;
}

int halo_vr_look_delta(float *yaw, float *pitch)
{
	/* past the head's tremble */
	const float threshold = 2.0f * PI / 180.0f;

	*yaw = *pitch = 0.0f;
	if (fabsf(vr.look_turned_yaw) > threshold)
	{
		*yaw = vr.look_turned_yaw;
		vr.look_turned_yaw = 0.0f;
	}
	if (fabsf(vr.look_turned_pitch) > threshold)
	{
		*pitch = vr.look_turned_pitch;
		vr.look_turned_pitch = 0.0f;
	}
	return *yaw != 0.0f || *pitch != 0.0f;
}

void halo_vr_set_laser(short local_player_index, const float start[3], const float direction[3])
{
	if (local_player_index != 0)
		return;
	/* (from model space) */
	memcpy(vr.laser_start, start, sizeof(vr.laser_start));
	memcpy(vr.laser_direction, direction, sizeof(vr.laser_direction));
	mirror_game_point(vr.laser_start);
	mirror_game_direction(vr.laser_direction);
	vr.laser_frame = vr.frame_number;
}

/* ---------- the crosshair

render.c tells, each frame, how far the weapon's aim runs before it hits
something; the crosshair floats there, along the controller's aim from
the muzzle, facing the eyes, as large as the HUD showed it. */

void halo_vr_set_aim_distance(float world_units)
{
	vr.aim_distance = world_units * METRES_PER_WORLD_UNIT;
	vr.aim_distance_frame = vr.frame_number;
}

/* a rotation from its axes (unit, right-handed) */
static void quaternion_from_axes(const float x[3], const float y[3], const float z[3], float q[4])
{
	float trace = x[0] + y[1] + z[2];

	if (trace > 0.0f)
	{
		float s = sqrtf(trace + 1.0f) * 2.0f;

		q[3] = 0.25f * s;
		q[0] = (y[2] - z[1]) / s;
		q[1] = (z[0] - x[2]) / s;
		q[2] = (x[1] - y[0]) / s;
	}
	else if (x[0] > y[1] && x[0] > z[2])
	{
		float s = sqrtf(1.0f + x[0] - y[1] - z[2]) * 2.0f;

		q[3] = (y[2] - z[1]) / s;
		q[0] = 0.25f * s;
		q[1] = (y[0] + x[1]) / s;
		q[2] = (z[0] + x[2]) / s;
	}
	else if (y[1] > z[2])
	{
		float s = sqrtf(1.0f + y[1] - x[0] - z[2]) * 2.0f;

		q[3] = (z[0] - x[2]) / s;
		q[0] = (y[0] + x[1]) / s;
		q[1] = 0.25f * s;
		q[2] = (z[1] + y[2]) / s;
	}
	else
	{
		float s = sqrtf(1.0f + z[2] - x[0] - y[1]) * 2.0f;

		q[3] = (x[1] - y[0]) / s;
		q[0] = (z[0] + x[2]) / s;
		q[1] = (z[1] + y[2]) / s;
		q[2] = 0.25f * s;
	}
}

static int crosshair_pose(struct halo_xr_pose *pose, float *size)
{
	static const float world_up[3] = { 0.0f, 1.0f, 0.0f };
	float forward[3], up[3], right[3], offset[3], start_world[3], start[3], direction[3], x[3], y[3], z[3];
	float distance = vr.aim_distance_frame == vr.frame_number ? vr.aim_distance : CROSSHAIR_FARTHEST;
	float angle;
	int axis;

	if (distance > CROSSHAIR_FARTHEST)
		distance = CROSSHAIR_FARTHEST;
	if (distance < CROSSHAIR_NEAREST)
		distance = CROSSHAIR_NEAREST;
	/* driving with no gun to aim, none */
	if (vr.seat_mode == HALO_VR_SEAT_STEER && !vr.seat_armed)
		return 0;
	/* seated, along the vehicle's aim; else along the weapon's aim from its
	muzzle, as its shots go */
	if (!seat_aim(start_world, forward))
	{
		if (!halo_vr_weapon_origin_offset(0, offset))
			return 0;
		weapon_axes(forward, up, right);
		for (axis = 0; axis < 3; axis++)
			start_world[axis] = vr.game_camera[axis] + offset[axis];
	}
	world_to_local(start_world, start);
	game_to_local(forward, direction);
	for (axis = 0; axis < 3; axis++)
	{
		pose->position[axis] = start[axis] + direction[axis] * distance;
		z[axis] = vr.frame.head.position[axis] - pose->position[axis];
	}
	/* facing the eyes, upright */
	if (normalize3(z) < 1e-4f)
		return 0;
	cross3(world_up, z, x);
	if (normalize3(x) < 1e-4f)
		return 0;
	cross3(z, x, y);
	quaternion_from_axes(x, y, z, pose->orientation);
	/* the angle the region spans in the HUD, at this distance */
	angle = CROSSHAIR_REGION_PIXELS / 640.0f * vr.hud_width / vr.hud_distance;
	*size = 2.0f * distance * tanf(angle * 0.5f);
	return 1;
}

/* ---------- the menus' pointer

In the menus (the screen fixed in the room), a controller points at the
screen and its trigger clicks, as the desktop builds' mouse does
(halo_ui_pointer.h): where its aim's line meets the screen, in the menus'
640x480. The hand whose trigger was pressed last points; a click gives a
short tick of the hand's rumble. */

static struct
{
	int valid;
	float x, y;
	float reported_x, reported_y;
	/* (a stick moved the menu's choice: the pointer, not moved far from
	here since, leaves it be) */
	int stick_quiet;
	float stick_x, stick_y;
	int hand;
	int trigger_down[HALO_XR_HAND_COUNT];
	int clicks;
	/* a click is taken where it pointed as the trigger began to move
	(pulling it through tips the controller) */
	int pulling[HALO_XR_HAND_COUNT];
	float pull_x[HALO_XR_HAND_COUNT], pull_y[HALO_XR_HAND_COUNT];
	float click_x, click_y;
} menu_pointer;

static void menu_pointer_update(void)
{
	static const float forward[3] = { 0.0f, 0.0f, -1.0f };
	const struct halo_xr_hand *hand;
	float direction[3], t, hit_x, hit_y, width, height;
	int index;

	menu_pointer.valid = 0;
	if (!vr.active || vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN || !vr.frame.focused)
	{
		menu_pointer.trigger_down[0] = menu_pointer.trigger_down[1] = 0;
		menu_pointer.pulling[0] = menu_pointer.pulling[1] = 0;
		return;
	}
	/* (the hand that clicked last points; a click counts as it goes down) */
	for (index = 0; index < HALO_XR_HAND_COUNT; index++)
	{
		int down = vr.frame.hands[index].active && (vr.frame.hands[index].buttons & HALO_XR_BUTTON_TRIGGER);

		if (down && !menu_pointer.trigger_down[index])
		{
			menu_pointer.hand = index;
			menu_pointer.clicks++;
		}
		menu_pointer.trigger_down[index] = down;
	}
	hand = &vr.frame.hands[menu_pointer.hand];
	if (!hand->active || !(hand->flags & HALO_XR_POSE_VALID))
	{
		hand = &vr.frame.hands[1 - menu_pointer.hand];
		if (!hand->active || !(hand->flags & HALO_XR_POSE_VALID))
			return;
	}
	quaternion_rotate(hand->aim.orientation, forward, direction);
	if (direction[2] > -1e-3f)
		return;
	t = (-vr.screen_distance - hand->aim.position[2]) / direction[2];
	if (t <= 0.0f)
		return;
	hit_x = hand->aim.position[0] + direction[0] * t;
	hit_y = hand->aim.position[1] + direction[1] * t;
	width = vr.screen_width;
	height = vr.screen_width * 0.75f;
	menu_pointer.x = (hit_x / width + 0.5f) * 640.0f;
	menu_pointer.y = (0.5f - hit_y / height) * 480.0f;
	menu_pointer.valid = menu_pointer.x >= 0.0f && menu_pointer.x < 640.0f && menu_pointer.y >= 0.0f &&
		menu_pointer.y < 480.0f;
	/* (the click where the trigger began to move, if it pointed then) */
	{
		int pulling = vr.frame.hands[menu_pointer.hand].trigger > 0.1f;

		if (pulling && !menu_pointer.pulling[menu_pointer.hand] && menu_pointer.valid)
		{
			menu_pointer.pull_x[menu_pointer.hand] = menu_pointer.x;
			menu_pointer.pull_y[menu_pointer.hand] = menu_pointer.y;
		}
		if (!pulling || !menu_pointer.pulling[menu_pointer.hand])
		{
			menu_pointer.click_x = menu_pointer.x;
			menu_pointer.click_y = menu_pointer.y;
		}
		else
		{
			menu_pointer.click_x = menu_pointer.pull_x[menu_pointer.hand];
			menu_pointer.click_y = menu_pointer.pull_y[menu_pointer.hand];
		}
		if (!pulling || menu_pointer.valid)
			menu_pointer.pulling[menu_pointer.hand] = pulling;
	}
}

int halo_vr_menu_pointer(int *x, int *y, int *click_x, int *click_y, int *moved, int *clicks)
{
	menu_pointer_update();
	*click_x = (int)menu_pointer.click_x;
	*click_y = (int)menu_pointer.click_y;
	*clicks = menu_pointer.clicks;
	if (menu_pointer.clicks)
		host_xr_haptic(menu_pointer.hand, 0.3f, 0.02f);
	menu_pointer.clicks = 0;
	if (!menu_pointer.valid)
		return 0;
	*x = (int)menu_pointer.x;
	*y = (int)menu_pointer.y;
	/* (a stick pushed: what it chooses stays chosen until the pointer
	moves well away) */
	{
		int index;

		for (index = 0; index < HALO_XR_HAND_COUNT; index++)
		{
			const struct halo_xr_hand *hand = &vr.frame.hands[index];

			if (hand->active && (fabsf(hand->thumbstick[0]) > 0.4f || fabsf(hand->thumbstick[1]) > 0.4f))
			{
				menu_pointer.stick_quiet = 1;
				menu_pointer.stick_x = menu_pointer.x;
				menu_pointer.stick_y = menu_pointer.y;
			}
		}
		if (menu_pointer.stick_quiet && (fabsf(menu_pointer.x - menu_pointer.stick_x) > 25.0f ||
			fabsf(menu_pointer.y - menu_pointer.stick_y) > 25.0f))
		{
			menu_pointer.stick_quiet = 0;
		}
	}
	/* (moved past a hand's tremor: a few pixels) */
	*moved = !menu_pointer.stick_quiet && (fabsf(menu_pointer.x - menu_pointer.reported_x) >= 3.0f ||
		fabsf(menu_pointer.y - menu_pointer.reported_y) >= 3.0f);
	if (*moved)
	{
		menu_pointer.reported_x = menu_pointer.x;
		menu_pointer.reported_y = menu_pointer.y;
	}
	return 1;
}

int halo_vr_menu_pointer_at(int *x, int *y)
{
	if (!menu_pointer.valid)
		return 0;
	*x = (int)menu_pointer.x;
	*y = (int)menu_pointer.y;
	return 1;
}

/* ---------- players' names

Names over the heads (hud.c) are drawn into slots of a region of the back
buffer, one under another, before the HUD, and each slot is shown on a quad
of its own over its player's head, facing the eyes, as wide as the name
looks at the HUD's distance: in VR the HUD is a screen at one distance,
where names drawn on it hung before the eyes. */

#define NAME_SLOT_LEFT 240
#define NAME_SLOT_WIDTH 160
#define NAME_SLOT_HEIGHT 30
/* how large a name looks against the HUD's text (smaller: over a head, not
on the screen before the eyes) */
#define NAME_SCALE 0.6f

int halo_vr_names_apart(void)
{
	return vr.active && halo_vr_stereo_frame();
}

void halo_vr_name_slot(int slot, short bounds[4])
{
	bounds[0] = NAME_SLOT_LEFT;
	bounds[1] = (short)(slot * NAME_SLOT_HEIGHT);
	bounds[2] = NAME_SLOT_LEFT + NAME_SLOT_WIDTH;
	bounds[3] = (short)((slot + 1) * NAME_SLOT_HEIGHT);
}

int halo_vr_add_name(const float point[3])
{
	int slot = vr.name_count;

	if (slot >= HALO_XR_MAXIMUM_NAMES)
		return -1;
	memcpy(vr.name_points[slot], point, sizeof(vr.name_points[slot]));
	vr.name_count++;
	return slot;
}

void halo_vr_capture_names(void)
{
	if (vr.name_count > 0)
	{
		vr.names_captured = vr_copied(host_xr_copy_layer_region(HALO_XR_LAYER_NAMES, d3d8_back_buffer_texture(),
			HALO_XR_COPY_ALPHA, NAME_SLOT_LEFT / 640.0f, 0.0f, (NAME_SLOT_LEFT + NAME_SLOT_WIDTH) / 640.0f,
			HALO_XR_MAXIMUM_NAMES * NAME_SLOT_HEIGHT / 480.0f));
	}
	d3d8_clear_back_buffer(0.0f, 0.0f, 0.0f, 0.0f);
}

static int name_pose(const float point[3], struct halo_xr_pose *pose, float *width)
{
	static const float world_up[3] = { 0.0f, 1.0f, 0.0f };
	float x[3], y[3], z[3];
	float distance;
	float angle;
	int axis;

	world_to_local(point, pose->position);
	for (axis = 0; axis < 3; axis++)
		z[axis] = vr.frame.head.position[axis] - pose->position[axis];
	distance = normalize3(z);
	/* (facing the eyes, upright) */
	if (distance < 0.05f)
		return 0;
	cross3(world_up, z, x);
	if (normalize3(x) < 1e-4f)
		return 0;
	cross3(z, x, y);
	quaternion_from_axes(x, y, z, pose->orientation);
	/* (as wide as the slot looks on the HUD, smaller) */
	angle = (float)NAME_SLOT_WIDTH / 640.0f * vr.hud_width / vr.hud_distance * NAME_SCALE;
	*width = 2.0f * distance * tanf(angle * 0.5f);
	return 1;
}

/* ---------- the scope

Zoomed in, the weapon shows its view on a disc over the barrel, like a
scope's eyepiece, from a camera there looking along the weapon: the disc
magnifies what it covers of the player's view by the zoom's magnification.
The world around it stays as it is (zooming the whole view sickens). */

#define SCOPE_ABOVE_HAND 0.07f
#define SCOPE_AHEAD_OF_HAND 0.22f

/* (the weapon in hand's: halo_vr_set_scope_raise) */
static float scope_raise;

void halo_vr_set_scope_raise(float metres)
{
	scope_raise = metres;
}

int halo_vr_scope(short local_player_index, float magnification, float position[3], float forward[3], float up[3],
	float *vertical_field_of_view)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[weapon_hand()];
	float right[3], game_offset[3], local_forward[3], local_up[3], local_right[3], back[3];
	float eye_distance = 0.0f;
	int axis;

	if (magnification <= 1.0f || vr.scope_size <= 0.0f || !weapon_hand_tracked(local_player_index))
		return 0;
	/* the disc: above and ahead of the hand, moved sideways over the barrel
	as drawn, facing back along the weapon's aim */
	weapon_axes(forward, up, right);
	game_to_local(forward, local_forward);
	game_to_local(up, local_up);
	game_to_local(right, local_right);
	/* (placed for the weapons at the old fixed place in the hand: moved as
	the weapon is from it, halo_vr_weapon_camera; sideways by the barrel,
	below) */
	for (axis = 0; axis < 3; axis++)
	{
		vr.scope_pose.position[axis] = hand->aim.position[axis] +
			local_up[axis] * (SCOPE_ABOVE_HAND + scope_raise + vr.weapon_shift[2]) +
			local_forward[axis] * (SCOPE_AHEAD_OF_HAND + vr.weapon_shift[0]);
	}
	if (barrel_known())
	{
		float barrel[3], sideways = 0.0f;

		world_to_local(vr.barrel, barrel);
		for (axis = 0; axis < 3; axis++)
			sideways += (barrel[axis] - hand->aim.position[axis]) * local_right[axis];
		for (axis = 0; axis < 3; axis++)
			vr.scope_pose.position[axis] += local_right[axis] * sideways;
	}
	for (axis = 0; axis < 3; axis++)
	{
		back[axis] = -local_forward[axis];
		eye_distance += (vr.scope_pose.position[axis] - vr.frame.head.position[axis]) *
			(vr.scope_pose.position[axis] - vr.frame.head.position[axis]);
	}
	quaternion_from_axes(local_right, local_up, back, vr.scope_pose.orientation);
	eye_distance = sqrtf(eye_distance);
	if (eye_distance < 0.05f)
		eye_distance = 0.05f;

	/* the camera: at the disc, along the weapon */
	tracked_offset(vr.scope_pose.position, game_offset);
	for (axis = 0; axis < 3; axis++)
		position[axis] = vr.game_camera[axis] + game_offset[axis] / METRES_PER_WORLD_UNIT;
	*vertical_field_of_view = 2.0f * atanf(vr.scope_size * 0.5f / eye_distance) / magnification;
	vr.scope_pass = 1;
	return 1;
}

int halo_vr_scope_pass(void)
{
	return vr.scope_pass;
}

void halo_vr_capture_scope(float left, float top, float right, float bottom)
{
	vr.scope_pass = 0;
	vr.scope_captured = vr_copied(host_xr_copy_layer_region(HALO_XR_LAYER_SCOPE, d3d8_back_buffer_texture(),
		HALO_XR_COPY_DISC, left, top, right, bottom));
}

int halo_vr_hide_crosshair(void)
{
	/* the HUD's crosshair marks where the head looks: left out of it, and
	drawn on its own to be taken to where the weapon aims (vr.crosshair) */
	return vr.active && vr.aim_with_controller;
}

int halo_vr_crosshair_apart(void)
{
	return vr.active && vr.aim_with_controller && vr.crosshair && halo_vr_stereo_frame();
}

void halo_vr_capture_crosshair(void)
{
	vr.crosshair_captured = vr_copied(host_xr_copy_layer_region(HALO_XR_LAYER_CROSSHAIR, d3d8_back_buffer_texture(),
		HALO_XR_COPY_ALPHA, CROSSHAIR_REGION_LEFT, CROSSHAIR_REGION_TOP, CROSSHAIR_REGION_RIGHT,
		CROSSHAIR_REGION_BOTTOM));
	d3d8_clear_back_buffer(0.0f, 0.0f, 0.0f, 0.0f);
}

/* Left-handed, in play, the controllers as the gamepad's sides: the weapon
hand's trigger and grip are the right side's (fire, melee), the free hand's
the left side's (the grenade, both hands); with vr.mirror_controls the
sticks, their clicks, the bumpers and the face buttons change sides too,
the Quest's X and Y for A and B (after HaloCE-Quest-VR, CC0). The Steam
Frame has A, B, X and Y on the right controller only, a D-pad on the left:
the D-pad becomes them, as their mirror image (down A, left B, right X, up
Y), and they the D-pad. The menus keep the controllers as they are (A
accepts and B goes back, as the screens show). */
static void controllers_mirrored(struct halo_xr_hand hands[HALO_XR_HAND_COUNT])
{
	struct halo_xr_hand *left = &hands[HALO_XR_HAND_LEFT];
	struct halo_xr_hand *right = &hands[HALO_XR_HAND_RIGHT];
	int left_buttons = left->buttons, right_buttons = right->buttons;
	int swapped = HALO_XR_BUTTON_TRIGGER | HALO_XR_BUTTON_SQUEEZE;
	int frame = vr.frame.controller_layout == HALO_XR_LAYOUT_FRAME;
	float value;
	int axis;

	value = left->trigger;
	left->trigger = right->trigger;
	right->trigger = value;
	value = left->squeeze;
	left->squeeze = right->squeeze;
	right->squeeze = value;
	if (vr.mirror_controls)
	{
		for (axis = 0; axis < 2; axis++)
		{
			value = left->thumbstick[axis];
			left->thumbstick[axis] = right->thumbstick[axis];
			right->thumbstick[axis] = value;
		}
		swapped |= HALO_XR_BUTTON_THUMBSTICK | HALO_XR_BUTTON_BUMPER;
		if (!frame)
			swapped |= HALO_XR_BUTTON_A | HALO_XR_BUTTON_B;
	}
	left->buttons = (left_buttons & ~swapped) | (right_buttons & swapped);
	right->buttons = (right_buttons & ~swapped) | (left_buttons & swapped);
	if (vr.mirror_controls && frame)
	{
		const int face = HALO_XR_BUTTON_A | HALO_XR_BUTTON_B;
		const int dpad = HALO_XR_BUTTON_DPAD_UP | HALO_XR_BUTTON_DPAD_DOWN | HALO_XR_BUTTON_DPAD_LEFT |
			HALO_XR_BUTTON_DPAD_RIGHT;

		/* (the right one's X and Y come as the left's A and B: host_xr.c) */
		left->buttons &= ~(face | dpad);
		right->buttons &= ~face;
		right->buttons |= ((left_buttons & HALO_XR_BUTTON_DPAD_DOWN) ? HALO_XR_BUTTON_A : 0) |
			((left_buttons & HALO_XR_BUTTON_DPAD_LEFT) ? HALO_XR_BUTTON_B : 0);
		left->buttons |= ((left_buttons & HALO_XR_BUTTON_DPAD_RIGHT) ? HALO_XR_BUTTON_A : 0) |
			((left_buttons & HALO_XR_BUTTON_DPAD_UP) ? HALO_XR_BUTTON_B : 0) |
			((right_buttons & HALO_XR_BUTTON_A) ? HALO_XR_BUTTON_DPAD_DOWN : 0) |
			((right_buttons & HALO_XR_BUTTON_B) ? HALO_XR_BUTTON_DPAD_LEFT : 0) |
			((left_buttons & HALO_XR_BUTTON_A) ? HALO_XR_BUTTON_DPAD_RIGHT : 0) |
			((left_buttons & HALO_XR_BUTTON_B) ? HALO_XR_BUTTON_DPAD_UP : 0);
	}
}

/* The buttons the actions are on (vr.button_*), as the gamepad's sides
have them once mirrored: A and B the right hand's, X and Y the left's
A and B, the move stick's click the left's, the turning stick's the
right's, the grip the right's (the weapon hand's). Each action's button
is read and handed on as the button the action has by default, which
xinput_sdl.c turns into it; with the defaults nothing changes. The grips
holding a Warthog's wheel press nothing. */
static const int input_hands[HALO_VR_INPUT_COUNT] =
{
	HALO_XR_HAND_RIGHT, HALO_XR_HAND_RIGHT, HALO_XR_HAND_LEFT, HALO_XR_HAND_LEFT, HALO_XR_HAND_LEFT,
	HALO_XR_HAND_RIGHT, HALO_XR_HAND_RIGHT, HALO_XR_HAND_LEFT,
};
static const int input_buttons[HALO_VR_INPUT_COUNT] =
{
	HALO_XR_BUTTON_A, HALO_XR_BUTTON_B, HALO_XR_BUTTON_A, HALO_XR_BUTTON_B, HALO_XR_BUTTON_THUMBSTICK,
	HALO_XR_BUTTON_THUMBSTICK, HALO_XR_BUTTON_SQUEEZE, HALO_XR_BUTTON_SQUEEZE,
};

/* The free hand's grip as a button (HALO_VR_INPUT_FREE_GRIP; push to talk
by default), the hands mirrored: pressed once closed for
FREE_GRIP_DELAY_US, at halo_vr_two_hand's threshold, unless it took the
weapon's foregrip or the Warthog's wheel (which both decide sooner) since
it closed; that press then is theirs until it opens. So a grip reaching
for the weapon presses nothing, not even for a moment. */
#define FREE_GRIP_DELAY_US 150000ULL

static int free_grip_down(const struct halo_xr_hand *free)
{
	unsigned long long now = now_us();

	if (free->squeeze <= (vr.free_grip_since_us ? 0.35f : 0.6f))
	{
		vr.free_grip_since_us = 0;
		vr.free_grip_taken = 0;
		return 0;
	}
	if (!vr.free_grip_since_us)
		vr.free_grip_since_us = now;
	if (vr.two_hand_gripping || vr.wheel_held[free_hand()])
		vr.free_grip_taken = 1;
	return !vr.free_grip_taken && now - vr.free_grip_since_us >= FREE_GRIP_DELAY_US;
}

/* the buttons' presses (HALO_VR_INPUT_*), the hands mirrored */
static void controls_down(const struct halo_xr_hand hands[HALO_XR_HAND_COUNT], int down[HALO_VR_INPUT_COUNT])
{
	int input;

	for (input = 0; input < HALO_VR_INPUT_COUNT; input++)
		down[input] = (hands[input_hands[input]].buttons & input_buttons[input]) != 0;
	if (vr.wheel_hands)
		down[HALO_VR_INPUT_GRIP] = 0;
	down[HALO_VR_INPUT_FREE_GRIP] = free_grip_down(&hands[HALO_XR_HAND_LEFT]);
}

static void controls_remapped(struct halo_xr_hand hands[HALO_XR_HAND_COUNT])
{
	int down[HALO_VR_INPUT_COUNT];
	int input, action;

	controls_down(hands, down);
	for (input = 0; input < HALO_VR_INPUT_COUNT; input++)
		hands[input_hands[input]].buttons &= ~input_buttons[input];
	for (action = 0; action < HALO_VR_ACTION_COUNT; action++)
	{
		/* (talking is no button of the game's: halo_vr_push_to_talk_held) */
		if (action != HALO_VR_ACTION_TALK && down[vr.action_inputs[action]])
			hands[input_hands[action]].buttons |= input_buttons[action];
	}
}

/* the controllers as the game is played with them: mirrored (left-handed)
and the actions on their buttons */
static void controls_in_play(struct halo_xr_hand hands[HALO_XR_HAND_COUNT])
{
	if (vr.left_handed)
		controllers_mirrored(hands);
	controls_remapped(hands);
}

/* Voice chat's push to talk in VR (network_voice.c, through
halo_push_to_talk_held): its button (vr.button_talk), in the menus too */
int halo_vr_push_to_talk_held(void)
{
	struct halo_xr_hand hands[HALO_XR_HAND_COUNT];
	int down[HALO_VR_INPUT_COUNT];

	if (!vr.active || !vr.frame.focused)
		return 0;
	memcpy(hands, vr.frame.hands, sizeof(hands));
	if (vr.left_handed)
		controllers_mirrored(hands);
	controls_down(hands, down);
	return down[vr.action_inputs[HALO_VR_ACTION_TALK]];
}

/* the controllers, for the gamepad the game reads (xinput_sdl.c) */
int halo_vr_controllers(struct halo_xr_hand hands[HALO_XR_HAND_COUNT], int *menus)
{
	if (!vr.active || !vr.frame.focused)
		return 0;
	memcpy(hands, vr.frame.hands, sizeof(vr.frame.hands));
	*menus = vr.last_overlay_mode == HALO_XR_OVERLAY_SCREEN;
	if (!*menus)
		controls_in_play(hands);
	return 1;
}

#else

int halo_vr_controller_layout(void) { return 0; }
int halo_vr_weapon_hand(void) { return 1; }
int halo_vr_action_input(int action) { return action; }
int halo_vr_graphics(void) { return HALO_VR_GRAPHICS_HIGH; }
int halo_vr_foveation_supported(void) { return 0; }
float halo_vr_gpu_milliseconds(void) { return 0.0f; }
float halo_vr_refresh_rate(void) { return 0.0f; }
int halo_vr_refresh_rates(float *rates, int maximum, float *current)
{
	(void)rates; (void)maximum; *current = 0.0f;
	return 0;
}
float halo_vr_started_resolution_scale(void) { return 1.0f; }
int halo_vr_active(void) { return 0; }
unsigned long long halo_vr_timing_start(void) { return 0; }
unsigned long halo_vr_frame_serial(void) { return 0; }
void halo_vr_timing_end(int part, unsigned long long start) { (void)part; (void)start; }
int halo_vr_stereo_frame(void) { return 0; }
int halo_vr_stereo_player(short local_player_index, int cinematic) { (void)local_player_index; (void)cinematic; return 0; }
void halo_vr_eye_camera(int eye, const float game_position[3], float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4])
{
	(void)eye; (void)game_position; (void)position; (void)forward; (void)up;
	(void)vertical_field_of_view; (void)frustum_bounds;
}
void halo_vr_capture_eye(int eye) { (void)eye; }
int halo_vr_multiview(void) { return 0; }
void halo_vr_center_camera(const float game_position[3], float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4])
{
	(void)game_position; (void)position; (void)forward; (void)up; (void)vertical_field_of_view; (void)frustum_bounds;
}
void halo_vr_capture_multiview(void) {}
void halo_vr_multiview_sky(int sky) { (void)sky; }
void halo_vr_multiview_lens_flares(int drawing) { (void)drawing; }
void halo_vr_multiview_projection(float projection_22, float projection_32)
{
	(void)projection_22; (void)projection_32;
}
void halo_vr_begin_overlay(int menus_active) { (void)menus_active; }
int halo_vr_aim(short local_player_index, float current_yaw, float current_pitch, float *yaw, float *pitch)
{
	(void)local_player_index; (void)current_yaw; (void)current_pitch; (void)yaw; (void)pitch;
	return 0;
}
void halo_vr_initialize(void) {}
void halo_vr_frame_begin(void) {}
void halo_vr_present(unsigned int back_buffer_texture) { (void)back_buffer_texture; }
int halo_vr_eye_size(int *width, int *height) { (void)width; (void)height; return 0; }
int halo_vr_aim_vector(short local_player_index, float vector[3]) { (void)local_player_index; (void)vector; return 0; }
int halo_vr_weapon_origin_offset(short local_player_index, float offset[3]) { (void)local_player_index; (void)offset; return 0; }
void halo_vr_weapon_wrist(long graph_index, const float wrist[3]) { (void)graph_index; (void)wrist; }
int halo_vr_weapon_camera(short local_player_index, long graph_index, float position[3], float forward[3],
	float up[3])
{
	(void)local_player_index; (void)graph_index; (void)position; (void)forward; (void)up;
	return 0;
}
int halo_vr_weapon_origin(short local_player_index, float origin[3])
{
	(void)local_player_index; (void)origin;
	return 0;
}
int halo_vr_laser(short local_player_index, float start[3], float direction[3])
{
	(void)local_player_index; (void)start; (void)direction;
	return 0;
}
int halo_vr_look_vector(short local_player_index, float vector[3]) { (void)local_player_index; (void)vector; return 0; }
void halo_vr_debug_look(short local_player_index, const float unit_looking[3], int in_vehicle)
{
	(void)local_player_index; (void)unit_looking; (void)in_vehicle;
}
int halo_vr_melee(void) { return 0; }
int halo_vr_push_to_talk_held(void) { return 0; }
int halo_vr_finger_curls(short local_player_index, int hand, float curls[4])
{
	(void)local_player_index; (void)hand; (void)curls;
	return 0;
}
int halo_vr_free_arm_bends(void) { return 0; }
int halo_vr_body_mode(void) { return HALO_VR_BODY_ARMS; }
double halo_vr_pose_time(void) { return 0.0; }
void halo_vr_set_seat(short local_player_index, int seat, int armed, float vehicle_yaw)
{
	(void)local_player_index; (void)seat; (void)armed; (void)vehicle_yaw;
}
void halo_vr_set_seat_frame(short local_player_index, float vehicle_yaw)
{
	(void)local_player_index; (void)vehicle_yaw;
}
int halo_vr_vehicle_first_person(void) { return 0; }
int halo_vr_take_vehicle_spawn(short local_player_index) { (void)local_player_index; return 0; }
float halo_vr_gunner_camera_back(void) { return 0.0f; }
float halo_vr_driver_camera_forward(void) { return 0.0f; }
float halo_vr_enclosed_seat_raise(int hull) { (void)hull; return 0.0f; }
float halo_vr_glass_strength(void) { return 1.0f; }
int halo_vr_grenade_held(short local_player_index) { (void)local_player_index; return 0; }
int halo_vr_grenade_in_hand(short local_player_index, float position[3], float forward[3], float up[3],
	float *scale)
{
	(void)local_player_index; (void)position; (void)forward; (void)up; (void)scale;
	return 0;
}
int halo_vr_grenade_throw(short local_player_index, float origin[3], float direction[3], float *strength)
{
	(void)local_player_index; (void)origin; (void)direction; (void)strength;
	return 0;
}
float halo_vr_melee_strength(void) { return 0.0f; }
int halo_vr_look_delta(float *yaw, float *pitch) { *yaw = *pitch = 0.0f; return 0; }
int halo_vr_body_frame(short local_player_index, float head[3], float neck[3], float forward[3])
{
	(void)local_player_index; (void)head; (void)neck; (void)forward;
	return 0;
}
int halo_vr_head_pose(short local_player_index, float eye[3], float forward[3], float up[3], float standing[3])
{
	(void)local_player_index; (void)eye; (void)forward; (void)up; (void)standing;
	return 0;
}
int halo_vr_free_hand(short local_player_index, float position[3], float forward[3], float up[3])
{
	(void)local_player_index; (void)position; (void)forward; (void)up;
	return 0;
}
int halo_vr_first_person_mirror(short local_player_index, float point[3], float normal[3])
{
	(void)local_player_index; (void)point; (void)normal;
	return 0;
}
int halo_vr_two_hand(short local_player_index, const float foregrip[3], const float reference[3])
{
	(void)local_player_index; (void)foregrip; (void)reference;
	return 0;
}
void halo_vr_set_laser(short local_player_index, const float start[3], const float direction[3])
{
	(void)local_player_index; (void)start; (void)direction;
}
int halo_vr_ready_laser(short local_player_index, float start[3], float direction[3])
{
	(void)local_player_index; (void)start; (void)direction;
	return 0;
}
int halo_vr_scope(short local_player_index, float magnification, float position[3], float forward[3], float up[3],
	float *vertical_field_of_view)
{
	(void)local_player_index; (void)magnification; (void)position; (void)forward; (void)up;
	(void)vertical_field_of_view;
	return 0;
}
int halo_vr_scope_pass(void) { return 0; }
int halo_vr_menu_pointer(int *x, int *y, int *click_x, int *click_y, int *moved, int *clicks)
{
	(void)x; (void)y; (void)click_x; (void)click_y; (void)moved; *clicks = 0;
	return 0;
}
int halo_vr_menu_pointer_at(int *x, int *y) { (void)x; (void)y; return 0; }
int halo_vr_names_apart(void) { return 0; }
void halo_vr_name_slot(int slot, short bounds[4]) { (void)slot; (void)bounds; }
int halo_vr_add_name(const float point[3]) { (void)point; return -1; }
void halo_vr_capture_names(void) {}
void halo_vr_set_scope_raise(float metres) { (void)metres; }
void halo_vr_capture_scope(float left, float top, float right, float bottom)
{
	(void)left; (void)top; (void)right; (void)bottom;
}
void halo_vr_set_aim_distance(float world_units) { (void)world_units; }
void halo_vr_set_barrel(short local_player_index, const float world[3], const float direction[3])
{
	(void)local_player_index; (void)world; (void)direction;
}
int halo_vr_hide_crosshair(void) { return 0; }
int halo_vr_crosshair_apart(void) { return 0; }
void halo_vr_capture_crosshair(void) {}

#endif
