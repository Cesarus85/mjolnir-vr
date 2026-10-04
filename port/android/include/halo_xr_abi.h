/*
HALO_XR_ABI.H

The structures the game (the ILP32 guest, port/linux/src/vr.c) and the
host's OpenXR session (port/android/host/host_xr.c) exchange. Both sides
compile this header, so every member is a 32-bit int or float: the layout
is the same under ILP32 and LP64.

Poses are in OpenXR's convention: metres, right-handed, +x right, +y up,
-z forward, relative to the LOCAL reference space (the head's position and
heading when the user last recentred). Quaternions are x, y, z, w.
*/

#ifndef __HALO_XR_ABI_H
#define __HALO_XR_ABI_H

enum
{
	HALO_XR_EYE_LEFT = 0,
	HALO_XR_EYE_RIGHT = 1,
	HALO_XR_EYE_COUNT = 2,

	/* the images the game fills each frame (host_xr_copy_layer) */
	HALO_XR_LAYER_EYE_LEFT = 0,
	HALO_XR_LAYER_EYE_RIGHT = 1,
	HALO_XR_LAYER_OVERLAY = 2, /* HUD, menus, cinematics: a flat quad */
	HALO_XR_LAYER_WRIST = 3,   /* the motion tracker, on the left wrist */
	HALO_XR_LAYER_SCOPE = 4,   /* the zoomed view, a disc over the weapon */
	HALO_XR_LAYER_CROSSHAIR = 5, /* the weapon's crosshair, where it aims */
	HALO_XR_LAYER_NAMES = 6,   /* players' names, each a quad over a head */
	HALO_XR_LAYER_COUNT = 7,
	/* the names layer's slots, one a name, stacked from its top */
	HALO_XR_MAXIMUM_NAMES = 8,

	/* host_xr_copy_layer's clear_alpha: the source's alpha kept, made
	opaque, or cut to a disc (premultiplied) */
	HALO_XR_COPY_ALPHA = 0,
	HALO_XR_COPY_OPAQUE = 1,
	HALO_XR_COPY_DISC = 2,

	HALO_XR_HAND_LEFT = 0,
	HALO_XR_HAND_RIGHT = 1,
	HALO_XR_HAND_COUNT = 2,
};

/* halo_xr_hand.buttons */
enum
{
	HALO_XR_BUTTON_A = 1 << 0,          /* right hand: A; left hand: X */
	HALO_XR_BUTTON_B = 1 << 1,          /* right hand: B; left hand: Y */
	HALO_XR_BUTTON_THUMBSTICK = 1 << 2, /* stick click */
	HALO_XR_BUTTON_MENU = 1 << 3,       /* left hand's menu button */
	HALO_XR_BUTTON_TRIGGER = 1 << 4,    /* trigger past its click point */
	HALO_XR_BUTTON_SQUEEZE = 1 << 5,    /* grip past its click point */
	HALO_XR_BUTTON_THUMBREST = 1 << 6,  /* thumb resting (touch) */
	/* the Steam Frame's controllers' (HALO_XR_LAYOUT_FRAME): the left one's
	View button and D-pad, either one's bumper (their right one's X, Y and
	menu button come as the left hand's A, B and MENU: the game's X, Y and
	Start, as on the Quest) */
	HALO_XR_BUTTON_VIEW = 1 << 7,
	HALO_XR_BUTTON_BUMPER = 1 << 8,
	HALO_XR_BUTTON_DPAD_UP = 1 << 9,
	HALO_XR_BUTTON_DPAD_DOWN = 1 << 10,
	HALO_XR_BUTTON_DPAD_LEFT = 1 << 11,
	HALO_XR_BUTTON_DPAD_RIGHT = 1 << 12,
};

/* the controllers' layout, for what the game tells the player */
enum
{
	HALO_XR_LAYOUT_TOUCH = 0, /* the Quest's: X, Y and menu on the left */
	HALO_XR_LAYOUT_FRAME = 1, /* the Steam Frame's: A, B, X, Y and menu on the right, a D-pad on the left */
};

/* halo_xr_hand.flags, halo_xr_view.flags, halo_xr_frame.head_flags */
enum
{
	HALO_XR_POSE_VALID = 1 << 0,
	HALO_XR_POSE_TRACKED = 1 << 1,
};

struct halo_xr_pose
{
	float orientation[4];
	float position[3];
};

struct halo_xr_view
{
	int flags;
	struct halo_xr_pose pose;
	/* tangents of the view's edges (left and down negative) */
	float tangent_left;
	float tangent_right;
	float tangent_up;
	float tangent_down;
};

struct halo_xr_hand
{
	int flags;
	int active;             /* a controller of this hand is connected */
	struct halo_xr_pose grip;
	struct halo_xr_pose aim;
	float trigger;          /* 0..1 */
	float squeeze;          /* 0..1 */
	float thumbstick[2];    /* x right, y up; -1..1 */
	int buttons;            /* HALO_XR_BUTTON_* */
};

struct halo_xr_info
{
	int eye_width;          /* the eye images' size in pixels */
	int eye_height;
	int overlay_width;
	int overlay_height;
	float refresh_rate;     /* Hz; 0 if unknown */
	char system_name[64];
};

struct halo_xr_frame
{
	int session_running;    /* frames are being submitted to the headset */
	int should_render;      /* the eye images will be shown this frame */
	int focused;            /* input goes to the app (no system menu over it) */
	int recentred;          /* the LOCAL space moved since the last frame */
	float predicted_display_seconds;
	int head_flags;
	struct halo_xr_pose head;
	struct halo_xr_view views[HALO_XR_EYE_COUNT];
	struct halo_xr_hand hands[HALO_XR_HAND_COUNT];
	int controller_layout;  /* HALO_XR_LAYOUT_* */
};

/* halo_xr_submit.overlay_mode */
enum
{
	HALO_XR_OVERLAY_NONE = 0,
	HALO_XR_OVERLAY_HEAD_LOCKED = 1, /* the HUD: follows the head */
	HALO_XR_OVERLAY_SCREEN = 2,      /* menus and cinematics: a screen fixed in the room */
};

struct halo_xr_submit
{
	int eyes;               /* the eye layers were filled this frame */
	int overlay_mode;       /* HALO_XR_OVERLAY_* */
	int overlay_blend;      /* the overlay has alpha (premultiplied): it is drawn over the eyes */
	float overlay_distance; /* metres */
	float overlay_width;    /* metres; the height follows from 4:3 */
	/* the screen's placement in LOCAL space (HALO_XR_OVERLAY_SCREEN) */
	struct halo_xr_pose screen;
	/* the wrist layer was filled: shown on the back of the left wrist,
	this wide (metres), while the wrist is turned to the eyes */
	int wrist;
	float wrist_size;
	/* its placement as the wrist turned to the eyes reads it: turned this
	many degrees clockwise, and moved this far right and down (metres) */
	float wrist_turn;
	float wrist_right;
	float wrist_down;
	/* the scope layer was filled: a disc this wide (metres), here in LOCAL
	space, facing back along the weapon */
	int scope;
	float scope_size;
	struct halo_xr_pose scope_pose;
	/* the crosshair layer was filled: shown this wide (metres), here in
	LOCAL space, facing the eyes */
	int crosshair;
	float crosshair_size;
	struct halo_xr_pose crosshair_pose;
	/* the names layer was filled: its first name_count slots, each shown
	this wide (metres, a slot's height following from its shape), here in
	LOCAL space, facing the eyes */
	int name_count;
	float name_widths[HALO_XR_MAXIMUM_NAMES];
	struct halo_xr_pose name_poses[HALO_XR_MAXIMUM_NAMES];
};

#endif
