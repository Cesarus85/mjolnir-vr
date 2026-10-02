/*
VOICE.C

The voice chat's sound (halo_voice.h): on Android, the host's
(port/android/host/host_voice.c); elsewhere none.
*/

#include "halo_voice.h"
#include "platform.h"
#include "port_config.h"

#ifdef HALO_ANDROID
#include "guest_host.h"

#include <stdio.h>

static int voice_started;

/* whether the app may use the microphone, as the launcher found
(LauncherActivity.java): opened without it, Android would ask in the middle
of the game */
static int voice_microphone_allowed(void)
{
	char path[512];
	FILE *file;
	int allowed = 0;

	snprintf(path, sizeof(path), "%s/microphone_permission.txt", platform_data_root());
	file = fopen(path, "rb");
	if (file)
	{
		allowed = fgetc(file) == '1';
		fclose(file);
	}
	return allowed;
}

int halo_voice_enabled(void)
{
	return config_boolean("voice.enabled");
}

int halo_voice_begin(void)
{
	int microphone;

	if (voice_started)
		return 1;
	voice_started = 1;
	microphone = config_boolean("voice.microphone") && voice_microphone_allowed();
	platform_log("voice: on, %s", microphone ? "with the microphone" :
		config_boolean("voice.microphone") ? "without the microphone (not allowed)" : "listening only");
	return host_voice_start(microphone, (float)config_real("voice.volume"), (float)config_real("voice.threshold"));
}

void halo_voice_end(void)
{
	if (!voice_started)
		return;
	voice_started = 0;
	host_voice_stop();
	platform_log("voice: off");
}

int halo_voice_take(void *buffer, int size)
{
	return voice_started ? host_voice_take_packets(buffer, size) : 0;
}

void halo_voice_play(int speaker, int sequence, const void *data, int length, float left, float right)
{
	if (voice_started)
		host_voice_play(speaker, sequence, data, length, left, right);
}

#else

int halo_voice_enabled(void) { return 0; }
int halo_voice_begin(void) { return 0; }
void halo_voice_end(void) {}
int halo_voice_take(void *buffer, int size) { (void)buffer; (void)size; return 0; }
void halo_voice_play(int speaker, int sequence, const void *data, int length, float left, float right)
{
	(void)speaker; (void)sequence; (void)data; (void)length; (void)left; (void)right;
}

#endif
