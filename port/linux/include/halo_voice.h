/*
HALO_VOICE.H

The voice chat's sound for the game (port/linux/src/voice.c: on Android the
host's microphone, Opus and mixer, port/android/host/host_voice.c; elsewhere
nothing). port/linux/game/network_voice.c sends and receives it.
*/

#ifndef __HALO_VOICE_H
#define __HALO_VOICE_H

#ifdef __cplusplus
extern "C" {
#endif

/* whether the voice chat is on (voice.enabled) */
int halo_voice_enabled(void);
/* starts playing voices, and the microphone if voice.microphone; stops */
int halo_voice_begin(void);
void halo_voice_end(void);
/* the microphone's speech since asked: Opus packets, each after a byte of
its length; returns the bytes */
int halo_voice_take(void *buffer, int size);
/* a speaker's packet, played at these gains (left, right) */
void halo_voice_play(int speaker, int sequence, const void *data, int length, float left, float right);

#ifdef __cplusplus
}
#endif

#endif
