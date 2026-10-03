/*
GAME_ENGINE_RACE.H
*/

#ifndef __GAME_ENGINE_RACE_H
#define __GAME_ENGINE_RACE_H
#pragma once

/* ---------- headers */

#include "game/game_engine.h"

/* ---------- structures */

struct race_globals;

/* ---------- prototypes/GAME_ENGINE_RACE.C */

void race_flags_make_unique(
	void);
/* port: the race flag a player is to touch next, the one of those it may
touch nearest the point (the VR port's bots, port/linux/game/bots.c);
FALSE: none */
boolean game_engine_race_next_flag(
	long player_index,
	real_point3d const *from,
	real_point3d *position);

/* ---------- globals */

extern struct game_engine race_engine;

#endif // __GAME_ENGINE_RACE_H
