/*
VR_GRAPHICS.C

The VR graphics presets (vr.graphics, the VR options' "Graphics" row): which
of the game's costlier effects are drawn, for headsets with little time to
spare (the Steam Frame spends most of its frame in its GL driver: fewer
draws help it most; the Quest 3 holds 90 Hz with all of them).

- "high", the default: everything, as before;
- "medium": no object shadows, environment reflections or lens flares;
- "low": nor specular light, bump maps, grass and other detail objects,
  the screen's fog layer, contrails, weather or camouflage's second pass;
- "max": "high", the models lit for each pixel and the shadows drawn at
  512x512 (d3d8_gl.c asks halo_vr_graphics).

Each effect is a switch the game has already: the rasterizer's console
variables (rasterizer_debug_options) and render.c's effect switches, after
HaloCE-Quest-VR's vr_graphics.c (CC0). Scripts and the console set some of
them (render_effects turns particles off in some cutscenes): a switch is
drawn only while both the game and the preset want it. A change the game
made since the last frame is taken as its wish, kept and combined with the
preset's. Playing in another's game, a client draws grass, lens flares and
the screen's fog as the host's rules have it (cheats.c), whatever the
preset; the reductions it does keep there, cheats.c leaves alone
(vr_graphics_reduces).

What each costs: shadows (each object's map, blur and projection: draws),
specular light and reflections (passes of the level's geometry: draws and
fill), detail objects (many small draws), lens flares (their visibility
tests), contrails and weather (particles), bump maps, the fog layer and
camouflage's pass (the GPU's fill).
*/

#ifdef HALO_ANDROID

#include "cseries.h"
#include "rasterizer/rasterizer_console_vars.h"
#include "networking/network_game_globals.h"
#include "halo_vr.h"

/* source/render/render.c, render_objects.c; source/effects/
weather_particle_systems.c */
extern boolean render_shadows;
extern boolean render_contrails_enabled;
extern boolean render_weather_particle_systems_enabled;
extern boolean weather;

void platform_log(char const *format, ...);

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

#define NUMBER_OF_VR_GRAPHICS_EFFECTS (sizeof(vr_graphics_effects) / sizeof(vr_graphics_effects[0]))
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
} vr_graphics;

static void vr_graphics_choose(
	int preset,
	boolean client)
{
	static char const *const names[] = { "low", "medium", "high", "max" };
	unsigned int index;

	vr_graphics.preset = preset;
	vr_graphics.client = client;
	for (index = 0; index < NUMBER_OF_VR_GRAPHICS_EFFECTS; index++)
	{
		struct vr_graphics_effect const *effect = &vr_graphics_effects[index];

		vr_graphics.allowed[index] = preset >= HALO_VR_GRAPHICS_HIGH || effect->preset[preset] ||
			(client && effect->host_rules);
	}
	platform_log("VR: graphics %s%s", names[preset], client ? " (in another's game)" : "");
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
		vr_graphics_choose(preset, client);
	}
	else if (preset != vr_graphics.preset || client != vr_graphics.client)
	{
		vr_graphics_choose(preset, client);
	}
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
