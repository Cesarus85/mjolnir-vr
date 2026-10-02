/*
BOTS.C

Bots in multiplayer (the VR port's): players of the host that it drives
itself. They are the players of machines of the host's own that nothing
connects to (network_server_manager.c: bots.count of them, added in the
lobby), so every machine sees them as it sees any other machine's players,
and their input goes where a client's does: every tick, the host makes each
bot's player_action and takes it as a client's input
(update_server_handle_distributed_input).

This first brain (phase 0 of the bots' plan) is simple: a bot looks for the
nearest enemy it can see, turns to it no faster than a player would (and
not quite right), walks at it weaving from side to side and fires while it
is on target; with none in sight it wanders, turning away from what stops
it. Finding its way (the maps' pathfinding, ai/path.c) comes next.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "units/units.h"
#include "network_distributed.h"

#include <math.h>

void platform_log(char const *format, ...);
boolean network_game_server_is_bot_machine(long machine_index);

/* ---------- constants */

enum
{
	MAXIMUM_BOTS = 16,
	/* how long a bot takes to notice an enemy (ticks) */
	REACTION_TICKS = 9,
};

/* radians a tick a bot turns at most */
#define BOT_TURN_RATE (3.5f / 30.0f)
/* how far a bot sees and shoots (world units), and how near it closes */
#define BOT_SIGHT_RANGE 40.0f
#define BOT_FIRE_RANGE 30.0f
#define BOT_CLOSE_RANGE 3.0f
/* how far off target it still fires (radians) */
#define BOT_FIRE_CONE 0.08f

/* ---------- structures */

struct bot
{
	long player_index;
	long target_unit_index;
	short target_seen_ticks;
	real yaw;
	real pitch;
	real aim_error_yaw;
	real aim_error_pitch;
	short error_ticks;
	real_point3d last_position;
	short stuck_ticks;
	real wander_yaw;
	short weave_phase;
};

/* ---------- globals */

static struct
{
	boolean map_logged;
	struct bot bots[MAXIMUM_BOTS];
	long seed;
	long logged_time;
} bots;

/* ---------- private code */

static real bot_random(
	void)
{
	bots.seed = bots.seed * 1103515245 + 12345;
	return (real)((bots.seed >> 16) & 0x7FFF) / 32767.0f;
}

static real bot_angle_difference(
	real to,
	real from)
{
	real difference = to - from;

	while (difference > _pi)
		difference -= 2.0f * _pi;
	while (difference < -_pi)
		difference += 2.0f * _pi;
	return difference;
}

static boolean bot_can_see(
	long unit_index,
	real_point3d const *from,
	real_point3d const *to)
{
	struct collision_result collision;
	real_vector3d vector;

	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	return !collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		from, &vector, unit_index, &collision);
}

/* the nearest enemy unit the bot can see, or NONE */
static long bot_find_target(
	struct player_datum const *player,
	real_point3d const *eye)
{
	boolean teams = game_engine_has_teams();
	long best = NONE;
	real best_distance = BOT_SIGHT_RANGE;
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *other = distributed_player(index);
		long unit_index;
		real_point3d head;
		real distance;

		if (!other || other == player || (teams && other->team_index == player->team_index))
			continue;
		unit_index = distributed_living_unit(other);
		if (unit_index == NONE)
			continue;
		unit_get_camera_position(unit_index, &head);
		distance = distance3d(eye, &head);
		if (distance < best_distance && bot_can_see(player->unit_index, eye, &head))
		{
			best = unit_index;
			best_distance = distance;
		}
	}
	return best;
}

static void bot_think(
	struct bot *bot,
	struct player_datum *player,
	struct player_action *action)
{
	struct unit_datum *unit = unit_try_and_get(player->unit_index);
	real_point3d eye;
	real_point3d position;
	real wanted_yaw;
	real wanted_pitch = 0.0f;
	boolean firing = FALSE;
	real moved;

	csmemset(action, 0, sizeof(*action));
	action->desired_weapon_index = NONE;
	action->desired_grenade_index = NONE;
	action->desired_zoom_level = NONE;
	if (!unit || unit->object.parent_object_index != NONE)
	{
		bot->target_unit_index = NONE;
		return;
	}
	action->desired_weapon_index = unit->unit.current_weapon_index;
	action->desired_grenade_index = unit->unit.current_grenade_index;
	unit_get_camera_position(player->unit_index, &eye);
	object_get_origin(player->unit_index, &position);
	/* (where it faces now, the first time) */
	if (bot->error_ticks == 0 && bot->yaw == 0.0f && bot->pitch == 0.0f)
	{
		bot->yaw = (real)atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i);
		bot->wander_yaw = bot->yaw;
	}

	/* the enemy: noticed after a moment, kept while in sight */
	{
		long target = bot_find_target(player, &eye);

		if (target != bot->target_unit_index)
		{
			bot->target_unit_index = target;
			bot->target_seen_ticks = 0;
		}
		else if (target != NONE && bot->target_seen_ticks < SHORT_MAX)
		{
			bot->target_seen_ticks++;
		}
	}
	/* (its aim wanders a little, anew now and then) */
	if (--bot->error_ticks <= 0)
	{
		bot->error_ticks = 10 + (short)(bot_random() * 20.0f);
		bot->aim_error_yaw = (bot_random() - 0.5f) * 0.06f;
		bot->aim_error_pitch = (bot_random() - 0.5f) * 0.04f;
	}

	if (bot->target_unit_index != NONE && bot->target_seen_ticks >= REACTION_TICKS)
	{
		real_point3d head;
		real dx, dy, dz, flat, distance;

		unit_get_camera_position(bot->target_unit_index, &head);
		/* (at the chest) */
		head.z -= 0.15f;
		dx = head.x - eye.x;
		dy = head.y - eye.y;
		dz = head.z - eye.z;
		flat = (real)sqrt(dx * dx + dy * dy);
		distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
		wanted_yaw = (real)atan2(dy, dx) + bot->aim_error_yaw;
		wanted_pitch = (real)atan2(dz, flat) + bot->aim_error_pitch;
		/* walks at it, weaving, until near */
		bot->weave_phase++;
		action->throttle.i = distance > BOT_CLOSE_RANGE ? 1.0f : 0.0f;
		action->throttle.j = (real)sin(bot->weave_phase * 0.08f) * 0.8f;
		firing = distance < BOT_FIRE_RANGE &&
			fabsf(bot_angle_difference(wanted_yaw, bot->yaw)) < BOT_FIRE_CONE &&
			fabsf(wanted_pitch - bot->pitch) < BOT_FIRE_CONE;
		/* (a melee blow when it is upon it) */
		if (distance < 1.0f)
			SET_FLAG(action->control_flags, _unit_control_use_equipment_bit, TRUE);
	}
	else
	{
		/* wanders: on, turning away from what stops it */
		wanted_yaw = bot->wander_yaw;
		action->throttle.i = 1.0f;
		if (bot->stuck_ticks > 20)
		{
			bot->wander_yaw += _pi * (0.5f + bot_random());
			bot->stuck_ticks = 0;
		}
		else if (bot_random() < 0.01f)
		{
			bot->wander_yaw += (bot_random() - 0.5f) * 1.5f;
		}
	}

	/* turned no faster than a player turns */
	{
		real turn = bot_angle_difference(wanted_yaw, bot->yaw);

		bot->yaw += PIN(turn, -BOT_TURN_RATE, BOT_TURN_RATE);
		bot->pitch += PIN(wanted_pitch - bot->pitch, -BOT_TURN_RATE, BOT_TURN_RATE);
		bot->pitch = PIN(bot->pitch, -1.4f, 1.4f);
		while (bot->yaw > _pi)
			bot->yaw -= 2.0f * _pi;
		while (bot->yaw < -_pi)
			bot->yaw += 2.0f * _pi;
	}
	action->desired_facing.yaw = bot->yaw < 0.0f ? bot->yaw + 2.0f * _pi : bot->yaw;
	action->desired_facing.pitch = bot->pitch;
	if (firing)
	{
		action->primary_trigger = 1.0f;
		SET_FLAG(action->control_flags, _unit_control_weapon_primary_trigger_bit, TRUE);
	}

	/* stuck: hardly moved while walking (it jumps, and then turns) */
	moved = distance3d(&position, &bot->last_position);
	bot->last_position = position;
	if (action->throttle.i > 0.5f && moved < 0.02f)
	{
		bot->stuck_ticks++;
		if (bot->stuck_ticks == 8)
			SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
	}
	else
	{
		bot->stuck_ticks = 0;
	}
}

/* the bot of a player, made when first needed */
static struct bot *bot_get(
	long player_index)
{
	short index;
	struct bot *free_bot = NULL;

	for (index = 0; index < MAXIMUM_BOTS; index++)
	{
		if (bots.bots[index].player_index == player_index)
			return &bots.bots[index];
		if (bots.bots[index].player_index == NONE && !free_bot)
			free_bot = &bots.bots[index];
	}
	if (free_bot)
	{
		csmemset(free_bot, 0, sizeof(*free_bot));
		free_bot->player_index = player_index;
		free_bot->target_unit_index = NONE;
	}
	return free_bot;
}

/* ---------- public code */

void bots_new_game(
	void)
{
	short index;

	bots.map_logged = FALSE;
	for (index = 0; index < MAXIMUM_BOTS; index++)
		bots.bots[index].player_index = NONE;
	bots.seed = 12345;
	bots.logged_time = 0;
}

/* (the host) after each tick: each bot's input for the next */
void bots_tick(
	void)
{
	short index;
	short count = 0;

	if (!bots.map_logged)
	{
		struct structure_bsp *bsp = global_structure_bsp_get();

		bots.map_logged = TRUE;
		if (bsp)
		{
			platform_log("bots: the map's pathfinding: %ld surfaces, %ld edges",
				bsp->pathfinding_surfaces.count, bsp->pathfinding_edges.count);
		}
	}
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *player = distributed_player(index);
		struct bot *bot;
		struct player_action action;
		unsigned short control_flags;

		if (!player || player->local_player_index != NONE ||
			!network_game_server_is_bot_machine(player->network_player_data.machine_index))
		{
			continue;
		}
		bot = bot_get(DATUM_INDEX_NEW(index, player->identifier));
		if (!bot)
			continue;
		bot_think(bot, player, &action);
		control_flags = (unsigned short)action.control_flags;
		update_server_handle_distributed_input(DATUM_INDEX_NEW(index, player->identifier), game_time_get(), &action,
			&control_flags, 1);
		count++;
	}
	if (count && game_time_get() - bots.logged_time >= 300)
	{
		bots.logged_time = game_time_get();
		platform_log("bots: driving %d", (int)count);
	}
}
