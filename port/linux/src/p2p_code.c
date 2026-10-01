/*
P2P_CODE.C

Join codes (the VR port's): a short code that stands for a hosted game's
invite (p2p.c), for a player who cannot pass a link on, as in a headset,
where a link is not copied out and 64 hex digits are not typed in.

- A host makes a code when it starts hosting: 8 characters of Crockford's
  base 32 (40 bits), shown as XXXX-XXXX (ui_join_code.c draws it in the
  lobby). It lasts while the host hosts.
- From the code come a topic and a key (PBKDF2-HMAC-SHA256, slow on
  purpose: CODE_KDF_ITERATIONS). The host listens on the topic through the
  signalling brokers (p2p_signal.c). A player who types the code asks
  there (ASK: a nonce), and the host answers with its invite, sealed with
  the key, with the nonce (INVITE); the asker opens it and joins as an
  invite link would.
- The brokers, and anyone who watches them, see the topic, but cannot tell
  the code from it, or open the answer, short of working the slow
  derivation out for codes until one fits; a code is guessed at one in 2^40
  of them. An asker who has the code has the invite, as one given the link
  has: it joins as anyone invited does.

A build without this (another of the same network version) has no codes,
and its players join with the link (which the host still logs and copies).
*/

#include "platform.h"
#include "posix.h"
#include "p2p_internal.h"

#include <stdio.h>
#include <string.h>

enum
{
	CODE_SYMBOLS = 8,
	CODE_KDF_ITERATIONS = 20000,
	CODE_NONCE_SIZE = 8,
	CODE_INVITE_SIZE = P2P_KEY_HASH_SIZE + P2P_TOKEN_SIZE,
	CODE_MESSAGE_VERSION = 1,
	/* milliseconds */
	ASK_INTERVAL = 1000,
	LOOK_TIMEOUT = 20000,
	/* the host's answers: this many at once, and one more each this often */
	MAXIMUM_ANSWERS = 8,
	ANSWER_INTERVAL = 250,
};

enum
{
	_code_message_ask = 'Q',
	_code_message_invite = 'I',
};

/* Crockford's base 32: no I, L, O or U */
static const char code_alphabet[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

static struct
{
	/* hosting: the code, its topic and key, and the invite it stands for */
	int hosting;
	char host_code[CODE_SYMBOLS + 1];
	char host_topic[P2P_CODE_TOPIC_SIZE];
	unsigned char host_key[P2P_SHA256_SIZE];
	unsigned char invite[CODE_INVITE_SIZE];
	int answers_left;
	unsigned long answers_time;

	/* looking a code up */
	int status;
	char look_topic[P2P_CODE_TOPIC_SIZE];
	unsigned char look_key[P2P_SHA256_SIZE];
	unsigned char look_nonce[CODE_NONCE_SIZE];
	unsigned long look_started_time;
	unsigned long asked_time;
	int asked;
} code;

static int code_elapsed(unsigned long since, unsigned long time)
{
	return (unsigned int)(p2p_now() - since) >= (unsigned int)time;
}

/* the code's symbols from what was typed: letters and digits, read as
Crockford reads them (O as 0, I and L as 1), the rest (dashes, spaces)
left out; 0 unless they are exactly a code's */
static int code_normalize(const char *text, char *symbols)
{
	int count = 0;

	for (; *text; text++)
	{
		char character = *text;

		if (character >= 'a' && character <= 'z')
			character = (char)(character - 'a' + 'A');
		if (character == 'O')
			character = '0';
		else if (character == 'I' || character == 'L')
			character = '1';
		if (!strchr(code_alphabet, character) || !character)
		{
			if (character == '-' || character == ' ' || character == '\t' || character == '\r' ||
				character == '\n')
			{
				continue;
			}
			return 0;
		}
		if (count == CODE_SYMBOLS)
			return 0;
		symbols[count++] = character;
	}
	symbols[count] = 0;
	return count == CODE_SYMBOLS;
}

/* the topic and key of a code (PBKDF2-HMAC-SHA256, one block) */
static void code_derive(const char *symbols, char *topic, unsigned char *key)
{
	static const char salt[] = "halo-ce-vr join code 1";
	unsigned char block[sizeof(salt) - 1 + 4];
	unsigned char u[P2P_SHA256_SIZE];
	unsigned char derived[P2P_SHA256_SIZE];
	unsigned char digest[P2P_SHA256_SIZE];
	char hex[2 * P2P_SHA256_SIZE + 1];
	int iteration;
	int index;

	memcpy(block, salt, sizeof(salt) - 1);
	block[sizeof(salt) - 1] = 0;
	block[sizeof(salt)] = 0;
	block[sizeof(salt) + 1] = 0;
	block[sizeof(salt) + 2] = 1;
	p2p_hmac_sha256((const unsigned char *)symbols, CODE_SYMBOLS, block, sizeof(block), u);
	memcpy(derived, u, sizeof(derived));
	for (iteration = 1; iteration < CODE_KDF_ITERATIONS; iteration++)
	{
		p2p_hmac_sha256((const unsigned char *)symbols, CODE_SYMBOLS, u, sizeof(u), u);
		for (index = 0; index < P2P_SHA256_SIZE; index++)
			derived[index] ^= u[index];
	}
	p2p_hmac_sha256(derived, sizeof(derived), "topic", 5, digest);
	p2p_hex(digest, 16, hex);
	snprintf(topic, P2P_CODE_TOPIC_SIZE, "hcvr/1/%s", hex);
	p2p_hmac_sha256(derived, sizeof(derived), "seal", 4, key);
}

/* ---------- p2p.c's side (under p2p_lock) */

void p2p_code_host(const unsigned char *invite)
{
	unsigned char random[CODE_SYMBOLS];
	int index;

	posix_random_bytes(random, sizeof(random));
	for (index = 0; index < CODE_SYMBOLS; index++)
		code.host_code[index] = code_alphabet[random[index] & 31];
	code.host_code[CODE_SYMBOLS] = 0;
	memcpy(code.invite, invite, CODE_INVITE_SIZE);
	code_derive(code.host_code, code.host_topic, code.host_key);
	code.answers_left = MAXIMUM_ANSWERS;
	code.answers_time = p2p_now();
	code.hosting = 1;
	platform_log("Internet play: join code %.4s-%.4s", code.host_code, code.host_code + 4);
	p2p_signal_sync_topics();
}

void p2p_code_stop_hosting(void)
{
	code.hosting = 0;
	code.host_code[0] = 0;
	p2p_signal_sync_topics();
}

int p2p_code_hosted(char *text, int size)
{
	if (!code.hosting || size < CODE_SYMBOLS + 2)
		return 0;
	snprintf(text, (size_t)size, "%.4s-%.4s", code.host_code, code.host_code + 4);
	return 1;
}

int p2p_code_look_up(const char *typed)
{
	char symbols[CODE_SYMBOLS + 1];

	if (!code_normalize(typed, symbols))
	{
		code.status = P2P_CODE_INVALID;
		return 0;
	}
	if (code.hosting && !strcmp(symbols, code.host_code))
	{
		code.status = P2P_CODE_OWN;
		return 0;
	}
	code_derive(symbols, code.look_topic, code.look_key);
	posix_random_bytes(code.look_nonce, sizeof(code.look_nonce));
	code.look_started_time = p2p_now();
	code.asked = 0;
	code.status = P2P_CODE_LOOKING;
	p2p_signal_sync_topics();
	return 1;
}

int p2p_code_status(void)
{
	return code.status;
}

void p2p_code_clear_status(void)
{
	if (code.status != P2P_CODE_LOOKING)
		code.status = P2P_CODE_IDLE;
}

/* ---------- p2p_signal.c's side (under p2p_lock) */

void p2p_code_topics(const char **host_topic, const char **look_topic)
{
	*host_topic = code.hosting ? code.host_topic : "";
	*look_topic = code.status == P2P_CODE_LOOKING ? code.look_topic : "";
}

static void code_answer(const unsigned char *nonce)
{
	unsigned char message[2 + CODE_NONCE_SIZE + CODE_INVITE_SIZE];
	unsigned char sealed[2 + sizeof(message) + P2P_SEAL_OVERHEAD];
	int sealed_size;

	/* (anyone watching the brokers can ask, as often as they like) */
	while (code.answers_left < MAXIMUM_ANSWERS && code_elapsed(code.answers_time, ANSWER_INTERVAL))
	{
		code.answers_left++;
		code.answers_time += ANSWER_INTERVAL;
	}
	if (code.answers_left == MAXIMUM_ANSWERS)
		code.answers_time = p2p_now();
	if (code.answers_left <= 0)
		return;
	code.answers_left--;
	message[0] = _code_message_invite;
	message[1] = CODE_MESSAGE_VERSION;
	memcpy(message + 2, nonce, CODE_NONCE_SIZE);
	memcpy(message + 2 + CODE_NONCE_SIZE, code.invite, CODE_INVITE_SIZE);
	sealed[0] = _code_message_invite;
	sealed[1] = CODE_MESSAGE_VERSION;
	sealed_size = p2p_seal(code.host_key, message, sizeof(message), sealed + 2);
	p2p_signal_publish(code.host_topic, sealed, 2 + sealed_size);
}

static void code_invite_received(const unsigned char *payload, int size)
{
	unsigned char message[2 + CODE_NONCE_SIZE + CODE_INVITE_SIZE + 16];
	int message_size;
	char hex[2 * CODE_INVITE_SIZE + 1];
	char link[P2P_LINK_SIZE];

	if (size > (int)sizeof(message) + P2P_SEAL_OVERHEAD)
		return;
	message_size = p2p_open(code.look_key, payload, size, message);
	if (message_size != 2 + CODE_NONCE_SIZE + CODE_INVITE_SIZE || message[0] != _code_message_invite ||
		message[1] != CODE_MESSAGE_VERSION || !p2p_equal(message + 2, code.look_nonce, CODE_NONCE_SIZE))
	{
		return;
	}
	code.status = P2P_CODE_FOUND;
	p2p_signal_sync_topics();
	p2p_hex(message + 2 + CODE_NONCE_SIZE, CODE_INVITE_SIZE, hex);
	snprintf(link, sizeof(link), "halo://join/%s", hex);
	platform_log("Internet play: the join code's host answered");
	p2p_invite_received(link);
}

int p2p_code_received(const char *topic, const unsigned char *payload, int size)
{
	if (size < 2 || payload[1] != CODE_MESSAGE_VERSION)
		return 0;
	if (code.hosting && !strcmp(topic, code.host_topic))
	{
		if (payload[0] == _code_message_ask && size == 2 + CODE_NONCE_SIZE)
			code_answer(payload + 2);
		return 1;
	}
	if (code.status == P2P_CODE_LOOKING && !strcmp(topic, code.look_topic))
	{
		if (payload[0] == _code_message_invite)
			code_invite_received(payload + 2, size - 2);
		return 1;
	}
	return 0;
}

void p2p_code_update(void)
{
	unsigned char message[2 + CODE_NONCE_SIZE];

	if (code.status != P2P_CODE_LOOKING)
		return;
	p2p_signal_start();
	if (code_elapsed(code.look_started_time, LOOK_TIMEOUT))
	{
		code.status = P2P_CODE_NOT_FOUND;
		p2p_signal_sync_topics();
		platform_log("Internet play: no game answered the join code");
		return;
	}
	if (!p2p_signal_connected() || (code.asked && !code_elapsed(code.asked_time, ASK_INTERVAL)))
		return;
	message[0] = _code_message_ask;
	message[1] = CODE_MESSAGE_VERSION;
	memcpy(message + 2, code.look_nonce, CODE_NONCE_SIZE);
	p2p_signal_publish(code.look_topic, message, sizeof(message));
	code.asked = 1;
	code.asked_time = p2p_now();
}
