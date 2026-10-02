/*
NETWORK_VOICE.C

The voice chat (the VR port's): an extension of the distributed netcode
(port/linux/NETCODE.md, network_distributed.c), as the VR players' hands
are (network_vr.c).

- Every tick, a machine sends what its microphone heard of its player's
  speech (halo_voice.h: Opus packets of 20 ms, only while they speak) in
  _distributed_message_voice (unreliable): a client to the host, which
  plays it and passes it on to its other clients; the host its own to
  every client.
- A machine plays each speaker's voice from where they are: a teammate
  (in a game of teams) over the radio, as loud anywhere and only a little
  to the side they are on; anyone else as near as they are (no further off
  than VOICE_RANGE), from their side. A speaker with no unit (dead) is
  heard over the radio by their team, and not by the rest.

A machine that does not know the message (a build without this, of the same
network version) drops it, as it drops any type past its own.
*/

#include "cseries.h"
#include "game/game.h"
#include "game/game_engine.h"
#include "game/players.h"
#include "units/units.h"
#include "network_distributed.h"
#include "halo_voice.h"
#include "halo_vr.h"

#include <math.h>

void platform_log(char const *format, ...);

/* ---------- constants */

enum
{
	/* an entry: the speaker, its sequence number and the packet's length,
	and the packet */
	VOICE_ENTRY_HEADER = 4,
	VOICE_MAXIMUM_PACKET = 200,
	VOICE_MAXIMUM_TAKEN = 1024,
};

/* how far off someone not on the radio is heard (world units: 60 metres) */
#define VOICE_RANGE 20.0f
/* how far to the side a voice is put: on the radio, and near */
#define VOICE_RADIO_PAN 0.3f
#define VOICE_NEAR_PAN 0.8f

/* ---------- globals */

static struct
{
	boolean active;
	word sequence;
	long sent;
	long played;
	long log_time;
} network_voice;

/* ---------- private code */

static boolean network_voice_in_network_game(
	void)
{
	short connection = game_connection();

	return (connection == _game_connection_network_server || connection == _game_connection_network_client) &&
		game_in_progress();
}

/* where a player's head is, and whether they have a unit */
static boolean network_voice_player_head(
	struct player_datum const *player,
	real_point3d *head)
{
	long unit_index = distributed_living_unit(player);

	if (unit_index == NONE)
		return FALSE;
	unit_get_camera_position(unit_index, head);
	return TRUE;
}

/* the gains a speaker is heard at here (FALSE: not heard) */
static boolean network_voice_gains(
	struct player_datum const *speaker,
	real *left,
	real *right)
{
	long listener_index = local_player_get_player_index(0);
	struct player_datum *listener = listener_index != NONE ? player_try_and_get(listener_index) : NULL;
	boolean teammate = listener && game_engine_has_teams() && listener->team_index == speaker->team_index;
	real_point3d speaker_head;
	real_point3d listener_head;
	real gain = 1.0f;
	real pan = 0.0f;

	if (!listener)
		return FALSE;
	if (!network_voice_player_head(speaker, &speaker_head))
	{
		/* (dead: on the radio, to the team) */
		if (!teammate)
			return FALSE;
		*left = *right = 1.0f;
		return TRUE;
	}
	if (network_voice_player_head(listener, &listener_head))
	{
		real dx = speaker_head.x - listener_head.x;
		real dy = speaker_head.y - listener_head.y;
		real dz = speaker_head.z - listener_head.z;
		real distance = (real)sqrt(dx * dx + dy * dy + dz * dz);
		real flat = (real)sqrt(dx * dx + dy * dy);
		float look[3];
		real forward_x;
		real forward_y;
		real length;
		struct unit_datum *unit = unit_try_and_get(distributed_living_unit(listener));

		/* (where the listener looks: the head, in VR) */
		if (halo_vr_look_vector(0, look))
		{
			forward_x = look[0];
			forward_y = look[1];
		}
		else
		{
			forward_x = unit ? unit->unit.looking_vector.i : 1.0f;
			forward_y = unit ? unit->unit.looking_vector.j : 0.0f;
		}
		length = (real)sqrt(forward_x * forward_x + forward_y * forward_y);
		if (length > 0.0001f && flat > 0.0001f)
		{
			/* (the right: forward turned clockwise) */
			pan = (dx * forward_y - dy * forward_x) / (flat * length);
		}
		if (!teammate)
		{
			gain = 1.0f - distance / VOICE_RANGE;
			if (gain <= 0.0f)
				return FALSE;
			gain *= gain;
		}
		pan *= teammate ? VOICE_RADIO_PAN : VOICE_NEAR_PAN;
	}
	else if (!teammate)
	{
		/* (the listener dead: the others as from where the body is not:
		not at all) */
		return FALSE;
	}
	*left = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
	*right = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
	return TRUE;
}

static void network_voice_play_entries(
	byte const *entries,
	byte const *end,
	short count,
	long machine_index)
{
	short index;

	for (index = 0; index < count && entries + VOICE_ENTRY_HEADER <= end; index++)
	{
		byte player_byte = entries[0];
		word sequence = (word)(entries[1] | entries[2] << 8);
		short length = entries[3];
		long player_index;
		struct player_datum *player;
		real left;
		real right;

		if (length <= 0 || entries + VOICE_ENTRY_HEADER + length > end)
			break;
		player_index = distributed_player_from_byte(player_byte);
		player = player_index != NONE ? player_try_and_get(player_index) : NULL;
		/* (the host: only a client's own players') */
		if (player && (machine_index == NONE || distributed_machine_has_player(machine_index, player_byte)) &&
			player->local_player_index == NONE && network_voice_gains(player, &left, &right))
		{
			halo_voice_play(player_byte, sequence, entries + VOICE_ENTRY_HEADER, length, left, right);
			network_voice.played++;
		}
		entries += VOICE_ENTRY_HEADER + length;
	}
}

/* ---------- public code */

void network_voice_new_game(
	void)
{
	network_voice.sequence = 0;
}

word network_voice_entry_size(
	void)
{
	return VOICE_ENTRY_HEADER;
}

/* after each tick (network_distributed_tick): this machine's speech sent */
void network_voice_tick(
	void)
{
	struct
	{
		struct distributed_message_header header;
		byte entries[VOICE_MAXIMUM_TAKEN + 64];
	} message;
	byte taken[VOICE_MAXIMUM_TAKEN];
	long player_index = local_player_get_player_index(0);
	long size;
	long offset = 0;
	long used = 0;
	short count = 0;

	if (!network_voice_in_network_game() || !halo_voice_enabled())
	{
		if (network_voice.active)
		{
			network_voice.active = FALSE;
			halo_voice_end();
		}
		return;
	}
	if (!network_voice.active)
	{
		network_voice.active = TRUE;
		halo_voice_begin();
	}
	if (game_time_get() - network_voice.log_time >= 150 && (network_voice.sent || network_voice.played))
	{
		platform_log("voice: sent %ld, played %ld packets (in 150 ticks)", network_voice.sent, network_voice.played);
		network_voice.sent = network_voice.played = 0;
		network_voice.log_time = game_time_get();
	}
	size = halo_voice_take(taken, sizeof(taken));
	if (player_index == NONE)
		return;
	while (offset < size)
	{
		short length = taken[offset];

		if (length <= 0 || offset + 1 + length > size || length > VOICE_MAXIMUM_PACKET)
			break;
		if (used + VOICE_ENTRY_HEADER + length > (long)sizeof(message.entries))
			break;
		message.entries[used] = distributed_player_to_byte(player_index);
		message.entries[used + 1] = (byte)(network_voice.sequence & 0xFF);
		message.entries[used + 2] = (byte)(network_voice.sequence >> 8);
		message.entries[used + 3] = (byte)length;
		csmemcpy(message.entries + used + VOICE_ENTRY_HEADER, taken + offset + 1, length);
		used += VOICE_ENTRY_HEADER + length;
		offset += 1 + length;
		network_voice.sequence++;
		count++;
	}
	if (!count)
		return;
	network_voice.sent += count;
	distributed_send(&message, _distributed_message_voice, count, (word)(sizeof(message.header) + used),
		game_connection() == _game_connection_network_server ? _distributed_to_clients : _distributed_to_host);
}

/* a machine's speech: on the host a client's, played and passed on to the
other clients; on a client the host's of any other */
void network_voice_handle(
	long machine_index,
	void const *message,
	word size)
{
	struct distributed_message_header header;
	byte const *entries = (byte const *)message + sizeof(header);
	byte const *end = (byte const *)message + size;

	if (!halo_voice_enabled() || !network_voice.active)
		return;
	csmemcpy(&header, message, sizeof(header));
	network_voice_play_entries(entries, end, header.count, machine_index);
	if (machine_index != NONE)
	{
		long machine_indices[HALO_PORT_MAXIMUM_NETWORK_MACHINES];
		short machine_count = distributed_client_machines(machine_indices, HALO_PORT_MAXIMUM_NETWORK_MACHINES);
		short number;
		struct
		{
			struct distributed_message_header header;
			byte entries[VOICE_MAXIMUM_TAKEN + 64];
		} relayed;
		word entries_size = (word)(end - entries);

		byte const *entry = entries;
		short index;

		/* (only the sender's own players' speech is passed on, whole) */
		if (entries_size < VOICE_ENTRY_HEADER || entries_size > sizeof(relayed.entries))
			return;
		for (index = 0; index < header.count; index++)
		{
			if (entry + VOICE_ENTRY_HEADER > end || entry[3] == 0 || entry + VOICE_ENTRY_HEADER + entry[3] > end ||
				!distributed_machine_has_player(machine_index, entry[0]))
			{
				return;
			}
			entry += VOICE_ENTRY_HEADER + entry[3];
		}
		if (entry != end)
			return;
		for (number = 0; number < machine_count; number++)
		{
			if (machine_indices[number] == machine_index)
				continue;
			csmemcpy(relayed.entries, entries, entries_size);
			distributed_send_to_machine(machine_indices[number], &relayed, _distributed_message_voice, header.count,
				(word)(sizeof(relayed.header) + entries_size));
		}
	}
}
