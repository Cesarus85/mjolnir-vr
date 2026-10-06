/*
VR_ARMS.H

The VR arms' bones (vr_arms.c), for the first-person hands
(interface/first_person_weapons.c): the arms bent from the shoulders to the
hands. Include after the game's math and animation headers.
*/

#ifndef __VR_ARMS_H
#define __VR_ARMS_H

#ifdef HALO_ANDROID

enum
{
	_vr_arm_upper,
	_vr_arm_fore,
	_vr_arm_hand,
	NUMBER_OF_VR_ARM_BONES
};

/* a skeleton's parents, and the nodes under each node (it among them), from
its animation graph: parents come before their children */
struct vr_skeleton
{
	long graph_index;
	short node_count;
	short parents[MAXIMUM_NODES_PER_ANIMATION];
	unsigned long long under[MAXIMUM_NODES_PER_ANIMATION];
};

/* an arm's last frame, for its elbow's swing and its forearm's twist to run
on smoothly (one each arm) */
struct vr_limb_history
{
	long graph_index;
	double time;
	real_point3d shoulder;
	real_vector3d bend;
	boolean valid;
	boolean roll_valid;
	real roll;
};

boolean vr_skeleton_build(struct vr_skeleton *skeleton, long graph_index, short node_count);
boolean vr_skeleton_under(struct vr_skeleton const *skeleton, short node_index, short root);

/* one arm (bones: upper arm, forearm, hand) bent from the shoulder for the
hand to reach target, its elbow towards the pole; the hand (and what it
holds) stays where it is, or turns with the forearm */
void vr_solve_arm(struct vr_skeleton const *skeleton, real_matrix4x3 *matrices,
	short const bones[NUMBER_OF_VR_ARM_BONES], real_point3d shoulder, real_point3d const *target,
	real_vector3d const *pole, boolean hand_stays, struct vr_limb_history *history);
/* the hand's roll from the animation (old_fore, old_hand: the arm as
animated) shared by the forearm and the upper arm */
void vr_distribute_arm_twist(real_matrix4x3 *matrices, short const bones[NUMBER_OF_VR_ARM_BONES],
	real_matrix4x3 const *old_fore, real_matrix4x3 const *old_hand, struct vr_limb_history *history);

#endif

#endif
