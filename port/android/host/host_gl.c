/*
HOST_GL.C

OpenGL ES for the guest. Its generated entry points (guest_gl.c) import
hostgl_<function>, resolved here to the driver's function; the arguments
already have host types by then. Only strings need copying back.
*/

#include "host.h"

#include <EGL/egl.h>
#include <GLES3/gl32.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <string.h>

void *host_gl_resolve(const char *name)
{
	static void *library;
	void *function = NULL;

	if (!library)
		library = dlopen("libGLESv3.so", RTLD_NOW | RTLD_GLOBAL);
	if (library)
		function = dlsym(library, name);
	if (!function)
		function = (void *)eglGetProcAddress(name);
	return function;
}

void host_gl_get_string(uint32_t name, int index, char *buffer, uint32_t size)
{
	const GLubyte *text = index >= 0 ? glGetStringi(name, (GLuint)index) : glGetString(name);

	if (!size)
		return;
	buffer[0] = 0;
	if (text)
	{
		strncpy(buffer, (const char *)text, size - 1);
		buffer[size - 1] = 0;
	}
}

int host_gl_has_extension(const char *name)
{
	GLint count = 0, index;

	glGetIntegerv(GL_NUM_EXTENSIONS, &count);
	for (index = 0; index < count; index++)
	{
		const char *extension = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)index);

		if (extension && !strcmp(extension, name))
			return 1;
	}
	return 0;
}

/* one 32-bit word of a buffer object (the visibility test counters of
d3d8_gl.c); ES has no glGetBufferSubData, and the mapping it offers
instead is a host pointer */
uint32_t host_gl_read_buffer_word(uint32_t buffer, uint32_t offset)
{
	uint32_t value = 0;
	GLint previous = 0;
	const void *mapping;

	glGetIntegerv(GL_ATOMIC_COUNTER_BUFFER_BINDING, &previous);
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, buffer);
	mapping = glMapBufferRange(GL_ATOMIC_COUNTER_BUFFER, offset, sizeof(value), GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(&value, mapping, sizeof(value));
		glUnmapBuffer(GL_ATOMIC_COUNTER_BUFFER);
	}
	glBindBuffer(GL_ATOMIC_COUNTER_BUFFER, (GLuint)previous);
	return value;
}

/* The visibility tests' counters (d3d8_gl.c), copied at the end of each
frame into a ring of buffers that are read once the GPU has finished that
frame: reading the counters themselves (host_gl_read_buffer_word) waits for
the GPU in the middle of a frame, which on a tiler (Adreno) also writes the
half-drawn targets out of tile memory, for every lens flare. */
#define COUNTER_SNAPSHOTS 8

static GLuint counter_snapshots[COUNTER_SNAPSHOTS];
static uint32_t counter_snapshot_sizes[COUNTER_SNAPSHOTS];

void host_gl_counters_snapshot(uint32_t buffer, uint32_t size, uint32_t slot)
{
	GLint read_binding = 0, write_binding = 0;

	if (slot >= COUNTER_SNAPSHOTS || !buffer || !size)
		return;
	glGetIntegerv(GL_COPY_READ_BUFFER_BINDING, &read_binding);
	glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &write_binding);
	if (!counter_snapshots[slot])
		glGenBuffers(1, &counter_snapshots[slot]);
	glBindBuffer(GL_COPY_WRITE_BUFFER, counter_snapshots[slot]);
	if (counter_snapshot_sizes[slot] != size)
	{
		glBufferData(GL_COPY_WRITE_BUFFER, size, NULL, GL_STREAM_READ);
		counter_snapshot_sizes[slot] = size;
	}
	/* the counts come from shaders' atomic writes */
	glMemoryBarrier(GL_ATOMIC_COUNTER_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
	glBindBuffer(GL_COPY_READ_BUFFER, buffer);
	glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, 0, 0, size);
	glBindBuffer(GL_COPY_READ_BUFFER, (GLuint)read_binding);
	glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)write_binding);
}

/* copies a snapshot out; its frame must be finished (host_gl_wait_frame) */
int host_gl_counters_read(uint32_t slot, void *out, uint32_t size)
{
	GLint write_binding = 0;
	const void *mapping;

	if (slot >= COUNTER_SNAPSHOTS || !counter_snapshots[slot] || size > counter_snapshot_sizes[slot])
		return 0;
	glGetIntegerv(GL_COPY_WRITE_BUFFER_BINDING, &write_binding);
	glBindBuffer(GL_COPY_WRITE_BUFFER, counter_snapshots[slot]);
	mapping = glMapBufferRange(GL_COPY_WRITE_BUFFER, 0, size, GL_MAP_READ_BIT);
	if (mapping)
	{
		memcpy(out, mapping, size);
		glUnmapBuffer(GL_COPY_WRITE_BUFFER);
	}
	glBindBuffer(GL_COPY_WRITE_BUFFER, (GLuint)write_binding);
	return mapping != NULL;
}

/* The renderer streams each frame's vertices and indices into the next of
a ring of buffers (d3d8_gl.c). A fence marks the end of each frame's work,
and a buffer is written again only once the GPU has passed the fence of the
frame that last used it: drivers queue several frames, and a draw still
waiting to run would otherwise read a later frame's vertices. */
#define FRAME_FENCE_SLOTS 8

static GLsync frame_fences[FRAME_FENCE_SLOTS];

void host_gl_fence_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS)
		return;
	if (frame_fences[slot])
		glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

void host_gl_wait_frame(uint32_t slot)
{
	if (slot >= FRAME_FENCE_SLOTS || !frame_fences[slot])
		return;
	/* at most a second: a lost context must not hang the game */
	glClientWaitSync(frame_fences[slot], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull);
	glDeleteSync(frame_fences[slot]);
	frame_fences[slot] = NULL;
}

/* Buffers the renderer writes into for every draw (d3d8_gl.c: the stream
and index rings, the visibility counters), mapped once for good where the
driver has GL_EXT_buffer_storage. Adreno (Quest) waits for the GPU in every
glMapBufferRange, unsynchronized or not, and the game streams several
hundred small uploads a frame; into a persistent mapping each is a memcpy.
The renderer's frame fences keep it from writing what the GPU still reads,
as with the unsynchronized mappings. */
#define PERSISTENT_BUFFERS 48 /* the rings, the counters and the mirror's 32 segments */

static struct persistent_buffer
{
	GLuint name;
	uint32_t size;
	uint8_t *mapping;
} persistent_buffers[PERSISTENT_BUFFERS];
static int persistent_buffer_count;

static GLenum binding_of(uint32_t target)
{
	switch (target)
	{
	case GL_ARRAY_BUFFER: return GL_ARRAY_BUFFER_BINDING;
	case GL_ELEMENT_ARRAY_BUFFER: return GL_ELEMENT_ARRAY_BUFFER_BINDING;
	case GL_ATOMIC_COUNTER_BUFFER: return GL_ATOMIC_COUNTER_BUFFER_BINDING;
	case GL_COPY_WRITE_BUFFER: return GL_COPY_WRITE_BUFFER_BINDING;
	default: return 0;
	}
}

/* gives the buffer bound to target persistent, coherent storage of size
bytes, instead of glBufferData; 0 if the driver cannot */
int host_gl_buffer_storage(uint32_t target, uint32_t size)
{
	static int checked;
	static PFNGLBUFFERSTORAGEEXTPROC buffer_storage;
	const GLbitfield flags = GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT_EXT | GL_MAP_COHERENT_BIT_EXT;
	GLint name = 0;
	void *mapping;

	if (!checked)
	{
		checked = 1;
		if (host_gl_has_extension("GL_EXT_buffer_storage"))
			buffer_storage = (PFNGLBUFFERSTORAGEEXTPROC)eglGetProcAddress("glBufferStorageEXT");
		host_logf(HOST_LOG_INFO, "persistent buffers: %s", buffer_storage ? "yes" : "no");
	}
	if (!buffer_storage || !binding_of(target) || persistent_buffer_count == PERSISTENT_BUFFERS)
		return 0;
	glGetIntegerv(binding_of(target), &name);
	if (!name)
		return 0;
	/* dynamic storage keeps glBufferSubData working on it too */
	buffer_storage(target, size, NULL, flags | GL_DYNAMIC_STORAGE_BIT_EXT);
	mapping = glMapBufferRange(target, 0, size, flags);
	if (!mapping)
	{
		host_logf(HOST_LOG_WARN, "persistent buffer %d cannot be mapped", name);
		return 0;
	}
	persistent_buffers[persistent_buffer_count].name = (GLuint)name;
	persistent_buffers[persistent_buffer_count].size = size;
	persistent_buffers[persistent_buffer_count].mapping = mapping;
	persistent_buffer_count++;
	return 1;
}

/* writes into a buffer given persistent storage; 0 if it has none */
int host_gl_persistent_write(uint32_t buffer, uint32_t offset, uint32_t size, const void *data)
{
	int index;

	for (index = 0; index < persistent_buffer_count; index++)
	{
		struct persistent_buffer *entry = &persistent_buffers[index];

		if (entry->name == buffer)
		{
			if (offset > entry->size || size > entry->size - offset)
				return 0;
			memcpy(entry->mapping + offset, data, size);
			return 1;
		}
	}
	return 0;
}

/* writes data into the buffer bound to target without waiting for the
GPU: the renderer only streams into ranges no queued draw uses. (Mali
copies the whole buffer for a glBufferSubData into a buffer that queued
draws still reference; with hundreds of small uploads per frame that
exhausts memory within seconds.) */
void host_gl_buffer_write(uint32_t target, uint32_t offset, uint32_t size, const void *data)
{
	void *mapping = glMapBufferRange(target, offset, size,
		GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT | GL_MAP_INVALIDATE_RANGE_BIT);

	if (!mapping)
	{
		glBufferSubData(target, offset, size, data);
		return;
	}
	memcpy(mapping, data, size);
	glUnmapBuffer(target);
}
