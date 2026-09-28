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

/* clears the back buffer to transparent black for the overlay, after the
eyes; menus_active chooses where the overlay is shown */
void halo_vr_begin_overlay(int menus_active);

/* the player's facing from the headset. Given the game's current desired
facing, returns nonzero with the facing to use instead. */
int halo_vr_aim(short local_player_index, float current_yaw, float current_pitch,
	float *yaw, float *pitch);

/* where the head looks, in the game's axes; 0 when not in VR play */
int halo_vr_look_vector(short local_player_index, float vector[3]);

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
int halo_vr_weapon_camera(short local_player_index, float position[3], float forward[3], float up[3]);
int halo_vr_laser(short local_player_index, float start[3], float direction[3]);
/* - and whether the HUD's crosshair, which marks where the head looks, is
  left out */
int halo_vr_hide_crosshair(void);

/* the platform layer's side (vr.c) */
void halo_vr_initialize(void);
void halo_vr_frame_begin(void);
void halo_vr_present(unsigned int back_buffer_texture);
/* the eye image size: the back buffer's size in pixels in VR */
int halo_vr_eye_size(int *width, int *height);

#ifdef __cplusplus
}
#endif

#endif
