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
};

/* ---------- globals */

/* (one search at a time, shared: 80 kB) */
static struct path_state bot_path_state;

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
		return FALSE;
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
		return FALSE;
	path_input_new(&input, 0.25f, FALSE, unit_index);
	path_input_set_start(&input, &start, start_surface);
	path_state_new(&input, &bot_path_state, NULL);
	path_state_destination(&bot_path_state, &goal, goal_surface, BOT_GOAL_RADIUS);
	if (!path_state_find(&bot_path_state) || !path_state_build_path(&bot_path_state, &bot->path))
	{
		bot->path.valid = FALSE;
		bots.paths_failed++;
		return FALSE;
	}
	bots.paths_found += bot->path.valid != FALSE;
	return bot->path.valid;
}

/* the point it walks to now: the path's next, or the goal itself */
static boolean bot_next_point(
	struct bot *bot,
	long unit_index,
	real_point3d const *position,
	real_point3d *point)
{
	if (!bot->has_goal)
		return FALSE;
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
		return distance > 10.0f ? worth + 2.0f : worth;
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
		if (carried < 2)
			return worth;
		return worth - worst > 0.5f ? worth - worst : 0.0f;
	}
	return 0.0f;
}

/* the thing most worth going for near the bot, or NONE */
static long bot_find_item(
	struct unit_datum const *unit,
	real_point3d const *position)
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
		if (distance > BOT_ITEM_RANGE)
			continue;
		worth = bot_item_worth(unit, iterator.index);
		if (worth <= 0.0f)
			continue;
		score = worth / (1.0f + distance / 8.0f);
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
		/* closes on it along its way, weaving, until near */
		bot->weave_phase++;
		if (distance > BOT_CLOSE_RANGE)
		{
			real_point3d point;

			object_get_origin(bot->target_unit_index, &bot->goal);
			bot->has_goal = TRUE;
			if (bot_next_point(bot, player->unit_index, &position, &point))
				bot_walk_to(action, bot->yaw, &position, &point);
		}
		action->throttle.j += (real)sin(bot->weave_phase * 0.08f) * 0.6f;
		firing = distance < BOT_FIRE_RANGE &&
			fabsf(bot_angle_difference(wanted_yaw, bot->yaw)) < BOT_FIRE_CONE &&
			fabsf(wanted_pitch - bot->pitch) < BOT_FIRE_CONE;
		/* (a melee blow when it is upon it) */
		if (distance < 1.0f)
			SET_FLAG(action->control_flags, _unit_control_use_equipment_bit, TRUE);
		/* (a grenade now and then, lobbed at one some way off) */
		if (distance > BOT_GRENADE_MINIMUM && distance < BOT_GRENADE_MAXIMUM &&
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

		/* something worth having near: there first */
		if (game_time_get() - bot->item_look_time >= ITEM_LOOK_TICKS || game_time_get() < bot->item_look_time)
		{
			long item = bot_find_item(unit, &position);

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
		if (bot->item_index != NONE)
		{
			/* (taken or gone: on) */
			if (!object_try_and_get(bot->item_index) || bot_item_worth(unit, bot->item_index) <= 0.0f)
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
		/* hunts, or roams: along its way, looking where it goes */
		if (bot->item_index == NONE &&
			(!bot->has_goal || game_time_get() - bot->goal_time > 90 ||
				distance3d(&position, &bot->goal) < BOT_GOAL_RADIUS))
		{
			bot_choose_goal(bot, player, &position);
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

	/* stuck: hardly moved while walking (it jumps, and then turns) */
	moved = distance3d(&position, &bot->last_position);
	bot->last_position = position;
	action->throttle.i = PIN(action->throttle.i, -1.0f, 1.0f);
	action->throttle.j = PIN(action->throttle.j, -1.0f, 1.0f);
	if ((fabsf(action->throttle.i) > 0.5f || fabsf(action->throttle.j) > 0.5f) && moved < 0.02f)
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
		free_bot->item_index = NONE;
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
		platform_log("bots: driving %d; paths found %ld, not %ld; weapon swaps asked %ld; grenades %ld; changes %ld",
			(int)count, bots.paths_found, bots.paths_failed, bots.pickups, bots.grenades, bots.switches);
		bots.paths_found = bots.paths_failed = bots.pickups = bots.grenades = bots.switches = 0;
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
			platform_log("bots:   player %d carries %s, grenades %d/%d", index, text, unit->unit.grenade_counts[0],
				unit->unit.grenade_counts[1]);
		}
	}
}
