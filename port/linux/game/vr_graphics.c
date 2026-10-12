/*
VR_GRAPHICS.C

The VR graphics presets (vr.graphics, the VR options' "Graphics" row): which
of the game's costlier effects are drawn, for headsets with little time to
spare.

- "auto": "high", less what the GPU's time asks to leave out (below);
- "high": everything;
- "medium", the default (2026-10-12): no object shadows, environment
  reflections or lens flares;
- "low": nor specular light, bump maps, grass and other detail objects,
  the screen's fog layer, contrails, weather or camouflage's second pass;
- "max": "high", the models lit for each pixel and the shadows drawn at
  512x512 (d3d8_gl.c asks halo_vr_graphics).

The Quest 3 held 90 Hz with everything in the autumn's first tests; by
2026-10-12 (the whole body, upstream's shield colors and glow, 15 bots) a
bot game on Blood Gulch had seconds at a time of 9-13 ms of GPU a frame,
more than the 11.1 ms a frame less the compositor's share: stale frames,
ASW, the picture doubled as the head turns. "medium" held. So "auto" reads
the app's GPU time (XR_META_performance_metrics, host_xr.c) each frame: past
four fifths of the frame for a quarter of a second, it leaves out the next
effect of its steps (vr_graphics_auto_steps, the costliest for the least
seen first); an effect comes back once the GPU time, plus what leaving it
out saved when it went, stays under about seven tenths of the frame for
three seconds (longer each time it had to go again soon after). Where the
headset does not tell the GPU's time (SteamVR: the Steam Frame, whose time
goes to its driver on the CPU) "auto" is "high".

debug.vr_graphics_measure leaves each effect out in turn (a second and a
half on, as long off) and logs what the GPU time says it costs.

Each effect is a switch the game has already: the rasterizer's console
variables (rasterizer_debug_options) and render.c's effect switches, after
HaloCE-Quest-VR's vr_graphics.c (CC0); and the shield's glow
(shield_glow.c, display.shield_glow: a light per flaring unit, each lighting
the level once more). Scripts and the console set some of them
(render_effects turns particles off in some cutscenes): a switch is drawn
only while both the game and the preset want it. A change the game made
since the last frame is taken as its wish, kept and combined with the
preset's. Playing in another's game, a client draws grass, lens flares and
the screen's fog as the host's rules have it (cheats.c), whatever the
preset; the reductions it does keep there, cheats.c leaves alone
(vr_graphics_reduces).

What each costs: shadows (each object's map, blur and projection onto the
level: draws and fill; drawn once with both eyes at once, multiview, twice
one eye at a time), specular light and reflections (passes of the level's
geometry: draws and fill), detail objects (many small draws), lens flares
(their visibility tests), contrails and weather (particles), bump maps, the
fog layer and camouflage's pass (the GPU's fill).
*/

#ifdef HALO_ANDROID

#include "cseries.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "networking/network_game_globals.h"
#include "halo_vr.h"

#include <stdio.h>
#include <string.h>

/* source/render/render.c, render_objects.c; source/effects/
weather_particle_systems.c */
extern boolean render_shadows;
extern boolean render_contrails_enabled;
extern boolean render_weather_particle_systems_enabled;
extern boolean weather;

void platform_log(char const *format, ...);
/* port/linux/src/port_config.c */
int config_boolean(char const *name);

/* an effect: whether each preset draws it (low, medium; high and max draw
everything), whether a client in another's game draws it anyway, and the
game's switches it governs */
struct vr_graphics_effect
{
	char const *name;
	boolean preset[2];
	boolean host_rules;
	boolean *switches[2];
};

static struct vr_graphics_effect vr_graphics_effects[] =
{
	{ "shadows", { FALSE, FALSE }, FALSE,
		{ &render_shadows, &rasterizer_debug_options.draw_environment_shadows } },
	{ "specular", { FALSE, TRUE }, FALSE,
		{ &rasterizer_debug_options.draw_environment_specular_lights,
		  &rasterizer_debug_options.draw_environment_specular_lightmaps } },
	{ "reflections", { FALSE, FALSE }, FALSE,
		{ &rasterizer_debug_options.draw_environment_reflections, NULL } },
	{ "bump maps", { FALSE, TRUE }, FALSE,
		{ &rasterizer_debug_options.bump_mapping_enabled, NULL } },
	{ "detail objects", { FALSE, TRUE }, TRUE,
		{ &rasterizer_debug_options.draw_detail_objects, NULL } },
	{ "lens flares", { FALSE, FALSE }, TRUE,
		{ &rasterizer_debug_options.draw_lens_flares, NULL } },
	{ "screen fog", { FALSE, TRUE }, TRUE,
		{ &rasterizer_debug_options.draw_environment_fog_screen, NULL } },
	{ "contrails", { FALSE, TRUE }, FALSE,
		{ &render_contrails_enabled, NULL } },
	{ "weather", { FALSE, TRUE }, FALSE,
		{ &weather, &render_weather_particle_systems_enabled } },
	{ "camouflage pass", { FALSE, TRUE }, FALSE,
		{ &rasterizer_debug_options.active_camouflage_multipass_enabled, NULL } },
};

enum
{
	/* vr_graphics_effects' order */
	_vr_effect_shadows,
	_vr_effect_specular,
	_vr_effect_reflections,
	_vr_effect_bump_maps,
	_vr_effect_detail_objects,
	_vr_effect_lens_flares,
	_vr_effect_screen_fog,
	_vr_effect_contrails,
	_vr_effect_weather,
	_vr_effect_camouflage_pass,
	/* the shield's glow (shield_glow.c), no switch of the table's */
	_vr_effect_shield_glow,
	NUMBER_OF_VR_EFFECTS
};

#define NUMBER_OF_VR_GRAPHICS_EFFECTS (sizeof(vr_graphics_effects) / sizeof(vr_graphics_effects[0]))

static char const *const vr_effect_names[NUMBER_OF_VR_EFFECTS] =
{
	"shadows", "specular", "reflections", "bump maps", "detail objects", "lens flares", "screen fog", "contrails",
	"weather", "camouflage pass", "shield glow"
};

/* "auto"'s steps, each leaving out more: the shield's glow (a light per
flaring unit, a pass of the level each), object shadows, reflections,
specular light, lens flares (with the first three, "medium"), bump maps,
then the rest of "low" */
static unsigned long const vr_graphics_auto_steps[] =
{
	1ul << _vr_effect_shield_glow,
	1ul << _vr_effect_shadows,
	1ul << _vr_effect_reflections,
	1ul << _vr_effect_specular,
	1ul << _vr_effect_lens_flares,
	1ul << _vr_effect_bump_maps,
	(1ul << _vr_effect_detail_objects) | (1ul << _vr_effect_screen_fog) | (1ul << _vr_effect_contrails) |
		(1ul << _vr_effect_weather) | (1ul << _vr_effect_camouflage_pass),
};

#define NUMBER_OF_VR_AUTO_STEPS (sizeof(vr_graphics_auto_steps) / sizeof(vr_graphics_auto_steps[0]))
/* of the frame's time: the GPU's past this leaves an effect out; under this
with an effect's cost brings it back */
#define VR_AUTO_HIGH 0.80f
#define VR_AUTO_LOW 0.70f
/* seconds: past the high mark this long (a share of the frames: the
metric's frames vary) leaves out; settled after a change; under the low
mark this long brings back (doubled, to a minute, while an effect brought
back had to go again within ten seconds) */
#define VR_AUTO_OVER_SECONDS 0.25f
#define VR_AUTO_OVER_SHARE 0.75f
#define VR_AUTO_SETTLE_SECONDS 0.4f
#define VR_AUTO_UNDER_SECONDS 3.0f
#define VR_AUTO_UNDER_MOST 60.0f
#define VR_AUTO_AGAIN_SECONDS 10.0f
/* an effect's cost (ms) until measured, and the least kept */
#define VR_AUTO_COST_GUESS 1.5f
#define VR_AUTO_COST_LEAST 0.3f
/* debug.vr_graphics_measure: each half (on, off), seconds, of which the
last part is measured */
#define VR_MEASURE_HALF_SECONDS 1.5f
#define VR_MEASURE_SKIP_SECONDS 0.5f
#define VR_GRAPHICS_SWITCHES (sizeof(vr_graphics_effects[0].switches) / sizeof(vr_graphics_effects[0].switches[0]))

static struct
{
	boolean started;
	int preset;
	boolean client;
	/* whether the preset allows each effect; what the game wants of each
	switch, and what was written last frame */
	boolean allowed[NUMBER_OF_VR_GRAPHICS_EFFECTS];
	boolean wanted[NUMBER_OF_VR_GRAPHICS_EFFECTS][VR_GRAPHICS_SWITCHES];
	boolean written[NUMBER_OF_VR_GRAPHICS_EFFECTS][VR_GRAPHICS_SWITCHES];
	/* the effects left out now ("auto", debug.vr_graphics_measure), as
	bits of _vr_effect_*, and the shield's glow allowed */
	unsigned long left_out;
	boolean shield_glow_allowed;

	/* "auto": the steps taken (0: everything drawn); the frames since the
	last change, over the high mark lately and under the low mark in a
	row; the GPU time's sums for the step's cost (before the last step, and
	after it settled); how long under the mark brings back (frames' seconds),
	and when (frames) an effect came back */
	boolean automatic, no_gpu_time_logged;
	unsigned long timed_frame;
	unsigned long steps;
	unsigned long frame, changed_frame, restored_frame, dropped_frame;
	float over_window[64];
	unsigned long over_count;
	unsigned long under_frames;
	float before_sum;
	unsigned long before_count;
	float after_sum;
	unsigned long after_count;
	unsigned long measured_step;
	float step_costs[NUMBER_OF_VR_AUTO_STEPS];
	float under_seconds;

	/* debug.vr_graphics_measure: the effect measured, the frame its turn
	began, its sums on and off, and each effect's costs measured */
	boolean measuring;
	unsigned long measure_effect, measure_start;
	float measure_on_sum, measure_off_sum;
	unsigned long measure_on_count, measure_off_count;
	float measure_costs[NUMBER_OF_VR_EFFECTS];
	unsigned long measure_counts[NUMBER_OF_VR_EFFECTS];
} vr_graphics;

static void vr_graphics_allow(
	void)
{
	unsigned int index;

	for (index = 0; index < NUMBER_OF_VR_GRAPHICS_EFFECTS; index++)
	{
		struct vr_graphics_effect const *effect = &vr_graphics_effects[index];
		int preset = vr_graphics.preset;

		vr_graphics.allowed[index] = (vr_graphics.client && effect->host_rules) ||
			((preset == HALO_VR_GRAPHICS_AUTO || preset >= HALO_VR_GRAPHICS_HIGH || effect->preset[preset]) &&
			!(vr_graphics.left_out & (1ul << index)));
	}
	vr_graphics.shield_glow_allowed = !(vr_graphics.left_out & (1ul << _vr_effect_shield_glow));
}

static void vr_graphics_choose(
	int preset,
	boolean client)
{
	static char const *const names[] = { "low", "medium", "high", "max", "auto" };

	vr_graphics.preset = preset;
	vr_graphics.client = client;
	vr_graphics.automatic = preset == HALO_VR_GRAPHICS_AUTO;
	if (!vr_graphics.automatic && !vr_graphics.measuring)
		vr_graphics.left_out = 0;
	vr_graphics_allow();
	platform_log("VR: graphics %s%s", names[preset], client ? " (in another's game)" : "");
}

/* the steps' effects left out, as bits */
static unsigned long vr_auto_left_out(
	unsigned long steps)
{
	unsigned long left_out = 0, step;

	for (step = 0; step < steps && step < NUMBER_OF_VR_AUTO_STEPS; step++)
		left_out |= vr_graphics_auto_steps[step];
	return left_out;
}

static void vr_auto_effects_names(
	unsigned long effects,
	char *text,
	size_t size)
{
	unsigned long effect;

	text[0] = 0;
	for (effect = 0; effect < NUMBER_OF_VR_EFFECTS; effect++)
	{
		if (effects & (1ul << effect))
		{
			size_t length = strlen(text);

			snprintf(text + length, size - length, "%s%s", length ? ", " : "", vr_effect_names[effect]);
		}
	}
}

/* whether a step leaves out anything drawn now: the shield's glow only
with display.shield_glow, an effect only while the game wants it */
static boolean vr_auto_step_matters(
	unsigned long step)
{
	unsigned long effects = vr_graphics_auto_steps[step];
	unsigned long effect;

	if ((effects & (1ul << _vr_effect_shield_glow)) && config_boolean("display.shield_glow"))
		return TRUE;
	for (effect = 0; effect < NUMBER_OF_VR_GRAPHICS_EFFECTS; effect++)
	{
		if ((effects & (1ul << effect)) && (vr_graphics.wanted[effect][0] || vr_graphics.wanted[effect][1]))
			return TRUE;
	}
	return FALSE;
}

/* "auto": steps more left out, enough of them for what is over the mark by
their costs as known (more: TRUE), or one brought back */
static void vr_auto_step(
	boolean more,
	float gpu_ms,
	float frame_ms,
	float excess_ms)
{
	char names[160];
	unsigned long first = vr_graphics.steps;

	if (more)
	{
		float saved = 0.f;

		while (vr_graphics.steps < NUMBER_OF_VR_AUTO_STEPS && (saved < excess_ms || saved == 0.f))
		{
			if (vr_auto_step_matters(vr_graphics.steps))
				saved += vr_graphics.step_costs[vr_graphics.steps];
			vr_graphics.steps++;
		}
		{
			unsigned long step, effects = 0;

			for (step = first; step < vr_graphics.steps; step++)
				effects |= vr_auto_step_matters(step) ? vr_graphics_auto_steps[step] : 0;
			vr_auto_effects_names(effects, names, sizeof(names));
		}
		platform_log("VR graphics auto: %s left out (step %lu): the GPU %.1f ms a frame of %.1f", names,
			vr_graphics.steps, gpu_ms, frame_ms);
		/* (their cost: the GPU's time before, against after it settles; one
		step's alone) */
		vr_graphics.measured_step = vr_graphics.steps - first == 1 ? vr_graphics.steps : 0;
		vr_graphics.after_sum = 0.f;
		vr_graphics.after_count = 0;
		/* (it had to go again soon after coming back: wait longer, once for
		each coming back) */
		if (vr_graphics.restored_frame && vr_graphics.frame - vr_graphics.restored_frame <
			(unsigned long)(VR_AUTO_AGAIN_SECONDS * (1000.f / frame_ms)))
		{
			vr_graphics.under_seconds = MIN(vr_graphics.under_seconds * 2.f, VR_AUTO_UNDER_MOST);
		}
		vr_graphics.restored_frame = 0;
		vr_graphics.dropped_frame = vr_graphics.frame;
	}
	else
	{
		/* (one that matters back, and those that do not below it with it) */
		vr_graphics.steps--;
		while (vr_graphics.steps && !vr_auto_step_matters(vr_graphics.steps - 1))
			vr_graphics.steps--;
		vr_auto_effects_names(vr_auto_left_out(first) & ~vr_auto_left_out(vr_graphics.steps), names, sizeof(names));
		platform_log("VR graphics auto: %s back (step %lu): the GPU %.1f ms a frame of %.1f, %.1f ms for it",
			names, vr_graphics.steps, gpu_ms, frame_ms, vr_graphics.step_costs[vr_graphics.steps]);
		vr_graphics.restored_frame = vr_graphics.frame;
		vr_graphics.measured_step = 0;
	}
	vr_graphics.changed_frame = vr_graphics.frame;
	vr_graphics.over_count = 0;
	vr_graphics.under_frames = 0;
	vr_graphics.left_out = vr_auto_left_out(vr_graphics.steps);
	vr_graphics_allow();
}

/* "auto", each frame: the GPU's time against the frame's */
static void vr_auto_update(
	float gpu_ms,
	float frame_ms,
	float frames_a_second)
{
	unsigned long settle = (unsigned long)(VR_AUTO_SETTLE_SECONDS * frames_a_second);
	unsigned long over_frames = (unsigned long)(VR_AUTO_OVER_SECONDS * frames_a_second);
	unsigned long since = vr_graphics.frame - vr_graphics.changed_frame;
	unsigned long index, over = 0;

	if (over_frames < 4)
		over_frames = 4;
	if (over_frames > NUMBEROF(vr_graphics.over_window))
		over_frames = NUMBEROF(vr_graphics.over_window);
	/* (what the last step saved: the GPU time before it, against the next
	second's after it settled) */
	if (vr_graphics.measured_step && since > settle && since <= settle + (unsigned long)frames_a_second)
	{
		vr_graphics.after_sum += gpu_ms;
		vr_graphics.after_count++;
		if (since == settle + (unsigned long)frames_a_second && vr_graphics.before_count && vr_graphics.after_count)
		{
			float cost = vr_graphics.before_sum / vr_graphics.before_count - vr_graphics.after_sum / vr_graphics.after_count;
			unsigned long step = vr_graphics.measured_step - 1;

			vr_graphics.step_costs[step] = MAX(VR_AUTO_COST_LEAST, 0.5f * (vr_graphics.step_costs[step] + cost));
			platform_log("VR graphics auto: leaving out step %lu saved %.1f ms (taken as %.1f)", step + 1, cost,
				vr_graphics.step_costs[step]);
			vr_graphics.measured_step = 0;
		}
	}
	if (since < settle)
		return;
	vr_graphics.over_window[vr_graphics.over_count++ % over_frames] = gpu_ms;
	for (index = 0; index < over_frames && index < vr_graphics.over_count; index++)
		over += vr_graphics.over_window[index] > VR_AUTO_HIGH * frame_ms;
	if (vr_graphics.over_count >= over_frames && over >= (unsigned long)(VR_AUTO_OVER_SHARE * over_frames) &&
		vr_graphics.steps < NUMBER_OF_VR_AUTO_STEPS)
	{
		float sum = 0.f;

		for (index = 0; index < over_frames; index++)
			sum += vr_graphics.over_window[index];
		vr_graphics.before_sum = sum;
		vr_graphics.before_count = over_frames;
		/* (far over: as many steps as its costs say it takes, at once) */
		vr_auto_step(TRUE, sum / over_frames, frame_ms, sum / over_frames - VR_AUTO_HIGH * frame_ms);
		return;
	}
	/* (back: the time, with the effect's own, under the low mark) */
	if (vr_graphics.steps && gpu_ms + vr_graphics.step_costs[vr_graphics.steps - 1] < VR_AUTO_LOW * frame_ms)
	{
		vr_graphics.under_frames++;
	}
	else
	{
		vr_graphics.under_frames = 0;
	}
	if (vr_graphics.steps && vr_graphics.under_frames > (unsigned long)(vr_graphics.under_seconds * frames_a_second))
		vr_auto_step(FALSE, gpu_ms, frame_ms, 0.f);
	/* (a minute without having to leave anything out: as quick as at first) */
	if (vr_graphics.frame - vr_graphics.dropped_frame > (unsigned long)(60.f * frames_a_second))
		vr_graphics.under_seconds = VR_AUTO_UNDER_SECONDS;
}

/* debug.vr_graphics_measure, each frame: each effect left out in turn */
static void vr_measure_update(
	float gpu_ms,
	float frames_a_second)
{
	unsigned long half = (unsigned long)(VR_MEASURE_HALF_SECONDS * frames_a_second);
	unsigned long skip = (unsigned long)(VR_MEASURE_SKIP_SECONDS * frames_a_second);
	unsigned long since = vr_graphics.frame - vr_graphics.measure_start;

	if (since >= skip && since < half)
	{
		vr_graphics.measure_on_sum += gpu_ms;
		vr_graphics.measure_on_count++;
	}
	else if (since >= half + skip && since < 2 * half)
	{
		vr_graphics.measure_off_sum += gpu_ms;
		vr_graphics.measure_off_count++;
	}
	if (since == half)
	{
		vr_graphics.left_out = 1ul << vr_graphics.measure_effect;
		vr_graphics_allow();
	}
	if (since < 2 * half)
		return;
	if (vr_graphics.measure_on_count && vr_graphics.measure_off_count)
	{
		unsigned long effect = vr_graphics.measure_effect;
		float on = vr_graphics.measure_on_sum / vr_graphics.measure_on_count;
		float off = vr_graphics.measure_off_sum / vr_graphics.measure_off_count;

		vr_graphics.measure_costs[effect] += on - off;
		vr_graphics.measure_counts[effect]++;
		platform_log("VR graphics measure: %s %.2f ms (on %.2f, off %.2f; mean of %lu: %.2f)", vr_effect_names[effect],
			on - off, on, off, vr_graphics.measure_counts[effect],
			vr_graphics.measure_costs[effect] / vr_graphics.measure_counts[effect]);
	}
	vr_graphics.measure_effect = (vr_graphics.measure_effect + 1) % NUMBER_OF_VR_EFFECTS;
	vr_graphics.measure_start = vr_graphics.frame;
	vr_graphics.measure_on_sum = vr_graphics.measure_off_sum = 0.f;
	vr_graphics.measure_on_count = vr_graphics.measure_off_count = 0;
	vr_graphics.left_out = 0;
	vr_graphics_allow();
	if (!vr_graphics.measure_effect)
	{
		char line[512] = "";
		unsigned long effect;

		for (effect = 0; effect < NUMBER_OF_VR_EFFECTS; effect++)
		{
			size_t length = strlen(line);

			if (vr_graphics.measure_counts[effect])
				snprintf(line + length, sizeof(line) - length, " %s %.2f;", vr_effect_names[effect],
					vr_graphics.measure_costs[effect] / vr_graphics.measure_counts[effect]);
		}
		platform_log("VR graphics measure: means (ms of GPU a frame):%s", line);
	}
}

/* the GPU's time against the frame's, each frame: "auto" and
debug.vr_graphics_measure */
static void vr_graphics_govern(
	void)
{
	float gpu_ms = halo_vr_gpu_milliseconds();
	float rate = halo_vr_refresh_rate();
	float frames_a_second = rate > 1.f ? rate : 90.f;

	vr_graphics.frame++;
	if (!vr_graphics.automatic && !vr_graphics.measuring)
		return;
	/* (none for five seconds: the headset does not tell it) */
	if (gpu_ms > 0.f)
		vr_graphics.timed_frame = vr_graphics.frame;
	if (gpu_ms <= 0.f && vr_graphics.frame - vr_graphics.timed_frame < (unsigned long)(5.f * frames_a_second))
		return;
	if (gpu_ms <= 0.f)
	{
		if (!vr_graphics.no_gpu_time_logged)
			platform_log("VR graphics %s: the headset does not tell the GPU's time: everything drawn (\"high\")",
				vr_graphics.measuring ? "measure" : "auto");
		vr_graphics.no_gpu_time_logged = TRUE;
		if (vr_graphics.left_out)
		{
			vr_graphics.left_out = 0;
			vr_graphics.steps = 0;
			vr_graphics_allow();
		}
		return;
	}
	if (vr_graphics.measuring)
		vr_measure_update(gpu_ms, frames_a_second);
	else
		vr_auto_update(gpu_ms, 1000.f / frames_a_second, frames_a_second);
}

/* each frame, before it is drawn (render.c) */
void vr_graphics_update(
	void)
{
	int preset = halo_vr_graphics();
	boolean client = network_game_distributed_client();
	unsigned int index, which;

	if (!vr_graphics.started)
	{
		/* the game's switches as it starts are its wishes */
		for (index = 0; index < NUMBER_OF_VR_GRAPHICS_EFFECTS; index++)
		{
			for (which = 0; which < VR_GRAPHICS_SWITCHES && vr_graphics_effects[index].switches[which]; which++)
			{
				vr_graphics.wanted[index][which] = *vr_graphics_effects[index].switches[which];
				vr_graphics.written[index][which] = *vr_graphics_effects[index].switches[which];
			}
		}
		vr_graphics.started = TRUE;
		vr_graphics.measuring = config_boolean("debug.vr_graphics_measure") != 0;
		vr_graphics.under_seconds = VR_AUTO_UNDER_SECONDS;
		for (index = 0; index < NUMBER_OF_VR_AUTO_STEPS; index++)
			vr_graphics.step_costs[index] = VR_AUTO_COST_GUESS;
		vr_graphics.shield_glow_allowed = TRUE;
		vr_graphics_choose(preset, client);
		if (vr_graphics.measuring)
			platform_log("VR graphics measure: each effect left out in turn, %.1f s on and %.1f s off",
				VR_MEASURE_HALF_SECONDS, VR_MEASURE_HALF_SECONDS);
	}
	else if (preset != vr_graphics.preset || client != vr_graphics.client)
	{
		vr_graphics.steps = 0;
		vr_graphics_choose(preset, client);
	}
	vr_graphics_govern();
	for (index = 0; index < NUMBER_OF_VR_GRAPHICS_EFFECTS; index++)
	{
		for (which = 0; which < VR_GRAPHICS_SWITCHES && vr_graphics_effects[index].switches[which]; which++)
		{
			boolean *game_switch = vr_graphics_effects[index].switches[which];
			boolean value;

			/* (the game, a script, the console changed it since: its wish) */
			if (*game_switch != vr_graphics.written[index][which])
				vr_graphics.wanted[index][which] = *game_switch;
			value = vr_graphics.wanted[index][which] && vr_graphics.allowed[index];
			*game_switch = value;
			vr_graphics.written[index][which] = value;
		}
	}
}

/* whether the shield's glow may be drawn (shield_glow.c): "auto" leaves it
out first */
boolean vr_graphics_allows_shield_glow(
	void)
{
	return !vr_graphics.started || vr_graphics.shield_glow_allowed;
}

/* whether the preset keeps this switch of the game's off now (cheats.c, a
client's rules: those it may keep off) */
boolean vr_graphics_reduces(
	boolean const *game_switch)
{
	unsigned int index, which;

	if (!vr_graphics.started)
		return FALSE;
	for (index = 0; index < NUMBER_OF_VR_GRAPHICS_EFFECTS; index++)
	{
		for (which = 0; which < VR_GRAPHICS_SWITCHES && vr_graphics_effects[index].switches[which]; which++)
		{
			if (vr_graphics_effects[index].switches[which] == game_switch)
				return !vr_graphics.allowed[index];
		}
	}
	return FALSE;
}

#endif
