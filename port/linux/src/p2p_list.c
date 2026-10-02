/*
P2P_LIST.C

Public games (the VR port's): a host may list its game for anyone, who
finds it in a list of online games and joins it by its join code
(p2p_code.c), without a server of this project's.

- A public game's host announces it through the signalling brokers
  (p2p_signal.c) every ANNOUNCE_INTERVAL, retained, on a topic of its own
  under hcvr/1/g<network version>/: its name, map, game type, players, its
  join code, and the time. Retained, a machine that starts listening gets
  every game's latest at once. A host that stops (it closes its game, or
  makes it private) clears its topic; one that quits without that leaves
  its last announcement, which listeners drop once it is older than
  STALE_SECONDS (by the machines' clocks).
- A machine that shows the list listens on the topics of its network
  version (a wildcard), and keeps the games heard of lately.
- An announcement carries no address: a machine reaches a public game's
  host only by its code, as anyone given the code does. Anyone can announce
  anything, so what is shown is checked to be printable and in bounds, and
  a game that is not what it says is only a code that does not answer.
*/

#include "platform.h"
#include "posix.h"
#include "port_config.h"
#include "p2p_internal.h"
#include "halo_port_limits.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

enum
{
	ANNOUNCEMENT_VERSION = 1,
	/* milliseconds */
	ANNOUNCE_INTERVAL = 10000,
	/* a listener's interest lapses this long after the menu last asked */
	WATCH_TIME = 3000,
	STALE_SECONDS = 45,
	MAXIMUM_LISTED_GAMES = 32,
};

/* what goes to the brokers (byte for byte) */
struct announcement
{
	char magic[4];
	unsigned char version;
	unsigned char players;
	unsigned char maximum_players;
	unsigned char flags;
	unsigned char time[8];
	char code[8];
	char name[P2P_PUBLIC_NAME_SIZE];
	char map[P2P_PUBLIC_MAP_SIZE];
	char mode[P2P_PUBLIC_MODE_SIZE];
};

struct listed_game
{
	int used;
	char topic[P2P_CODE_TOPIC_SIZE];
	unsigned long heard_time;
	long long announced_time;
	struct p2p_public_game game;
};

static struct
{
	/* hosting (whether public: network.public_game, until Y changes it) */
	int initialized;
	int public_game;
	char topic[P2P_CODE_TOPIC_SIZE];
	int announced;
	unsigned long announced_time;
	char name[P2P_PUBLIC_NAME_SIZE];
	char map[P2P_PUBLIC_MAP_SIZE];
	char mode[P2P_PUBLIC_MODE_SIZE];
	int players;
	int maximum_players;

	/* listening */
	unsigned long watched_time;
	int watching;
	char watch_topic[P2P_CODE_TOPIC_SIZE];
	struct listed_game games[MAXIMUM_LISTED_GAMES];
} list;

static int list_elapsed(unsigned long since, unsigned long time)
{
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

/* text as shown: printable ASCII only, ended */
static void list_text(char *destination, int size, const char *source, int source_size)
{
	int index;

	for (index = 0; index < size - 1 && index < source_size && source[index]; index++)
		destination[index] = source[index] >= 32 && source[index] < 127 ? source[index] : '?';
	destination[index] = 0;
}

static long long list_time_now(void)
{
	return (long long)time(NULL);
}

static void list_topics_make(void)
{
	if (!list.topic[0])
	{
		unsigned char random[8];
		char hex[17];

		posix_random_bytes(random, sizeof(random));
		p2p_hex(random, sizeof(random), hex);
		snprintf(list.topic, sizeof(list.topic), "hcvr/1/g%d/%s", HALO_PORT_NETWORK_VERSION, hex);
	}
	if (!list.watch_topic[0])
		snprintf(list.watch_topic, sizeof(list.watch_topic), "hcvr/1/g%d/+", HALO_PORT_NETWORK_VERSION);
}

static void list_announce(void)
{
	struct announcement announcement;
	char code[16];
	long long now = list_time_now();
	int index;

	if (!p2p_code_hosted(code, sizeof(code)))
		return;
	memset(&announcement, 0, sizeof(announcement));
	memcpy(announcement.magic, "HCVR", 4);
	announcement.version = ANNOUNCEMENT_VERSION;
	announcement.players = (unsigned char)(list.players < 0 ? 0 : list.players > 255 ? 255 : list.players);
	announcement.maximum_players =
		(unsigned char)(list.maximum_players < 0 ? 0 : list.maximum_players > 255 ? 255 : list.maximum_players);
	for (index = 0; index < 8; index++)
		announcement.time[index] = (unsigned char)(now >> (8 * index));
	/* (the code without its dash) */
	memcpy(announcement.code, code, 4);
	memcpy(announcement.code + 4, code + 5, 4);
	memcpy(announcement.name, list.name, sizeof(announcement.name));
	memcpy(announcement.map, list.map, sizeof(announcement.map));
	memcpy(announcement.mode, list.mode, sizeof(announcement.mode));
	p2p_signal_publish_retained(list.topic, (const unsigned char *)&announcement, sizeof(announcement));
	if (!list.announced)
		platform_log("Internet play: the game is listed as public (%s, %s, %s)", list.name, list.map, list.mode);
	list.announced = 1;
	list.announced_time = p2p_now();
}

static void list_withdraw(void)
{
	if (!list.announced)
		return;
	p2p_signal_publish_retained(list.topic, NULL, 0);
	platform_log("Internet play: the game is no longer listed as public");
	list.announced = 0;
}

/* ---------- p2p.c's side (under p2p_lock) */

void p2p_list_set_public(int public_game)
{
	if (public_game == list.public_game)
		return;
	list.public_game = public_game;
	list_topics_make();
	if (!public_game)
		list_withdraw();
	else
		list.announced_time = p2p_now() - ANNOUNCE_INTERVAL;
}

int p2p_list_public(void)
{
	return list.public_game;
}

void p2p_list_set_description(const char *name, const char *map, const char *mode, int players,
	int maximum_players)
{
	int changed = strncmp(list.name, name, sizeof(list.name) - 1) || strncmp(list.map, map, sizeof(list.map) - 1) ||
		strncmp(list.mode, mode, sizeof(list.mode) - 1) || players != list.players ||
		maximum_players != list.maximum_players;

	list_text(list.name, sizeof(list.name), name, P2P_PUBLIC_NAME_SIZE);
	list_text(list.map, sizeof(list.map), map, P2P_PUBLIC_MAP_SIZE);
	list_text(list.mode, sizeof(list.mode), mode, P2P_PUBLIC_MODE_SIZE);
	list.players = players;
	list.maximum_players = maximum_players;
	/* (a change goes out soon, not at the next interval) */
	if (changed && list.announced && list_elapsed(list.announced_time, 1000))
		list.announced_time = p2p_now() - ANNOUNCE_INTERVAL;
}

int p2p_list_games(struct p2p_public_game *games, int maximum)
{
	int count = 0;
	int index;

	list.watched_time = p2p_now();
	list.watching = 1;
	for (index = 0; index < MAXIMUM_LISTED_GAMES && count < maximum; index++)
	{
		if (list.games[index].used)
			games[count++] = list.games[index].game;
	}
	return count;
}

/* ---------- p2p_signal.c's side (under p2p_lock) */

const char *p2p_list_topic(void)
{
	return list.watching ? list.watch_topic : "";
}

int p2p_list_received(const char *topic, const unsigned char *payload, int size)
{
	struct announcement announcement;
	struct listed_game *slot = NULL;
	long long announced = 0;
	char code[16];
	int index;

	if (!list.watch_topic[0] || strncmp(topic, list.watch_topic, strlen(list.watch_topic) - 1))
		return 0;
	/* (this machine's own, as the broker echoes it) */
	if (!strcmp(topic, list.topic))
		return 1;
	for (index = 0; index < MAXIMUM_LISTED_GAMES; index++)
	{
		if (list.games[index].used && !strcmp(list.games[index].topic, topic))
			slot = &list.games[index];
	}
	if (size != (int)sizeof(announcement))
	{
		/* (empty: withdrawn) */
		if (slot && size == 0)
			slot->used = 0;
		return 1;
	}
	memcpy(&announcement, payload, sizeof(announcement));
	if (memcmp(announcement.magic, "HCVR", 4) || announcement.version != ANNOUNCEMENT_VERSION ||
		!announcement.maximum_players || announcement.players > announcement.maximum_players)
	{
		return 1;
	}
	for (index = 0; index < 8; index++)
		announced |= (long long)announcement.time[index] << (8 * index);
	if (announced < list_time_now() - STALE_SECONDS || announced > list_time_now() + STALE_SECONDS)
	{
		if (slot)
			slot->used = 0;
		return 1;
	}
	snprintf(code, sizeof(code), "%.4s-%.4s", announcement.code, announcement.code + 4);
	for (index = 0; code[index]; index++)
	{
		if (index != 4 && !((code[index] >= '0' && code[index] <= '9') || (code[index] >= 'A' && code[index] <= 'Z')))
			return 1;
	}
	if (!slot)
	{
		int oldest = -1;

		for (index = 0; index < MAXIMUM_LISTED_GAMES && !slot; index++)
		{
			if (!list.games[index].used)
				slot = &list.games[index];
			else if (oldest < 0 || list.games[index].announced_time < list.games[oldest].announced_time)
				oldest = index;
		}
		if (!slot)
			slot = &list.games[oldest];
		memset(slot, 0, sizeof(*slot));
		snprintf(slot->topic, sizeof(slot->topic), "%s", topic);
	}
	slot->used = 1;
	slot->heard_time = p2p_now();
	slot->announced_time = announced;
	list_text(slot->game.name, sizeof(slot->game.name), announcement.name, sizeof(announcement.name));
	list_text(slot->game.map, sizeof(slot->game.map), announcement.map, sizeof(announcement.map));
	list_text(slot->game.mode, sizeof(slot->game.mode), announcement.mode, sizeof(announcement.mode));
	snprintf(slot->game.code, sizeof(slot->game.code), "%s", code);
	slot->game.players = announcement.players;
	slot->game.maximum_players = announcement.maximum_players;
	return 1;
}

void p2p_list_update(int hosting)
{
	int index;

	if (!list.initialized)
	{
		list.initialized = 1;
		if (!list.public_game)
			list.public_game = config_boolean("network.public_game");
	}
	list_topics_make();
	/* hosting a public game: announced every so often; withdrawn when it
	is not */
	if (hosting && list.public_game)
	{
		p2p_signal_start();
		if (p2p_signal_connected() && list_elapsed(list.announced_time, ANNOUNCE_INTERVAL))
			list_announce();
	}
	else if (list.announced)
	{
		list_withdraw();
	}
	/* listening while the menu asks */
	if (list.watching && list_elapsed(list.watched_time, WATCH_TIME))
	{
		list.watching = 0;
		memset(list.games, 0, sizeof(list.games));
		p2p_signal_sync_topics();
	}
	else if (list.watching)
	{
		p2p_signal_start();
		p2p_signal_sync_topics();
	}
	for (index = 0; index < MAXIMUM_LISTED_GAMES; index++)
	{
		if (list.games[index].used && list.games[index].announced_time < list_time_now() - STALE_SECONDS)
			list.games[index].used = 0;
	}
}
