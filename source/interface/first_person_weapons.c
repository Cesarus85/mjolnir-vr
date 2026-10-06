/*
FIRST_PERSON_WEAPONS.C

symbols in this file:
000CBC60 0020:
	_animation_update_render_only (0000)
000CBC80 0040:
	_first_person_weapons_initialize (0000)
000CBCC0 0010:
	_first_person_weapons_dispose (0000)
000CBCD0 0050:
	_first_person_weapons_initialize_for_new_map (0000)
000CBD20 0010:
	_first_person_weapons_dispose_from_old_map (0000)
000CBD30 00c0:
	_code_000cbd30 (0000)
000CBDF0 0110:
	_code_000cbdf0 (0000)
000CBF00 0120:
	_code_000cbf00 (0000)
000CC020 0040:
	_code_000cc020 (0000)
000CC060 00a0:
	_code_000cc060 (0000)
000CC100 00b0:
	_code_000cc100 (0000)
000CC1B0 00e0:
	_code_000cc1b0 (0000)
000CC290 0060:
	_code_000cc290 (0000)
000CC2F0 0040:
	_code_000cc2f0 (0000)
000CC330 0080:
	_code_000cc330 (0000)
000CC3B0 0290:
	_first_person_weapon_draw (0000)
000CC640 0080:
	_first_person_weapon_get_local_index (0000)
000CC6C0 00d0:
	_first_person_weapon_get_marker_by_name (0000)
000CC790 00e0:
	_first_person_weapon_center_flashlight (0000)
000CC870 00d0:
	_first_person_weapon_adjust_light (0000)
000CC940 00c0:
	_first_person_weapon_get_node_matrix (0000)
000CCA00 00b0:
	_code_000cca00 (0000)
000CCAB0 0560:
	_code_000ccab0 (0000)
000CD010 00b0:
	_first_person_weapon_render_update (0000)
000CD0C0 0040:
	_first_person_weapon_get_marker_by_name_render (0000)
000CD100 02b0:
	_code_000cd100 (0000)
000CD3B0 0260:
	_code_000cd3b0 (0000)
000CD610 0060:
	_code_000cd610 (0000)
000CD670 0220:
	_code_000cd670 (0000)
000CD890 0050:
	_first_person_weapon_message_from_unit (0000)
000CD8E0 0040:
	_first_person_weapon_message_from_weapon (0000)
000CD920 0170:
	_code_000cd920 (0000)
000CDA90 0600:
	_code_000cda90 (0000)
000CE090 00f0:
	_first_person_weapons_update (0000)
0027076C 0015:
	??_C@_0BF@MEAEHHHF@first_person_weapons?$AA@ (0000)
00270784 0030:
	??_C@_0DA@LGOECHKB@c?3?2halo?2SOURCE?2interface?2first_p@ (0000)
002707B4 0015:
	??_C@_0BF@LKJCKPFC@first?5person?5weapons?$AA@ (0000)
002707CC 0028:
	??_C@_0CI@FFNKKBPO@first_person_weapon?9?$DOweapon_inde@ (0000)
002707F8 0059:
	??_C@_0FJ@DFINBDBK@animation_graph_node_index?$DO?$DN0?5?$CG?$CG@ (0000)
00270854 000b:
	??_C@_0L@HGCOMEEN@flashlight?$AA@ (0000)
00270860 0039:
	??_C@_0DJ@KDPHKICA@node_index?$DO?$DN0?5?$CG?$CG?5node_index?$DManim@ (0000)
0027089C 0035:
	??_C@_0DF@COECBIGL@local?5player?5?$CFd?0?5weapon?5?$CI0x?$CFx?$CJ?0?5@ (0000)
002708D8 00a5:
	??_C@_0KF@OCJNMLNK@first_person_weapon?9?$DOshotgun_rel@ (0000)
00270980 0004:
	__real@c1f00000 (0000)
00453CC8 0004:
	_bss_00453cc8 (0000)
*/

/* ---------- headers */

#define REAL_MATH_EXTERNAL_SIGNED_ANGULAR_DIFFERENCE
#define REAL_MATH_EXTERNAL_REAL_LOCAL_RANDOM
#define REAL_MATH_EXTERNAL_REAL_LOCAL_RANDOM_RANGE
#include "interface/first_person_weapons.h"

#include "cache/predicted_resources.h"
#include "camera/director.h"
#include "cseries/cseries.h"
#include "cseries/errors.h"
#include "effects/effects.h"
#include "effects/particles.h"
#include "game/game_globals.h"
#include "game/players.h"
#include "items/weapon_datum_flags.h"
#include "items/weapon_definitions.h"
#include "items/weapons.h"
#include "models/model_animation_definitions.h"
#include "models/model_definitions.h"
#include "models/models.h"
#include "networking/network_connection.h"
#include "objects/objects.h"
#include "rasterizer/rasterizer_model_types.h"
#include "render/render.h"
#include "saved games/game_state.h"
#include "scenario/scenario.h"
#include "sound/game_sound.h"
#include "sound/sound_manager.h"
#include "units/units.h"
#ifdef HALO_ANDROID
#include "halo_vr.h"
#include "vr_arms.h"
#endif

/* ---------- constants */

enum
{
	_render_model_first_person_bit = 3,
};

enum
{
	NUMBER_OF_FIRST_PERSON_WEAPON_OVERLAY_FRAMES = 9,
	FIRST_PERSON_WEAPON_RELOAD_SETTLE_TICKS = 44,
};

#define FIRST_PERSON_WEAPON_FIRING_PUSH_BACK_VELOCITY 0.05f

enum first_person_weapon_state
{
	_first_person_weapon_state_idle = 0,
	_first_person_weapon_state_overheating,
	_first_person_weapon_state_overheating_again,
	_first_person_weapon_state_overheated,
	_first_person_weapon_state_charged,
	_first_person_weapon_state_posing,
	_first_person_weapon_state_primary_fire,
	_first_person_weapon_state_secondary_fire,
	_first_person_weapon_state_primary_misfire,
	_first_person_weapon_state_secondary_misfire,
	_first_person_weapon_state_melee,
	_first_person_weapon_state_light_on,
	_first_person_weapon_state_light_off,
	_first_person_weapon_state_reload_while_empty,
	_first_person_weapon_state_reload_while_full,
	_first_person_weapon_state_shotgun_enter_reload,
	_first_person_weapon_state_shotgun_exit_reload_empty,
	_first_person_weapon_state_shotgun_exit_reload_full,
	_first_person_weapon_state_put_away,
	_first_person_weapon_state_ready,
	_first_person_weapon_state_throw_grenade,
	_first_person_weapon_state_throw_grenade_overheated,
	_first_person_weapon_state_overheated_exit,
	_first_person_weapon_state_overheating_super_recoil,
	NUMBER_OF_FIRST_PERSON_WEAPON_STATES,
};

enum
{
	_render_model_effect_type_none = 0,
	_render_model_effect_type_active_camouflage,
};

enum
{
	animation_update_kind_render_only = 0,
};

enum animation_update_result
{
	_animation_no_key_frame = 0,
	_animation_key_frame,
	_animation_will_restart_on_next_frame,
};

/* ---------- macros */

/* ---------- structures */

struct shader;

/* Kept TU-private because moving this recovered layout into a shared header perturbs VC7 output. */
struct animation_state_continuous
{
	short index;
	word pad;
	real frame_index;
};

/* Recovered animation-graph block layouts used only by this translation unit. */
struct animation_graph_sound_reference
{
	struct tag_reference sound;
	long unused;
};

typedef char verify_animation_graph_sound_reference_size[
	sizeof(struct animation_graph_sound_reference) == 0x14 ? 1 : -1];

struct animation_graph_first_person_weapon_animations
{
	long unused[4];
	struct tag_block animations;		// animation_graph_animation_index
};

typedef char verify_animation_graph_first_person_weapon_animations_size[
	sizeof(struct animation_graph_first_person_weapon_animations) == 0x1C ? 1 : -1];

/* Only the leading name is consumed here; the remaining bytes preserve the January layout. */
struct animation_graph_node
{
	char name[TAG_STRING_LENGTH+1];
	byte reserved0020[0x20];
};

typedef char verify_animation_graph_node_size[
	sizeof(struct animation_graph_node) == 0x40 ? 1 : -1];

typedef char verify_render_model_effect_size[
	sizeof(struct render_model_effect) == 0x28 ? 1 : -1];

struct first_person_weapon
{
	boolean visible;
	byte reserved0001[3];
	long unit_index;
	long weapon_index;
	short state;
	short ticks_until_pose;
	short ticks_idle;
	short ticks_until_predict;
	short overlay_animation_index;
	struct animation_state state_animation;
	struct animation_state moving_animation;
	short reserved001e;
	struct animation_state_continuous overcharged_jitter_animation;
	real firing_push_back;
	real firing_push_back_velocity;
	real_vector2d position;
	real_vector2d position_velocity;
	real_vector2d turning;
	real_vector2d turning_velocity;
	boolean rendered;
	byte reserved0051[3];
	real_vector3d render_forward;
	real_euler_angles2d render_facing;
	real_euler_angles2d last_render_facing;
	real_point3d render_position;
	real_point3d last_render_position;
	short interpolation_frame_index;
	short interpolation_frame_count;
	real_orientation node_orientations[MAXIMUM_NODES_PER_ANIMATION];
	real_orientation original_node_orientations[MAXIMUM_NODES_PER_ANIMATION];
	struct real_matrix4x3 node_matrices[MAXIMUM_NODES_PER_ANIMATION];
	boolean weapon_node_remapping_table_valid;
	byte reserved1d8d;
	short weapon_node_remapping_table[MAXIMUM_NODES_PER_ANIMATION];
	boolean hands_node_remapping_table_valid;
	byte reserved1e0f;
	short hands_node_remapping_table[MAXIMUM_NODES_PER_ANIMATION];
	boolean shotgun_empty;
	byte reserved1e91;
	short shotgun_shells_to_reload;
	short shotgun_reload_type;
	short reserved1e96;
	long current_sound_index;
	short current_sound_state;
	byte reserved1e9e[2];
};

typedef char verify_first_person_weapon_size[
	sizeof(struct first_person_weapon) == 0x1EA0 ? 1 : -1];
typedef char verify_first_person_weapon_node_matrices_offset[
	offsetof(struct first_person_weapon, node_matrices) == 0x108C ? 1 : -1];
typedef char verify_first_person_weapon_rendered_offset[
	offsetof(struct first_person_weapon, rendered) == 0x50 ? 1 : -1];
typedef char verify_first_person_weapon_node_orientations_offset[
	offsetof(struct first_person_weapon, node_orientations) == 0x8C ? 1 : -1];
typedef char verify_first_person_weapon_node_remapping_table_offset[
	offsetof(struct first_person_weapon, weapon_node_remapping_table) == 0x1D8E ? 1 : -1];
typedef char verify_first_person_weapon_shotgun_empty_offset[
	offsetof(struct first_person_weapon, shotgun_empty) == 0x1E90 ? 1 : -1];
typedef char verify_first_person_weapon_current_sound_index_offset[
	offsetof(struct first_person_weapon, current_sound_index) == 0x1E98 ? 1 : -1];
typedef char verify_first_person_weapon_current_sound_state_offset[
	offsetof(struct first_person_weapon, current_sound_state) == 0x1E9C ? 1 : -1];
typedef char verify_weapon_first_person_animations_index_offset[
	offsetof(
		struct weapon_definition,
		weapon.interface_definition.first_person_animations.index) == 0x478 ? 1 : -1];
typedef char verify_weapon_flags_offset[
	offsetof(struct weapon_datum, weapon.flags) == 0x1DC ? 1 : -1];
typedef char verify_weapon_type_offset[
	offsetof(struct weapon_definition, weapon.weapon_type) == 0x4E2 ? 1 : -1];
typedef char verify_animation_graph_first_person_weapon_animations_offset[
	offsetof(struct animation_graph, first_person_weapon_animations) == 0x48 ? 1 : -1];
typedef char verify_unit_flags_offset[
	offsetof(struct unit_datum, unit.flags) == 0x1B4 ? 1 : -1];
typedef char verify_unit_active_camouflage_offset[
	offsetof(struct unit_datum, unit.active_camouflage) == 0x32C ? 1 : -1];
typedef char verify_object_outgoing_function_values_offset[
	offsetof(struct object_datum, object.outgoing_function_values) == 0xE4 ? 1 : -1];
typedef char verify_object_outgoing_change_colors_offset[
	offsetof(struct object_datum, object.outgoing_change_colors) == 0x168 ? 1 : -1];
typedef char verify_game_globals_first_person_interface_offset[
	offsetof(struct game_globals, first_person_interface) == 0x17C ? 1 : -1];
typedef char verify_animation_graph_nodes_offset[
	offsetof(struct animation_graph, nodes) == 0x68 ? 1 : -1];

/* ---------- prototypes */

static void first_person_weapon_set_visibility(
	short local_player_index,
	boolean visible);
static void first_person_weapon_build_node_matrices(
	short local_player_index);
static void first_person_weapon_predict(
	short local_player_index);
static void first_person_weapon_start_interpolation(
	short local_player_index,
	short frame_count);
static void first_person_weapon_set_state(
	short local_player_index,
	short new_state,
	boolean reset_sounds);
static void first_person_weapon_switch_weapons(
	short local_player_index);
static void first_person_weapon_forget_weapon(
	short local_player_index);
static void first_person_weapon_new_unit(
	short local_player_index,
	long unit_index);
static void first_person_weapon_message(
	short local_player_index,
	short message_type);
static void first_person_weapon_next_state(
	short local_player_index);
static void first_person_weapon_update(
	short local_player_index);
static boolean model_build_remapping_table_for_animation_graph(
	long model_index,
	long animation_graph_index,
	short *node_remapping_table);
static void weapon_play_first_person_weapon_sound(
	long weapon_index,
	short message_type);
static long first_person_weapon_state_from_weapon_message(
	short message_type);
static long first_person_animation_type_from_weapon_state(
	short state);
static void model_remap_node_matrices_to_match_animation_graph(
	long model_index,
	struct real_matrix4x3 *model_node_matrices,
	long animation_graph_index,
	struct real_matrix4x3 const *node_matrices,
	short const *node_remapping_table);
static struct first_person_weapon *first_person_weapon_get(
	short local_player_index);
static short first_person_weapon_index_from_weapon_index(
	long weapon_index);
static short first_person_weapon_index_from_unit_index(
	long unit_index);

/* port/linux/game/pal_tags.c's */
boolean pal_tags_first_person_advance(short local_player_index, long graph_index, short animation_index,
	short frame_index);
real pal_tags_first_person_fraction(short local_player_index, long graph_index, short animation_index,
	short frame_index);

/* ---------- globals */

static struct first_person_weapon *first_person_weapons;

/* ---------- public code */

short animation_update_render_only(
	long animation_graph_index,
	struct animation_state *state,
	long *sound_index)
{
	return animation_update_internal(
		0,
		animation_graph_index,
		state,
		sound_index);
}

void first_person_weapons_initialize(
	void)
{
	first_person_weapons = (struct first_person_weapon *)game_state_malloc(
		"first person weapons",
		NULL,
		sizeof(*first_person_weapons) * MAXIMUM_NUMBER_OF_LOCAL_PLAYERS);
	match_assert(
		"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
		240,
		first_person_weapons);

	return;
}

void first_person_weapons_dispose(
	void)
{
	return;
}

void first_person_weapons_initialize_for_new_map(
	void)
{
	short local_player_index;

	for (local_player_index=0; local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		csmemset(
			&first_person_weapons[local_player_index],
			0,
			sizeof(first_person_weapons[local_player_index]));
		first_person_weapons[local_player_index].unit_index= NONE;
		first_person_weapons[local_player_index].current_sound_index= NONE;
		first_person_weapons[local_player_index].current_sound_state= NONE;
	}

	return;
}

void first_person_weapons_dispose_from_old_map(
	void)
{
	return;
}

long first_person_weapon_get_local_index(
	long weapon_index)
{
	short local_player_index;

	for (local_player_index=0; local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

		if (first_person_weapon->weapon_index==weapon_index && first_person_weapon->visible)
		{
			break;
		}
	}

	return (local_player_index==MAXIMUM_NUMBER_OF_LOCAL_PLAYERS) ? NONE : local_player_index;
}

#ifdef HALO_ANDROID
int config_boolean(char const *name);
void model_vr_log_skin(long model_index);

/* VR: the headset's frame each local player's pose was last built for
from the controller (halo_vr_frame_serial; 0 when not), and its weapon */
static unsigned long first_person_weapon_vr_pose_serial[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];
static long first_person_weapon_vr_pose_weapon[MAXIMUM_NUMBER_OF_LOCAL_PLAYERS];

/* the pose was built for this frame already, from the controller, which
the eyes do not move (from the eye's own camera, each eye builds its own) */
static boolean first_person_weapon_vr_pose_kept(
	short local_player_index)
{
	unsigned long serial= halo_vr_frame_serial();

	return serial!=0 &&
		first_person_weapon_vr_pose_serial[local_player_index]==serial &&
		first_person_weapon_vr_pose_weapon[local_player_index]==first_person_weapon_get(local_player_index)->weapon_index;
}

/* VR: the grenade the player holds (the free hand's trigger, units.c)
hangs from the body, which the first-person view leaves out, and with it
the grenade: drawn instead in the free hand, as it is this frame, with the
hands (which are drawn over the world, and would hide it wherever they
overlap; in the room, not mirrored) */
static void first_person_weapon_draw_held_grenade(
	long unit_index,
	struct render_lighting *lighting,
	struct render_model_effect const *hands_effect)
{
	struct unit_datum *unit= unit_get(unit_index);
	struct object_datum *grenade;
	struct object_definition *definition;
	struct model *model;
	struct render_model_effect model_effect;
	real_matrix4x3 *node_matrices;
	real_matrix4x3 hand, inverse, delta;
	real_point3d position;
	real_vector3d forward, up;
	float hand_position[3], hand_forward[3], hand_up[3], scale;
	short node_index;

	if (unit->unit.grenade_object_index==NONE ||
		unit->unit.grenade_throw_state!=_unit_grenade_throw_in_hand ||
		!halo_vr_grenade_in_hand(render.local_player_index, hand_position, hand_forward, hand_up, &scale))
	{
		return;
	}
	grenade= object_get(unit->unit.grenade_object_index);
	definition= object_definition_get(grenade->definition_index);
	if (definition->object.model.index==NONE || TEST_FLAG(grenade->object.flags, _object_invisible_bit))
		return;
	model= model_definition_get(definition->object.model.index);
	if (model->nodes.count<=0)
		return;
	node_matrices= object_get_node_matrices(unit->unit.grenade_object_index);

	/* the whole grenade moved so its root lies in the hand, as the hand
	turns (drawn only: the next tick puts it back on the body) */
	position.x= hand_position[0];
	position.y= hand_position[1];
	position.z= hand_position[2];
	forward.i= hand_forward[0];
	forward.j= hand_forward[1];
	forward.k= hand_forward[2];
	up.i= hand_up[0];
	up.j= hand_up[1];
	up.k= hand_up[2];
	matrix4x3_from_point_and_vectors(&hand, &position, &forward, &up);
	hand.scale= scale;
	matrix4x3_inverse(&node_matrices[0], &inverse);
	matrix4x3_multiply(&hand, &inverse, &delta);
	for (node_index= 0; node_index<model->nodes.count; node_index++)
	{
		real_matrix4x3 node= node_matrices[node_index];

		matrix4x3_multiply(&delta, &node, &node_matrices[node_index]);
	}

	model_effect= *hands_effect;
	render_model(
		definition->object.model.index,
		0.0f,
		node_matrices,
		grenade->object.region_permutations,
		grenade->object.outgoing_change_colors,
		grenade->object.outgoing_function_values,
		lighting,
		&render.camera.position,
		0.0f,
		&model_effect,
		unit->unit.grenade_object_index,
		grenade->object.forced_shader_permutation_index,
		FLAG(_render_model_first_person_bit));
}

#endif
void first_person_weapon_draw(
	void)
{
#ifdef HALO_ANDROID
	/* VR: not in the scope's view, taken from the weapon itself */
	if (halo_vr_scope_pass())
		return;
#endif
	if (render.local_player_index!=NONE)
	{
		struct first_person_weapon *first_person_weapon= first_person_weapon_get(render.local_player_index);

		if (local_player_get_player_index(render.local_player_index)!=NONE)
		{
			long unit_index= player_get(
				local_player_get_player_index(render.local_player_index))->unit_index;

			if (unit_index!=NONE &&
				first_person_weapon->visible &&
				first_person_weapon->unit_index!=NONE &&
				first_person_weapon->weapon_index!=NONE
#ifdef HALO_ANDROID
				/* VR: at a wheel or a gun the body's own hands hold them */
				&& !(halo_vr_active() && render_object_vr_at_controls(unit_index))
#endif
				)
			{
				struct unit_datum *unit= unit_get(unit_index);
				struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
				struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

				if (weapon_definition->weapon.interface_definition.first_person_animations.index!=NONE)
				{
					struct real_matrix4x3 model_node_matrices[MAXIMUM_NODES_PER_ANIMATION];
					struct render_model_effect model_effect;
					struct game_globals_first_person_interface *first_person_interface=
						TAG_BLOCK_GET_ELEMENT(
							&scenario_get_game_globals()->first_person_interface,
							0,
							struct game_globals_first_person_interface);
					struct animation_graph *animation_graph= animation_graph_definition_get(
						weapon_definition->weapon.interface_definition.first_person_animations.index);
					struct render_lighting *lighting= object_get_cached_render_lighting(unit_index, FLT_MAX);

					model_effect.modifier_shader= NULL;
					if (TEST_FLAG(unit->unit.flags, _unit_active_camouflaged_bit) ||
						unit->unit.active_camouflage>0.0f)
					{
						model_effect.type= _render_model_effect_type_active_camouflage;
						model_effect.intensity= unit->unit.active_camouflage;
						model_effect.parameter= unit->unit.active_camouflage_super_amount;
						model_effect.source_object_index= unit_index;
						model_effect.source_object_centroid= render.camera.position;
					}
					else
					{
						model_effect.type= _render_model_effect_type_none;
					}

					if (first_person_weapon->weapon_node_remapping_table_valid &&
						weapon_definition->weapon.interface_definition.first_person_model.index!=NONE)
					{
						model_remap_node_matrices_to_match_animation_graph(
							weapon_definition->weapon.interface_definition.first_person_model.index,
							model_node_matrices,
							weapon_definition->weapon.interface_definition.first_person_animations.index,
							first_person_weapon->node_matrices,
							first_person_weapon->weapon_node_remapping_table);
						render_model(
							weapon_definition->weapon.interface_definition.first_person_model.index,
							0.0f,
							model_node_matrices,
							NULL,
							weapon->object.outgoing_change_colors,
							weapon->object.outgoing_function_values,
							lighting,
							&render.camera.position,
							0.0f,
							&model_effect,
							first_person_weapon->weapon_index,
							0,
							FLAG(_render_model_first_person_bit));
					}

					if (first_person_weapon->hands_node_remapping_table_valid &&
						first_person_interface->hands.index!=NONE)
					{
						model_remap_node_matrices_to_match_animation_graph(
							first_person_interface->hands.index,
							model_node_matrices,
							weapon_definition->weapon.interface_definition.first_person_animations.index,
							first_person_weapon->node_matrices,
							first_person_weapon->hands_node_remapping_table);
#ifdef HALO_ANDROID
						/* (vr.log_performance: once, how its skin hangs from its
						bones, for the VR arms) */
						{
							static int log_skin= -1;

							if (log_skin<0)
								log_skin= config_boolean("vr.log_performance")!=0;
							if (log_skin)
								model_vr_log_skin(first_person_interface->hands.index);
						}
#endif
#ifdef HALO_ANDROID
						/* VR: the weapon alone (vr.body "off") */
						if (halo_vr_body_mode()!=HALO_VR_BODY_OFF)
#endif
						render_model(
							first_person_interface->hands.index,
							0.0f,
							model_node_matrices,
							NULL,
							unit->object.outgoing_change_colors,
							unit->object.outgoing_function_values,
							lighting,
							&render.camera.position,
							0.0f,
							&model_effect,
							first_person_weapon->weapon_index,
							0,
							FLAG(_render_model_first_person_bit));
					}
#ifdef HALO_ANDROID
					if (halo_vr_active())
						first_person_weapon_draw_held_grenade(unit_index, lighting, &model_effect);
#endif
				}
			}
		}
	}

	return;
}

short first_person_weapon_get_marker_by_name(
	long weapon_index,
	char const *name,
	struct object_marker *markers,
	short maximum_marker_count)
{
	short marker_count= 0;
	struct weapon_datum *weapon= weapon_try_and_get(weapon_index);

	if (weapon)
	{
		short local_player_index= first_person_weapon_index_from_weapon_index(weapon_index);

		if (local_player_index!=NONE &&
			director_get_perspective(local_player_index)==_director_perspective_first_person)
		{
			struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
			struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

			if (first_person_weapon->weapon_node_remapping_table_valid &&
				weapon_definition->weapon.interface_definition.first_person_model.index!=NONE &&
				weapon_definition->weapon.interface_definition.first_person_animations.index!=NONE)
			{
				marker_count= model_get_marker_by_name(
					weapon_definition->weapon.interface_definition.first_person_model.index,
					name,
					NULL,
					first_person_weapon->weapon_node_remapping_table,
					(short)animation_graph_definition_get(
						weapon_definition->weapon.interface_definition.first_person_animations.index)->nodes.count,
					first_person_weapon->node_matrices,
					FALSE,
					markers,
					maximum_marker_count);
			}
		}
	}

	return marker_count;
}

void first_person_weapon_render_update(
	void)
{
	if (render.local_player_index!=NONE)
	{
		struct first_person_weapon *first_person_weapon= first_person_weapon_get(render.local_player_index);

		if (first_person_weapon->unit_index!=NONE &&
			first_person_weapon->weapon_index!=NONE)
		{
			boolean visible= director_get_perspective(render.local_player_index)==_director_perspective_first_person &&
				player_control_get_zoom_level(render.local_player_index)==NONE;
#ifdef HALO_ANDROID
			/* VR: the weapon stays in the hand zoomed in (the scope is on it) */
			if (halo_vr_active())
				visible= director_get_perspective(render.local_player_index)==_director_perspective_first_person;
#endif

			first_person_weapon_set_visibility(render.local_player_index, visible);
			if (first_person_weapon->visible)
			{
#ifdef HALO_ANDROID
				unsigned long long timing= halo_vr_timing_start();

				/* VR: posed from the controller, the pose is the same for
				every eye (and the scope's view): built once a frame */
				if (first_person_weapon_vr_pose_kept(render.local_player_index))
				{
					halo_vr_timing_end(HALO_VR_TIMING_FIRST_PERSON_KEPT, timing);
				}
				else
				{
					first_person_weapon_build_node_matrices(render.local_player_index);
					halo_vr_timing_end(HALO_VR_TIMING_FIRST_PERSON, timing);
				}
#else
				first_person_weapon_build_node_matrices(render.local_player_index);
#endif
			}
		}
	}

	return;
}

short first_person_weapon_get_marker_by_name_render(
	long weapon_index,
	char const *name,
	struct object_marker *markers,
	short maximum_marker_count)
{
	if (render.local_player_index==first_person_weapon_index_from_weapon_index(weapon_index))
	{
		return first_person_weapon_get_marker_by_name(
			weapon_index,
			name,
			markers,
			maximum_marker_count);
	}

	return 0;
}

void first_person_weapon_center_flashlight(
	long unit_index,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up)
{
	short local_player_index= first_person_weapon_index_from_unit_index(unit_index);

	if (local_player_index!=NONE)
	{
		struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

		if (first_person_weapon->visible)
		{
			struct object_marker marker;

			if (first_person_weapon_get_marker_by_name(
				first_person_weapon->weapon_index,
				"flashlight",
				&marker,
				1)>0)
			{
				position->x= marker.matrix.position.x - marker.matrix.forward.i*0.5f;
				position->y= marker.matrix.position.y - marker.matrix.forward.j*0.5f;
				position->z= marker.matrix.position.z - marker.matrix.forward.k*0.5f;
				*forward= marker.matrix.forward;
				*up= marker.matrix.up;
			}
		}
	}

	return;
}

boolean first_person_weapon_adjust_light(
	long weapon_index,
	char const *marker_name,
	real_point3d *position,
	real_vector3d *forward,
	real_vector3d *up)
{
	struct weapon_datum *weapon= weapon_get(weapon_index);
	struct unit_datum *unit= unit_get(weapon->object.parent_object_index);
	long player_index= unit->unit.player_index;
	boolean adjusted= FALSE;

	if (player_index!=NONE)
	{
		struct player_datum *player= player_get(player_index);
		short local_player_index= player->local_player_index;
		struct object_marker marker;

		if (local_player_index!=NONE &&
			local_player_index==render.local_player_index &&
			first_person_weapon_get(local_player_index)->visible &&
			first_person_weapon_get_marker_by_name(weapon_index, marker_name, &marker, 1)>0)
		{
			*position= marker.matrix.position;
			*forward= marker.matrix.forward;
			*up= marker.matrix.up;
			adjusted= TRUE;
		}
	}

	return adjusted;
}

void first_person_weapon_message_from_unit(
	long unit_index,
	short message_type)
{
	short local_player_index= first_person_weapon_index_from_unit_index(unit_index);

	first_person_weapon_message(local_player_index, message_type);
	if (local_player_index==NONE)
	{
		struct unit_datum *unit= unit_get(unit_index);

		/* port: the weapon in hand's object (not its slot's number, which
		played some other object's sound) */
		if (unit->unit.current_weapon_index>=0 &&
			unit->unit.current_weapon_index<MAXIMUM_WEAPONS_PER_UNIT)
		{
			weapon_play_first_person_weapon_sound(
				unit->unit.weapon_object_indices[unit->unit.current_weapon_index],
				message_type);
		}
	}

	return;
}

void first_person_weapon_message_from_weapon(
	long weapon_index,
	short message_type)
{
	short local_player_index= first_person_weapon_index_from_weapon_index(weapon_index);

	first_person_weapon_message(local_player_index, message_type);
	if (local_player_index==NONE)
	{
		weapon_play_first_person_weapon_sound(weapon_index, message_type);
	}

	return;
}

struct real_matrix4x3 *first_person_weapon_get_node_matrix(
	short local_player_index,
	short node_index)
{
	struct first_person_weapon *first_person_weapon;
	struct weapon_datum *weapon;
	struct weapon_definition *weapon_definition;
	struct animation_graph *animation_graph;

	match_assert(
		"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
		1433,
		local_player_index>=0 && local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS);
	first_person_weapon = &first_person_weapons[local_player_index];
	/* port: the last matrices built when the weapon is gone (an effect or
	particle on it asking before first_person_weapon_forget_weapon stopped
	them was a NULL weapon, and a crash) */
	weapon = first_person_weapon->weapon_index!=NONE ? weapon_try_and_get(first_person_weapon->weapon_index) : NULL;
	if (!weapon)
	{
		return &first_person_weapon->node_matrices[
			node_index>=0 && node_index<MAXIMUM_NODES_PER_ANIMATION ? node_index : 0];
	}
	weapon_definition = weapon_definition_get(weapon->definition_index);
	animation_graph = animation_graph_definition_get(
		weapon_definition->weapon.interface_definition.first_person_animations.index);
	match_assert(
		"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
		718,
		node_index>=0 && node_index<animation_graph->nodes.count);

	return &first_person_weapon->node_matrices[node_index];
}

void first_person_weapons_update(
	void)
{
	short local_player_index;

	for (local_player_index=0; local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index= local_player_get_player_index(local_player_index);

		if (player_index!=NONE)
		{
			struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
			long unit_index= player_get(player_index)->unit_index;

			if (first_person_weapon->unit_index!=unit_index)
			{
				first_person_weapon_new_unit(local_player_index, unit_index);
			}
			if (first_person_weapon->weapon_index==NONE)
			{
				first_person_weapon_switch_weapons(local_player_index);
			}
			first_person_weapon_update(local_player_index);
		}
	}

	return;
}

/* ---------- private code */

static void first_person_weapon_set_visibility(
	short local_player_index,
	boolean visible)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

	if (visible!=first_person_weapon->visible)
	{
		if (visible)
		{
			match_assert(
				"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
				1555,
				first_person_weapon->weapon_index!=NONE);
			effects_start_on_first_person_weapon(local_player_index, first_person_weapon->weapon_index);
		}
		else
		{
			effects_stop_on_first_person_weapon(local_player_index);
			particles_stop_on_first_person_weapon(local_player_index);
		}
		first_person_weapon->visible= visible;
	}

	return;
}

/* port: the weapon dropped or deleted: hidden first, which stops the effects
and particles that follow it in first person (they asked for the weapon's
nodes, and found none, until the next update switched weapons; the
renderer shows the next weapon again) */
static void first_person_weapon_forget_weapon(
	short local_player_index)
{
	first_person_weapon_set_visibility(local_player_index, FALSE);
	first_person_weapon_get(local_player_index)->weapon_index= NONE;

	return;
}

static void first_person_weapon_predict(
	short local_player_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

	if (first_person_weapon->weapon_index!=NONE)
	{
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
		struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

		predicted_resources_precache(&weapon_definition->weapon.predicted_resources);
	}
	first_person_weapon->ticks_until_predict= TICKS_PER_SECOND;

	return;
}

static void first_person_weapon_start_interpolation(
	short local_player_index,
	short frame_count)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
	struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
	struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);
	struct animation_graph *animation_graph= animation_graph_definition_get(
		weapon_definition->weapon.interface_definition.first_person_animations.index);

	csmemcpy(
		first_person_weapon->original_node_orientations,
		first_person_weapon->node_orientations,
		animation_graph->nodes.count*sizeof(real_orientation));
	if (frame_count>=first_person_weapon->interpolation_frame_count-
		first_person_weapon->interpolation_frame_index)
	{
		first_person_weapon->interpolation_frame_index= 0;
		first_person_weapon->interpolation_frame_count= frame_count;
	}

	return;
}

static void first_person_weapon_set_state(
	short local_player_index,
	short new_state,
	boolean reset_sounds)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

	if (first_person_weapon->weapon_index!=NONE)
	{
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);

		if (TEST_FLAG(weapon->weapon.flags, _weapon_overheated_bit))
		{
			switch (new_state)
			{
				case _first_person_weapon_state_ready:
					new_state= _first_person_weapon_state_overheating_again;
					break;

				case _first_person_weapon_state_throw_grenade:
					new_state= _first_person_weapon_state_throw_grenade_overheated;
					break;
			}
		}
	}

	switch (new_state)
	{
		case _first_person_weapon_state_ready:
			if (first_person_weapon->state==_first_person_weapon_state_ready)
			{
				new_state= NONE;
			}
			break;

		case _first_person_weapon_state_light_off:
		case _first_person_weapon_state_light_on:
			if (first_person_weapon->state!=_first_person_weapon_state_idle &&
				first_person_weapon->state!=_first_person_weapon_state_posing)
			{
				new_state= NONE;
			}
			break;

		case _first_person_weapon_state_primary_fire:
		case _first_person_weapon_state_secondary_fire:
		case _first_person_weapon_state_primary_misfire:
		case _first_person_weapon_state_secondary_misfire:
			if (first_person_weapon->state!=_first_person_weapon_state_idle &&
				first_person_weapon->state!=_first_person_weapon_state_posing &&
				first_person_weapon->state!=_first_person_weapon_state_primary_fire &&
				first_person_weapon->state!=_first_person_weapon_state_charged &&
				first_person_weapon->state!=_first_person_weapon_state_shotgun_enter_reload &&
				first_person_weapon->state!=_first_person_weapon_state_overheated_exit &&
				first_person_weapon->state!=_first_person_weapon_state_shotgun_exit_reload_empty &&
				first_person_weapon->state!=_first_person_weapon_state_shotgun_exit_reload_full &&
				first_person_weapon->state!=_first_person_weapon_state_reload_while_empty &&
				first_person_weapon->state!=_first_person_weapon_state_reload_while_full)
			{
				new_state= NONE;
			}
			break;
	}

	if (new_state!=NONE && first_person_weapon->weapon_index!=NONE)
	{
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
		struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);
		short animation_type;
		short interpolation_frame_count;

		if (weapon_definition->weapon.weapon_type==_weapon_type_plasma_pistol &&
			new_state==_first_person_weapon_state_overheated &&
			!TEST_FLAG(weapon->weapon.flags, _weapon_overheated_bit))
		{
			new_state= _first_person_weapon_state_idle;
		}
		animation_type= (short)first_person_animation_type_from_weapon_state(new_state);
		if (weapon_definition->weapon.weapon_type==_weapon_type_shotgun &&
			first_person_weapon->state==_first_person_weapon_state_shotgun_exit_reload_empty)
		{
			interpolation_frame_count= 0;
		}
		else
		{
			switch (new_state)
			{
				case _first_person_weapon_state_overheated:
				case _first_person_weapon_state_melee:
				case _first_person_weapon_state_ready:
					interpolation_frame_count= 0;
					break;

				case _first_person_weapon_state_primary_fire:
				case _first_person_weapon_state_secondary_fire:
				case _first_person_weapon_state_primary_misfire:
				case _first_person_weapon_state_secondary_misfire:
					interpolation_frame_count= 3;
					break;

				default:
					interpolation_frame_count= 6;
					break;
			}
		}

		if (first_person_weapon->unit_index!=NONE &&
			first_person_weapon->weapon_index!=NONE)
		{
			struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
			struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);
			struct animation_graph *animation_graph= animation_graph_definition_get(
				weapon_definition->weapon.interface_definition.first_person_animations.index);

			if (animation_graph->first_person_weapon_animations.count)
			{
				struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
					TAG_BLOCK_GET_ELEMENT(
						&animation_graph->first_person_weapon_animations,
						0,
						struct animation_graph_first_person_weapon_animations);

				if (first_person_weapon_animations &&
					animation_type>=0 &&
					animation_type<first_person_weapon_animations->animations.count)
				{
					short animation_index= animation_graph_animation_index_get(
						&first_person_weapon_animations->animations)[animation_type].animation_index;

					if (animation_index!=NONE)
					{
						if (reset_sounds &&
							first_person_weapon->current_sound_index!=NONE &&
							first_person_weapon->current_sound_state!=_first_person_weapon_state_overheating)
						{
							sound_stop_impulse(first_person_weapon->current_sound_index);
							first_person_weapon->current_sound_index= NONE;
							first_person_weapon->current_sound_state= NONE;
						}
						if (interpolation_frame_count>0)
						{
							first_person_weapon_start_interpolation(
								local_player_index,
								interpolation_frame_count);
						}
						first_person_weapon->state= new_state;
						first_person_weapon->state_animation.index= animation_index;
						first_person_weapon->state_animation.frame_index= 0;
					}
				}
			}
		}
	}

	return;
}

#ifdef HALO_ANDROID
/* ---------- VR arms

The first-person skeleton hangs from the camera at the weapon hand's
controller (halo_vr_weapon_camera), so the weapon and the right hand lie
where they should; its arms, though, reach up to shoulders that float with
the weapon. Here each arm's upper arm and forearm are turned (two-bone
inverse kinematics, port/linux/game/vr_arms.c) to run from the player's
shoulders, below the neck and square to the torso, to its wrist, the
controller's roll shared along the forearm. The left wrist, unless the hand
holds the weapon's foregrip, goes to the free controller, its fingers with
it.

Left-handed (vr.handedness) all this is done in model space (halo_vr.h): a
mirror image of the room, where the left controller holds the weapon as a
right hand would; the body is mirrored into it, and the posed skeleton
mirrored back (vr_mirror_pose), so that the weapon lies in the left hand,
the model's left arm reaches to the right controller. */

#define VR_METRES_PER_WORLD_UNIT 3.048f

/* a point or a vector across the mirror (a point on it, its normal) */
static void vr_mirror_point(
	float const on[3],
	float const normal[3],
	float point[3])
{
	float along = (point[0] - on[0]) * normal[0] + (point[1] - on[1]) * normal[1] + (point[2] - on[2]) * normal[2];

	point[0] -= 2.f * along * normal[0];
	point[1] -= 2.f * along * normal[1];
	point[2] -= 2.f * along * normal[2];
}

static void vr_mirror_vector(
	float const normal[3],
	float vector[3])
{
	float along = vector[0] * normal[0] + vector[1] * normal[1] + vector[2] * normal[2];

	vector[0] -= 2.f * along * normal[0];
	vector[1] -= 2.f * along * normal[1];
	vector[2] -= 2.f * along * normal[2];
}

/* a node mirrored across the plane through on with the normal, its axes too */
static void vr_mirror_node(
	real_matrix4x3 *node,
	float const on[3],
	float const normal[3])
{
	real_vector3d *axes[3];
	float vector[3];
	short axis;

	vector[0] = node->position.x;
	vector[1] = node->position.y;
	vector[2] = node->position.z;
	vr_mirror_point(on, normal, vector);
	node->position.x = vector[0];
	node->position.y = vector[1];
	node->position.z = vector[2];
	axes[0] = &node->forward;
	axes[1] = &node->left;
	axes[2] = &node->up;
	for (axis = 0; axis < 3; axis++)
	{
		vector[0] = axes[axis]->i;
		vector[1] = axes[axis]->j;
		vector[2] = axes[axis]->k;
		vr_mirror_vector(normal, vector);
		axes[axis]->i = vector[0];
		axes[axis]->j = vector[1];
		axes[axis]->k = vector[2];
	}
}

/* the posed skeleton out of model space: every node mirrored, its axes too
(a left-handed frame: rasterizer_xbox.c turns its triangles' winding over).
But the ammo display (the assault rifle's, its node "... display" and what
hangs from it): mirrored back about its own centre, so it stays where the
mirrored gun has it but reads the right way round, and keeps its winding
(after HaloCE-Quest-VR, CC0) */
static void vr_mirror_pose(
	short local_player_index,
	long graph_index,
	real_matrix4x3 *node_matrices,
	short node_count)
{
	short display;
	float on[3], normal[3];
	short node_index;

	if (!halo_vr_first_person_mirror(local_player_index, on, normal))
		return;
	for (node_index = 0; node_index < node_count; node_index++)
		vr_mirror_node(&node_matrices[node_index], on, normal);
	display = graph_index != NONE ? animation_graph_find_node(graph_index, "display") : NONE;
	if (display >= 0 && display < node_count)
	{
		float centre[3];

		centre[0] = node_matrices[display].position.x;
		centre[1] = node_matrices[display].position.y;
		centre[2] = node_matrices[display].position.z;
		for (node_index = 0; node_index < node_count; node_index++)
		{
			short ancestor = node_index;

			/* (up through the parents, each before its child: the root's
			may name itself, which hung the game, Frame 2026-10-06) */
			while (ancestor >= 0 && ancestor != display)
			{
				short parent = animation_graph_node_parent(graph_index, ancestor);

				ancestor = parent >= 0 && parent < ancestor ? parent : NONE;
			}
			if (ancestor == display)
				vr_mirror_node(&node_matrices[node_index], centre, normal);
		}
	}
}

struct vr_arm_nodes
{
	long graph_index;
	short upper[2];
	short fore[2];
	short wrist[2];
	short gun;
	/* the left hand's knuckles: index, middle and little finger */
	short left_index;
	short left_middle;
	short left_pinky;
	long barrel_logged_graph_index;
	/* the fingers' joints (vr_finger_nodes) of the skeleton they were
	found in: each hand's thumb, index, middle, ring, little finger */
	long fingers_graph_index;
	short fingers[2][5][3];
	/* the whole pose in the weapon camera's frame, outside a melee attack */
	boolean idle_pose_valid;
	short idle_pose_count;
	real_matrix4x3 idle_pose[MAXIMUM_NODES_PER_ANIMATION];
};

static struct vr_arm_nodes vr_arm_nodes = { NONE, { NONE, NONE }, { NONE, NONE }, { NONE, NONE }, NONE, NONE, NONE, NONE,
	NONE, NONE };
/* the skeleton this frame, and each arm's last (its elbow's swing and its
forearm's twist run on from them) */
static struct vr_skeleton vr_arm_skeleton;
static struct vr_limb_history vr_arm_history[2];

static void vr_vector_scale_add(
	real_vector3d *out,
	real_vector3d const *vector,
	real scale)
{
	out->i += vector->i * scale;
	out->j += vector->j * scale;
	out->k += vector->k * scale;
}

/* turns the vector by the least rotation that takes from to to (unit) */
static void vr_rotate_between(
	real_vector3d const *from,
	real_vector3d const *to,
	real_vector3d *v)
{
	real_vector3d axis;
	real_vector3d turned;
	real sine, cosine, along;

	cross_product3d(from, to, &axis);
	sine = normalize3d(&axis);
	if (sine < 1e-6f)
		return;
	cosine = dot_product3d(from, to);
	cross_product3d(&axis, v, &turned);
	along = dot_product3d(&axis, v);
	v->i = v->i * cosine + turned.i * sine + axis.i * along * (1.f - cosine);
	v->j = v->j * cosine + turned.j * sine + axis.j * along * (1.f - cosine);
	v->k = v->k * cosine + turned.k * sine + axis.k * along * (1.f - cosine);
}

/* a node turned so that the bone to its child, from old to new direction */
static void vr_turn_bone(
	real_matrix4x3 *node,
	real_vector3d const *old_direction,
	real_vector3d const *new_direction,
	real_point3d const *position)
{
	real_vector3d from = *old_direction;
	real_vector3d to = *new_direction;

	normalize3d(&from);
	normalize3d(&to);
	vr_rotate_between(&from, &to, &node->forward);
	vr_rotate_between(&from, &to, &node->left);
	vr_rotate_between(&from, &to, &node->up);
	node->position = *position;
}

/* the upper arm turned about the elbow to point at the shoulder, keeping
its length (the free left arm: vr_left_hand_free) */
static void vr_point_upper_arm(
	real_matrix4x3 *node_matrices,
	short upper,
	short fore,
	real_point3d const *shoulder)
{
	real_vector3d old_direction, new_direction;
	real_point3d position;
	real length;

	vector_from_points3d(&node_matrices[upper].position, &node_matrices[fore].position, &old_direction);
	vector_from_points3d(shoulder, &node_matrices[fore].position, &new_direction);
	length = magnitude3d(&old_direction);
	if (length < 1e-4f || normalize3d(&new_direction) < 1e-4f)
		return;
	point_from_line3d(&node_matrices[fore].position, &new_direction, -length, &position);
	vr_turn_bone(&node_matrices[upper], &old_direction, &new_direction, &position);
}

/* an orthonormal frame from a direction and a second one across it */
static void vr_frame_from(
	real_vector3d const *direction,
	real_vector3d const *across,
	real_vector3d frame[3])
{
	frame[0] = *direction;
	normalize3d(&frame[0]);
	frame[1] = *across;
	vr_vector_scale_add(&frame[1], &frame[0], -dot_product3d(across, &frame[0]));
	normalize3d(&frame[1]);
	cross_product3d(&frame[0], &frame[1], &frame[2]);
}

/* the free left hand: the wrist behind the controller's grip, the fingers
along the controller and the knuckles, little finger to index, up its
handle, as a hand holds it; its fingers with it. The arm is bent to it
from the shoulder afterwards (vr_solve_arm), or, rigid (vr.arm_ik off),
follows the hand as it is shaped on the foregrip. (The arm's skin hangs
from the arm's bones alone, none from the skeleton's root: 3a.0's log.) */
static void vr_left_hand_free(
	struct vr_skeleton const *skeleton,
	real_matrix4x3 *node_matrices,
	boolean rigid,
	float const hand_position[3],
	float const hand_forward[3],
	float const hand_up[3])
{
	short wrist_index = vr_arm_nodes.wrist[0];
	real_matrix4x3 old_wrist = node_matrices[wrist_index];
	real_matrix4x3 *wrist = &node_matrices[wrist_index];
	real_vector3d fingers, knuckles, controller_forward, controller_up;
	real_vector3d from[3], to[3];
	real_vector3d *axes[3];
	short axis;

	if (vr_arm_nodes.left_index == NONE || vr_arm_nodes.left_middle == NONE || vr_arm_nodes.left_pinky == NONE)
		return;
	vector_from_points3d(&old_wrist.position, &node_matrices[vr_arm_nodes.left_middle].position, &fingers);
	vector_from_points3d(&node_matrices[vr_arm_nodes.left_pinky].position,
		&node_matrices[vr_arm_nodes.left_index].position, &knuckles);
	controller_forward.i = hand_forward[0];
	controller_forward.j = hand_forward[1];
	controller_forward.k = hand_forward[2];
	controller_up.i = hand_up[0];
	controller_up.j = hand_up[1];
	controller_up.k = hand_up[2];
	vr_frame_from(&fingers, &knuckles, from);
	vr_frame_from(&controller_forward, &controller_up, to);
	axes[0] = &wrist->forward;
	axes[1] = &wrist->left;
	axes[2] = &wrist->up;
	for (axis = 0; axis < 3; axis++)
	{
		real_vector3d old_axis = *axes[axis];
		real x = dot_product3d(&old_axis, &from[0]);
		real y = dot_product3d(&old_axis, &from[1]);
		real z = dot_product3d(&old_axis, &from[2]);

		axes[axis]->i = to[0].i * x + to[1].i * y + to[2].i * z;
		axes[axis]->j = to[0].j * x + to[1].j * y + to[2].j * z;
		axes[axis]->k = to[0].k * x + to[1].k * y + to[2].k * z;
	}
	wrist->position.x = hand_position[0];
	wrist->position.y = hand_position[1];
	wrist->position.z = hand_position[2];
	point_from_line3d(&wrist->position, &to[0], -0.07f / VR_METRES_PER_WORLD_UNIT, &wrist->position);
	/* what the wrist carries (its fingers) with it, and rigid, the forearm
	and the upper arm too, as they are on the foregrip */
	{
		real_matrix4x3 inverse, delta, moved;
		short node_index;

		matrix4x3_inverse(&old_wrist, &inverse);
		matrix4x3_multiply(wrist, &inverse, &delta);
		for (node_index = 0; node_index < skeleton->node_count; node_index++)
		{
			if (node_index == wrist_index ||
				!(vr_skeleton_under(skeleton, node_index, wrist_index) ||
				(rigid && (node_index == vr_arm_nodes.fore[0] || node_index == vr_arm_nodes.upper[0]))))
			{
				continue;
			}
			matrix4x3_multiply(&delta, &node_matrices[node_index], &moved);
			node_matrices[node_index] = moved;
		}
	}
}

/* Through a melee attack the skeleton keeps the pose it had before, in the
hand: the attack's animation swings the gun before the camera and moves
both hands about it (the right one to the stock, the left one onto the
barrel), and the swing is the player's own. The pose is kept while the
weapon is held ready (store), and held through the attack (melee). */
static void vr_hold_pose(
	real_matrix4x3 *node_matrices,
	short node_count,
	struct render_camera const *weapon_camera,
	boolean store,
	boolean melee)
{
	real_matrix4x3 camera_matrix;
	real_matrix4x3 inverse;
	short node_index;

	if (node_count > MAXIMUM_NODES_PER_ANIMATION)
		return;
	matrix4x3_from_point_and_vectors(&camera_matrix, &weapon_camera->position, &weapon_camera->forward,
		&weapon_camera->up);
	if (store)
	{
		matrix4x3_inverse(&camera_matrix, &inverse);
		for (node_index = 0; node_index < node_count; node_index++)
			matrix4x3_multiply(&inverse, &node_matrices[node_index], &vr_arm_nodes.idle_pose[node_index]);
		vr_arm_nodes.idle_pose_count = node_count;
		vr_arm_nodes.idle_pose_valid = TRUE;
	}
	else if (melee && vr_arm_nodes.idle_pose_valid && vr_arm_nodes.idle_pose_count == node_count)
	{
		for (node_index = 0; node_index < node_count; node_index++)
			matrix4x3_multiply(&camera_matrix, &vr_arm_nodes.idle_pose[node_index], &node_matrices[node_index]);
	}
}

/* While the animation turns the weapon away from where it aims (a reload,
drawing or putting it away), the laser follows the barrel: the gun node's
turn from its ready pose, applied to the laser at the ready. */
static void vr_laser_follows_barrel(
	short local_player_index,
	real_matrix4x3 const *node_matrices,
	struct render_camera const *weapon_camera)
{
	real_matrix4x3 camera_matrix, ready_gun, inverse, delta;
	real_point3d start, moved_start;
	real_vector3d direction, moved_direction;
	float laser_start[3], laser_direction[3];

	if (vr_arm_nodes.gun == NONE || !vr_arm_nodes.idle_pose_valid ||
		!halo_vr_ready_laser(local_player_index, laser_start, laser_direction))
	{
		return;
	}
	{
		/* (the laser at the ready is the room's: into model space) */
		float on[3], normal[3];

		if (halo_vr_first_person_mirror(local_player_index, on, normal))
		{
			vr_mirror_point(on, normal, laser_start);
			vr_mirror_vector(normal, laser_direction);
		}
	}
	matrix4x3_from_point_and_vectors(&camera_matrix, &weapon_camera->position, &weapon_camera->forward,
		&weapon_camera->up);
	matrix4x3_multiply(&camera_matrix, &vr_arm_nodes.idle_pose[vr_arm_nodes.gun], &ready_gun);
	matrix4x3_inverse(&ready_gun, &inverse);
	matrix4x3_multiply(&node_matrices[vr_arm_nodes.gun], &inverse, &delta);
	start.x = laser_start[0];
	start.y = laser_start[1];
	start.z = laser_start[2];
	direction.i = laser_direction[0];
	direction.j = laser_direction[1];
	direction.k = laser_direction[2];
	matrix4x3_transform_point(&delta, &start, &moved_start);
	matrix4x3_transform_vector(&delta, &direction, &moved_direction);
	normalize3d(&moved_direction);
	laser_start[0] = moved_start.x;
	laser_start[1] = moved_start.y;
	laser_start[2] = moved_start.z;
	laser_direction[0] = moved_direction.i;
	laser_direction[1] = moved_direction.j;
	laser_direction[2] = moved_direction.k;
	halo_vr_set_laser(local_player_index, laser_start, laser_direction);
}

/* The fingers (vr.fingers): the free left hand's from its controller's
trigger, grip and touch (vr_pose_fingers); of the hand on the weapon, only
the index finger, lifted a little off the trigger when the controller
says it is (the rest keeps the weapon's grip). */
static void first_person_weapon_vr_fingers(
	short local_player_index,
	real_matrix4x3 *node_matrices,
	boolean left_free,
	float const hand_forward[3],
	float const hand_up[3])
{
	float curls[4];
	int fingers;

	if (vr_arm_nodes.fingers_graph_index != vr_arm_skeleton.graph_index)
	{
		vr_arm_nodes.fingers_graph_index = vr_arm_skeleton.graph_index;
		vr_finger_nodes(&vr_arm_skeleton, vr_arm_nodes.wrist[0], "l ", vr_arm_nodes.fingers[0]);
		vr_finger_nodes(&vr_arm_skeleton, vr_arm_nodes.wrist[1], "r ", vr_arm_nodes.fingers[1]);
	}
	if (left_free && vr_arm_nodes.wrist[0] != NONE &&
		halo_vr_finger_curls(local_player_index, 0, curls))
	{
		/* the palm's frame: along the fingers, up the knuckles, and out of
		the left palm (inwards, to the right) */
		real_vector3d forward, up, palm;
		real along;

		forward.i = hand_forward[0];
		forward.j = hand_forward[1];
		forward.k = hand_forward[2];
		up.i = hand_up[0];
		up.j = hand_up[1];
		up.k = hand_up[2];
		along = dot_product3d(&forward, &up);
		vr_vector_scale_add(&up, &forward, -along);
		if (normalize3d(&forward) > 1e-4f && normalize3d(&up) > 1e-4f)
		{
			cross_product3d(&forward, &up, &palm);
			vr_pose_fingers(&vr_arm_skeleton, node_matrices, vr_arm_nodes.wrist[0], vr_arm_nodes.fingers[0], curls,
				&palm, &forward, &up);
		}
	}
	fingers = halo_vr_finger_curls(local_player_index, 1, curls);
	if (vr_arm_nodes.wrist[1] != NONE && (fingers & HALO_VR_FINGERS_TOUCH))
	{
		/* (resting on the trigger the index curls 0.22 or more) */
		vr_straighten_finger(&vr_arm_skeleton, node_matrices, vr_arm_nodes.fingers[1][1],
			0.6f * PIN(1.f - curls[1] / 0.22f, 0.f, 1.f));
	}
}

static void first_person_weapon_vr_arms(
	short local_player_index,
	long graph_index,
	real_matrix4x3 *node_matrices,
	short node_count,
	struct render_camera const *weapon_camera,
	short state)
{
	/* a grenade's throw is the player's own too */
	boolean melee = state==_first_person_weapon_state_melee ||
		state==_first_person_weapon_state_throw_grenade ||
		state==_first_person_weapon_state_throw_grenade_overheated;
	/* held ready: firing or not, the gun where it aims */
	boolean ready = state==_first_person_weapon_state_idle ||
		state==_first_person_weapon_state_primary_fire ||
		state==_first_person_weapon_state_secondary_fire ||
		state==_first_person_weapon_state_primary_misfire ||
		state==_first_person_weapon_state_secondary_misfire;
	static char const *const upper_names[2] = { "l upperarm", "r upperarm" };
	static char const *const fore_names[2] = { "l forearm", "r forearm" };
	static char const *const wrist_names[2] = { "l wriste", "r wriste" };
	float head[3], neck[3], body_forward[3];
	real_vector3d forward, left, weapon_left, weapon_right;
	real_point3d eye;
	real_matrix4x3 animated_upper[2];
	real_matrix4x3 animated_forearm[2];
	real_matrix4x3 animated_wrist[2];
	short side;
	boolean gripping;
	boolean left_free;
	boolean rigid = !halo_vr_free_arm_bends();
	int body = halo_vr_body_mode();
	float hand_position[3], hand_forward[3], hand_up[3];

	if (vr_arm_nodes.graph_index != graph_index)
	{
		vr_arm_nodes.graph_index = graph_index;
		for (side = 0; side < 2; side++)
		{
			vr_arm_nodes.upper[side] = animation_graph_find_node(graph_index, upper_names[side]);
			vr_arm_nodes.fore[side] = animation_graph_find_node(graph_index, fore_names[side]);
			vr_arm_nodes.wrist[side] = animation_graph_find_node(graph_index, wrist_names[side]);
		}
		vr_arm_nodes.gun = animation_graph_find_node(graph_index, "gun");
		vr_arm_nodes.left_index = animation_graph_find_node(graph_index, "l index low");
		vr_arm_nodes.left_middle = animation_graph_find_node(graph_index, "l middlelow");
		vr_arm_nodes.left_pinky = animation_graph_find_node(graph_index, "l pinky low");
		vr_arm_nodes.idle_pose_valid = FALSE;
	}
	vr_hold_pose(node_matrices, node_count, weapon_camera, ready, melee);
	if (vr_arm_nodes.gun != NONE && vr_arm_nodes.idle_pose_valid)
	{
		/* the gun at the ready, as drawn: where the weapon's shots and its
		crosshair start (in a reload, where it was at the ready) */
		real_matrix4x3 camera_matrix;
		real_matrix4x3 const *ready_gun = &vr_arm_nodes.idle_pose[vr_arm_nodes.gun];
		real_vector3d const *axes[3];
		real_point3d gun;
		float barrel[3], direction[3];
		real best = 0.f;
		short axis;

		matrix4x3_from_point_and_vectors(&camera_matrix, &weapon_camera->position, &weapon_camera->forward,
			&weapon_camera->up);
		matrix4x3_transform_point(&camera_matrix, &ready_gun->position, &gun);
		barrel[0] = gun.x;
		barrel[1] = gun.y;
		barrel[2] = gun.z;
		/* the barrel: the gun node's axis nearest the camera's forward, in
		the camera's frame (forward, left, up) */
		axes[0] = &ready_gun->forward;
		axes[1] = &ready_gun->left;
		axes[2] = &ready_gun->up;
		direction[0] = 1.f;
		direction[1] = direction[2] = 0.f;
		for (axis = 0; axis < 3; axis++)
		{
			real along = axes[axis]->i;

			if (along * along > best * best)
			{
				real length = magnitude3d(axes[axis]);
				real sign = along < 0.f ? -1.f : 1.f;

				best = along;
				if (length > 0.f)
				{
					direction[0] = sign * axes[axis]->i / length;
					direction[1] = sign * axes[axis]->j / length;
					direction[2] = sign * axes[axis]->k / length;
				}
			}
		}
		halo_vr_set_barrel(local_player_index, barrel, direction);
		if (vr_arm_nodes.barrel_logged_graph_index != graph_index)
		{
			/* once per weapon (debug.txt) */
			error(2, "VR barrel: %.1f degrees off the weapon camera; gun node %.2f %.2f %.2f m from it (forward, left, up)",
				arccosine(PIN(direction[0], -1.f, 1.f)) * 180.f / _pi,
				ready_gun->position.x * VR_METRES_PER_WORLD_UNIT, ready_gun->position.y * VR_METRES_PER_WORLD_UNIT,
				ready_gun->position.z * VR_METRES_PER_WORLD_UNIT);
			vr_arm_nodes.barrel_logged_graph_index = graph_index;
		}
	}
	if (!ready && !melee)
		vr_laser_follows_barrel(local_player_index, node_matrices, weapon_camera);
	if (!halo_vr_body_frame(local_player_index, head, neck, body_forward) ||
		!vr_skeleton_build(&vr_arm_skeleton, graph_index, node_count))
	{
		return;
	}
	{
		/* (the body is the room's: into model space, where the shoulders
		change sides) */
		float on[3], normal[3];

		if (halo_vr_first_person_mirror(local_player_index, on, normal))
		{
			vr_mirror_point(on, normal, head);
			vr_mirror_point(on, normal, neck);
			vr_mirror_vector(normal, body_forward);
		}
	}
	/* (the shoulders hang from the neck, which the head turns about) */
	eye.x = neck[0];
	eye.y = neck[1];
	eye.z = neck[2];
	forward.i = body_forward[0];
	forward.j = body_forward[1];
	forward.k = 0.f;
	cross_product3d(global_up3d, &forward, &left);
	cross_product3d(&weapon_camera->up, &weapon_camera->forward, &weapon_left);
	weapon_right.i = -weapon_left.i;
	weapon_right.j = -weapon_left.j;
	weapon_right.k = -weapon_left.k;

	/* the animated arms, for the forearms' twist */
	for (side = 0; side < 2; side++)
	{
		if (vr_arm_nodes.fore[side] != NONE && vr_arm_nodes.wrist[side] != NONE)
		{
			animated_forearm[side] = node_matrices[vr_arm_nodes.fore[side]];
			animated_wrist[side] = node_matrices[vr_arm_nodes.wrist[side]];
			if (vr_arm_nodes.upper[side] != NONE)
				animated_upper[side] = node_matrices[vr_arm_nodes.upper[side]];
		}
	}

	/* the left hand on the foregrip, or at the controller */
	gripping = FALSE;
	left_free = FALSE;
	if (vr_arm_nodes.wrist[0] != NONE && vr_arm_nodes.wrist[1] != NONE)
	{
		real_matrix4x3 const *left_wrist = &node_matrices[vr_arm_nodes.wrist[0]];
		real_matrix4x3 const *right_wrist = &node_matrices[vr_arm_nodes.wrist[1]];
		real_vector3d between;
		float foregrip[3], reference[3];

		vector_from_points3d(&right_wrist->position, &left_wrist->position, &between);
		foregrip[0] = left_wrist->position.x;
		foregrip[1] = left_wrist->position.y;
		foregrip[2] = left_wrist->position.z;
		reference[0] = dot_product3d(&between, &weapon_camera->forward);
		reference[1] = dot_product3d(&between, &weapon_right);
		reference[2] = dot_product3d(&between, &weapon_camera->up);
		gripping = halo_vr_two_hand(local_player_index, foregrip, reference);
	}
	/* (the weapon alone: no hands to pose) */
	if (body == HALO_VR_BODY_OFF)
		return;
	if (!gripping && vr_arm_nodes.wrist[0] != NONE)
	{
		if (halo_vr_free_hand(local_player_index, hand_position, hand_forward, hand_up))
		{
			/* (rigid: the hand and its arm carried to the controller now;
			bent, the arm is solved to it first, below, and the hand turned
			after, as HaloCE-Quest-VR does: turned first, the solve's swing
			turned it on and the arm's skin stretched) */
			if (rigid)
				vr_left_hand_free(&vr_arm_skeleton, node_matrices, rigid, hand_position, hand_forward, hand_up);
			left_free = TRUE;
		}
	}

	for (side = 0; side < 2; side++)
	{
		real_point3d shoulder;
		real_vector3d pole;
		short bones[NUMBER_OF_VR_ARM_BONES];
		real outward = side ? -1.f : 1.f; /* left is +left */

		if (vr_arm_nodes.upper[side] == NONE || vr_arm_nodes.fore[side] == NONE || vr_arm_nodes.wrist[side] == NONE)
			continue;

		/* the shoulder: below the eyes, behind them (a little behind the
		neck: in front of it, the open end of the sleeve came into view
		looking down to the side), to the side, square to the torso */
		shoulder = eye;
		point_from_line3d(&shoulder, global_up3d, -0.20f / VR_METRES_PER_WORLD_UNIT, &shoulder);
		point_from_line3d(&shoulder, &forward, -0.04f / VR_METRES_PER_WORLD_UNIT, &shoulder);
		point_from_line3d(&shoulder, &left, outward * 0.19f / VR_METRES_PER_WORLD_UNIT, &shoulder);
		/* the elbow bends down, out and back */
		pole.i = -global_up3d->i;
		pole.j = -global_up3d->j;
		pole.k = -global_up3d->k;
		vr_vector_scale_add(&pole, &left, outward * 0.6f);
		vr_vector_scale_add(&pole, &forward, -0.3f);
		/* and as the hand rolls: an elbow lifted out to the side rolls the
		controller's top in, an elbow tucked in rolls it out. Its underside
		(away from the thumb) leads the elbow: the right hand's the weapon's,
		the free left hand's its controller's (the hand on the foregrip
		keeps the default) */
		{
			real_vector3d below;
			boolean rolls = FALSE;

			if (side)
			{
				below.i = -weapon_camera->up.i;
				below.j = -weapon_camera->up.j;
				below.k = -weapon_camera->up.k;
				rolls = TRUE;
			}
			else if (left_free)
			{
				below.i = -hand_up[0];
				below.j = -hand_up[1];
				below.k = -hand_up[2];
				rolls = TRUE;
			}
			if (rolls && normalize3d(&pole) > 0.f)
			{
				vr_vector_scale_add(&pole, &below, 1.5f);
				if (normalize3d(&pole) <= 0.f)
				{
					pole.i = -global_up3d->i;
					pole.j = -global_up3d->j;
					pole.k = -global_up3d->k;
				}
			}
		}
		bones[_vr_arm_upper] = vr_arm_nodes.upper[side];
		bones[_vr_arm_fore] = vr_arm_nodes.fore[side];
		bones[_vr_arm_hand] = vr_arm_nodes.wrist[side];
		/* (the hands alone: the arms are shrunk away below) */
		if (body == HALO_VR_BODY_HANDS)
			continue;
		if (!side && left_free && rigid)
		{
			/* the free left arm keeps its shape (vr_left_hand_free), but
			for the upper arm, which points back at the shoulder */
			vr_point_upper_arm(node_matrices, vr_arm_nodes.upper[side], vr_arm_nodes.fore[side], &shoulder);
			continue;
		}
		if (!side && left_free)
		{
			/* the free left arm bent to the controller's grip (the wrist
			7 cm behind it), the hand turned to the controller after */
			real_point3d target;

			target.x = hand_position[0] - hand_forward[0] * 0.07f / VR_METRES_PER_WORLD_UNIT;
			target.y = hand_position[1] - hand_forward[1] * 0.07f / VR_METRES_PER_WORLD_UNIT;
			target.z = hand_position[2] - hand_forward[2] * 0.07f / VR_METRES_PER_WORLD_UNIT;
			vr_solve_arm(&vr_arm_skeleton, node_matrices, bones, shoulder, &target, &pole, FALSE,
				&vr_arm_history[side]);
			vr_left_hand_free(&vr_arm_skeleton, node_matrices, FALSE, hand_position, hand_forward, hand_up);
		}
		else
		{
			/* the hand stays where it is (on the weapon): the arm bends to
			it */
			vr_solve_arm(&vr_arm_skeleton, node_matrices, bones, shoulder,
				&node_matrices[bones[_vr_arm_hand]].position, &pole, TRUE, &vr_arm_history[side]);
		}
		vr_align_elbow(node_matrices, bones, &animated_upper[side], &animated_forearm[side], &animated_wrist[side]);
		vr_distribute_arm_twist(node_matrices, bones, &animated_forearm[side], &animated_wrist[side],
			&vr_arm_history[side]);
	}
	first_person_weapon_vr_fingers(local_player_index, node_matrices, left_free, hand_forward, hand_up);
	if (body == HALO_VR_BODY_HANDS)
	{
		for (side = 0; side < 2; side++)
		{
			short bones[NUMBER_OF_VR_ARM_BONES];

			bones[_vr_arm_upper] = vr_arm_nodes.upper[side];
			bones[_vr_arm_fore] = vr_arm_nodes.fore[side];
			bones[_vr_arm_hand] = vr_arm_nodes.wrist[side];
			if (bones[_vr_arm_upper] != NONE && bones[_vr_arm_fore] != NONE && bones[_vr_arm_hand] != NONE)
				vr_hide_arm(&vr_arm_skeleton, node_matrices, bones, vr_arm_nodes.fingers[side], vr_arm_nodes.gun);
		}
	}
}

#endif
static void first_person_weapon_build_node_matrices(
	short local_player_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
	struct render_camera const *camera= &render.camera;
#ifdef HALO_ANDROID
	/* VR: posed from the weapon hand's controller, so the weapon lies in the
	hand (port/linux/src/vr.c) */
	struct render_camera weapon_camera;
	float weapon_position[3], weapon_forward[3], weapon_up[3];

	if (first_person_weapon->weapon_index!=NONE)
	{
		/* once per skeleton: its nodes, for the arms (debug.txt) */
		static long logged_graph_index= NONE;
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
		long graph_index= weapon_definition_get(weapon->definition_index)->weapon.interface_definition.first_person_animations.index;

		if (graph_index!=NONE && graph_index!=logged_graph_index)
		{
			short node_index;

			logged_graph_index= graph_index;
			for (node_index= 0; node_index<animation_graph_node_count(graph_index); node_index++)
			{
				error(2, "first-person node %d '%s' parent %d", node_index,
					animation_graph_node_name(graph_index, node_index),
					animation_graph_node_parent(graph_index, node_index));
			}
		}
	}
	if (halo_vr_weapon_camera(local_player_index, weapon_position, weapon_forward, weapon_up))
	{
		weapon_camera= render.camera;
		weapon_camera.position.x= weapon_position[0];
		weapon_camera.position.y= weapon_position[1];
		weapon_camera.position.z= weapon_position[2];
		weapon_camera.forward.i= weapon_forward[0];
		weapon_camera.forward.j= weapon_forward[1];
		weapon_camera.forward.k= weapon_forward[2];
		weapon_camera.up.i= weapon_up[0];
		weapon_camera.up.j= weapon_up[1];
		weapon_camera.up.k= weapon_up[2];
		camera= &weapon_camera;
	}
#endif

	if (!first_person_weapon->rendered)
	{
		euler_angles2d_from_vector3d(&first_person_weapon->render_facing, &camera->forward);
		first_person_weapon->render_position= camera->position;
	}
	/* The native ports draw several frames per tick. The turning sway
	(first_person_weapon_update) takes the facing change since the last
	frame as a tick's worth: move the last facing on once a tick. */
	{
		static long last_render_ticks[MAXIMUM_LOCAL_PLAYERS] = { NONE, NONE, NONE, NONE };

		if (!halo_interpolation_enabled() || last_render_ticks[local_player_index] != game_time_get())
		{
			first_person_weapon->last_render_facing= first_person_weapon->render_facing;
			first_person_weapon->last_render_position= first_person_weapon->render_position;
			last_render_ticks[local_player_index]= game_time_get();
		}
	}
	euler_angles2d_from_vector3d(&first_person_weapon->render_facing, &camera->forward);
	first_person_weapon->render_position= camera->position;
	first_person_weapon->render_forward= camera->forward;
	first_person_weapon->rendered= TRUE;

	if (first_person_weapon->weapon_index!=NONE &&
		!weapon_try_and_get(first_person_weapon->weapon_index))
	{
		error(
			_error_log,
			"local player %d, weapon (0x%x), deleted unexpectedly",
			local_player_index,
			first_person_weapon->weapon_index);
		first_person_weapon_forget_weapon(local_player_index);
	}

	if (first_person_weapon->weapon_index!=NONE)
	{
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
		struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);
		struct model *model= model_definition_get(
			weapon_definition->weapon.interface_definition.first_person_model.index);
		struct animation_graph *animation_graph= animation_graph_definition_get(
			weapon_definition->weapon.interface_definition.first_person_animations.index);

		if (animation_graph->first_person_weapon_animations.count)
		{
			struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
				TAG_BLOCK_GET_ELEMENT(
					&animation_graph->first_person_weapon_animations,
					0,
					struct animation_graph_first_person_weapon_animations);

			if (first_person_weapon_animations)
			{
				short firing_animation_index;
				short overlay_animation_index;

				if (first_person_weapon->state_animation.index!=NONE)
				{
					struct animation *state_animation= TAG_BLOCK_GET_ELEMENT(
						&animation_graph->animations,
						first_person_weapon->state_animation.index,
						struct animation);

					animation_get_node_orientations(
						NULL,
						state_animation,
						first_person_weapon->state_animation.frame_index,
						first_person_weapon->node_orientations);
					/* port: a PAL map's animation, slowed to the NTSC maps' pace,
					between the frame it is on and the next: held on the frame, it
					stood still for that tick, and a reload moved in fits and starts
					(port/linux/game/pal_tags.c) */
					{
						real fraction= pal_tags_first_person_fraction(
							local_player_index,
							weapon_definition->weapon.interface_definition.first_person_animations.index,
							first_person_weapon->state_animation.index,
							first_person_weapon->state_animation.frame_index);

						if (fraction>0.0f &&
							first_person_weapon->state_animation.frame_index+1<state_animation->frame_count)
						{
							real_orientation next_node_orientations[MAXIMUM_NODES_PER_ANIMATION];
							short node_index;

							animation_get_node_orientations(
								NULL,
								state_animation,
								(short)(first_person_weapon->state_animation.frame_index+1),
								next_node_orientations);
							for (node_index= 0; node_index<state_animation->node_count; node_index++)
							{
								orientations_interpolate(
									&first_person_weapon->node_orientations[node_index],
									&next_node_orientations[node_index],
									fraction,
									&first_person_weapon->node_orientations[node_index]);
							}
						}
					}
				}
				else
				{
					match_vassert(
						"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
						1294,
						FALSE,
						NULL);
					model_get_node_orientations(model, first_person_weapon->node_orientations);
				}

				firing_animation_index= VALID_INDEX(
					_first_person_weapon_animation_ammunition,
					first_person_weapon_animations->animations.count) ?
					animation_graph_animation_index_get(
						&first_person_weapon_animations->animations)
							[_first_person_weapon_animation_ammunition].animation_index :
					(short)NONE;
				if (firing_animation_index!=NONE)
				{
					struct animation *firing_animation= TAG_BLOCK_GET_ELEMENT(
						&animation_graph->animations,
						firing_animation_index,
						struct animation);

					if (weapon_definition->weapon.weapon_type==_weapon_type_needler &&
						(first_person_weapons->state==_first_person_weapon_state_reload_while_empty ||
						first_person_weapons->state==_first_person_weapon_state_reload_while_full))
					{
						short frame_index= weapon->weapon.magazines[0].rounds_loaded;
						short settle_ticks= weapon->weapon.magazines[0].original_time-
							weapon->weapon.magazines[0].state_timer;

						if (settle_ticks>=FIRST_PERSON_WEAPON_RELOAD_SETTLE_TICKS)
						{
							struct weapon_magazine_definition *magazine_definition= TAG_BLOCK_GET_ELEMENT(
								&weapon_definition->weapon.magazines,
								0,
								struct weapon_magazine_definition);
							real settle_fraction= (settle_ticks-FIRST_PERSON_WEAPON_RELOAD_SETTLE_TICKS)*0.2f;
							short predicted_frame_index;

							if (settle_fraction>1.0f)
							{
								settle_fraction= 1.0f;
							}
							predicted_frame_index= (short)MIN(
								weapon->weapon.magazines[0].rounds_total,
								magazine_definition->rounds_loaded_maximum);
							frame_index= (short)((predicted_frame_index-frame_index)*settle_fraction)+
								frame_index;
						}
						overlay_animation_apply(
							firing_animation,
							frame_index,
							first_person_weapon->node_orientations);
					}
					else if (weapon->weapon.magazines[0].rounds_loaded<firing_animation->frame_count)
					{
						overlay_animation_apply(
							firing_animation,
							weapon->weapon.magazines[0].rounds_loaded,
							first_person_weapon->node_orientations);
					}
				}

				if (first_person_weapon->moving_animation.index!=NONE)
				{
					struct animation *moving_animation= TAG_BLOCK_GET_ELEMENT(
						&animation_graph->animations,
						first_person_weapon->moving_animation.index,
						struct animation);

					overlay_animation_apply(
						moving_animation,
						first_person_weapon->moving_animation.frame_index,
						first_person_weapon->node_orientations);
				}

				if (first_person_weapon->overcharged_jitter_animation.index!=NONE)
				{
					struct animation *overcharged_jitter_animation= TAG_BLOCK_GET_ELEMENT(
						&animation_graph->animations,
						first_person_weapon->overcharged_jitter_animation.index,
						struct animation);

					overlay_animation_apply_continuous_scaled(
						overcharged_jitter_animation,
						first_person_weapon->overcharged_jitter_animation.frame_index,
						weapon->weapon.overcharged+0.5f,
						first_person_weapon->node_orientations);
				}

				overlay_animation_index= VALID_INDEX(
					_first_person_weapon_animation_overlays,
					first_person_weapon_animations->animations.count) ?
					animation_graph_animation_index_get(
						&first_person_weapon_animations->animations)
							[_first_person_weapon_animation_overlays].animation_index :
					(short)NONE;
				if (overlay_animation_index!=NONE)
				{
					struct animation *overlay_animation= TAG_BLOCK_GET_ELEMENT(
						&animation_graph->animations,
						overlay_animation_index,
						struct animation);

					if (overlay_animation->frame_count>=NUMBER_OF_FIRST_PERSON_WEAPON_OVERLAY_FRAMES)
					{
						if (first_person_weapon->position.i>0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								0,
								first_person_weapon->position.i,
								first_person_weapon->node_orientations);
						}
						else if (first_person_weapon->position.i<0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								1,
								-first_person_weapon->position.i,
								first_person_weapon->node_orientations);
						}

						if (first_person_weapon->position.j>0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								3,
								first_person_weapon->position.j,
								first_person_weapon->node_orientations);
						}
						else if (first_person_weapon->position.j<0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								2,
								-first_person_weapon->position.j,
								first_person_weapon->node_orientations);
						}

						if (first_person_weapon->turning.i>0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								4,
								first_person_weapon->turning.i,
								first_person_weapon->node_orientations);
						}
						else if (first_person_weapon->turning.i<0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								5,
								-first_person_weapon->turning.i,
								first_person_weapon->node_orientations);
						}

						if (first_person_weapon->turning.j>0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								7,
								first_person_weapon->turning.j,
								first_person_weapon->node_orientations);
						}
						else if (first_person_weapon->turning.j<0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								6,
								-first_person_weapon->turning.j,
								first_person_weapon->node_orientations);
						}

						if (first_person_weapon->firing_push_back>0.0f)
						{
							overlay_animation_apply_scaled(
								overlay_animation,
								8,
								first_person_weapon->firing_push_back,
								first_person_weapon->node_orientations);
						}
					}
				}

				if (first_person_weapon->interpolation_frame_count>0)
				{
					interpolate_node_orientations(
						(short)animation_graph->nodes.count,
						first_person_weapon->original_node_orientations,
						first_person_weapon->node_orientations,
						first_person_weapon->interpolation_frame_index,
						first_person_weapon->interpolation_frame_count);
				}
			}
		}

		animation_graph_node_matrices_from_orientations(
			weapon_definition->weapon.interface_definition.first_person_animations.index,
			first_person_weapon->node_matrices,
			first_person_weapon->node_orientations,
			&camera->position,
			&camera->forward,
			&camera->up);
		/* the pose between the last two ticks (render_interpolation.c) */
		render_interpolation_first_person(
			local_player_index,
			first_person_weapon->node_matrices,
			(short)animation_graph->nodes.count,
			camera);
#ifdef HALO_ANDROID
		first_person_weapon_vr_pose_serial[local_player_index]= camera!=&render.camera ? halo_vr_frame_serial() : 0;
		first_person_weapon_vr_pose_weapon[local_player_index]= first_person_weapon->weapon_index;
		if (camera!=&render.camera)
		{
			unsigned long long timing= halo_vr_timing_start();

			first_person_weapon_vr_arms(
				local_player_index,
				weapon_definition->weapon.interface_definition.first_person_animations.index,
				first_person_weapon->node_matrices,
				(short)animation_graph->nodes.count,
				camera,
				first_person_weapon->state);
			/* (left-handed: out of model space; what the frame takes from
			the pose after, markers, effects, is then the room's) */
			vr_mirror_pose(local_player_index,
				weapon_definition->weapon.interface_definition.first_person_animations.index,
				first_person_weapon->node_matrices,
				(short)MIN(animation_graph->nodes.count, MAXIMUM_NODES_PER_ANIMATION));
			halo_vr_timing_end(HALO_VR_TIMING_ARMS, timing);
		}
#endif
	}

	return;
}

static void first_person_weapon_message(
	short local_player_index,
	short message_type)
{
	if (local_player_index!=NONE)
	{
		struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
		short state= NONE;

		switch (message_type)
		{
			case _first_person_weapon_message_drop:
				first_person_weapon_forget_weapon(local_player_index);
				break;

			case _first_person_weapon_message_ready:
				first_person_weapon_switch_weapons(local_player_index);
				break;

			case _first_person_weapon_message_primary_fire:
				first_person_weapon->firing_push_back_velocity+=
					FIRST_PERSON_WEAPON_FIRING_PUSH_BACK_VELOCITY;
				break;

			case _first_person_weapon_message_reload_while_empty:
			case _first_person_weapon_message_reload_while_full:
				player_control_unzoom(first_person_weapon->unit_index);
				break;
		}

		if (first_person_weapon->weapon_index!=NONE)
		{
			struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);

			if (weapon->definition_index!=NONE)
			{
				struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

				switch (weapon_definition->weapon.weapon_type)
				{
					case _weapon_type_shotgun:
						if (message_type==_first_person_weapon_message_reload_while_empty ||
							message_type==_first_person_weapon_message_reload_while_full)
						{
							struct weapon_magazine_definition *magazine_definition= TAG_BLOCK_GET_ELEMENT(
								&weapon_definition->weapon.magazines,
								0,
								struct weapon_magazine_definition);
							short rounds_loaded= weapon->weapon.magazines[0].rounds_loaded;
							short rounds_total= weapon->weapon.magazines[0].rounds_total;

							if (first_person_weapon->state==_first_person_weapon_state_shotgun_enter_reload ||
								first_person_weapon->state==_first_person_weapon_state_overheated_exit ||
								first_person_weapon->state==_first_person_weapon_state_shotgun_exit_reload_empty ||
								first_person_weapon->state==_first_person_weapon_state_shotgun_exit_reload_full ||
								first_person_weapon->state==_first_person_weapon_state_reload_while_empty ||
								first_person_weapon->state==_first_person_weapon_state_reload_while_full ||
								weapon->weapon.magazines[0].state)
							{
								if (MIN(magazine_definition->rounds_loaded_maximum-rounds_loaded, rounds_total)==1)
								{
									first_person_weapon->shotgun_reload_type= _shotgun_reload_type_last_round;
								}
								else
								{
									first_person_weapon->shotgun_reload_type= NONE;
								}
							}
							else
							{
								short shells_to_reload= (short)MIN(
									magazine_definition->rounds_loaded_maximum-rounds_loaded,
									rounds_total);

								first_person_weapon->shotgun_shells_to_reload= shells_to_reload;
								first_person_weapon->shotgun_empty= rounds_loaded==0;
								first_person_weapon->shotgun_reload_type= (shells_to_reload!=1) ?
									_shotgun_reload_type_first_round :
									_shotgun_reload_type_first_and_last_round;
							}

							switch (first_person_weapon->shotgun_reload_type)
							{
								case _shotgun_reload_type_first_round:
								case _shotgun_reload_type_first_and_last_round:
									state= _first_person_weapon_state_shotgun_enter_reload;
									break;

								case NONE:
									state= _first_person_weapon_state_reload_while_empty;
									break;
							}
						}
						break;
				}
			}
		}

		if (state==NONE)
		{
			state= (short)first_person_weapon_state_from_weapon_message(message_type);
		}
		if (state!=NONE)
		{
			first_person_weapon_set_state(local_player_index, state, TRUE);
		}
		switch (message_type)
		{
			case _first_person_weapon_message_ready:
				first_person_weapon->interpolation_frame_count= 0;
				break;
		}
	}

	return;
}

static void first_person_weapon_switch_weapons(
	short local_player_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
	boolean visible= first_person_weapon->visible;

	first_person_weapon->weapon_index= NONE;
	if (visible)
	{
		first_person_weapon_set_visibility(local_player_index, FALSE);
	}

	if (first_person_weapon->unit_index!=NONE)
	{
		struct unit_datum *unit= unit_get(first_person_weapon->unit_index);
		long weapon_index= unit_inventory_get_weapon(
			first_person_weapon->unit_index,
			unit->unit.current_weapon_index);

		if (weapon_index!=NONE)
		{
			struct weapon_datum *weapon= weapon_get(weapon_index);
			struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

			if (weapon_definition->weapon.interface_definition.first_person_model.index!=NONE &&
				weapon_definition->weapon.interface_definition.first_person_animations.index!=NONE)
			{
				struct animation_graph *animation_graph= animation_graph_definition_get(
					weapon_definition->weapon.interface_definition.first_person_animations.index);

				if (animation_graph->first_person_weapon_animations.count)
				{
					struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
						TAG_BLOCK_GET_ELEMENT(
							&animation_graph->first_person_weapon_animations,
							0,
							struct animation_graph_first_person_weapon_animations);

					if (first_person_weapon_animations)
					{
						struct game_globals_first_person_interface *first_person_interface;

						first_person_weapon->overlay_animation_index= NONE;
						if (VALID_INDEX(
							_first_person_weapon_animation_overlays,
							first_person_weapon_animations->animations.count))
						{
							short overlay_animation_index= animation_graph_animation_index_get(
								&first_person_weapon_animations->animations)
									[_first_person_weapon_animation_overlays].animation_index;

							if (overlay_animation_index!=NONE &&
								TAG_BLOCK_GET_ELEMENT(
									&animation_graph->animations,
									overlay_animation_index,
									struct animation)->frame_count>=
										NUMBER_OF_FIRST_PERSON_WEAPON_OVERLAY_FRAMES)
							{
								first_person_weapon->overlay_animation_index= overlay_animation_index;
							}
						}

						first_person_interface= TAG_BLOCK_GET_ELEMENT(
							&scenario_get_game_globals()->first_person_interface,
							0,
							struct game_globals_first_person_interface);
						if (first_person_interface->hands.index!=NONE)
						{
							first_person_weapon->hands_node_remapping_table_valid=
								model_build_remapping_table_for_animation_graph(
									first_person_interface->hands.index,
									weapon_definition->weapon.interface_definition.first_person_animations.index,
									first_person_weapon->hands_node_remapping_table);
						}
						first_person_weapon->weapon_node_remapping_table_valid=
							model_build_remapping_table_for_animation_graph(
								weapon_definition->weapon.interface_definition.first_person_model.index,
								weapon_definition->weapon.interface_definition.first_person_animations.index,
								first_person_weapon->weapon_node_remapping_table);

						if (first_person_weapon->weapon_node_remapping_table_valid &&
							first_person_weapon->hands_node_remapping_table_valid)
						{
							first_person_weapon->weapon_index= weapon_index;
							first_person_weapon->state= NONE;
							first_person_weapon->state_animation.index= NONE;
							first_person_weapon->moving_animation.index= NONE;
							first_person_weapon->overcharged_jitter_animation.index= NONE;
							first_person_weapon->firing_push_back= 0.0f;
							first_person_weapon->firing_push_back_velocity= 0.0f;
							first_person_weapon->ticks_idle= 0;
							first_person_weapon->current_sound_index= NONE;
							first_person_weapon->current_sound_state= NONE;
							first_person_weapon_set_state(
								local_player_index,
								_first_person_weapon_state_idle,
								TRUE);
							first_person_weapon->interpolation_frame_count= 0;
							if (visible)
							{
								first_person_weapon_set_visibility(local_player_index, TRUE);
							}
						}
					}
				}
			}
		}
	}
	first_person_weapon_predict(local_player_index);

	return;
}

static void first_person_weapon_new_unit(
	short local_player_index,
	long unit_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

	first_person_weapon->rendered= FALSE;
	first_person_weapon->unit_index= unit_index;
	first_person_weapon_switch_weapons(local_player_index);

	return;
}

static void first_person_weapon_next_state(
	short local_player_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);
	short new_state= NONE;

	switch (first_person_weapon->state)
	{
		case _first_person_weapon_state_idle:
		case _first_person_weapon_state_posing:
		case _first_person_weapon_state_primary_fire:
		case _first_person_weapon_state_secondary_fire:
		case _first_person_weapon_state_primary_misfire:
		case _first_person_weapon_state_secondary_misfire:
		case _first_person_weapon_state_melee:
		case _first_person_weapon_state_light_off:
		case _first_person_weapon_state_light_on:
		case _first_person_weapon_state_shotgun_exit_reload_empty:
		case _first_person_weapon_state_shotgun_exit_reload_full:
		case _first_person_weapon_state_ready:
		case _first_person_weapon_state_throw_grenade:
		case _first_person_weapon_state_overheated_exit:
			new_state= _first_person_weapon_state_idle;
			break;

		case _first_person_weapon_state_overheating:
		case _first_person_weapon_state_overheating_again:
		case _first_person_weapon_state_throw_grenade_overheated:
		case _first_person_weapon_state_overheating_super_recoil:
			new_state= _first_person_weapon_state_overheated;
			break;

		case _first_person_weapon_state_put_away:
			first_person_weapon->state_animation.frame_index--;
			break;

		case _first_person_weapon_state_shotgun_enter_reload:
		{
			struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
			struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

			if (weapon_definition->weapon.weapon_type==_weapon_type_shotgun &&
				first_person_weapon->shotgun_reload_type==_shotgun_reload_type_first_and_last_round)
			{
				new_state= first_person_weapon->shotgun_empty ?
					_first_person_weapon_state_shotgun_exit_reload_empty :
					_first_person_weapon_state_shotgun_exit_reload_full;
			}
			else
			{
				new_state= _first_person_weapon_state_idle;
			}
			break;
		}

		case _first_person_weapon_state_reload_while_empty:
		case _first_person_weapon_state_reload_while_full:
		{
			struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
			struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

			if (weapon_definition->weapon.weapon_type!=_weapon_type_shotgun ||
				!first_person_weapon->shotgun_reload_type ||
				first_person_weapon->shotgun_reload_type==NONE)
			{
				new_state= _first_person_weapon_state_idle;
			}
			else
			{
				match_assert(
					"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
					799,
					first_person_weapon->shotgun_reload_type == _shotgun_reload_type_first_and_last_round ||
						first_person_weapon->shotgun_reload_type == _shotgun_reload_type_last_round);
				new_state= first_person_weapon->shotgun_empty ?
					_first_person_weapon_state_shotgun_exit_reload_empty :
					_first_person_weapon_state_shotgun_exit_reload_full;
			}
			break;
		}
	}

	if (new_state!=NONE)
	{
		first_person_weapon_set_state(local_player_index, new_state, FALSE);
	}

	return;
}

static void first_person_weapon_update(
	short local_player_index)
{
	struct first_person_weapon *first_person_weapon= first_person_weapon_get(local_player_index);

	if (first_person_weapon->weapon_index!=NONE &&
		!weapon_try_and_get(first_person_weapon->weapon_index))
	{
		error(
			_error_log,
			"local player %d, weapon (0x%x), deleted unexpectedly",
			local_player_index,
			first_person_weapon->weapon_index);
		first_person_weapon_forget_weapon(local_player_index);
	}

	if (first_person_weapon->unit_index!=NONE && first_person_weapon->weapon_index!=NONE)
	{
		struct unit_datum *unit= unit_get(first_person_weapon->unit_index);
		struct weapon_datum *weapon= weapon_get(first_person_weapon->weapon_index);
		struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);
		struct animation_graph *animation_graph;
		long sound_definition_index;
		short animation_update_result;
		boolean moving;

		model_definition_get(weapon_definition->weapon.interface_definition.first_person_model.index);
		animation_graph= animation_graph_definition_get(
			weapon_definition->weapon.interface_definition.first_person_animations.index);

		if (first_person_weapon->state==_first_person_weapon_state_overheated ||
			first_person_weapon->state==_first_person_weapon_state_overheating)
		{
			if (TEST_FLAG(weapon->weapon.flags, _weapon_overheated_exit_bit))
			{
				first_person_weapon_set_state(
					local_player_index,
					_first_person_weapon_state_overheated_exit,
					TRUE);
			}
			if (!TEST_FLAG(weapon->weapon.flags, _weapon_overheated_bit))
			{
				first_person_weapon_set_state(
					local_player_index,
					_first_person_weapon_state_idle,
					TRUE);
			}
		}

		/* port: a PAL map's first-person animation at the NTSC maps' pace,
		which the weapon's timing keeps (port/linux/game/pal_tags.c) */
		if (!pal_tags_first_person_advance(local_player_index,
			weapon_definition->weapon.interface_definition.first_person_animations.index,
			first_person_weapon->state_animation.index, first_person_weapon->state_animation.frame_index))
		{
			animation_update_result= _animation_no_key_frame;
			sound_definition_index= NONE;
		}
		else
		animation_update_result= animation_update_render_only(
			weapon_definition->weapon.interface_definition.first_person_animations.index,
			&first_person_weapon->state_animation,
			&sound_definition_index);
		if (animation_update_result==_animation_key_frame)
		{
			/* a key frame does not advance the first-person weapon state */
		}
		else if (animation_update_result==_animation_will_restart_on_next_frame)
		{
			first_person_weapon_next_state(local_player_index);
		}

		if (sound_definition_index!=NONE &&
			director_get_perspective(local_player_index)==_director_perspective_first_person)
		{
			first_person_weapon->current_sound_index= object_impulse_sound_new(
				first_person_weapon->weapon_index,
				sound_definition_index,
				NONE,
				global_origin3d,
				global_forward3d,
				1.0f);
			first_person_weapon->current_sound_state= first_person_weapon->state;
		}

		moving= magnitude3d(&unit->unit.throttle)>0.1f;
		if (unit_flying_through_air(first_person_weapon->unit_index))
		{
			moving= FALSE;
		}

		if (first_person_weapon->moving_animation.index!=NONE)
		{
			animation_update_render_only(
				weapon_definition->weapon.interface_definition.first_person_animations.index,
				&first_person_weapon->moving_animation,
				NULL);
			if (!moving)
			{
				if (first_person_weapon->state==_first_person_weapon_state_idle)
				{
					first_person_weapon_start_interpolation(local_player_index, 6);
				}
				first_person_weapon->moving_animation.index= NONE;
			}
		}
		else if (moving)
		{
			struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
				!animation_graph->first_person_weapon_animations.count ?
					NULL :
					TAG_BLOCK_GET_ELEMENT(
						&animation_graph->first_person_weapon_animations,
						0,
						struct animation_graph_first_person_weapon_animations);

			first_person_weapon->moving_animation.frame_index= 0;
			/* no NULL test (January, and the later first-party build at /Od 0x61fcea): a non-NONE
			 * weapon_index is only committed by first_person_weapon_switch_weapons inside
			 * `if (animation_graph->first_person_weapon_animations.count)` for this same graph. The
			 * new-map reset leaves weapon_index 0 but unit_index NONE, and first_person_weapon_new_unit
			 * runs switch_weapons (which stores NONE first) as soon as it sets unit_index, so the NULL
			 * arm above cannot reach this read. */
			first_person_weapon->moving_animation.index=
				VALID_INDEX(
					_first_person_weapon_animation_moving,
					first_person_weapon_animations->animations.count) ?
					animation_graph_animation_index_get(
						&first_person_weapon_animations->animations)
							[_first_person_weapon_animation_moving].animation_index :
					(short)NONE;
		}

		if (first_person_weapon->overcharged_jitter_animation.index==NONE)
		{
			if (first_person_weapon->state==_first_person_weapon_state_charged)
			{
				struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
					!animation_graph->first_person_weapon_animations.count ?
						NULL :
						TAG_BLOCK_GET_ELEMENT(
							&animation_graph->first_person_weapon_animations,
							0,
							struct animation_graph_first_person_weapon_animations);

				first_person_weapon->overcharged_jitter_animation.frame_index= 0.0f;
				/* no NULL test: the same switch_weapons invariant as the moving animation above
				 * (January and /Od 0x61fd96). */
				first_person_weapon->overcharged_jitter_animation.index=
					VALID_INDEX(
						_first_person_weapon_animation_overcharged_jitter,
						first_person_weapon_animations->animations.count) ?
						animation_graph_animation_index_get(
							&first_person_weapon_animations->animations)
								[_first_person_weapon_animation_overcharged_jitter].animation_index :
						(short)NONE;
			}
		}
		else if (first_person_weapon->state==_first_person_weapon_state_charged)
		{
			struct animation *animation= TAG_BLOCK_GET_ELEMENT(
				&animation_graph->animations,
				first_person_weapon->overcharged_jitter_animation.index,
				struct animation);

			first_person_weapon->overcharged_jitter_animation.frame_index= (real)fmod(
				(weapon->weapon.overcharged+1.0f)*2.0f+
					first_person_weapon->overcharged_jitter_animation.frame_index,
				(real)animation->frame_count);
		}
		else
		{
			first_person_weapon->overcharged_jitter_animation.index= NONE;
		}

		if (first_person_weapon->rendered)
		{
			real_vector2d turning;

			accelerate_to_position(
				&first_person_weapon->position.i,
				&first_person_weapon->position_velocity.i,
				unit->unit.throttle.i,
				0.08f,
				0.5f,
				-1.0f,
				1.0f,
				FALSE);
			accelerate_to_position(
				&first_person_weapon->position.j,
				&first_person_weapon->position_velocity.j,
				unit->unit.throttle.j,
				0.08f,
				0.5f,
				-1.0f,
				1.0f,
				FALSE);

			turning.i= signed_angular_difference(
				first_person_weapon->last_render_facing.yaw,
				first_person_weapon->render_facing.yaw)*30.0f;
			turning.j= signed_angular_difference(
				first_person_weapon->last_render_facing.pitch,
				first_person_weapon->render_facing.pitch)*-30.0f;

			turning.i= PIN(turning.i, -1.0f, 1.0f);
			turning.j= PIN(turning.j, -1.0f, 1.0f);

			accelerate_to_position(
				&first_person_weapon->turning.i,
				&first_person_weapon->turning_velocity.i,
				turning.i,
				0.03f,
				0.2f,
				-1.0f,
				1.0f,
				FALSE);
			accelerate_to_position(
				&first_person_weapon->turning.j,
				&first_person_weapon->turning_velocity.j,
				turning.j,
				0.03f,
				0.2f,
				-1.0f,
				1.0f,
				FALSE);
		}

		accelerate_to_position(
			&first_person_weapon->firing_push_back,
			&first_person_weapon->firing_push_back_velocity,
			0.0f,
			0.01f,
			0.2f,
			0.0f,
			1.0f,
			FALSE);
		if (first_person_weapon->firing_push_back==1.0f)
		{
			first_person_weapon->firing_push_back_velocity= 0.0f;
		}

		if (first_person_weapon->interpolation_frame_count>0)
		{
			first_person_weapon->interpolation_frame_index++;
			if (first_person_weapon->interpolation_frame_index>=
				first_person_weapon->interpolation_frame_count)
			{
				first_person_weapon->interpolation_frame_count= 0;
			}
		}

		if (player_control_get_autoaim_level(local_player_index)!=0.0f ||
			player_control_get_zoom_level(local_player_index)!=NONE ||
			first_person_weapon->firing_push_back!=0.0f ||
			first_person_weapon->position.i!=0.0f ||
			first_person_weapon->position.j!=0.0f ||
			first_person_weapon->turning.i!=0.0f ||
			first_person_weapon->turning.j!=0.0f)
		{
			first_person_weapon->ticks_idle= 0;
			if (first_person_weapon->state==_first_person_weapon_state_posing)
			{
				first_person_weapon_set_state(
					local_player_index,
					_first_person_weapon_state_idle,
					TRUE);
			}
		}
		else if (first_person_weapon->state==_first_person_weapon_state_idle)
		{
			struct game_globals_player_information *player_information= TAG_BLOCK_GET_ELEMENT(
				&scenario_get_game_globals()->player_information,
				0,
				struct game_globals_player_information);

			if (!first_person_weapon->ticks_until_pose)
			{
				first_person_weapon->ticks_until_pose= (short)(real_local_random_range(
					player_information->first_person_idle_time_lower_bound,
					player_information->first_person_idle_time_upper_bound)*TICKS_PER_SECOND);
			}
			first_person_weapon->ticks_idle++;
			if (first_person_weapon->ticks_idle>first_person_weapon->ticks_until_pose)
			{
				first_person_weapon->ticks_until_pose= 0;
				if (real_local_random()>=player_information->first_person_idle_skip_fraction)
				{
					first_person_weapon_set_state(
						local_player_index,
						_first_person_weapon_state_posing,
						TRUE);
				}
			}
		}
		else
		{
			first_person_weapon->ticks_idle= 0;
		}
	}

	first_person_weapon->ticks_until_predict--;
	if (first_person_weapon->ticks_until_predict<=0)
	{
		first_person_weapon_predict(local_player_index);
	}

	return;
}

static boolean model_build_remapping_table_for_animation_graph(
	long model_index,
	long animation_graph_index,
	short *node_remapping_table)
{
	struct model *model= model_definition_get(model_index);
	struct animation_graph *animation_graph= animation_graph_definition_get(animation_graph_index);
	boolean valid= TRUE;
	short model_node_index;

	for (model_node_index=0; model_node_index<model->nodes.count; model_node_index++)
	{
		struct model_node *model_node= TAG_BLOCK_GET_ELEMENT(
			&model->nodes,
			model_node_index,
			struct model_node);
		short animation_graph_node_index= NONE;
		short node_index;

		for (node_index=0; node_index<animation_graph->nodes.count; node_index++)
		{
			struct animation_graph_node *animation_graph_node= TAG_BLOCK_GET_ELEMENT(
				&animation_graph->nodes,
				node_index,
				struct animation_graph_node);

			if (!csstrcmp(model_node->name, animation_graph_node->name))
			{
				animation_graph_node_index= node_index;
				break;
			}
		}

		if (animation_graph_node_index!=NONE)
		{
			node_remapping_table[model_node_index]= animation_graph_node_index;
		}
		else
		{
			valid= FALSE;
		}
	}

	return valid;
}

static void weapon_play_first_person_weapon_sound(
	long weapon_index,
	short message_type)
{
	if (weapon_index!=NONE && message_type!=NONE && weapon_try_and_get(weapon_index))
	{
		struct weapon_datum *weapon= weapon_get(weapon_index);
		struct weapon_definition *weapon_definition= weapon_definition_get(weapon->definition_index);

		if (weapon_definition->weapon.interface_definition.first_person_animations.index!=NONE)
		{
			short state= (short)first_person_weapon_state_from_weapon_message(message_type);

			if (state!=NONE)
			{
				short animation_type= (short)first_person_animation_type_from_weapon_state(state);

				if (animation_type!=NONE)
				{
					struct animation_graph *animation_graph= animation_graph_definition_get(
						weapon_definition->weapon.interface_definition.first_person_animations.index);
					struct animation_graph_first_person_weapon_animations *first_person_weapon_animations=
						!animation_graph->first_person_weapon_animations.count ?
							NULL :
							TAG_BLOCK_GET_ELEMENT(
								&animation_graph->first_person_weapon_animations,
								0,
								struct animation_graph_first_person_weapon_animations);
					short animation_index= (animation_type>=0 &&
						animation_type<first_person_weapon_animations->animations.count) ?
							animation_graph_animation_index_get(
								&first_person_weapon_animations->animations)
									[animation_type].animation_index :
							(short)NONE;

					if (animation_index!=NONE)
					{
						short sound_index= TAG_BLOCK_GET_ELEMENT(
							&animation_graph->animations,
							animation_index,
							struct animation)->sound_index;

						if (sound_index!=NONE)
						{
							long definition_index= TAG_BLOCK_GET_ELEMENT(
								&animation_graph->sound_references,
								sound_index,
								struct animation_graph_sound_reference)->sound.index;

							if (definition_index!=NONE)
							{
								object_impulse_sound_new(
									weapon_index,
									definition_index,
									NONE,
									global_origin3d,
									global_forward3d,
									1.0f);
							}
						}
					}
				}
			}
		}
	}

	return;
}

static long first_person_weapon_state_from_weapon_message(
	short message_type)
{
	switch (message_type)
	{
		case _first_person_weapon_message_primary_fire:
			return _first_person_weapon_state_primary_fire;
		case _first_person_weapon_message_secondary_fire:
			return _first_person_weapon_state_secondary_fire;
		case _first_person_weapon_message_primary_misfire:
			return _first_person_weapon_state_primary_misfire;
		case _first_person_weapon_message_secondary_misfire:
			return _first_person_weapon_state_secondary_misfire;
		case _first_person_weapon_message_melee:
			return _first_person_weapon_state_melee;
		case _first_person_weapon_message_light_on:
			return _first_person_weapon_state_light_on;
		case _first_person_weapon_message_light_off:
			return _first_person_weapon_state_light_off;
		case _first_person_weapon_message_reload_while_empty:
			return _first_person_weapon_state_reload_while_empty;
		case _first_person_weapon_message_reload_while_full:
			return _first_person_weapon_state_reload_while_full;
		case _first_person_weapon_message_put_away:
			return _first_person_weapon_state_put_away;
		case _first_person_weapon_message_ready:
			return _first_person_weapon_state_ready;
		case _first_person_weapon_message_charged:
			return _first_person_weapon_state_charged;
		case _first_person_weapon_message_overheating:
			return _first_person_weapon_state_overheating;
		case _first_person_weapon_message_throw_grenade:
			return _first_person_weapon_state_throw_grenade;
		case _first_person_weapon_message_overheating_super_recoil:
			return _first_person_weapon_state_overheating_super_recoil;
	}

	return NONE;
}

static long first_person_animation_type_from_weapon_state(
	short state)
{
	switch (state)
	{
		case _first_person_weapon_state_idle:
			return _first_person_weapon_animation_idle;
		case _first_person_weapon_state_overheated:
			return _first_person_weapon_animation_overheated;
		case _first_person_weapon_state_charged:
			return _first_person_weapon_animation_overcharged;
		case _first_person_weapon_state_posing:
			return _first_person_weapon_animation_posing;
		case _first_person_weapon_state_primary_fire:
			return _first_person_weapon_animation_primary_fire;
		case _first_person_weapon_state_secondary_fire:
			return _first_person_weapon_animation_secondary_fire;
		case _first_person_weapon_state_primary_misfire:
			return _first_person_weapon_animation_primary_misfire;
		case _first_person_weapon_state_secondary_misfire:
			return _first_person_weapon_animation_secondary_misfire;
		case _first_person_weapon_state_melee:
			return _first_person_weapon_animation_melee;
		case _first_person_weapon_state_light_on:
			return _first_person_weapon_animation_light_on;
		case _first_person_weapon_state_light_off:
			return _first_person_weapon_animation_light_off;
		case _first_person_weapon_state_reload_while_empty:
			return _first_person_weapon_animation_reload_while_empty;
		case _first_person_weapon_state_reload_while_full:
			return _first_person_weapon_animation_reload_while_full;
		case _first_person_weapon_state_put_away:
			return _first_person_weapon_animation_put_away;
		case _first_person_weapon_state_ready:
			return _first_person_weapon_animation_ready;
		case _first_person_weapon_state_throw_grenade:
			return _first_person_weapon_animation_throw_grenade;
		case _first_person_weapon_state_throw_grenade_overheated:
			return _first_person_weapon_animation_throw_grenade_overheated;
		case _first_person_weapon_state_overheating:
			return _first_person_weapon_animation_overheating;
		case _first_person_weapon_state_overheating_again:
			return _first_person_weapon_animation_overheating_again;
		case _first_person_weapon_state_shotgun_enter_reload:
			return _first_person_weapon_animation_shotgun_enter;
		case _first_person_weapon_state_shotgun_exit_reload_empty:
			return _first_person_weapon_animation_shotgun_exit_empty;
		case _first_person_weapon_state_shotgun_exit_reload_full:
			return _first_person_weapon_animation_shotgun_exit_full;
		case _first_person_weapon_state_overheated_exit:
			return _first_person_weapon_animation_overheated_exit;
		case _first_person_weapon_state_overheating_super_recoil:
			return _first_person_weapon_animation_overheated_supercharge_enter;
	}

	return NONE;
}

static void model_remap_node_matrices_to_match_animation_graph(
	long model_index,
	struct real_matrix4x3 *model_node_matrices,
	long animation_graph_index,
	struct real_matrix4x3 const *node_matrices,
	short const *node_remapping_table)
{
	struct model *model= model_definition_get(model_index);
	struct animation_graph *animation_graph= animation_graph_definition_get(animation_graph_index);
	short node_index;

	for (node_index=0; node_index<model->nodes.count; node_index++)
	{
		short animation_graph_node_index= node_remapping_table[node_index];

		match_assert(
			"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
			1587,
			animation_graph_node_index>=0 && animation_graph_node_index<animation_graph->nodes.count);
		model_node_matrices[node_index]= node_matrices[animation_graph_node_index];
	}

	return;
}

static struct first_person_weapon *first_person_weapon_get(
	short local_player_index)
{
	match_assert(
		"c:\\halo\\SOURCE\\interface\\first_person_weapons.c",
		1433,
		local_player_index>=0 && local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS);

	return &first_person_weapons[local_player_index];
}

static short first_person_weapon_index_from_weapon_index(
	long weapon_index)
{
	short local_player_index;

	for (local_player_index=0; local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index= local_player_get_player_index(local_player_index);

		if (player_index!=NONE)
		{
			long unit_index= player_get(player_index)->unit_index;

			if (unit_index!=NONE)
			{
				struct unit_datum *unit= unit_get(unit_index);

				if (unit->unit.current_weapon_index!=NONE &&
					weapon_index==unit->unit.weapon_object_indices[unit->unit.current_weapon_index])
				{
					return local_player_index;
				}
			}
		}
	}

	return NONE;
}

static short first_person_weapon_index_from_unit_index(
	long unit_index)
{
	short local_player_index;

	for (local_player_index=0; local_player_index<MAXIMUM_NUMBER_OF_LOCAL_PLAYERS; local_player_index++)
	{
		long player_index= local_player_get_player_index(local_player_index);

		if (player_index!=NONE && player_get(player_index)->unit_index==unit_index)
		{
			return local_player_index;
		}
	}

	return NONE;
}
