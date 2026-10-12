/*
HOST_GL_WORKER.C

The shader programs' worker (port/linux/src/xgpu_program_cache.c): a thread
with a GL context of its own, sharing the game's objects, that makes the
programs the game asks for while the game goes on drawing. The drivers
compile a program the first time it is drawn with, 10 to 70 ms each on the
Quest's, and the frame waited for it: in VR a frame shown late, the picture
doubled as the head turns.

- host_gl_worker_start, on the game's thread with its context current:
  makes a context that shares the game's (surfaceless, or on a 1x1
  pbuffer) and the thread that keeps it current.
- host_gl_program_request: a program of these sources, from their binary
  where one is given and the driver takes it, else compiled and linked
  (its binary kept for the cache's file). The sources and the binary are
  copied: the caller's may go at once. Urgent requests (a draw waits for
  them) go before the others (a map's programs made ready as it loads).
- On the worker each program is linked, its samplers set to their units
  (as d3d8_gl.c sets them), drawn with once into a small target of its own
  (a degenerate triangle: the driver makes the variants it makes at a
  first draw now, not in the game's frame; "warm up"), and a fence put
  after it all.
- host_gl_program_poll, on the game's thread: done once the fence has
  passed (GL's rule for an object made in one context and used in another:
  the work finished before the other binds it); then the program is the
  game's (a program name is the same in both contexts).

host_gl_program_warm_up draws once with a program on the calling thread's
context instead (debug.shader_warm_up "load": on the game's, as a map
loads), for comparing the two.
*/

#include "host.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define JOB_BUCKETS 1024
/* the warm-up's target, each way */
#define WARM_UP_SIZE 16

enum
{
	JOB_QUEUED,
	JOB_WORKING,
	JOB_DONE,
	JOB_FAILED,
};

struct job
{
	struct job *next_in_bucket;
	struct job *next_in_queue;
	uint32_t id;
	int urgent;
	char *sources[2];
	void *binary;
	uint32_t binary_size;
	uint32_t binary_format;
	int keep_binary;
	int state;
	/* the results */
	GLuint program;
	GLsync sync;
	int from_binary;
	void *kept_binary;
	uint32_t kept_size;
	uint32_t kept_format;
	uint32_t microseconds;
};

/* a context's warm-up target: the eyes' kinds, one layer and two
(multiview), RGBA8 with a depth and stencil buffer */
struct warm_up_targets
{
	int made;
	/* each kind's framebuffer complete */
	int complete[2];
	GLuint framebuffers[2];
	GLuint textures[4];
	GLuint vertex_array;
	GLuint counters;
};

static struct
{
	int started;
	int running;
	int warm_up;
	EGLDisplay display;
	EGLContext context;
	EGLSurface surface;
	pthread_t thread;
	pthread_mutex_t lock;
	pthread_cond_t wake;
	pthread_cond_t ready;
	struct job *buckets[JOB_BUCKETS];
	struct job *urgent_first, *urgent_last;
	struct job *later_first, *later_last;
	uint32_t next_id;
	uint32_t waiting;
	int failures_logged;
	PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC framebuffer_texture_multiview;
	struct warm_up_targets targets[2];
} worker;

static uint64_t now_us(void)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (uint64_t)now.tv_sec * 1000000u + (uint64_t)now.tv_nsec / 1000u;
}

/* ---------- the warm-up draw */

static int targets_make(struct warm_up_targets *targets)
{
	GLint major = 0, minor = 0;
	int kind;

	if (targets->made)
		return targets->made > 0;
	targets->made = -1;
	glGenTextures(4, targets->textures);
	glGenFramebuffers(2, targets->framebuffers);
	for (kind = 0; kind < 2; kind++)
	{
		GLenum target = kind ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;

		glBindTexture(target, targets->textures[kind * 2]);
		if (kind)
			glTexStorage3D(target, 1, GL_RGBA8, WARM_UP_SIZE, WARM_UP_SIZE, 2);
		else
			glTexStorage2D(target, 1, GL_RGBA8, WARM_UP_SIZE, WARM_UP_SIZE);
		glBindTexture(target, targets->textures[kind * 2 + 1]);
		if (kind)
			glTexStorage3D(target, 1, GL_DEPTH24_STENCIL8, WARM_UP_SIZE, WARM_UP_SIZE, 2);
		else
			glTexStorage2D(target, 1, GL_DEPTH24_STENCIL8, WARM_UP_SIZE, WARM_UP_SIZE);
		glBindTexture(target, 0);
		glBindFramebuffer(GL_FRAMEBUFFER, targets->framebuffers[kind]);
		if (kind)
		{
			if (!worker.framebuffer_texture_multiview)
				continue;
			worker.framebuffer_texture_multiview(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, targets->textures[2], 0, 0, 2);
			worker.framebuffer_texture_multiview(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, targets->textures[3], 0,
				0, 2);
		}
		else
		{
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, targets->textures[0], 0);
			glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_TEXTURE_2D, targets->textures[1], 0);
		}
		targets->complete[kind] = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
		if (!targets->complete[kind])
			host_logf(HOST_LOG_WARN, "gl worker: the %s warm-up target is incomplete; no warm-up there",
				kind ? "multiview" : "plain");
	}
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glGenVertexArrays(1, &targets->vertex_array);
	/* (the visibility tests' programs count samples into binding 0) */
	glGetIntegerv(GL_MAJOR_VERSION, &major);
	glGetIntegerv(GL_MINOR_VERSION, &minor);
	if (major > 3 || (major == 3 && minor >= 1))
	{
		glGenBuffers(1, &targets->counters);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, targets->counters);
		glBufferData(GL_ATOMIC_COUNTER_BUFFER, 64, NULL, GL_DYNAMIC_DRAW);
		glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, 0);
	}
	targets->made = 1;
	return 1;
}

/* one degenerate triangle with the program into the target of its kind,
depth tested and blended as most of the game's draws are; the GL state it
changes is left changed (the worker's context has no other use, and the
game's sets it again: d3d8_host_state_changed) */
static void warm_up_draw(struct warm_up_targets *targets, GLuint program, int multiview)
{
	if (!targets_make(targets) || !targets->complete[multiview ? 1 : 0])
		return;
	glBindFramebuffer(GL_FRAMEBUFFER, targets->framebuffers[multiview ? 1 : 0]);
	glViewport(0, 0, WARM_UP_SIZE, WARM_UP_SIZE);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_STENCIL_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);
	glDepthMask(GL_TRUE);
	glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glBindVertexArray(targets->vertex_array);
	if (targets->counters)
		glBindBufferBase(GL_ATOMIC_COUNTER_BUFFER, 0, targets->counters);
	glUseProgram(program);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glUseProgram(0);
	glBindVertexArray(0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void host_gl_program_warm_up(uint32_t program, int multiview)
{
	if (!worker.running || !program)
		return;
	warm_up_draw(&worker.targets[0], program, multiview);
}

/* ---------- the worker's jobs */

static struct job *job_find(uint32_t id)
{
	struct job *job;

	for (job = worker.buckets[id % JOB_BUCKETS]; job; job = job->next_in_bucket)
	{
		if (job->id == id)
			return job;
	}
	return NULL;
}

static void queue_append(struct job *job)
{
	struct job **first = job->urgent ? &worker.urgent_first : &worker.later_first;
	struct job **last = job->urgent ? &worker.urgent_last : &worker.later_last;

	job->next_in_queue = NULL;
	if (*last)
		(*last)->next_in_queue = job;
	else
		*first = job;
	*last = job;
}

static struct job *queue_take(void)
{
	struct job *job = worker.urgent_first;

	if (job)
	{
		worker.urgent_first = job->next_in_queue;
		if (!worker.urgent_first)
			worker.urgent_last = NULL;
		return job;
	}
	job = worker.later_first;
	if (job)
	{
		worker.later_first = job->next_in_queue;
		if (!worker.later_first)
			worker.later_last = NULL;
	}
	return job;
}

static GLuint shader_compile(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		char log[1024];

		log[0] = 0;
		glGetShaderInfoLog(shader, sizeof(log), NULL, log);
		if (worker.failures_logged++ < 4)
			host_logf(HOST_LOG_WARN, "gl worker: a %s shader does not compile: %s",
				type == GL_VERTEX_SHADER ? "vertex" : "pixel", log);
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/* the job's program (outside the lock: only the worker touches a job
being worked on) */
static void job_work(struct job *job)
{
	uint64_t start = now_us();
	GLuint program = 0;
	GLint status = 0;

	if (job->binary && job->binary_size)
	{
		program = glCreateProgram();
		glProgramBinary(program, (GLenum)job->binary_format, job->binary, (GLsizei)job->binary_size);
		glGetProgramiv(program, GL_LINK_STATUS, &status);
		if (status)
			job->from_binary = 1;
		else
		{
			glDeleteProgram(program);
			program = 0;
			while (glGetError() != GL_NO_ERROR)
			{
			}
		}
	}
	if (!program && job->sources[0] && job->sources[1])
	{
		GLuint vertex = shader_compile(GL_VERTEX_SHADER, job->sources[0]);
		GLuint fragment = vertex ? shader_compile(GL_FRAGMENT_SHADER, job->sources[1]) : 0;

		if (vertex && fragment)
		{
			program = glCreateProgram();
			glProgramParameteri(program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
			glAttachShader(program, vertex);
			glAttachShader(program, fragment);
			glLinkProgram(program);
			glGetProgramiv(program, GL_LINK_STATUS, &status);
			if (!status)
			{
				char log[1024];

				log[0] = 0;
				glGetProgramInfoLog(program, sizeof(log), NULL, log);
				if (worker.failures_logged++ < 4)
					host_logf(HOST_LOG_WARN, "gl worker: a program does not link: %s", log);
				glDeleteProgram(program);
				program = 0;
			}
		}
		/* (attached, they go with the program) */
		if (vertex)
			glDeleteShader(vertex);
		if (fragment)
			glDeleteShader(fragment);
		if (program && job->keep_binary)
		{
			GLint length = 0;
			GLsizei written = 0;
			GLenum format = 0;

			glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
			if (length > 0 && (job->kept_binary = malloc((size_t)length)) != NULL)
			{
				glGetProgramBinary(program, length, &written, &format, job->kept_binary);
				if (written > 0 && written <= length)
				{
					job->kept_size = (uint32_t)written;
					job->kept_format = (uint32_t)format;
				}
				else
				{
					free(job->kept_binary);
					job->kept_binary = NULL;
				}
			}
		}
	}
	if (program)
	{
		int unit;

		/* the samplers' units, as d3d8_gl.c's program_get sets them (a
		first draw with two kinds of sampler on one unit draws nothing) */
		glUseProgram(program);
		for (unit = 0; unit < 4; unit++)
		{
			char name[8];

			snprintf(name, sizeof(name), "tex%d", unit);
			glUniform1i(glGetUniformLocation(program, name), unit);
		}
		glUseProgram(0);
		if (worker.warm_up)
			warm_up_draw(&worker.targets[1], program,
				job->sources[0] && strstr(job->sources[0], "GL_OVR_multiview") != NULL);
		job->sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
	}
	glFlush();
	job->program = program;
	job->microseconds = (uint32_t)(now_us() - start);
}

static void *worker_main(void *unused)
{
	(void)unused;
	if (!eglMakeCurrent(worker.display, worker.surface, worker.surface, worker.context))
	{
		host_logf(HOST_LOG_ERROR, "gl worker: its context cannot be made current (0x%x)", eglGetError());
		pthread_mutex_lock(&worker.lock);
		worker.started = -1;
		pthread_cond_broadcast(&worker.ready);
		pthread_mutex_unlock(&worker.lock);
		return NULL;
	}
	pthread_mutex_lock(&worker.lock);
	worker.started = 1;
	pthread_cond_broadcast(&worker.ready);
	for (;;)
	{
		struct job *job;

		while ((job = queue_take()) == NULL)
			pthread_cond_wait(&worker.wake, &worker.lock);
		job->state = JOB_WORKING;
		pthread_mutex_unlock(&worker.lock);
		job_work(job);
		pthread_mutex_lock(&worker.lock);
		free(job->binary);
		job->binary = NULL;
		free(job->sources[0]);
		free(job->sources[1]);
		job->sources[0] = job->sources[1] = NULL;
		job->state = job->program ? JOB_DONE : JOB_FAILED;
		worker.waiting--;
	}
	return NULL;
}

/* ---------- the game's */

int host_gl_worker_start(int warm_up)
{
	EGLDisplay display = eglGetCurrentDisplay();
	EGLContext shared = eglGetCurrentContext();
	EGLint config_id = 0, count = 0, version = 3;
	EGLint config_attributes[] = { EGL_CONFIG_ID, 0, EGL_NONE };
	EGLint context_attributes[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	EGLConfig config;
	const char *extensions;
	int started;

	if (worker.running)
		return 1;
	if (worker.started)
		return 0;
	worker.warm_up = warm_up;
	if (display == EGL_NO_DISPLAY || shared == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_WARN, "gl worker: no current context to share");
		worker.started = -1;
		return 0;
	}
	eglQueryContext(display, shared, EGL_CONFIG_ID, &config_id);
	eglQueryContext(display, shared, EGL_CONTEXT_CLIENT_VERSION, &version);
	config_attributes[1] = config_id;
	if (!eglChooseConfig(display, config_attributes, &config, 1, &count) || count != 1)
	{
		host_logf(HOST_LOG_WARN, "gl worker: the game's context's configuration is not found");
		worker.started = -1;
		return 0;
	}
	context_attributes[1] = version;
	extensions = eglQueryString(display, EGL_EXTENSIONS);
	worker.surface = EGL_NO_SURFACE;
	if (!extensions || !strstr(extensions, "EGL_KHR_surfaceless_context"))
	{
		/* (a 1x1 pbuffer to be current on, of a configuration that has one) */
		EGLint pbuffer_config[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
			EGL_NONE };
		EGLint pbuffer_size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };

		if (!eglChooseConfig(display, pbuffer_config, &config, 1, &count) || count != 1 ||
			(worker.surface = eglCreatePbufferSurface(display, config, pbuffer_size)) == EGL_NO_SURFACE)
		{
			host_logf(HOST_LOG_WARN, "gl worker: neither surfaceless contexts nor a pbuffer");
			worker.started = -1;
			return 0;
		}
	}
	worker.context = eglCreateContext(display, config, shared, context_attributes);
	if (worker.context == EGL_NO_CONTEXT)
	{
		host_logf(HOST_LOG_WARN, "gl worker: no context sharing the game's (0x%x)", eglGetError());
		worker.started = -1;
		return 0;
	}
	worker.display = display;
	worker.next_id = 1;
	worker.framebuffer_texture_multiview = (PFNGLFRAMEBUFFERTEXTUREMULTIVIEWOVRPROC)eglGetProcAddress(
		"glFramebufferTextureMultiviewOVR");
	pthread_mutex_init(&worker.lock, NULL);
	pthread_cond_init(&worker.wake, NULL);
	pthread_cond_init(&worker.ready, NULL);
	if (pthread_create(&worker.thread, NULL, worker_main, NULL) != 0)
	{
		eglDestroyContext(display, worker.context);
		worker.started = -1;
		return 0;
	}
	pthread_mutex_lock(&worker.lock);
	while (!worker.started)
		pthread_cond_wait(&worker.ready, &worker.lock);
	started = worker.started > 0;
	pthread_mutex_unlock(&worker.lock);
	worker.running = started;
	/* (Mesa's trace markers, for the new context's too: host_gl.c) */
	if (started)
		host_gl_quiet_trace_markers();
	host_logf(HOST_LOG_INFO, "gl worker: %s (ES %d, %s%s)", started ? "running" : "failed", (int)version,
		worker.surface == EGL_NO_SURFACE ? "surfaceless" : "on a pbuffer", warm_up ? ", programs warmed up" : "");
	return started;
}

uint32_t host_gl_program_request(int urgent, const char *vertex_source, const char *fragment_source,
	uint32_t binary_format, const void *binary, uint32_t binary_size, int keep_binary)
{
	struct job *job;
	uint32_t id;

	if (!worker.running)
		return 0;
	job = calloc(1, sizeof(*job));
	if (!job)
		return 0;
	job->urgent = urgent != 0;
	job->keep_binary = keep_binary != 0;
	job->sources[0] = vertex_source ? strdup(vertex_source) : NULL;
	job->sources[1] = fragment_source ? strdup(fragment_source) : NULL;
	if (binary && binary_size && (job->binary = malloc(binary_size)) != NULL)
	{
		memcpy(job->binary, binary, binary_size);
		job->binary_size = binary_size;
		job->binary_format = binary_format;
	}
	pthread_mutex_lock(&worker.lock);
	id = job->id = worker.next_id++;
	if (!worker.next_id)
		worker.next_id = 1;
	job->state = JOB_QUEUED;
	job->next_in_bucket = worker.buckets[id % JOB_BUCKETS];
	worker.buckets[id % JOB_BUCKETS] = job;
	queue_append(job);
	worker.waiting++;
	pthread_cond_signal(&worker.wake);
	pthread_mutex_unlock(&worker.lock);
	return id;
}

/* a queued job a draw now waits for: before the others */
void host_gl_program_hurry(uint32_t id)
{
	struct job *job, **link;

	if (!worker.running)
		return;
	pthread_mutex_lock(&worker.lock);
	job = job_find(id);
	if (job && job->state == JOB_QUEUED && !job->urgent)
	{
		for (link = &worker.later_first; *link; link = &(*link)->next_in_queue)
		{
			if (*link == job)
			{
				struct job *previous = NULL, *walk;

				*link = job->next_in_queue;
				/* (the last of the later queue found again) */
				for (walk = worker.later_first; walk; walk = walk->next_in_queue)
					previous = walk;
				worker.later_last = previous;
				job->urgent = 1;
				queue_append(job);
				break;
			}
		}
	}
	pthread_mutex_unlock(&worker.lock);
}

int host_gl_program_poll(uint32_t id, uint32_t *program, uint32_t *binary_size, uint32_t *microseconds)
{
	struct job *job;
	int result = 0;

	if (!worker.running)
		return -1;
	pthread_mutex_lock(&worker.lock);
	job = job_find(id);
	if (!job || job->state == JOB_FAILED)
		result = -1;
	else if (job->state == JOB_DONE)
	{
		/* (the fence passed: everything the worker did with the program is
		done, and this context may use it) */
		if (job->sync)
		{
			GLenum waited = glClientWaitSync(job->sync, 0, 0);

			if (waited == GL_ALREADY_SIGNALED || waited == GL_CONDITION_SATISFIED || waited == GL_WAIT_FAILED)
			{
				glDeleteSync(job->sync);
				job->sync = NULL;
			}
		}
		if (!job->sync)
		{
			*program = job->program;
			*binary_size = job->kept_size;
			*microseconds = job->microseconds;
			result = 1;
		}
	}
	pthread_mutex_unlock(&worker.lock);
	return result;
}

uint32_t host_gl_program_binary(uint32_t id, void *buffer, uint32_t size, uint32_t *format)
{
	struct job *job;
	uint32_t copied = 0;

	if (!worker.running)
		return 0;
	pthread_mutex_lock(&worker.lock);
	job = job_find(id);
	if (job && job->state == JOB_DONE && job->kept_binary && size >= job->kept_size)
	{
		memcpy(buffer, job->kept_binary, job->kept_size);
		*format = job->kept_format;
		copied = job->kept_size;
	}
	pthread_mutex_unlock(&worker.lock);
	return copied;
}

/* forgets a job (its program stays: the game's); a job still queued or
being worked on is let go when it is done */
void host_gl_program_release(uint32_t id)
{
	struct job *job, **link;

	if (!worker.running)
		return;
	pthread_mutex_lock(&worker.lock);
	for (link = &worker.buckets[id % JOB_BUCKETS]; (job = *link) != NULL; link = &job->next_in_bucket)
	{
		if (job->id != id)
			continue;
		if (job->state == JOB_QUEUED || job->state == JOB_WORKING)
			break;
		*link = job->next_in_bucket;
		if (job->sync)
			glDeleteSync(job->sync);
		free(job->kept_binary);
		free(job);
		break;
	}
	pthread_mutex_unlock(&worker.lock);
}

uint32_t host_gl_worker_waiting(void)
{
	uint32_t waiting;

	if (!worker.running)
		return 0;
	pthread_mutex_lock(&worker.lock);
	waiting = worker.waiting;
	pthread_mutex_unlock(&worker.lock);
	return waiting;
}
