/*
NETWORK_VR.C

VR players' hands for the other machines: an extension of the distributed
netcode (port/linux/NETCODE.md, network_distributed.c). A VR player's
weapon aims where the weapon hand's controller points and fires from the
hand, and the free hand holds a grenade and throws it its own way
(port/linux/src/vr.c; the right hand and the left, or left-handed the other
way round); the input the netcode relays has only where the head looks, so
the other machines would show that player aiming and throwing where they
look.

Every tick, a machine with a VR player sends the host that player's hands
(_distributed_message_vr_poses, unreliable): where the weapon aims, where
its muzzle is from the unit's camera (whichever hand holds it: the others
see shots leave the left hand of a left-handed player), whether the free
hand holds a grenade, and the last throw (from where, which way, how hard)
for a second after it. The host keeps them, its own VR players' too, and
sends each client those of the players of other machines. A machine drives
a VR player of another (players.c, weapons.c, units.c) as long as their
hands keep coming, else as before.

What the hands decide is only how the game looks: damage is the shooter's
report, which the host checks as any other (network_damage.c).

A machine that does not know the message (a build without this, of the same
network version) drops it, as it drops any type past its own
(network_distributed_handle_message): VR and other builds still play
together, the others seeing a VR player aim where the head looks.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/players.h"
#include "units/units.h"
#include "network_distributed.h"
#include "halo_vr.h"

#include <math.h>

void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	_vr_pose_aim_bit,
	_vr_pose_weapon_offset_bit,
	_vr_pose_grenade_held_bit,
	_vr_pose_throw_bit,
};

/* how long a player's hands drive them after the last came (ticks): longer
and they are driven as before (the hands stopped coming: the player left
VR, or the network). Where the weapon aims and fires from is held a
second, over the gaps of a network that delivers in bursts (as the input
the netcode relays is); a grenade held, only a third of one, so that it
does not hang in the hand after the hand let go. The host relays only
hands as fresh as that. */
#define VR_POSE_FRESH_TICKS 10
#define VR_AIM_HOLD_TICKS 30
/* how long a throw is sent after it (ticks), and taken after it came */
#define VR_THROW_TICKS 30
/* offsets from the unit's camera: in 1/4096 of a world unit, no further
than a hand reaches */
#define VR_OFFSET_SCALE 4096.0f
#define VR_OFFSET_MAXIMUM 1.0f
/* how often what was sent and heard is logged (ticks) */
#define VR_LOG_TICKS 150

/* ---------- structures */

struct distributed_vr_pose
{
	byte player_index;
	byte flags;
	byte throw_sequence;
	byte throw_strength;
	struct distributed_vector aim;
	struct distributed_vector weapon_offset;
	struct distributed_vector throw_offset;
	struct distributed_vector throw_direction;
};

typedef char distributed_vr_pose_size_assert[sizeof(struct distributed_vr_pose) == 28 ? 1 : -1];

/* a player's hands, as last heard of (this machine's own VR players on the
host too, to send) */
struct network_vr_player
{
	boolean valid;
	long received_time;
	struct distributed_vr_pose pose;
	/* the throw this machine has taken for the player's grenade */
	boolean throw_taken;
	byte throw_taken_sequence;
	long throw_received_time;
	byte throw_received_sequence;
};

/* a local VR player's last throw */
struct network_vr_throw
{
	boolean valid;
	long time;
	byte sequence;
	real_vector3d offset;
	real_vector3d direction;
	real strength;
};

/* ---------- globals */

static struct network_vr_player network_vr_players[MAXIMUM_TRACKED_PLAYERS];
static struct network_vr_throw network_vr_throws[MAXIMUM_LOCAL_PLAYERS];
static byte network_vr_throw_sequences[MAXIMUM_LOCAL_PLAYERS];
/* what was sent and heard since last logged */
static struct
{
	long sent;
	long heard;
	long aimed;
} network_vr_counts;

/* ---------- private code */

static boolean network_vr_in_network_game(
	void)
{
	short connection = game_connection();

	return connection == _game_connection_network_server || connection == _game_connection_network_client;
}

static void network_vr_offset_pack(
	real_vector3d const *offset,
	struct distributed_vector *result)
{
	distributed_vector_pack(offset, VR_OFFSET_SCALE, result);
}

/* (FALSE: not a hand's offset) */
static boolean network_vr_offset_unpack(
	struct distributed_vector const *packed,
	real_vector3d *offset)
{
	distributed_vector_unpack(packed, VR_OFFSET_SCALE, offset);
	return fabsf(offset->i) <= VR_OFFSET_MAXIMUM && fabsf(offset->j) <= VR_OFFSET_MAXIMUM &&
		fabsf(offset->k) <= VR_OFFSET_MAXIMUM;
}

/* (FALSE: no direction) */
static boolean network_vr_direction_unpack(
	struct distributed_vector const *packed,
	real_vector3d *direction)
{
	distributed_unit_vector_unpack(packed, direction);
	return direction->i * direction->i + direction->j * direction->j + direction->k * direction->k > 0.5f;
}

/* a player of another machine whose hands came lately, or NULL */
static struct network_vr_player *network_vr_remote_player(
	long player_index,
	long fresh_ticks)
{
	struct network_vr_player *vr_player;
	long absolute_index;

	if (player_index == NONE || !network_vr_in_network_game() || distributed_player_is_local(player_index))
		return NULL;
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_PLAYERS)
		return NULL;
	vr_player = &network_vr_players[absolute_index];
	if (!vr_player->valid || game_time_get() - vr_player->received_time > fresh_ticks ||
		game_time_get() < vr_player->received_time)
	{
		return NULL;
	}
	return vr_player;
}

/* what is said of a player's hands, kept (checked: a client's of its own) */
static void network_vr_take_pose(
	struct distributed_vr_pose const *pose,
	boolean heard)
{
	struct network_vr_player *vr_player;
	real_vector3d vector;

	if (pose->player_index >= MAXIMUM_TRACKED_PLAYERS)
		return;
	vr_player = &network_vr_players[pose->player_index];
	vr_player->pose = *pose;
	if (TEST_FLAG(pose->flags, _vr_pose_aim_bit) && !network_vr_direction_unpack(&pose->aim, &vector))
		SET_FLAG(vr_player->pose.flags, _vr_pose_aim_bit, FALSE);
	if (TEST_FLAG(pose->flags, _vr_pose_weapon_offset_bit) && !network_vr_offset_unpack(&pose->weapon_offset, &vector))
		SET_FLAG(vr_player->pose.flags, _vr_pose_weapon_offset_bit, FALSE);
	if (TEST_FLAG(pose->flags, _vr_pose_throw_bit) &&
		(!network_vr_offset_unpack(&pose->throw_offset, &vector) ||
			!network_vr_direction_unpack(&pose->throw_direction, &vector)))
	{
		SET_FLAG(vr_player->pose.flags, _vr_pose_throw_bit, FALSE);
	}
	/* (a throw new to this machine: when it came) */
	if (TEST_FLAG(vr_player->pose.flags, _vr_pose_throw_bit) &&
		(!vr_player->valid || vr_player->throw_received_sequence != pose->throw_sequence ||
			vr_player->throw_received_time == NONE))
	{
		vr_player->throw_received_sequence = pose->throw_sequence;
		vr_player->throw_received_time = game_time_get();
	}
	vr_player->valid = TRUE;
	vr_player->received_time = game_time_get();
	if (heard)
		network_vr_counts.heard++;
}

/* this machine's VR player's hands, to send; FALSE when it has none */
static boolean network_vr_local_pose(
	short local_player_index,
	struct distributed_vr_pose *pose)
{
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct unit_datum *unit = player && player->unit_index != NONE ? unit_try_and_get(player->unit_index) : NULL;
	struct network_vr_throw *last_throw = &network_vr_throws[local_player_index];
	float vector[3];

	if (!unit || !halo_vr_active())
		return FALSE;
	csmemset(pose, 0, sizeof(*pose));
	pose->player_index = distributed_player_to_byte(player_index);
	/* (on foot: seated, the seat aims, and the input says where) */
	if (unit->object.parent_object_index == NONE)
	{
		if (halo_vr_aim_vector(local_player_index, vector))
		{
			real_vector3d aim;

			aim.i = vector[0];
			aim.j = vector[1];
			aim.k = vector[2];
			distributed_vector_pack(&aim, DISTRIBUTED_UNIT_SCALE, &pose->aim);
			SET_FLAG(pose->flags, _vr_pose_aim_bit, TRUE);
		}
		if (halo_vr_weapon_origin_offset(local_player_index, vector))
		{
			real_vector3d offset;

			offset.i = vector[0];
			offset.j = vector[1];
			offset.k = vector[2];
			if (fabsf(offset.i) <= VR_OFFSET_MAXIMUM && fabsf(offset.j) <= VR_OFFSET_MAXIMUM &&
				fabsf(offset.k) <= VR_OFFSET_MAXIMUM)
			{
				network_vr_offset_pack(&offset, &pose->weapon_offset);
				SET_FLAG(pose->flags, _vr_pose_weapon_offset_bit, TRUE);
			}
		}
	}
	if (halo_vr_grenade_held(local_player_index))
		SET_FLAG(pose->flags, _vr_pose_grenade_held_bit, TRUE);
	if (last_throw->valid && game_time_get() - last_throw->time <= VR_THROW_TICKS &&
		game_time_get() >= last_throw->time)
	{
		network_vr_offset_pack(&last_throw->offset, &pose->throw_offset);
		distributed_vector_pack(&last_throw->direction, DISTRIBUTED_UNIT_SCALE, &pose->throw_direction);
		pose->throw_sequence = last_throw->sequence;
		pose->throw_strength = (byte)(PIN(last_throw->strength, 0.0f, 1.0f) * 255.0f + 0.5f);
		SET_FLAG(pose->flags, _vr_pose_throw_bit, TRUE);
	}
	return TRUE;
}

/* ---------- public code */

void network_vr_new_game(
	void)
{
	short index;

	csmemset(network_vr_players, 0, sizeof(network_vr_players));
	for (index = 0; index < MAXIMUM_TRACKED_PLAYERS; index++)
		network_vr_players[index].throw_received_time = NONE;
	csmemset(network_vr_throws, 0, sizeof(network_vr_throws));
	csmemset(&network_vr_counts, 0, sizeof(network_vr_counts));
}

word network_vr_entry_size(
	void)
{
	return sizeof(struct distributed_vr_pose);
}

/* after each tick (network_distributed_tick): a client its own VR players'
hands to the host; the host its own and its clients' to each client */
void network_vr_tick(
	void)
{
	short connection = game_connection();
	short local_player_index;
	struct
	{
		struct distributed_message_header header;
		struct distributed_vr_pose poses[MAXIMUM_TRACKED_PLAYERS];
	} message;
	short count = 0;

	if (game_time_get() % VR_LOG_TICKS == 0 &&
		(network_vr_counts.sent || network_vr_counts.heard || network_vr_counts.aimed))
	{
		platform_log("VR network: hands sent %ld, heard %ld, other players aimed by them %ld (in %d ticks)",
			network_vr_counts.sent, network_vr_counts.heard, network_vr_counts.aimed, VR_LOG_TICKS);
		csmemset(&network_vr_counts, 0, sizeof(network_vr_counts));
	}
	if (connection == _game_connection_network_client)
	{
		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			if (network_vr_local_pose(local_player_index, &message.poses[count]))
				count++;
		}
		network_vr_counts.sent += count;
		if (count)
		{
			distributed_send(&message, _distributed_message_vr_poses, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_vr_pose)), _distributed_to_host);
		}
	}
	else if (connection == _game_connection_network_server)
	{
		long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
		short machine_count;
		short machine_number;
		short player_index;

		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			struct distributed_vr_pose pose;

			if (network_vr_local_pose(local_player_index, &pose))
				network_vr_take_pose(&pose, FALSE);
		}
		machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		for (machine_number = 0; machine_number < machine_count; machine_number++)
		{
			long machine_index = machine_indices[machine_number];

			count = 0;
			for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS && count < 255; player_index++)
			{
				struct network_vr_player const *vr_player = &network_vr_players[player_index];

				if (vr_player->valid && game_time_get() - vr_player->received_time <= VR_POSE_FRESH_TICKS &&
					game_time_get() >= vr_player->received_time &&
					distributed_player(player_index) &&
					!distributed_machine_has_player(machine_index, player_index))
				{
					message.poses[count++] = vr_player->pose;
				}
			}
			network_vr_counts.sent += count;
			if (count)
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_vr_poses, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_vr_pose)));
			}
		}
	}
}

/* the hands a machine sent: the host's of a client's players, a client's
of the others */
void network_vr_handle_poses(
	long machine_index,
	void const *entries,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_vr_pose pose;

		csmemcpy(&pose, (byte const *)entries + index * sizeof(pose), sizeof(pose));
		if (pose.player_index >= MAXIMUM_TRACKED_PLAYERS || !distributed_player(pose.player_index))
			continue;
		if (machine_index != NONE)
		{
			/* (the host: only of that client's own players) */
			if (!distributed_machine_has_player(machine_index, pose.player_index))
				continue;
		}
		else if (distributed_player_is_local(distributed_player_from_byte(pose.player_index)))
		{
			continue;
		}
		network_vr_take_pose(&pose, TRUE);
	}
}

/* ---------- the game's side (halo_vr.h) */

int halo_vr_network_aim(long player_index, float vector[3])
{
	struct network_vr_player *vr_player = network_vr_remote_player(player_index, VR_AIM_HOLD_TICKS);
	real_vector3d aim;

	if (!vr_player || !TEST_FLAG(vr_player->pose.flags, _vr_pose_aim_bit) ||
		!network_vr_direction_unpack(&vr_player->pose.aim, &aim))
	{
		return 0;
	}
	vector[0] = aim.i;
	vector[1] = aim.j;
	vector[2] = aim.k;
	network_vr_counts.aimed++;
	return 1;
}

int halo_vr_network_weapon_origin(long player_index, long unit_index, float origin[3])
{
	struct network_vr_player *vr_player = network_vr_remote_player(player_index, VR_AIM_HOLD_TICKS);
	real_vector3d offset;
	real_point3d camera;

	if (!vr_player || unit_index == NONE || !TEST_FLAG(vr_player->pose.flags, _vr_pose_weapon_offset_bit) ||
		!network_vr_offset_unpack(&vr_player->pose.weapon_offset, &offset))
	{
		return 0;
	}
	unit_get_camera_position(unit_index, &camera);
	origin[0] = camera.x + offset.i;
	origin[1] = camera.y + offset.j;
	origin[2] = camera.z + offset.k;
	return 1;
}

int halo_vr_network_grenade_held(long player_index)
{
	struct network_vr_player *vr_player = network_vr_remote_player(player_index, VR_POSE_FRESH_TICKS);

	return vr_player && TEST_FLAG(vr_player->pose.flags, _vr_pose_grenade_held_bit);
}

int halo_vr_network_grenade_throw(long player_index, long unit_index, float origin[3], float direction[3],
	float *strength)
{
	struct network_vr_player *vr_player = network_vr_remote_player(player_index, VR_THROW_TICKS);
	real_vector3d offset;
	real_vector3d way;
	real_point3d camera;

	if (!vr_player || unit_index == NONE || !TEST_FLAG(vr_player->pose.flags, _vr_pose_throw_bit) ||
		vr_player->throw_received_time == NONE || game_time_get() - vr_player->throw_received_time > VR_THROW_TICKS ||
		(vr_player->throw_taken && vr_player->throw_taken_sequence == vr_player->pose.throw_sequence) ||
		!network_vr_offset_unpack(&vr_player->pose.throw_offset, &offset) ||
		!network_vr_direction_unpack(&vr_player->pose.throw_direction, &way))
	{
		return 0;
	}
	vr_player->throw_taken = TRUE;
	vr_player->throw_taken_sequence = vr_player->pose.throw_sequence;
	unit_get_camera_position(unit_index, &camera);
	origin[0] = camera.x + offset.i;
	origin[1] = camera.y + offset.j;
	origin[2] = camera.z + offset.k;
	direction[0] = way.i;
	direction[1] = way.j;
	direction[2] = way.k;
	*strength = (float)vr_player->pose.throw_strength / 255.0f;
	platform_log("VR network: player %ld's grenade thrown as their hand threw it (%.2f)",
		DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index), *strength);
	return 1;
}

void halo_vr_network_grenade_thrown(short local_player_index, long unit_index, const float origin[3],
	const float direction[3], float strength)
{
	struct network_vr_throw *last_throw;
	real_point3d camera;

	if (local_player_index < 0 || local_player_index >= MAXIMUM_LOCAL_PLAYERS || unit_index == NONE ||
		!network_vr_in_network_game())
	{
		return;
	}
	last_throw = &network_vr_throws[local_player_index];
	unit_get_camera_position(unit_index, &camera);
	last_throw->offset.i = origin[0] - camera.x;
	last_throw->offset.j = origin[1] - camera.y;
	last_throw->offset.k = origin[2] - camera.z;
	/* (a hand further off than it reaches: none sent) */
	if (fabsf(last_throw->offset.i) > VR_OFFSET_MAXIMUM || fabsf(last_throw->offset.j) > VR_OFFSET_MAXIMUM ||
		fabsf(last_throw->offset.k) > VR_OFFSET_MAXIMUM)
	{
		last_throw->valid = FALSE;
		return;
	}
	last_throw->direction.i = direction[0];
	last_throw->direction.j = direction[1];
	last_throw->direction.k = direction[2];
	last_throw->strength = strength;
	last_throw->time = game_time_get();
	last_throw->sequence = ++network_vr_throw_sequences[local_player_index];
	last_throw->valid = TRUE;
	platform_log("VR network: grenade throw #%d sent (%.2f)", last_throw->sequence, strength);
}
