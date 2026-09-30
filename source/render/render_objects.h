/*
RENDER_OBJECTS.H
*/

#ifndef __RENDER_OBJECTS_H
#define __RENDER_OBJECTS_H
#pragma once

/* ---------- headers */

#include "cseries.h"

/* ---------- structures */

struct render_lighting;

/* ---------- prototypes/RENDER_OBJECTS.C */

void render_objects_initialize(
	void);
void render_objects_initialize_for_new_map(
	void);
void render_objects_dispose_from_old_map(
	void);
void render_objects_dispose(
	void);
struct render_lighting *object_get_cached_render_lighting(
	long object_index,
	real level_of_detail_pixels);
void render_objects(
	void);
void render_object_shadows(
	void);

#ifdef HALO_ANDROID
/* VR: whether a player's own unit is drawn, seated at a wheel or a gun (the
first-person weapon is not, then) */
boolean render_object_vr_seated_body(
	long object_index);
/* - and whether it sits at a wheel or a gun at all */
boolean render_object_vr_at_controls(
	long object_index);
#endif

#endif // __RENDER_OBJECTS_H
