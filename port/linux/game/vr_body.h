/*
VR_BODY.H

The whole body in VR (vr.body "full", vr_body.c): the player's own biped
drawn under the first-person arms and weapon. Include after the game's
math, object and render headers.
*/

#ifndef __VR_BODY_H
#define __VR_BODY_H

#ifdef HALO_ANDROID

struct render_camera;

/* render_objects.c: whether the object is the local player's own unit, drawn
whole in first person this frame (its weapon and what it carries left
out), and its node matrices as posed */
boolean vr_body_drawn(short local_player_index, long object_index);
real_matrix4x3 *vr_body_node_matrices(long object_index);

/* first_person_weapons.c: the drawn body's shoulders this frame (world
units; the model's left, then right), from which the first-person arms hang;
FALSE when no body is drawn */
boolean vr_body_shoulders(short local_player_index, real_point3d shoulders[2]);

/* debug.vr_body, flat (the emulator): the camera of a player's view moved to
look at the body (render.c), and whether the first-person weapon is left out
of it (first_person_weapons.c) */
boolean vr_body_debug_camera(short local_player_index, struct render_camera *render_camera,
	struct render_camera *rasterizer_camera);
boolean vr_body_debug_hides_first_person(void);

#endif

#endif
