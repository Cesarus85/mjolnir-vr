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

#define METRES_PER_WORLD_UNIT 3.048f
#define PI 3.14159265358979f
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

	float body_yaw;
	int aim_synced;
	float last_yaw;
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
} vr;

/* d3d8_gl.c */
unsigned int d3d8_back_buffer_texture(void);
void d3d8_clear_back_buffer(float red, float green, float blue, float alpha);

/* ---------- math */

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
	if (vr.smooth_turn)
	{
		if (fabsf(x) > 0.15f)
			vr.body_yaw -= x * vr.smooth_turn_degrees_per_second * PI / 180.0f * seconds;
	}
	else if (!vr.snap_latched && fabsf(x) > 0.7f)
	{
		vr.body_yaw -= (x > 0.0f ? 1.0f : -1.0f) * vr.snap_turn_degrees * PI / 180.0f;
		vr.snap_latched = 1;
	}
	else if (fabsf(x) < 0.3f)
	{
		vr.snap_latched = 0;
	}
	vr.body_yaw = wrap_angle(vr.body_yaw);
}

void halo_vr_frame_begin(void)
{
	if (!vr.active || vr.frame_begun)
		return;
	if (!host_xr_begin_frame(&vr.frame))
		memset(&vr.frame, 0, sizeof(vr.frame));
	vr.frame_begun = 1;
	vr.eyes_captured = 0;
	vr.overlay_mode = HALO_XR_OVERLAY_SCREEN;
	vr.overlay_blend = 0;
	if (vr.frame.recentred)
		vr.aim_synced = 0;
	turn_update();
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
	host_xr_copy_layer(HALO_XR_LAYER_OVERLAY, back_buffer_texture, !vr.overlay_blend);
	host_xr_end_frame(&submit);
	vr.last_overlay_mode = vr.overlay_mode;
	vr.frame_begun = 0;
}

/* ---------- the game's side */

int halo_vr_stereo_frame(void)
{
	return vr.active && vr.frame_begun && vr.frame.session_running && vr.frame.should_render &&
		(vr.frame.views[0].flags & HALO_XR_POSE_VALID) && (vr.frame.views[1].flags & HALO_XR_POSE_VALID);
}

int halo_vr_stereo_player(short local_player_index, int cinematic)
{
	return halo_vr_stereo_frame() && local_player_index == 0 && (!cinematic || vr.cinematics_3d);
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

	local_to_game(view->pose.position, vr.body_yaw, offset);
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
	if (!vr.aim_synced)
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
	if (*yaw < 0.0f)
		*yaw += 2.0f * PI;
	*pitch = head_pitch;
	vr.last_yaw = *yaw;
	return 1;
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

#endif
