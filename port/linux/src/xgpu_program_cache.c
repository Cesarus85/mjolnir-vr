/*
XGPU_PROGRAM_CACHE.C

The shader programs' binary cache, their making on a thread of their own,
and their making ready as a map loads (the Android app). Its drivers compile
a program the first time it is drawn with, 10 to 70 ms on the Quest's, which
the frame waited for: in VR a frame shown late, the picture doubled as the
head turns; a map's first seconds, a weapon or effect first seen and the
scores' screen compiled dozens at once.

- A program linked is written to shader_cache in the data folder: its
  binary (glGetProgramBinary), the driver that made it (a hash of
  GL_RENDERER and GL_VERSION) and its two shaders' sources, in a file named
  by the sources' hash (xgpu_program_key). A file is used again when the
  driver is the one that made it, the file is whole (its sizes and
  checksum) and its sources are the ones asked for.
- The programs are made on the worker (port/android/host/host_gl_worker.c:
  a thread whose GL context shares the game's): loaded from the file's
  binary, or compiled from the sources (the driver refused the binary, or
  there was none), and drawn with once there to warm them up. A draw whose
  program is not made yet is left out (d3d8_gl.c) until it is, a frame or a
  few later, and the frame does not wait. display.shader_worker false, or a
  context that cannot share, makes them on the game's thread as before.
- Each map keeps the list of the programs it drew with (shader_cache/maps/
  <map>.txt), and all maps together one more (shader_cache/all.txt). As a
  map loads (game_initialize_for_new_map), its own programs are asked of the
  worker, then everyone else's a few a frame: a map first played, or a
  weapon first fired, mostly finds its programs made already.

display.shader_cache false leaves it all out. A file the cache cannot use is
passed over; the folder may be deleted at any time.
*/

#include "xgpu.h"
#include "port_config.h"
#include "halo_linux_source_fixups.h"

#ifdef HALO_ANDROID

/* (the folder through posix_files.c's helpers: struct stat and struct
dirent have the platform layer's own) */
#include "posix.h"
#include "guest_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* "RPC1" */
#define PROGRAM_FILE_MAGIC 0x31435052u
/* raised with any change to the file's layout */
#define PROGRAM_FILE_VERSION 1u
#define MAXIMUM_BINARY_SIZE (16u << 20)
#define MAXIMUM_SOURCE_SIZE (1u << 20)
#define MAXIMUM_BINARY_FORMATS 16
/* past these at the start, the programs' files are deleted (the sources
change with the app's versions: old files are never asked for again) */
#define CACHE_MAXIMUM_BYTES (192ul << 20)
#define CACHE_MAXIMUM_FILES 12000ul
#define MAXIMUM_LISTED_PROGRAMS 8192ul
#define JOB_BUCKETS 1024
/* the lists are written this often while they grow */
#define LIST_WRITE_MICROSECONDS 20000000ull
/* without the worker, how long a map's load may compile */
#define PREWARM_COMPILE_MICROSECONDS 2000000ull
/* each frame: the other maps' programs asked of the worker, the jobs looked
at, and the binaries of those it compiled written */
#define REQUESTS_A_FRAME 8
#define POLLS_A_FRAME 16
#define WRITES_A_FRAME 2
/* debug.shader_warm_up "load": how long a map's load waits for its programs,
and draws with them */
#define LOAD_WAIT_MICROSECONDS 3000000ull
#define LOAD_WARM_UP_MICROSECONDS 3000000ull

struct program_file_header
{
	unsigned int magic;
	unsigned int version;
	unsigned int driver[2];
	unsigned int key[2];
	unsigned int binary_format;
	unsigned int binary_size;
	unsigned int vertex_size;
	unsigned int fragment_size;
	/* of everything after the header */
	unsigned int checksum;
	unsigned int reserved;
};

/* a program asked for (of the worker, or made here as a map loaded), until
the renderer takes it */
enum
{
	JOB_WAITING,
	JOB_READY,
	JOB_FAILED,
};

struct program_job
{
	struct program_job *next;
	unsigned long long key;
	/* the worker's number (0: made here) */
	unsigned int id;
	int state;
	GLuint program;
	/* a draw waits for it; and it was hurried along */
	BOOL urgent, hurried;
	/* its vertex program draws both eyes at once (debug.shader_warm_up
	"load": the target it is warmed up in) */
	BOOL multiview;
	/* the sources, for the file of a program the worker compiled */
	char *sources[2];
};

/* a set of keys (open addressing; 0 is no key) */
struct key_set
{
	unsigned long long *keys;
	unsigned long capacity, count;
};

static struct
{
	BOOL enabled;
	char folder[512];
	unsigned long long driver;
	GLint binary_formats[MAXIMUM_BINARY_FORMATS];
	int binary_format_count;
	unsigned long long bytes;
	unsigned long files;
	BOOL full_logged;

	/* the worker runs (host_gl_worker.c), and how programs are warmed up */
	BOOL worker;
	int warm_up;
	struct program_job *jobs[JOB_BUCKETS];
	/* the jobs the worker has not finished, looked at a few a frame */
	struct program_job **waiting;
	unsigned long waiting_count, waiting_capacity, waiting_next;
	/* the programs the renderer has (taken or stored here) */
	struct key_set linked;
	/* the other maps' programs still to ask for (as the map loaded) */
	unsigned long long *requests;
	unsigned long request_count, request_next, request_capacity;

	/* the map loaded (a file name), the programs it drew with or listed */
	char map[96];
	struct key_set used;
	BOOL used_changed;
	/* every map's (all.txt) */
	struct key_set all;
	BOOL all_changed;
	unsigned long long list_written_us;

	/* for the log: since the map loaded (asked for, taken made, of them
	compiled by the worker, read and stored here); binaries refused here */
	unsigned long requested, found_ready, compiled, found_file, stored;
	unsigned long refused;
} cache;

unsigned long xgpu_program_map_serial = 1;

/* ---------- hashes */

static unsigned long long hash_bytes(unsigned long long hash, const void *data, size_t size)
{
	const unsigned char *bytes = data;

	while (size--)
		hash = (hash ^ *bytes++) * 0x100000001b3ull;
	return hash;
}

unsigned long long xgpu_source_hash(const char *source)
{
	return hash_bytes(0xcbf29ce484222325ull, source, strlen(source));
}

static unsigned long long mix(unsigned long long value)
{
	value ^= value >> 30;
	value *= 0xbf58476d1ce4e5b9ull;
	value ^= value >> 27;
	value *= 0x94d049bb133111ebull;
	return value ^ (value >> 31);
}

unsigned long long xgpu_program_key(unsigned long long vertex_hash, unsigned long long fragment_hash)
{
	unsigned long long key = mix(vertex_hash ^ mix(fragment_hash + 0x9e3779b97f4a7c15ull));

	return key ? key : 1;
}

static unsigned int checksum(const unsigned char *data, size_t size)
{
	unsigned int hash = 2166136261u;

	while (size--)
		hash = (hash ^ *data++) * 16777619u;
	return hash;
}

/* ---------- key sets */

static BOOL key_set_has(const struct key_set *set, unsigned long long key)
{
	unsigned long slot;

	if (!set->capacity)
		return FALSE;
	for (slot = (unsigned long)mix(key) & (set->capacity - 1); set->keys[slot]; slot = (slot + 1) & (set->capacity - 1))
	{
		if (set->keys[slot] == key)
			return TRUE;
	}
	return FALSE;
}

/* TRUE if it is new to the set */
static BOOL key_set_add(struct key_set *set, unsigned long long key)
{
	unsigned long slot;

	if (!key || key_set_has(set, key))
		return FALSE;
	if ((set->count + 1) * 2 > set->capacity)
	{
		unsigned long capacity = set->capacity ? set->capacity * 2 : 256;
		unsigned long long *keys = calloc(capacity, sizeof(*keys));
		unsigned long index;

		if (!keys)
			return FALSE;
		for (index = 0; index < set->capacity; index++)
		{
			if (set->keys[index])
			{
				for (slot = (unsigned long)mix(set->keys[index]) & (capacity - 1); keys[slot];
					slot = (slot + 1) & (capacity - 1))
				{
				}
				keys[slot] = set->keys[index];
			}
		}
		free(set->keys);
		set->keys = keys;
		set->capacity = capacity;
	}
	for (slot = (unsigned long)mix(key) & (set->capacity - 1); set->keys[slot]; slot = (slot + 1) & (set->capacity - 1))
	{
	}
	set->keys[slot] = key;
	set->count++;
	return TRUE;
}

static void key_set_clear(struct key_set *set)
{
	if (set->keys)
		memset(set->keys, 0, set->capacity * sizeof(*set->keys));
	set->count = 0;
}

/* ---------- files */

static void program_path(char *path, size_t size, unsigned long long key)
{
	snprintf(path, size, "%s/%016llx.bin", cache.folder, key);
}

/* a program's file, whole, or NULL: header, then the vertex and fragment
sources and the binary (free() it) */
static unsigned char *program_file_read(unsigned long long key, struct program_file_header *header)
{
	char path[600];
	FILE *file;
	unsigned char *data = NULL;
	long size;
	size_t payload;

	program_path(path, sizeof(path), key);
	file = fopen(path, "rb");
	if (!file)
		return NULL;
	if (fread(header, sizeof(*header), 1, file) != 1 || header->magic != PROGRAM_FILE_MAGIC ||
		header->version != PROGRAM_FILE_VERSION || header->key[0] != (unsigned int)key ||
		header->key[1] != (unsigned int)(key >> 32) || header->binary_size > MAXIMUM_BINARY_SIZE ||
		header->vertex_size > MAXIMUM_SOURCE_SIZE || header->fragment_size > MAXIMUM_SOURCE_SIZE ||
		fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0)
	{
		fclose(file);
		return NULL;
	}
	payload = (size_t)header->vertex_size + header->fragment_size + header->binary_size;
	if ((unsigned long)size != sizeof(*header) + payload || fseek(file, (long)sizeof(*header), SEEK_SET) != 0 ||
		(data = malloc(payload + 2)) == NULL || fread(data, 1, payload, file) != payload ||
		checksum(data, payload) != header->checksum)
	{
		free(data);
		fclose(file);
		return NULL;
	}
	fclose(file);
	return data;
}

/* a program's file: its sources and binary, written whole under another
name, then renamed over the old one */
static BOOL program_file_write(unsigned long long key, unsigned int format, const void *binary, unsigned int size,
	const char *vertex_source, const char *fragment_source)
{
	struct program_file_header header;
	char path[600], temporary[640];
	size_t vertex_size = strlen(vertex_source), fragment_size = strlen(fragment_source);
	unsigned char *data;
	FILE *file;
	BOOL ok;

	if (cache.bytes > CACHE_MAXIMUM_BYTES || cache.files > CACHE_MAXIMUM_FILES)
	{
		if (!cache.full_logged)
			platform_log("shader cache: full (%lu programs, %llu MB); no more are kept", cache.files,
				cache.bytes >> 20);
		cache.full_logged = TRUE;
		return FALSE;
	}
	if (!size || size > MAXIMUM_BINARY_SIZE || vertex_size > MAXIMUM_SOURCE_SIZE || fragment_size > MAXIMUM_SOURCE_SIZE)
		return FALSE;
	data = malloc(vertex_size + fragment_size + size);
	if (!data)
		return FALSE;
	memcpy(data, vertex_source, vertex_size);
	memcpy(data + vertex_size, fragment_source, fragment_size);
	memcpy(data + vertex_size + fragment_size, binary, size);
	memset(&header, 0, sizeof(header));
	header.magic = PROGRAM_FILE_MAGIC;
	header.version = PROGRAM_FILE_VERSION;
	header.driver[0] = (unsigned int)cache.driver;
	header.driver[1] = (unsigned int)(cache.driver >> 32);
	header.key[0] = (unsigned int)key;
	header.key[1] = (unsigned int)(key >> 32);
	header.binary_format = format;
	header.binary_size = size;
	header.vertex_size = (unsigned int)vertex_size;
	header.fragment_size = (unsigned int)fragment_size;
	header.checksum = checksum(data, vertex_size + fragment_size + size);
	program_path(path, sizeof(path), key);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	ok = file && fwrite(&header, sizeof(header), 1, file) == 1 &&
		fwrite(data, 1, vertex_size + fragment_size + size, file) == vertex_size + fragment_size + size;
	if (file && fclose(file) != 0)
		ok = FALSE;
	ok = ok && rename(temporary, path) == 0;
	if (!ok)
		remove(temporary);
	else
	{
		cache.files++;
		cache.bytes += sizeof(header) + vertex_size + fragment_size + size;
	}
	free(data);
	return ok;
}

/* the same for a program linked on this thread */
static BOOL program_file_write_linked(unsigned long long key, GLuint program, const char *vertex_source,
	const char *fragment_source)
{
	GLint length = 0;
	GLsizei written = 0;
	GLenum format = 0;
	void *binary;
	BOOL ok;

	glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
	if (length <= 0 || (unsigned long)length > MAXIMUM_BINARY_SIZE || (binary = malloc((size_t)length)) == NULL)
		return FALSE;
	glGetProgramBinary(program, length, &written, &format, binary);
	ok = written > 0 && written <= length &&
		program_file_write(key, (unsigned int)format, binary, (unsigned int)written, vertex_source, fragment_source);
	free(binary);
	return ok;
}

/* a program of this binary, linked, or 0 */
static GLuint program_from_binary(unsigned int format, const void *binary, unsigned int size)
{
	GLuint program;
	GLint status = 0;

	/* (the format is not checked against GL_PROGRAM_BINARY_FORMATS: the
	emulator's GL lists ANGLE's and hands out one of its own, which it takes
	back; the driver checks the binary itself, and was the one that made it) */
	program = glCreateProgram();
	glProgramBinary(program, (GLenum)format, binary, (GLsizei)size);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		/* (refused: a driver update the hash missed; its error is no one
		else's) */
		GLenum error = glGetError();

		cache.refused++;
		if (cache.refused <= 3)
		{
			char log[512];

			log[0] = 0;
			glGetProgramInfoLog(program, sizeof(log), NULL, log);
			platform_log("shader cache: the driver refused a program's binary (format 0x%x, %u bytes, GL error 0x%x)%s%s",
				format, size, (unsigned)error, log[0] ? ": " : "", log);
		}
		glDeleteProgram(program);
		while (glGetError() != GL_NO_ERROR)
		{
		}
		return 0;
	}
	return program;
}

/* ---------- the lists */

static void list_path(char *path, size_t size, const char *name)
{
	snprintf(path, size, "%s/%s.txt", cache.folder, name);
}

static void list_write(const char *name, const struct key_set *set)
{
	char path[600], temporary[640];
	FILE *file;
	unsigned long index, written = 0;
	BOOL ok;

	list_path(path, sizeof(path), name);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "w");
	if (!file)
		return;
	ok = fprintf(file, "# shader programs drawn with (xgpu_program_cache.c)\n") > 0;
	for (index = 0; ok && index < set->capacity && written < MAXIMUM_LISTED_PROGRAMS; index++)
	{
		if (set->keys[index])
		{
			ok = fprintf(file, "%016llx\n", set->keys[index]) > 0;
			written++;
		}
	}
	if (fclose(file) != 0)
		ok = FALSE;
	if (!ok || rename(temporary, path) != 0)
		remove(temporary);
}

static void list_read(const char *name, struct key_set *set)
{
	char path[600], line[64];
	FILE *file;

	list_path(path, sizeof(path), name);
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file) && set->count < MAXIMUM_LISTED_PROGRAMS)
	{
		char *end = NULL;
		unsigned long long key;

		if (line[0] == '#')
			continue;
		key = strtoull(line, &end, 16);
		if (end && end - line == 16)
			key_set_add(set, key);
	}
	fclose(file);
}

/* the map's list and every map's, where they grew */
static void lists_write(void)
{
	char name[128];

	cache.list_written_us = xgpu_shader_work_now_us();
	if (cache.map[0] && cache.used_changed)
	{
		snprintf(name, sizeof(name), "maps/%s", cache.map);
		list_write(name, &cache.used);
	}
	if (cache.all_changed)
		list_write("all", &cache.all);
	cache.used_changed = cache.all_changed = FALSE;
}

/* ---------- jobs */

static struct program_job *job_find(unsigned long long key)
{
	struct program_job *job;

	for (job = cache.jobs[key % JOB_BUCKETS]; job; job = job->next)
	{
		if (job->key == key)
			return job;
	}
	return NULL;
}

static void waiting_remove(struct program_job *job)
{
	unsigned long index;

	for (index = 0; index < cache.waiting_count; index++)
	{
		if (cache.waiting[index] == job)
		{
			cache.waiting[index] = cache.waiting[--cache.waiting_count];
			return;
		}
	}
}

static struct program_job *job_add(unsigned long long key)
{
	struct program_job *job = calloc(1, sizeof(*job));

	if (!job)
		return NULL;
	job->key = key;
	job->next = cache.jobs[key % JOB_BUCKETS];
	cache.jobs[key % JOB_BUCKETS] = job;
	return job;
}

/* taken by the renderer or given up: forgotten (the program stays, the
renderer's) */
static void job_remove(struct program_job *job)
{
	struct program_job **link;

	for (link = &cache.jobs[job->key % JOB_BUCKETS]; *link; link = &(*link)->next)
	{
		if (*link == job)
		{
			*link = job->next;
			break;
		}
	}
	if (job->state == JOB_WAITING)
		waiting_remove(job);
	if (job->id)
		host_gl_program_release(job->id);
	free(job->sources[0]);
	free(job->sources[1]);
	free(job);
}

static char *text_copy(const void *data, size_t size)
{
	char *text = malloc(size + 1);

	if (text)
	{
		memcpy(text, data, size);
		text[size] = 0;
	}
	return text;
}

/* the worker asked for the program of this key: its file's binary where
the driver is the one that made it, and the sources (given, or the file's:
a map's list names only the key) to compile where it is refused or there
is none. NULL: no sources to make it from */
static struct program_job *job_request(unsigned long long key, const char *vertex_source,
	const char *fragment_source, BOOL urgent)
{
	struct program_file_header header;
	unsigned char *data = program_file_read(key, &header);
	const unsigned char *binary = NULL;
	struct program_job *job;
	char *sources[2] = { NULL, NULL };

	if (data)
	{
		const char *file_vertex = (const char *)data;
		const char *file_fragment = (const char *)data + header.vertex_size;
		unsigned long long driver = (unsigned long long)header.driver[0] | ((unsigned long long)header.driver[1] << 32);
		/* (the sources compared too: the key is only their hash) */
		BOOL same = !vertex_source || (header.vertex_size == strlen(vertex_source) &&
			header.fragment_size == strlen(fragment_source) &&
			memcmp(file_vertex, vertex_source, header.vertex_size) == 0 &&
			memcmp(file_fragment, fragment_source, header.fragment_size) == 0);

		if (same && !vertex_source)
		{
			sources[0] = text_copy(file_vertex, header.vertex_size);
			sources[1] = text_copy(file_fragment, header.fragment_size);
		}
		if (same && driver == cache.driver)
			binary = data + header.vertex_size + header.fragment_size;
	}
	if (vertex_source)
	{
		sources[0] = text_copy(vertex_source, strlen(vertex_source));
		sources[1] = text_copy(fragment_source, strlen(fragment_source));
	}
	if (!sources[0] || !sources[1] || (job = job_add(key)) == NULL)
	{
		free(sources[0]);
		free(sources[1]);
		free(data);
		return NULL;
	}
	job->sources[0] = sources[0];
	job->sources[1] = sources[1];
	job->urgent = job->hurried = urgent;
	job->multiview = strstr(sources[0], "GL_OVR_multiview") != NULL;
	job->state = JOB_WAITING;
	job->id = host_gl_program_request(urgent, sources[0], sources[1], binary ? header.binary_format : 0, binary,
		binary ? header.binary_size : 0, TRUE);
	free(data);
	if (!job->id)
	{
		job_remove(job);
		return NULL;
	}
	if (cache.waiting_count == cache.waiting_capacity)
	{
		unsigned long capacity = cache.waiting_capacity ? cache.waiting_capacity * 2 : 256;
		struct program_job **waiting = realloc(cache.waiting, capacity * sizeof(*waiting));

		if (!waiting)
		{
			host_gl_program_release(job->id);
			job->id = 0;
			job_remove(job);
			return NULL;
		}
		cache.waiting = waiting;
		cache.waiting_capacity = capacity;
	}
	cache.waiting[cache.waiting_count++] = job;
	cache.requested++;
	return job;
}

/* whether the worker is done with it (and, for a program it compiled, its
binary written to the file: *written) */
static void job_poll(struct program_job *job, BOOL *written)
{
	unsigned int program = 0, binary_size = 0, microseconds = 0;
	int result;

	if (job->state != JOB_WAITING)
		return;
	result = host_gl_program_poll(job->id, &program, &binary_size, &microseconds);
	if (!result)
		return;
	waiting_remove(job);
	if (result < 0)
	{
		job->state = JOB_FAILED;
		return;
	}
	job->state = JOB_READY;
	job->program = program;
	if (binary_size)
	{
		/* (compiled: the driver refused the file's binary, or there was
		none) */
		void *binary = malloc(binary_size);
		unsigned int format = 0;

		cache.compiled++;
		if (binary && host_gl_program_binary(job->id, binary, binary_size, &format) == binary_size &&
			program_file_write(job->key, format, binary, binary_size, job->sources[0], job->sources[1]) && written)
		{
			*written = TRUE;
		}
		free(binary);
	}
	free(job->sources[0]);
	free(job->sources[1]);
	job->sources[0] = job->sources[1] = NULL;
	host_gl_program_release(job->id);
	job->id = 0;
}

/* ---------- without the worker */

/* the program of the file's binary, made here, or 0 */
static GLuint program_from_file(unsigned long long key, const char *vertex_source, const char *fragment_source)
{
	struct program_file_header header;
	unsigned char *data = program_file_read(key, &header);
	GLuint program = 0;

	if (data)
	{
		unsigned long long driver = (unsigned long long)header.driver[0] | ((unsigned long long)header.driver[1] << 32);
		unsigned long long start = xgpu_shader_work_now_us();

		if (driver == cache.driver && header.vertex_size == strlen(vertex_source) &&
			header.fragment_size == strlen(fragment_source) &&
			memcmp(data, vertex_source, header.vertex_size) == 0 &&
			memcmp(data + header.vertex_size, fragment_source, header.fragment_size) == 0)
		{
			program = program_from_binary(header.binary_format, data + header.vertex_size + header.fragment_size,
				header.binary_size);
			if (program)
			{
				xgpu_shader_work_add(TRUE, start);
				cache.found_file++;
			}
		}
		free(data);
	}
	return program;
}

static GLuint shader_from_source(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);
	GLint status = 0;

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	glGetShaderiv(shader, GL_COMPILE_STATUS, &status);
	if (!status)
	{
		glDeleteShader(shader);
		return 0;
	}
	return shader;
}

/* a listed program made here as its map loads: from the file's binary, or
compiled from its sources while compile_until_us has not passed */
static BOOL job_make_here(unsigned long long key, unsigned long long compile_until_us)
{
	struct program_file_header header;
	unsigned char *data = program_file_read(key, &header);
	unsigned long long driver, start = xgpu_shader_work_now_us();
	struct program_job *job;
	GLuint program = 0;

	if (!data)
		return FALSE;
	driver = (unsigned long long)header.driver[0] | ((unsigned long long)header.driver[1] << 32);
	if (driver == cache.driver)
		program = program_from_binary(header.binary_format, data + header.vertex_size + header.fragment_size,
			header.binary_size);
	if (program)
		xgpu_shader_work_add(TRUE, start);
	else if (xgpu_shader_work_now_us() < compile_until_us)
	{
		char *vertex = text_copy(data, header.vertex_size);
		char *fragment = text_copy(data + header.vertex_size, header.fragment_size);
		GLuint vertex_shader = vertex ? shader_from_source(GL_VERTEX_SHADER, vertex) : 0;
		GLuint fragment_shader = vertex_shader && fragment ? shader_from_source(GL_FRAGMENT_SHADER, fragment) : 0;

		if (fragment_shader)
			program = xgpu_link_program(vertex_shader, fragment_shader, "cached");
		if (vertex_shader)
			glDeleteShader(vertex_shader);
		if (fragment_shader)
			glDeleteShader(fragment_shader);
		if (program)
			program_file_write_linked(key, program, vertex, fragment);
		free(vertex);
		free(fragment);
	}
	free(data);
	if (!program || (job = job_add(key)) == NULL)
	{
		if (program)
			glDeleteProgram(program);
		return FALSE;
	}
	job->state = JOB_READY;
	job->program = program;
	return TRUE;
}

/* ---------- a map */

static void map_name_set(const char *map_name)
{
	size_t index, length = 0;

	/* (a file name: "levels\test\bloodgulch\bloodgulch" and the like,
	custom_maps' too) */
	for (index = 0; map_name && map_name[index] && length < sizeof(cache.map) - 1; index++)
	{
		char c = map_name[index];

		cache.map[length++] = (char)((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
			c == '-' ? c : '_');
	}
	cache.map[length] = 0;
}

/* debug.shader_warm_up "load": the map's programs waited for (a while), and
each drawn with once on this thread, the game's, before its first frame */
static void load_warm_up(void)
{
	unsigned long long start = xgpu_shader_work_now_us();
	unsigned long bucket, warmed = 0;

	while (cache.waiting_count && xgpu_shader_work_now_us() - start < LOAD_WAIT_MICROSECONDS)
	{
		unsigned long index;

		for (index = cache.waiting_count; index > 0; index--)
			job_poll(cache.waiting[index - 1], NULL);
		if (cache.waiting_count)
		{
			struct timespec pause = { 0, 2000000 };

			nanosleep(&pause, NULL);
		}
	}
	for (bucket = 0; bucket < JOB_BUCKETS; bucket++)
	{
		struct program_job *job;

		for (job = cache.jobs[bucket]; job; job = job->next)
		{
			if (job->state != JOB_READY || xgpu_shader_work_now_us() - start > LOAD_WAIT_MICROSECONDS +
				LOAD_WARM_UP_MICROSECONDS)
			{
				continue;
			}
			host_gl_program_warm_up(job->program, job->multiview);
			warmed++;
		}
	}
	d3d8_host_state_changed();
	platform_log("shader cache: %lu programs drawn with once as the map loaded, in %.1f ms (%lu still compiling)",
		warmed, (xgpu_shader_work_now_us() - start) * 1e-3f, cache.waiting_count);
}

void halo_shader_programs_map_loaded(char const *map_name)
{
	unsigned long long start, compile_until;
	unsigned long index, listed = 0, asked = 0, made = 0, missing = 0;
	char name[128];

	if (!cache.enabled)
		return;
	lists_write();
	xgpu_program_map_serial++;
	key_set_clear(&cache.used);
	map_name_set(map_name);
	if (!cache.map[0])
		return;
	snprintf(name, sizeof(name), "maps/%s", cache.map);
	list_read(name, &cache.used);
	cache.used_changed = FALSE;
	cache.requested = cache.found_ready = cache.compiled = cache.found_file = cache.stored = 0;
	start = xgpu_shader_work_now_us();
	compile_until = start + PREWARM_COMPILE_MICROSECONDS;
	/* the map's own now; with the worker, every map's after them, a few a
	frame (xgpu_program_cache_frame) */
	cache.request_count = cache.request_next = 0;
	for (index = 0; index < cache.used.capacity; index++)
	{
		unsigned long long key = cache.used.keys[index];

		if (!key)
			continue;
		listed++;
		if (key_set_has(&cache.linked, key) || job_find(key))
			continue;
		if (cache.worker ? job_request(key, NULL, NULL, FALSE) != NULL : job_make_here(key, compile_until))
			cache.worker ? asked++ : made++;
		else
			missing++;
	}
	if (cache.worker)
	{
		for (index = 0; index < cache.all.capacity; index++)
		{
			unsigned long long key = cache.all.keys[index];

			if (!key || key_set_has(&cache.used, key) || key_set_has(&cache.linked, key) || job_find(key))
				continue;
			if (cache.request_count == cache.request_capacity)
			{
				unsigned long capacity = cache.request_capacity ? cache.request_capacity * 2 : 512;
				unsigned long long *requests = realloc(cache.requests, capacity * sizeof(*requests));

				if (!requests)
					break;
				cache.requests = requests;
				cache.request_capacity = capacity;
			}
			cache.requests[cache.request_count++] = key;
		}
	}
	platform_log("shader cache: %s: %lu programs listed; %lu asked of the worker, %lu made here, %lu not (no file, "
		"or out of time), %lu there already; %lu of other maps to ask for; in %.1f ms", cache.map, listed, asked, made,
		missing, listed - asked - made - missing, cache.request_count, (xgpu_shader_work_now_us() - start) * 1e-3f);
	if (cache.worker && cache.warm_up == 2)
		load_warm_up();
	xgpu_gl_state_invalidate();
}

void halo_shader_programs_map_disposed(void)
{
	if (!cache.enabled || !cache.map[0])
		return;
	platform_log("shader cache: %s: %lu programs asked of the worker; %lu taken made (%lu of them compiled there); "
		"%lu read from the cache here, %lu new ones kept", cache.map, cache.requested, cache.found_ready,
		cache.compiled, cache.found_file, cache.stored);
	lists_write();
}

/* ---------- the renderer's */

BOOL xgpu_program_cache_enabled(void)
{
	return cache.enabled;
}

int xgpu_program_cache_take(unsigned long long key, const char *vertex_source, const char *fragment_source,
	GLuint *program)
{
	struct program_job *job;

	*program = 0;
	if (!cache.enabled)
		return XGPU_PROGRAM_NONE;
	job = job_find(key);
	if (job)
	{
		job_poll(job, NULL);
		if (job->state == JOB_READY)
		{
			*program = job->program;
			job_remove(job);
			key_set_add(&cache.linked, key);
			cache.found_ready++;
			/* (the long frames' "programs from the cache") */
			xgpu_shader_work_add(TRUE, xgpu_shader_work_now_us());
			return XGPU_PROGRAM_READY;
		}
		if (job->state == JOB_WAITING)
		{
			/* (a map's program a draw needs now: before the others) */
			if (!job->hurried)
			{
				host_gl_program_hurry(job->id);
				job->hurried = TRUE;
			}
			return XGPU_PROGRAM_WAITING;
		}
		/* (the worker could not make it: here, to log why) */
		job_remove(job);
		return XGPU_PROGRAM_NONE;
	}
	if (cache.worker)
		return job_request(key, vertex_source, fragment_source, TRUE) ? XGPU_PROGRAM_WAITING : XGPU_PROGRAM_NONE;
	*program = program_from_file(key, vertex_source, fragment_source);
	if (*program)
	{
		key_set_add(&cache.linked, key);
		return XGPU_PROGRAM_READY;
	}
	return XGPU_PROGRAM_NONE;
}

void xgpu_program_cache_store(unsigned long long key, GLuint program, const char *vertex_source,
	const char *fragment_source)
{
	if (!cache.enabled)
		return;
	key_set_add(&cache.linked, key);
	if (program_file_write_linked(key, program, vertex_source, fragment_source))
		cache.stored++;
}

void xgpu_program_cache_used(unsigned long long key)
{
	if (!cache.enabled || !cache.map[0])
		return;
	if (cache.used.count < MAXIMUM_LISTED_PROGRAMS && key_set_add(&cache.used, key))
		cache.used_changed = TRUE;
	if (cache.all.count < MAXIMUM_LISTED_PROGRAMS && key_set_add(&cache.all, key))
		cache.all_changed = TRUE;
}

void xgpu_program_cache_frame(void)
{
	if (!cache.enabled)
		return;
	if (cache.worker)
	{
		BOOL written = FALSE;
		int writes = 0, polls;

		/* every map's programs asked for, a few a frame (a file read each) */
		for (polls = 0; polls < REQUESTS_A_FRAME && cache.request_next < cache.request_count; cache.request_next++)
		{
			unsigned long long key = cache.requests[cache.request_next];

			if (key_set_has(&cache.linked, key) || job_find(key))
				continue;
			job_request(key, NULL, NULL, FALSE);
			polls++;
		}
		/* those the worker finished: the binaries of those it compiled
		written, a few a frame */
		for (polls = 0; polls < POLLS_A_FRAME && cache.waiting_count && writes < WRITES_A_FRAME; polls++)
		{
			struct program_job *job;

			if (cache.waiting_next >= cache.waiting_count)
				cache.waiting_next = 0;
			job = cache.waiting[cache.waiting_next++];
			written = FALSE;
			job_poll(job, &written);
			writes += written;
		}
	}
	if ((cache.used_changed || cache.all_changed) &&
		xgpu_shader_work_now_us() - cache.list_written_us > LIST_WRITE_MICROSECONDS)
	{
		lists_write();
	}
}

/* ---------- start */

/* the folder's programs counted, and all deleted past three quarters of
the limits (the sources change with the app's versions, and old programs'
files are never asked for again); files left half written deleted */
static void folder_survey(BOOL empty)
{
	void *directory = posix_directory_open(cache.folder);
	char name[256], path[800];

	cache.files = 0;
	cache.bytes = 0;
	if (!directory)
		return;
	while (posix_directory_next(directory, name, sizeof(name)))
	{
		size_t length = strlen(name);
		struct posix_file_information information;

		if (length < 4 || (strcmp(name + length - 4, ".bin") != 0 && strcmp(name + length - 4, ".new") != 0))
			continue;
		snprintf(path, sizeof(path), "%s/%s", cache.folder, name);
		if (empty || strcmp(name + length - 4, ".new") == 0)
			remove(path);
		else if (posix_stat(path, &information) == 0)
		{
			cache.files++;
			cache.bytes += information.size_low | ((unsigned long long)information.size_high << 32);
		}
	}
	posix_directory_close(directory);
	if (!empty && (cache.bytes > CACHE_MAXIMUM_BYTES * 3 / 4 || cache.files > CACHE_MAXIMUM_FILES * 3 / 4))
	{
		platform_log("shader cache: %lu programs, %llu MB: emptied", cache.files, cache.bytes >> 20);
		folder_survey(TRUE);
	}
}

void xgpu_program_cache_initialize(void)
{
	const char *root = getenv("HALO_DATA_ROOT");
	const char *renderer = (const char *)glGetString(GL_RENDERER);
	const char *version = (const char *)glGetString(GL_VERSION);
	char path[600];
	GLint formats = xgpu_capabilities.program_binary_formats;

	cache.enabled = FALSE;
	if (!config_boolean("display.shader_cache"))
	{
		platform_log("shader cache: off (display.shader_cache)");
		return;
	}
	if (formats <= 0 || !root || !*root)
	{
		platform_log("shader cache: off (%s)", formats <= 0 ? "the driver has no program binary formats" :
			"no data folder");
		return;
	}
	if (formats > MAXIMUM_BINARY_FORMATS)
		formats = MAXIMUM_BINARY_FORMATS;
	{
		GLint all[64];

		memset(all, 0, sizeof(all));
		glGetIntegerv(GL_PROGRAM_BINARY_FORMATS, all);
		memcpy(cache.binary_formats, all, (size_t)formats * sizeof(GLint));
		cache.binary_format_count = (int)formats;
	}
	/* the driver: its name and its build */
	cache.driver = hash_bytes(0xcbf29ce484222325ull, renderer ? renderer : "", renderer ? strlen(renderer) : 0);
	cache.driver = hash_bytes(cache.driver, "\n", 1);
	cache.driver = hash_bytes(cache.driver, version ? version : "", version ? strlen(version) : 0);
	snprintf(cache.folder, sizeof(cache.folder), "%s/shader_cache", root);
	posix_make_directory(cache.folder);
	snprintf(path, sizeof(path), "%s/maps", cache.folder);
	posix_make_directory(path);
	{
		struct posix_file_information information;

		if (posix_stat(path, &information) != 0 || !(information.flags & _posix_file_is_directory))
		{
			platform_log("shader cache: off (%s cannot be made)", path);
			return;
		}
	}
	folder_survey(FALSE);
	list_read("all", &cache.all);
	cache.enabled = TRUE;
	{
		/* the worker (display.shader_worker), and its warm-up draws
		(debug.shader_warm_up: "worker", "load" on this thread as a map
		loads, or "off") */
		const char *warm_up = config_string("debug.shader_warm_up");

		cache.warm_up = !strcmp(warm_up, "off") ? 0 : !strcmp(warm_up, "load") ? 2 : 1;
		cache.worker = config_boolean("display.shader_worker") && host_gl_worker_start(cache.warm_up == 1);
	}
	platform_log("shader cache: %s, %lu programs kept (%llu MB, %lu listed), driver %016llx, binary format 0x%x%s; "
		"programs made %s%s", cache.folder, cache.files, cache.bytes >> 20, cache.all.count, cache.driver,
		(unsigned)cache.binary_formats[0], cache.binary_format_count > 1 ? " and others" : "",
		cache.worker ? "on the worker" : "on the game's thread",
		!cache.worker ? "" : cache.warm_up == 1 ? ", warmed up there" : cache.warm_up == 2 ?
		", warmed up as a map loads" : "");
}

#else

/* (the desktop builds' drivers keep their own caches) */
void halo_shader_programs_map_loaded(char const *map_name)
{
	(void)map_name;
}

void halo_shader_programs_map_disposed(void)
{
}

#endif
