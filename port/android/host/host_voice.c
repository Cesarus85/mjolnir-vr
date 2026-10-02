/*
HOST_VOICE.C

The voice chat's sound (the VR port's; port/linux/game/network_voice.c
sends and receives it): the microphone, Opus, and the voices played. It
runs here, natively, where SDL's audio threads may call it.

- The microphone: SDL's default recording device, mono at 16 kHz, with
  Android's voice communication preset (its echo cancelling and noise
  suppression: SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET). Every 20 ms of it
  that is loud enough to be speech (or follows speech closely, VAD_HANGOVER)
  is encoded (Opus, VOIP, 16 kbit/s) for the guest to send.
- The voices: each speaker's packets, decoded as they come (a gap in their
  numbers concealed by Opus's packet loss concealment), wait in the
  speaker's buffer until JITTER_START of them is there, and are mixed, each
  at the gains its latest packet was given (left and right: where the
  speaker is), into a stream of their own.

The devices are opened and closed on a thread of their own (voice_worker):
opening a microphone can take a long while (an emulator's took a minute),
which the game, whose clock the other machines follow, must not wait for.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <opus.h>
#include <pthread.h>
#include <string.h>

enum
{
	VOICE_RATE = 16000,
	FRAME_SAMPLES = VOICE_RATE / 50,
	MAXIMUM_PACKET = 200,
	MAXIMUM_SPEAKERS = 32,
	/* a speaker's buffer: two seconds */
	SPEAKER_BUFFER = VOICE_RATE * 2,
	/* played from once this much is there, after silence */
	JITTER_START = FRAME_SAMPLES * 3,
	/* more than this behind: the oldest dropped */
	JITTER_MAXIMUM = FRAME_SAMPLES * 15,
	/* frames sent on after the voice drops below the threshold */
	VAD_HANGOVER = 15,
	BITRATE = 16000,
};

struct speaker
{
	int used;
	int id;
	OpusDecoder *decoder;
	int has_sequence;
	unsigned short sequence;
	short buffer[SPEAKER_BUFFER];
	int read;
	int count;
	int playing;
	float gain_left;
	float gain_right;
	unsigned long long heard_ticks;
};

static struct
{
	SDL_Mutex *lock;
	/* the devices wanted, which the worker opens and closes to match
	(under worker_lock); the recording stream is read under it too */
	pthread_mutex_t worker_lock;
	pthread_cond_t worker_wake;
	int worker_started;
	int want_playback;
	int want_recording;
	/* the microphone */
	SDL_AudioStream *recording;
	OpusEncoder *encoder;
	short frame[FRAME_SAMPLES];
	int frame_count;
	int hangover;
	float threshold;
	int level;
	/* the voices */
	SDL_AudioStream *playback;
	struct speaker speakers[MAXIMUM_SPEAKERS];
	float volume;
} voice;

static void voice_lock(void)
{
	if (!voice.lock)
		voice.lock = SDL_CreateMutex();
	SDL_LockMutex(voice.lock);
}

static void voice_unlock(void)
{
	SDL_UnlockMutex(voice.lock);
}

/* the voices mixed, as SDL asks for them (on its audio thread) */
static void SDLCALL voice_playback_callback(void *userdata, SDL_AudioStream *stream, int additional, int total)
{
	short mixed[2 * 512];
	(void)userdata;
	(void)total;

	while (additional > 0)
	{
		int frames = additional / 4;
		int frame;
		int index;

		if (frames > 512)
			frames = 512;
		if (frames <= 0)
			frames = 1;
		memset(mixed, 0, sizeof(mixed));
		voice_lock();
		for (index = 0; index < MAXIMUM_SPEAKERS; index++)
		{
			struct speaker *speaker = &voice.speakers[index];

			if (!speaker->used)
				continue;
			if (!speaker->playing && speaker->count >= JITTER_START)
				speaker->playing = 1;
			if (!speaker->playing)
				continue;
			for (frame = 0; frame < frames && speaker->count > 0; frame++)
			{
				float sample = (float)speaker->buffer[speaker->read] * voice.volume;
				int left = mixed[2 * frame] + (int)(sample * speaker->gain_left);
				int right = mixed[2 * frame + 1] + (int)(sample * speaker->gain_right);

				mixed[2 * frame] = (short)(left > 32767 ? 32767 : left < -32768 ? -32768 : left);
				mixed[2 * frame + 1] = (short)(right > 32767 ? 32767 : right < -32768 ? -32768 : right);
				speaker->read = (speaker->read + 1) % SPEAKER_BUFFER;
				speaker->count--;
			}
			/* (run dry: wait for enough again) */
			if (!speaker->count)
				speaker->playing = 0;
		}
		voice_unlock();
		SDL_PutAudioStreamData(stream, mixed, frames * 4);
		additional -= frames * 4;
	}
}

static int voice_open_playback(void)
{
	SDL_AudioSpec spec;

	if (voice.playback)
		return 1;
	spec.format = SDL_AUDIO_S16;
	spec.channels = 2;
	spec.freq = VOICE_RATE;
	voice.playback = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, voice_playback_callback,
		NULL);
	if (!voice.playback)
	{
		host_logf(HOST_LOG_WARN, "voice: cannot open the playback stream: %s", SDL_GetError());
		return 0;
	}
	SDL_ResumeAudioStreamDevice(voice.playback);
	return 1;
}

static SDL_AudioStream *voice_open_recording(void)
{
	SDL_AudioSpec spec;
	SDL_AudioStream *recording;
	int error;

	if (!voice.encoder)
	{
		voice.encoder = opus_encoder_create(VOICE_RATE, 1, OPUS_APPLICATION_VOIP, &error);
		if (!voice.encoder)
		{
			host_logf(HOST_LOG_WARN, "voice: cannot make the encoder (%d)", error);
			return NULL;
		}
		opus_encoder_ctl(voice.encoder, OPUS_SET_BITRATE(BITRATE));
		opus_encoder_ctl(voice.encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
		opus_encoder_ctl(voice.encoder, OPUS_SET_INBAND_FEC(1));
		opus_encoder_ctl(voice.encoder, OPUS_SET_PACKET_LOSS_PERC(10));
	}
	/* Android's voice communication preset: echo cancelling, noise
	suppression (AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION) */
	SDL_SetHint(SDL_HINT_ANDROID_AAUDIO_INPUT_PRESET, "7");
	spec.format = SDL_AUDIO_S16;
	spec.channels = 1;
	spec.freq = VOICE_RATE;
	recording = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &spec, NULL, NULL);
	if (!recording)
	{
		host_logf(HOST_LOG_WARN, "voice: cannot open the microphone: %s", SDL_GetError());
		return NULL;
	}
	SDL_ResumeAudioStreamDevice(recording);
	host_logf(HOST_LOG_INFO, "voice: the microphone is open");
	return recording;
}

static void voice_close_recording(void)
{
	SDL_AudioStream *recording;

	pthread_mutex_lock(&voice.worker_lock);
	recording = voice.recording;
	voice.recording = NULL;
	voice.level = 0;
	pthread_mutex_unlock(&voice.worker_lock);
	if (!recording)
		return;
	SDL_DestroyAudioStream(recording);
	host_logf(HOST_LOG_INFO, "voice: the microphone is closed");
}

static void voice_close_playback(void)
{
	SDL_AudioStream *playback = voice.playback;

	voice.playback = NULL;
	if (playback)
		SDL_DestroyAudioStream(playback);
}

/* opens and closes the devices as wanted, for as long as the app runs */
static void *voice_worker(void *unused)
{
	(void)unused;
	pthread_mutex_lock(&voice.worker_lock);
	for (;;)
	{
		int want_playback = voice.want_playback;
		int want_recording = voice.want_recording;
		int have_playback = voice.playback != NULL;
		int have_recording = voice.recording != NULL;

		if (want_playback == have_playback && want_recording == have_recording)
		{
			pthread_cond_wait(&voice.worker_wake, &voice.worker_lock);
			continue;
		}
		pthread_mutex_unlock(&voice.worker_lock);
		if (want_recording && !have_recording)
		{
			/* (opened outside the lock, which the game reads it under) */
			SDL_AudioStream *recording = voice_open_recording();

			pthread_mutex_lock(&voice.worker_lock);
			voice.recording = recording;
			pthread_mutex_unlock(&voice.worker_lock);
			/* (a microphone that will not open is not tried again until
			wanted anew) */
			if (!recording)
			{
				pthread_mutex_lock(&voice.worker_lock);
				voice.want_recording = 0;
				pthread_mutex_unlock(&voice.worker_lock);
			}
		}
		else if (!want_recording && have_recording)
		{
			voice_close_recording();
		}
		if (want_playback && !have_playback)
		{
			if (!voice_open_playback())
			{
				pthread_mutex_lock(&voice.worker_lock);
				voice.want_playback = 0;
				pthread_mutex_unlock(&voice.worker_lock);
			}
		}
		else if (!want_playback && have_playback)
		{
			voice_close_playback();
		}
		pthread_mutex_lock(&voice.worker_lock);
	}
	return NULL;
}

static void voice_want(int playback, int recording)
{
	if (!voice.worker_started)
	{
		voice.worker_started = 1;
		pthread_mutex_init(&voice.worker_lock, NULL);
		pthread_cond_init(&voice.worker_wake, NULL);
		if (host_native_thread_create(voice_worker, NULL, 256 * 1024) != 0)
		{
			host_logf(HOST_LOG_WARN, "voice: cannot start the device thread");
			return;
		}
	}
	pthread_mutex_lock(&voice.worker_lock);
	voice.want_playback = playback;
	voice.want_recording = recording;
	pthread_cond_signal(&voice.worker_wake);
	pthread_mutex_unlock(&voice.worker_lock);
}

/* ---------- the guest's side */

int host_voice_start(int microphone, float volume, float threshold)
{
	voice_lock();
	voice.volume = volume < 0.0f ? 0.0f : volume > 4.0f ? 4.0f : volume;
	voice.threshold = threshold;
	voice_unlock();
	voice_want(1, microphone);
	return 1;
}

void host_voice_stop(void)
{
	int index;

	voice_want(0, 0);
	voice_lock();
	for (index = 0; index < MAXIMUM_SPEAKERS; index++)
	{
		if (voice.speakers[index].decoder)
			opus_decoder_destroy(voice.speakers[index].decoder);
	}
	memset(voice.speakers, 0, sizeof(voice.speakers));
	voice_unlock();
}

int host_voice_take_packets(void *buffer, int size)
{
	unsigned char *out = buffer;
	int used = 0;

	if (!voice.worker_started)
		return 0;
	pthread_mutex_lock(&voice.worker_lock);
	if (!voice.recording)
	{
		voice.frame_count = 0;
		voice.hangover = 0;
		pthread_mutex_unlock(&voice.worker_lock);
		return 0;
	}
	for (;;)
	{
		int wanted = (FRAME_SAMPLES - voice.frame_count) * (int)sizeof(short);
		int got = SDL_GetAudioStreamData(voice.recording, voice.frame + voice.frame_count, wanted);
		double energy = 0.0;
		int index;

		if (got <= 0)
			break;
		voice.frame_count += got / (int)sizeof(short);
		if (voice.frame_count < FRAME_SAMPLES)
			break;
		voice.frame_count = 0;
		for (index = 0; index < FRAME_SAMPLES; index++)
			energy += (double)voice.frame[index] * voice.frame[index];
		energy = SDL_sqrt(energy / FRAME_SAMPLES) / 32768.0;
		voice.level = (int)(energy * 1000.0);
		if (energy >= voice.threshold)
			voice.hangover = VAD_HANGOVER;
		else if (voice.hangover > 0)
			voice.hangover--;
		else
			continue;
		if (used + 1 + MAXIMUM_PACKET > size)
			continue;
		{
			int length = opus_encode(voice.encoder, voice.frame, FRAME_SAMPLES, out + used + 1, MAXIMUM_PACKET);

			if (length > 0 && length < 256)
			{
				out[used] = (unsigned char)length;
				used += 1 + length;
			}
		}
	}
	pthread_mutex_unlock(&voice.worker_lock);
	return used;
}

int host_voice_level(void)
{
	return voice.level;
}

void host_voice_play(int id, int sequence, const void *data, int length, float gain_left, float gain_right)
{
	struct speaker *speaker = NULL;
	struct speaker *oldest = NULL;
	short decoded[FRAME_SAMPLES * 6];
	int frames[4];
	int frame_count = 0;
	int index;

	if (length <= 0 || length > MAXIMUM_PACKET)
		return;
	voice_lock();
	for (index = 0; index < MAXIMUM_SPEAKERS && !speaker; index++)
	{
		if (voice.speakers[index].used && voice.speakers[index].id == id)
			speaker = &voice.speakers[index];
	}
	if (!speaker)
	{
		for (index = 0; index < MAXIMUM_SPEAKERS && !speaker; index++)
		{
			if (!voice.speakers[index].used)
				speaker = &voice.speakers[index];
			else if (!oldest || voice.speakers[index].heard_ticks < oldest->heard_ticks)
				oldest = &voice.speakers[index];
		}
		if (!speaker)
			speaker = oldest;
		if (!speaker->decoder)
		{
			int error;

			speaker->decoder = opus_decoder_create(VOICE_RATE, 1, &error);
			if (!speaker->decoder)
			{
				voice_unlock();
				return;
			}
		}
		else
		{
			opus_decoder_ctl(speaker->decoder, OPUS_RESET_STATE);
		}
		speaker->used = 1;
		speaker->id = id;
		speaker->has_sequence = 0;
		speaker->read = speaker->count = 0;
		speaker->playing = 0;
	}
	speaker->heard_ticks = SDL_GetTicks();
	speaker->gain_left = gain_left;
	speaker->gain_right = gain_right;
	{
		unsigned short number = (unsigned short)sequence;

		if (speaker->has_sequence)
		{
			short gap = (short)(number - speaker->sequence);

			/* (late, or again: dropped) */
			if (gap <= 0)
			{
				voice_unlock();
				return;
			}
			/* (lost ones: concealed, a few) */
			if (gap > 1 && gap <= 4 && speaker->count > 0)
			{
				int missing;

				for (missing = 1; missing < gap && frame_count < 3; missing++)
				{
					frames[frame_count] = opus_decode(speaker->decoder, NULL, 0, decoded + frame_count * FRAME_SAMPLES,
						FRAME_SAMPLES, 0);
					frame_count++;
				}
			}
		}
		speaker->has_sequence = 1;
		speaker->sequence = number;
	}
	frames[frame_count] = opus_decode(speaker->decoder, data, length, decoded + frame_count * FRAME_SAMPLES,
		FRAME_SAMPLES * 2, 0);
	frame_count++;
	for (index = 0; index < frame_count; index++)
	{
		int sample;

		for (sample = 0; sample < frames[index] && sample < FRAME_SAMPLES * 2; sample++)
		{
			if (speaker->count >= JITTER_MAXIMUM)
			{
				speaker->read = (speaker->read + 1) % SPEAKER_BUFFER;
				speaker->count--;
			}
			speaker->buffer[(speaker->read + speaker->count) % SPEAKER_BUFFER] =
				decoded[index * FRAME_SAMPLES + sample];
			speaker->count++;
		}
	}
	voice_unlock();
}
