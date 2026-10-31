/*
VR_AVATARS.C

Other VR players drawn after their heads and hands (vr_avatars.h): a
player of another machine who plays in VR sends their head and hands
(network_vr_avatars.c), and their biped is drawn with its head turned the
way theirs looks and its arms bent (two-bone IK, vr_arms.c) to where their
hands are, the weapon in its right hand carried with that hand. The
model's right hand takes the weapon hand, its left the free one, whichever
hand the player holds the weapon in.

Only a copy of the node matrices is posed, once a frame for each such
player: the game's own pose (its hit boxes, markers, what the host checks)
is left as it is, and nothing here changes the game. On foot only: seated,
a player is drawn as the game poses them.
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
#include "game/players.h"
#include "render/render.h"
#include "halo_vr.h"
#include "vr_arms.h"
#include "vr_avatars.h"

#include <math.h>
#include <string.h>

void platform_log(char const *format, ...);
char *tag_get_name(long tag_index);

/* ---------- constants */

/* world units a metre */
#define VR_AVATAR_UNITS_PER_METRE (1.0f / 3.048f)
/* the wrist behind the controller's grip, along where it points (metres) */
#define VR_AVATAR_WRIST_BEHIND_GRIP 0.07f
/* the head turned at most this far from where the game has it look */
#define VR_AVATAR_HEAD_MOST (70.0f * 3.14159265f / 180.0f)
/* the posed bodies kept a frame, and the skeletons */
#define VR_AVATAR_POSES 8
#define VR_AVATAR_SKELETONS 4

enum
{
	_vr_avatar_left,
	_vr_avatar_right,
	NUMBER_OF_VR_AVATAR_SIDES
};

/* ---------- globals */

/* a skeleton's nodes, found by name for its animation graph */
static struct vr_avatar_skeleton
{
	long graph_index;
	short node_count;
	boolean usable;
	struct vr_skeleton skeleton;
	short head;
	short arms[NUMBER_OF_VR_AVATAR_SIDES][NUMBER_OF_VR_ARM_BONES];
} vr_avatar_skeletons[VR_AVATAR_SKELETONS];
static short vr_avatar_skeleton_next;

/* a body posed this frame (render.frame_index), and how far its weapon hand
moved, which the weapon it holds is moved by */
static struct vr_avatar_pose
{
	long object_index;
	long frame_index;
	boolean valid;
	short node_count;
	real_matrix4x3 matrices[MAXIMUM_NODES_PER_ANIMATION];
	real_vector3d weapon_moved;
} vr_avatar_poses[VR_AVATAR_POSES];
static short vr_avatar_pose_next;

/* copies of what a posed body holds (its weapon), moved with the hand that
holds it: a few for each pose, kept for the frame (the transparent parts
are drawn after the list) */
#define VR_AVATAR_HELD 3
static struct
{
	long object_index;
	long frame_index;
	real_matrix4x3 matrices[MAXIMUM_NODES_PER_MODEL];
} vr_avatar_held[VR_AVATAR_POSES][VR_AVATAR_HELD];

/* ---------- private code */

static struct vr_avatar_skeleton *vr_avatar_skeleton(
	long graph_index,
	short node_count)
{
	static char const *const sides[NUMBER_OF_VR_AVATAR_SIDES] = { "l ", "r " };
	static char const *const bones[NUMBER_OF_VR_ARM_BONES] = { "upperarm", "forearm", "hand" };
	struct vr_avatar_skeleton *skeleton;
	short index, side, bone;

	for (index = 0; index < VR_AVATAR_SKELETONS; index++)
	{
		skeleton = &vr_avatar_skeletons[index];
		if (skeleton->node_count > 0 && skeleton->graph_index == graph_index && skeleton->node_count == node_count)
			return skeleton->usable ? skeleton : NULL;
	}
	skeleton = &vr_avatar_skeletons[vr_avatar_skeleton_next++ % VR_AVATAR_SKELETONS];
	csmemset(skeleton, 0, sizeof(*skeleton));
	skeleton->graph_index = graph_index;
	skeleton->node_count = node_count;
	if (!vr_skeleton_build(&skeleton->skeleton, graph_index, node_count))
	{
		platform_log("VR avatars: %s (%d nodes) cannot be posed", tag_get_name(graph_index), node_count);
		return NULL;
	}
	skeleton->head = animation_graph_find_node(graph_index, "head");
	skeleton->usable = skeleton->head != NONE;
	for (side = 0; side < NUMBER_OF_VR_AVATAR_SIDES; side++)
	{
		for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
		{
			char name[16];

			snprintf(name, sizeof(name), "%s%s", sides[side], bones[bone]);
			skeleton->arms[side][bone] = animation_graph_find_node(graph_index, name);
			if (skeleton->arms[side][bone] == NONE)
				skeleton->usable = FALSE;
		}
		if (skeleton->usable &&
			(!vr_skeleton_under(&skeleton->skeleton, skeleton->arms[side][_vr_arm_fore],
				skeleton->arms[side][_vr_arm_upper]) ||
			!vr_skeleton_under(&skeleton->skeleton, skeleton->arms[side][_vr_arm_hand],
				skeleton->arms[side][_vr_arm_fore])))
		{
			skeleton->usable = FALSE;
		}
	}
	platform_log("VR avatars: %s, %d nodes: head %d, arms %d %d %d / %d %d %d%s", tag_get_name(graph_index),
		node_count, skeleton->head, skeleton->arms[0][0], skeleton->arms[0][1], skeleton->arms[0][2],
		skeleton->arms[1][0], skeleton->arms[1][1], skeleton->arms[1][2], skeleton->usable ? "" : ": not posed");
	return skeleton->usable ? skeleton : NULL;
}

/* the head turned from where the game has it look to where the player's
looks, at most VR_AVATAR_HEAD_MOST */
static void vr_avatar_turn_head(
	struct vr_avatar_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	real_vector3d const *looking,
	real_vector3d const *forward)
{
	real_vector3d from = *looking, to = *forward, across;
	real rotation[3][3];
	real along, angle;

	if (normalize3d(&from) < 1e-4f || normalize3d(&to) < 1e-4f)
		return;
	along = PIN(dot_product3d(&from, &to), -1.0f, 1.0f);
	angle = (real)acos(along);
	if (angle > VR_AVATAR_HEAD_MOST)
	{
		across.i = to.i - from.i * along;
		across.j = to.j - from.j * along;
		across.k = to.k - from.k * along;
		if (normalize3d(&across) < 1e-4f)
			return;
		to.i = from.i * (real)cos(VR_AVATAR_HEAD_MOST) + across.i * (real)sin(VR_AVATAR_HEAD_MOST);
		to.j = from.j * (real)cos(VR_AVATAR_HEAD_MOST) + across.j * (real)sin(VR_AVATAR_HEAD_MOST);
		to.k = from.k * (real)cos(VR_AVATAR_HEAD_MOST) + across.k * (real)sin(VR_AVATAR_HEAD_MOST);
	}
	vr_rotation_between(&from, &to, rotation);
	vr_turn_subtree(&skeleton->skeleton, matrices, skeleton->head, rotation);
}

/* an arm bent for its hand (the wrist) to reach the controller: the hand
and all it carries moved there as the animation turns them; how far the
hand moved */
static void vr_avatar_reach(
	struct vr_avatar_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short side,
	real_point3d const *grip,
	real_vector3d const *grip_forward,
	real_vector3d const *body_left,
	real_vector3d *moved)
{
	short const *bones = skeleton->arms[side];
	short hand = bones[_vr_arm_hand];
	unsigned long long under = skeleton->skeleton.under[hand];
	real_matrix4x3 held[MAXIMUM_NODES_PER_ANIMATION];
	real_point3d target;
	real_vector3d pole;
	real behind = VR_AVATAR_WRIST_BEHIND_GRIP * VR_AVATAR_UNITS_PER_METRE;
	real outward = side == _vr_avatar_left ? 0.5f : -0.5f;
	short node_index;

	moved->i = moved->j = moved->k = 0.0f;
	target.x = grip->x - grip_forward->i * behind;
	target.y = grip->y - grip_forward->j * behind;
	target.z = grip->z - grip_forward->k * behind;
	/* (the elbow down and out to its side) */
	pole.i = body_left->i * outward;
	pole.j = body_left->j * outward;
	pole.k = body_left->k * outward - 1.0f;
	csmemcpy(held, matrices, skeleton->skeleton.node_count * sizeof(real_matrix4x3));
	vr_solve_arm(&skeleton->skeleton, matrices, bones, matrices[bones[_vr_arm_upper]].position, &target, &pole,
		FALSE, NULL);
	/* (the hand and what it carries turned as the animation has them, only
	moved to where the arm reaches) */
	moved->i = matrices[hand].position.x - held[hand].position.x;
	moved->j = matrices[hand].position.y - held[hand].position.y;
	moved->k = matrices[hand].position.z - held[hand].position.z;
	for (node_index = 0; node_index < skeleton->skeleton.node_count; node_index++)
	{
		if (under & (1ULL << node_index))
		{
			matrices[node_index] = held[node_index];
			matrices[node_index].position.x += moved->i;
			matrices[node_index].position.y += moved->j;
			matrices[node_index].position.z += moved->k;
		}
	}
}

/* the pose of another VR player's biped this frame, built once; NULL where
it is drawn as the game poses it */
static struct vr_avatar_pose *vr_avatar_pose(
	long object_index)
{
	struct vr_avatar_pose *pose;
	struct object_datum *object;
	struct unit_datum *unit;
	struct object_definition *definition;
	struct vr_avatar_skeleton *skeleton;
	struct vr_avatar_world world;
	real_vector3d body_left, moved;
	short index, node_count;

	for (index = 0; index < VR_AVATAR_POSES; index++)
	{
		pose = &vr_avatar_poses[index];
		if (pose->object_index == object_index && pose->frame_index == render.frame_index)
			return pose->valid ? pose : NULL;
	}
	/* (a body drawn as the game poses it takes no place among the poses) */
	object = object_get(object_index);
	if (object->object.type != _object_type_biped || object->object.parent_object_index != NONE ||
		TEST_FLAG(object->object.damage_flags, _object_dead_bit))
	{
		return NULL;
	}
	unit = unit_get(object_index);
	if (unit->unit.player_index == NONE || !network_vr_avatar(unit->unit.player_index, object_index, &world))
		return NULL;
	pose = &vr_avatar_poses[vr_avatar_pose_next++ % VR_AVATAR_POSES];
	pose->object_index = object_index;
	pose->frame_index = render.frame_index;
	pose->valid = FALSE;
	definition = object_definition_get(object->definition_index);
	if (definition->object.model.index == NONE || definition->object.animation_graph.index == NONE)
		return NULL;
	node_count = model_definition_get(definition->object.model.index)->nodes.count;
	if (node_count <= 0 || node_count > MAXIMUM_NODES_PER_ANIMATION || node_count > 64)
		return NULL;
	skeleton = vr_avatar_skeleton(definition->object.animation_graph.index, node_count);
	if (!skeleton)
		return NULL;
	pose->node_count = node_count;
	csmemcpy(pose->matrices, object_get_node_matrices(object_index), node_count * sizeof(real_matrix4x3));
	pose->weapon_moved.i = pose->weapon_moved.j = pose->weapon_moved.k = 0.0f;
	if (world.known & HALO_VR_AVATAR_HEAD)
		vr_avatar_turn_head(skeleton, pose->matrices, &unit->unit.looking_vector, &world.head_forward);
	/* (the body's left: square to where it faces, level) */
	body_left.i = -object->object.forward.j;
	body_left.j = object->object.forward.i;
	body_left.k = 0.0f;
	if (normalize3d(&body_left) < 1e-4f)
	{
		body_left.i = 0.0f;
		body_left.j = 1.0f;
	}
	if (world.known & HALO_VR_AVATAR_FREE_HAND)
	{
		vr_avatar_reach(skeleton, pose->matrices, _vr_avatar_left, &world.free_hand, &world.free_forward,
			&body_left, &moved);
	}
	if (world.known & HALO_VR_AVATAR_WEAPON_HAND)
	{
		vr_avatar_reach(skeleton, pose->matrices, _vr_avatar_right, &world.weapon_hand, &world.weapon_forward,
			&body_left, &pose->weapon_moved);
	}
	pose->valid = TRUE;
	return pose;
}

/* ---------- public code */

real_matrix4x3 const *vr_avatar_node_matrices(
	long object_index,
	real_matrix4x3 const *matrices)
{
	struct object_datum *object = object_get(object_index);
	struct vr_avatar_pose *pose;

	if (object->object.type == _object_type_biped)
	{
		pose = vr_avatar_pose(object_index);
		return pose ? pose->matrices : matrices;
	}
	/* (what a posed biped holds: carried by the weapon hand) */
	if (object->object.parent_object_index != NONE && matrices &&
		object_get(object->object.parent_object_index)->object.type == _object_type_biped)
	{
		struct object_definition *definition = object_definition_get(object->definition_index);
		short node_count, node_index, slot, held;
		real_matrix4x3 *moved = NULL;

		pose = vr_avatar_pose(object->object.parent_object_index);
		if (!pose || definition->object.model.index == NONE)
			return matrices;
		node_count = model_definition_get(definition->object.model.index)->nodes.count;
		if (node_count <= 0 || node_count > MAXIMUM_NODES_PER_MODEL)
			return matrices;
		slot = (short)(pose - vr_avatar_poses);
		for (held = 0; held < VR_AVATAR_HELD && !moved; held++)
		{
			if (vr_avatar_held[slot][held].frame_index != render.frame_index ||
				vr_avatar_held[slot][held].object_index == object_index)
			{
				vr_avatar_held[slot][held].frame_index = render.frame_index;
				vr_avatar_held[slot][held].object_index = object_index;
				moved = vr_avatar_held[slot][held].matrices;
			}
		}
		if (!moved)
			return matrices;
		for (node_index = 0; node_index < node_count; node_index++)
		{
			moved[node_index] = matrices[node_index];
			moved[node_index].position.x += pose->weapon_moved.i;
			moved[node_index].position.y += pose->weapon_moved.j;
			moved[node_index].position.z += pose->weapon_moved.k;
		}
		return moved;
	}
	return matrices;
}

#endif
