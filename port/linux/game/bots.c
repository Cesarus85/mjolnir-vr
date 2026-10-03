/*
BOTS.C

Bots in multiplayer (the VR port's): players of the host that it drives
itself. They are the players of machines of the host's own that nothing
connects to (network_server_manager.c: bots.count of them, added in the
lobby), so every machine sees them as it sees any other machine's players,
and their input goes where a client's does: every tick, the host makes each
bot's player_action and takes it as a client's input
(update_server_handle_distributed_input).

A bot looks for the nearest enemy it can see, turns to it no faster than a
player would (and not quite right), closes on it weaving from side to side
and fires while it is on target. With none in sight it hunts the nearest
enemy, or else roams to a point picked at random near someone. It finds its
way with the game's own pathfinding (ai/path.c: the maps' pathfinding
surfaces), as the campaign's actors do: a path a few times a second, whose
next point it walks to, looking where it goes; where no path is found it
walks straight at the goal, jumping and turning off when it is stuck.

Out of a fight, a bot goes for what lies near that is worth having: a
weapon better than the worse of its two (and swaps that one for it, as a
player holding the action button does), ammunition for one it has when it
runs low, an overshield, camouflage, a health pack when hurt, grenades.
In a fight it changes to its other weapon when the one in hand is empty,
and now and then lobs a grenade at an enemy some way off.

In the objective games a bot plays for the objective, fighting on the way:
- capture the flag: the carrier of the enemy's flag runs it home; the rest
  hunt whoever carries their own flag, return it where it lies dropped, or
  else go for the enemy's flag (every third bot stays to guard its own);
- king of the hill: to the hill, and around in it;
- oddball: to the ball (taken as a weapon is), hunting whoever carries it;
  with it, away from the enemy, striking those who come near.

In a game of teams a bot plays with its team: it holds its fire while a
teammate is in the line of it, takes on first the enemies near its
teammates (people above all), escorts a teammate who carries the enemy's
flag or the ball, and, with nothing to do, every other bot keeps near the
nearest person on its team.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "game/player_queues_new.h"
#include "objects/objects.h"
#include "physics/collisions.h"
#include "ai/path.h"
#include "units/bipeds.h"
#include "items/items.h"
#include "items/weapons.h"
#include "tag_files/tag_files.h"
#include "scenario/scenario_definitions.h"

#include <string.h>
#include "scenario/scenario.h"
#include "structures/structure_bsp_definitions.h"
#include "units/units.h"
#include "network_distributed.h"

#include <math.h>

void platform_log(char const *format, ...);
long config_integer(char const *name);
boolean network_game_server_is_bot_machine(long machine_index);

/* ---------- constants */

enum
{
	MAXIMUM_BOTS = 16,
	/* how often a bot finds its path again (ticks) */
	PATH_REFRESH_TICKS = 15,
	/* how often it looks around for things to pick up (ticks) */
	ITEM_LOOK_TICKS = 30,
	/* the least time between its grenades (ticks) */
	GRENADE_INTERVAL_TICKS = 150,
};

/* how far off it goes for things (world units) */
#define BOT_ITEM_RANGE 25.0f
/* a grenade's range: at least, at most (world units) */
#define BOT_GRENADE_MINIMUM 6.0f
#define BOT_GRENADE_MAXIMUM 20.0f

/* how near a path's point counts as reached, and the goal (world units) */
#define BOT_STEP_RADIUS 0.4f
#define BOT_GOAL_RADIUS 1.0f

/* how far a bot sees and shoots (world units), and how near it closes */
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
	/* the point it walks to, how near it was, and a step aside (ticks
	left, and which way) when it makes no way */
	real_point3d walk_point;
	real walk_distance;
	short sidestep_ticks;
	real sidestep;
	/* whether it walks to a point this tick (not only weaving in a fight) */
	boolean walking;
	/* the paths to its goal not found in a row, and goals given up for a
	while on that account: following a person, a thing to pick up */
	short path_failures;
	long follow_again_time;
	long ignored_item_index;
	long ignored_item_time;
	real wander_yaw;
	short weave_phase;
	/* where it is going, and the way there */
	boolean has_goal;
	real_point3d goal;
	long goal_time;
	long path_time;
	struct path_result path;
	/* the thing it goes for, and when it last looked */
	long item_index;
	long item_look_time;
	long grenade_time;
	long weapon_switch_time;
	/* the objective, as it played it this tick: where it goes, whom it
	hunts first, and whether it carries the flag or ball (no firing) */
	boolean has_objective;
	real_point3d objective;
	long objective_unit_index;
	boolean carrying;
};

/* the objective game's things this tick: the flags or balls (weapons), whose
team or ball number, where, and who carries them */
enum
{
	MAXIMUM_OBJECTIVE_ITEMS = 8,
};

struct objective_item
{
	long item_index;
	short owner;
	real_point3d position;
	long carrier_player_index;
	boolean handled;
};

/* ---------- globals */

/* (one search at a time, shared: 80 kB) */
static struct path_state bot_path_state;
static struct path_debug_storage bot_path_debug;

static struct
{
	boolean map_logged;
	struct bot bots[MAXIMUM_BOTS];
	long seed;
	long logged_time;
	long paths_found;
	long paths_failed;
	long grenades;
	long pickups;
	long switches;
	long stucks;
	long long_stucks;
	long fail_start;
	long fail_goal;
	long fail_search;
	/* the objective game's things (bots_update_objectives) */
	short objective_item_count;
	struct objective_item objective_items[MAXIMUM_OBJECTIVE_ITEMS];
	/* how well they play (bots.skill, as the game began) */
	short reaction_ticks;
	real turn_rate;
	real sight_range;
	real aim_error;
} bots;

/* easy, normal, hard: the ticks a bot takes to notice an enemy, how fast it
turns (radians a tick), how far it sees and how far off its aim wanders */
static struct
{
	short reaction_ticks;
	real turn_rate;
	real sight_range;
	real aim_error;
} const bot_skills[] =
{
	{ 20, 2.2f / 30.0f, 25.0f, 3.0f },
	{ 13, 2.8f / 30.0f, 32.0f, 1.8f },
	{ 9, 3.5f / 30.0f, 40.0f, 1.0f },
};

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

/* ---------- the team */

/* whether a player is a person (on a machine of its own, not a bot's) */
static boolean bot_player_is_person(
	struct player_datum const *player)
{
	return !network_game_server_is_bot_machine(player->network_player_data.machine_index);
}

/* how much nearer an enemy at this point counts for being near the bot's
teammates: up to 10 world units for one by a person, 6 by a bot */
static real bot_threat_to_team(
	struct player_datum const *player,
	real_point3d const *enemy)
{
	real threat = 0.0f;
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *mate = distributed_player(index);
		long unit_index;
		real_point3d head;
		real distance;
		real most;

		if (!mate || mate == player || mate->team_index != player->team_index)
			continue;
		unit_index = distributed_living_unit(mate);
		if (unit_index == NONE)
			continue;
		unit_get_camera_position(unit_index, &head);
		distance = distance3d(enemy, &head);
		most = bot_player_is_person(mate) ? 10.0f : 6.0f;
		if (distance < 12.0f)
			threat = MAX(threat, most * (1.0f - distance / 12.0f));
	}
	return threat;
}

/* whether a teammate stands in the line of fire to a point (nearer than
it, within 0.8 world units of the line) */
static boolean bot_teammate_in_line(
	struct player_datum const *player,
	real_point3d const *eye,
	real_point3d const *target)
{
	real_vector3d line;
	real length;
	short index;

	if (!game_engine_has_teams())
		return FALSE;
	vector_from_points3d(eye, target, &line);
	length = magnitude3d(&line);
	if (length < 0.01f)
		return FALSE;
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *mate = distributed_player(index);
		long unit_index;
		real_point3d center;
		real_vector3d offset;
		real along;
		real_point3d nearest;

		if (!mate || mate == player || mate->team_index != player->team_index)
			continue;
		unit_index = distributed_living_unit(mate);
		if (unit_index == NONE)
			continue;
		unit_get_camera_position(unit_index, &center);
		center.z -= 0.25f;
		vector_from_points3d(eye, &center, &offset);
		along = dot_product3d(&offset, &line) / length;
		if (along <= 0.0f || along >= length)
			continue;
		nearest.x = eye->x + line.i * along / length;
		nearest.y = eye->y + line.j * along / length;
		nearest.z = eye->z + line.k * along / length;
		if (distance3d(&nearest, &center) < 0.8f)
			return TRUE;
	}
	return FALSE;
}

/* the nearest person on the bot's team, alive (NONE: none) */
static long bot_nearest_person(
	struct player_datum const *player,
	real_point3d const *position)
{
	long best = NONE;
	real best_distance = 40.0f;
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *mate = distributed_player(index);
		long unit_index;
		real_point3d origin;
		real distance;

		if (!mate || mate == player || mate->team_index != player->team_index || !bot_player_is_person(mate))
			continue;
		unit_index = distributed_living_unit(mate);
		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		distance = distance3d(position, &origin);
		if (distance < best_distance)
		{
			best_distance = distance;
			best = unit_index;
		}
	}
	return best;
}

/* the nearest enemy unit the bot can see, or NONE */
static long bot_find_target(
	struct player_datum const *player,
	real_point3d const *eye)
{
	boolean teams = game_engine_has_teams();
	long best = NONE;
	real best_distance = bots.sight_range;
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
		if (distance >= bots.sight_range)
			continue;
		/* (one near a teammate counts as nearer: helped first) */
		if (teams)
			distance -= bot_threat_to_team(player, &head);
		if (distance < best_distance && bot_can_see(player->unit_index, eye, &head))
		{
			best = unit_index;
			best_distance = distance;
		}
	}
	return best;
}

/* the way from where the bot's unit stands to the goal (FALSE: none) */
static boolean bot_find_path(
	struct bot *bot,
	long unit_index)
{
	struct path_input input;
	real_point3d start;
	long start_surface = biped_find_pathfinding_surface_index(unit_index, &start);
	real_point3d goal = bot->goal;
	long goal_surface = NONE;

	bot->path.valid = FALSE;
	if (start_surface == NONE)
	{
		bots.fail_start++;
		return FALSE;
	}
	/* (the goal's surface: under it, as a biped would stand there) */
	{
		struct collision_result collision;
		real_vector3d down = { 0.0f, 0.0f, -3.0f };
		real_point3d above = goal;

		above.z += 0.5f;
		if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
			&above, &down, NONE, &collision))
		{
			goal = collision.point;
			goal_surface = collision.surface_index;
		}
	}
	if (goal_surface == NONE)
	{
		bots.fail_goal++;
		bot->path_failures++;
		return FALSE;
	}
	/* (a little wider than a biped: paths that keep off walls and corners) */
	path_input_new(&input, 0.25f, FALSE, unit_index);
	path_input_set_start(&input, &start, start_surface);
	bot_path_debug.path_traverse_result = 0;
	bot_path_debug.path_build_result = 0;
	path_state_new(&input, &bot_path_state, &bot_path_debug);
	path_state_destination(&bot_path_state, &goal, goal_surface, BOT_GOAL_RADIUS);
	if (path_state_find(&bot_path_state) && !path_state_build_path(&bot_path_state, &bot->path) &&
		bot_path_debug.path_build_result == _path_build_result_obstacle_avoidance_failed &&
		bot_path_debug.smoothed_step_count > 0)
	{
		/* (a way there, but none round the objects on it, the other players
		among them: the way, smoothed, without; a step aside settles a
		bump) */
		short step;

		csmemset(&bot->path, 0, sizeof(bot->path));
		bot->path.valid = TRUE;
		bot->path.step_count = (char)MIN(bot_path_debug.smoothed_step_count, 4);
		for (step = 0; step < bot->path.step_count; step++)
			bot->path.steps[step] = bot_path_debug.smoothed_steps[step];
		bot->path.endpoint.point = bot->path.steps[bot->path.step_count - 1].point;
		bot->path.endpoint.surface_index = bot->path.steps[bot->path.step_count - 1].surface_index;
		bot->path.steps_finish_path = bot_path_debug.raw_step_count <= 4;
		bots.paths_found++;
		return TRUE;
	}
	if (!bot->path.valid)
	{
		bot->path.valid = FALSE;
		bots.paths_failed++;
		bots.fail_search++;
		bot->path_failures++;
		return FALSE;
	}
	bots.paths_found += bot->path.valid != FALSE;
	bot->path_failures = 0;
	return bot->path.valid;
}

/* the point it walks to now: the path's next, or the goal itself */
/* whether a biped walks straight from one point to the other: nothing of
the level between them at its knees */
static boolean bot_clear_walk(
	long unit_index,
	real_point3d const *from,
	real_point3d const *to)
{
	struct collision_result collision;
	real_point3d start = *from;
	real_vector3d vector;

	start.z += 0.3f;
	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	return !collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&start, &vector, unit_index, &collision);
}

static boolean bot_next_point_now(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	real_point3d *point);

/* the point it walks to now (kept, to tell whether it gets nearer) */
static boolean bot_next_point(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	real_point3d *point)
{
	if (!bot_next_point_now(bot, unit_index, position, point))
		return FALSE;
	if (distance3d(point, &bot->walk_point) > 0.2f)
		bot->walk_distance = 1000000.0f;
	bot->walk_point = *point;
	bot->walking = TRUE;
	return TRUE;
}

static boolean bot_next_point_now(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	real_point3d *point)
{
	if (!bot->has_goal)
		return FALSE;
	/* (the last steps: straight there, over what the path goes round, as
	a flag on its stand, where a carrier must step to score, when nothing
	of the level is in the way) */
	{
		real dx = bot->goal.x - position->x;
		real dy = bot->goal.y - position->y;

		if (dx * dx + dy * dy < 3.0f * 3.0f && fabsf(bot->goal.z - position->z) < 0.6f &&
			bot_clear_walk(unit_index, position, &bot->goal))
		{
			*point = bot->goal;
			return TRUE;
		}
	}
	if (game_time_get() - bot->path_time >= PATH_REFRESH_TICKS || game_time_get() < bot->path_time)
	{
		bot->path_time = game_time_get();
		bot_find_path(bot, unit_index);
	}
	if (bot->path.valid)
	{
		while (bot->path.step_index < bot->path.step_count)
		{
			struct path_step const *step = &bot->path.steps[(short)bot->path.step_index];
			real dx = step->point.x - position->x;
			real dy = step->point.y - position->y;

			if (dx * dx + dy * dy > BOT_STEP_RADIUS * BOT_STEP_RADIUS)
			{
				/* (the next point in plain view: on to it, not round by this
				one, a corner's) */
				real_point3d const *next = bot->path.step_index + 1 < bot->path.step_count ?
					&bot->path.steps[(short)bot->path.step_index + 1].point : &bot->path.endpoint.point;

				/* (on its own level only: one above may be in view over a
				ledge that a ramp leads round to) */
				if (fabsf(next->z - position->z) < 0.6f && bot_clear_walk(unit_index, position, next))
				{
					bot->path.step_index++;
					if (bot->path.step_index >= bot->path.step_count)
						break;
					continue;
				}
				*point = step->point;
				return TRUE;
			}
			bot->path.step_index++;
		}
		/* (the steps done: on to the end, or a path found anew soon) */
		if (!bot->path.steps_finish_path && game_time_get() - bot->path_time > 5)
			bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
		*point = bot->path.endpoint.point;
		return TRUE;
	}
	*point = bot->goal;
	return TRUE;
}

/* a goal when none is in sight: the nearest enemy, or somewhere near
someone at random */
static void bot_choose_goal(
	struct bot *bot,
	struct player_datum const *player,
	real_point3d const *position)
{
	boolean teams = game_engine_has_teams();
	real best_distance = 1000000.0f;
	short index;
	boolean found = FALSE;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *other = distributed_player(index);
		long unit_index;
		real_point3d origin;
		real distance;

		if (!other || other == player || (teams && other->team_index == player->team_index))
			continue;
		unit_index = distributed_living_unit(other);
		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		distance = distance3d(position, &origin);
		if (distance < best_distance)
		{
			best_distance = distance;
			bot->goal = origin;
			found = TRUE;
		}
	}
	if (!found)
	{
		/* (no enemy alive: a few metres off, somewhere) */
		real angle = bot_random() * 2.0f * _pi;

		bot->goal = *position;
		bot->goal.x += (real)cos(angle) * 5.0f;
		bot->goal.y += (real)sin(angle) * 5.0f;
	}
	bot->has_goal = TRUE;
	bot->goal_time = game_time_get();
	bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
}

/* ---------- things to pick up */

/* how much a weapon is worth having, by its tag's name (0: not known) */
static real bot_weapon_worth(
	long definition_index)
{
	static struct
	{
		char const *name;
		real worth;
	} const weapons[] =
	{
		{ "rocket launcher", 9.0f }, { "sniper rifle", 8.0f }, { "fuel rod", 8.0f }, { "shotgun", 7.0f },
		{ "flamethrower", 6.0f }, { "pistol", 6.0f }, { "plasma rifle", 5.0f }, { "assault rifle", 5.0f },
		{ "needler", 4.0f }, { "plasma pistol", 4.0f },
	};
	char const *name = tag_get_name(definition_index);
	short index;

	if (!name)
		return 0.0f;
	/* (plasma pistol before pistol: the longer name first) */
	if (strstr(name, "plasma pistol"))
		return 4.0f;
	for (index = 0; index < (short)NUMBEROF(weapons); index++)
	{
		if (strstr(name, weapons[index].name))
			return weapons[index].worth;
	}
	return 3.0f;
}

/* how well a weapon suits a fight at that distance (world units) */
static real bot_weapon_suits(
	long definition_index,
	real distance)
{
	char const *name = tag_get_name(definition_index);
	real worth = bot_weapon_worth(definition_index);

	if (!name)
		return worth;
	if (strstr(name, "shotgun") || strstr(name, "flamethrower"))
		return distance < 6.0f ? worth + 5.0f : distance > 10.0f ? worth - 5.0f : worth;
	if (strstr(name, "sniper rifle"))
		return distance > 12.0f ? worth + 3.0f : worth - 4.0f;
	if (strstr(name, "rocket launcher") || strstr(name, "fuel rod"))
		return distance < 4.0f ? worth - 8.0f : worth + 1.0f;
	if (strstr(name, "plasma pistol") || strstr(name, "needler"))
		return worth;
	if (strstr(name, "pistol"))
		return distance > 10.0f ? worth + 1.0f : worth - 0.5f;
	if (strstr(name, "assault rifle") || strstr(name, "plasma rifle"))
		return distance < 12.0f ? worth + 1.5f : worth;
	return worth;
}

/* whether a weapon has rounds left (a battery: charge) */
static boolean bot_weapon_has_ammunition(
	long weapon_index)
{
	struct weapon_datum *weapon = weapon_try_and_get(weapon_index);

	if (!weapon)
		return FALSE;
	if (weapon->weapon.magazines[0].rounds_total > 0 || weapon->weapon.magazines[0].rounds_loaded > 0)
		return TRUE;
	/* (no magazine: a battery, spent when its age reaches 1) */
	return weapon->weapon.magazines[0].rounds_loaded == 0 && weapon->weapon.magazines[0].rounds_total == 0 &&
		weapon->weapon.age < 1.0f && weapon->weapon.magazines[0].original_time == 0;
}

/* how much the bot would gain by a thing lying there (0: nothing) */
static real bot_item_worth(
	struct unit_datum const *unit,
	long item_index)
{
	struct object_datum *object = object_get(item_index);
	char const *name = tag_get_name(object->definition_index);
	short slot;

	if (!name)
		return 0.0f;
	if (object->object.parent_object_index != NONE)
		return 0.0f;
	if (TEST_FLAG(_object_mask_equipment, object->object.type))
	{
		if (strstr(name, "over shield") || strstr(name, "overshield"))
			return 9.0f;
		if (strstr(name, "camouflage"))
			return 8.0f;
		if (strstr(name, "health"))
			return unit->object.body_vitality < 0.7f ? 7.0f : 0.0f;
		if (strstr(name, "frag grenade"))
			return unit->unit.grenade_counts[0] < 2 ? 3.0f : 0.0f;
		if (strstr(name, "plasma grenade"))
			return NUMBER_OF_UNIT_GRENADE_TYPES > 1 && unit->unit.grenade_counts[1] < 2 ? 3.0f : 0.0f;
		return 0.0f;
	}
	if (TEST_FLAG(_object_mask_weapon, object->object.type))
	{
		real worth = bot_weapon_worth(object->definition_index);
		real worst = 1000.0f;
		short carried = 0;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			long carried_index = unit->unit.weapon_object_indices[slot];
			struct object_datum *carried_weapon = carried_index != NONE ? object_try_and_get(carried_index) : NULL;

			if (!carried_weapon)
				continue;
			carried++;
			/* (one it has: worth its ammunition when it runs low) */
			if (carried_weapon->definition_index == object->definition_index)
				return bot_weapon_has_ammunition(carried_index) ? 0.0f : 4.0f;
			worst = MIN(worst, bot_weapon_worth(carried_weapon->definition_index));
		}
		/* (with two, worth what it betters the worse by, and a little for a
		change of weapon) */
		if (carried < 2)
			return worth;
		return worth - worst > 0.5f ? worth - worst + 1.5f : 0.0f;
	}
	return 0.0f;
}

/* the thing most worth going for near the bot, or NONE */
static long bot_find_item(
	struct unit_datum const *unit,
	real_point3d const *position,
	real range,
	real least_worth)
{
	struct object_iterator iterator;
	long best = NONE;
	real best_score = 0.0f;

	object_iterator_new(&iterator, _object_mask_weapon | _object_mask_equipment, 0);
	while (object_iterator_next(&iterator))
	{
		real_point3d origin;
		real distance;
		real worth;
		real score;

		object_get_origin(iterator.index, &origin);
		distance = distance3d(position, &origin);
		if (distance > range)
			continue;
		worth = bot_item_worth(unit, iterator.index);
		if (worth <= 0.0f || worth < least_worth)
			continue;
		/* (the strong ones worth a longer way) */
		score = worth * worth / (1.0f + distance / 15.0f);
		if (score > best_score)
		{
			best_score = score;
			best = iterator.index;
		}
	}
	return best;
}

/* near a weapon it wants: the worse of its two in hand (the one a swap
leaves), and the action held while the game offers the weapon */
static void bot_take_item(
	struct bot *bot,
	struct player_datum const *player,
	struct unit_datum const *unit,
	struct player_action *action)
{
	struct object_datum *item = object_try_and_get(bot->item_index);
	short slot;
	short worst_slot = NONE;
	real worst = 1000.0f;
	short carried = 0;

	if (!item || !TEST_FLAG(_object_mask_weapon, item->object.type))
		return;
	for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
	{
		long carried_index = unit->unit.weapon_object_indices[slot];
		struct object_datum *carried_weapon = carried_index != NONE ? object_try_and_get(carried_index) : NULL;
		real worth;

		if (!carried_weapon)
			continue;
		carried++;
		worth = bot_weapon_worth(carried_weapon->definition_index);
		if (worth < worst)
		{
			worst = worth;
			worst_slot = slot;
		}
	}
	if (carried >= 2 && worst_slot != NONE)
		action->desired_weapon_index = worst_slot;
	if (player->action_object_index == bot->item_index &&
		(player->action_result == _player_action_result_swap_for_weapon ||
			player->action_result == _player_action_result_add_weapon_to_inventory) &&
		(game_time_get() & 1))
	{
		SET_FLAG(action->control_flags, _unit_control_swap_weapons_bit, TRUE);
		bots.pickups++;
	}
}

/* ---------- objectives */

static long bots_game_type(
	void)
{
	return game_engine ? game_engine->type : NONE;
}

/* the flags or balls this tick (once for all bots) */
static void bots_update_objectives(
	void)
{
	struct object_iterator iterator;
	long type = bots_game_type();

	bots.objective_item_count = 0;
	if (type != game_engine_ctf && type != game_engine_oddball)
		return;
	object_iterator_new(&iterator, _object_mask_weapon, 0);
	while (object_iterator_next(&iterator) && bots.objective_item_count < MAXIMUM_OBJECTIVE_ITEMS)
	{
		struct weapon_datum *weapon;
		struct objective_item *item;
		short index;

		if (!weapon_is_flag(iterator.index))
			continue;
		weapon = weapon_get(iterator.index);
		item = &bots.objective_items[bots.objective_item_count++];
		item->item_index = iterator.index;
		item->owner = weapon->object.owner_team_index;
		/* (bit 6: the flag was taken from its stand, game_engine_ctf.c) */
		item->handled = TEST_FLAG(weapon->weapon.flags, 6);
		item_get_position_even_if_in_inventory(iterator.index, &item->position);
		item->carrier_player_index = NONE;
		for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
		{
			struct player_datum *player = distributed_player(index);

			if (player && player->unit_index != NONE && unit_has_weapon(player->unit_index, iterator.index))
			{
				item->carrier_player_index = DATUM_INDEX_NEW(index, player->identifier);
				break;
			}
		}
	}
}

/* a team's flag stand (FALSE: none) */
static boolean bots_ctf_base(
	long team_index,
	real_point3d *position)
{
	struct scenario *scenario = global_scenario_get();
	struct game_variant *variant = game_engine_get_variant();
	long slot = variant && variant->game_engine_variant.ctf.assault ? (team_index + 1) % 2 : team_index;
	long index;

	if (!scenario)
		return FALSE;
	for (index = 0; index < scenario->netgame_flags.count; index++)
	{
		struct scenario_netgame_flag *flag =
			TAG_BLOCK_GET_ELEMENT(&scenario->netgame_flags, index, struct scenario_netgame_flag);

		if (flag->type == _netgame_flag_ctf_flag && flag->team_index == slot)
		{
			*position = flag->position;
			return TRUE;
		}
	}
	return FALSE;
}

/* near a teammate who carries something, a few world units off on the bot's
side of it (an escort, not on the carrier's heels) */
static void bot_escort_point(
	real_point3d const *position,
	real_point3d const *carried,
	real_point3d *point)
{
	real dx = position->x - carried->x;
	real dy = position->y - carried->y;
	real length = (real)sqrt(dx * dx + dy * dy);

	*point = *carried;
	if (length > 0.01f)
	{
		point->x += dx / length * 2.5f;
		point->y += dy / length * 2.5f;
	}
}

/* what the objective asks of the bot now (bot->has_objective and the rest) */
static void bot_play_objective_now(
	struct bot *bot,
	struct player_datum const *player,
	long player_index,
	real_point3d const *position);

static void bot_play_objective(
	struct bot *bot,
	struct player_datum const *player,
	long player_index,
	real_point3d const *position)
{
	boolean was_carrying = bot->carrying;

	bot_play_objective_now(bot, player, player_index, position);
	if (bot->carrying != was_carrying)
	{
		platform_log("bots: player %ld %s", DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index),
			bot->carrying ? "has taken the flag or ball" : "no longer carries it");
	}
}

static void bot_play_objective_now(
	struct bot *bot,
	struct player_datum const *player,
	long player_index,
	real_point3d const *position)
{
	long type = bots_game_type();
	short index;

	bot->has_objective = FALSE;
	bot->objective_unit_index = NONE;
	bot->carrying = FALSE;
	if (type == game_engine_king)
	{
		if (game_engine_get_goal_in_use(0))
		{
			real_point3d center;
			real dx, dy;

			game_engine_get_goal_position(&center, 0);
			center.z -= 0.63f;
			dx = center.x - position->x;
			dy = center.y - position->y;
			bot->has_objective = TRUE;
			bot->objective = center;
			/* (on the hill: about in it, not on its middle) */
			if (dx * dx + dy * dy < 1.5f * 1.5f)
			{
				bot->objective.x += (real)cos(bot->weave_phase * 0.05f) * 1.0f;
				bot->objective.y += (real)sin(bot->weave_phase * 0.05f) * 1.0f;
			}
		}
		return;
	}
	if (type == game_engine_ctf)
	{
		struct objective_item *own = NULL;
		struct objective_item *enemy = NULL;
		real_point3d base;

		for (index = 0; index < bots.objective_item_count; index++)
		{
			struct objective_item *item = &bots.objective_items[index];

			if (item->owner == player->team_index)
				own = item;
			else
				enemy = item;
		}
		/* carrying theirs: home */
		if (enemy && enemy->carrier_player_index == player_index)
		{
			bot->carrying = TRUE;
			if (bots_ctf_base(player->team_index, &base))
			{
				bot->has_objective = TRUE;
				bot->objective = base;
			}
			return;
		}
		/* ours carried off: after its carrier */
		if (own && own->carrier_player_index != NONE)
		{
			struct player_datum *carrier = player_try_and_get(own->carrier_player_index);

			bot->has_objective = TRUE;
			bot->objective = own->position;
			bot->objective_unit_index = carrier ? carrier->unit_index : NONE;
			return;
		}
		/* ours dropped: touched, it goes home */
		if (own && own->handled)
		{
			bot->has_objective = TRUE;
			bot->objective = own->position;
			return;
		}
		/* (every third guards its own) */
		if ((DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) % 3) == 2 && bots_ctf_base(player->team_index, &base))
		{
			bot->has_objective = TRUE;
			bot->objective = base;
			if (distance3d(position, &base) < 4.0f)
			{
				bot->objective.x += (real)cos(bot->weave_phase * 0.03f) * 3.0f;
				bot->objective.y += (real)sin(bot->weave_phase * 0.03f) * 3.0f;
			}
			return;
		}
		/* theirs: to take it, or to escort the teammate carrying it */
		if (enemy)
		{
			bot->has_objective = TRUE;
			if (enemy->carrier_player_index != NONE)
				bot_escort_point(position, &enemy->position, &bot->objective);
			else
				bot->objective = enemy->position;
		}
		return;
	}
	if (type == game_engine_oddball)
	{
		struct objective_item *nearest = NULL;
		real nearest_distance = 1000000.0f;

		for (index = 0; index < bots.objective_item_count; index++)
		{
			struct objective_item *item = &bots.objective_items[index];
			real distance;

			if (item->carrier_player_index == player_index)
			{
				bot->carrying = TRUE;
				return;
			}
			/* (one a teammate carries: escorted, not chased) */
			if (item->carrier_player_index != NONE && game_engine_has_teams())
			{
				struct player_datum *carrier = player_try_and_get(item->carrier_player_index);

				if (carrier && carrier->team_index == player->team_index)
				{
					if (!nearest)
					{
						bot->has_objective = TRUE;
						bot_escort_point(position, &item->position, &bot->objective);
					}
					continue;
				}
			}
			distance = distance3d(position, &item->position);
			if (distance < nearest_distance)
			{
				nearest_distance = distance;
				nearest = item;
			}
		}
		if (nearest)
		{
			bot->has_objective = TRUE;
			bot->objective = nearest->position;
			if (nearest->carrier_player_index != NONE)
			{
				struct player_datum *carrier = player_try_and_get(nearest->carrier_player_index);

				bot->objective_unit_index = carrier ? carrier->unit_index : NONE;
			}
			else
			{
				/* (lying there: taken as a weapon is) */
				bot->item_index = nearest->item_index;
			}
		}
	}
}

/* the throttle that walks a unit facing yaw towards a point */
static void bot_walk_to(
	struct player_action *action,
	real yaw,
	real_point3d const *position,
	real_point3d const *point)
{
	real dx = point->x - position->x;
	real dy = point->y - position->y;
	real length = (real)sqrt(dx * dx + dy * dy);

	if (length < 0.001f)
		return;
	dx /= length;
	dy /= length;
	/* (forward along the facing; left, its strafe's positive) */
	action->throttle.i = dx * (real)cos(yaw) + dy * (real)sin(yaw);
	action->throttle.j = -dx * (real)sin(yaw) + dy * (real)cos(yaw);
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
	boolean following = FALSE;
	real moved;

	csmemset(action, 0, sizeof(*action));
	bot->walking = FALSE;
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
	/* (the weapon in hand empty: the other) */
	if (unit->unit.current_weapon_index >= 0 &&
		!bot_weapon_has_ammunition(unit->unit.weapon_object_indices[unit->unit.current_weapon_index]))
	{
		short slot;

		for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
		{
			if (slot != unit->unit.current_weapon_index && unit->unit.weapon_object_indices[slot] != NONE &&
				bot_weapon_has_ammunition(unit->unit.weapon_object_indices[slot]))
			{
				action->desired_weapon_index = slot;
				break;
			}
		}
	}
	unit_get_camera_position(player->unit_index, &eye);
	object_get_origin(player->unit_index, &position);
	/* (where it faces now, the first time) */
	if (bot->error_ticks == 0 && bot->yaw == 0.0f && bot->pitch == 0.0f)
	{
		bot->yaw = (real)atan2(unit->unit.aiming_vector.j, unit->unit.aiming_vector.i);
		bot->wander_yaw = bot->yaw;
	}

	/* what the objective game asks */
	bot_play_objective(bot, player, bot->player_index, &position);

	/* the enemy: noticed after a moment, kept while in sight (the one the
	objective hunts first, when it is in sight) */
	{
		long target = bot_find_target(player, &eye);

		if (bot->objective_unit_index != NONE)
		{
			real_point3d head;

			unit_get_camera_position(bot->objective_unit_index, &head);
			if (distance3d(&eye, &head) < bots.sight_range && bot_can_see(player->unit_index, &eye, &head))
				target = bot->objective_unit_index;
		}

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
		bot->aim_error_yaw = (bot_random() - 0.5f) * 0.06f * bots.aim_error;
		bot->aim_error_pitch = (bot_random() - 0.5f) * 0.04f * bots.aim_error;
	}

	if (bot->target_unit_index != NONE && bot->target_seen_ticks >= bots.reaction_ticks)
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
		/* the weapon that suits the distance, if it has rounds (changed no
		more than every two seconds) */
		if (game_time_get() - bot->weapon_switch_time > 60 || game_time_get() < bot->weapon_switch_time)
		{
			short slot;
			short best_slot = NONE;
			real best = -1000.0f;

			for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[slot];
				real suits;

				if (weapon_index == NONE || !bot_weapon_has_ammunition(weapon_index))
					continue;
				suits = bot_weapon_suits(object_get(weapon_index)->definition_index, distance) +
					(slot == unit->unit.current_weapon_index ? 1.0f : 0.0f);
				if (suits > best)
				{
					best = suits;
					best_slot = slot;
				}
			}
			if (best_slot != NONE && best_slot != unit->unit.current_weapon_index)
			{
				action->desired_weapon_index = best_slot;
				bot->weapon_switch_time = game_time_get();
				bots.switches++;
			}
		}
		bot->weave_phase++;
		/* (a good weapon or powerup close by: taken in the fight too) */
		if (!bot->carrying && (game_time_get() - bot->item_look_time >= ITEM_LOOK_TICKS / 2 ||
			game_time_get() < bot->item_look_time))
		{
			bot->item_look_time = game_time_get();
			bot->item_index = bot_find_item(unit, &position, 10.0f, 4.0f);
		}
		if (bot->item_index != NONE && !bot->carrying && object_try_and_get(bot->item_index) &&
			bot_item_worth(unit, bot->item_index) > 0.0f)
		{
			real_point3d item_position;
			real_point3d point;

			object_get_origin(bot->item_index, &item_position);
			bot->goal = item_position;
			bot->has_goal = TRUE;
			if (bot_next_point(bot, player->unit_index, &position, &point))
				bot_walk_to(action, bot->yaw, &position, &point);
			if (distance3d(&position, &item_position) < 2.0f)
				bot_take_item(bot, player, unit, action);
		}
		/* closes on it along its way, weaving, until near */
		else if (bot->has_objective && bot->objective_unit_index != bot->target_unit_index)
		{
			/* (the objective first: it fights on the way) */
			real_point3d point;

			bot->goal = bot->objective;
			bot->has_goal = TRUE;
			if (bot_next_point(bot, player->unit_index, &position, &point))
			{
				bot_walk_to(action, bot->yaw, &position, &point);
				/* (carrying it: looking where it runs, unless struck at) */
				if (bot->carrying && distance > 1.5f)
				{
					real dx = point.x - position.x;
					real dy = point.y - position.y;

					if (dx * dx + dy * dy > 0.0001f)
						wanted_yaw = (real)atan2(dy, dx);
					wanted_pitch = 0.0f;
				}
			}
		}
		else if (distance > BOT_CLOSE_RANGE)
		{
			real_point3d point;

			object_get_origin(bot->target_unit_index, &bot->goal);
			bot->has_goal = TRUE;
			if (bot_next_point(bot, player->unit_index, &position, &point))
				bot_walk_to(action, bot->yaw, &position, &point);
		}
		action->throttle.j += (real)sin(bot->weave_phase * 0.08f) * 0.6f;
		firing = !bot->carrying && distance < BOT_FIRE_RANGE &&
			fabsf(bot_angle_difference(wanted_yaw, bot->yaw)) < BOT_FIRE_CONE &&
			fabsf(wanted_pitch - bot->pitch) < BOT_FIRE_CONE;
		/* (a teammate in the line of fire: held, and a step aside for a
		clear shot) */
		if (firing && bot_teammate_in_line(player, &eye, &head))
		{
			firing = FALSE;
			action->throttle.j = (bot->weave_phase / 30) & 1 ? 1.0f : -1.0f;
		}
		/* (a melee blow when it is upon it) */
		if (distance < 1.0f)
			SET_FLAG(action->control_flags, _unit_control_use_equipment_bit, TRUE);
		/* (a grenade now and then, lobbed at one some way off) */
		if (!bot->carrying && distance > BOT_GRENADE_MINIMUM && distance < BOT_GRENADE_MAXIMUM &&
			game_time_get() - bot->grenade_time > GRENADE_INTERVAL_TICKS && bot_random() < 0.02f)
		{
			short type;

			for (type = 0; type < NUMBER_OF_UNIT_GRENADE_TYPES; type++)
			{
				if (unit->unit.grenade_counts[type] > 0)
					break;
			}
			if (type < NUMBER_OF_UNIT_GRENADE_TYPES)
			{
				bot->grenade_time = game_time_get();
				bots.grenades++;
				action->desired_grenade_index = type;
				wanted_pitch += PIN(distance * 0.02f, 0.05f, 0.4f);
				SET_FLAG(action->control_flags, _unit_control_throw_grenade_bit, TRUE);
				SET_FLAG(action->control_flags, _unit_control_weapon_secondary_trigger_bit, TRUE);
				firing = FALSE;
			}
		}
	}
	else
	{
		real_point3d point;

		/* the objective: there (and the ball, taken there) */
		if (bot->has_objective)
		{
			bot->goal = bot->objective;
			bot->has_goal = TRUE;
			bot->goal_time = game_time_get();
			if (bots_game_type() == game_engine_oddball && bot->item_index != NONE &&
				distance3d(&position, &bot->goal) < 2.0f)
			{
				bot_take_item(bot, player, unit, action);
			}
		}
		/* something worth having near: there first */
		else if (game_time_get() - bot->item_look_time >= ITEM_LOOK_TICKS || game_time_get() < bot->item_look_time)
		{
			long item = bot_find_item(unit, &position, BOT_ITEM_RANGE, 0.0f);

			bot->item_look_time = game_time_get();
			if (item == bot->ignored_item_index && game_time_get() - bot->ignored_item_time < 600)
				item = NONE;
			if (item != bot->item_index)
			{
				bot->item_index = item;
				if (item != NONE)
				{
					object_get_origin(item, &bot->goal);
					bot->has_goal = TRUE;
					bot->goal_time = game_time_get();
					bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
				}
			}
		}
		if (bot->item_index != NONE && !bot->has_objective)
		{
			/* (taken or gone, or no way to it: on) */
			if (bot->path_failures >= 3)
			{
				bot->ignored_item_index = bot->item_index;
				bot->ignored_item_time = game_time_get();
				bot->path_failures = 0;
				bot->item_index = NONE;
				bot->has_goal = FALSE;
			}
			else if (!object_try_and_get(bot->item_index) || bot_item_worth(unit, bot->item_index) <= 0.0f)
			{
				bot->item_index = NONE;
				bot->has_goal = FALSE;
			}
			else
			{
				object_get_origin(bot->item_index, &bot->goal);
				bot->goal_time = game_time_get();
				if (distance3d(&position, &bot->goal) < 2.0f)
					bot_take_item(bot, player, unit, action);
			}
		}
		/* (in a team, every other bot with nothing to do keeps near the
		nearest person on its team) */
		following = FALSE;
		if (bot->item_index == NONE && !bot->has_objective && game_engine_has_teams() &&
			(DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index) & 1) == 0 &&
			game_time_get() >= bot->follow_again_time)
		{
			long person = bot_nearest_person(player, &position);

			if (person != NONE)
			{
				real_point3d origin;

				following = TRUE;
				object_get_origin(person, &origin);
				/* (near: where it is, not on top of them) */
				if (distance3d(&position, &origin) > 6.0f)
					bot_escort_point(&position, &origin, &bot->goal);
				else
					bot->goal = position;
				bot->has_goal = TRUE;
				bot->goal_time = game_time_get();
				/* (no way to them, as up where only a teleporter goes: not for
				ten seconds) */
				if (bot->path_failures >= 3)
				{
					bot->follow_again_time = game_time_get() + 300;
					bot->path_failures = 0;
					following = FALSE;
				}
			}
		}
		/* hunts, or roams: along its way, looking where it goes */
		if (bot->item_index == NONE && !bot->has_objective && !following &&
			(!bot->has_goal || game_time_get() - bot->goal_time > 90 || bot->path_failures >= 3 ||
				distance3d(&position, &bot->goal) < BOT_GOAL_RADIUS))
		{
			boolean unreachable = bot->path_failures >= 3;

			bot_choose_goal(bot, player, &position);
			/* (no way to the last: somewhere near at random instead) */
			if (unreachable)
			{
				real angle = bot_random() * 2.0f * _pi;

				bot->goal = position;
				bot->goal.x += (real)cos(angle) * 6.0f;
				bot->goal.y += (real)sin(angle) * 6.0f;
			}
			bot->path_failures = 0;
		}
		wanted_yaw = bot->wander_yaw;
		if (bot_next_point(bot, player->unit_index, &position, &point))
		{
			real dx = point.x - position.x;
			real dy = point.y - position.y;

			if (dx * dx + dy * dy > 0.0001f)
				bot->wander_yaw = wanted_yaw = (real)atan2(dy, dx);
			bot_walk_to(action, bot->yaw, &position, &point);
		}
		if (bot->carrying && !bot->has_objective &&
			(game_time_get() - bot->goal_time > 60 || distance3d(&position, &bot->goal) < BOT_GOAL_RADIUS))
		{
			/* (the ball: away, somewhere) */
			real angle = bot_random() * 2.0f * _pi;

			bot->goal = position;
			bot->goal.x += (real)cos(angle) * 8.0f;
			bot->goal.y += (real)sin(angle) * 8.0f;
			bot->has_goal = TRUE;
			bot->goal_time = game_time_get();
		}
		if (bot->stuck_ticks > 20)
		{
			/* (stuck: somewhere else) */
			bot_choose_goal(bot, player, &position);
			bot->goal.x += (bot_random() - 0.5f) * 6.0f;
			bot->goal.y += (bot_random() - 0.5f) * 6.0f;
			bot->stuck_ticks = 0;
		}
	}

	/* turned no faster than a player turns */
	{
		real turn = bot_angle_difference(wanted_yaw, bot->yaw);

		bot->yaw += PIN(turn, -bots.turn_rate, bots.turn_rate);
		bot->pitch += PIN(wanted_pitch - bot->pitch, -bots.turn_rate, bots.turn_rate);
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

	/* stuck: making no way to the point it walks to (against a wall, in
	a corner): soon a step aside and its way found anew, then a jump, and
	in the end somewhere else */
	if (bot->sidestep_ticks > 0)
	{
		bot->sidestep_ticks--;
		action->throttle.j = bot->sidestep;
		action->throttle.i *= 0.3f;
	}
	moved = distance3d(&position, &bot->last_position);
	bot->last_position = position;
	action->throttle.i = PIN(action->throttle.i, -1.0f, 1.0f);
	action->throttle.j = PIN(action->throttle.j, -1.0f, 1.0f);
	if (bot->walking && (fabsf(action->throttle.i) > 0.5f || fabsf(action->throttle.j) > 0.5f))
	{
		real distance = (real)sqrt((bot->walk_point.x - position.x) * (bot->walk_point.x - position.x) +
			(bot->walk_point.y - position.y) * (bot->walk_point.y - position.y));
		boolean progress = distance < bot->walk_distance - 0.015f || moved > 0.06f;

		bot->walk_distance = MIN(bot->walk_distance, distance);
		if (progress && moved > 0.02f)
		{
			bot->stuck_ticks = 0;
		}
		else
		{
			bot->stuck_ticks++;
			if (bot->stuck_ticks == 6)
			{
				bot->sidestep = bot_random() < 0.5f ? 1.0f : -1.0f;
				bot->sidestep_ticks = 10;
				/* (near a point of its path it cannot reach, at a corner: the
				next one) */
				if (distance < 1.5f && bot->path.valid && bot->path.step_index < bot->path.step_count)
				{
					bot->path.step_index++;
					bot->sidestep_ticks = 4;
				}
				bots.stucks++;

				bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
				bot->walk_distance = 1000000.0f;
			}
			if (bot->stuck_ticks == 12)
				SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
			if (bot->stuck_ticks == 24)
				bots.long_stucks++;
		}
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
		free_bot->item_index = NONE;
		free_bot->ignored_item_index = NONE;
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
	{
		long skill = PIN(config_integer("bots.skill"), 0, (long)NUMBEROF(bot_skills) - 1);

		bots.reaction_ticks = bot_skills[skill].reaction_ticks;
		bots.turn_rate = bot_skills[skill].turn_rate;
		bots.sight_range = bot_skills[skill].sight_range;
		bots.aim_error = bot_skills[skill].aim_error;
	}
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
	bots_update_objectives();
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
		platform_log("bots: driving %d; paths found %ld, not %ld; weapon swaps asked %ld; grenades %ld; changes %ld; stuck %ld, long %ld (no start %ld, no goal %ld, no way %ld)",
			(int)count, bots.paths_found, bots.paths_failed, bots.pickups, bots.grenades, bots.switches, bots.stucks, bots.long_stucks, bots.fail_start, bots.fail_goal, bots.fail_search);
		bots.fail_start = bots.fail_goal = bots.fail_search = 0;
		bots.paths_found = bots.paths_failed = bots.pickups = bots.grenades = bots.switches = bots.stucks = bots.long_stucks = 0;
		for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
		{
			struct player_datum *player = distributed_player(index);
			struct unit_datum *unit;
			short slot;
			char text[160] = "";

			if (!player || !network_game_server_is_bot_machine(player->network_player_data.machine_index) ||
				!(unit = unit_try_and_get(player->unit_index)))
			{
				continue;
			}
			for (slot = 0; slot < MAXIMUM_WEAPONS_PER_UNIT; slot++)
			{
				long weapon_index = unit->unit.weapon_object_indices[slot];
				char const *name = weapon_index != NONE ? tag_get_name(object_get(weapon_index)->definition_index) : NULL;
				char const *last = name ? strrchr(name, '\\') : NULL;

				if (name)
				{
					strncat(text, last ? last + 1 : name, sizeof(text) - strlen(text) - 3);
					strncat(text, slot == unit->unit.current_weapon_index ? "* " : " ", sizeof(text) - strlen(text) - 1);
				}
			}
			{
				struct bot *bot = bot_get(DATUM_INDEX_NEW(index, player->identifier));

				platform_log("bots:   player %d (team %ld) carries %s, grenades %d/%d%s%s", index, player->team_index, text,
					unit->unit.grenade_counts[0], unit->unit.grenade_counts[1],
					bot && bot->has_objective ? ", to the objective" : "", bot && bot->carrying ? ", CARRYING" : "");
				if (bot && bot->carrying && bot->has_objective)
				{
					real_point3d origin;

					object_get_origin(player->unit_index, &origin);
					platform_log("bots:     carrier at (%.1f %.1f %.1f), goal (%.1f %.1f %.1f), %.2f off; path %s",
						origin.x, origin.y, origin.z, bot->objective.x, bot->objective.y, bot->objective.z,
						distance3d(&origin, &bot->objective), bot->path.valid ? "found" : "none");
				}
			}
		}
	}
}
