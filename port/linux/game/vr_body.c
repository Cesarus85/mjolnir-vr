/*
VR_BODY.C

The whole body in VR (vr.body "full"): the player's own biped, which the
first-person view leaves out, drawn for the local player under the
first-person arms and weapon (vr_body.h). render_objects.c draws it among
the world's objects, so that it sorts against the world by depth; the
first-person weapon and arms, drawn before the world into the stencil, stay
over it. Only a copy of its node matrices is posed, once a frame: the
game's own pose (its hit boxes, markers, what other machines see) is left
as it is, and nothing here changes the game.

- The torso faces the torso's heading (vr.c: the head's beyond a cone,
  drawn towards the hands; the stick's turns turn it at once), upright under
  a neck below and behind the eyes, which the head turns about: looking
  down, the chest stays behind the eyes. The hips go down with the physical
  crouch, a little back; leaning, they follow part of the way and the
  spine tilts the rest.
- The hips and legs keep the game's animation (walking, running, crouched
  by the button), turned to the torso's heading. Standing still, the feet
  stay planted and step, one at a time, after the body when it turns far
  enough; the knees bend (two-bone IK, vr_arms.c) as the hips go down.
- The head (with the neck's top) and the third-person arms are shrunk to
  nothing: the first-person arms hang from the body's shoulders instead
  (vr_body_shoulders), and the weapon and all the biped carries are left
  out, the first-person weapon held instead.
- Seated in an open vehicle's passenger seat, the body sits as the seat's
  animation has it, the head and arms shrunk alike; at a wheel or a gun
  render_objects.c draws it whole, its hands on them, as before; in a
  closed seat (a Ghost, a Banshee, the tank) none is drawn.

After HaloCE-Quest-VR (moistman42069, CC0), port/linux/game/vr_render.c,
its vr_render_body_matrices: the neck pivot, the planted feet and their
steps, the leg's clamped reach, rebuilt for this port's skeleton
(vr_arms.c) and its once-a-frame poses.

debug.vr_body (flat, the emulator) poses the body from the flat camera, with
the head turning, crouching or leaning on its own, and can move the camera
to look at it (vr_body_debug_camera).
*/

#ifdef HALO_ANDROID

#include "cseries.h"
#include "math/real_math.h"
#include "objects/objects.h"
#include "objects/object_definitions.h"
#include "objects/object_types.h"
#include "models/models.h"
#include "models/model_definitions.h"
#include "models/model_animation_definitions.h"
#include "units/units.h"
#include "units/vehicles.h"
#include "game/players.h"
#include "camera/director.h"
#include "cutscene/cinematics.h"
#include "render/render.h"
#include "render/render_cameras.h"
#include "halo_vr.h"
#include "vr_arms.h"
#include "vr_body.h"

#include <math.h>
#include <string.h>

void platform_log(char const *format, ...);
char const *config_string(char const *name);
char *tag_get_name(long tag_index);
unsigned long system_milliseconds(void);

/* world units a metre */
#define VR_BODY_UNITS_PER_METRE (1.0f / 3.048f)
#define VR_BODY_PI 3.14159265f

/* the neck: behind and below the eyes in the head's frame (its pitch held
within -60..+30 degrees) */
#define VR_BODY_NECK_BACK 0.14f
#define VR_BODY_NECK_DOWN 0.20f
/* the hips: how far they follow the neck leaning aside (the spine tilts the rest),
at most how far, how far back they go as the body crouches, and how far
down and up at most */
#define VR_BODY_HIPS_FOLLOW 0.35f
#define VR_BODY_LEAN_MOST 0.45f
#define VR_BODY_HIPS_BACK 0.25f
#define VR_BODY_CROUCH_MOST 1.20f
#define VR_BODY_RISE_MOST 0.02f
/* the first-person arms' shoulders: at most and least this far to each
side of the neck (metres) */
#define VR_BODY_SHOULDER_LEAST 0.14f
#define VR_BODY_SHOULDER_MOST 0.24f
/* a planted foot steps when the body turns this far from it, or its place
in the animation is this far away (metres); a step's time and lift */
#define VR_BODY_STEP_TURN (35.0f * VR_BODY_PI / 180.0f)
#define VR_BODY_STEP_GAP 0.18f
#define VR_BODY_STEP_SECONDS 0.22f
#define VR_BODY_STEP_LIFT 0.06f

/* debug.vr_body: the camera, and the head's motion */
enum
{
	_vr_body_debug_off,
	_vr_body_debug_eye,
	_vr_body_debug_down,
	_vr_body_debug_front,
	_vr_body_debug_side,
	_vr_body_debug_back,
};

enum
{
	_vr_body_motion_cycle,
	_vr_body_motion_still,
	_vr_body_motion_turn,
	_vr_body_motion_crouch,
	_vr_body_motion_lean,
	_vr_body_motion_raw,
};

/* what the body is posed from: the eyes (world units), the head's forward
and up, the eyes standing straight, the torso's flat heading, the time */
struct vr_body_input
{
	real_point3d eye;
	real_vector3d head_forward;
	real_vector3d head_up;
	real_point3d standing;
	real_vector3d torso_forward;
	double time;
};

static struct
{
	/* the pose this frame (render.frame_index), of which unit */
	long frame_index;
	long unit_index;
	/* the skeleton and its nodes, found by name for its animation graph */
	long graph_index;
	boolean usable;
	struct vr_skeleton skeleton;
	short spine, neck, head;
	short upper[2], thigh[2], calf[2], foot[2];
	boolean legs[2];

	boolean valid;
	short node_count;
	real_matrix4x3 matrices[MAXIMUM_NODES_PER_ANIMATION];
	real_point3d shoulders[2];
} vr_body = { -1, NONE, NONE };

/* the feet as planted, and the step under way (the foot, NONE) */
static struct
{
	boolean valid;
	long unit_index;
	double time;
	double step_start;
	short step;
	real yaw[2];
	real_matrix4x3 planted[2];
	real_matrix4x3 step_from;
	real_matrix4x3 step_to;
} vr_body_stance;

static struct
{
	int view;
	int motion;
	/* the flat camera this frame, and what the head made of it */
	long frame_index;
	boolean input_valid;
	struct vr_body_input input;
	real torso_yaw;
	boolean torso_valid;
	double torso_time;
} vr_body_debug = { -1 };

/* ---------- small maths */

static boolean vr_body_unit(
	real_vector3d *v)
{
	real length = (real)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);

	if (!(length > 1e-6f && length < 1e8f))
		return FALSE;
	v->i /= length;
	v->j /= length;
	v->k /= length;
	return TRUE;
}

static void vr_body_minus(
	real_point3d const *a,
	real_point3d const *b,
	real_vector3d *out)
{
	out->i = a->x - b->x;
	out->j = a->y - b->y;
	out->k = a->z - b->z;
}

/* a frame (forward, left, up; orthonormal) from an up and a rough left */
static boolean vr_body_frame(
	real_vector3d const *up,
	real_vector3d const *left,
	real_vector3d axes[3])
{
	real along;

	axes[2] = *up;
	if (!vr_body_unit(&axes[2]))
		return FALSE;
	axes[1] = *left;
	along = dot_product3d(&axes[1], &axes[2]);
	axes[1].i -= along * axes[2].i;
	axes[1].j -= along * axes[2].j;
	axes[1].k -= along * axes[2].k;
	if (!vr_body_unit(&axes[1]))
		return FALSE;
	cross_product3d(&axes[1], &axes[2], &axes[0]);
	return TRUE;
}

/* the rotation (column vectors) taking frame from to frame to */
static void vr_body_rotation(
	real_vector3d const from[3],
	real_vector3d const to[3],
	real rotation[3][3])
{
	short row, column, axis;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			rotation[row][column] = 0.0f;
			for (axis = 0; axis < 3; axis++)
				rotation[row][column] += to[axis].n[row] * from[axis].n[column];
		}
	}
}

/* every node turned about the vertical through pivot (cosine, sine) and
moved */
static void vr_body_turn_and_move(
	real_matrix4x3 *matrices,
	short node_count,
	real_point3d const *pivot,
	real c,
	real s,
	real_vector3d const *move)
{
	short node_index;

	for (node_index = 0; node_index < node_count; node_index++)
	{
		real_matrix4x3 *node = &matrices[node_index];
		real x = node->position.x - pivot->x;
		real y = node->position.y - pivot->y;
		real_vector3d *axes[3];
		short axis;

		node->position.x = pivot->x + c * x - s * y + move->i;
		node->position.y = pivot->y + s * x + c * y + move->j;
		node->position.z += move->k;
		axes[0] = &node->forward;
		axes[1] = &node->left;
		axes[2] = &node->up;
		for (axis = 0; axis < 3; axis++)
		{
			real i = axes[axis]->i;
			real j = axes[axis]->j;

			axes[axis]->i = c * i - s * j;
			axes[axis]->j = s * i + c * j;
		}
	}
}

/* a node and all under it turned about the node, but for the legs (in some
skeletons the thighs hang from the spine) */
static void vr_body_turn_torso(
	short root,
	real rotation[3][3])
{
	real_matrix4x3 *matrices = vr_body.matrices;
	real_point3d pivot = matrices[root].position;
	short node_index, side;

	for (node_index = 0; node_index < vr_body.node_count; node_index++)
	{
		real_matrix4x3 *node = &matrices[node_index];
		real_vector3d offset, *axes[3];
		short axis;
		boolean leg = FALSE;

		for (side = 0; side < 2; side++)
			leg |= vr_body.thigh[side] != NONE && vr_skeleton_under(&vr_body.skeleton, node_index, vr_body.thigh[side]);
		if (leg || !vr_skeleton_under(&vr_body.skeleton, node_index, root))
			continue;
		vr_body_minus(&node->position, &pivot, &offset);
		node->position.x = pivot.x + rotation[0][0] * offset.i + rotation[0][1] * offset.j + rotation[0][2] * offset.k;
		node->position.y = pivot.y + rotation[1][0] * offset.i + rotation[1][1] * offset.j + rotation[1][2] * offset.k;
		node->position.z = pivot.z + rotation[2][0] * offset.i + rotation[2][1] * offset.j + rotation[2][2] * offset.k;
		axes[0] = &node->forward;
		axes[1] = &node->left;
		axes[2] = &node->up;
		for (axis = 0; axis < 3; axis++)
		{
			real_vector3d v = *axes[axis];

			axes[axis]->i = rotation[0][0] * v.i + rotation[0][1] * v.j + rotation[0][2] * v.k;
			axes[axis]->j = rotation[1][0] * v.i + rotation[1][1] * v.j + rotation[1][2] * v.k;
			axes[axis]->k = rotation[2][0] * v.i + rotation[2][1] * v.j + rotation[2][2] * v.k;
		}
	}
}

/* ---------- what the body is posed from */

/* the neck the head turns about (after HaloCE-Quest-VR's vr_neck_pivot) */
static void vr_body_neck_pivot(
	struct vr_body_input const *input,
	real_point3d *neck)
{
	real_vector3d yaw;
	real pitch, c, s, back;

	yaw.i = input->head_forward.i - input->head_forward.k * input->head_up.i;
	yaw.j = input->head_forward.j - input->head_forward.k * input->head_up.j;
	yaw.k = 0.0f;
	if (!vr_body_unit(&yaw))
		yaw = input->torso_forward;
	pitch = (real)asin(PIN(input->head_forward.k, -1.0f, 1.0f));
	pitch = PIN(pitch, -60.0f * VR_BODY_PI / 180.0f, 30.0f * VR_BODY_PI / 180.0f);
	c = (real)cos(pitch);
	s = (real)sin(pitch);
	back = (VR_BODY_NECK_BACK * c - VR_BODY_NECK_DOWN * s) * VR_BODY_UNITS_PER_METRE;
	neck->x = input->eye.x - back * yaw.i;
	neck->y = input->eye.y - back * yaw.j;
	neck->z = input->eye.z - (VR_BODY_NECK_BACK * s + VR_BODY_NECK_DOWN * c) * VR_BODY_UNITS_PER_METRE;
}

static void vr_body_debug_settings(
	void)
{
	char const *setting;
	static char const *const views[] = { "off", "eye", "down", "front", "side", "back" };
	static char const *const motions[] = { "cycle", "still", "turn", "crouch", "lean", "raw" };
	short index;

	if (vr_body_debug.view >= 0)
		return;
	vr_body_debug.view = _vr_body_debug_off;
	vr_body_debug.motion = _vr_body_motion_cycle;
	setting = config_string("debug.vr_body");
	if (!setting)
		return;
	for (index = 0; index < (short)NUMBEROF(views); index++)
	{
		size_t length = strlen(views[index]);

		if (!strncmp(setting, views[index], length) && (setting[length] == 0 || setting[length] == ':'))
			vr_body_debug.view = index;
	}
	setting = strchr(setting, ':');
	for (index = 0; setting && index < (short)NUMBEROF(motions); index++)
	{
		if (!strcmp(setting + 1, motions[index]))
			vr_body_debug.motion = index;
	}
	if (vr_body_debug.view != _vr_body_debug_off)
		platform_log("VR body: debug.vr_body view %d, motion %d (flat)", vr_body_debug.view, vr_body_debug.motion);
}

/* debug.vr_body: the head as the flat camera has it, turning, crouching or
leaning on its own (five seconds each, in turn), and the torso after it as
vr.c's torso_update has it (beyond 15 degrees, eased) */
static void vr_body_debug_input(
	struct render_camera const *camera)
{
	struct vr_body_input *input = &vr_body_debug.input;
	double now = (double)system_milliseconds() * 0.001;
	real phase = (real)fmod(now, 15.0);
	int motion = vr_body_debug.motion;
	real yaw, pitch, turn = 0.0f, crouch = 0.0f, lean = 0.0f;
	real cone = 15.0f * VR_BODY_PI / 180.0f;

	if (motion == _vr_body_motion_cycle)
		motion = phase < 5.0f ? _vr_body_motion_turn : phase < 10.0f ? _vr_body_motion_crouch : _vr_body_motion_lean;
	phase = (real)fmod(now, 5.0) / 5.0f;
	if (motion == _vr_body_motion_turn)
		turn = 60.0f * VR_BODY_PI / 180.0f * (real)sin(2.0f * VR_BODY_PI * phase);
	else if (motion == _vr_body_motion_crouch)
		crouch = 0.5f * (0.5f - 0.5f * (real)cos(2.0f * VR_BODY_PI * phase));
	else if (motion == _vr_body_motion_lean)
		lean = 0.3f * (real)sin(2.0f * VR_BODY_PI * phase);
	yaw = (real)atan2(camera->forward.j, camera->forward.i) + turn;
	pitch = (real)asin(PIN(camera->forward.k, -1.0f, 1.0f));
	if (vr_body_debug.view == _vr_body_debug_down)
		pitch = -60.0f * VR_BODY_PI / 180.0f;
	input->head_forward.i = (real)(cos(yaw) * cos(pitch));
	input->head_forward.j = (real)(sin(yaw) * cos(pitch));
	input->head_forward.k = (real)sin(pitch);
	input->head_up.i = (real)(-cos(yaw) * sin(pitch));
	input->head_up.j = (real)(-sin(yaw) * sin(pitch));
	input->head_up.k = (real)cos(pitch);
	input->standing = camera->position;
	input->eye = camera->position;
	input->eye.x += (real)cos(yaw) * lean * VR_BODY_UNITS_PER_METRE;
	input->eye.y += (real)sin(yaw) * lean * VR_BODY_UNITS_PER_METRE;
	input->eye.z -= (crouch + 0.1f * (real)fabs(lean)) * VR_BODY_UNITS_PER_METRE;
	input->time = now;

	if (!vr_body_debug.torso_valid || now - vr_body_debug.torso_time > 0.25 || now < vr_body_debug.torso_time)
	{
		vr_body_debug.torso_yaw = yaw;
	}
	else
	{
		real difference = (real)atan2(sin(yaw - vr_body_debug.torso_yaw), cos(yaw - vr_body_debug.torso_yaw));

		if (fabs(difference) > cone)
		{
			real seconds = (real)MIN(now - vr_body_debug.torso_time, 0.05);

			difference -= difference > 0.0f ? cone : -cone;
			vr_body_debug.torso_yaw += difference * (1.0f - (real)exp(-10.0f * seconds));
		}
	}
	vr_body_debug.torso_valid = TRUE;
	vr_body_debug.torso_time = now;
	input->torso_forward.i = (real)cos(vr_body_debug.torso_yaw);
	input->torso_forward.j = (real)sin(vr_body_debug.torso_yaw);
	input->torso_forward.k = 0.0f;
	vr_body_debug.input_valid = TRUE;
	vr_body_debug.frame_index = render.frame_index;
}

static boolean vr_body_input(
	short local_player_index,
	struct vr_body_input *input)
{
	if (halo_vr_active())
	{
		float eye[3], forward[3], up[3], standing[3], head[3], neck[3], torso[3];

		if (!halo_vr_head_pose(local_player_index, eye, forward, up, standing) ||
			!halo_vr_body_frame(local_player_index, head, neck, torso))
		{
			return FALSE;
		}
		input->eye.x = eye[0];
		input->eye.y = eye[1];
		input->eye.z = eye[2];
		input->head_forward.i = forward[0];
		input->head_forward.j = forward[1];
		input->head_forward.k = forward[2];
		input->head_up.i = up[0];
		input->head_up.j = up[1];
		input->head_up.k = up[2];
		input->standing.x = standing[0];
		input->standing.y = standing[1];
		input->standing.z = standing[2];
		input->torso_forward.i = torso[0];
		input->torso_forward.j = torso[1];
		input->torso_forward.k = 0.0f;
		input->time = halo_vr_pose_time();
		return vr_body_unit(&input->torso_forward) && vr_body_unit(&input->head_forward) &&
			vr_body_unit(&input->head_up);
	}
	if (!vr_body_debug.input_valid || vr_body_debug.frame_index != render.frame_index)
		return FALSE;
	*input = vr_body_debug.input;
	return TRUE;
}

/* ---------- the skeleton */

static boolean vr_body_skeleton(
	long graph_index,
	short node_count)
{
	static char const *const sides[2] = { "l ", "r " };
	short side;

	if (vr_body.graph_index == graph_index && vr_body.skeleton.node_count == node_count)
		return vr_body.usable;
	vr_body.graph_index = graph_index;
	vr_body.usable = FALSE;
	vr_body_stance.valid = FALSE;
	if (!vr_skeleton_build(&vr_body.skeleton, graph_index, node_count))
	{
		vr_body.skeleton.node_count = node_count;
		platform_log("VR body: %s has %d nodes, its model %d: not drawn", tag_get_name(graph_index),
			animation_graph_node_count(graph_index), node_count);
		return FALSE;
	}
	vr_body.spine = animation_graph_find_node(graph_index, "spine");
	vr_body.neck = animation_graph_find_node(graph_index, "neck");
	vr_body.head = animation_graph_find_node(graph_index, "head");
	for (side = 0; side < 2; side++)
	{
		char name[16];

		snprintf(name, sizeof(name), "%supperarm", sides[side]);
		vr_body.upper[side] = animation_graph_find_node(graph_index, name);
		snprintf(name, sizeof(name), "%sthigh", sides[side]);
		vr_body.thigh[side] = animation_graph_find_node(graph_index, name);
		snprintf(name, sizeof(name), "%scalf", sides[side]);
		vr_body.calf[side] = animation_graph_find_node(graph_index, name);
		snprintf(name, sizeof(name), "%sfoot", sides[side]);
		vr_body.foot[side] = animation_graph_find_node(graph_index, name);
		vr_body.legs[side] = vr_body.thigh[side] != NONE && vr_body.calf[side] != NONE && vr_body.foot[side] != NONE &&
			vr_skeleton_under(&vr_body.skeleton, vr_body.calf[side], vr_body.thigh[side]) &&
			vr_skeleton_under(&vr_body.skeleton, vr_body.foot[side], vr_body.calf[side]);
	}
	vr_body.usable = vr_body.spine != NONE && vr_body.neck != NONE && vr_body.head != NONE &&
		vr_body.upper[0] != NONE && vr_body.upper[1] != NONE &&
		vr_skeleton_under(&vr_body.skeleton, vr_body.neck, vr_body.spine) &&
		!vr_skeleton_under(&vr_body.skeleton, vr_body.neck, vr_body.head);
	platform_log("VR body: %s, %d nodes: spine %d, neck %d, head %d, upper arms %d %d, legs %s %s%s",
		tag_get_name(graph_index), node_count, vr_body.spine, vr_body.neck, vr_body.head, vr_body.upper[0],
		vr_body.upper[1], vr_body.legs[0] ? "yes" : "no", vr_body.legs[1] ? "yes" : "no",
		vr_body.usable ? "" : ": not drawn");
	if (vr_body_debug.view > _vr_body_debug_off)
	{
		short node_index;

		for (node_index = 0; node_index < node_count; node_index++)
		{
			platform_log("VR body:   node %d '%s', parent %d", node_index,
				animation_graph_node_name(graph_index, node_index), vr_body.skeleton.parents[node_index]);
		}
	}
	return vr_body.usable;
}

/* ---------- the pose */

/* standing still, the feet stay where they were (planted); one at a time
they step to where the animation, turned with the body, has them, when the
body has turned far from them or that place is far; walking, running or in
the air, they are the animation's (after HaloCE-Quest-VR's
vr_body_plant_feet). The feet's places (world units) for the legs' IK. */
static void vr_body_plant_feet(
	long unit_index,
	struct object_datum const *object,
	real torso_yaw,
	double now,
	real_point3d feet[2])
{
	real_matrix4x3 *matrices = vr_body.matrices;
	real_vector3d const *velocity = &object->object.translational_velocity;
	real speed = (real)sqrt(velocity->i * velocity->i + velocity->j * velocity->j);
	boolean moving = speed > 0.006f * VR_BODY_UNITS_PER_METRE ||
		fabs(velocity->k) > 0.015f * VR_BODY_UNITS_PER_METRE;
	short side;

	if (!vr_body.legs[0] || !vr_body.legs[1])
	{
		vr_body_stance.valid = FALSE;
		return;
	}
	if (!vr_body_stance.valid || vr_body_stance.unit_index != unit_index || now < vr_body_stance.time ||
		now - vr_body_stance.time > 0.25 || moving)
	{
		vr_body_stance.valid = TRUE;
		vr_body_stance.unit_index = unit_index;
		vr_body_stance.step = NONE;
		for (side = 0; side < 2; side++)
		{
			vr_body_stance.planted[side] = matrices[vr_body.foot[side]];
			vr_body_stance.yaw[side] = torso_yaw;
		}
	}
	vr_body_stance.time = now;
	if (!moving)
	{
		if (vr_body_stance.step == NONE)
		{
			real most = 1.0f;

			for (side = 0; side < 2; side++)
			{
				real_vector3d gap;
				real turn = (real)fabs(atan2(sin(torso_yaw - vr_body_stance.yaw[side]),
					cos(torso_yaw - vr_body_stance.yaw[side])));
				real need;

				vr_body_minus(&matrices[vr_body.foot[side]].position, &vr_body_stance.planted[side].position, &gap);
				need = MAX(magnitude3d(&gap) / (VR_BODY_STEP_GAP * VR_BODY_UNITS_PER_METRE), turn / VR_BODY_STEP_TURN);
				if (need > most)
				{
					most = need;
					vr_body_stance.step = side;
				}
			}
			if (vr_body_stance.step != NONE)
			{
				side = vr_body_stance.step;
				vr_body_stance.step_start = now;
				vr_body_stance.step_from = vr_body_stance.planted[side];
				vr_body_stance.yaw[side] = torso_yaw;
				if (vr_body_debug.view > _vr_body_debug_off)
					platform_log("VR body: the %s foot steps (%.1f times as far as it may)", side ? "right" : "left", most);
			}
		}
		if (vr_body_stance.step != NONE)
		{
			/* (to where the animation has the foot now: the body may turn on
			while it steps) */
			real t = PIN((real)((now - vr_body_stance.step_start) / VR_BODY_STEP_SECONDS), 0.0f, 1.0f);
			real eased = t * t * (3.0f - 2.0f * t);
			real_matrix4x3 const *from = &vr_body_stance.step_from;
			real_matrix4x3 const *to = &matrices[vr_body.foot[vr_body_stance.step]];
			real_matrix4x3 *foot = &vr_body_stance.planted[vr_body_stance.step];
			short axis;

			for (axis = 0; axis < 3; axis++)
			{
				foot->position.n[axis] = from->position.n[axis] + (to->position.n[axis] - from->position.n[axis]) * eased;
				foot->forward.n[axis] = from->forward.n[axis] + (to->forward.n[axis] - from->forward.n[axis]) * eased;
				foot->up.n[axis] = from->up.n[axis] + (to->up.n[axis] - from->up.n[axis]) * eased;
			}
			foot->position.z += (real)sin(t * VR_BODY_PI) * VR_BODY_STEP_LIFT * VR_BODY_UNITS_PER_METRE;
			if (vr_body_unit(&foot->forward))
			{
				cross_product3d(&foot->up, &foot->forward, &foot->left);
				if (vr_body_unit(&foot->left))
					cross_product3d(&foot->forward, &foot->left, &foot->up);
			}
			if (t >= 1.0f)
			{
				*foot = *to;
				vr_body_stance.yaw[vr_body_stance.step] = torso_yaw;
				vr_body_stance.step = NONE;
			}
		}
	}
	for (side = 0; side < 2; side++)
	{
		if (moving)
			vr_body_stance.planted[side] = matrices[vr_body.foot[side]];
		feet[side] = vr_body_stance.planted[side].position;
	}
}

/* a leg bent (two-bone IK) for its foot to reach its planted place, never
lengthened (an unreachable foot stops short), the knee forward; the foot
turned as planted (after HaloCE-Quest-VR's vr_body_leg) */
static void vr_body_leg(
	short side,
	real_point3d const *foot,
	real_vector3d const *forward)
{
	real_matrix4x3 *matrices = vr_body.matrices;
	short bones[NUMBER_OF_VR_ARM_BONES];
	real_vector3d thigh, calf, reach, pole, from[3], to[3];
	real thigh_length, calf_length, distance, shortest, longest;
	real_point3d target;
	real rotation[3][3];
	real outward = side ? -1.0f : 1.0f; /* the left leg's out is +left */

	bones[_vr_arm_upper] = vr_body.thigh[side];
	bones[_vr_arm_fore] = vr_body.calf[side];
	bones[_vr_arm_hand] = vr_body.foot[side];
	vr_body_minus(&matrices[bones[_vr_arm_fore]].position, &matrices[bones[_vr_arm_upper]].position, &thigh);
	vr_body_minus(&matrices[bones[_vr_arm_hand]].position, &matrices[bones[_vr_arm_fore]].position, &calf);
	thigh_length = magnitude3d(&thigh);
	calf_length = magnitude3d(&calf);
	shortest = (real)fabs(thigh_length - calf_length) + 0.002f;
	longest = (thigh_length + calf_length) * 0.998f;
	vr_body_minus(foot, &matrices[bones[_vr_arm_upper]].position, &reach);
	distance = magnitude3d(&reach);
	if (!(distance > 1e-4f && distance < 1e4f) || longest <= shortest)
		return;
	distance = PIN(distance, shortest, longest) / distance;
	target.x = matrices[bones[_vr_arm_upper]].position.x + reach.i * distance;
	target.y = matrices[bones[_vr_arm_upper]].position.y + reach.j * distance;
	target.z = matrices[bones[_vr_arm_upper]].position.z + reach.k * distance;
	pole.i = forward->i - forward->j * outward * 0.15f;
	pole.j = forward->j + forward->i * outward * 0.15f;
	pole.k = 0.0f;
	vr_solve_arm(&vr_body.skeleton, matrices, bones, matrices[bones[_vr_arm_upper]].position, &target, &pole, FALSE,
		NULL);
	from[0] = matrices[bones[_vr_arm_hand]].forward;
	from[1] = matrices[bones[_vr_arm_hand]].left;
	from[2] = matrices[bones[_vr_arm_hand]].up;
	to[0] = vr_body_stance.planted[side].forward;
	to[1] = vr_body_stance.planted[side].left;
	to[2] = vr_body_stance.planted[side].up;
	vr_body_rotation(from, to, rotation);
	vr_turn_subtree(&vr_body.skeleton, matrices, bones[_vr_arm_hand], rotation);
}

/* the body's own pose (the game's, between the last two ticks) turned,
moved and bent to the head, its head and arms shrunk */
static boolean vr_body_pose(
	long unit_index,
	struct vr_body_input const *input,
	boolean seated)
{
	struct object_datum *object = object_get(unit_index);
	real_matrix4x3 *matrices = vr_body.matrices;
	real_point3d neck_target, collapse[3];
	real_vector3d left;
	short node_index, side;

	csmemcpy(matrices, object_get_node_matrices(unit_index), vr_body.node_count * sizeof(real_matrix4x3));
	left.i = -input->torso_forward.j;
	left.j = input->torso_forward.i;
	left.k = 0.0f;
	vr_body_neck_pivot(input, &neck_target);

	/* (debug.vr_body "...:raw": the game's pose, only shrunk) */
	if (!seated && !(vr_body_debug.view > _vr_body_debug_off && vr_body_debug.motion == _vr_body_motion_raw))
	{
		real_matrix4x3 const *tick = (real_matrix4x3 const *)object_header_block_get(unit_index,
			&object->object.node_matrices);
		real_vector3d facing = object->object.forward;
		real_vector3d model_forward, move;
		real along[3], c, s, drop, horizontal;
		real_point3d pivot = matrices[0].position;
		real_point3d standing_neck, feet[2];
		real torso_yaw = (real)atan2(input->torso_forward.j, input->torso_forward.i);

		/* the body turned from the way it faces (as drawn: the facing in the
		root's own axes at the tick, those axes now) to the torso's heading */
		along[0] = dot_product3d(&facing, &tick[0].forward);
		along[1] = dot_product3d(&facing, &tick[0].left);
		along[2] = dot_product3d(&facing, &tick[0].up);
		model_forward.i = matrices[0].forward.i * along[0] + matrices[0].left.i * along[1] + matrices[0].up.i * along[2];
		model_forward.j = matrices[0].forward.j * along[0] + matrices[0].left.j * along[1] + matrices[0].up.j * along[2];
		model_forward.k = 0.0f;
		if (!vr_body_unit(&model_forward))
			return FALSE;
		c = dot_product3d(&model_forward, &input->torso_forward);
		s = model_forward.i * input->torso_forward.j - model_forward.j * input->torso_forward.i;
		move.i = move.j = move.k = 0.0f;
		vr_body_turn_and_move(matrices, vr_body.node_count, &pivot, c, s, &move);
		/* and moved for its neck to be where the neck is standing straight
		(behind the game's camera, whose eyes the body's model has a little
		further forward) */
		standing_neck.x = input->standing.x - input->torso_forward.i * VR_BODY_NECK_BACK * VR_BODY_UNITS_PER_METRE;
		standing_neck.y = input->standing.y - input->torso_forward.j * VR_BODY_NECK_BACK * VR_BODY_UNITS_PER_METRE;
		move.i = standing_neck.x - matrices[vr_body.neck].position.x;
		move.j = standing_neck.y - matrices[vr_body.neck].position.y;
		vr_body_turn_and_move(matrices, vr_body.node_count, &pivot, 1.0f, 0.0f, &move);

		/* the feet, where they stand */
		vr_body_plant_feet(unit_index, object, torso_yaw, input->time, feet);

		/* the hips down as far as the neck is below where it is standing
		straight, and back a little; leaning, aside part of the way (the
		spine tilts the rest) */
		drop = neck_target.z - (input->standing.z - VR_BODY_NECK_DOWN * VR_BODY_UNITS_PER_METRE);
		drop = PIN(drop, -VR_BODY_CROUCH_MOST * VR_BODY_UNITS_PER_METRE, VR_BODY_RISE_MOST * VR_BODY_UNITS_PER_METRE);
		move.i = neck_target.x - standing_neck.x;
		move.j = neck_target.y - standing_neck.y;
		horizontal = (real)sqrt(move.i * move.i + move.j * move.j);
		if (horizontal > VR_BODY_LEAN_MOST * VR_BODY_UNITS_PER_METRE)
		{
			move.i *= VR_BODY_LEAN_MOST * VR_BODY_UNITS_PER_METRE / horizontal;
			move.j *= VR_BODY_LEAN_MOST * VR_BODY_UNITS_PER_METRE / horizontal;
		}
		move.i = move.i * VR_BODY_HIPS_FOLLOW + input->torso_forward.i * drop * VR_BODY_HIPS_BACK;
		move.j = move.j * VR_BODY_HIPS_FOLLOW + input->torso_forward.j * drop * VR_BODY_HIPS_BACK;
		move.k = drop;
		if (vr_body_debug.view > _vr_body_debug_off)
		{
			/* (debug.vr_body: once a second, how the pose was moved) */
			static unsigned long logged;
			unsigned long now_ms = system_milliseconds();

			if (now_ms - logged > 1000)
			{
				real m = 1.0f / VR_BODY_UNITS_PER_METRE;

				logged = now_ms;
				platform_log("VR body: turned %.0f deg; moved %.2f %.2f %.2f m; neck %.2f m below the eyes, "
					"%.2f m behind; target %.2f below, pelvis %.2f m below; feet %.2f %.2f / %.2f %.2f m from the pelvis",
					atan2(s, c) * 180.0f / VR_BODY_PI, move.i * m, move.j * m, move.k * m,
					(input->standing.z - matrices[vr_body.neck].position.z) * m,
					-((matrices[vr_body.neck].position.x - input->standing.x) * input->torso_forward.i +
					(matrices[vr_body.neck].position.y - input->standing.y) * input->torso_forward.j) * m,
					(input->eye.z - neck_target.z) * m, (input->standing.z - matrices[0].position.z) * m,
					(feet[0].x - matrices[0].position.x) * m, (feet[0].y - matrices[0].position.y) * m,
					(feet[1].x - matrices[0].position.x) * m, (feet[1].y - matrices[0].position.y) * m);
			}
		}
		vr_body_turn_and_move(matrices, vr_body.node_count, &pivot, 1.0f, 0.0f, &move);

		/* the torso upright from the spine to the neck, square to the
		torso's heading (whatever the aiming's animation made of it) */
		{
			real_vector3d up, across, from[3], to[3];
			real rotation[3][3];

			vr_body_minus(&matrices[vr_body.neck].position, &matrices[vr_body.spine].position, &up);
			vr_body_minus(&matrices[vr_body.upper[0]].position, &matrices[vr_body.upper[1]].position, &across);
			if (vr_body_frame(&up, &across, from))
			{
				vr_body_minus(&neck_target, &matrices[vr_body.spine].position, &up);
				if (vr_body_frame(&up, &left, to))
				{
					vr_body_rotation(from, to, rotation);
					vr_body_turn_torso(vr_body.spine, rotation);
				}
			}
		}

		/* the legs bent to the feet */
		if (vr_body_stance.valid)
		{
			for (side = 0; side < 2; side++)
				vr_body_leg(side, &feet[side], &input->torso_forward);
		}
		if (vr_body_debug.view > _vr_body_debug_off)
		{
			static unsigned long logged;
			unsigned long now_ms = system_milliseconds();

			if (now_ms - logged > 1000)
			{
				real_vector3d f = input->torso_forward;
				real m = 1.0f / VR_BODY_UNITS_PER_METRE;
				real_point3d const *p = &matrices[0].position;
				real_point3d const *n = &matrices[vr_body.neck].position;
				real_point3d const *k = &matrices[vr_body.calf[0]].position;

				logged = now_ms;
				platform_log("VR body: posed: neck %.2f m ahead of the pelvis, %.2f above; left knee %.2f ahead, "
					"%.2f above; eye %.2f ahead of the neck; shoulders %.2f apart, %.2f below the neck",
					((n->x - p->x) * f.i + (n->y - p->y) * f.j) * m, (n->z - p->z) * m,
					((k->x - p->x) * f.i + (k->y - p->y) * f.j) * m, (k->z - p->z) * m,
					((input->eye.x - n->x) * f.i + (input->eye.y - n->y) * f.j) * m,
					distance3d(&matrices[vr_body.upper[0]].position, &matrices[vr_body.upper[1]].position) * m,
					(n->z - 0.5f * (matrices[vr_body.upper[0]].position.z + matrices[vr_body.upper[1]].position.z)) * m);
			}
		}
	}

	/* the shoulders, for the first-person arms; then the head and the neck
	shrunk onto the neck's root, each arm onto its shoulder */
	{
		/* (from the neck: below it, a little behind, to each side as far
		as the body's shoulder joints are, within 14 to 24 cm, which the aim's
		animation can shrug; as HaloCE-Quest-VR has them) */
		real_vector3d up, across, chest[3];
		real width;

		vr_body_minus(&matrices[vr_body.neck].position, &matrices[vr_body.spine].position, &up);
		vr_body_minus(&matrices[vr_body.upper[0]].position, &matrices[vr_body.upper[1]].position, &across);
		width = PIN(0.5f * magnitude3d(&across), VR_BODY_SHOULDER_LEAST * VR_BODY_UNITS_PER_METRE,
			VR_BODY_SHOULDER_MOST * VR_BODY_UNITS_PER_METRE);
		if (!vr_body_frame(&up, &across, chest))
			return FALSE;
		for (side = 0; side < 2; side++)
		{
			real_point3d *shoulder = &vr_body.shoulders[side];

			*shoulder = matrices[vr_body.neck].position;
			point_from_line3d(shoulder, &chest[1], side ? -width : width, shoulder);
			point_from_line3d(shoulder, &chest[0], -0.02f * VR_BODY_UNITS_PER_METRE, shoulder);
			point_from_line3d(shoulder, &chest[2], -0.03f * VR_BODY_UNITS_PER_METRE, shoulder);
		}
	}
	collapse[0] = matrices[vr_body.neck].position;
	collapse[1] = matrices[vr_body.upper[0]].position;
	collapse[2] = matrices[vr_body.upper[1]].position;
	for (node_index = 0; node_index < vr_body.node_count; node_index++)
	{
		short part = node_index == vr_body.neck || vr_skeleton_under(&vr_body.skeleton, node_index, vr_body.head) ? 0 :
			vr_skeleton_under(&vr_body.skeleton, node_index, vr_body.upper[0]) ? 1 :
			vr_skeleton_under(&vr_body.skeleton, node_index, vr_body.upper[1]) ? 2 : NONE;

		if (!isfinite(matrices[node_index].position.x) || !isfinite(matrices[node_index].position.y) ||
			!isfinite(matrices[node_index].position.z))
		{
			return FALSE;
		}
		if (part != NONE)
		{
			matrices[node_index].scale = 0.0f;
			matrices[node_index].position = collapse[part];
		}
	}
	return TRUE;
}

/* whether the local player's own unit is drawn whole this frame, and if it
sits (a passenger's seat in an open vehicle) */
static boolean vr_body_wanted(
	short local_player_index,
	long object_index,
	boolean *seated)
{
	long player_index;
	struct object_datum *object;

	*seated = FALSE;
	if (local_player_index != 0)
		return FALSE;
	if (halo_vr_active())
	{
		if (halo_vr_body_mode() != HALO_VR_BODY_FULL || !halo_vr_stereo_frame())
			return FALSE;
	}
	else
	{
		vr_body_debug_settings();
		if (vr_body_debug.view == _vr_body_debug_off)
			return FALSE;
	}
	player_index = local_player_get_player_index(local_player_index);
	if (player_index == NONE || player_get(player_index)->unit_index != object_index ||
		director_get_perspective(local_player_index) != _director_perspective_first_person ||
		cinematic_in_progress() || render.camera.mirrored)
	{
		return FALSE;
	}
	object = object_get(object_index);
	if (object->object.type != _object_type_biped || TEST_FLAG(object->object.damage_flags, _object_dead_bit) ||
		TEST_FLAG(object->object.flags, _object_invisible_bit))
	{
		return FALSE;
	}
	if (object->object.parent_object_index != NONE)
	{
		struct object_datum *parent = object_get(object->object.parent_object_index);

		/* (at a wheel or a gun render_objects.c draws it whole; in a closed
		seat, none) */
		if (render_object_vr_at_controls(object_index) ||
			(TEST_FLAG(_object_mask_vehicle, parent->object.type) &&
				vehicle_vr_encloses_riders(object->object.parent_object_index)))
		{
			return FALSE;
		}
		*seated = TRUE;
	}
	return TRUE;
}

/* the pose this frame, built once (every eye, the scope's view and the
first-person arms take it) */
static boolean vr_body_build(
	short local_player_index,
	long unit_index)
{
	struct vr_body_input input;
	struct object_datum *object;
	struct object_definition *definition;
	boolean seated;
	unsigned long long timing;

	if (vr_body.frame_index == render.frame_index && vr_body.unit_index == unit_index)
		return vr_body.valid;
	vr_body.frame_index = render.frame_index;
	vr_body.unit_index = unit_index;
	vr_body.valid = FALSE;
	if (unit_index == NONE || !vr_body_wanted(local_player_index, unit_index, &seated) ||
		!vr_body_input(local_player_index, &input))
	{
		return FALSE;
	}
	timing = halo_vr_timing_start();
	object = object_get(unit_index);
	definition = object_definition_get(object->definition_index);
	if (definition->object.model.index == NONE || definition->object.animation_graph.index == NONE)
		return FALSE;
	vr_body.node_count = model_definition_get(definition->object.model.index)->nodes.count;
	if (vr_body.node_count > 0 && vr_body.node_count <= MAXIMUM_NODES_PER_ANIMATION &&
		vr_body_skeleton(definition->object.animation_graph.index, vr_body.node_count))
	{
		vr_body.valid = vr_body_pose(unit_index, &input, seated);
	}
	halo_vr_timing_end(HALO_VR_TIMING_BODY, timing);
	return vr_body.valid;
}

/* ---------- public code */

boolean vr_body_drawn(
	short local_player_index,
	long object_index)
{
	return vr_body_build(local_player_index, object_index);
}

real_matrix4x3 *vr_body_node_matrices(
	long object_index)
{
	return vr_body.valid && vr_body.unit_index == object_index && vr_body.frame_index == render.frame_index ?
		vr_body.matrices : object_get_node_matrices(object_index);
}

boolean vr_body_shoulders(
	short local_player_index,
	real_point3d shoulders[2])
{
	long player_index = local_player_index == NONE ? NONE : local_player_get_player_index(local_player_index);

	if (player_index == NONE || !vr_body_build(local_player_index, player_get(player_index)->unit_index))
		return FALSE;
	shoulders[0] = vr_body.shoulders[0];
	shoulders[1] = vr_body.shoulders[1];
	return TRUE;
}

/* the camera looking from target's side (world units: away from it, up),
at it */
static void vr_body_debug_look(
	struct render_camera *camera,
	real_point3d const *from,
	real_point3d const *at)
{
	real_vector3d forward, up;
	real along;

	vr_body_minus(at, from, &forward);
	if (!vr_body_unit(&forward))
		return;
	up.i = up.j = 0.0f;
	up.k = 1.0f;
	along = dot_product3d(&up, &forward);
	up.i -= along * forward.i;
	up.j -= along * forward.j;
	up.k -= along * forward.k;
	if (!vr_body_unit(&up))
		return;
	camera->position = *from;
	camera->forward = forward;
	camera->up = up;
}

boolean vr_body_debug_camera(
	short local_player_index,
	struct render_camera *render_camera,
	struct render_camera *rasterizer_camera)
{
	struct vr_body_input const *input = &vr_body_debug.input;
	real_point3d from, at;
	real_vector3d left;
	real m = VR_BODY_UNITS_PER_METRE;

	vr_body_debug_settings();
	if (local_player_index != 0 || halo_vr_active() || vr_body_debug.view == _vr_body_debug_off)
		return FALSE;
	vr_body_debug_input(render_camera);
	left.i = -input->torso_forward.j;
	left.j = input->torso_forward.i;
	left.k = 0.0f;
	from = input->eye;
	at = input->eye;
	switch (vr_body_debug.view)
	{
	case _vr_body_debug_down:
		/* (as the headset looks down) */
		render_camera->position = input->eye;
		render_camera->forward = input->head_forward;
		render_camera->up = input->head_up;
		/* (and about as wide as the headset's view) */
		render_camera->vertical_field_of_view = 90.0f * VR_BODY_PI / 180.0f;
		rasterizer_camera->vertical_field_of_view = render_camera->vertical_field_of_view;
		break;
	case _vr_body_debug_front:
		point_from_line3d(&from, &input->torso_forward, 2.6f * m, &from);
		from.z -= 0.6f * m;
		break;
	case _vr_body_debug_side:
		point_from_line3d(&from, &left, 2.6f * m, &from);
		from.z -= 0.6f * m;
		break;
	case _vr_body_debug_back:
		point_from_line3d(&from, &input->torso_forward, -2.6f * m, &from);
		from.z += 0.1f * m;
		break;
	default:
		return FALSE;
	}
	if (vr_body_debug.view != _vr_body_debug_down)
	{
		at.z -= 0.9f * m;
		vr_body_debug_look(render_camera, &from, &at);
	}
	rasterizer_camera->position = render_camera->position;
	rasterizer_camera->forward = render_camera->forward;
	rasterizer_camera->up = render_camera->up;
	return TRUE;
}

boolean vr_body_debug_hides_first_person(
	void)
{
	vr_body_debug_settings();
	return !halo_vr_active() && vr_body_debug.view >= _vr_body_debug_front;
}

#endif
