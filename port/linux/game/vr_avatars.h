/*
VR_AVATARS.H

Other VR players' heads and hands, as their machines send them
(network_vr_avatars.c), and their bodies posed after them for drawing
(vr_avatars.c). Include after the game's math headers.
*/

#ifndef __VR_AVATARS_H
#define __VR_AVATARS_H

/* a VR player's head and hands in the world (world units), each with its
frame: forward, left, up; known: HALO_VR_AVATAR_* (halo_vr.h) */
struct vr_avatar_world
{
	int known;
	real_point3d head;
	real_vector3d head_forward, head_left, head_up;
	real_point3d weapon_hand;
	real_vector3d weapon_forward, weapon_left, weapon_up;
	real_point3d free_hand;
	real_vector3d free_forward, free_left, free_up;
};

/* ---------- network_vr_avatars.c */

void network_vr_avatars_new_game(void);
void network_vr_avatars_tick(void);
word network_vr_avatars_entry_size(void);
void network_vr_avatars_handle(long machine_index, void const *entries, short count);
/* a player of another machine's head and hands, as lately heard of, eased
between what came (FALSE: none, or not lately) */
boolean network_vr_avatar(long player_index, long unit_index, struct vr_avatar_world *avatar);

/* ---------- vr_avatars.c */

/* an object's node matrices as drawn: another VR player's biped posed after
their head and hands, the weapon in its hand moved with it; else as given */
real_matrix4x3 const *vr_avatar_node_matrices(long object_index, real_matrix4x3 const *matrices);

#endif
