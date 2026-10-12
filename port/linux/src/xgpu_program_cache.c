/*
XGPU_PROGRAM_CACHE.C

The shader programs' binary cache, and their making ready as a map loads
(the Android app). Its drivers compile a program the first time it is drawn
with, in 10 to 30 ms on the Quest's, which the frame waits for; in VR such a
frame is shown late, the picture doubled as the head turns, and a map's
first seconds and its end (the scores' screen) compiled dozens at once.

- A program linked (d3d8_gl.c program_make) is written to shader_cache in
  the data folder: its binary (glGetProgramBinary), the driver that made it
  (a hash of GL_RENDERER and GL_VERSION) and its two shaders' sources, in a
  file named by the sources' hash (xgpu_program_key).
- Asked for again, in this run or a later one, the file's binary is loaded
  (glProgramBinary) instead: when the driver is the one that made it, the
  file is whole (its sizes and checksum) and its sources are the ones asked
  for. Anything else, and the program is compiled and linked as before, its
  file written again.
- Each map keeps the list of the programs it drew with (shader_cache/maps/
  <map>.txt, written as it goes and as it ends). As the map loads
  (game_initialize_for_new_map, before its first frame) the programs listed
  are made ready: loaded from their binaries, or (after a driver update)
  compiled again from the sources in their files, in the background where
  the driver has GL_KHR_parallel_shader_compile, else up to a time limit.

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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
#define READY_BUCKETS 1024
/* the map's list is written this often while it has new programs */
#define LIST_WRITE_MICROSECONDS 20000000ull
/* without background compiling, how long a map's load may compile */
#define PREWARM_COMPILE_MICROSECONDS 2000000ull
/* programs compiled in the background whose binaries a frame writes */
#define WRITES_A_FRAME 2

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

/* a program made ready as its map loaded, until it is asked for */
struct ready_program
{
	struct ready_program *next;
	unsigned long long key;
	GLuint program;
	/* compiled from its file's sources (another driver): its shaders, and
	the sources for its file once linked */
	GLuint shaders[2];
	char *sources[2];
	/* still linking in the background (GL_KHR_parallel_shader_compile) */
	BOOL compiling;
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

	struct ready_program *ready[READY_BUCKETS];
	unsigned long compiling;
	/* the programs the renderer has (found or stored here) */
	struct key_set linked;

	/* the map loaded (a file name), the programs it drew with or listed */
	char map[96];
	struct key_set used;
	BOOL used_changed;
	unsigned long long list_written_us;

	/* for the log: since the map loaded */
	unsigned long found_ready, found_file, stored;
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

static void list_path(char *path, size_t size, const char *map)
{
	snprintf(path, size, "%s/maps/%s.txt", cache.folder, map);
}

static BOOL binary_format_known(unsigned int format)
{
	int index;

	for (index = 0; index < cache.binary_format_count; index++)
	{
		if ((unsigned int)cache.binary_formats[index] == format)
			return TRUE;
	}
	return FALSE;
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

/* written whole under another name, then renamed over the old one */
static BOOL program_file_write(unsigned long long key, GLuint program, const char *vertex_source,
	const char *fragment_source)
{
	struct program_file_header header;
	char path[600], temporary[640];
	GLint length = 0;
	GLsizei written = 0;
	GLenum format = 0;
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
	glGetProgramiv(program, GL_PROGRAM_BINARY_LENGTH, &length);
	if (length <= 0 || (unsigned long)length > MAXIMUM_BINARY_SIZE || vertex_size > MAXIMUM_SOURCE_SIZE ||
		fragment_size > MAXIMUM_SOURCE_SIZE)
	{
		return FALSE;
	}
	data = malloc(vertex_size + fragment_size + (size_t)length);
	if (!data)
		return FALSE;
	memcpy(data, vertex_source, vertex_size);
	memcpy(data + vertex_size, fragment_source, fragment_size);
	glGetProgramBinary(program, length, &written, &format, data + vertex_size + fragment_size);
	if (written <= 0 || written > length)
	{
		free(data);
		return FALSE;
	}
	memset(&header, 0, sizeof(header));
	header.magic = PROGRAM_FILE_MAGIC;
	header.version = PROGRAM_FILE_VERSION;
	header.driver[0] = (unsigned int)cache.driver;
	header.driver[1] = (unsigned int)(cache.driver >> 32);
	header.key[0] = (unsigned int)key;
	header.key[1] = (unsigned int)(key >> 32);
	header.binary_format = (unsigned int)format;
	header.binary_size = (unsigned int)written;
	header.vertex_size = (unsigned int)vertex_size;
	header.fragment_size = (unsigned int)fragment_size;
	header.checksum = checksum(data, vertex_size + fragment_size + (size_t)written);
	program_path(path, sizeof(path), key);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "wb");
	ok = file && fwrite(&header, sizeof(header), 1, file) == 1 &&
		fwrite(data, 1, vertex_size + fragment_size + (size_t)written, file) == vertex_size + fragment_size + (size_t)written;
	if (file && fclose(file) != 0)
		ok = FALSE;
	ok = ok && rename(temporary, path) == 0;
	if (!ok)
		remove(temporary);
	else
	{
		cache.files++;
		cache.bytes += sizeof(header) + vertex_size + fragment_size + (size_t)written;
	}
	free(data);
	return ok;
}

/* a program of this binary, linked, or 0 */
static GLuint program_from_binary(unsigned int format, const void *binary, unsigned int size)
{
	GLuint program;
	GLint status = 0;

	if (!binary_format_known(format))
		return 0;
	program = glCreateProgram();
	glProgramBinary(program, (GLenum)format, binary, (GLsizei)size);
	glGetProgramiv(program, GL_LINK_STATUS, &status);
	if (!status)
	{
		/* (refused: a driver update the hash missed; its error is no one
		else's) */
		glDeleteProgram(program);
		while (glGetError() != GL_NO_ERROR)
		{
		}
		return 0;
	}
	return program;
}

/* ---------- the map's list */

static void list_write(void)
{
	char path[600], temporary[640];
	FILE *file;
	unsigned long index, written = 0;
	BOOL ok;

	if (!cache.map[0] || !cache.used_changed)
		return;
	cache.used_changed = FALSE;
	cache.list_written_us = xgpu_shader_work_now_us();
	list_path(path, sizeof(path), cache.map);
	snprintf(temporary, sizeof(temporary), "%s.new", path);
	file = fopen(temporary, "w");
	if (!file)
		return;
	ok = fprintf(file, "# shader programs %s drew with (xgpu_program_cache.c)\n", cache.map) > 0;
	for (index = 0; ok && index < cache.used.capacity && written < MAXIMUM_LISTED_PROGRAMS; index++)
	{
		if (cache.used.keys[index])
		{
			ok = fprintf(file, "%016llx\n", cache.used.keys[index]) > 0;
			written++;
		}
	}
	if (fclose(file) != 0)
		ok = FALSE;
	if (!ok || rename(temporary, path) != 0)
		remove(temporary);
}

static void list_read(void)
{
	char path[600], line[64];
	FILE *file;

	list_path(path, sizeof(path), cache.map);
	file = fopen(path, "r");
	if (!file)
		return;
	while (fgets(line, sizeof(line), file) && cache.used.count < MAXIMUM_LISTED_PROGRAMS)
	{
		char *end = NULL;
		unsigned long long key;

		if (line[0] == '#')
			continue;
		key = strtoull(line, &end, 16);
		if (end && end - line == 16)
			key_set_add(&cache.used, key);
	}
	fclose(file);
}

/* ---------- making a map's programs ready */

static void ready_add(struct ready_program *ready)
{
	struct ready_program **bucket = &cache.ready[ready->key % READY_BUCKETS];

	ready->next = *bucket;
	*bucket = ready;
	if (ready->compiling)
		cache.compiling++;
}

static struct ready_program *ready_take(unsigned long long key)
{
	struct ready_program **link;

	for (link = &cache.ready[key % READY_BUCKETS]; *link; link = &(*link)->next)
	{
		struct ready_program *ready = *link;

		if (ready->key == key)
		{
			*link = ready->next;
			if (ready->compiling)
				cache.compiling--;
			return ready;
		}
	}
	return NULL;
}

static BOOL ready_has(unsigned long long key)
{
	struct ready_program *ready;

	for (ready = cache.ready[key % READY_BUCKETS]; ready; ready = ready->next)
	{
		if (ready->key == key)
			return TRUE;
	}
	return FALSE;
}

static void ready_free(struct ready_program *ready, BOOL delete_program)
{
	if (ready->shaders[0])
		glDeleteShader(ready->shaders[0]);
	if (ready->shaders[1])
		glDeleteShader(ready->shaders[1]);
	if (delete_program && ready->program)
		glDeleteProgram(ready->program);
	free(ready->sources[0]);
	free(ready->sources[1]);
	free(ready);
}

/* a shader of a file's source, compiling (not waited for) */
static GLuint shader_from_source(GLenum type, const char *source)
{
	GLuint shader = glCreateShader(type);

	glShaderSource(shader, 1, &source, NULL);
	glCompileShader(shader);
	return shader;
}

static char *text_copy(const unsigned char *data, unsigned int size)
{
	char *text = malloc((size_t)size + 1);

	if (text)
	{
		memcpy(text, data, size);
		text[size] = 0;
	}
	return text;
}

/* ready: loaded from the file's binary, or compiled from its sources (in
the background, or waited for while compile_until_us has not passed);
returns 1 loaded, 2 compiling, 3 compiled, 0 not */
static int program_prepare(unsigned long long key, unsigned long long compile_until_us)
{
	struct program_file_header header;
	unsigned char *data = program_file_read(key, &header);
	struct ready_program *ready;
	unsigned long long driver;

	if (!data)
		return 0;
	ready = calloc(1, sizeof(*ready));
	if (!ready)
	{
		free(data);
		return 0;
	}
	ready->key = key;
	driver = (unsigned long long)header.driver[0] | ((unsigned long long)header.driver[1] << 32);
	if (driver == cache.driver)
	{
		unsigned long long start = xgpu_shader_work_now_us();

		ready->program = program_from_binary(header.binary_format,
			data + header.vertex_size + header.fragment_size, header.binary_size);
		if (ready->program)
		{
			xgpu_shader_work_add(TRUE, start);
			free(data);
			ready_add(ready);
			return 1;
		}
	}
	if (!xgpu_capabilities.parallel_compile && xgpu_shader_work_now_us() > compile_until_us)
	{
		free(ready);
		free(data);
		return 0;
	}
	/* another driver: compiled again from the sources the file keeps */
	ready->sources[0] = text_copy(data, header.vertex_size);
	ready->sources[1] = text_copy(data + header.vertex_size, header.fragment_size);
	free(data);
	if (!ready->sources[0] || !ready->sources[1])
	{
		ready_free(ready, FALSE);
		return 0;
	}
	{
		unsigned long long start = xgpu_shader_work_now_us();

		ready->shaders[0] = shader_from_source(GL_VERTEX_SHADER, ready->sources[0]);
		ready->shaders[1] = shader_from_source(GL_FRAGMENT_SHADER, ready->sources[1]);
		ready->program = glCreateProgram();
		glProgramParameteri(ready->program, GL_PROGRAM_BINARY_RETRIEVABLE_HINT, GL_TRUE);
		glAttachShader(ready->program, ready->shaders[0]);
		glAttachShader(ready->program, ready->shaders[1]);
		glLinkProgram(ready->program);
		if (xgpu_capabilities.parallel_compile)
		{
			ready->compiling = TRUE;
			ready_add(ready);
			return 2;
		}
		{
			GLint status = 0;

			glGetProgramiv(ready->program, GL_LINK_STATUS, &status);
			xgpu_shader_work_add(FALSE, start);
			if (!status)
			{
				ready_free(ready, TRUE);
				return 0;
			}
		}
	}
	program_file_write(key, ready->program, ready->sources[0], ready->sources[1]);
	ready_add(ready);
	return 3;
}

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

void halo_shader_programs_map_loaded(char const *map_name)
{
	unsigned long long start, compile_until;
	unsigned long index, listed = 0, results[4] = { 0, 0, 0, 0 };

	if (!cache.enabled)
		return;
	list_write();
	xgpu_program_map_serial++;
	key_set_clear(&cache.used);
	cache.used_changed = FALSE;
	map_name_set(map_name);
	if (!cache.map[0])
		return;
	list_read();
	cache.found_ready = cache.found_file = cache.stored = 0;
	start = xgpu_shader_work_now_us();
	compile_until = start + PREWARM_COMPILE_MICROSECONDS;
	for (index = 0; index < cache.used.capacity; index++)
	{
		unsigned long long key = cache.used.keys[index];

		if (!key)
			continue;
		listed++;
		if (key_set_has(&cache.linked, key) || ready_has(key))
			continue;
		results[program_prepare(key, compile_until)]++;
	}
	platform_log("shader cache: %s: %lu programs listed; %lu loaded, %lu compiling in the background, "
		"%lu compiled, %lu not ready (no file, or out of time), %lu there already, in %.1f ms", cache.map, listed,
		results[1], results[2], results[3], results[0], listed - results[0] - results[1] - results[2] - results[3],
		(xgpu_shader_work_now_us() - start) * 1e-3f);
	xgpu_gl_state_invalidate();
}

void halo_shader_programs_map_disposed(void)
{
	if (!cache.enabled || !cache.map[0])
		return;
	platform_log("shader cache: %s: %lu programs drawn with were made ready as it loaded, %lu read from the "
		"cache, %lu new ones kept", cache.map, cache.found_ready, cache.found_file, cache.stored);
	list_write();
}

/* ---------- the renderer's */

BOOL xgpu_program_cache_enabled(void)
{
	return cache.enabled;
}

GLuint xgpu_program_cache_find(unsigned long long key, const char *vertex_source, const char *fragment_source)
{
	struct ready_program *ready;
	struct program_file_header header;
	unsigned char *data;
	GLuint program = 0;

	if (!cache.enabled)
		return 0;
	ready = ready_take(key);
	if (ready)
	{
		GLint status = 0;
		unsigned long long start = xgpu_shader_work_now_us();

		/* (one still compiling is waited for: sooner than starting again) */
		glGetProgramiv(ready->program, GL_LINK_STATUS, &status);
		if (status)
		{
			if (ready->compiling)
				xgpu_shader_work_add(FALSE, start);
			if (ready->sources[0])
				program_file_write(key, ready->program, ready->sources[0], ready->sources[1]);
			program = ready->program;
			cache.found_ready++;
		}
		ready_free(ready, !status);
		if (program)
		{
			key_set_add(&cache.linked, key);
			return program;
		}
	}
	data = program_file_read(key, &header);
	if (data)
	{
		unsigned long long driver = (unsigned long long)header.driver[0] | ((unsigned long long)header.driver[1] << 32);
		unsigned long long start = xgpu_shader_work_now_us();

		/* (the sources compared too: the key is only their hash) */
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
				key_set_add(&cache.linked, key);
				cache.found_file++;
			}
		}
		free(data);
	}
	return program;
}

void xgpu_program_cache_store(unsigned long long key, GLuint program, const char *vertex_source,
	const char *fragment_source)
{
	if (!cache.enabled)
		return;
	key_set_add(&cache.linked, key);
	if (program_file_write(key, program, vertex_source, fragment_source))
		cache.stored++;
}

void xgpu_program_cache_used(unsigned long long key)
{
	if (cache.enabled && cache.map[0] && cache.used.count < MAXIMUM_LISTED_PROGRAMS && key_set_add(&cache.used, key))
		cache.used_changed = TRUE;
}

void xgpu_program_cache_frame(void)
{
	if (!cache.enabled)
		return;
	if (cache.compiling)
	{
		/* those compiled in the background since: their binaries kept, a
		few a frame */
		unsigned long bucket;
		int writes = 0;

		for (bucket = 0; bucket < READY_BUCKETS && writes < WRITES_A_FRAME; bucket++)
		{
			struct ready_program **link = &cache.ready[bucket];

			while (*link && writes < WRITES_A_FRAME)
			{
				struct ready_program *ready = *link;
				GLint done = 0, status = 0;

				if (!ready->compiling)
				{
					link = &ready->next;
					continue;
				}
				glGetProgramiv(ready->program, GL_COMPLETION_STATUS_KHR, &done);
				if (!done)
				{
					link = &ready->next;
					continue;
				}
				ready->compiling = FALSE;
				cache.compiling--;
				glGetProgramiv(ready->program, GL_LINK_STATUS, &status);
				if (!status)
				{
					*link = ready->next;
					ready_free(ready, TRUE);
					continue;
				}
				program_file_write(ready->key, ready->program, ready->sources[0], ready->sources[1]);
				free(ready->sources[0]);
				free(ready->sources[1]);
				ready->sources[0] = ready->sources[1] = NULL;
				writes++;
				link = &ready->next;
			}
		}
	}
	if (cache.used_changed && xgpu_shader_work_now_us() - cache.list_written_us > LIST_WRITE_MICROSECONDS)
		list_write();
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
	cache.enabled = TRUE;
	platform_log("shader cache: %s, %lu programs kept (%llu MB), driver %016llx%s", cache.folder, cache.files,
		cache.bytes >> 20, cache.driver,
		xgpu_capabilities.parallel_compile ? "; a map's programs compiled in the background" : "");
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
