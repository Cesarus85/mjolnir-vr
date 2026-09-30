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
The right stick turns the body; the head turns on top of it, so the facing
the game uses (halo_vr_aim) is the body's yaw plus the head's.
*/

#include "halo_vr.h"

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

static struct
{
	int active;
	struct halo_xr_info info;
	struct halo_xr_frame frame;
	int frame_begun;
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
	unsigned long long report_start_us;
	unsigned long long frame_start_us;
	unsigned long long eyes_start_us;
	unsigned long long eyes_us;
	unsigned long long work_us;
	unsigned long long wait_us;
	int report_frames;
	int report_stereo_frames;

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
	int seat_mode;
	int seat_armed;
	float seat_gun_start[3];
	float seat_gun_direction[3];
	unsigned long seat_gun_frame;
	float seat_yaw;
	float seat_frame_yaw;
	int seat_frame_valid;
	float seat_turn;
	int vehicle_first_person;
	int turret_handles;         /* a gun's barrel rises as its handles go down */
	float gunner_camera_back;   /* metres */
	float driver_camera_forward; /* metres */
	float enclosed_seat_raise[5];  /* metres, by HALO_VR_HULL_* */
	float glass_strength;
	float vehicle_steer_angle;  /* radians */
	float vehicle_pitch_angle;
	/* the comfort vignette: how strong at most, now, and the heading it
	last saw */
	float comfort_vignette;
	float vignette;
	float vignette_last_yaw;
	int vignette_yaw_valid;
	unsigned long long vignette_last_us;
	int snap_latched;
	unsigned long long last_frame_ms;

	/* settings (config.toml, [vr]) */
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
	float weapon_offset[3];  /* forward, right, down, metres: the weapon model's camera from the hand */
	float muzzle_distance;
	/* the left hand on the weapon (halo_vr_two_hand) */
	int two_hand_gripping;
	int two_hand_latched;    /* the grip is held (on the weapon or not) */
	int two_hand_valid;
	float two_hand_reach;    /* metres from the foregrip a closing grip takes the weapon */
	/* the grenade in the left hand (grenade_update, vr.grenade_throw_speed):
	the left trigger held, the hand's last positions (LOCAL), and the throw
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
	int melee_grip_down;
	float melee_strength;    /* 0..1, the last attack's swing */
	unsigned long long melee_jab_start_us; /* the right grip's attack: the weapon jabs */
	/* the laser along the barrel, for the frame it was set in */
	unsigned long frame_number;
	unsigned long laser_frame;
	float laser_start[3];
	float laser_direction[3];
	float two_hand_reference[3]; /* left wrist from right, in the weapon's forward, right, up */
	float lean_limit;
	float crouch_limit;
	float rise_limit;
} vr;

/* d3d8_gl.c */
unsigned int d3d8_back_buffer_texture(void);
void d3d8_clear_back_buffer(float red, float green, float blue, float alpha);
void d3d8_clear_back_buffer_region(float left, float top, float right, float bottom);

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
static int weapon_hand_tracked(short local_player_index);
static void controller_axes(float forward[3], float up[3], float right[3]);
static void hand_offset(float out[3]);

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

void halo_vr_initialize(void)
{
	if (vr.active || !getenv("HALO_VR"))
		return;
	vr.smooth_turn = config_boolean("vr.smooth_turn");
	vr.snap_turn_degrees = (float)config_real("vr.snap_turn_degrees");
	vr.smooth_turn_degrees_per_second = (float)config_real("vr.smooth_turn_speed");
	vr.cinematics_3d = config_boolean("vr.cinematics_in_3d");
	vr.hud_distance = (float)config_real("vr.hud_distance");
	vr.hud_width = 2.0f * vr.hud_distance * tanf((float)config_real("vr.hud_field_of_view") * PI / 360.0f);
	vr.screen_distance = (float)config_real("vr.screen_distance");
	vr.screen_width = (float)config_real("vr.screen_width");
	vr.positional_tracking = config_boolean("vr.positional_tracking");
	vr.motion_tracker_on_wrist = config_boolean("vr.motion_tracker_on_wrist");
	vr.scope_size = (float)config_real("vr.scope_size");
	vr.crosshair = config_boolean("vr.crosshair");
	vr.motion_tracker_size = (float)config_real("vr.motion_tracker_size");
	vr.motion_tracker_turn = (float)config_real("vr.motion_tracker_turn");
	vr.motion_tracker_right = (float)config_real("vr.motion_tracker_right");
	vr.motion_tracker_down = (float)config_real("vr.motion_tracker_down");
	vr.lean_limit = (float)config_real("vr.lean_limit");
	vr.crouch_limit = (float)config_real("vr.crouch_limit");
	vr.rise_limit = 0.25f;
	vr.log_performance = config_boolean("vr.log_performance");
	vr.aim_with_controller = strcmp(config_string("vr.aim_with"), "head") != 0;
	vr.laser = config_boolean("vr.laser");
	vr.weapon_offset[0] = (float)config_real("vr.weapon_forward");
	vr.weapon_offset[1] = (float)config_real("vr.weapon_right");
	vr.weapon_offset[2] = (float)config_real("vr.weapon_down");
	vr.muzzle_distance = 0.3f;
	vr.two_hand_reach = (float)config_real("vr.two_hand_reach");
	vr.melee_swing_speed = (float)config_real("vr.melee_swing_speed");
	vr.grenade_throw_speed = (float)config_real("vr.grenade_throw_speed");
	vr.vehicle_first_person = config_boolean("vr.vehicle_first_person");
	vr.turret_handles = config_boolean("vr.turret_handles");
	vr.gunner_camera_back = (float)config_real("vr.gunner_camera_back");
	vr.driver_camera_forward = (float)config_real("vr.driver_camera_forward");
	vr.enclosed_seat_raise[HALO_VR_HULL_SCOUT] = (float)config_real("vr.ghost_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_FIGHTER] = (float)config_real("vr.banshee_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_TURRET] = (float)config_real("vr.turret_eye_raise");
	vr.enclosed_seat_raise[HALO_VR_HULL_TANK] = (float)config_real("vr.tank_eye_raise");
	vr.glass_strength = (float)config_real("vr.glass_strength");
	vr.vehicle_steer_angle = (float)config_real("vr.vehicle_steer_degrees") * PI / 180.0f;
	vr.vehicle_pitch_angle = (float)config_real("vr.vehicle_pitch_degrees") * PI / 180.0f;
	vr.comfort_vignette = (float)config_real("vr.comfort_vignette");
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
}

int halo_vr_active(void)
{
	return vr.active;
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
static void performance_report(int stereo)
{
	unsigned long long now = now_us();

	vr.work_us += now - vr.frame_start_us;
	vr.report_frames++;
	vr.report_stereo_frames += stereo != 0;
	if (!vr.report_start_us)
		vr.report_start_us = now;
	if (now - vr.report_start_us < 5000000)
		return;
	{
		float frames = (float)vr.report_frames;
		float seconds = (float)(now - vr.report_start_us) * 1e-6f;

		platform_log("VR: %.1f fps (%d%% stereo); per frame %.1f ms working (eyes %.1f ms), %.1f ms waiting "
			"for the headset", frames / seconds, (int)(100.0f * vr.report_stereo_frames / frames),
			vr.work_us * 1e-3f / frames, vr.eyes_us * 1e-3f / frames, vr.wait_us * 1e-3f / frames);
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

/* The grenade in the left hand: while the left trigger is held the game
keeps it in the hand (halo_vr_grenade_held); as the trigger lets go, the
fastest the hand moved in its last moments is the throw (halo_vr_grenade_throw). */
static void grenade_update(void)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_LEFT];
	unsigned long long now = now_us();
	int valid = hand->active && (hand->flags & HALO_XR_POSE_VALID);
	int held = valid && vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN &&
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
			host_xr_haptic(HALO_XR_HAND_LEFT, 0.6f, 0.06f);
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
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_LEFT];
	float held[3], offset[3], direction[3];
	int axis;

	if (!vr.aim_with_controller || local_player_index != 0 || !halo_vr_stereo_frame() || !hand->active ||
		!(hand->flags & HALO_XR_POSE_VALID))
	{
		return 0;
	}
	/* the grip's +x faces out of the left palm; the drawn hand's knuckles
	run along the aim's y, and its fingers along the aim's -z
	(first_person_weapons.c, vr_left_hand_free) */
	{
		float knuckles[3], fingers[3];

		quaternion_rotate(hand->grip.orientation, vr.grenade_offset, held);
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
		/* the right grip's melee attack (xinput_sdl.c) aims the same way */
		int grip_down = vr.frame.hands[HALO_XR_HAND_RIGHT].active &&
			(vr.frame.hands[HALO_XR_HAND_RIGHT].buttons & HALO_XR_BUTTON_SQUEEZE);

		if (grip_down && !vr.melee_grip_down)
		{
			vr.melee_aim_until_us = now + 700000;
			vr.melee_strength = 0.75f;
			vr.melee_jab_start_us = now;
		}
		vr.melee_grip_down = grip_down;
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
		if (vr.hand_last_valid[hand] && seconds > 0.0f && seconds < 0.1f && vr.melee_swing_speed > 0.0f &&
			vr.last_overlay_mode != HALO_XR_OVERLAY_SCREEN && now >= vr.melee_ready_us &&
			!(hand == HALO_XR_HAND_LEFT && (vr.grenade_held || now - vr.grenade_released_us < 500000)))
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
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_RIGHT];
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

/* The comfort vignette: while a vehicle turns the player (from 15 degrees a
second, fully at 60), the eye images darken from their edges; it comes in
quickly and goes slowly. */
static void vignette_update(void)
{
	unsigned long long now = now_us();
	float seconds = vr.vignette_last_us ? (float)(now - vr.vignette_last_us) * 1e-6f : 0.0f;
	float target = 0.0f, rate;

	vr.vignette_last_us = now;
	if (seconds > 0.1f)
		seconds = 0.1f;
	if (vr.seat_mode != HALO_VR_SEAT_NONE && vr.comfort_vignette > 0.0f)
	{
		float yaw = seat_heading();

		if (vr.vignette_yaw_valid && seconds > 0.0f)
		{
			float turning = fabsf(wrap_angle(yaw - vr.vignette_last_yaw)) / seconds;

			target = (turning - 15.0f * PI / 180.0f) / (45.0f * PI / 180.0f);
			if (target < 0.0f)
				target = 0.0f;
			if (target > 1.0f)
				target = 1.0f;
			target *= vr.comfort_vignette;
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

void halo_vr_set_seat_gun(short local_player_index, const float start[3], const float direction[3])
{
	if (!vr.active || local_player_index != 0)
		return;
	memcpy(vr.seat_gun_start, start, sizeof(vr.seat_gun_start));
	memcpy(vr.seat_gun_direction, direction, sizeof(vr.seat_gun_direction));
	vr.seat_gun_frame = vr.frame_number;
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

/* seated at a gun or a window, where the right controller aims it: at a
gun's handles (vr.turret_handles), the barrel rises as the hand tips down */
static void seat_aim_direction(float direction[3])
{
	float up[3], right[3];

	controller_axes(direction, up, right);
	if ((vr.seat_armed & HALO_VR_SEAT_TURRET) && vr.turret_handles)
		direction[2] = -direction[2];
}

/* seated, where the aim runs from (world units) and along: from the right
hand where it points, or driving, from the eyes the way the stick steers */
static int seat_aim(float start[3], float direction[3])
{
	int axis;

	if (vr.seat_mode == HALO_VR_SEAT_NONE)
		return 0;
	if (vr.seat_gun_frame == vr.frame_number)
	{
		/* a manned gun's own line, as it fires */
		memcpy(start, vr.seat_gun_start, sizeof(vr.seat_gun_start));
		memcpy(direction, vr.seat_gun_direction, sizeof(vr.seat_gun_direction));
		return 1;
	}
	if (vr.seat_mode == HALO_VR_SEAT_AIM && weapon_hand_tracked(0))
	{
		float hand[3];

		seat_aim_direction(direction);
		hand_offset(hand);
		for (axis = 0; axis < 3; axis++)
			start[axis] = vr.game_camera[axis] + hand[axis];
		return 1;
	}
	direction[0] = cosf(vr.last_pitch) * cosf(vr.last_yaw);
	direction[1] = cosf(vr.last_pitch) * sinf(vr.last_yaw);
	direction[2] = sinf(vr.last_pitch);
	memcpy(start, vr.game_camera, 3 * sizeof(float));
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
	vr.frame_start_us = now_us();
	vr.wait_us += vr.frame_start_us - waited;
	vr.frame_begun = 1;
	vr.frame_number++;
	vr.eyes_captured = 0;
	vr.scope_captured = 0;
	vr.crosshair_captured = 0;
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
	turn_update();
	vignette_update();
	look_update();
	grenade_update();
	melee_update();
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
		submit.wrist = host_xr_copy_layer_region(HALO_XR_LAYER_WRIST, back_buffer_texture, HALO_XR_COPY_ALPHA,
			WRIST_REGION_LEFT,
			WRIST_REGION_TOP, WRIST_REGION_RIGHT, WRIST_REGION_BOTTOM);
		submit.wrist_size = vr.motion_tracker_size;
		submit.wrist_turn = vr.motion_tracker_turn;
		submit.wrist_right = vr.motion_tracker_right;
		submit.wrist_down = vr.motion_tracker_down;
		d3d8_clear_back_buffer_region(WRIST_REGION_LEFT, WRIST_REGION_TOP, WRIST_REGION_RIGHT, WRIST_REGION_BOTTOM);
	}
	/* the crosshair, drawn apart (halo_vr_capture_crosshair), to where the
	weapon aims */
	if (vr.crosshair_captured && vr.overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED)
		submit.crosshair = crosshair_pose(&submit.crosshair_pose, &submit.crosshair_size);
	if (vr.scope_captured && submit.eyes)
	{
		submit.scope = 1;
		submit.scope_size = vr.scope_size;
		submit.scope_pose = vr.scope_pose;
	}
	host_xr_copy_layer(HALO_XR_LAYER_OVERLAY, back_buffer_texture,
		vr.overlay_blend ? HALO_XR_COPY_ALPHA : HALO_XR_COPY_OPAQUE);
	host_xr_end_frame(&submit);
	vr.last_overlay_mode = vr.overlay_mode;
	vr.last_frame_stereo = submit.eyes;
	vr.frame_begun = 0;
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

	if (eye == 0)
		vr.eyes_start_us = now_us();
	float half_tangent;
	int axis;

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

void halo_vr_capture_eye(int eye)
{
	if (!vr.active || eye < 0 || eye >= HALO_XR_EYE_COUNT)
		return;
	if (host_xr_copy_layer(HALO_XR_LAYER_EYE_LEFT + eye, d3d8_back_buffer_texture(), 1))
		vr.eyes_captured |= 1 << eye;
	if (eye == HALO_XR_EYE_COUNT - 1 && vr.eyes_start_us)
		vr.eyes_us += now_us() - vr.eyes_start_us;
}

void halo_vr_begin_overlay(int menus_active)
{
	if (!vr.active)
		return;
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
		/* driving: the right stick steers off the vehicle's heading, and
		aims above or below the horizon (the body turns with the vehicle) */
		const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_RIGHT];
		float x = hand->active && vr.frame.focused ? hand->thumbstick[0] : 0.0f;
		float y = hand->active && vr.frame.focused ? hand->thumbstick[1] : 0.0f;

		if (fabsf(x) < 0.15f)
			x = 0.0f;
		if (fabsf(y) < 0.15f)
			y = 0.0f;
		*yaw = wrap_angle(vr.seat_yaw - x * vr.vehicle_steer_angle);
		*pitch = y * vr.vehicle_pitch_angle;
		vr.aim_synced = 1;
	}
	else if (vr.seat_mode == HALO_VR_SEAT_AIM && weapon_hand_tracked(local_player_index))
	{
		/* a gunner or passenger: where the right controller points */
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

/* ---------- the weapon in the right hand (vr.aim_with = "controller") */

static int weapon_hand_tracked(short local_player_index)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_RIGHT];

	return vr.aim_with_controller && local_player_index == 0 && halo_vr_stereo_frame() &&
		hand->active && (hand->flags & HALO_XR_POSE_VALID);
}

/* the weapon's axes in the game's: where the right controller points */
/* The weapon's frames, all in the game's axes:
- the controller's: where it points;
- the model's: the controller's turned so that the first-person model's
  barrel, pointed in towards the screen's centre, points where the
  controller does (barrel_direction);
- with both hands on it, both turned alike so that the line from the
  model's right hand to its left, in the model's frame, runs from the right
  controller to the left one.
The weapon aims along the controller's frame (so turned); the model hangs
from the model's frame. */
static void controller_axes(float forward[3], float up[3], float right[3])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const float *orientation = vr.frame.hands[HALO_XR_HAND_RIGHT].aim.orientation;
	float direction[3];

	quaternion_rotate(orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);
	/* the game's axes are right-handed with +z up: forward x up is right */
	cross3(forward, up, right);
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
line with the hands (from, to); 0 without */
static int two_hand_turn(const float model_forward[3], const float model_up[3], const float model_right[3],
	float from[3], float to[3])
{
	float left[3], right_hand[3];
	int axis;

	if (!vr.two_hand_gripping || !vr.two_hand_valid)
		return 0;
	for (axis = 0; axis < 3; axis++)
	{
		from[axis] = model_forward[axis] * vr.two_hand_reference[0] + model_right[axis] * vr.two_hand_reference[1] +
			model_up[axis] * vr.two_hand_reference[2];
	}
	tracked_offset(vr.frame.hands[HALO_XR_HAND_LEFT].grip.position, left);
	tracked_offset(vr.frame.hands[HALO_XR_HAND_RIGHT].grip.position, right_hand);
	for (axis = 0; axis < 3; axis++)
		to[axis] = left[axis] - right_hand[axis];
	return normalize3(from) > 0.0f && normalize3(to) > 0.0f;
}

static void turn_axes(const float from[3], const float to[3], float forward[3], float up[3], float right[3])
{
	rotate_between(from, to, forward);
	rotate_between(from, to, up);
	cross3(forward, up, right);
}

/* where the weapon aims */
static void weapon_axes(float forward[3], float up[3], float right[3])
{
	float model_forward[3], model_up[3], model_right[3], from[3], to[3];

	controller_axes(forward, up, right);
	model_axes(forward, up, right, model_forward, model_up, model_right);
	if (two_hand_turn(model_forward, model_up, model_right, from, to))
		turn_axes(from, to, forward, up, right);
}

/* how the weapon's model hangs */
static void weapon_model_axes(float forward[3], float up[3], float right[3])
{
	float controller_forward[3], controller_up[3], controller_right[3], from[3], to[3];

	controller_axes(controller_forward, controller_up, controller_right);
	model_axes(controller_forward, controller_up, controller_right, forward, up, right);
	if (two_hand_turn(forward, up, right, from, to))
		turn_axes(from, to, forward, up, right);
}

/* the hand's offset from the game's camera, in world units */
static void hand_offset(float out[3])
{
	int axis;

	tracked_offset(vr.frame.hands[HALO_XR_HAND_RIGHT].grip.position, out);
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
	memcpy(vr.barrel, world, sizeof(vr.barrel));
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

int halo_vr_weapon_camera(short local_player_index, float position[3], float forward[3], float up[3])
{
	float right[3], hand[3];
	int axis;

	if (!weapon_hand_tracked(local_player_index))
		return 0;
	weapon_model_axes(forward, up, right);
	hand_offset(hand);
	{
		/* the right grip's melee attack, which the hand does not swing: the
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
	model's grip lands in the hand */
	for (axis = 0; axis < 3; axis++)
	{
		position[axis] = vr.game_camera[axis] + hand[axis] -
			(forward[axis] * vr.weapon_offset[0] + right[axis] * vr.weapon_offset[1] -
			up[axis] * vr.weapon_offset[2]) / METRES_PER_WORLD_UNIT;
	}
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

/* the body: where the head is (world units) and its heading, flat */
int halo_vr_body_frame(short local_player_index, float head[3], float forward[3])
{
	float offset[3], look[3];
	int axis;

	if (local_player_index != 0 || !halo_vr_stereo_frame() || !(vr.frame.head_flags & HALO_XR_POSE_VALID))
		return 0;
	tracked_offset(vr.frame.head.position, offset);
	for (axis = 0; axis < 3; axis++)
		head[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	if (!halo_vr_look_vector(local_player_index, look))
		return 0;
	forward[0] = look[0];
	forward[1] = look[1];
	forward[2] = 0.0f;
	if (normalize3(forward) < 1e-3f)
	{
		forward[0] = cosf(vr.body_yaw);
		forward[1] = sinf(vr.body_yaw);
	}
	return 1;
}

int halo_vr_left_hand(short local_player_index, float position[3], float forward[3], float up[3])
{
	static const float local_forward[3] = { 0.0f, 0.0f, -1.0f };
	static const float local_up[3] = { 0.0f, 1.0f, 0.0f };
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_LEFT];
	float offset[3], direction[3];
	int axis;

	if (!vr.aim_with_controller || local_player_index != 0 || !halo_vr_stereo_frame() || !hand->active ||
		!(hand->flags & HALO_XR_POSE_VALID))
	{
		return 0;
	}
	tracked_offset(hand->grip.position, offset);
	for (axis = 0; axis < 3; axis++)
		position[axis] = vr.game_camera[axis] + offset[axis] / METRES_PER_WORLD_UNIT;
	quaternion_rotate(hand->aim.orientation, local_forward, direction);
	local_to_game(direction, vr.body_yaw, forward);
	quaternion_rotate(hand->aim.orientation, local_up, direction);
	local_to_game(direction, vr.body_yaw, up);
	return 1;
}

int halo_vr_two_hand(short local_player_index, const float foregrip[3], const float reference[3])
{
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_LEFT];
	float position[3], forward[3], up[3];
	int squeezed = hand->squeeze > (vr.two_hand_latched ? 0.35f : 0.6f);

	memcpy(vr.two_hand_reference, reference, sizeof(vr.two_hand_reference));
	vr.two_hand_valid = 1;
	if (!squeezed || !halo_vr_left_hand(local_player_index, position, forward, up))
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
			host_xr_haptic(HALO_XR_HAND_LEFT, 0.4f, 0.04f);
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
	memcpy(vr.laser_start, start, sizeof(vr.laser_start));
	memcpy(vr.laser_direction, direction, sizeof(vr.laser_direction));
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

/* ---------- the scope

Zoomed in, the weapon shows its view on a disc over the barrel, like a
scope's eyepiece, from a camera there looking along the weapon: the disc
magnifies what it covers of the player's view by the zoom's magnification.
The world around it stays as it is (zooming the whole view sickens). */

#define SCOPE_ABOVE_HAND 0.07f
#define SCOPE_AHEAD_OF_HAND 0.22f

int halo_vr_scope(short local_player_index, float magnification, float position[3], float forward[3], float up[3],
	float *vertical_field_of_view)
{
	const struct halo_xr_hand *hand = &vr.frame.hands[HALO_XR_HAND_RIGHT];
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
	for (axis = 0; axis < 3; axis++)
	{
		vr.scope_pose.position[axis] = hand->aim.position[axis] + local_up[axis] * SCOPE_ABOVE_HAND +
			local_forward[axis] * SCOPE_AHEAD_OF_HAND;
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
	vr.scope_captured = host_xr_copy_layer_region(HALO_XR_LAYER_SCOPE, d3d8_back_buffer_texture(), HALO_XR_COPY_DISC,
		left, top, right, bottom);
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
	vr.crosshair_captured = host_xr_copy_layer_region(HALO_XR_LAYER_CROSSHAIR, d3d8_back_buffer_texture(),
		HALO_XR_COPY_ALPHA, CROSSHAIR_REGION_LEFT, CROSSHAIR_REGION_TOP, CROSSHAIR_REGION_RIGHT,
		CROSSHAIR_REGION_BOTTOM);
	d3d8_clear_back_buffer(0.0f, 0.0f, 0.0f, 0.0f);
}

/* the controllers, for the gamepad the game reads (xinput_sdl.c) */
int halo_vr_controllers(struct halo_xr_hand hands[HALO_XR_HAND_COUNT], int *menus)
{
	if (!vr.active || !vr.frame.focused)
		return 0;
	memcpy(hands, vr.frame.hands, sizeof(vr.frame.hands));
	*menus = vr.last_overlay_mode == HALO_XR_OVERLAY_SCREEN;
	return 1;
}

#else

int halo_vr_active(void) { return 0; }
int halo_vr_stereo_frame(void) { return 0; }
int halo_vr_stereo_player(short local_player_index, int cinematic) { (void)local_player_index; (void)cinematic; return 0; }
void halo_vr_eye_camera(int eye, const float game_position[3], float position[3], float forward[3], float up[3],
	float *vertical_field_of_view, float frustum_bounds[4])
{
	(void)eye; (void)game_position; (void)position; (void)forward; (void)up;
	(void)vertical_field_of_view; (void)frustum_bounds;
}
void halo_vr_capture_eye(int eye) { (void)eye; }
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
int halo_vr_weapon_camera(short local_player_index, float position[3], float forward[3], float up[3])
{
	(void)local_player_index; (void)position; (void)forward; (void)up;
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
void halo_vr_set_seat(short local_player_index, int seat, int armed, float vehicle_yaw)
{
	(void)local_player_index; (void)seat; (void)armed; (void)vehicle_yaw;
}
void halo_vr_set_seat_frame(short local_player_index, float vehicle_yaw)
{
	(void)local_player_index; (void)vehicle_yaw;
}
int halo_vr_vehicle_first_person(void) { return 0; }
void halo_vr_set_seat_gun(short local_player_index, const float start[3], const float direction[3])
{
	(void)local_player_index; (void)start; (void)direction;
}
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
int halo_vr_body_frame(short local_player_index, float head[3], float forward[3])
{
	(void)local_player_index; (void)head; (void)forward;
	return 0;
}
int halo_vr_left_hand(short local_player_index, float position[3], float forward[3], float up[3])
{
	(void)local_player_index; (void)position; (void)forward; (void)up;
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
