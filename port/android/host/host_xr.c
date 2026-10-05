/*
HOST_XR.C

The OpenXR session of the VR build (port/android/README.md, "VR"). The
game (port/linux/src/vr.c) drives it from its own thread, the one its GL
context is current on:

- host_xr_initialize, once the context exists: creates the instance, the
  session on that context, the swapchains and the controller actions;
- host_xr_begin_frame, before the game reads its input: handles the
  runtime's events, waits for the frame, and reports where the head, the
  eyes and the controllers will be when it is displayed;
- host_xr_copy_layer, for each image the game drew: copies one of its
  render targets into the matching swapchain (through a shader, see below);
- host_xr_end_frame: submits the frame's layers.

Everything is written for the Khronos loader and core OpenXR plus
XR_KHR_opengl_es_enable, so the one APK runs on Meta's runtime (Quest 3 and
3S) and on the Steam Frame's (Android apps there run in Lepton). Optional
extensions are used only where the runtime lists them.

The game draws in gamma space, as the Xbox did. The swapchains are sRGB
(what the runtimes composite correctly), so the copy decodes the game's
values to linear and the hardware encodes them again on write: the stored
bytes are the game's. The render targets have row 0 at the top of the
picture; the copy flips them to GL's bottom-up images.
*/

#include "host.h"
#include "halo_xr_abi.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <SDL3/SDL.h>
#include <jni.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <stdio.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define MAXIMUM_SWAPCHAIN_IMAGES 8

struct xr_swapchain
{
	XrSwapchain handle;
	int width;
	int height;
	uint32_t image_count;
	GLuint images[MAXIMUM_SWAPCHAIN_IMAGES];
	GLuint framebuffers[MAXIMUM_SWAPCHAIN_IMAGES];
	uint32_t acquired_index;
	int filled; /* this frame */
};

struct xr_hand_actions
{
	XrPath path;
	XrSpace grip_space;
	XrSpace aim_space;
};

static struct
{
	int initialized;
	XrInstance instance;
	XrSystemId system;
	char system_name[XR_MAX_SYSTEM_NAME_SIZE];
	int eye_width;
	int eye_height;
	XrSession session;
	XrSessionState state;
	int running;
	int focused;
	int exit_requested;
	XrSpace local_space;
	XrSpace view_space;

	/* the frame between host_xr_begin_frame and host_xr_end_frame */
	int frame_begun;
	XrFrameState frame_state;
	XrView views[HALO_XR_EYE_COUNT];
	int views_valid;
	int recentred;

	struct xr_swapchain swapchains[HALO_XR_LAYER_COUNT];
	int64_t format;

	XrActionSet action_set;
	XrAction grip_pose;
	XrAction aim_pose;
	XrAction trigger;
	XrAction squeeze;
	XrAction thumbstick;
	XrAction thumbstick_click;
	XrAction button_a; /* A on the right controller, X on the left */
	XrAction button_b; /* B on the right controller, Y on the left */
	XrAction menu;
	XrAction thumbrest;
	/* the Steam Frame's controllers' own (XR_VALVE_frame_controller_interaction) */
	int has_frame_controller;
	int frame_layout;
	XrAction button_x;
	XrAction button_y;
	XrAction view;
	XrAction bumper;
	XrAction dpad[4];
	XrAction haptic;
	struct xr_hand_actions hands[HALO_XR_HAND_COUNT];

	/* where the head and the left hand are this frame (the wrist layer) */
	XrPosef head;
	XrPosef left_grip;
	int left_grip_valid;

	int has_performance_settings;
	/* (XR_META_performance_metrics: the compositor's counts, in the log,
	for telling frames shown late from the game's own pace) */
	int has_metrics;
	PFN_xrQueryPerformanceMetricsCounterMETA query_metric;
	XrPath metric_paths[16];
	uint32_t metric_count;
	struct timespec metrics_logged;
	/* (SteamVR: the frames' thread kept on the fast cores, xr_fast_cores) */
	int steamvr;
	int cores_pinned;
	int has_refresh_rate;
	PFN_xrRequestDisplayRefreshRateFB request_refresh_rate;
	PFN_xrGetDisplayRefreshRateFB get_refresh_rate;
	PFN_xrEnumerateDisplayRefreshRatesFB enumerate_refresh_rates;

	/* the copy's GL objects */
	GLuint program;
	GLint source_uniform;
	GLint rect_uniform;
	GLint cut_disc_uniform;
	GLint vignette_uniform;
	GLint alpha_from_colour_uniform;
	/* the runtime draws a quad's texels by their alpha alone, ignoring the
	colour of those with none (SteamVR on the Steam Frame): the game's
	overlays, whose additive parts have none, get alpha from their colour */
	int quad_alpha_from_colour;
	GLint tangents_uniform;
	float vignette;
	/* the same copy from a layer of a texture array (the eyes drawn at
	once, multiview): its program and uniforms, and the layer for the copy
	under way (host_xr_copy_layer_array; -1: a plain texture) */
	GLuint array_program;
	GLint array_source_uniform, array_rect_uniform, array_layer_uniform;
	GLint array_vignette_uniform, array_tangents_uniform;
	int copy_array_layer;
	GLuint vertex_array;
	GLuint sampler;
} xr;

/* ---------- helpers */

static int xr_ok(XrResult result, const char *what)
{
	char text[XR_MAX_RESULT_STRING_SIZE];

	if (XR_SUCCEEDED(result))
		return 1;
	text[0] = 0;
	if (xr.instance)
		xrResultToString(xr.instance, result, text);
	host_logf(HOST_LOG_ERROR, "xr: %s failed: %s (%d)", what, text[0] ? text : "?", (int)result);
	return 0;
}

static int has_extension(const XrExtensionProperties *extensions, uint32_t count, const char *name)
{
	uint32_t index;

	for (index = 0; index < count; index++)
	{
		if (!strcmp(extensions[index].extensionName, name))
			return 1;
	}
	return 0;
}

static XrPath path_from_string(const char *text)
{
	XrPath path = XR_NULL_PATH;

	xrStringToPath(xr.instance, text, &path);
	return path;
}

static void pose_from_xr(struct halo_xr_pose *pose, const XrPosef *source)
{
	pose->orientation[0] = source->orientation.x;
	pose->orientation[1] = source->orientation.y;
	pose->orientation[2] = source->orientation.z;
	pose->orientation[3] = source->orientation.w;
	pose->position[0] = source->position.x;
	pose->position[1] = source->position.y;
	pose->position[2] = source->position.z;
}

static void pose_to_xr(XrPosef *pose, const struct halo_xr_pose *source)
{
	pose->orientation.x = source->orientation[0];
	pose->orientation.y = source->orientation[1];
	pose->orientation.z = source->orientation[2];
	pose->orientation.w = source->orientation[3];
	pose->position.x = source->position[0];
	pose->position.y = source->position[1];
	pose->position.z = source->position[2];
}

static int flags_from_location(XrSpaceLocationFlags flags)
{
	int result = 0;

	if ((flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		result |= HALO_XR_POSE_VALID;
	if ((flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) && (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT))
		result |= HALO_XR_POSE_TRACKED;
	return result;
}

/* ---------- the copy shader */

static const char copy_vertex_shader[] =
	"#version 300 es\n"
	"uniform vec4 rect; /* the source's part: left, top, right, bottom; 0..1 from its top left */\n"
	"out vec2 coordinate;\n"
	"out vec2 disc;\n"
	"void main()\n"
	"{\n"
	"	vec2 corner = vec2(float(gl_VertexID & 1), float(gl_VertexID >> 1));\n"
	"	coordinate = vec2(mix(rect.x, rect.z, corner.x), mix(rect.w, rect.y, corner.y));\n"
	"	disc = corner * 2.0 - 1.0;\n"
	"	gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);\n"
	"}\n";

static const char copy_fragment_shader[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform sampler2D source;\n"
	"uniform float cut_disc;\n"
	"uniform float alpha_from_colour; /* 1: as opaque as it is bright (a quad's, SteamVR) */\n"
	"uniform float vignette; /* 0..1: the eye images darken from their edges */\n"
	"uniform vec4 tangents; /* the eye's edges: left, right, down, up */\n"
	"in vec2 coordinate;\n"
	"in vec2 disc;\n"
	"out vec4 color;\n"
	"vec3 to_linear(vec3 c)\n"
	"{\n"
	"	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec4 texel = texture(source, coordinate);\n"
	"	color = vec4(to_linear(texel.rgb), texel.a);\n"
	"	if (alpha_from_colour > 0.5)\n"
	"		color.a = max(color.a, max(color.r, max(color.g, color.b)));\n"
	"	if (cut_disc > 0.5)\n"
	"	{\n"
	"		float inside = 1.0 - smoothstep(0.95, 1.0, length(disc));\n"
	"		/* a fine cross, open at the centre */\n"
	"		vec2 away = abs(disc);\n"
	"		float line = (away.x < 0.008 || away.y < 0.008) && max(away.x, away.y) > 0.06 ? 0.85 : 0.0;\n"
	"		color.rgb *= 1.0 - line;\n"
	"		color = vec4(color.rgb * inside, inside);\n"
	"	}\n"
	"	if (vignette > 0.0)\n"
	"	{\n"
	"		/* by the angle off straight ahead (tangents), not the image's centre */\n"
	"		vec2 corner = disc * 0.5 + 0.5;\n"
	"		vec2 away = vec2(mix(tangents.x, tangents.y, corner.x), mix(tangents.z, tangents.w, corner.y));\n"
	"		float edge = mix(1.8, 0.45, vignette);\n"
	"		color.rgb *= 1.0 - smoothstep(edge - 0.3, edge, length(away));\n"
	"	}\n"
	"}\n";

static const char copy_array_fragment_shader[] =
	"#version 300 es\n"
	"precision highp float;\n"
	"uniform highp sampler2DArray source;\n"
	"uniform float layer;\n"
	"uniform float vignette;\n"
	"uniform vec4 tangents;\n"
	"in vec2 coordinate;\n"
	"in vec2 disc;\n"
	"out vec4 color;\n"
	"vec3 to_linear(vec3 c)\n"
	"{\n"
	"	return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));\n"
	"}\n"
	"void main()\n"
	"{\n"
	"	vec4 texel = texture(source, vec3(coordinate, layer));\n"
	"	color = vec4(to_linear(texel.rgb), texel.a);\n"
	"	if (vignette > 0.0)\n"
	"	{\n"
	"		vec2 corner = disc * 0.5 + 0.5;\n"
	"		vec2 away = vec2(mix(tangents.x, tangents.y, corner.x), mix(tangents.z, tangents.w, corner.y));\n"
	"		float edge = mix(1.8, 0.45, vignette);\n"
	"		color.rgb *= 1.0 - smoothstep(edge - 0.3, edge, length(away));\n"
	"	}\n"
	"}\n";

static GLuint compile_shader(GLenum type, const char *text)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &text, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024];

		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		host_logf(HOST_LOG_ERROR, "xr: copy shader: %s", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

static int copy_initialize(void)
{
	GLuint vertex = compile_shader(GL_VERTEX_SHADER, copy_vertex_shader);
	GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, copy_fragment_shader);
	GLint status = 0;

	if (!vertex || !fragment)
		return 0;
	xr.program = glCreateProgram();
	glAttachShader(xr.program, vertex);
	glAttachShader(xr.program, fragment);
	glLinkProgram(xr.program);
	glDeleteShader(vertex);
	glDeleteShader(fragment);
	glGetProgramiv(xr.program, GL_LINK_STATUS, &status);
	if (!status)
	{
		host_logf(HOST_LOG_ERROR, "xr: copy program does not link");
		return 0;
	}
	xr.source_uniform = glGetUniformLocation(xr.program, "source");
	xr.rect_uniform = glGetUniformLocation(xr.program, "rect");
	xr.cut_disc_uniform = glGetUniformLocation(xr.program, "cut_disc");
	xr.vignette_uniform = glGetUniformLocation(xr.program, "vignette");
	xr.alpha_from_colour_uniform = glGetUniformLocation(xr.program, "alpha_from_colour");
	xr.tangents_uniform = glGetUniformLocation(xr.program, "tangents");
	xr.copy_array_layer = -1;
	{
		GLuint array_vertex = compile_shader(GL_VERTEX_SHADER, copy_vertex_shader);
		GLuint array_fragment = compile_shader(GL_FRAGMENT_SHADER, copy_array_fragment_shader);

		if (array_vertex && array_fragment)
		{
			xr.array_program = glCreateProgram();
			glAttachShader(xr.array_program, array_vertex);
			glAttachShader(xr.array_program, array_fragment);
			glLinkProgram(xr.array_program);
			glGetProgramiv(xr.array_program, GL_LINK_STATUS, &status);
			if (!status)
			{
				host_logf(HOST_LOG_ERROR, "xr: array copy program does not link");
				glDeleteProgram(xr.array_program);
				xr.array_program = 0;
			}
			else
			{
				xr.array_source_uniform = glGetUniformLocation(xr.array_program, "source");
				xr.array_rect_uniform = glGetUniformLocation(xr.array_program, "rect");
				xr.array_layer_uniform = glGetUniformLocation(xr.array_program, "layer");
				xr.array_vignette_uniform = glGetUniformLocation(xr.array_program, "vignette");
				xr.array_tangents_uniform = glGetUniformLocation(xr.array_program, "tangents");
			}
		}
		if (array_vertex)
			glDeleteShader(array_vertex);
		if (array_fragment)
			glDeleteShader(array_fragment);
	}
	glGenVertexArrays(1, &xr.vertex_array);
	glGenSamplers(1, &xr.sampler);
	glSamplerParameteri(xr.sampler, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glSamplerParameteri(xr.sampler, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glSamplerParameteri(xr.sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glSamplerParameteri(xr.sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return 1;
}

/* the GL state the copy changes, put back afterwards: the game's renderer
keeps a shadow of its state (d3d8_gl.c), which must stay true */
struct gl_saved_state
{
	GLint draw_framebuffer;
	GLint read_framebuffer;
	GLint program;
	GLint vertex_array;
	GLint active_texture;
	GLint texture;
	GLint sampler;
	GLint viewport[4];
	GLboolean scissor;
	GLboolean blend;
	GLboolean depth;
	GLboolean cull;
	GLboolean stencil;
	GLboolean color_mask[4];
};

static void gl_save(struct gl_saved_state *state)
{
	glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &state->draw_framebuffer);
	glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &state->read_framebuffer);
	glGetIntegerv(GL_CURRENT_PROGRAM, &state->program);
	glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &state->vertex_array);
	glGetIntegerv(GL_ACTIVE_TEXTURE, &state->active_texture);
	glActiveTexture(GL_TEXTURE0);
	glGetIntegerv(GL_TEXTURE_BINDING_2D, &state->texture);
	glGetIntegerv(GL_SAMPLER_BINDING, &state->sampler);
	glGetIntegerv(GL_VIEWPORT, state->viewport);
	state->scissor = glIsEnabled(GL_SCISSOR_TEST);
	state->blend = glIsEnabled(GL_BLEND);
	state->depth = glIsEnabled(GL_DEPTH_TEST);
	state->cull = glIsEnabled(GL_CULL_FACE);
	state->stencil = glIsEnabled(GL_STENCIL_TEST);
	glGetBooleanv(GL_COLOR_WRITEMASK, state->color_mask);
}

static void set_enabled(GLenum capability, GLboolean enabled)
{
	if (enabled)
		glEnable(capability);
	else
		glDisable(capability);
}

static void gl_restore(const struct gl_saved_state *state)
{
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)state->draw_framebuffer);
	glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)state->read_framebuffer);
	glUseProgram((GLuint)state->program);
	glBindVertexArray((GLuint)state->vertex_array);
	glBindTexture(GL_TEXTURE_2D, (GLuint)state->texture);
	glBindSampler(0, (GLuint)state->sampler);
	glActiveTexture((GLenum)state->active_texture);
	glViewport(state->viewport[0], state->viewport[1], state->viewport[2], state->viewport[3]);
	set_enabled(GL_SCISSOR_TEST, state->scissor);
	set_enabled(GL_BLEND, state->blend);
	set_enabled(GL_DEPTH_TEST, state->depth);
	set_enabled(GL_CULL_FACE, state->cull);
	set_enabled(GL_STENCIL_TEST, state->stencil);
	glColorMask(state->color_mask[0], state->color_mask[1], state->color_mask[2], state->color_mask[3]);
}

/* ---------- swapchains */

static int swapchain_create(struct xr_swapchain *swapchain, int width, int height)
{
	XrSwapchainCreateInfo information = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	XrSwapchainImageOpenGLESKHR images[MAXIMUM_SWAPCHAIN_IMAGES];
	uint32_t index;

	information.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
	information.format = xr.format;
	information.sampleCount = 1;
	information.width = (uint32_t)width;
	information.height = (uint32_t)height;
	information.faceCount = 1;
	information.arraySize = 1;
	information.mipCount = 1;
	if (!xr_ok(xrCreateSwapchain(xr.session, &information, &swapchain->handle), "xrCreateSwapchain"))
		return 0;
	swapchain->width = width;
	swapchain->height = height;
	if (!xr_ok(xrEnumerateSwapchainImages(swapchain->handle, 0, &swapchain->image_count, NULL),
		"xrEnumerateSwapchainImages"))
	{
		return 0;
	}
	if (swapchain->image_count > MAXIMUM_SWAPCHAIN_IMAGES)
		swapchain->image_count = MAXIMUM_SWAPCHAIN_IMAGES;
	for (index = 0; index < swapchain->image_count; index++)
	{
		images[index].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
		images[index].next = NULL;
	}
	if (!xr_ok(xrEnumerateSwapchainImages(swapchain->handle, swapchain->image_count, &swapchain->image_count,
		(XrSwapchainImageBaseHeader *)images), "xrEnumerateSwapchainImages"))
	{
		return 0;
	}
	glGenFramebuffers((GLsizei)swapchain->image_count, swapchain->framebuffers);
	for (index = 0; index < swapchain->image_count; index++)
	{
		swapchain->images[index] = images[index].image;
		glBindFramebuffer(GL_FRAMEBUFFER, swapchain->framebuffers[index]);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, swapchain->images[index], 0);
		if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE)
		{
			host_logf(HOST_LOG_ERROR, "xr: swapchain image %u is not a complete framebuffer", index);
			return 0;
		}
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	return 1;
}

static int64_t choose_format(void)
{
	int64_t *formats;
	int64_t chosen = 0;
	uint32_t count = 0, index;

	if (!xr_ok(xrEnumerateSwapchainFormats(xr.session, 0, &count, NULL), "xrEnumerateSwapchainFormats") || !count)
		return 0;
	formats = calloc(count, sizeof(*formats));
	if (!formats || !xr_ok(xrEnumerateSwapchainFormats(xr.session, count, &count, formats),
		"xrEnumerateSwapchainFormats"))
	{
		free(formats);
		return 0;
	}
	for (index = 0; index < count && !chosen; index++)
	{
		if (formats[index] == GL_SRGB8_ALPHA8)
			chosen = formats[index];
	}
	if (!chosen)
	{
		host_logf(HOST_LOG_WARN, "xr: no sRGB swapchain format; colours will be off");
		chosen = formats[0];
	}
	free(formats);
	return chosen;
}

/* ---------- actions */

static XrAction action_create(const char *name, const char *localized, XrActionType type)
{
	XrActionCreateInfo information = { XR_TYPE_ACTION_CREATE_INFO };
	XrPath subactions[HALO_XR_HAND_COUNT] = { xr.hands[0].path, xr.hands[1].path };
	XrAction action = XR_NULL_HANDLE;

	strncpy(information.actionName, name, sizeof(information.actionName) - 1);
	strncpy(information.localizedActionName, localized, sizeof(information.localizedActionName) - 1);
	information.actionType = type;
	information.countSubactionPaths = HALO_XR_HAND_COUNT;
	information.subactionPaths = subactions;
	xr_ok(xrCreateAction(xr.action_set, &information, &action), name);
	return action;
}

struct binding
{
	XrAction *action;
	const char *path;
};

static void suggest(const char *profile, const struct binding *bindings, int count)
{
	XrActionSuggestedBinding suggested[32];
	XrInteractionProfileSuggestedBinding information = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	int index;

	for (index = 0; index < count && index < 32; index++)
	{
		suggested[index].action = *bindings[index].action;
		suggested[index].binding = path_from_string(bindings[index].path);
	}
	information.interactionProfile = path_from_string(profile);
	information.suggestedBindings = suggested;
	information.countSuggestedBindings = (uint32_t)index;
	if (!XR_SUCCEEDED(xrSuggestInteractionProfileBindings(xr.instance, &information)))
		host_logf(HOST_LOG_WARN, "xr: no bindings for %s", profile);
}

static int actions_initialize(void)
{
	XrActionSetCreateInfo set = { XR_TYPE_ACTION_SET_CREATE_INFO };
	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	XrActionSpaceCreateInfo space = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	int hand;

	strcpy(set.actionSetName, "gameplay");
	strcpy(set.localizedActionSetName, "Gameplay");
	if (!xr_ok(xrCreateActionSet(xr.instance, &set, &xr.action_set), "xrCreateActionSet"))
		return 0;
	xr.hands[HALO_XR_HAND_LEFT].path = path_from_string("/user/hand/left");
	xr.hands[HALO_XR_HAND_RIGHT].path = path_from_string("/user/hand/right");

	xr.grip_pose = action_create("grip_pose", "Hand pose", XR_ACTION_TYPE_POSE_INPUT);
	xr.aim_pose = action_create("aim_pose", "Aim pose", XR_ACTION_TYPE_POSE_INPUT);
	xr.trigger = action_create("trigger", "Trigger", XR_ACTION_TYPE_FLOAT_INPUT);
	xr.squeeze = action_create("squeeze", "Grip", XR_ACTION_TYPE_FLOAT_INPUT);
	xr.thumbstick = action_create("thumbstick", "Thumbstick", XR_ACTION_TYPE_VECTOR2F_INPUT);
	xr.thumbstick_click = action_create("thumbstick_click", "Thumbstick click", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.button_a = action_create("button_lower", "A / X", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.button_b = action_create("button_upper", "B / Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.menu = action_create("menu", "Menu", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.thumbrest = action_create("thumbrest", "Thumb rest", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.button_x = action_create("button_x", "X", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.button_y = action_create("button_y", "Y", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.view = action_create("view", "View", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.bumper = action_create("bumper", "Bumper", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.dpad[0] = action_create("dpad_up", "D-pad up", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.dpad[1] = action_create("dpad_down", "D-pad down", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.dpad[2] = action_create("dpad_left", "D-pad left", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.dpad[3] = action_create("dpad_right", "D-pad right", XR_ACTION_TYPE_BOOLEAN_INPUT);
	xr.haptic = action_create("haptic", "Rumble", XR_ACTION_TYPE_VIBRATION_OUTPUT);

	{
		/* Quest's Touch and Touch Plus, and what the Steam Frame's
		controllers present themselves as by default */
		static const struct binding touch[] =
		{
			{ &xr.grip_pose, "/user/hand/left/input/grip/pose" },
			{ &xr.grip_pose, "/user/hand/right/input/grip/pose" },
			{ &xr.aim_pose, "/user/hand/left/input/aim/pose" },
			{ &xr.aim_pose, "/user/hand/right/input/aim/pose" },
			{ &xr.trigger, "/user/hand/left/input/trigger/value" },
			{ &xr.trigger, "/user/hand/right/input/trigger/value" },
			{ &xr.squeeze, "/user/hand/left/input/squeeze/value" },
			{ &xr.squeeze, "/user/hand/right/input/squeeze/value" },
			{ &xr.thumbstick, "/user/hand/left/input/thumbstick" },
			{ &xr.thumbstick, "/user/hand/right/input/thumbstick" },
			{ &xr.thumbstick_click, "/user/hand/left/input/thumbstick/click" },
			{ &xr.thumbstick_click, "/user/hand/right/input/thumbstick/click" },
			{ &xr.button_a, "/user/hand/left/input/x/click" },
			{ &xr.button_a, "/user/hand/right/input/a/click" },
			{ &xr.button_b, "/user/hand/left/input/y/click" },
			{ &xr.button_b, "/user/hand/right/input/b/click" },
			{ &xr.menu, "/user/hand/left/input/menu/click" },
			{ &xr.thumbrest, "/user/hand/left/input/thumbrest/touch" },
			{ &xr.thumbrest, "/user/hand/right/input/thumbrest/touch" },
			{ &xr.haptic, "/user/hand/left/output/haptic" },
			{ &xr.haptic, "/user/hand/right/output/haptic" },
		};
		static const struct binding index[] =
		{
			{ &xr.grip_pose, "/user/hand/left/input/grip/pose" },
			{ &xr.grip_pose, "/user/hand/right/input/grip/pose" },
			{ &xr.aim_pose, "/user/hand/left/input/aim/pose" },
			{ &xr.aim_pose, "/user/hand/right/input/aim/pose" },
			{ &xr.trigger, "/user/hand/left/input/trigger/value" },
			{ &xr.trigger, "/user/hand/right/input/trigger/value" },
			{ &xr.squeeze, "/user/hand/left/input/squeeze/value" },
			{ &xr.squeeze, "/user/hand/right/input/squeeze/value" },
			{ &xr.thumbstick, "/user/hand/left/input/thumbstick" },
			{ &xr.thumbstick, "/user/hand/right/input/thumbstick" },
			{ &xr.thumbstick_click, "/user/hand/left/input/thumbstick/click" },
			{ &xr.thumbstick_click, "/user/hand/right/input/thumbstick/click" },
			{ &xr.button_a, "/user/hand/left/input/a/click" },
			{ &xr.button_a, "/user/hand/right/input/a/click" },
			{ &xr.button_b, "/user/hand/left/input/b/click" },
			{ &xr.button_b, "/user/hand/right/input/b/click" },
			{ &xr.menu, "/user/hand/left/input/system/click" },
			{ &xr.haptic, "/user/hand/left/output/haptic" },
			{ &xr.haptic, "/user/hand/right/output/haptic" },
		};
		static const struct binding simple[] =
		{
			{ &xr.grip_pose, "/user/hand/left/input/grip/pose" },
			{ &xr.grip_pose, "/user/hand/right/input/grip/pose" },
			{ &xr.aim_pose, "/user/hand/left/input/aim/pose" },
			{ &xr.aim_pose, "/user/hand/right/input/aim/pose" },
			{ &xr.button_a, "/user/hand/left/input/select/click" },
			{ &xr.button_a, "/user/hand/right/input/select/click" },
			{ &xr.menu, "/user/hand/left/input/menu/click" },
			{ &xr.haptic, "/user/hand/left/output/haptic" },
			{ &xr.haptic, "/user/hand/right/output/haptic" },
		};

		/* the Steam Frame's controllers, as themselves: the right one has
		A, B, X, Y and a menu button, the left one a D-pad and a View button
		(port/android/STEAM_FRAME.md) */
		static const struct binding frame_controller[] =
		{
			{ &xr.grip_pose, "/user/hand/left/input/grip/pose" },
			{ &xr.grip_pose, "/user/hand/right/input/grip/pose" },
			{ &xr.aim_pose, "/user/hand/left/input/aim/pose" },
			{ &xr.aim_pose, "/user/hand/right/input/aim/pose" },
			{ &xr.trigger, "/user/hand/left/input/trigger/value" },
			{ &xr.trigger, "/user/hand/right/input/trigger/value" },
			{ &xr.squeeze, "/user/hand/left/input/squeeze/value" },
			{ &xr.squeeze, "/user/hand/right/input/squeeze/value" },
			{ &xr.thumbstick, "/user/hand/left/input/thumbstick" },
			{ &xr.thumbstick, "/user/hand/right/input/thumbstick" },
			{ &xr.thumbstick_click, "/user/hand/left/input/thumbstick/click" },
			{ &xr.thumbstick_click, "/user/hand/right/input/thumbstick/click" },
			{ &xr.button_a, "/user/hand/right/input/a/click" },
			{ &xr.button_b, "/user/hand/right/input/b/click" },
			{ &xr.button_x, "/user/hand/right/input/x/click" },
			{ &xr.button_y, "/user/hand/right/input/y/click" },
			{ &xr.menu, "/user/hand/right/input/menu/click" },
			{ &xr.view, "/user/hand/left/input/view/click" },
			{ &xr.bumper, "/user/hand/left/input/bumper/click" },
			{ &xr.bumper, "/user/hand/right/input/bumper/click" },
			{ &xr.dpad[0], "/user/hand/left/input/dpad_up/click" },
			{ &xr.dpad[1], "/user/hand/left/input/dpad_down/click" },
			{ &xr.dpad[2], "/user/hand/left/input/dpad_left/click" },
			{ &xr.dpad[3], "/user/hand/left/input/dpad_right/click" },
			{ &xr.haptic, "/user/hand/left/output/haptic" },
			{ &xr.haptic, "/user/hand/right/output/haptic" },
		};

		if (xr.has_frame_controller)
		{
			suggest("/interaction_profiles/valve/frame_controller_valve", frame_controller,
				sizeof(frame_controller) / sizeof(frame_controller[0]));
		}
		suggest("/interaction_profiles/oculus/touch_controller", touch, sizeof(touch) / sizeof(touch[0]));
		suggest("/interaction_profiles/valve/index_controller", index, sizeof(index) / sizeof(index[0]));
		suggest("/interaction_profiles/khr/simple_controller", simple, sizeof(simple) / sizeof(simple[0]));
	}

	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
	{
		space.poseInActionSpace.orientation.w = 1.0f;
		space.subactionPath = xr.hands[hand].path;
		space.action = xr.grip_pose;
		xr_ok(xrCreateActionSpace(xr.session, &space, &xr.hands[hand].grip_space), "xrCreateActionSpace(grip)");
		space.action = xr.aim_pose;
		xr_ok(xrCreateActionSpace(xr.session, &space, &xr.hands[hand].aim_space), "xrCreateActionSpace(aim)");
	}

	attach.countActionSets = 1;
	attach.actionSets = &xr.action_set;
	return xr_ok(xrAttachSessionActionSets(xr.session, &attach), "xrAttachSessionActionSets");
}

static float action_float(XrAction action, int hand)
{
	XrActionStateGetInfo information = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateFloat state = { XR_TYPE_ACTION_STATE_FLOAT };

	information.action = action;
	information.subactionPath = xr.hands[hand].path;
	if (!XR_SUCCEEDED(xrGetActionStateFloat(xr.session, &information, &state)) || !state.isActive)
		return 0.0f;
	return state.currentState;
}

static int action_boolean(XrAction action, int hand)
{
	XrActionStateGetInfo information = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateBoolean state = { XR_TYPE_ACTION_STATE_BOOLEAN };

	information.action = action;
	information.subactionPath = xr.hands[hand].path;
	if (!XR_SUCCEEDED(xrGetActionStateBoolean(xr.session, &information, &state)) || !state.isActive)
		return 0;
	return state.currentState != XR_FALSE;
}

static int action_vector(XrAction action, int hand, float vector[2])
{
	XrActionStateGetInfo information = { XR_TYPE_ACTION_STATE_GET_INFO };
	XrActionStateVector2f state = { XR_TYPE_ACTION_STATE_VECTOR2F };

	vector[0] = vector[1] = 0.0f;
	information.action = action;
	information.subactionPath = xr.hands[hand].path;
	if (!XR_SUCCEEDED(xrGetActionStateVector2f(xr.session, &information, &state)) || !state.isActive)
		return 0;
	vector[0] = state.currentState.x;
	vector[1] = state.currentState.y;
	return 1;
}

static void hands_read(struct halo_xr_frame *frame)
{
	XrActiveActionSet active = { xr.action_set, XR_NULL_PATH };
	XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
	int hand;

	sync.countActiveActionSets = 1;
	sync.activeActionSets = &active;
	if (!XR_SUCCEEDED(xrSyncActions(xr.session, &sync)))
		return;
	{
		/* (the controllers' profile the runtime chose, in the log as it
		changes) */
		static XrPath logged[HALO_XR_HAND_COUNT];

		for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
		{
			XrInteractionProfileState state = { XR_TYPE_INTERACTION_PROFILE_STATE };

			if (XR_SUCCEEDED(xrGetCurrentInteractionProfile(xr.session, xr.hands[hand].path, &state)) &&
				state.interactionProfile != logged[hand])
			{
				char name[XR_MAX_PATH_LENGTH] = "none";
				uint32_t length = 0;

				logged[hand] = state.interactionProfile;
				if (state.interactionProfile != XR_NULL_PATH)
					xrPathToString(xr.instance, state.interactionProfile, sizeof(name), &length, name);
				if (hand == HALO_XR_HAND_RIGHT)
					xr.frame_layout = strstr(name, "frame_controller") != NULL;
				host_logf(HOST_LOG_INFO, "xr: the %s hand's controller: %s", hand ? "right" : "left", name);
			}
		}
	}
	for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
	{
		struct halo_xr_hand *out = &frame->hands[hand];
		XrActionStateGetInfo information = { XR_TYPE_ACTION_STATE_GET_INFO };
		XrActionStatePose pose = { XR_TYPE_ACTION_STATE_POSE };
		XrSpaceLocation location = { XR_TYPE_SPACE_LOCATION };

		information.action = xr.grip_pose;
		information.subactionPath = xr.hands[hand].path;
		out->active = XR_SUCCEEDED(xrGetActionStatePose(xr.session, &information, &pose)) && pose.isActive;
		if (!out->active)
			continue;
		if (XR_SUCCEEDED(xrLocateSpace(xr.hands[hand].grip_space, xr.local_space,
			xr.frame_state.predictedDisplayTime, &location)))
		{
			out->flags = flags_from_location(location.locationFlags);
			pose_from_xr(&out->grip, &location.pose);
			if (hand == HALO_XR_HAND_LEFT)
			{
				xr.left_grip = location.pose;
				xr.left_grip_valid = (out->flags & HALO_XR_POSE_VALID) != 0;
			}
		}
		location.locationFlags = 0;
		if (XR_SUCCEEDED(xrLocateSpace(xr.hands[hand].aim_space, xr.local_space,
			xr.frame_state.predictedDisplayTime, &location)) &&
			(location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		{
			pose_from_xr(&out->aim, &location.pose);
		}
		else
		{
			out->aim = out->grip;
		}
		out->trigger = action_float(xr.trigger, hand);
		out->squeeze = action_float(xr.squeeze, hand);
		action_vector(xr.thumbstick, hand, out->thumbstick);
		out->buttons =
			(action_boolean(xr.button_a, hand) ? HALO_XR_BUTTON_A : 0) |
			(action_boolean(xr.button_b, hand) ? HALO_XR_BUTTON_B : 0) |
			(action_boolean(xr.thumbstick_click, hand) ? HALO_XR_BUTTON_THUMBSTICK : 0) |
			(action_boolean(xr.menu, hand) ? HALO_XR_BUTTON_MENU : 0) |
			(action_boolean(xr.thumbrest, hand) ? HALO_XR_BUTTON_THUMBREST : 0) |
			(out->trigger > 0.5f ? HALO_XR_BUTTON_TRIGGER : 0) |
			(out->squeeze > 0.5f ? HALO_XR_BUTTON_SQUEEZE : 0) |
			(action_boolean(xr.view, hand) ? HALO_XR_BUTTON_VIEW : 0) |
			(action_boolean(xr.bumper, hand) ? HALO_XR_BUTTON_BUMPER : 0) |
			(action_boolean(xr.dpad[0], hand) ? HALO_XR_BUTTON_DPAD_UP : 0) |
			(action_boolean(xr.dpad[1], hand) ? HALO_XR_BUTTON_DPAD_DOWN : 0) |
			(action_boolean(xr.dpad[2], hand) ? HALO_XR_BUTTON_DPAD_LEFT : 0) |
			(action_boolean(xr.dpad[3], hand) ? HALO_XR_BUTTON_DPAD_RIGHT : 0);
	}
	frame->controller_layout = xr.frame_layout ? HALO_XR_LAYOUT_FRAME : HALO_XR_LAYOUT_TOUCH;
	if (xr.frame_layout)
	{
		/* (the Frame's right X, Y and menu button: the game's X, Y and Start,
		which the Quest has on the left) */
		struct halo_xr_hand *left = &frame->hands[HALO_XR_HAND_LEFT];
		struct halo_xr_hand *right = &frame->hands[HALO_XR_HAND_RIGHT];

		if (action_boolean(xr.button_x, HALO_XR_HAND_RIGHT))
			left->buttons |= HALO_XR_BUTTON_A;
		if (action_boolean(xr.button_y, HALO_XR_HAND_RIGHT))
			left->buttons |= HALO_XR_BUTTON_B;
		if (right->buttons & HALO_XR_BUTTON_MENU)
		{
			right->buttons &= ~HALO_XR_BUTTON_MENU;
			left->buttons |= HALO_XR_BUTTON_MENU;
		}
	}
}

/* ---------- refresh rate (XR_FB_display_refresh_rate) */

static void refresh_rate_choose(float wanted)
{
	float rates[16];
	uint32_t count = 0, index;
	float best = 0.0f;

	if (!xr.has_refresh_rate ||
		!XR_SUCCEEDED(xr.enumerate_refresh_rates(xr.session, 16, &count, rates)))
	{
		return;
	}
	{
		char line[128] = "";

		for (index = 0; index < count; index++)
			snprintf(line + strlen(line), sizeof(line) - strlen(line), " %.0f", rates[index]);
		host_logf(HOST_LOG_INFO, "xr: refresh rates offered:%s Hz", line);
	}
	/* the highest rate not above the one wanted (0: the highest) */
	for (index = 0; index < count; index++)
	{
		if ((wanted <= 0.0f || rates[index] <= wanted + 0.5f) && rates[index] > best)
			best = rates[index];
	}
	if (best > 0.0f && XR_SUCCEEDED(xr.request_refresh_rate(xr.session, best)))
		host_logf(HOST_LOG_INFO, "xr: display refresh rate %.0f Hz", best);
}

/* ---------- the wrist layer (the motion tracker)

On the back of the left wrist, like a watch: behind the palm, where the
grip pose is, and out of the back of the hand. In the left grip pose -Z
runs to the fingers, +Y up the handle and -X out of the back of the hand;
the layer faces -X with its top towards the elbow, as the wrist turned to
the eyes reads it. Shown only while it is. */

#define WRIST_IMAGE_SIZE 512
#define CROSSHAIR_IMAGE_SIZE 256
/* the names layer: HALO_XR_MAXIMUM_NAMES slots of 4:1, one over another */
#define NAMES_IMAGE_WIDTH 512
#define NAMES_IMAGE_HEIGHT (NAMES_IMAGE_WIDTH / 4 * HALO_XR_MAXIMUM_NAMES)
/* a name's slot, as the game draws it: wider than its image's 4:1 (the
game's 640x480 drawn square): the height a slot shows at, to its width */
#define NAME_SLOT_ASPECT (30.0f / 160.0f)
#define WRIST_BEHIND_PALM 0.05f
#define WRIST_DOWN_HANDLE 0.02f
#define WRIST_ABOVE_HAND 0.04f
#define WRIST_PI 3.14159265f
#define WRIST_FACING_EYES 0.35f

static void quaternion_multiply(const XrQuaternionf *a, const XrQuaternionf *b, XrQuaternionf *out)
{
	XrQuaternionf result;

	result.w = a->w * b->w - a->x * b->x - a->y * b->y - a->z * b->z;
	result.x = a->w * b->x + a->x * b->w + a->y * b->z - a->z * b->y;
	result.y = a->w * b->y - a->x * b->z + a->y * b->w + a->z * b->x;
	result.z = a->w * b->z + a->x * b->y - a->y * b->x + a->z * b->w;
	*out = result;
}

static void quaternion_rotate_vector(const XrQuaternionf *q, const XrVector3f *v, XrVector3f *out)
{
	float tx = 2.0f * (q->y * v->z - q->z * v->y);
	float ty = 2.0f * (q->z * v->x - q->x * v->z);
	float tz = 2.0f * (q->x * v->y - q->y * v->x);

	out->x = v->x + q->w * tx + (q->y * tz - q->z * ty);
	out->y = v->y + q->w * ty + (q->z * tx - q->x * tz);
	out->z = v->z + q->w * tz + (q->x * ty - q->y * tx);
}

static int wrist_pose(const struct halo_xr_submit *submit, XrPosef *pose)
{
	/* the layer's axes in the grip's: x = -Y, y = +Z, z = -X, so the
	layer's right and down, as it is read, are the grip's -Y and -Z */
	static const XrQuaternionf layer_in_grip = { 0.5f, -0.5f, -0.5f, 0.5f };
	const XrVector3f offset = { -WRIST_ABOVE_HAND, -WRIST_DOWN_HANDLE - submit->wrist_right,
		WRIST_BEHIND_PALM - submit->wrist_down };
	const XrVector3f layer_normal = { 0.0f, 0.0f, 1.0f };
	/* clockwise as read: about the layer's normal, negatively */
	float half_turn = -submit->wrist_turn * WRIST_PI / 360.0f;
	XrQuaternionf turn = { 0.0f, 0.0f, sinf(half_turn), cosf(half_turn) };
	XrQuaternionf unturned;
	XrVector3f moved, normal, to_eyes;
	float distance;

	if (!xr.left_grip_valid)
		return 0;
	quaternion_rotate_vector(&xr.left_grip.orientation, &offset, &moved);
	pose->position.x = xr.left_grip.position.x + moved.x;
	pose->position.y = xr.left_grip.position.y + moved.y;
	pose->position.z = xr.left_grip.position.z + moved.z;
	quaternion_multiply(&xr.left_grip.orientation, &layer_in_grip, &unturned);
	quaternion_multiply(&unturned, &turn, &pose->orientation);

	quaternion_rotate_vector(&pose->orientation, &layer_normal, &normal);
	to_eyes.x = xr.head.position.x - pose->position.x;
	to_eyes.y = xr.head.position.y - pose->position.y;
	to_eyes.z = xr.head.position.z - pose->position.z;
	distance = sqrtf(to_eyes.x * to_eyes.x + to_eyes.y * to_eyes.y + to_eyes.z * to_eyes.z);
	return distance > 0.0f &&
		(normal.x * to_eyes.x + normal.y * to_eyes.y + normal.z * to_eyes.z) / distance > WRIST_FACING_EYES;
}

/* ---------- public code */

/* Called by host_main.c before the game starts: whether there is a headset
(an OpenXR runtime with a head-mounted system), and the size of the eye
images. Creates the instance, which host_xr_initialize then uses. */
int host_xr_probe(float resolution_scale, int *eye_width, int *eye_height)
{
	XrExtensionProperties extensions[128];
	uint32_t extension_count = 0, index;
	const char *enabled[8];
	uint32_t enabled_count = 0;
	PFN_xrInitializeLoaderKHR initialize_loader = NULL;
	XrLoaderInitInfoAndroidKHR loader = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	XrInstanceCreateInfoAndroidKHR android = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	XrInstanceCreateInfo instance = { XR_TYPE_INSTANCE_CREATE_INFO };
	XrSystemGetInfo system = { XR_TYPE_SYSTEM_GET_INFO };
	XrSystemProperties properties = { XR_TYPE_SYSTEM_PROPERTIES };
	XrViewConfigurationView views[HALO_XR_EYE_COUNT];
	uint32_t view_count = 0;
	JNIEnv *environment;
	JavaVM *machine = NULL;
	jobject activity;
	int width, height;

	environment = (JNIEnv *)SDL_GetAndroidJNIEnv();
	activity = (jobject)SDL_GetAndroidActivity();
	if (!environment || !activity || (*environment)->GetJavaVM(environment, &machine) != JNI_OK)
	{
		host_logf(HOST_LOG_ERROR, "xr: no Java environment");
		return 0;
	}
	/* the loader and the runtime keep the activity */
	activity = (*environment)->NewGlobalRef(environment, activity);

	/* (The Steam Frame lists an implicit API layer of its own for Vulkan
	apps, XR_APILAYER_VALVE_fdm_injection, whose library an app cannot load
	from its namespace, failing xrCreateInstance; the Android loader reads
	no variable to turn it off, so the app carries a pass-through layer
	under that library's name: port/android/xr_layer/fdm_injection_stub.c) */

	if (!XR_SUCCEEDED(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
		(PFN_xrVoidFunction *)&initialize_loader)) || !initialize_loader)
	{
		host_logf(HOST_LOG_INFO, "xr: no OpenXR loader");
		return 0;
	}
	loader.applicationVM = machine;
	loader.applicationContext = activity;
	if (!XR_SUCCEEDED(initialize_loader((const XrLoaderInitInfoBaseHeaderKHR *)&loader)))
	{
		host_logf(HOST_LOG_INFO, "xr: no OpenXR runtime");
		return 0;
	}

	if (!XR_SUCCEEDED(xrEnumerateInstanceExtensionProperties(NULL, 0, &extension_count, NULL)) || !extension_count)
	{
		host_logf(HOST_LOG_INFO, "xr: no OpenXR runtime");
		return 0;
	}
	if (extension_count > 128)
		extension_count = 128;
	for (index = 0; index < extension_count; index++)
	{
		extensions[index].type = XR_TYPE_EXTENSION_PROPERTIES;
		extensions[index].next = NULL;
	}
	xrEnumerateInstanceExtensionProperties(NULL, extension_count, &extension_count, extensions);
	{
		/* (the runtime's extensions, in the log: what a headset offers) */
		char line[512] = "";

		for (index = 0; index < extension_count; index++)
		{
			if (strlen(line) + strlen(extensions[index].extensionName) + 2 >= sizeof(line))
			{
				host_logf(HOST_LOG_INFO, "xr: extensions %s", line);
				line[0] = 0;
			}
			strcat(line, " ");
			strcat(line, extensions[index].extensionName);
		}
		if (line[0])
			host_logf(HOST_LOG_INFO, "xr: extensions %s", line);
	}
	if (!has_extension(extensions, extension_count, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME))
	{
		host_logf(HOST_LOG_ERROR, "xr: the runtime has no OpenGL ES support");
		return 0;
	}
	enabled[enabled_count++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
	if (has_extension(extensions, extension_count, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME))
		enabled[enabled_count++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
	xr.has_performance_settings = has_extension(extensions, extension_count, XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME);
	if (xr.has_performance_settings)
		enabled[enabled_count++] = XR_EXT_PERFORMANCE_SETTINGS_EXTENSION_NAME;
	xr.has_frame_controller = has_extension(extensions, extension_count, "XR_VALVE_frame_controller_interaction");
	if (xr.has_frame_controller)
		enabled[enabled_count++] = "XR_VALVE_frame_controller_interaction";
	xr.has_refresh_rate = has_extension(extensions, extension_count, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME);
	if (xr.has_refresh_rate)
		enabled[enabled_count++] = XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME;
	xr.has_metrics = has_extension(extensions, extension_count, XR_META_PERFORMANCE_METRICS_EXTENSION_NAME);
	if (xr.has_metrics)
		enabled[enabled_count++] = XR_META_PERFORMANCE_METRICS_EXTENSION_NAME;

	android.applicationVM = machine;
	android.applicationActivity = activity;
	instance.next = &android;
	strcpy(instance.applicationInfo.applicationName, "Halo VR");
	instance.applicationInfo.applicationVersion = 1;
	strcpy(instance.applicationInfo.engineName, "halo-ce-universal");
	instance.applicationInfo.engineVersion = 1;
	instance.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	instance.enabledExtensionCount = enabled_count;
	instance.enabledExtensionNames = enabled;
	if (!xr_ok(xrCreateInstance(&instance, &xr.instance), "xrCreateInstance"))
		return 0;

	system.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	if (!XR_SUCCEEDED(xrGetSystem(xr.instance, &system, &xr.system)))
	{
		host_logf(HOST_LOG_INFO, "xr: no headset");
		xrDestroyInstance(xr.instance);
		xr.instance = XR_NULL_HANDLE;
		return 0;
	}
	xrGetSystemProperties(xr.instance, xr.system, &properties);
	strncpy(xr.system_name, properties.systemName, sizeof(xr.system_name) - 1);
	{
		/* (SteamVR: the quads' alpha from their colour; HALO_XR_QUAD_ALPHA
		"colour" or "keep" says otherwise, for trying) */
		XrInstanceProperties instance = { XR_TYPE_INSTANCE_PROPERTIES };
		const char *choice = getenv("HALO_XR_QUAD_ALPHA");

		if (XR_SUCCEEDED(xrGetInstanceProperties(xr.instance, &instance)))
		{
			xr.quad_alpha_from_colour = strstr(instance.runtimeName, "SteamVR") != NULL;
			xr.steamvr = xr.quad_alpha_from_colour;
			host_logf(HOST_LOG_INFO, "xr: runtime %s", instance.runtimeName);
		}
		if (choice)
			xr.quad_alpha_from_colour = !strcmp(choice, "colour");
		if (xr.quad_alpha_from_colour)
			host_logf(HOST_LOG_INFO, "xr: the overlays' alpha from their colour");
	}

	views[0].type = views[1].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
	views[0].next = views[1].next = NULL;
	if (!xr_ok(xrEnumerateViewConfigurationViews(xr.instance, xr.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
		HALO_XR_EYE_COUNT, &view_count, views), "xrEnumerateViewConfigurationViews") || view_count != HALO_XR_EYE_COUNT)
	{
		return 0;
	}
	if (resolution_scale <= 0.0f)
		resolution_scale = 1.0f;
	width = (int)(views[0].recommendedImageRectWidth * resolution_scale + 0.5f) & ~1;
	height = (int)(views[0].recommendedImageRectHeight * resolution_scale + 0.5f) & ~1;
	if (width > (int)views[0].maxImageRectWidth)
		width = (int)views[0].maxImageRectWidth;
	if (height > (int)views[0].maxImageRectHeight)
		height = (int)views[0].maxImageRectHeight;
	xr.eye_width = *eye_width = width;
	xr.eye_height = *eye_height = height;
	host_logf(HOST_LOG_INFO, "xr: %s, eyes %dx%d (recommended %ux%u)", xr.system_name, width, height,
		views[0].recommendedImageRectWidth, views[0].recommendedImageRectHeight);
	return 1;
}

/* the session, on the game's GL context, which is current on this thread
(the game's) */
int host_xr_initialize(struct halo_xr_info *info, float wanted_refresh_rate)
{
	PFN_xrGetOpenGLESGraphicsRequirementsKHR requirements_function = NULL;
	XrGraphicsRequirementsOpenGLESKHR requirements = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	XrSessionCreateInfo session = { XR_TYPE_SESSION_CREATE_INFO };
	XrReferenceSpaceCreateInfo space = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	EGLint config_id = 0, config_count = 0;
	EGLint config_attributes[] = { EGL_CONFIG_ID, 0, EGL_NONE };
	int layer;

	memset(info, 0, sizeof(*info));
	if (xr.initialized || !xr.instance)
		return 0;

	if (!xr_ok(xrGetInstanceProcAddr(xr.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
		(PFN_xrVoidFunction *)&requirements_function), "xrGetOpenGLESGraphicsRequirementsKHR"))
	{
		return 0;
	}
	/* required before xrCreateSession */
	requirements_function(xr.instance, xr.system, &requirements);

	binding.display = eglGetCurrentDisplay();
	binding.context = eglGetCurrentContext();
	eglQueryContext(binding.display, binding.context, EGL_CONFIG_ID, &config_id);
	config_attributes[1] = config_id;
	eglChooseConfig(binding.display, config_attributes, &binding.config, 1, &config_count);
	if (binding.context == EGL_NO_CONTEXT || config_count != 1)
	{
		host_logf(HOST_LOG_ERROR, "xr: no current EGL context");
		return 0;
	}
	session.next = &binding;
	session.systemId = xr.system;
	if (!xr_ok(xrCreateSession(xr.instance, &session, &xr.session), "xrCreateSession"))
		return 0;

	space.poseInReferenceSpace.orientation.w = 1.0f;
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	if (!xr_ok(xrCreateReferenceSpace(xr.session, &space, &xr.local_space), "xrCreateReferenceSpace(LOCAL)"))
		return 0;
	space.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	if (!xr_ok(xrCreateReferenceSpace(xr.session, &space, &xr.view_space), "xrCreateReferenceSpace(VIEW)"))
		return 0;

	xr.format = choose_format();
	if (!xr.format)
		return 0;
	for (layer = 0; layer < HALO_XR_LAYER_COUNT; layer++)
	{
		int size = layer == HALO_XR_LAYER_WRIST || layer == HALO_XR_LAYER_SCOPE ? WRIST_IMAGE_SIZE :
			layer == HALO_XR_LAYER_CROSSHAIR ? CROSSHAIR_IMAGE_SIZE : 0;
		int width = size ? size : xr.eye_width;
		int height = size ? size : xr.eye_height;

		if (layer == HALO_XR_LAYER_NAMES)
		{
			width = NAMES_IMAGE_WIDTH;
			height = NAMES_IMAGE_HEIGHT;
		}
		if (!swapchain_create(&xr.swapchains[layer], width, height))
			return 0;
	}
	if (!copy_initialize() || !actions_initialize())
		return 0;

	if (xr.has_refresh_rate)
	{
		xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRefreshRateFB",
			(PFN_xrVoidFunction *)&xr.request_refresh_rate);
		xrGetInstanceProcAddr(xr.instance, "xrGetDisplayRefreshRateFB",
			(PFN_xrVoidFunction *)&xr.get_refresh_rate);
		xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRefreshRatesFB",
			(PFN_xrVoidFunction *)&xr.enumerate_refresh_rates);
		xr.has_refresh_rate = xr.request_refresh_rate && xr.get_refresh_rate && xr.enumerate_refresh_rates;
		refresh_rate_choose(wanted_refresh_rate);
		if (xr.has_refresh_rate)
			xr.get_refresh_rate(xr.session, &info->refresh_rate);
	}

	if (xr.has_metrics)
	{
		PFN_xrEnumeratePerformanceMetricsCounterPathsMETA enumerate = NULL;
		PFN_xrSetPerformanceMetricsStateMETA set_state = NULL;
		XrPerformanceMetricsStateMETA state = { XR_TYPE_PERFORMANCE_METRICS_STATE_META };

		xrGetInstanceProcAddr(xr.instance, "xrEnumeratePerformanceMetricsCounterPathsMETA",
			(PFN_xrVoidFunction *)&enumerate);
		xrGetInstanceProcAddr(xr.instance, "xrSetPerformanceMetricsStateMETA", (PFN_xrVoidFunction *)&set_state);
		xrGetInstanceProcAddr(xr.instance, "xrQueryPerformanceMetricsCounterMETA",
			(PFN_xrVoidFunction *)&xr.query_metric);
		state.enabled = XR_TRUE;
		xr.has_metrics = enumerate && set_state && xr.query_metric &&
			XR_SUCCEEDED(enumerate(xr.instance, 16, &xr.metric_count, xr.metric_paths)) &&
			XR_SUCCEEDED(set_state(xr.session, &state));
		host_logf(HOST_LOG_INFO, "xr: performance metrics %s (%u counters)", xr.has_metrics ? "on" : "off",
			xr.has_metrics ? xr.metric_count : 0);
	}

	if (xr.has_performance_settings)
	{
		/* the game's one busy thread and its GPU work both need the clocks
		up; left alone, Horizon OS judges the load by the device's average
		and keeps them low */
		PFN_xrPerfSettingsSetPerformanceLevelEXT set_level = NULL;

		xrGetInstanceProcAddr(xr.instance, "xrPerfSettingsSetPerformanceLevelEXT", (PFN_xrVoidFunction *)&set_level);
		if (set_level)
		{
			set_level(xr.session, XR_PERF_SETTINGS_DOMAIN_CPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
			set_level(xr.session, XR_PERF_SETTINGS_DOMAIN_GPU_EXT, XR_PERF_SETTINGS_LEVEL_SUSTAINED_HIGH_EXT);
			host_logf(HOST_LOG_INFO, "xr: CPU and GPU at sustained high");
		}
	}

	info->eye_width = xr.eye_width;
	info->eye_height = xr.eye_height;
	info->overlay_width = xr.eye_width;
	info->overlay_height = xr.eye_height;
	strncpy(info->system_name, xr.system_name, sizeof(info->system_name) - 1);
	xr.initialized = 1;
	host_logf(HOST_LOG_INFO, "xr: session created (swapchain format 0x%llx)", (unsigned long long)xr.format);
	return 1;
}

static void handle_events(void)
{
	XrEventDataBuffer event;

	for (;;)
	{
		event.type = XR_TYPE_EVENT_DATA_BUFFER;
		event.next = NULL;
		if (xrPollEvent(xr.instance, &event) != XR_SUCCESS)
			break;
		switch (event.type)
		{
		case XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED:
		{
			const XrEventDataSessionStateChanged *changed = (const XrEventDataSessionStateChanged *)&event;

			xr.state = changed->state;
			host_logf(HOST_LOG_INFO, "xr: session state %d", (int)xr.state);
			switch (xr.state)
			{
			case XR_SESSION_STATE_READY:
			{
				XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };

				begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
				if (xr_ok(xrBeginSession(xr.session, &begin), "xrBeginSession"))
					xr.running = 1;
				break;
			}
			case XR_SESSION_STATE_STOPPING:
				xrEndSession(xr.session);
				xr.running = 0;
				break;
			case XR_SESSION_STATE_EXITING:
			case XR_SESSION_STATE_LOSS_PENDING:
				xr.exit_requested = 1;
				break;
			default:
				break;
			}
			xr.focused = xr.state == XR_SESSION_STATE_FOCUSED;
			break;
		}
		case XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING:
			xr.recentred = 1;
			break;
		case XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING:
			xr.exit_requested = 1;
			break;
		default:
			break;
		}
	}
}

int host_xr_begin_frame(struct halo_xr_frame *frame)
{
	XrFrameWaitInfo wait = { XR_TYPE_FRAME_WAIT_INFO };
	XrFrameBeginInfo begin = { XR_TYPE_FRAME_BEGIN_INFO };
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	XrSpaceLocation head = { XR_TYPE_SPACE_LOCATION };
	uint32_t view_count = 0;
	int eye, layer;

	memset(frame, 0, sizeof(*frame));
	if (!xr.initialized)
		return 0;
	handle_events();
	if (xr.exit_requested)
		host_exit(0);
	frame->recentred = xr.recentred;
	xr.recentred = 0;
	if (!xr.running)
	{
		/* not shown (headset off, or the session not yet begun): the game
		keeps going at about the display's pace */
		usleep(11000);
		return 1;
	}
	if (xr.frame_begun)
		return 1;

	xr.frame_state.type = XR_TYPE_FRAME_STATE;
	xr.frame_state.next = NULL;
	if (!xr_ok(xrWaitFrame(xr.session, &wait, &xr.frame_state), "xrWaitFrame"))
		return 0;
	if (!xr_ok(xrBeginFrame(xr.session, &begin), "xrBeginFrame"))
		return 0;
	xr.frame_begun = 1;
	for (layer = 0; layer < HALO_XR_LAYER_COUNT; layer++)
		xr.swapchains[layer].filled = 0;

	frame->session_running = 1;
	frame->should_render = xr.frame_state.shouldRender != XR_FALSE;
	frame->focused = xr.focused;
	frame->predicted_display_seconds = (float)((double)xr.frame_state.predictedDisplayTime * 1e-9);

	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = xr.frame_state.predictedDisplayTime;
	locate.space = xr.local_space;
	xr.views[0].type = xr.views[1].type = XR_TYPE_VIEW;
	xr.views[0].next = xr.views[1].next = NULL;
	xr.views_valid = XR_SUCCEEDED(xrLocateViews(xr.session, &locate, &view_state, HALO_XR_EYE_COUNT, &view_count,
		xr.views)) && view_count == HALO_XR_EYE_COUNT &&
		(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
	if (xr.views_valid)
	{
		for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
		{
			struct halo_xr_view *view = &frame->views[eye];

			view->flags = flags_from_location(view_state.viewStateFlags);
			pose_from_xr(&view->pose, &xr.views[eye].pose);
			view->tangent_left = tanf(xr.views[eye].fov.angleLeft);
			view->tangent_right = tanf(xr.views[eye].fov.angleRight);
			view->tangent_up = tanf(xr.views[eye].fov.angleUp);
			view->tangent_down = tanf(xr.views[eye].fov.angleDown);
		}
	}
	if (XR_SUCCEEDED(xrLocateSpace(xr.view_space, xr.local_space, xr.frame_state.predictedDisplayTime, &head)))
	{
		frame->head_flags = flags_from_location(head.locationFlags);
		pose_from_xr(&frame->head, &head.pose);
		xr.head = head.pose;
	}
	xr.left_grip_valid = 0;
	if (xr.focused)
		hands_read(frame);
	return 1;
}

int host_xr_copy_layer(int layer, uint32_t texture, int clear_alpha)
{
	return host_xr_copy_layer_region(layer, texture, clear_alpha, 0.0f, 0.0f, 1.0f, 1.0f);
}

/* copies a layer of a texture array (the eyes drawn at once) into a
layer's image, opaque */
int host_xr_copy_layer_array(int layer, uint32_t texture, int array_layer)
{
	int copied;

	if (!xr.array_program)
		return 0;
	xr.copy_array_layer = array_layer;
	copied = host_xr_copy_layer_region(layer, texture, HALO_XR_COPY_OPAQUE, 0.0f, 0.0f, 1.0f, 1.0f);
	xr.copy_array_layer = -1;
	return copied;
}

/* copies part of a render target (left, top, right, bottom, 0..1 from its
top left) into a layer's image */
int host_xr_copy_layer_region(int layer, uint32_t texture, int clear_alpha, float left, float top, float right,
	float bottom)
{
	struct xr_swapchain *swapchain;
	XrSwapchainImageAcquireInfo acquire = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	XrSwapchainImageReleaseInfo release = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	struct gl_saved_state saved;

	if (!xr.frame_begun || !xr.frame_state.shouldRender || layer < 0 || layer >= HALO_XR_LAYER_COUNT)
		return 0;
	swapchain = &xr.swapchains[layer];
	if (swapchain->filled)
		return 1;
	if (!xr_ok(xrAcquireSwapchainImage(swapchain->handle, &acquire, &swapchain->acquired_index),
		"xrAcquireSwapchainImage"))
	{
		return 0;
	}
	wait.timeout = XR_INFINITE_DURATION;
	if (!xr_ok(xrWaitSwapchainImage(swapchain->handle, &wait), "xrWaitSwapchainImage"))
		return 0;

	gl_save(&saved);
	glBindFramebuffer(GL_DRAW_FRAMEBUFFER, swapchain->framebuffers[swapchain->acquired_index]);
	glViewport(0, 0, swapchain->width, swapchain->height);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);
	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glDisable(GL_STENCIL_TEST);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	if (texture && xr.copy_array_layer >= 0)
	{
		/* a layer of the eyes' texture array (multiview) */
		glUseProgram(xr.array_program);
		glUniform1i(xr.array_source_uniform, 0);
		glUniform4f(xr.array_rect_uniform, left, top, right, bottom);
		glUniform1f(xr.array_layer_uniform, (float)xr.copy_array_layer);
		if ((layer == HALO_XR_LAYER_EYE_LEFT || layer == HALO_XR_LAYER_EYE_RIGHT) && xr.vignette > 0.001f)
		{
			const XrFovf *fov = &xr.views[layer - HALO_XR_LAYER_EYE_LEFT].fov;

			glUniform1f(xr.array_vignette_uniform, xr.vignette);
			glUniform4f(xr.array_tangents_uniform, tanf(fov->angleLeft), tanf(fov->angleRight),
				tanf(fov->angleDown), tanf(fov->angleUp));
		}
		else
		{
			glUniform1f(xr.array_vignette_uniform, 0.0f);
		}
		glBindVertexArray(xr.vertex_array);
		glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
		glBindSampler(0, xr.sampler);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glBindTexture(GL_TEXTURE_2D_ARRAY, 0);
		if (clear_alpha == HALO_XR_COPY_OPAQUE)
		{
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	else if (texture)
	{
		GLint swizzle_r, swizzle_b;

		glUseProgram(xr.program);
		glUniform1i(xr.source_uniform, 0);
		glUniform4f(xr.rect_uniform, left, top, right, bottom);
		glUniform1f(xr.cut_disc_uniform, clear_alpha == HALO_XR_COPY_DISC ? 1.0f : 0.0f);
		glUniform1f(xr.alpha_from_colour_uniform, xr.quad_alpha_from_colour && clear_alpha == HALO_XR_COPY_ALPHA &&
			layer != HALO_XR_LAYER_EYE_LEFT && layer != HALO_XR_LAYER_EYE_RIGHT ? 1.0f : 0.0f);
		if ((layer == HALO_XR_LAYER_EYE_LEFT || layer == HALO_XR_LAYER_EYE_RIGHT) && xr.vignette > 0.001f)
		{
			const XrFovf *fov = &xr.views[layer - HALO_XR_LAYER_EYE_LEFT].fov;

			glUniform1f(xr.vignette_uniform, xr.vignette);
			glUniform4f(xr.tangents_uniform, tanf(fov->angleLeft), tanf(fov->angleRight), tanf(fov->angleDown),
				tanf(fov->angleUp));
		}
		else
		{
			glUniform1f(xr.vignette_uniform, 0.0f);
		}
		glBindVertexArray(xr.vertex_array);
		glBindTexture(GL_TEXTURE_2D, texture);
		glBindSampler(0, xr.sampler);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, &swizzle_r);
		glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, &swizzle_b);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, GL_RED);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, GL_BLUE);
		glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_R, swizzle_r);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_B, swizzle_b);
		if (clear_alpha == HALO_XR_COPY_OPAQUE)
		{
			/* an opaque layer: the game's alpha channel holds other things */
			glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
			glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	else
	{
		glClearColor(0.0f, 0.0f, 0.0f, clear_alpha ? 1.0f : 0.0f);
		glClear(GL_COLOR_BUFFER_BIT);
	}
	gl_restore(&saved);

	xrReleaseSwapchainImage(swapchain->handle, &release);
	swapchain->filled = 1;
	return 1;
}

/* The Steam Frame: Android in a container, without XR_EXT_performance_settings
to ask for the clocks, and the scheduler free to put the game's one busy
thread, which simulates and draws each frame, on a slow core (its two
2.3 GHz ones) now and then: frames late to the compositor, shown twice,
the picture doubled as the head turns. The thread that ends the frames is
kept on the cores no slower than nine tenths of the fastest. */
static void xr_fast_cores(void)
{
	cpu_set_t set;
	long fastest = 0, speeds[CPU_SETSIZE];
	int cpu, count = 0;
	char line[128] = "";

	for (cpu = 0; cpu < CPU_SETSIZE && cpu < 64; cpu++)
	{
		char path[96];
		FILE *file;

		speeds[cpu] = 0;
		snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu);
		file = fopen(path, "r");
		if (!file)
			continue;
		if (fscanf(file, "%ld", &speeds[cpu]) != 1)
			speeds[cpu] = 0;
		fclose(file);
		if (speeds[cpu] > fastest)
			fastest = speeds[cpu];
	}
	if (!fastest)
	{
		host_logf(HOST_LOG_INFO, "xr: the cores' speeds unknown: the frames' thread left where it runs");
		return;
	}
	CPU_ZERO(&set);
	for (cpu = 0; cpu < CPU_SETSIZE && cpu < 64; cpu++)
	{
		if (speeds[cpu] && speeds[cpu] * 10 >= fastest * 9)
		{
			char item[8];

			CPU_SET(cpu, &set);
			count++;
			snprintf(item, sizeof(item), " %d", cpu);
			strcat(line, item);
		}
	}
	if (count && sched_setaffinity(0, sizeof(set), &set) == 0)
		host_logf(HOST_LOG_INFO, "xr: the frames' thread on the fast cores:%s (fastest %ld MHz)", line, fastest / 1000);
	else
		host_logf(HOST_LOG_INFO, "xr: the frames' thread could not be kept on the fast cores");
}

int host_xr_end_frame(const struct halo_xr_submit *submit)
{
	XrFrameEndInfo end = { XR_TYPE_FRAME_END_INFO };
	XrCompositionLayerProjection projection = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
	XrCompositionLayerProjectionView projection_views[HALO_XR_EYE_COUNT];
	XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad wrist = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad scope = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad crosshair = { XR_TYPE_COMPOSITION_LAYER_QUAD };
	XrCompositionLayerQuad names[HALO_XR_MAXIMUM_NAMES];
	const XrCompositionLayerBaseHeader *layers[5 + HALO_XR_MAXIMUM_NAMES];
	uint32_t layer_count = 0;
	int eye;

	if (!xr.frame_begun)
		return 0;
	xr.frame_begun = 0;

	if (xr.frame_state.shouldRender && submit->eyes && xr.views_valid &&
		xr.swapchains[HALO_XR_LAYER_EYE_LEFT].filled && xr.swapchains[HALO_XR_LAYER_EYE_RIGHT].filled)
	{
		for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
		{
			struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_EYE_LEFT + eye];

			memset(&projection_views[eye], 0, sizeof(projection_views[eye]));
			projection_views[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			projection_views[eye].pose = xr.views[eye].pose;
			projection_views[eye].fov = xr.views[eye].fov;
			projection_views[eye].subImage.swapchain = swapchain->handle;
			projection_views[eye].subImage.imageRect.extent.width = swapchain->width;
			projection_views[eye].subImage.imageRect.extent.height = swapchain->height;
		}
		projection.space = xr.local_space;
		projection.viewCount = HALO_XR_EYE_COUNT;
		projection.views = projection_views;
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&projection;
	}

	if (xr.frame_state.shouldRender && submit->overlay_mode != HALO_XR_OVERLAY_NONE &&
		xr.swapchains[HALO_XR_LAYER_OVERLAY].filled)
	{
		struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_OVERLAY];

		quad.subImage.swapchain = swapchain->handle;
		quad.subImage.imageRect.extent.width = swapchain->width;
		quad.subImage.imageRect.extent.height = swapchain->height;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.size.width = submit->overlay_width;
		quad.size.height = submit->overlay_width * 0.75f;
		if (submit->overlay_mode == HALO_XR_OVERLAY_HEAD_LOCKED)
		{
			/* the HUD over the scene: premultiplied alpha, so its additive
			parts (alpha 0) add to the scene */
			quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.space = xr.view_space;
			quad.pose.orientation.w = 1.0f;
			quad.pose.position.z = -submit->overlay_distance;
		}
		else
		{
			if (submit->overlay_blend)
				quad.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad.space = xr.local_space;
			pose_to_xr(&quad.pose, &submit->screen);
		}
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&quad;
	}

	if (xr.frame_state.shouldRender && submit->wrist && xr.swapchains[HALO_XR_LAYER_WRIST].filled &&
		wrist_pose(submit, &wrist.pose))
	{
		struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_WRIST];

		wrist.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		wrist.space = xr.local_space;
		wrist.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		wrist.subImage.swapchain = swapchain->handle;
		wrist.subImage.imageRect.extent.width = swapchain->width;
		wrist.subImage.imageRect.extent.height = swapchain->height;
		wrist.size.width = wrist.size.height = submit->wrist_size;
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&wrist;
	}

	if (xr.frame_state.shouldRender && submit->scope && xr.swapchains[HALO_XR_LAYER_SCOPE].filled)
	{
		struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_SCOPE];

		scope.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		scope.space = xr.local_space;
		scope.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		scope.subImage.swapchain = swapchain->handle;
		scope.subImage.imageRect.extent.width = swapchain->width;
		scope.subImage.imageRect.extent.height = swapchain->height;
		scope.size.width = scope.size.height = submit->scope_size;
		pose_to_xr(&scope.pose, &submit->scope_pose);
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&scope;
	}

	if (xr.frame_state.shouldRender && submit->crosshair && xr.swapchains[HALO_XR_LAYER_CROSSHAIR].filled)
	{
		struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_CROSSHAIR];

		crosshair.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		crosshair.space = xr.local_space;
		crosshair.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		crosshair.subImage.swapchain = swapchain->handle;
		crosshair.subImage.imageRect.extent.width = swapchain->width;
		crosshair.subImage.imageRect.extent.height = swapchain->height;
		crosshair.size.width = crosshair.size.height = submit->crosshair_size;
		pose_to_xr(&crosshair.pose, &submit->crosshair_pose);
		layers[layer_count++] = (const XrCompositionLayerBaseHeader *)&crosshair;
	}

	if (xr.frame_state.shouldRender && submit->name_count > 0 && xr.swapchains[HALO_XR_LAYER_NAMES].filled)
	{
		struct xr_swapchain *swapchain = &xr.swapchains[HALO_XR_LAYER_NAMES];
		int slot_height = swapchain->height / HALO_XR_MAXIMUM_NAMES;
		int name;

		for (name = 0; name < submit->name_count && name < HALO_XR_MAXIMUM_NAMES; name++)
		{
			XrCompositionLayerQuad *quad_name = &names[name];

			memset(quad_name, 0, sizeof(*quad_name));
			quad_name->type = XR_TYPE_COMPOSITION_LAYER_QUAD;
			quad_name->layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			quad_name->space = xr.local_space;
			quad_name->eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			quad_name->subImage.swapchain = swapchain->handle;
			/* (slot 0 at the image's top: GL's rows run bottom up) */
			quad_name->subImage.imageRect.offset.x = 0;
			quad_name->subImage.imageRect.offset.y = swapchain->height - (name + 1) * slot_height;
			quad_name->subImage.imageRect.extent.width = swapchain->width;
			quad_name->subImage.imageRect.extent.height = slot_height;
			quad_name->size.width = submit->name_widths[name];
			quad_name->size.height = submit->name_widths[name] * NAME_SLOT_ASPECT;
			pose_to_xr(&quad_name->pose, &submit->name_poses[name]);
			layers[layer_count++] = (const XrCompositionLayerBaseHeader *)quad_name;
		}
	}

	if (xr.steamvr && !xr.cores_pinned)
	{
		xr.cores_pinned = 1;
		if (!getenv("HALO_XR_ANY_CORE"))
			xr_fast_cores();
	}
	/* (every five seconds, the compositor's counters, and the core the
	frames' thread is on) */
	if (xr.has_metrics)
	{
		struct timespec now;

		clock_gettime(CLOCK_MONOTONIC, &now);
		if (now.tv_sec - xr.metrics_logged.tv_sec >= 5)
		{
			char line[768] = "";
			uint32_t counter;

			xr.metrics_logged = now;
			for (counter = 0; counter < xr.metric_count && counter < 16; counter++)
			{
				XrPerformanceMetricsCounterMETA value = { XR_TYPE_PERFORMANCE_METRICS_COUNTER_META };
				char name[XR_MAX_PATH_LENGTH];
				uint32_t length = 0;
				char item[96];

				if (!XR_SUCCEEDED(xr.query_metric(xr.session, xr.metric_paths[counter], &value)) ||
					!XR_SUCCEEDED(xrPathToString(xr.instance, xr.metric_paths[counter], sizeof(name), &length, name)))
				{
					continue;
				}
				snprintf(item, sizeof(item), " %s=%g", strrchr(name, '/') ? strrchr(name, '/') + 1 : name,
					(value.counterFlags & XR_PERFORMANCE_METRICS_COUNTER_FLOAT_VALUE_VALID_BIT_META) ?
						(double)value.floatValue : (double)value.uintValue);
				if (strlen(line) + strlen(item) < sizeof(line))
					strcat(line, item);
			}
			host_logf(HOST_LOG_INFO, "xr: metrics%s, on core %d", line, sched_getcpu());
		}
	}

	end.displayTime = xr.frame_state.predictedDisplayTime;
	end.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	end.layerCount = layer_count;
	end.layers = layers;
	return xr_ok(xrEndFrame(xr.session, &end), "xrEndFrame");
}

void host_xr_set_vignette(float strength)
{
	xr.vignette = strength < 0.0f ? 0.0f : strength > 1.0f ? 1.0f : strength;
}

void host_xr_haptic(int hand, float amplitude, float seconds)
{
	XrHapticActionInfo information = { XR_TYPE_HAPTIC_ACTION_INFO };
	XrHapticVibration vibration = { XR_TYPE_HAPTIC_VIBRATION };

	if (!xr.initialized || !xr.running || hand < 0 || hand >= HALO_XR_HAND_COUNT)
		return;
	information.action = xr.haptic;
	information.subactionPath = xr.hands[hand].path;
	if (amplitude <= 0.0f)
	{
		xrStopHapticFeedback(xr.session, &information);
		return;
	}
	vibration.amplitude = amplitude > 1.0f ? 1.0f : amplitude;
	vibration.duration = (XrDuration)(seconds * 1e9);
	vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
	xrApplyHapticFeedback(xr.session, &information, (const XrHapticBaseHeader *)&vibration);
}

/* ---------- self-test

With a file named xr_test in the app's data folder the app runs this
instead of the game (host_main.c): each eye cleared to its own colour
(red while the right trigger is held), and a checkerboard screen standing
in the room, which shows whether tracking, the layers and the sRGB copy
work, without any game data. The controllers' buttons are logged. */

static void test_fill(GLuint framebuffer, int width, int height, int eye, const struct halo_xr_frame *frame)
{
	const struct halo_xr_hand *right = &frame->hands[HALO_XR_HAND_RIGHT];
	float trigger = right->trigger;
	int x, y;

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glDisable(GL_SCISSOR_TEST);
	if (eye >= 0)
	{
		/* left blue, right green; in gamma space, as the game draws */
		glClearColor(0.2f + 0.8f * trigger, eye == 0 ? 0.2f : 0.5f, eye == 0 ? 0.6f : 0.2f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		/* a mid-grey bar: the same brightness as the screen's grey squares
		if the colours come through unchanged */
		glEnable(GL_SCISSOR_TEST);
		glScissor(0, height / 2 - height / 40, width, height / 20);
		glClearColor(0.5f, 0.5f, 0.5f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT);
		glDisable(GL_SCISSOR_TEST);
		return;
	}
	glEnable(GL_SCISSOR_TEST);
	for (y = 0; y < 6; y++)
	{
		for (x = 0; x < 8; x++)
		{
			float value = (x + y) & 1 ? 0.5f : 1.0f;

			glScissor(x * width / 8, y * height / 6, width / 8 + 1, height / 6 + 1);
			/* the top-left square is red, to show which way is up */
			if (x == 0 && y == 0)
				glClearColor(1.0f, 0.0f, 0.0f, 1.0f);
			else
				glClearColor(value, value, value, 1.0f);
			glClear(GL_COLOR_BUFFER_BIT);
		}
	}
	glDisable(GL_SCISSOR_TEST);
}

void host_xr_test(void)
{
	struct halo_xr_info info;
	struct halo_xr_frame frame;
	struct halo_xr_submit submit;
	SDL_Window *window;
	SDL_GLContext context;
	GLuint texture, framebuffer;
	int eye_width, eye_height, eye, hand;
	int previous_buttons[HALO_XR_HAND_COUNT] = { 0, 0 };

	host_logf(HOST_LOG_INFO, "xr test: starting");
	if (!xr.instance && !host_xr_probe(1.0f, &eye_width, &eye_height))
		host_fatal("VR test: no OpenXR headset found (see logcat, tag \"halo\")");
	SDL_InitSubSystem(SDL_INIT_VIDEO);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
	SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
	window = SDL_CreateWindow("Halo", 640, 480, SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN);
	context = window ? SDL_GL_CreateContext(window) : NULL;
	if (!context || !SDL_GL_MakeCurrent(window, context))
		host_fatal("VR test: no OpenGL ES context: %s", SDL_GetError());
	if (!host_xr_initialize(&info, 90.0f))
		host_fatal("VR test: the OpenXR session could not be created (see logcat, tag \"halo\")");
	host_logf(HOST_LOG_INFO, "xr test: %s, %dx%d, %.0f Hz", info.system_name, info.eye_width, info.eye_height,
		info.refresh_rate);

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, info.eye_width, info.eye_height);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

	for (;;)
	{
		if (!host_xr_begin_frame(&frame))
			continue;
		for (hand = 0; hand < HALO_XR_HAND_COUNT; hand++)
		{
			if (frame.hands[hand].buttons != previous_buttons[hand])
			{
				host_logf(HOST_LOG_INFO, "xr test: %s hand buttons %#x, trigger %.2f, grip %.2f, stick %.2f %.2f, "
					"grip at %.2f %.2f %.2f", hand ? "right" : "left", frame.hands[hand].buttons,
					frame.hands[hand].trigger, frame.hands[hand].squeeze, frame.hands[hand].thumbstick[0],
					frame.hands[hand].thumbstick[1], frame.hands[hand].grip.position[0],
					frame.hands[hand].grip.position[1], frame.hands[hand].grip.position[2]);
				previous_buttons[hand] = frame.hands[hand].buttons;
			}
		}
		if (!frame.session_running)
			continue;
		if (frame.should_render)
		{
			for (eye = 0; eye < HALO_XR_EYE_COUNT; eye++)
			{
				test_fill(framebuffer, info.eye_width, info.eye_height, eye, &frame);
				host_xr_copy_layer(HALO_XR_LAYER_EYE_LEFT + eye, texture, 1);
			}
			test_fill(framebuffer, info.eye_width, info.eye_height, -1, &frame);
			host_xr_copy_layer(HALO_XR_LAYER_OVERLAY, texture, 1);
		}
		memset(&submit, 0, sizeof(submit));
		submit.eyes = 1;
		submit.overlay_mode = HALO_XR_OVERLAY_SCREEN;
		submit.overlay_width = 1.6f;
		submit.overlay_distance = 2.0f;
		submit.screen.orientation[3] = 1.0f;
		submit.screen.position[2] = -2.0f;
		host_xr_end_frame(&submit);
	}
}
