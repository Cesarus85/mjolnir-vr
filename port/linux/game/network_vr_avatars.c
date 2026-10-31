/*
NETWORK_VR_AVATARS.C

VR players' heads and hands for the other machines, to draw their bodies
after them (vr_avatars.c): an extension of the distributed netcode
(port/linux/NETCODE.md, network_distributed.c), as network_vr.c's hands
are.

Every tick, a machine with a VR player sends the host that player's head
and hands (_distributed_message_vr_avatars, unreliable): the eyes, the
weapon hand's grip and the free hand's grip, each an offset from the
unit's camera and a rotation (halo_vr_avatar), as they are in the room.
The host keeps them, its own VR players' too, and sends each client those
of the players of other machines, as long as they keep coming.

They only change how the other machines draw a VR player: the game's pose,
its hit boxes and what the host checks are as before. A machine that does
not know the message (a build without it, of the same network version)
drops it, as it drops any type past its own.
*/

#include "cseries.h"
#include "math/real_math.h"
#include "game/game.h"
#include "game/players.h"
#include "units/units.h"
#include "network_distributed.h"
#include "halo_vr.h"
#include "vr_avatars.h"

#include <math.h>

unsigned long system_milliseconds(void);

/* ---------- constants */

/* how long a player's head and hands are drawn after the last came (ticks):
longer, and the body is drawn as the game poses it. The host relays only
those as fresh as that. */
#define VR_AVATAR_FRESH_TICKS 10
/* offsets from the unit's camera: in 1/4096 of a world unit, no further
than a hand reaches */
#define VR_AVATAR_OFFSET_SCALE 4096.0f
#define VR_AVATAR_OFFSET_MAXIMUM 1.0f
/* what is drawn lags what came this long (milliseconds), to ease between
the ticks' poses */
#define VR_AVATAR_DELAY_MS 50

enum
{
	_vr_avatar_head,
	_vr_avatar_weapon_hand,
	_vr_avatar_free_hand,
	NUMBER_OF_VR_AVATAR_PARTS
};

/* ---------- structures */

struct distributed_vr_avatar_part
{
	struct distributed_vector offset;
	short rotation[4];
};

struct distributed_vr_avatar
{
	byte player_index;
	byte known;
	struct distributed_vr_avatar_part parts[NUMBER_OF_VR_AVATAR_PARTS];
};

typedef char distributed_vr_avatar_size_assert[sizeof(struct distributed_vr_avatar) == 44 ? 1 : -1];

/* a part as taken: its offset (world units) and rotation (x, y, z, w) */
struct vr_avatar_sample
{
	unsigned long received_ms;
	byte known;
	real_vector3d offsets[NUMBER_OF_VR_AVATAR_PARTS];
	real rotations[NUMBER_OF_VR_AVATAR_PARTS][4];
};

/* a player's head and hands as last heard of, and before (this machine's
own VR players on the host too, to send) */
struct network_vr_avatar
{
	boolean valid;
	long received_time;
	struct distributed_vr_avatar entry;
	boolean previous_valid;
	struct vr_avatar_sample previous, latest;
};

/* ---------- globals */

static struct network_vr_avatar network_vr_avatars[MAXIMUM_TRACKED_PLAYERS];

/* ---------- private code */

static boolean network_vr_avatars_in_network_game(
	void)
{
	short connection = game_connection();

	return connection == _game_connection_network_server || connection == _game_connection_network_client;
}

static void network_vr_avatar_pack(
	float const offset[3],
	float const rotation[4],
	struct distributed_vr_avatar_part *part)
{
	real_vector3d vector;
	short index;

	vector.i = offset[0];
	vector.j = offset[1];
	vector.k = offset[2];
	distributed_vector_pack(&vector, VR_AVATAR_OFFSET_SCALE, &part->offset);
	for (index = 0; index < 4; index++)
		part->rotation[index] = (short)(PIN(rotation[index], -1.0f, 1.0f) * 32767.0f);
}

/* (FALSE: not a part's offset, or no rotation) */
static boolean network_vr_avatar_unpack(
	struct distributed_vr_avatar_part const *part,
	real_vector3d *offset,
	real rotation[4])
{
	real length = 0.0f;
	short index;

	distributed_vector_unpack(&part->offset, VR_AVATAR_OFFSET_SCALE, offset);
	if (!(fabsf(offset->i) <= VR_AVATAR_OFFSET_MAXIMUM && fabsf(offset->j) <= VR_AVATAR_OFFSET_MAXIMUM &&
		fabsf(offset->k) <= VR_AVATAR_OFFSET_MAXIMUM))
	{
		return FALSE;
	}
	for (index = 0; index < 4; index++)
	{
		rotation[index] = (real)part->rotation[index] / 32767.0f;
		length += rotation[index] * rotation[index];
	}
	if (length < 0.5f)
		return FALSE;
	length = (real)sqrt(length);
	for (index = 0; index < 4; index++)
		rotation[index] /= length;
	return TRUE;
}

/* what is said of a player's head and hands, kept (checked: a client's of
its own) */
static void network_vr_avatar_take(
	struct distributed_vr_avatar const *entry)
{
	struct network_vr_avatar *avatar;
	struct vr_avatar_sample sample;
	short part;

	if (entry->player_index >= MAXIMUM_TRACKED_PLAYERS)
		return;
	csmemset(&sample, 0, sizeof(sample));
	sample.received_ms = system_milliseconds();
	for (part = 0; part < NUMBER_OF_VR_AVATAR_PARTS; part++)
	{
		if ((entry->known & (1 << part)) &&
			network_vr_avatar_unpack(&entry->parts[part], &sample.offsets[part], sample.rotations[part]))
		{
			sample.known |= (byte)(1 << part);
		}
	}
	avatar = &network_vr_avatars[entry->player_index];
	avatar->previous_valid = avatar->valid && game_time_get() - avatar->received_time <= VR_AVATAR_FRESH_TICKS;
	avatar->previous = avatar->latest;
	avatar->latest = sample;
	avatar->entry = *entry;
	avatar->valid = TRUE;
	avatar->received_time = game_time_get();
}

/* this machine's VR player's head and hands, to send; FALSE when it has none */
static boolean network_vr_avatar_local(
	short local_player_index,
	struct distributed_vr_avatar *entry)
{
	long player_index = local_player_get_player_index(local_player_index);
	struct player_datum *player = player_index != NONE ? player_try_and_get(player_index) : NULL;
	struct halo_vr_avatar avatar;

	if (!player || player->unit_index == NONE || !unit_try_and_get(player->unit_index) ||
		!halo_vr_avatar(local_player_index, &avatar))
	{
		return FALSE;
	}
	csmemset(entry, 0, sizeof(*entry));
	entry->player_index = distributed_player_to_byte(player_index);
	/* (HALO_VR_AVATAR_* are 1 << the parts here) */
	entry->known = (byte)(avatar.known & 7);
	network_vr_avatar_pack(avatar.head, avatar.head_rotation, &entry->parts[_vr_avatar_head]);
	network_vr_avatar_pack(avatar.weapon_hand, avatar.weapon_hand_rotation, &entry->parts[_vr_avatar_weapon_hand]);
	network_vr_avatar_pack(avatar.free_hand, avatar.free_hand_rotation, &entry->parts[_vr_avatar_free_hand]);
	return TRUE;
}

/* a rotation's frame: forward, left, up */
static void network_vr_avatar_axes(
	real const q[4],
	real_vector3d *forward,
	real_vector3d *left,
	real_vector3d *up)
{
	real x = q[0], y = q[1], z = q[2], w = q[3];

	forward->i = 1.0f - 2.0f * (y * y + z * z);
	forward->j = 2.0f * (x * y + z * w);
	forward->k = 2.0f * (x * z - y * w);
	left->i = 2.0f * (x * y - z * w);
	left->j = 1.0f - 2.0f * (x * x + z * z);
	left->k = 2.0f * (y * z + x * w);
	up->i = 2.0f * (x * z + y * w);
	up->j = 2.0f * (y * z - x * w);
	up->k = 1.0f - 2.0f * (x * x + y * y);
}

/* ---------- public code */

void network_vr_avatars_new_game(
	void)
{
	csmemset(network_vr_avatars, 0, sizeof(network_vr_avatars));
}

word network_vr_avatars_entry_size(
	void)
{
	return sizeof(struct distributed_vr_avatar);
}

/* after each tick (network_vr_tick): a client its own VR players' heads and
hands to the host; the host its own and its clients' to each client */
void network_vr_avatars_tick(
	void)
{
	short connection = game_connection();
	short local_player_index;
	struct
	{
		struct distributed_message_header header;
		struct distributed_vr_avatar avatars[MAXIMUM_TRACKED_PLAYERS];
	} message;
	short count = 0;

	if (connection == _game_connection_network_client)
	{
		for (local_player_index = 0; local_player_index < MAXIMUM_LOCAL_PLAYERS; local_player_index++)
		{
			if (network_vr_avatar_local(local_player_index, &message.avatars[count]))
				count++;
		}
		if (count)
		{
			distributed_send(&message, _distributed_message_vr_avatars, count,
				(word)(sizeof(message.header) + count * sizeof(struct distributed_vr_avatar)), _distributed_to_host);
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
			struct distributed_vr_avatar entry;

			if (network_vr_avatar_local(local_player_index, &entry))
				network_vr_avatar_take(&entry);
		}
		machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		for (machine_number = 0; machine_number < machine_count; machine_number++)
		{
			long machine_index = machine_indices[machine_number];

			count = 0;
			for (player_index = 0; player_index < MAXIMUM_TRACKED_PLAYERS && count < 255; player_index++)
			{
				struct network_vr_avatar const *avatar = &network_vr_avatars[player_index];

				if (avatar->valid && game_time_get() - avatar->received_time <= VR_AVATAR_FRESH_TICKS &&
					game_time_get() >= avatar->received_time &&
					distributed_player(player_index) &&
					!distributed_machine_has_player(machine_index, player_index))
				{
					message.avatars[count++] = avatar->entry;
				}
			}
			if (count)
			{
				distributed_send_to_machine(machine_index, &message, _distributed_message_vr_avatars, count,
					(word)(sizeof(message.header) + count * sizeof(struct distributed_vr_avatar)));
			}
		}
	}
}

/* the heads and hands a machine sent: the host's of a client's players, a
client's of the others */
void network_vr_avatars_handle(
	long machine_index,
	void const *entries,
	short count)
{
	short index;

	for (index = 0; index < count; index++)
	{
		struct distributed_vr_avatar entry;

		csmemcpy(&entry, (byte const *)entries + index * sizeof(entry), sizeof(entry));
		if (entry.player_index >= MAXIMUM_TRACKED_PLAYERS || !distributed_player(entry.player_index))
			continue;
		if (machine_index != NONE)
		{
			/* (the host: only of that client's own players) */
			if (!distributed_machine_has_player(machine_index, entry.player_index))
				continue;
		}
		else if (distributed_player_is_local(distributed_player_from_byte(entry.player_index)))
		{
			continue;
		}
		network_vr_avatar_take(&entry);
	}
}

boolean network_vr_avatar(
	long player_index,
	long unit_index,
	struct vr_avatar_world *world)
{
	struct network_vr_avatar *avatar;
	struct vr_avatar_sample const *latest, *previous;
	long absolute_index;
	unsigned long now, shown;
	real blend = 1.0f;
	real_point3d camera;
	real_point3d *points[NUMBER_OF_VR_AVATAR_PARTS];
	real_vector3d *axes[NUMBER_OF_VR_AVATAR_PARTS][3];
	short part;

	csmemset(world, 0, sizeof(*world));
	if (player_index == NONE || unit_index == NONE || !network_vr_avatars_in_network_game() ||
		distributed_player_is_local(player_index))
	{
		return FALSE;
	}
	absolute_index = DATUM_INDEX_TO_ABSOLUTE_INDEX(player_index);
	if (absolute_index < 0 || absolute_index >= MAXIMUM_TRACKED_PLAYERS)
		return FALSE;
	avatar = &network_vr_avatars[absolute_index];
	if (!avatar->valid || game_time_get() < avatar->received_time ||
		game_time_get() - avatar->received_time > VR_AVATAR_FRESH_TICKS)
	{
		return FALSE;
	}
	latest = &avatar->latest;
	previous = avatar->previous_valid ? &avatar->previous : latest;
	/* (between the two last as of a little before now) */
	now = system_milliseconds();
	shown = now - VR_AVATAR_DELAY_MS;
	if (previous != latest && latest->received_ms != previous->received_ms)
	{
		long span = (long)(latest->received_ms - previous->received_ms);
		long into = (long)(shown - previous->received_ms);

		blend = span > 0 ? PIN((real)into / (real)span, 0.0f, 1.0f) : 1.0f;
	}
	unit_get_camera_position(unit_index, &camera);
	points[_vr_avatar_head] = &world->head;
	points[_vr_avatar_weapon_hand] = &world->weapon_hand;
	points[_vr_avatar_free_hand] = &world->free_hand;
	axes[_vr_avatar_head][0] = &world->head_forward;
	axes[_vr_avatar_head][1] = &world->head_left;
	axes[_vr_avatar_head][2] = &world->head_up;
	axes[_vr_avatar_weapon_hand][0] = &world->weapon_forward;
	axes[_vr_avatar_weapon_hand][1] = &world->weapon_left;
	axes[_vr_avatar_weapon_hand][2] = &world->weapon_up;
	axes[_vr_avatar_free_hand][0] = &world->free_forward;
	axes[_vr_avatar_free_hand][1] = &world->free_left;
	axes[_vr_avatar_free_hand][2] = &world->free_up;
	for (part = 0; part < NUMBER_OF_VR_AVATAR_PARTS; part++)
	{
		real_vector3d offset = latest->offsets[part];
		real rotation[4];
		real dot = 0.0f, length = 0.0f;
		short index;

		if (!(latest->known & (1 << part)))
			continue;
		for (index = 0; index < 4; index++)
			rotation[index] = latest->rotations[part][index];
		if ((previous->known & (1 << part)) && blend < 1.0f)
		{
			/* (eased: the offsets along a line, the rotations the short way) */
			offset.i = previous->offsets[part].i + (offset.i - previous->offsets[part].i) * blend;
			offset.j = previous->offsets[part].j + (offset.j - previous->offsets[part].j) * blend;
			offset.k = previous->offsets[part].k + (offset.k - previous->offsets[part].k) * blend;
			for (index = 0; index < 4; index++)
				dot += previous->rotations[part][index] * rotation[index];
			for (index = 0; index < 4; index++)
			{
				real from = dot < 0.0f ? -previous->rotations[part][index] : previous->rotations[part][index];

				rotation[index] = from + (rotation[index] - from) * blend;
				length += rotation[index] * rotation[index];
			}
			length = length > 1e-8f ? (real)sqrt(length) : 1.0f;
			for (index = 0; index < 4; index++)
				rotation[index] /= length;
		}
		points[part]->x = camera.x + offset.i;
		points[part]->y = camera.y + offset.j;
		points[part]->z = camera.z + offset.k;
		network_vr_avatar_axes(rotation, axes[part][0], axes[part][1], axes[part][2]);
		world->known |= 1 << part;
	}
	return world->known != 0;
}
