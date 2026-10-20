/*
HOST_BOTS.C

(was bots.c, which OpenCE's local bots now are: those of a local split
screen game, bots.c, not in VR's network lobbies)

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

Race: a bot heads for the flag it is to touch next, in a vehicle if it can
have one.

Vehicles: a bot whose way is long takes an empty vehicle it can drive (a
Warthog, a Ghost, a Scorpion) that stands near, walking to the driver's
door and stopping there until the game offers the seat. Driving, it follows
its way as a vehicle's (wider, its steps further apart), turning a Warthog or
a Ghost to where it goes by facing there, pushing a Scorpion there from where
its gun faces; it runs an enemy down, or with a gunner aboard circles it,
fires a Ghost's or a Scorpion's guns at it, backs off what it is stuck on,
and gets out near the objective, when the vehicle is overturned, or when it
keeps getting stuck. A bot mans a turret near it while an enemy is about,
and leaves it once there has been no one to shoot for a while. In a game
of teams a bot also mans the gun of a
teammate's vehicle, or rides along in a person's: there it aims and fires
the gun, and gets out once no one drives.

In a game of teams a bot plays with its team: it holds its fire while a
teammate is in the line of it, takes on first the enemies near its
teammates (people above all), escorts a teammate who carries the enemy's
flag or the ball, and, with nothing to do, every third bot keeps near the
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
#include "units/unit_definitions.h"
#include "units/vehicles.h"
#include "game/game_engine_race.h"
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

/* (how many things to pick up it gives up at once, and for how long) */
#define BOT_IGNORED_ITEMS 4
#define BOT_IGNORE_ITEM_TICKS 600

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
	/* things to pick up given up for a while (no way to them, or there and
	not taken: on a ledge, out of reach), and how long it has stood by the
	one it goes for */
	long ignored_items[BOT_IGNORED_ITEMS];
	long ignored_item_times[BOT_IGNORED_ITEMS];
	short ignored_item_next;
	/* (the nearest it has come to the thing it goes for, and when) */
	long item_progress_index;
	real item_best_distance;
	long item_best_time;
	/* stuck again and again about the same place (as at the foot of a ramp
	or behind its rail): how often since when, to give the goal up */
	short stuck_count;
	long stuck_count_time;
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
	/* the objective is a place to hold (the hill, its own flag's stand) */
	/* (the race's flag it last headed for) */
	real_point3d race_flag;
	/* (how near a step of its path counts as reached: a vehicle's further) */
	real step_radius;
	/* a vehicle: the one it goes to get into and which seat, since when,
	one given up for a while, and none until a while after getting out */
	long vehicle_index;
	short vehicle_seat;
	long vehicle_time;
	long vehicle_look_time;
	long ignored_vehicle_index;
	long ignored_vehicle_time;
	long vehicle_again_time;
	/* riding: since when no one drives; driving: stuck, backing off, how
	often in a row, and where the vehicle was */
	boolean riding;
	long no_driver_time;
	short drive_stuck_ticks;
	short reverse_ticks;
	short drive_stucks;
	short drive_moving_ticks;
	short drive_slow_ticks;
	short flipped_ticks;
	/* (at a turret: when it last had someone to shoot) */
	long turret_target_time;
	/* (driving: when it got in, to wait a moment for those coming along) */
	long seated_time;
	boolean leave_logged;
	real_point3d drive_last;
	boolean holding;
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
	boolean vehicles_logged;
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
	long gave_up;
	long fail_start;
	long fail_goal;
	long fail_search;
	long vehicle_entries;
	long vehicle_exits;
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
	/* (driving: from the vehicle, and wide enough for it) */
	boolean vehicle = TEST_FLAG(_object_mask_vehicle, object_get(unit_index)->object.type);
	long start_surface = vehicle ? vehicle_find_pathfinding_surface_index(unit_index, &start) :
		biped_find_pathfinding_surface_index(unit_index, &start);
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
	path_input_new(&input, vehicle ? 0.6f : 0.25f, FALSE, unit_index);
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
	real_point3d const *to,
	boolean wide)
{
	struct collision_result collision;
	real_vector3d vector;
	real length;
	short line;

	vector.i = to->x - from->x;
	vector.j = to->y - from->y;
	vector.k = to->z - from->z;
	length = (real)sqrt(vector.i * vector.i + vector.j * vector.j);
	/* (at its knees and at its chest: a rail with room under it, a slit in
	a wall, are not ways through; wide, along its shoulders too: a door it
	would pass the middle of but catch its frame) */
	for (line = 0; line < (wide && length > 0.001f ? 4 : 2); line++)
	{
		real_point3d start = *from;

		start.z += line == 1 ? 1.0f : line ? 0.6f : 0.3f;
		if (line >= 2)
		{
			real side = line == 2 ? 0.35f : -0.35f;

			start.x += -vector.j / length * side;
			start.y += vector.i / length * side;
		}
		if (collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
			&start, &vector, unit_index, &collision))
		{
			return FALSE;
		}
	}
	return TRUE;
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
			bot_clear_walk(unit_index, position, &bot->goal, FALSE))
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

			if (dx * dx + dy * dy > bot->step_radius * bot->step_radius)
			{
				/* (the next point in plain view: on to it, not round by this
				one, a corner's) */
				real_point3d const *next = bot->path.step_index + 1 < bot->path.step_count ?
					&bot->path.steps[(short)bot->path.step_index + 1].point : &bot->path.endpoint.point;

				/* (on its own level only: one above may be in view over a
				ledge that a ramp leads round to) */
				if (fabsf(next->z - position->z) < 0.6f && bot_clear_walk(unit_index, position, next, TRUE))
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

/* a goal when none is in sight: an enemy, or somewhere near someone at
random. Not all the nearest: each bot one of the three nearest (by its
number), and while it is far, a few metres beside them, so that a team
spreads out over the ways there instead of going as one crowd */
static void bot_choose_goal(
	struct bot *bot,
	struct player_datum const *player,
	real_point3d const *position)
{
	boolean teams = game_engine_has_teams();
	real best_distance[3] = { 1000000.0f, 1000000.0f, 1000000.0f };
	real_point3d best_origin[3];
	short found = 0;
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *other = distributed_player(index);
		long unit_index;
		real_point3d origin;
		real distance;
		short rank;

		if (!other || other == player || (teams && other->team_index == player->team_index))
			continue;
		unit_index = distributed_living_unit(other);
		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		distance = distance3d(position, &origin);
		/* (kept in order, nearest first) */
		for (rank = 0; rank < 3 && distance >= best_distance[rank]; rank++)
			;
		if (rank == 3)
			continue;
		found = (short)MIN(found + 1, 3);
		{
			short move;

			for (move = 2; move > rank; move--)
			{
				best_distance[move] = best_distance[move - 1];
				best_origin[move] = best_origin[move - 1];
			}
		}
		best_distance[rank] = distance;
		best_origin[rank] = origin;
	}
	if (found)
	{
		short pick = (short)(DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index) % found);

		bot->goal = best_origin[pick];
		if (best_distance[pick] > 25.0f)
		{
			real angle = (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index) * 2.4f;

			bot->goal.x += (real)cos(angle) * 6.0f;
			bot->goal.y += (real)sin(angle) * 6.0f;
		}
	}
	else
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
static boolean bot_item_ignored(
	struct bot const *bot,
	long item_index)
{
	short index;

	for (index = 0; index < BOT_IGNORED_ITEMS; index++)
	{
		if (bot->ignored_items[index] == item_index &&
			game_time_get() - bot->ignored_item_times[index] < BOT_IGNORE_ITEM_TICKS &&
			game_time_get() >= bot->ignored_item_times[index])
		{
			return TRUE;
		}
	}
	return FALSE;
}

static void bot_ignore_item(
	struct bot *bot,
	long item_index)
{
	bot->ignored_items[bot->ignored_item_next] = item_index;
	bot->ignored_item_times[bot->ignored_item_next] = game_time_get();
	bot->ignored_item_next = (short)((bot->ignored_item_next + 1) % BOT_IGNORED_ITEMS);
	bot->item_progress_index = NONE;
}

/* going for a thing to pick up: whether to give it up, coming no nearer
(half a metre) for two and a half seconds: out of reach, past a rail, up
where no way it finds goes, or in a crowd */
static boolean bot_item_out_of_reach(
	struct bot *bot,
	real distance)
{
	if (bot->item_progress_index != bot->item_index || game_time_get() < bot->item_best_time)
	{
		bot->item_progress_index = bot->item_index;
		bot->item_best_distance = distance;
		bot->item_best_time = game_time_get();
		return FALSE;
	}
	if (distance < bot->item_best_distance - 0.5f)
	{
		bot->item_best_distance = distance;
		bot->item_best_time = game_time_get();
		return FALSE;
	}
	return game_time_get() - bot->item_best_time > 75;
}

static long bot_find_item(
	struct bot const *bot,
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
		if (worth <= 0.0f || worth < least_worth || bot_item_ignored(bot, iterator.index))
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

/* Bots with one place to go spread out: each has a direction of its own
(the golden angle on from the last bot's), a place of its own about a
point to hold, and comes to a point from far off by a way of its own,
wide of the others', going straight to it only for the last stretch. */

static real bot_spread_angle(
	long player_index)
{
	return (real)DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) * 2.39996f;
}

/* the bot's place about a point (radius: world units) */
static void bot_spot_around(
	real_point3d const *center,
	long player_index,
	real radius,
	real_point3d *spot)
{
	real angle = bot_spread_angle(player_index);

	*spot = *center;
	spot->x += (real)cos(angle) * radius;
	spot->y += (real)sin(angle) * radius;
}

/* its place about a point, or the point itself when there is no way to the
place (in a wall, past a ledge) */
static void bot_hold_spot(
	struct bot const *bot,
	real_point3d const *center,
	real radius,
	real_point3d *spot)
{
	if (bot->path_failures >= 3)
		*spot = *center;
	else
		bot_spot_around(center, bot->player_index, radius, spot);
}

/* where it heads for a point from where it is: from far off, out to a side
of the straight way (its own side and width), in from 10 world units */
static void bot_approach(
	struct bot const *bot,
	real_point3d const *position,
	real_point3d const *goal,
	real_point3d *point)
{
	long index = DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index);
	real dx = goal->x - position->x;
	real dy = goal->y - position->y;
	real distance = (real)sqrt(dx * dx + dy * dy);
	real side = (index & 1 ? 1.0f : -1.0f) * (3.0f + (real)((index >> 1) % 3) * 1.5f);
	real share;

	*point = *goal;
	/* (no way to the side: straight) */
	if (distance < 10.0f || bot->path_failures >= 2)
		return;
	share = MIN((distance - 10.0f) / 10.0f, 1.0f);
	point->x += -dy / distance * side * share;
	point->y += dx / distance * side * share;
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
	bot->holding = FALSE;
	bot->objective_unit_index = NONE;
	bot->carrying = FALSE;
	if (type == game_engine_race)
	{
		bot->has_objective = game_engine_race_next_flag(player_index, position, &bot->objective);
		if (bot->has_objective && distance3d(&bot->race_flag, &bot->objective) > 1.0f)
		{
			bot->race_flag = bot->objective;
			platform_log("bots: player %ld races to the flag at (%.1f %.1f %.1f)%s",
				DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), bot->objective.x, bot->objective.y, bot->objective.z,
				player_get(player_index)->unit_index != NONE &&
					object_get(player_get(player_index)->unit_index)->object.parent_object_index != NONE ? ", driving" : "");
		}
		return;
	}
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
			bot->holding = TRUE;
			/* (near: a place of its own on the hill; far: a way of its own) */
			if (dx * dx + dy * dy < 6.0f * 6.0f)
				bot_hold_spot(bot, &center, 1.2f, &bot->objective);
			else
				bot_approach(bot, position, &center, &bot->objective);
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
		/* (every third guards its own: about the stand, or, every other one
		of them, ahead of it towards the enemy's) */
		if ((DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) % 3) == 2 && bots_ctf_base(player->team_index, &base))
		{
			real_point3d enemy_base;

			bot->has_objective = TRUE;
			bot->holding = TRUE;
			if (((DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index) / 3) & 1) &&
				bots_ctf_base((player->team_index + 1) % 2, &enemy_base))
			{
				real dx = enemy_base.x - base.x;
				real dy = enemy_base.y - base.y;
				real length = (real)sqrt(dx * dx + dy * dy);

				bot->objective = base;
				if (length > 0.01f)
				{
					bot->objective.x += dx / length * 7.0f;
					bot->objective.y += dy / length * 7.0f;
				}
			}
			else
			{
				bot_hold_spot(bot, &base, 3.0f, &bot->objective);
			}
			return;
		}
		/* theirs: to take it, or to escort the teammate carrying it */
		if (enemy)
		{
			bot->has_objective = TRUE;
			if (enemy->carrier_player_index != NONE)
				bot_hold_spot(bot, &enemy->position, 3.0f, &bot->objective);
			else
				bot_approach(bot, position, &enemy->position, &bot->objective);
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
						bot_hold_spot(bot, &item->position, 3.0f, &bot->objective);
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
			bot_approach(bot, position, &nearest->position, &bot->objective);
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

/* ---------- vehicles */

static void bot_walk_to(
	struct player_action *action,
	real yaw,
	real_point3d const *position,
	real_point3d const *point);

enum
{
	_bot_seat_driver,
	_bot_seat_gunner,
	_bot_seat_passenger,
	/* (unit_definitions.h's seat flags past the driver's and gunner's) */
	_bot_seat_requires_driver_bit = 9,
	/* how long it goes for a vehicle before giving up (ticks), and how long
	after getting out until it takes one again */
	BOT_VEHICLE_TIMEOUT_TICKS = 360,
	/* (bot->vehicle_seat: not a seat, the vehicle to turn upright) */
	BOT_FLIP_VEHICLE = -2,
	BOT_VEHICLE_AGAIN_TICKS = 150,
};

/* how near a vehicle it takes one (world units): to drive, to man a gun or
ride with a person, with a bot */
#define BOT_VEHICLE_RANGE 20.0f
#define BOT_RACE_VEHICLE_RANGE 40.0f
#define BOT_PERSON_VEHICLE_RANGE 25.0f
#define BOT_BOT_VEHICLE_RANGE 12.0f
/* how near a turret it mans one, and how near it an enemy must be */
#define BOT_TURRET_RANGE 15.0f
#define BOT_TURRET_ENEMY_RANGE 35.0f
/* how near a step of a vehicle's path counts as reached (world units) */
#define BOT_DRIVE_STEP_RADIUS 2.0f

static struct unit_seat *bot_vehicle_seat(
	long vehicle_index,
	short seat_index)
{
	struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);

	if (seat_index < 0 || seat_index >= definition->unit.seats.count)
		return NULL;
	return TAG_BLOCK_GET_ELEMENT(&definition->unit.seats, seat_index, struct unit_seat);
}

static short bot_seat_kind(
	struct unit_seat const *seat)
{
	if (TEST_FLAG(seat->flags, _unit_seat_driver_bit))
		return _bot_seat_driver;
	if (TEST_FLAG(seat->flags, _unit_seat_gunner_bit))
		return _bot_seat_gunner;
	return _bot_seat_passenger;
}

/* whether a vehicle stands to be got into (whole, upright) */
static boolean bot_vehicle_usable(
	long vehicle_index)
{
	struct unit_datum *vehicle = unit_try_and_get(vehicle_index);

	return vehicle && TEST_FLAG(_object_mask_vehicle, vehicle->object.type) &&
		!TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) && !vehicle_is_flipped(vehicle_index);
}

/* whether the bot may take the seat now: free, and a passenger's only with
a driver */
static boolean bot_seat_free(
	long unit_index,
	long vehicle_index,
	short seat_index)
{
	struct unit_seat *seat = bot_vehicle_seat(vehicle_index, seat_index);
	long occupant_index = NONE;

	if (!seat || !bot_vehicle_usable(vehicle_index))
		return FALSE;
	if (TEST_FLAG(seat->flags, _bot_seat_requires_driver_bit) &&
		unit_get(vehicle_index)->unit.driver_object_index == NONE)
	{
		return FALSE;
	}
	return unit_can_enter_seat(unit_index, vehicle_index, seat_index, &occupant_index);
}

/* a free seat of the kind (NONE: none), not one another bot goes for */
static short bot_free_seat(
	struct bot const *bot,
	long unit_index,
	long vehicle_index,
	short kind)
{
	struct unit_definition *definition = unit_definition_get(object_get(vehicle_index)->definition_index);
	short seat_index;

	for (seat_index = 0; seat_index < definition->unit.seats.count; seat_index++)
	{
		short index;
		boolean taken = FALSE;

		if (bot_seat_kind(bot_vehicle_seat(vehicle_index, seat_index)) != kind ||
			!bot_seat_free(unit_index, vehicle_index, seat_index))
		{
			continue;
		}
		for (index = 0; index < MAXIMUM_BOTS; index++)
		{
			struct bot const *other = &bots.bots[index];

			if (other != bot && other->player_index != NONE && other->vehicle_index == vehicle_index &&
				other->vehicle_seat == seat_index)
			{
				taken = TRUE;
			}
		}
		if (!taken)
			return seat_index;
	}
	return NONE;
}

/* whether an enemy is alive within range of a point */
static boolean bot_enemy_near(
	struct player_datum const *player,
	real_point3d const *point,
	real range)
{
	boolean teams = game_engine_has_teams();
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *other = distributed_player(index);
		long unit_index;
		real_point3d origin;

		if (!other || other == player || (teams && other->team_index == player->team_index))
			continue;
		unit_index = distributed_living_unit(other);
		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		if (distance3d(point, &origin) < range)
			return TRUE;
	}
	return FALSE;
}

/* how far off the nearest enemy alive is (a long way: none) */
static real bot_nearest_enemy_distance(
	struct player_datum const *player,
	real_point3d const *point)
{
	boolean teams = game_engine_has_teams();
	real nearest = 1000000.0f;
	short index;

	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *other = distributed_player(index);
		long unit_index;
		real_point3d origin;

		if (!other || other == player || (teams && other->team_index == player->team_index))
			continue;
		unit_index = distributed_living_unit(other);
		if (unit_index == NONE)
			continue;
		object_get_origin(unit_index, &origin);
		nearest = MIN(nearest, distance3d(point, &origin));
	}
	return nearest;
}

/* whether a teammate is coming to get in: a bot going for the vehicle, or
a person on foot near it while a gun's seat is free */
static boolean bot_riders_coming(
	struct bot const *bot,
	struct player_datum const *player,
	long vehicle_index)
{
	real_point3d origin;
	short index;

	for (index = 0; index < MAXIMUM_BOTS; index++)
	{
		struct bot const *other = &bots.bots[index];

		if (other != bot && other->player_index != NONE && other->vehicle_index == vehicle_index)
			return TRUE;
	}
	if (!game_engine_has_teams() || bot_free_seat(bot, player->unit_index, vehicle_index, _bot_seat_gunner) == NONE)
		return FALSE;
	object_get_origin(vehicle_index, &origin);
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *mate = distributed_player(index);
		long unit_index;
		real_point3d mate_origin;

		if (!mate || mate == player || mate->team_index != player->team_index || !bot_player_is_person(mate))
			continue;
		unit_index = distributed_living_unit(mate);
		if (unit_index == NONE || object_get(unit_index)->object.parent_object_index != NONE)
			continue;
		object_get_origin(unit_index, &mate_origin);
		if (distance3d(&origin, &mate_origin) < 10.0f)
			return TRUE;
	}
	return FALSE;
}

/* whether a teammate on foot stands in the way the vehicle goes (ahead of
it, or behind it backing), near; and on which side of the way (+1 left) */
static boolean bot_teammate_in_the_way(
	struct player_datum const *player,
	real_point3d const *position,
	real heading,
	boolean backing,
	real *side)
{
	short index;

	if (!game_engine_has_teams())
		return FALSE;
	if (backing)
		heading += _pi;
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
	{
		struct player_datum *mate = distributed_player(index);
		long unit_index;
		real_point3d origin;

		if (!mate || mate == player || mate->team_index != player->team_index)
			continue;
		unit_index = distributed_living_unit(mate);
		if (unit_index == NONE || object_get(unit_index)->object.parent_object_index != NONE)
			continue;
		object_get_origin(unit_index, &origin);
		{
			real off = bot_angle_difference((real)atan2(origin.y - position->y, origin.x - position->x), heading);

			if (distance3d(position, &origin) < 8.0f && fabsf(off) < 0.7f)
			{
				*side = off >= 0.0f ? 1.0f : -1.0f;
				return TRUE;
			}
		}
	}
	return FALSE;
}

/* now and then, on foot: a vehicle to get into (bot->vehicle_index): a
teammate's to man its gun (or a person's to ride along in), or, when the way
is long, an empty one to drive */
static void bot_choose_vehicle(
	struct bot *bot,
	struct player_datum const *player,
	real_point3d const *position,
	boolean wants_to_drive)
{
	struct object_iterator iterator;
	boolean teams = game_engine_has_teams();
	boolean race = bots_game_type() == game_engine_race;
	real best_score = 0.0f;

	bot->vehicle_look_time = game_time_get();
	object_iterator_new(&iterator, _object_mask_vehicle, 0);
	while (object_iterator_next(&iterator))
	{
		struct unit_datum *vehicle = unit_get(iterator.index);
		real_point3d origin;
		real distance;
		short seat_index = NONE;
		real score = 0.0f;

		if (iterator.index == bot->ignored_vehicle_index && game_time_get() - bot->ignored_vehicle_time < 600)
			continue;
		object_get_origin(iterator.index, &origin);
		distance = distance3d(position, &origin);
		if (!bot_vehicle_usable(iterator.index))
		{
			/* (overturned, empty, one it would drive: turned upright first; but
			in a race, only one close by) */
			if (wants_to_drive && !TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) &&
				vehicle_is_flipped(iterator.index) && vehicle->unit.driver_object_index == NONE &&
				vehicle_bot_drive_kind(iterator.index) != _vehicle_bot_drive_none &&
				distance < (race ? BOT_RACE_VEHICLE_RANGE : 10.0f) && 0.8f / (1.0f + distance / 10.0f) > best_score)
			{
				short index;
				boolean taken = FALSE;

				for (index = 0; index < MAXIMUM_BOTS; index++)
				{
					if (&bots.bots[index] != bot && bots.bots[index].player_index != NONE &&
						bots.bots[index].vehicle_index == iterator.index)
					{
						taken = TRUE;
					}
				}
				if (!taken)
				{
					best_score = 0.8f / (1.0f + distance / 10.0f);
					bot->vehicle_index = iterator.index;
					bot->vehicle_seat = BOT_FLIP_VEHICLE;
				}
			}
			continue;
		}
		if (vehicle->unit.driver_object_index != NONE)
		{
			struct player_datum *driver_player = NULL;
			boolean person;
			short index;

			for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
			{
				struct player_datum *other = distributed_player(index);

				if (other && other->unit_index == vehicle->unit.driver_object_index)
					driver_player = other;
			}

			/* (a teammate's: its gun, or with a person, a seat) */
			if (!teams || !driver_player || driver_player == player || driver_player->team_index != player->team_index)
				continue;
			person = bot_player_is_person(driver_player);
			if (distance > (person ? BOT_PERSON_VEHICLE_RANGE : BOT_BOT_VEHICLE_RANGE))
				continue;
			seat_index = bot_free_seat(bot, player->unit_index, iterator.index, _bot_seat_gunner);
			if (seat_index == NONE && person)
				seat_index = bot_free_seat(bot, player->unit_index, iterator.index, _bot_seat_passenger);
			score = (person ? 3.0f : 1.5f) / (1.0f + distance / 10.0f);
		}
		else if (vehicle_bot_drive_kind(iterator.index) == _vehicle_bot_drive_turret)
		{
			/* (a turret, while an enemy is about it; not in a race) */
			if (!race && distance < BOT_TURRET_RANGE && bot_enemy_near(player, &origin, BOT_TURRET_ENEMY_RANGE))
			{
				seat_index = bot_free_seat(bot, player->unit_index, iterator.index, _bot_seat_driver);
				if (seat_index == NONE)
					seat_index = bot_free_seat(bot, player->unit_index, iterator.index, _bot_seat_gunner);
				score = 1.2f / (1.0f + distance / 10.0f);
			}
		}
		else if (wants_to_drive && vehicle_bot_drive_kind(iterator.index) != _vehicle_bot_drive_none &&
			distance < (race ? BOT_RACE_VEHICLE_RANGE : BOT_VEHICLE_RANGE))
		{
			seat_index = bot_free_seat(bot, player->unit_index, iterator.index, _bot_seat_driver);
			score = 1.0f / (1.0f + distance / 10.0f);
		}
		if (seat_index != NONE && score > best_score)
		{
			best_score = score;
			bot->vehicle_index = iterator.index;
			bot->vehicle_seat = seat_index;
		}
	}
	if (bot->vehicle_index != NONE)
	{
		bot->leave_logged = FALSE;
		if (bot->vehicle_seat == BOT_FLIP_VEHICLE)
			platform_log("bots: player %ld goes to turn a vehicle upright", DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index));
		bot->vehicle_time = game_time_get();
		bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
		bot->path_failures = 0;
	}
}

static void bot_give_up_vehicle(
	struct bot *bot)
{
	bot->ignored_vehicle_index = bot->vehicle_index;
	bot->ignored_vehicle_time = game_time_get();
	bot->vehicle_index = NONE;
	bot->path_failures = 0;
}

/* on foot, going for a vehicle: where to get in (FALSE: not going for one,
or given it up: gone, taken, driven off, no way there, too long) */
static boolean bot_vehicle_entrance(
	struct bot *bot,
	struct player_datum const *player,
	real_point3d const *position,
	real_point3d *entrance)
{
	real_point3d seat_point;

	if (bot->vehicle_index == NONE)
		return FALSE;
	if (bot->vehicle_seat == BOT_FLIP_VEHICLE)
	{
		struct unit_datum *vehicle = unit_try_and_get(bot->vehicle_index);

		/* (upright again, or too long: given up, to be chosen anew) */
		if (!vehicle || TEST_FLAG(vehicle->object.damage_flags, _object_dead_bit) ||
			!vehicle_is_flipped(bot->vehicle_index) || vehicle->unit.driver_object_index != NONE ||
			game_time_get() - bot->vehicle_time > BOT_VEHICLE_TIMEOUT_TICKS || game_time_get() < bot->vehicle_time)
		{
			if (vehicle && !vehicle_is_flipped(bot->vehicle_index))
			{
				bot->vehicle_index = NONE;
				bot->vehicle_look_time = game_time_get() - ITEM_LOOK_TICKS;
			}
			else
			{
				bot_give_up_vehicle(bot);
			}
			return FALSE;
		}
		object_get_origin(bot->vehicle_index, entrance);
		return TRUE;
	}
	if (!bot_seat_free(player->unit_index, bot->vehicle_index, bot->vehicle_seat) ||
		game_time_get() - bot->vehicle_time > BOT_VEHICLE_TIMEOUT_TICKS || game_time_get() < bot->vehicle_time ||
		bot->path_failures >= 3 ||
		!unit_get_seat_entrance_point(player->unit_index, bot->vehicle_index, bot->vehicle_seat, entrance,
			&seat_point, NULL) ||
		distance3d(position, entrance) > BOT_PERSON_VEHICLE_RANGE + 10.0f)
	{
		bot_give_up_vehicle(bot);
		return FALSE;
	}
	return TRUE;
}

/* at the door: standing still (the game offers a seat only to one who
does), and in as it offers the seat */
static void bot_get_in(
	struct bot *bot,
	struct player_datum const *player,
	real_point3d const *position,
	struct player_action *action)
{
	real_point3d origin;

	action->throttle.i = 0.0f;
	action->throttle.j = 0.0f;
	bot->walking = FALSE;
	object_get_origin(bot->vehicle_index, &origin);
	/* (the seat it came for, or one like it; or the vehicle turned over) */
	if (player->action_object_index == bot->vehicle_index &&
		(bot->vehicle_seat == BOT_FLIP_VEHICLE ?
			player->action_result == _player_action_result_flip_vehicle :
			player->action_result == _player_action_result_enter_vehicle &&
				bot_vehicle_seat(bot->vehicle_index, player->action_seat_index) &&
				bot_seat_kind(bot_vehicle_seat(bot->vehicle_index, player->action_seat_index)) ==
					bot_seat_kind(bot_vehicle_seat(bot->vehicle_index, bot->vehicle_seat))))
	{
		SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
		if (player->action_result == _player_action_result_flip_vehicle && !bot->leave_logged)
		{
			platform_log("bots: player %ld turns a vehicle upright", DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index));
			bot->leave_logged = TRUE;
		}
	}
	(void)position;
}

/* a place a long way off to drive to: one of the map's spawn points (a
point at random may be off the map, or in a cliff) */
static void bot_far_point(
	real_point3d const *position,
	real_point3d *point)
{
	struct scenario *scenario = global_scenario_get();
	short attempt;

	if (scenario && scenario->players.count > 0)
	{
		for (attempt = 0; attempt < 8; attempt++)
		{
			struct player_starting_location *location = TAG_BLOCK_GET_ELEMENT(&scenario->players,
				(long)(bot_random() * (real)scenario->players.count) % scenario->players.count,
				struct player_starting_location);

			*point = location->position;
			if (distance3d(position, point) > 20.0f)
				return;
		}
		return;
	}
	{
		real angle = bot_random() * 2.0f * _pi;

		*point = *position;
		point->x += (real)cos(angle) * 30.0f;
		point->y += (real)sin(angle) * 30.0f;
	}
}

/* how far a vehicle runs along a yaw before the level stops it (world
units, up to length): a ray at about its bumper's height */
static real bot_drive_clearance(
	long vehicle_index,
	real_point3d const *position,
	real yaw,
	real length)
{
	struct collision_result collision;
	real_point3d start = *position;
	real_vector3d vector;

	start.z += 0.5f;
	vector.i = (real)cos(yaw) * length;
	vector.j = (real)sin(yaw) * length;
	vector.k = 0.0f;
	/* (ground rising ahead, not too steep to drive up, is no wall) */
	if (!collision_test_vector(FLAG(_collision_test_structure_bit) | FLAG(_collision_test_front_facing_surfaces_bit),
		&start, &vector, vehicle_index, &collision) || collision.plane.n.k > 0.6f)
	{
		return length;
	}
	return distance3d(&start, &collision.point);
}

/* the yaw from one point to another */
static real bot_yaw_to(
	real_point3d const *from,
	real_point3d const *to)
{
	return (real)atan2(to->y - from->y, to->x - from->x);
}

/* seated in a vehicle: driving it, manning its gun, or riding along */
static void bot_ride(
	struct bot *bot,
	struct player_datum *player,
	struct unit_datum *unit,
	struct player_action *action)
{
	long vehicle_index = unit->object.parent_object_index;
	struct unit_datum *vehicle = TEST_FLAG(_object_mask_vehicle, object_get(vehicle_index)->object.type) ?
		unit_get(vehicle_index) : NULL;
	struct unit_seat *seat = vehicle ? bot_vehicle_seat(vehicle_index, unit->unit.parent_seat_index) : NULL;
	short kind = seat ? bot_seat_kind(seat) : _bot_seat_passenger;
	/* (at a turret: as a gunner, whatever its seat) */
	boolean turret = vehicle && vehicle_bot_drive_kind(vehicle_index) == _vehicle_bot_drive_turret;
	real_point3d eye;
	real_point3d position;
	real_vector3d forward;
	real_vector3d up;
	real heading;
	real wanted_yaw;
	real wanted_pitch = 0.0f;
	boolean firing = FALSE;
	boolean leave = FALSE;
	char const *why = "";
	boolean turn_freely = FALSE;
	real target_distance = 0.0f;
	real_point3d target_head;

	bot->vehicle_index = NONE;
	if (!bot->riding)
	{
		bot->riding = TRUE;
		bot->no_driver_time = NONE;
		bot->drive_stuck_ticks = 0;
		bot->reverse_ticks = 0;
		bot->drive_stucks = 0;
		bot->drive_moving_ticks = 0;
		bot->drive_slow_ticks = 0;
		bot->turret_target_time = game_time_get();
		bot->seated_time = game_time_get();
		bot->leave_logged = FALSE;
		bot->has_goal = FALSE;
		bot->path.valid = FALSE;
		bots.vehicle_entries++;
		platform_log("bots: player %ld got into a vehicle (seat %d, %s)", DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index),
			(int)unit->unit.parent_seat_index, kind == _bot_seat_driver ? "driving" : kind == _bot_seat_gunner ? "at the gun" : "riding");
	}
	unit_get_camera_position(player->unit_index, &eye);
	object_get_origin(vehicle_index, &position);
	object_get_orientation(vehicle_index, &forward, &up);
	heading = (real)atan2(forward.j, forward.i);
	wanted_yaw = bot->yaw;

	bot_play_objective(bot, player, bot->player_index, &position);

	/* the enemy, as on foot (a gun sees further) */
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
	if (--bot->error_ticks <= 0)
	{
		bot->error_ticks = 10 + (short)(bot_random() * 20.0f);
		bot->aim_error_yaw = (bot_random() - 0.5f) * 0.06f * bots.aim_error;
		bot->aim_error_pitch = (bot_random() - 0.5f) * 0.04f * bots.aim_error;
	}
	if (bot->target_unit_index != NONE && bot->target_seen_ticks >= bots.reaction_ticks)
	{
		unit_get_camera_position(bot->target_unit_index, &target_head);
		target_head.z -= 0.15f;
		target_distance = distance3d(&eye, &target_head);
	}
	else
	{
		bot->target_unit_index = NONE;
	}

	/* (overturned a moment, not only tipped as it lands) */
	bot->flipped_ticks = vehicle && vehicle_is_flipped(vehicle_index) ? bot->flipped_ticks + 1 : 0;
	if (!vehicle || bot->flipped_ticks > 20)
	{
		leave = TRUE;
		why = "overturned";
	}
	else if (vehicle->unit.driver_object_index == NONE)
	{
		if (bot->no_driver_time == NONE)
			bot->no_driver_time = game_time_get();
	}
	else
	{
		bot->no_driver_time = NONE;
	}

	if (turret)
	{
		kind = _bot_seat_gunner;
		if (bot->target_unit_index != NONE)
			bot->turret_target_time = game_time_get();
		else if (game_time_get() - bot->turret_target_time > 300 || game_time_get() < bot->turret_target_time)
		{
			leave = TRUE;
			why = "no one to shoot";
		}
	}
	if (!leave && kind != _bot_seat_driver)
	{
		/* (a turret's gunner drives it: no driver to wait for) */
		long without = bot->no_driver_time == NONE || turret ? 0 : game_time_get() - bot->no_driver_time;

		/* no one drives: out, a gunner once there is no one to shoot */
		if ((kind == _bot_seat_passenger && without > 30) ||
			(kind == _bot_seat_gunner && without > 60 && bot->target_unit_index == NONE))
		{
			leave = TRUE;
			why = "no driver";
		}
		if (bot->target_unit_index != NONE && kind == _bot_seat_gunner)
		{
			real dx = target_head.x - eye.x;
			real dy = target_head.y - eye.y;

			wanted_yaw = (real)atan2(dy, dx) + bot->aim_error_yaw;
			wanted_pitch = (real)atan2(target_head.z - eye.z, (real)sqrt(dx * dx + dy * dy)) + bot->aim_error_pitch;
			firing = target_distance < bots.sight_range * 1.5f &&
				fabsf(bot_angle_difference(wanted_yaw, bot->yaw)) < BOT_FIRE_CONE * 1.5f &&
				fabsf(wanted_pitch - bot->pitch) < BOT_FIRE_CONE * 1.5f &&
				!bot_teammate_in_line(player, &eye, &target_head);
		}
		else
		{
			/* (looking ahead, a little about) */
			wanted_yaw = heading + (real)sin(game_time_get() * 0.02f + bot->player_index) * 0.6f;
		}
	}
	else if (!leave)
	{
		short drive = vehicle_bot_drive_kind(vehicle_index);
		boolean race = bots_game_type() == game_engine_race;
		real_point3d point;
		real moved = distance3d(&position, &bot->drive_last);
		real mate_side = 0.0f;

		bot->drive_last = position;
		bot->step_radius = BOT_DRIVE_STEP_RADIUS;
		if (drive == _vehicle_bot_drive_none)
		{
			leave = TRUE;
			why = "not one to drive";
		}
		/* where to: the race's next flag, the objective (on foot there, but
		in a race), the enemy in sight (run down, or circled with a gunner
		aboard; a Scorpion keeps off), else the nearest enemy */
		if (bot->has_objective)
		{
			bot->goal = bot->objective;
			bot->has_goal = TRUE;
			if (!race && distance3d(&position, &bot->objective) < 5.0f)
			{
				leave = TRUE;
				why = "at the objective";
			}
		}
		else if (bot->target_unit_index != NONE)
		{
			real_point3d origin;

			object_get_origin(bot->target_unit_index, &origin);
			bot->goal = origin;
			bot->has_goal = TRUE;
			if (drive == _vehicle_bot_drive_relative && target_distance < 15.0f)
			{
				bot->goal = position;
			}
			else if (vehicle->unit.gunner_object_index != NONE && drive == _vehicle_bot_drive_facing)
			{
				real angle = bot_yaw_to(&origin, &position) +
					((game_time_get() / 150 + DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index)) & 1 ? 0.9f : -0.9f);

				bot->goal.x = origin.x + (real)cos(angle) * 10.0f;
				bot->goal.y = origin.y + (real)sin(angle) * 10.0f;
			}
		}
		else if (!bot->has_goal || game_time_get() - bot->goal_time > 150 || game_time_get() < bot->goal_time ||
			distance3d(&position, &bot->goal) < 6.0f || bot->path_failures >= 3)
		{
			/* (the nearest enemy, unless that is where it is, out of sight
			where a vehicle cannot go: a long way off somewhere instead) */
			bot_choose_goal(bot, player, &position);
			if (distance3d(&position, &bot->goal) < 10.0f || bot->path_failures >= 3)
				bot_far_point(&position, &bot->goal);
			bot->path_failures = 0;
		}
		if (!leave && bot_next_point(bot, vehicle_index, &position, &point))
		{
			real to_point = bot_yaw_to(&position, &point);
			real off = bot_angle_difference(to_point, heading);
			real distance = distance3d(&position, &bot->goal);

			if (drive == _vehicle_bot_drive_facing)
			{
				real speed = magnitude3d(&vehicle->object.translational_velocity);
				real ahead = 4.0f + speed * 20.0f;
				real way = heading + PIN(off, -0.6f, 0.6f);

				/* turned by facing off its heading, as a stick turns it */
				turn_freely = TRUE;
				wanted_yaw = heading + PIN(off, -1.0f, 1.0f);
				action->throttle.i = fabsf(off) > 1.6f ? 0.4f : fabsf(off) > 0.8f ? 0.6f : fabsf(off) > 0.4f ? 0.8f : 1.0f;
				if (distance < 6.0f && !race)
					action->throttle.i *= 0.5f;
				/* (a wall or a rock ahead, the way it turns: round it, by the
				side most open nearest the way it wants, slower; boxed in, back
				off) */
				if (bot->reverse_ticks == 0 && bot_drive_clearance(vehicle_index, &position, way, ahead) < ahead)
				{
					static real const turns[] = { 0.4f, -0.4f, 0.8f, -0.8f, 1.2f, -1.2f };
					real best_clear = 0.0f;
					real best_turn = 0.0f;
					short index;

					for (index = 0; index < (short)NUMBEROF(turns); index++)
					{
						real turn = (off < 0.0f ? -1.0f : 1.0f) * turns[index];
						real clear = bot_drive_clearance(vehicle_index, &position, heading + turn, ahead);

						if (clear > best_clear + 0.5f)
						{
							best_clear = clear;
							best_turn = turn;
						}
					}
					if (best_clear < 3.0f)
					{
						bot->reverse_ticks = 35;
						bots.stucks++;
					}
					else
					{
						wanted_yaw = heading + PIN(best_turn * 1.5f, -1.0f, 1.0f);
						action->throttle.i = MIN(action->throttle.i, best_clear < ahead * 0.5f ? 0.5f : 0.75f);
					}
				}
				/* (a Ghost's guns at an enemy ahead of it) */
				if (bot->target_unit_index != NONE && TEST_FLAG(seat->flags, _unit_seat_gunner_bit))
				{
					real to_target = bot_yaw_to(&eye, &target_head) + bot->aim_error_yaw;

					if (fabsf(bot_angle_difference(to_target, heading)) < 0.4f)
					{
						wanted_yaw = to_target;
						wanted_pitch = (real)atan2(target_head.z - eye.z, target_distance) + bot->aim_error_pitch;
						firing = TRUE;
					}
				}
			}
			else
			{
				/* a Scorpion: pushed there from where its gun faces, which
				turns to the enemy */
				bot_walk_to(action, bot->yaw, &position, &point);
				wanted_yaw = to_point;
				if (bot->target_unit_index != NONE)
				{
					wanted_yaw = bot_yaw_to(&eye, &target_head) + bot->aim_error_yaw;
					wanted_pitch = (real)atan2(target_head.z - eye.z, target_distance) + bot->aim_error_pitch;
					firing = target_distance < bots.sight_range * 1.5f &&
						fabsf(bot_angle_difference(wanted_yaw, bot->yaw)) < BOT_FIRE_CONE * 1.5f;
				}
				if (distance3d(&position, &bot->goal) < 1.0f)
					action->throttle.i = action->throttle.j = 0.0f;
			}
		}
		/* (just in, with a teammate coming to get in: a moment's wait) */
		if (game_time_get() - bot->seated_time < 180 && game_time_get() >= bot->seated_time &&
			bot_riders_coming(bot, player, vehicle_index))
		{
			action->throttle.i = action->throttle.j = 0.0f;
			bot->drive_stuck_ticks = 0;
			bot->reverse_ticks = 0;
		}
		/* (a teammate in the way: braked, not run down, and round them
		slowly, turned off their side) */
		else if (bot_teammate_in_the_way(player, &position, heading,
			bot->reverse_ticks > 0 || action->throttle.i < 0.0f, &mate_side))
		{
			boolean backing = bot->reverse_ticks > 0 || action->throttle.i < 0.0f;

			if (moved > 0.08f)
				action->throttle.i = backing ? 1.0f : -1.0f;
			else
				action->throttle.i = backing ? -0.3f : 0.3f;
			action->throttle.j = 0.0f;
			if (drive == _vehicle_bot_drive_facing)
				wanted_yaw = heading - mate_side * (backing ? -0.9f : 0.9f);
			bot->drive_stuck_ticks = 0;
		}
		/* (an enemy close by, and the vehicle all but still: out to fight it
		on foot) */
		if (bot->target_unit_index != NONE && target_distance < 10.0f && moved < 0.05f)
		{
			if (++bot->drive_slow_ticks > 45 && !leave)
			{
				leave = TRUE;
				why = "enemy close";
			}
		}
		else
		{
			bot->drive_slow_ticks = 0;
		}
		/* stuck (pushing, but not moving): backed off, turned the other way;
		stuck again and again, out */
		if (bot->reverse_ticks > 0)
		{
			bot->reverse_ticks--;
			action->throttle.i = -action->throttle.i;
			action->throttle.j = -action->throttle.j;
			if (drive == _vehicle_bot_drive_facing)
			{
				action->throttle.i = -1.0f;
				wanted_yaw = heading - bot_angle_difference(wanted_yaw, heading);
			}
		}
		else if (fabsf(action->throttle.i) > 0.3f || fabsf(action->throttle.j) > 0.3f)
		{
			/* (making no way where it pushes: a jeep's way along its heading,
			however its wheels spin) */
			real forward_speed = drive == _vehicle_bot_drive_facing ?
				(action->throttle.i > 0.0f ? 1.0f : -1.0f) *
					dot_product3d(&vehicle->object.translational_velocity, &forward) :
				moved;

			if (forward_speed < 0.02f)
			{
				bot->drive_moving_ticks = 0;
				if (++bot->drive_stuck_ticks >= 45)
				{
					bot->drive_stuck_ticks = 0;
					bot->reverse_ticks = 40;
					bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
					bots.stucks++;
					if (++bot->drive_stucks >= 4)
					{
						/* (and that vehicle left alone a while) */
						leave = TRUE;
						why = "stuck";
						bot->ignored_vehicle_index = vehicle_index;
						bot->ignored_vehicle_time = game_time_get();
					}
				}
			}
			else
			{
				bot->drive_stuck_ticks = 0;
				if (++bot->drive_moving_ticks > 90)
					bot->drive_stucks = 0;
			}
		}
	}

	if (turn_freely)
	{
		bot->yaw = wanted_yaw;
		bot->pitch += PIN(wanted_pitch - bot->pitch, -bots.turn_rate, bots.turn_rate);
	}
	else
	{
		bot->yaw += PIN(bot_angle_difference(wanted_yaw, bot->yaw), -bots.turn_rate, bots.turn_rate);
		bot->pitch += PIN(wanted_pitch - bot->pitch, -bots.turn_rate, bots.turn_rate);
	}
	bot->pitch = PIN(bot->pitch, -1.4f, 1.4f);
	while (bot->yaw > _pi)
		bot->yaw -= 2.0f * _pi;
	while (bot->yaw < -_pi)
		bot->yaw += 2.0f * _pi;
	action->desired_facing.yaw = bot->yaw < 0.0f ? bot->yaw + 2.0f * _pi : bot->yaw;
	action->desired_facing.pitch = bot->pitch;
	action->throttle.i = PIN(action->throttle.i, -1.0f, 1.0f);
	action->throttle.j = PIN(action->throttle.j, -1.0f, 1.0f);
	if (firing)
	{
		action->primary_trigger = 1.0f;
		SET_FLAG(action->control_flags, _unit_control_weapon_primary_trigger_bit, TRUE);
	}
	if (leave)
	{
		if (!bot->leave_logged)
			platform_log("bots: player %ld leaves its vehicle (%s)", DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index), why);
		bot->leave_logged = TRUE;
		SET_FLAG(action->control_flags, _unit_control_action_bit, TRUE);
		action->throttle.i = action->throttle.j = 0.0f;
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
	if (!unit)
	{
		bot->target_unit_index = NONE;
		return;
	}
	if (unit->object.parent_object_index != NONE)
	{
		bot_ride(bot, player, unit, action);
		return;
	}
	if (bot->riding)
	{
		/* (out: none again for a while) */
		bot->riding = FALSE;
		bot->vehicle_again_time = game_time_get() + BOT_VEHICLE_AGAIN_TICKS;
		bot->has_goal = FALSE;
		bot->path.valid = FALSE;
		bots.vehicle_exits++;
		platform_log("bots: player %ld got out of a vehicle", DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index));
	}
	bot->step_radius = BOT_STEP_RADIUS;
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

	/* a vehicle: looked for now and then, out of a close fight, while the
	way is long (or always, in a race), and gone to, its door before all */
	{
		real_point3d entrance;
		boolean race = bots_game_type() == game_engine_race;

		if (bot->vehicle_index == NONE && !bot->carrying && game_time_get() >= bot->vehicle_again_time &&
			(game_time_get() - bot->vehicle_look_time >= ITEM_LOOK_TICKS || game_time_get() < bot->vehicle_look_time))
		{
			/* (far: the objective, or else the nearest enemy, a long way off) */
			boolean far = race || (bot->has_objective ? distance3d(&position, &bot->objective) > 25.0f :
				bot_nearest_enemy_distance(player, &position) > 30.0f);
			boolean close_fight = bot->target_unit_index != NONE &&
				object_try_and_get(bot->target_unit_index) &&
				(object_get_origin(bot->target_unit_index, &entrance), distance3d(&position, &entrance) < 12.0f);

			if (!close_fight && !(bot->holding && !far))
				bot_choose_vehicle(bot, player, &position, far);
		}
		if (bot->carrying)
			bot->vehicle_index = NONE;
		if (bot_vehicle_entrance(bot, player, &position, &entrance))
		{
			bot->has_objective = TRUE;
			bot->holding = FALSE;
			bot->objective = entrance;
			bot->objective_unit_index = NONE;
		}
	}

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
		if (!bot->carrying && !bot->has_objective && (game_time_get() - bot->item_look_time >= ITEM_LOOK_TICKS / 2 ||
			game_time_get() < bot->item_look_time))
		{
			bot->item_look_time = game_time_get();
			bot->item_index = bot_find_item(bot, unit, &position, 10.0f, 4.0f);
		}
		if (bot->item_index != NONE && !bot->carrying && !bot->has_objective && object_try_and_get(bot->item_index) &&
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
			if (bot_item_out_of_reach(bot, distance3d(&position, &item_position)))
			{
				bot_ignore_item(bot, bot->item_index);
				bot->item_index = NONE;
			}
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
			long item = bot_find_item(bot, unit, &position, BOT_ITEM_RANGE, 0.0f);

			bot->item_look_time = game_time_get();
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
				bot_ignore_item(bot, bot->item_index);
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
				/* (no nearer a while: out of reach, as on a ledge, or not
				offered: given up for a while) */
				if (bot_item_out_of_reach(bot, distance3d(&position, &bot->goal)))
				{
					bot_ignore_item(bot, bot->item_index);
					bot->item_index = NONE;
					bot->has_goal = FALSE;
				}
			}
		}
		/* (in a team, every third bot with nothing to do keeps near the
		nearest person on its team; the rest go after the enemy) */
		following = FALSE;
		if (bot->item_index == NONE && !bot->has_objective && game_engine_has_teams() &&
			(DATUM_INDEX_TO_ABSOLUTE_INDEX(bot->player_index) % 3) == 0 &&
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
		/* (holding a spot and near it: there, not pressing on into what may
		stand on it, a pillar, a wall) */
		if (bot->holding && bot->has_objective &&
			(bot->goal.x - position.x) * (bot->goal.x - position.x) +
				(bot->goal.y - position.y) * (bot->goal.y - position.y) < 1.5f * 1.5f)
		{
			bot->walking = FALSE;
		}
		else if (bot_next_point(bot, player->unit_index, &position, &point))
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

	/* at a vehicle's door: still, and in */
	if (bot->vehicle_index != NONE)
	{
		real_point3d entrance;
		real_point3d seat_point;

		if (bot->vehicle_seat == BOT_FLIP_VEHICLE)
		{
			object_get_origin(bot->vehicle_index, &entrance);
			if (distance3d(&position, &entrance) < 2.5f)
				bot_get_in(bot, player, &position, action);
		}
		else if (unit_get_seat_entrance_point(player->unit_index, bot->vehicle_index, bot->vehicle_seat, &entrance,
			&seat_point, NULL) && distance3d(&position, &entrance) < 1.0f)
		{
			bot_get_in(bot, player, &position, action);
		}
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
		/* (at the point, as on a thing to pick up: there, not stuck) */
		if ((progress && moved > 0.02f) || distance < 0.5f)
		{
			bot->stuck_ticks = 0;
		}
		else
		{
			bot->stuck_ticks++;
			if (bot->stuck_ticks == 6)
			{
				/* (to the side that is open, as round a pillar; either, at
				random: left, the strafe's positive, bot_walk_to) */
				real_point3d left = position, right = position;
				boolean left_open, right_open;

				left.x += -(real)sin(bot->yaw) * 1.2f;
				left.y += (real)cos(bot->yaw) * 1.2f;
				right.x -= -(real)sin(bot->yaw) * 1.2f;
				right.y -= (real)cos(bot->yaw) * 1.2f;
				left_open = bot_clear_walk(player->unit_index, &position, &left, FALSE);
				right_open = bot_clear_walk(player->unit_index, &position, &right, FALSE);
				bot->sidestep = left_open != right_open ? (left_open ? 1.0f : -1.0f) :
					bot_random() < 0.5f ? 1.0f : -1.0f;
				bot->sidestep_ticks = 10;
				/* (the point a little above and near, up a step or the lip of
				a ramp: a jump on towards it, not a step aside) */
				if (bot->walk_point.z - position.z > 0.3f && distance < 3.0f)
				{
					bot->sidestep_ticks = 0;
					SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
				}
				/* (near a point of its path it cannot reach, at a corner: the
				next one) */
				if (distance < 1.5f && bot->path.valid && bot->path.step_index < bot->path.step_count)
				{
					bot->path.step_index++;
					bot->sidestep_ticks = 4;
				}
				bots.stucks++;
				/* (the third time in five seconds: the goal given up, the
				thing to pick up for a while, and somewhere else, away from
				where it could not get) */
				if (game_time_get() - bot->stuck_count_time > 150 || game_time_get() < bot->stuck_count_time)
				{
					bot->stuck_count = 0;
					bot->stuck_count_time = game_time_get();
				}
				if (++bot->stuck_count >= 3)
				{
					real away = (real)atan2(position.y - bot->walk_point.y, position.x - bot->walk_point.x) +
						(bot_random() - 0.5f) * 1.5f;

					bot->stuck_count = 0;
					if (bot->item_index != NONE)
					{
						bot_ignore_item(bot, bot->item_index);
						bot->item_index = NONE;
					}
					if (bot->holding)
						bot->path_failures = 3;
					else if (!bot->has_objective)
					{
						bot->goal = position;
						bot->goal.x += (real)cos(away) * 10.0f;
						bot->goal.y += (real)sin(away) * 10.0f;
						bot->has_goal = TRUE;
						bot->goal_time = game_time_get();
						bot->follow_again_time = game_time_get() + 150;
					}
					bots.gave_up++;
				}

				bot->path_time = game_time_get() - PATH_REFRESH_TICKS;
				bot->walk_distance = 1000000.0f;
			}
			if (bot->stuck_ticks == 12)
				SET_FLAG(action->control_flags, _unit_control_jump_bit, TRUE);
			if (bot->stuck_ticks == 24)
				bots.long_stucks++;
			/* (a spot to hold it cannot get to: the place's centre instead,
			bot_hold_spot) */
			if (bot->stuck_ticks == 18 && bot->holding)
				bot->path_failures = 3;
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
		free_bot->item_progress_index = NONE;
		{
			short ignored;

			for (ignored = 0; ignored < BOT_IGNORED_ITEMS; ignored++)
				free_bot->ignored_items[ignored] = NONE;
		}
		free_bot->vehicle_index = NONE;
		free_bot->ignored_vehicle_index = NONE;
		free_bot->step_radius = BOT_STEP_RADIUS;
	}
	return free_bot;
}

/* ---------- public code */

void bots_new_game(
	void)
{
	short index;

	bots.map_logged = FALSE;
	bots.vehicles_logged = FALSE;
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
		if (!bots.vehicles_logged)
		{
			/* (the map's vehicles, once) */
			struct object_iterator iterator;

			bots.vehicles_logged = TRUE;
			object_iterator_new(&iterator, _object_mask_vehicle, 0);
			while (object_iterator_next(&iterator))
			{
				char const *name = tag_get_name(object_get(iterator.index)->definition_index);

				platform_log("bots: a vehicle: %s (kind %d)", name ? name : "?", (int)vehicle_bot_drive_kind(iterator.index));
			}
		}
		bots.logged_time = game_time_get();
		platform_log("bots: driving %d; paths found %ld, not %ld; weapon swaps asked %ld; grenades %ld; changes %ld; stuck %ld, long %ld, gave up %ld (no start %ld, no goal %ld, no way %ld); vehicles: in %ld, out %ld",
			(int)count, bots.paths_found, bots.paths_failed, bots.pickups, bots.grenades, bots.switches, bots.stucks, bots.long_stucks, bots.gave_up, bots.fail_start, bots.fail_goal, bots.fail_search,
			bots.vehicle_entries, bots.vehicle_exits);
		bots.vehicle_entries = bots.vehicle_exits = 0;
		bots.fail_start = bots.fail_goal = bots.fail_search = 0;
		bots.paths_found = bots.paths_failed = bots.pickups = bots.grenades = bots.switches = bots.stucks = bots.long_stucks = bots.gave_up = 0;
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
				if (bot && bot->riding && unit->object.parent_object_index != NONE)
				{
					real_point3d origin;
					real_vector3d velocity = object_get(unit->object.parent_object_index)->object.translational_velocity;

					object_get_origin(unit->object.parent_object_index, &origin);
					platform_log("bots:     in a vehicle, seat %d, at (%.1f %.1f %.1f) going %.2f; goal %.1f off, path %s, stuck %d",
						(int)unit->unit.parent_seat_index, origin.x, origin.y, origin.z, magnitude3d(&velocity),
						bot->has_goal ? distance3d(&origin, &bot->goal) : -1.0f, bot->path.valid ? "found" : "none",
						(int)bot->drive_stucks);
				}
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
