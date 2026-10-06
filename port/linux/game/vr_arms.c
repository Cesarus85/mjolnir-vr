/*
VR_ARMS.C

The VR arms' bones (vr_arms.h): the first-person hands' arms bent from the
shoulders to the hands (interface/first_person_weapons.c poses them).

The solver, the forearm's twist and their helpers are after
moistman42069/HaloCE-Quest-VR (CC0), port/linux/game/vr_render.c, nearly
as they are there: the skeleton is read through its animation graph's
index (the nodes under each node found once a frame, not per node), and
the time is the headset's frame's (halo_vr_pose_time).
*/

#include "cseries.h"
#include "math/real_math.h"
#include "models/model_animation_definitions.h"
#include "halo_vr.h"
#include "vr_arms.h"

#include <math.h>
#include <string.h>

#ifdef HALO_ANDROID

#define VR_UNITS_PER_METRE (1.0f / 3.048f)

/* ---------- the skeleton */

boolean vr_skeleton_build(
	struct vr_skeleton *skeleton,
	long graph_index,
	short node_count)
{
	short node_index;

	skeleton->graph_index = NONE;
	if (graph_index == NONE || node_count <= 0 || node_count > MAXIMUM_NODES_PER_ANIMATION ||
		node_count != animation_graph_node_count(graph_index))
	{
		return FALSE;
	}
	for (node_index = 0; node_index < node_count; node_index++)
	{
		short parent = animation_graph_node_parent(graph_index, node_index);

		/* (parents before their children: else no parent) */
		skeleton->parents[node_index] = parent >= 0 && parent < node_index ? parent : NONE;
		skeleton->under[node_index] = 0;
	}
	/* each node under itself and all its ancestors */
	for (node_index = 0; node_index < node_count; node_index++)
	{
		short ancestor = node_index;

		while (ancestor != NONE)
		{
			skeleton->under[ancestor] |= 1ULL << node_index;
			ancestor = skeleton->parents[ancestor];
		}
	}
	skeleton->graph_index = graph_index;
	skeleton->node_count = node_count;
	return TRUE;
}

boolean vr_skeleton_under(
	struct vr_skeleton const *skeleton,
	short node_index,
	short root)
{
	return root >= 0 && root < skeleton->node_count && node_index >= 0 && node_index < skeleton->node_count &&
		(skeleton->under[root] & (1ULL << node_index)) != 0;
}

/* ---------- rotations (column vectors) */

/* v += a * scale */
static void vr_vector_scale_add_local(
	real_vector3d *v,
	real_vector3d const *a,
	real scale)
{
	v->i += a->i * scale;
	v->j += a->j * scale;
	v->k += a->k * scale;
}

static real vr_length(
	real_vector3d const *v)
{
	return (real)sqrt(v->i * v->i + v->j * v->j + v->k * v->k);
}

static void vr_point_minus(
	real_point3d const *a,
	real_point3d const *b,
	real_vector3d *out)
{
	out->i = a->x - b->x;
	out->j = a->y - b->y;
	out->k = a->z - b->z;
}

static boolean vr_unit_vector(
	real_vector3d *v)
{
	real length = vr_length(v);

	if (!(length > 1e-5f && length < 1e8f))
		return FALSE;
	v->i /= length;
	v->j /= length;
	v->k /= length;
	return TRUE;
}

/* the rotation taking direction a to direction b, the shortest way */
static void vr_rotation_between(
	real_vector3d const *a,
	real_vector3d const *b,
	real rotation[3][3])
{
	real_vector3d u = *a, v = *b, axis;
	real c, s, t;

	normalize3d(&u);
	normalize3d(&v);
	cross_product3d(&u, &v, &axis);
	s = vr_length(&axis);
	c = u.i * v.i + u.j * v.j + u.k * v.k;
	if (s < 1e-6f)
	{
		memset(rotation, 0, sizeof(real) * 9);
		if (c >= 0.0f)
		{
			rotation[0][0] = rotation[1][1] = rotation[2][2] = 1.0f;
		}
		else
		{
			/* half a turn about an axis across */
			real_vector3d basis;
			int row, column;

			basis.i = 1.0f;
			basis.j = basis.k = 0.0f;
			if (fabs(u.i) > 0.8f)
			{
				basis.i = 0.0f;
				basis.j = 1.0f;
			}
			cross_product3d(&u, &basis, &axis);
			normalize3d(&axis);
			for (row = 0; row < 3; row++)
			{
				for (column = 0; column < 3; column++)
					rotation[row][column] = 2.0f * axis.n[row] * axis.n[column] - (row == column ? 1.0f : 0.0f);
			}
		}
		return;
	}
	axis.i /= s;
	axis.j /= s;
	axis.k /= s;
	t = 1.0f - c;
	rotation[0][0] = c + axis.i * axis.i * t;
	rotation[0][1] = axis.i * axis.j * t - axis.k * s;
	rotation[0][2] = axis.i * axis.k * t + axis.j * s;
	rotation[1][0] = axis.j * axis.i * t + axis.k * s;
	rotation[1][1] = c + axis.j * axis.j * t;
	rotation[1][2] = axis.j * axis.k * t - axis.i * s;
	rotation[2][0] = axis.k * axis.i * t - axis.j * s;
	rotation[2][1] = axis.k * axis.j * t + axis.i * s;
	rotation[2][2] = c + axis.k * axis.k * t;
}

/* a rotation of angle radians about the unit axis */
static void vr_axis_rotation(
	real_vector3d const *axis,
	real angle,
	real rotation[3][3])
{
	real c = (real)cos(angle), s = (real)sin(angle), t = 1.0f - c;
	real x = axis->i, y = axis->j, z = axis->k;

	rotation[0][0] = c + x * x * t;
	rotation[0][1] = x * y * t - z * s;
	rotation[0][2] = x * z * t + y * s;
	rotation[1][0] = y * x * t + z * s;
	rotation[1][1] = c + y * y * t;
	rotation[1][2] = y * z * t - x * s;
	rotation[2][0] = z * x * t - y * s;
	rotation[2][1] = z * y * t + x * s;
	rotation[2][2] = c + z * z * t;
}

static void vr_rotate_vector(
	real rotation[3][3],
	real_vector3d const *in,
	real_vector3d *out)
{
	real_vector3d v = *in;

	out->i = rotation[0][0] * v.i + rotation[0][1] * v.j + rotation[0][2] * v.k;
	out->j = rotation[1][0] * v.i + rotation[1][1] * v.j + rotation[1][2] * v.k;
	out->k = rotation[2][0] * v.i + rotation[2][1] * v.j + rotation[2][2] * v.k;
}

static void vr_multiply_rotations(
	real a[3][3],
	real b[3][3],
	real out[3][3])
{
	real r[3][3];
	int row, column;

	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
			r[row][column] = a[row][0] * b[0][column] + a[row][1] * b[1][column] + a[row][2] * b[2][column];
	}
	memcpy(out, r, sizeof(r));
}

/* a node moved from `from` to `to` and turned by rotation about itself: its
matrix, or that of a node it carries (whose old matrix is node) */
static void vr_carry(
	real_matrix4x3 *node,
	real_point3d const *from,
	real_point3d const *to,
	real rotation[3][3])
{
	real_vector3d offset;

	vr_point_minus(&node->position, from, &offset);
	vr_rotate_vector(rotation, &offset, &offset);
	node->position.x = to->x + offset.i;
	node->position.y = to->y + offset.j;
	node->position.z = to->z + offset.k;
	vr_rotate_vector(rotation, &node->forward, &node->forward);
	vr_rotate_vector(rotation, &node->left, &node->left);
	vr_rotate_vector(rotation, &node->up, &node->up);
}

/* ---------- the arm */

void vr_solve_arm(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short const bones[NUMBER_OF_VR_ARM_BONES],
	real_point3d shoulder,
	real_point3d const *target,
	real_vector3d const *pole,
	boolean hand_stays,
	struct vr_limb_history *history)
{
	real_matrix4x3 old[NUMBER_OF_VR_ARM_BONES];
	real rotation[NUMBER_OF_VR_ARM_BONES][3][3];
	real_point3d moved_to[NUMBER_OF_VR_ARM_BONES];
	real_vector3d upper, fore, reach, direction, bend, old_direction;
	real upper_length, fore_length, distance, along, across;
	real_point3d solved_target = *target;
	short moved[MAXIMUM_NODES_PER_ANIMATION];
	short index, bone;

	for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
	{
		if (bones[bone] < 0 || bones[bone] >= skeleton->node_count)
			return;
		old[bone] = matrices[bones[bone]];
	}
	vr_point_minus(&old[_vr_arm_fore].position, &old[_vr_arm_upper].position, &upper);
	vr_point_minus(&old[_vr_arm_hand].position, &old[_vr_arm_fore].position, &fore);
	upper_length = vr_length(&upper);
	fore_length = vr_length(&fore);
	if (!isfinite(upper_length) || !isfinite(fore_length) || upper_length < 1e-4f || fore_length < 1e-4f)
		return;
	vr_point_minus(target, &shoulder, &reach);
	distance = vr_length(&reach);
	if (!isfinite(distance) || distance < 1e-4f)
		return;
	direction = reach;
	normalize3d(&direction);
	/* out of reach: the shoulder reaches a little (8 cm), then the arm
	stretches a little (18%); the hand stays where it is held */
	if (distance > (upper_length + fore_length) * (history ? 0.996f : 0.999f))
	{
		real maximum = (upper_length + fore_length) * (history ? 0.996f : 0.999f);
		real travel = distance - maximum;

		travel = MIN(travel, 0.08f * VR_UNITS_PER_METRE);
		shoulder.x += direction.i * travel;
		shoulder.y += direction.j * travel;
		shoulder.z += direction.k * travel;
		distance -= travel;
		if (history && distance > maximum)
		{
			real stretch = MIN(distance / maximum, 1.18f);

			upper_length *= stretch;
			fore_length *= stretch;
			maximum *= stretch;
		}
		if (hand_stays && distance > maximum)
		{
			/* (further still, as this port had it: the shoulder follows the
			hand, and the arm stays whole) */
			shoulder.x += direction.i * (distance - maximum);
			shoulder.y += direction.j * (distance - maximum);
			shoulder.z += direction.k * (distance - maximum);
		}
		distance = MIN(distance, maximum);
	}
	if (history)
	{
		/* bent no further than 145 degrees: nearer, the bones shorten (to
		65% at most) */
		real minimum = (real)sqrt(upper_length * upper_length + fore_length * fore_length -
			1.638304f * upper_length * fore_length);

		if (distance < minimum)
		{
			real compress = MAX(0.65f, distance / minimum);

			upper_length *= compress;
			fore_length *= compress;
		}
	}
	if (distance < (real)fabs(upper_length - fore_length) + 1e-3f)
		distance = (real)fabs(upper_length - fore_length) + 1e-3f;
	solved_target.x = shoulder.x + direction.i * distance;
	solved_target.y = shoulder.y + direction.j * distance;
	solved_target.z = shoulder.z + direction.k * distance;
	/* the elbow: in the plane of the reach and the pole */
	{
		real dot = pole->i * direction.i + pole->j * direction.j + pole->k * direction.k;

		bend.i = pole->i - direction.i * dot;
		bend.j = pole->j - direction.j * dot;
		bend.k = pole->k - direction.k * dot;
		if (vr_length(&bend) < 1e-4f)
		{
			/* reaching along the pole: the old bend, else a steady side */
			dot = dot_product3d(&upper, &direction);
			bend = upper;
			bend.i -= dot * direction.i;
			bend.j -= dot * direction.j;
			bend.k -= dot * direction.k;
			if (vr_length(&bend) < 1e-4f)
			{
				real_vector3d basis;

				basis.i = basis.j = 0.0f;
				basis.k = 1.0f;
				if (fabs(direction.k) > 0.9f)
				{
					basis.j = 1.0f;
					basis.k = 0.0f;
				}
				cross_product3d(&direction, &basis, &bend);
			}
		}
		if (history)
		{
			/* the elbow's swing about the reach, eased (18 a second, at most 8
			radians a second) unless the arm was away or jumped */
			double now = halo_vr_pose_time();
			double dt = now - history->time;
			real_vector3d previous = history->bend, travel;
			real confidence = PIN(vr_length(&bend) / 0.35f, 0.0f, 1.0f);

			vr_point_minus(&shoulder, &history->shoulder, &travel);
			dot = dot_product3d(&previous, &direction);
			previous.i -= dot * direction.i;
			previous.j -= dot * direction.j;
			previous.k -= dot * direction.k;
			if (history->valid && history->graph_index == skeleton->graph_index && dt >= 0.0 && dt < 0.25 &&
				vr_length(&travel) < 0.5f * VR_UNITS_PER_METRE && vr_unit_vector(&previous) && vr_unit_vector(&bend))
			{
				real_vector3d tangent;
				real angle, change;

				cross_product3d(&previous, &bend, &tangent);
				angle = (real)atan2(dot_product3d(&tangent, &direction), dot_product3d(&previous, &bend));
				change = angle * (1.0f - (real)exp(-18.0 * MIN(dt, 0.05))) * confidence;
				change = PIN(change, (real)(-8.0 * dt), (real)(8.0 * dt));
				cross_product3d(&direction, &previous, &tangent);
				bend.i = previous.i * (real)cos(change) + tangent.i * (real)sin(change);
				bend.j = previous.j * (real)cos(change) + tangent.j * (real)sin(change);
				bend.k = previous.k * (real)cos(change) + tangent.k * (real)sin(change);
			}
			else
			{
				history->roll_valid = FALSE;
			}
			normalize3d(&bend);
			history->valid = TRUE;
			history->graph_index = skeleton->graph_index;
			history->time = now;
			history->shoulder = shoulder;
			history->bend = bend;
		}
		else
		{
			normalize3d(&bend);
		}
	}
	along = (upper_length * upper_length + distance * distance - fore_length * fore_length) / (2.0f * distance);
	across = upper_length * upper_length - along * along;
	across = across > 0.0f ? (real)sqrt(across) : 0.0f;
	moved_to[_vr_arm_upper] = shoulder;
	moved_to[_vr_arm_fore].x = shoulder.x + direction.i * along + bend.i * across;
	moved_to[_vr_arm_fore].y = shoulder.y + direction.j * along + bend.j * across;
	moved_to[_vr_arm_fore].z = shoulder.z + direction.k * along + bend.k * across;
	moved_to[_vr_arm_hand] = solved_target;

	/* each bone turned to point at the next joint, and the hand carried by
	the forearm's turn */
	vr_point_minus(&moved_to[_vr_arm_fore], &moved_to[_vr_arm_upper], &reach);
	vr_rotation_between(&upper, &reach, rotation[_vr_arm_upper]);
	vr_rotate_vector(rotation[_vr_arm_upper], &fore, &old_direction);
	vr_point_minus(&moved_to[_vr_arm_hand], &moved_to[_vr_arm_fore], &reach);
	vr_rotation_between(&old_direction, &reach, rotation[_vr_arm_fore]);
	vr_multiply_rotations(rotation[_vr_arm_fore], rotation[_vr_arm_upper], rotation[_vr_arm_fore]);
	if (hand_stays)
	{
		/* the hand stays as it was put, and what it holds */
		memset(rotation[_vr_arm_hand], 0, sizeof(rotation[_vr_arm_hand]));
		rotation[_vr_arm_hand][0][0] = rotation[_vr_arm_hand][1][1] = rotation[_vr_arm_hand][2][2] = 1.0f;
		moved_to[_vr_arm_hand] = old[_vr_arm_hand].position;
	}
	else
	{
		memcpy(rotation[_vr_arm_hand], rotation[_vr_arm_fore], sizeof(rotation[_vr_arm_hand]));
	}

	/* every node under a moved bone goes with the nearest one above it
	(parents come first) */
	for (index = 0; index < skeleton->node_count; index++)
	{
		short parent = skeleton->parents[index];

		moved[index] = NONE;
		for (bone = 0; bone < NUMBER_OF_VR_ARM_BONES; bone++)
		{
			if (bones[bone] == index)
				moved[index] = bone;
		}
		if (moved[index] == NONE && parent != NONE)
			moved[index] = moved[parent];
		if (moved[index] != NONE)
		{
			bone = moved[index];
			vr_carry(&matrices[index], &old[bone].position, &moved_to[bone], rotation[bone]);
		}
	}
}

/* The controller's roll shared along the arm rather than all of it at the
wrist's skin: the hand's turn about the forearm from where the animation
has it against its forearm (old_fore, old_hand), 85% of it on the forearm
(none on the upper arm: vr_align_elbow rolls it), within 110 degrees, running on across half a
turn (history). The bones' places and the hand stay. */
void vr_distribute_arm_twist(
	real_matrix4x3 *matrices,
	short const bones[NUMBER_OF_VR_ARM_BONES],
	real_matrix4x3 const *old_fore,
	real_matrix4x3 const *old_hand,
	struct vr_limb_history *history)
{
	real_matrix4x3 *fore = &matrices[bones[_vr_arm_fore]];
	real_matrix4x3 *hand = &matrices[bones[_vr_arm_hand]];
	real_matrix3x3 delta;
	real_vector3d expected[3], desired[3], axis;
	real_quaternion q;
	real angle, rotation[3][3];
	int a, b, row, column;

	vr_point_minus(&hand->position, &fore->position, &axis);
	if (!vr_unit_vector(&axis))
		return;
	for (a = 0; a < 3; a++)
	{
		expected[a].i = expected[a].j = expected[a].k = 0.0f;
		desired[a].i = hand->n[a][0];
		desired[a].j = hand->n[a][1];
		desired[a].k = hand->n[a][2];
		for (b = 0; b < 3; b++)
		{
			real amount = old_fore->n[b][0] * old_hand->n[a][0] + old_fore->n[b][1] * old_hand->n[a][1] +
				old_fore->n[b][2] * old_hand->n[a][2];

			for (row = 0; row < 3; row++)
				expected[a].n[row] += fore->n[b][row] * amount;
		}
		if (!vr_unit_vector(&expected[a]) || !vr_unit_vector(&desired[a]))
			return;
	}
	for (row = 0; row < 3; row++)
	{
		for (column = 0; column < 3; column++)
		{
			delta.n[row][column] = 0.0f;
			for (a = 0; a < 3; a++)
				delta.n[row][column] += desired[a].n[row] * expected[a].n[column];
		}
	}
	matrix3x3_rotation_to_quaternion(&delta, &q);
	angle = 2.0f * (real)atan2(dot_product3d(&q.v, &axis), q.w);
	if (!isfinite(angle))
		return;
	angle = (real)atan2(sin(angle), cos(angle));
	/* (continuous across half a turn before the limit) */
	if (history && history->roll_valid)
		angle = history->roll + (real)atan2(sin(angle - history->roll), cos(angle - history->roll));
	angle = PIN(angle, -1.92f, 1.92f);
	if (history)
	{
		history->roll = angle;
		history->roll_valid = TRUE;
	}
	vr_axis_rotation(&axis, angle * 0.85f, rotation);
	vr_rotate_vector(rotation, &fore->forward, &fore->forward);
	vr_rotate_vector(rotation, &fore->left, &fore->left);
	vr_rotate_vector(rotation, &fore->up, &fore->up);
}

/* The upper arm rolled about its length so that the elbow bends in the
plane the arm now bends in, as the animation has it bend: the shortest
turn that took the upper arm to its new direction leaves its hinge where
it was, and with the elbow out to the side the elbow's skin turned in
(Quest, 2026-10-06). The hinge is the animation's bend (upper arm by
forearm, old_upper, old_fore, old_hand) in the upper arm's own axes. A
straight arm, then or now, has no bend: left as it is. */
void vr_align_elbow(
	real_matrix4x3 *matrices,
	short const bones[NUMBER_OF_VR_ARM_BONES],
	real_matrix4x3 const *old_upper,
	real_matrix4x3 const *old_fore,
	real_matrix4x3 const *old_hand)
{
	real_matrix4x3 *upper = &matrices[bones[_vr_arm_upper]];
	real_matrix4x3 const *fore = &matrices[bones[_vr_arm_fore]];
	real_matrix4x3 const *hand = &matrices[bones[_vr_arm_hand]];
	real_vector3d old_along, old_below, old_bend, along, below, bend, hinge;
	real c_forward, c_left, c_up, angle, rotation[3][3];

	vr_point_minus(&old_fore->position, &old_upper->position, &old_along);
	vr_point_minus(&old_hand->position, &old_fore->position, &old_below);
	cross_product3d(&old_along, &old_below, &old_bend);
	vr_point_minus(&fore->position, &upper->position, &along);
	vr_point_minus(&hand->position, &fore->position, &below);
	cross_product3d(&along, &below, &bend);
	if (vr_length(&old_bend) < 1e-4f * vr_length(&old_along) * vr_length(&old_below) ||
		vr_length(&bend) < 0.05f * vr_length(&along) * vr_length(&below) ||
		!vr_unit_vector(&old_bend) || !vr_unit_vector(&bend) || !vr_unit_vector(&along))
	{
		return;
	}
	/* (the hinge in the upper arm's axes, then where those axes are now) */
	c_forward = dot_product3d(&old_bend, &old_upper->forward);
	c_left = dot_product3d(&old_bend, &old_upper->left);
	c_up = dot_product3d(&old_bend, &old_upper->up);
	hinge.i = upper->forward.i * c_forward + upper->left.i * c_left + upper->up.i * c_up;
	hinge.j = upper->forward.j * c_forward + upper->left.j * c_left + upper->up.j * c_up;
	hinge.k = upper->forward.k * c_forward + upper->left.k * c_left + upper->up.k * c_up;
	/* (both across the arm: the turn about it between them) */
	vr_vector_scale_add_local(&hinge, &along, -dot_product3d(&hinge, &along));
	vr_vector_scale_add_local(&bend, &along, -dot_product3d(&bend, &along));
	if (!vr_unit_vector(&hinge) || !vr_unit_vector(&bend))
		return;
	{
		real_vector3d cross;

		cross_product3d(&hinge, &bend, &cross);
		angle = (real)atan2(dot_product3d(&cross, &along), dot_product3d(&hinge, &bend));
	}
	if (!isfinite(angle))
		return;
	vr_axis_rotation(&along, angle, rotation);
	vr_rotate_vector(rotation, &upper->forward, &upper->forward);
	vr_rotate_vector(rotation, &upper->left, &upper->left);
	vr_rotate_vector(rotation, &upper->up, &upper->up);
}

/* ---------- the fingers

Each finger's joints are found by name: "finger" and its number (0 the
thumb to 4 the little finger), or "thumb", "index", "middle", "ring",
"pinky", on the hand's side ("l ", "r "). They are posed by absolute
directions in the controller's palm frame, each segment's subtree turned,
its length kept (after HaloCE-Quest-VR, CC0, nearly verbatim; its touch of
the world, a few rays a finger, is left out: it costs the Steam Frame's
processor too much). */

/* the finger (0 the thumb .. 4 the little finger) a node's name says it is
a joint of, on that side; NONE when not a finger's */
static short vr_finger_of(
	char const *name,
	char const *side)
{
	static char const *const names[] = { "thumb", "index", "middle", "ring", "pinky" };
	char const *finger;
	short index;

	if (!name || !strstr(name, side))
		return NONE;
	finger = strstr(name, "finger");
	if (finger && finger[6] >= '0' && finger[6] <= '4')
		return (short)(finger[6] - '0');
	for (index = 0; index < 5; index++)
	{
		if (strstr(name, names[index]))
			return index;
	}
	return NONE;
}

void vr_finger_nodes(
	struct vr_skeleton const *skeleton,
	short hand,
	char const *side,
	short joints[5][3])
{
	short node_index, finger, depth;

	for (finger = 0; finger < 5; finger++)
	{
		for (depth = 0; depth < 3; depth++)
			joints[finger][depth] = NONE;
	}
	for (node_index = 0; node_index < skeleton->node_count; node_index++)
	{
		short at;

		finger = vr_finger_of(animation_graph_node_name(skeleton->graph_index, node_index), side);
		if (finger == NONE || !vr_skeleton_under(skeleton, node_index, hand))
			continue;
		/* its depth: how many of the same finger's joints above it */
		depth = 0;
		for (at = skeleton->parents[node_index]; at != NONE; at = skeleton->parents[at])
		{
			if (vr_finger_of(animation_graph_node_name(skeleton->graph_index, at), side) == finger)
				depth++;
		}
		if (depth < 3)
			joints[finger][depth] = node_index;
	}
}

/* a node and all under it turned about the node */
static void vr_turn_subtree(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short root,
	real rotation[3][3])
{
	real_point3d pivot = matrices[root].position;
	unsigned long long under = skeleton->under[root];
	short node_index;

	for (node_index = 0; node_index < skeleton->node_count; node_index++)
	{
		if (under & (1ULL << node_index))
			vr_carry(&matrices[node_index], &pivot, &pivot, rotation);
	}
}

/* a finger's joints (low, middle, tip) curled 0 (straight, a little bent
at rest) to 1 (a fist), in the palm's frame: forward along the fingers,
up the knuckles towards the index, palm out of the palm */
static boolean vr_pose_finger(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short hand,
	short const joints[3],
	int finger,
	real curl,
	real_vector3d const *palm,
	real_vector3d const *forward,
	real_vector3d const *up)
{
	static real const rest[3] = { 0.12f, 0.15f, 0.10f };
	static real const bend[3] = { 1.25f, 1.6f, 1.1f };
	real_vector3d base, axis, direction, wanted;
	real angle = 0.0f, dot, rotation[3][3];
	int depth;

	if (joints[0] == NONE || joints[1] == NONE || joints[2] == NONE)
		return FALSE;
	vr_point_minus(&matrices[joints[0]].position, &matrices[hand].position, &base);
	dot = dot_product3d(&base, palm);
	base.i -= dot * palm->i;
	base.j -= dot * palm->j;
	base.k -= dot * palm->k;
	if (finger == 0)
	{
		/* the thumb: up the handle, or across the palm */
		base.i = (up->i * 0.9f + forward->i * 0.3f) * (1.0f - curl) +
			(forward->i * 0.75f + palm->i * 0.55f + up->i * 0.1f) * curl;
		base.j = (up->j * 0.9f + forward->j * 0.3f) * (1.0f - curl) +
			(forward->j * 0.75f + palm->j * 0.55f + up->j * 0.1f) * curl;
		base.k = (up->k * 0.9f + forward->k * 0.3f) * (1.0f - curl) +
			(forward->k * 0.75f + palm->k * 0.55f + up->k * 0.1f) * curl;
	}
	if (!vr_unit_vector(&base))
		return FALSE;
	cross_product3d(&base, palm, &axis);
	if (!vr_unit_vector(&axis))
		return FALSE;
	for (depth = 0; depth < 3; depth++)
	{
		short joint = joints[depth];

		if (depth < 2)
		{
			vr_point_minus(&matrices[joints[depth + 1]].position, &matrices[joint].position, &direction);
		}
		else
		{
			/* the tip: its own axis that the middle joint's bone runs along */
			real_vector3d previous;
			real_vector3d *axes[3];
			real_vector3d *last[3];
			real best = 0.8f, sign = 1.0f;
			int a, found = -1;

			axes[0] = &matrices[joints[1]].forward;
			axes[1] = &matrices[joints[1]].left;
			axes[2] = &matrices[joints[1]].up;
			last[0] = &matrices[joint].forward;
			last[1] = &matrices[joint].left;
			last[2] = &matrices[joint].up;
			vr_point_minus(&matrices[joint].position, &matrices[joints[1]].position, &previous);
			if (!vr_unit_vector(&previous))
				return FALSE;
			for (a = 0; a < 3; a++)
			{
				real_vector3d unit = *axes[a];

				if (!vr_unit_vector(&unit))
					continue;
				dot = dot_product3d(&unit, &previous);
				if (fabs(dot) > best)
				{
					best = (real)fabs(dot);
					found = a;
					sign = dot < 0.0f ? -1.0f : 1.0f;
				}
			}
			if (found < 0)
				return TRUE;
			direction.i = last[found]->i * sign;
			direction.j = last[found]->j * sign;
			direction.k = last[found]->k * sign;
		}
		if (!vr_unit_vector(&direction))
			return FALSE;
		angle += finger == 0 ? (depth == 0 ? 0.0f : (depth == 1 ? 0.35f : 0.3f) * curl) :
			rest[depth] + bend[depth] * curl;
		vr_axis_rotation(&axis, angle, rotation);
		vr_rotate_vector(rotation, &base, &wanted);
		vr_rotation_between(&direction, &wanted, rotation);
		vr_turn_subtree(skeleton, matrices, joint, rotation);
	}
	return TRUE;
}

void vr_pose_fingers(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short hand,
	short const joints[5][3],
	float const curls[4],
	real_vector3d const *palm,
	real_vector3d const *forward,
	real_vector3d const *up)
{
	int finger;

	for (finger = 0; finger < 5; finger++)
		vr_pose_finger(skeleton, matrices, hand, joints[finger], finger, curls[finger < 3 ? finger : 3], palm, forward, up);
}

/* the finger's middle and tip turned this much (0..1) towards the line of
its low joint: a finger lifted off the trigger (the weapon hand's index,
whose low joint, and the rest of the hand, keep the weapon's grip) */
void vr_straighten_finger(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short const joints[3],
	real amount)
{
	real_vector3d low, middle, axis;
	real rotation[3][3];
	real angle;

	if (amount <= 0.0f || joints[0] == NONE || joints[1] == NONE || joints[2] == NONE)
		return;
	vr_point_minus(&matrices[joints[1]].position, &matrices[joints[0]].position, &low);
	vr_point_minus(&matrices[joints[2]].position, &matrices[joints[1]].position, &middle);
	if (!vr_unit_vector(&low) || !vr_unit_vector(&middle))
		return;
	cross_product3d(&middle, &low, &axis);
	if (!vr_unit_vector(&axis))
		return;
	angle = (real)acos(PIN(dot_product3d(&middle, &low), -1.0f, 1.0f)) * PIN(amount, 0.0f, 1.0f);
	vr_axis_rotation(&axis, angle, rotation);
	vr_turn_subtree(skeleton, matrices, joints[1], rotation);
}

/* ---------- hands alone (vr.body "hands")

The arm's bones (not the hand's, its fingers' or the weapon's) shrunk to
nothing at a point 3.5 cm back from the wrist along the hand's line,
knuckles to wrist: the cuff closes just behind the hand. Shrunk onto their
own places, the forearm drew the wrist's skin to the elbow and the hand
looked cut off (after HaloCE-Quest-VR, CC0, its vr_hide_forearms and
vr_hand_back_axis). */
void vr_hide_arm(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *matrices,
	short const bones[NUMBER_OF_VR_ARM_BONES],
	short const fingers[5][3],
	short gun)
{
	real_point3d centre = matrices[bones[_vr_arm_hand]].position;
	real_point3d knuckles;
	real_vector3d back;
	boolean along = FALSE;
	short finger, node_index, found = 0;

	knuckles.x = knuckles.y = knuckles.z = 0.0f;
	/* (the four fingers' bases: the thumb's sits off the line) */
	for (finger = 1; finger < 5; finger++)
	{
		if (fingers[finger][0] == NONE)
			continue;
		knuckles.x += matrices[fingers[finger][0]].position.x;
		knuckles.y += matrices[fingers[finger][0]].position.y;
		knuckles.z += matrices[fingers[finger][0]].position.z;
		found++;
	}
	if (found)
	{
		knuckles.x /= found;
		knuckles.y /= found;
		knuckles.z /= found;
		vr_point_minus(&centre, &knuckles, &back);
		along = vr_unit_vector(&back);
	}
	if (!along)
	{
		vr_point_minus(&matrices[bones[_vr_arm_fore]].position, &centre, &back);
		along = vr_unit_vector(&back);
	}
	if (along)
	{
		centre.x += back.i * 0.035f * VR_UNITS_PER_METRE;
		centre.y += back.j * 0.035f * VR_UNITS_PER_METRE;
		centre.z += back.k * 0.035f * VR_UNITS_PER_METRE;
	}
	for (node_index = 0; node_index < skeleton->node_count; node_index++)
	{
		if (!vr_skeleton_under(skeleton, node_index, bones[_vr_arm_upper]) ||
			vr_skeleton_under(skeleton, node_index, bones[_vr_arm_hand]) ||
			(gun != NONE && vr_skeleton_under(skeleton, node_index, gun)))
		{
			continue;
		}
		matrices[node_index].scale = 0.0f;
		matrices[node_index].position = centre;
	}
}

#endif
