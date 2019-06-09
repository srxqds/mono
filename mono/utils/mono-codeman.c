/**
 * \file
 */

#include "config.h"

#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <glib.h>

/* For dlmalloc.h */
#define USE_DL_PREFIX 1

#include "mono-codeman.h"
#include "mono-mmap.h"
#include "mono-counters.h"
#include "dlmalloc.h"
#include <mono/metadata/profiler-private.h>
#ifdef HAVE_VALGRIND_MEMCHECK_H
#include <valgrind/memcheck.h>
#endif

#include <mono/utils/mono-os-mutex.h>


static uintptr_t code_memory_used = 0;
static size_t dynamic_code_alloc_count;
static size_t dynamic_code_bytes_count;
static size_t dynamic_code_frees_count;
static MonoCodeManagerCallbacks code_manager_callbacks;

/*
 * AMD64 processors maintain icache coherency only for pages which are 
 * marked executable. Also, windows DEP requires us to obtain executable memory from
 * malloc when using dynamic code managers. The system malloc can't do this so we use a 
 * slighly modified version of Doug Lea's Malloc package for this purpose:
 * http://g.oswego.edu/dl/html/malloc.html
 */

#define MIN_PAGES 16

#if defined(__x86_64__) || defined (_WIN64)
/*
 * We require 16 byte alignment on amd64 so the fp literals embedded in the code are 
 * properly aligned for SSE2.
 */
#define MIN_ALIGN 16
#else
#define MIN_ALIGN 8
#endif

/* if a chunk has less than this amount of free space it's considered full */
#define MAX_WASTAGE 32
#define MIN_BSIZE 32

#ifdef __x86_64__
#define ARCH_MAP_FLAGS MONO_MMAP_32BIT
#else
#define ARCH_MAP_FLAGS 0
#endif

#define MONO_PROT_RWX (MONO_MMAP_READ|MONO_MMAP_WRITE|MONO_MMAP_EXEC|MONO_MMAP_JIT)

// extend by dsqiu
typedef struct _MonoUnusedEntity {
	struct _MonoUnusedEntity* next;
	guint32 size;
	char* pos;
} MonoUnusedEntity;

// extend end

typedef struct _CodeChunck CodeChunk;

enum {
	CODE_FLAG_MMAP,
	CODE_FLAG_MALLOC
};

struct _CodeChunck {
	char *data;
	int pos;
	int size;
	CodeChunk *next;
	unsigned int flags: 8;
	/* this number of bytes is available to resolve addresses far in memory */
	unsigned int bsize: 24;
};

struct _MonoCodeManager {
	int dynamic;
	int read_only;
	CodeChunk *current;
	CodeChunk *full;
	CodeChunk *last;
	// extend by dsqiu
	// empty->empty->unused_memory->unused_memory
	MonoUnusedEntity* unuseds;
	mono_bool reusable;
	// extend end
};

#define ALIGN_INT(val,alignment) (((val) + (alignment - 1)) & ~(alignment - 1))

#define VALLOC_FREELIST_SIZE 16

// extend by dsqiu

void
mono_code_set_reusable(MonoCodeManager *pool, mono_bool enable)
{
	pool->reusable = enable;
}

static void
mono_code_unused_status(MonoCodeManager* pool)
{
	if (!pool)
		return;
	// print status
	int count = 0;
	guint32 still_free = 0;
	int empty_count = 0;

	MonoUnusedEntity* unuseds = pool->unuseds;
	while (unuseds)
	{
		still_free += unuseds->size;
		if (unuseds->size == 0)
			empty_count += 1;
		count += 1;
		unuseds = unuseds->next;

	}
	g_print("mono_code_unused_status count: %d, empty: %d, size: %d\n", count, empty_count, still_free);
}

static CodeChunk* mono_code_chunk_find(MonoCodeManager* cman, char* addr)
{	
	CodeChunk* chunk = NULL;
	for (chunk = cman->current; chunk; chunk = chunk->next)
	{
		if (addr >= chunk->data && addr < chunk->data + chunk->size)
			return chunk;
	}
	for (chunk = cman->full; chunk; chunk = chunk->next)
	{
		if (addr >= chunk->data && addr < chunk->data + chunk->size)
			return chunk;
	}
	return NULL;
}

static void
mono_code_unused_destroy(MonoUnusedEntity* unused_entity)
{
	MonoUnusedEntity *p, *n;
	p = unused_entity;
	while (p) {
		n = p->next;
		g_free(p);
		p = n;
	}
}

static MonoUnusedEntity*
mono_code_unused_new()
{
	MonoUnusedEntity* unused_entity = (MonoUnusedEntity *)g_malloc(sizeof(MonoUnusedEntity));
	unused_entity->pos = NULL;
	unused_entity->size = 0;
	unused_entity->next = NULL;
	return unused_entity;
}

static void
mono_code_unused_recycle(MonoCodeManager* root, MonoUnusedEntity* reuse_entity)
{
	reuse_entity->pos = NULL;
	reuse_entity->size = 0;
	MonoUnusedEntity* unused_list = root->unuseds;
	while (unused_list)
	{
		if (unused_list->next == reuse_entity)
		{
			unused_list->next = reuse_entity->next;
			reuse_entity->next = NULL;
			break;
		}
		unused_list = unused_list->next;
	}
	reuse_entity->next = root->unuseds;
	root->unuseds = reuse_entity;
}

// insert free memory to unuseds
static gboolean
mono_code_unused_insert(MonoCodeManager* root, char* addr, gint32 size, CodeChunk* chunk)
{
	if (!root->reusable)
		return FALSE;
	MonoUnusedEntity* new_entity = NULL;
	if (!root->unuseds)
	{
		root->unuseds = mono_code_unused_new();
		root->unuseds->pos = addr;
		root->unuseds->size = size;
		return TRUE;
	}
	g_print("mono_code_unused_insert size: %d\n", size);
	char* pool_end = chunk->data + chunk->size;
	char* pool_start = chunk->data;
	MonoUnusedEntity* unused_list = root->unuseds;
	// check if has adjacent entity, join
	while (unused_list)
	{
		char* pre_addr = unused_list->pos;
		if (pre_addr >= pool_start && pre_addr < pool_end)
		{
			if (addr == unused_list->pos)
			{
				g_print("Free code repeatly\n");
				return FALSE;
			}
			if (unused_list->pos + unused_list->size == addr)
			{
				unused_list->size += size;
				// check next entity if adjacent
				MonoUnusedEntity* next_entity = unused_list->next;
				if (next_entity && next_entity->pos >= pool_start && next_entity->pos < pool_end && (next_entity->pos == unused_list->pos + unused_list->size))
				{
					unused_list->size += next_entity->size;
					// mono_code_unused_status(root);
					mono_code_unused_recycle(root, next_entity);
					// mono_code_unused_status(root);
				}
				return TRUE;
			}
			else if (addr + size == unused_list->pos)
			{
				unused_list->pos = addr;
				unused_list->size += size;
				return TRUE;
			}
		}
		unused_list = unused_list->next;
	}
	if (root->unuseds->pos)
	{
		new_entity = mono_code_unused_new();
	}
	else
	{
		new_entity = root->unuseds;
		root->unuseds = new_entity->next;
		new_entity->next = NULL;
	}
	new_entity->size = size;
	new_entity->pos = addr;
	unused_list = root->unuseds;
	gboolean has_same_pool = FALSE;
	MonoUnusedEntity* pre_entity = NULL;
	while (unused_list)
	{
		char* pre_addr = unused_list->pos;
		if (pre_addr >= pool_start && pre_addr < pool_end)
		{
			has_same_pool = TRUE;
			if (pre_addr > addr)
			{
				break;
			}
			else
			{
				pre_entity = unused_list;
			}
		}
		else if (has_same_pool)
		{
			break;
		}
		else // last
		{
			pre_entity = unused_list;
		}
		unused_list = unused_list->next;
	}
	if (pre_entity)
	{
		new_entity->next = pre_entity->next;
		pre_entity->next = new_entity;
	}
	else
	{
		new_entity->next = root->unuseds;
		root->unuseds = new_entity;
	}
	return TRUE;
}

static gpointer
mono_code_unused_fetch(MonoCodeManager* root, guint32 size, guint32 alignment)
{
	if (!root->reusable)
		return NULL;
	MonoUnusedEntity* unused_list = root->unuseds;
	MonoUnusedEntity* reuse_entity = NULL;
	// max number
	guint32 resue_size = (1 << (8* sizeof(guint32))) - 1;
	guint32 align_mask = alignment - 1;
	mono_code_unused_status(root);
	while (unused_list)
	{
		if (unused_list->pos)
		{
			CodeChunk* chunk = mono_code_chunk_find(root, unused_list->pos);
			guint32 pos = ALIGN_INT(unused_list->pos - chunk->data, alignment);
			char* ptr1 = (void*)((((uintptr_t)chunk->data + align_mask) & ~(uintptr_t)align_mask) + pos);
			char* ptr = (char*)(((uintptr_t)unused_list->pos + align_mask) & ~(uintptr_t)align_mask);
			if (ptr1 != ptr)
			{
				int a = 1;
			}
			if (ptr + size <= unused_list->pos + unused_list->size)
			{
				if (ptr == unused_list->pos && unused_list->size == size)
				{
					reuse_entity = unused_list;
					break;
				}
				else if (resue_size > unused_list->size)
				{
					resue_size = unused_list->size;
					reuse_entity = unused_list;
				}
			}
		}
		unused_list = unused_list->next;
	}
	if (reuse_entity)
	{
		char* rval = (char*)(((uintptr_t)reuse_entity->pos + align_mask) & ~(uintptr_t)align_mask);
		if (rval == reuse_entity->pos && reuse_entity->size == size)
		{
			mono_code_unused_recycle(root, reuse_entity);
		}
		else
		{
			if (rval + size == reuse_entity->pos + reuse_entity->size)
			{
				reuse_entity->size -= size;
			}
			else
			{
				guint32 left_size = rval - reuse_entity->pos;
				guint32 right_size = reuse_entity->size - left_size - size;
				if (left_size != 0)
				{
					reuse_entity->size = left_size;
					mono_code_unused_insert(root, rval + size, right_size, mono_code_chunk_find(root, reuse_entity->pos));
				}
				else
				{
					reuse_entity->size = right_size;
					reuse_entity->pos = rval + size;
				}
			}
		}
		g_print("--------------->mono_code_unused_fetch size: %d\n", size);
		mono_code_unused_status(root);
		return rval;
	}
	return NULL;
}

// extend end



static mono_mutex_t valloc_mutex;
static GHashTable *valloc_freelists;

static void*
codechunk_valloc (void *preferred, guint32 size)
{
	void *ptr;
	GSList *freelist;

	if (!valloc_freelists) {
		mono_os_mutex_init_recursive (&valloc_mutex);
		valloc_freelists = g_hash_table_new (NULL, NULL);
	}

	/*
	 * Keep a small freelist of memory blocks to decrease pressure on the kernel memory subsystem to avoid #3321.
	 */
	mono_os_mutex_lock (&valloc_mutex);
	freelist = (GSList *) g_hash_table_lookup (valloc_freelists, GUINT_TO_POINTER (size));
	if (freelist) {
		ptr = freelist->data;
		memset (ptr, 0, size);
		freelist = g_slist_delete_link (freelist, freelist);
		g_hash_table_insert (valloc_freelists, GUINT_TO_POINTER (size), freelist);
	} else {
		ptr = mono_valloc (preferred, size, MONO_PROT_RWX | ARCH_MAP_FLAGS, MONO_MEM_ACCOUNT_CODE);
		if (!ptr && preferred)
			ptr = mono_valloc (NULL, size, MONO_PROT_RWX | ARCH_MAP_FLAGS, MONO_MEM_ACCOUNT_CODE);
	}
	mono_os_mutex_unlock (&valloc_mutex);
	return ptr;
}

static void
codechunk_vfree (void *ptr, guint32 size)
{
	GSList *freelist;

	mono_os_mutex_lock (&valloc_mutex);
	freelist = (GSList *) g_hash_table_lookup (valloc_freelists, GUINT_TO_POINTER (size));
	if (!freelist || g_slist_length (freelist) < VALLOC_FREELIST_SIZE) {
		freelist = g_slist_prepend (freelist, ptr);
		g_hash_table_insert (valloc_freelists, GUINT_TO_POINTER (size), freelist);
	} else {
		mono_vfree (ptr, size, MONO_MEM_ACCOUNT_CODE);
	}
	mono_os_mutex_unlock (&valloc_mutex);
}		

static void
codechunk_cleanup (void)
{
	GHashTableIter iter;
	gpointer key, value;

	if (!valloc_freelists)
		return;
	g_hash_table_iter_init (&iter, valloc_freelists);
	while (g_hash_table_iter_next (&iter, &key, &value)) {
		GSList *freelist = (GSList *) value;
		GSList *l;

		for (l = freelist; l; l = l->next) {
			mono_vfree (l->data, GPOINTER_TO_UINT (key), MONO_MEM_ACCOUNT_CODE);
		}
		g_slist_free (freelist);
	}
	g_hash_table_destroy (valloc_freelists);
}

void
mono_code_manager_init (void)
{
	mono_counters_register ("Dynamic code allocs", MONO_COUNTER_JIT | MONO_COUNTER_ULONG, &dynamic_code_alloc_count);
	mono_counters_register ("Dynamic code bytes", MONO_COUNTER_JIT | MONO_COUNTER_ULONG, &dynamic_code_bytes_count);
	mono_counters_register ("Dynamic code frees", MONO_COUNTER_JIT | MONO_COUNTER_ULONG, &dynamic_code_frees_count);
}

void
mono_code_manager_cleanup (void)
{
	codechunk_cleanup ();
}

void
mono_code_manager_install_callbacks (MonoCodeManagerCallbacks* callbacks)
{
	code_manager_callbacks = *callbacks;
}

/**
 * mono_code_manager_new:
 *
 * Creates a new code manager. A code manager can be used to allocate memory
 * suitable for storing native code that can be later executed.
 * A code manager allocates memory from the operating system in large chunks
 * (typically 64KB in size) so that many methods can be allocated inside them
 * close together, improving cache locality.
 *
 * Returns: the new code manager
 */
MonoCodeManager* 
mono_code_manager_new (void)
{
	// extend by dsqiu
	MonoCodeManager * value = (MonoCodeManager *)g_malloc0(sizeof(MonoCodeManager));
	value->unuseds = NULL;
	value->reusable = FALSE;
	return value;
	// extend end
	//return (MonoCodeManager *) g_malloc0 (sizeof (MonoCodeManager));
}

/**
 * mono_code_manager_new_dynamic:
 *
 * Creates a new code manager suitable for holding native code that can be
 * used for single or small methods that need to be deallocated independently
 * of other native code.
 *
 * Returns: the new code manager
 */
MonoCodeManager* 
mono_code_manager_new_dynamic (void)
{
	MonoCodeManager *cman = mono_code_manager_new ();
	cman->dynamic = 1;
	return cman;
}


static void
free_chunklist (CodeChunk *chunk)
{
	CodeChunk *dead;
	
#if defined(HAVE_VALGRIND_MEMCHECK_H) && defined (VALGRIND_JIT_UNREGISTER_MAP)
	int valgrind_unregister = 0;
	if (RUNNING_ON_VALGRIND)
		valgrind_unregister = 1;
#define valgrind_unregister(x) do { if (valgrind_unregister) { VALGRIND_JIT_UNREGISTER_MAP(NULL,x); } } while (0) 
#else
#define valgrind_unregister(x)
#endif

	for (; chunk; ) {
		dead = chunk;
		MONO_PROFILER_RAISE (jit_chunk_destroyed, ((mono_byte *) dead->data));
		if (code_manager_callbacks.chunk_destroy)
			code_manager_callbacks.chunk_destroy ((gpointer)dead->data);
		chunk = chunk->next;
		if (dead->flags == CODE_FLAG_MMAP) {
			codechunk_vfree (dead->data, dead->size);
			/* valgrind_unregister(dead->data); */
		} else if (dead->flags == CODE_FLAG_MALLOC) {
			dlfree (dead->data);
		}
		code_memory_used -= dead->size;
		g_free (dead);
	}
}

/**
 * mono_code_manager_destroy:
 * \param cman a code manager
 * Free all the memory associated with the code manager \p cman.
 */
void
mono_code_manager_destroy (MonoCodeManager *cman)
{
	// extend by dsqiu
	mono_code_unused_destroy(cman->unuseds);
	// extend end

	free_chunklist (cman->full);
	free_chunklist (cman->current);
	g_free (cman);
}

/**
 * mono_code_manager_invalidate:
 * \param cman a code manager
 * Fill all the memory with an invalid native code value
 * so that any attempt to execute code allocated in the code
 * manager \p cman will fail. This is used for debugging purposes.
 */
void             
mono_code_manager_invalidate (MonoCodeManager *cman)
{
	CodeChunk *chunk;

#if defined(__i386__) || defined(__x86_64__)
	int fill_value = 0xcc; /* x86 break */
#else
	int fill_value = 0x2a;
#endif

	for (chunk = cman->current; chunk; chunk = chunk->next)
		memset (chunk->data, fill_value, chunk->size);
	for (chunk = cman->full; chunk; chunk = chunk->next)
		memset (chunk->data, fill_value, chunk->size);
}

/**
 * mono_code_manager_set_read_only:
 * \param cman a code manager
 * Make the code manager read only, so further allocation requests cause an assert.
 */
void             
mono_code_manager_set_read_only (MonoCodeManager *cman)
{
	cman->read_only = TRUE;
}

/**
 * mono_code_manager_foreach:
 * \param cman a code manager
 * \param func a callback function pointer
 * \param user_data additional data to pass to \p func
  * Invokes the callback \p func for each different chunk of memory allocated
 * in the code manager \p cman.
 */
void
mono_code_manager_foreach (MonoCodeManager *cman, MonoCodeManagerFunc func, void *user_data)
{
	CodeChunk *chunk;
	for (chunk = cman->current; chunk; chunk = chunk->next) {
		if (func (chunk->data, chunk->size, chunk->bsize, user_data))
			return;
	}
	for (chunk = cman->full; chunk; chunk = chunk->next) {
		if (func (chunk->data, chunk->size, chunk->bsize, user_data))
			return;
	}
}

/* BIND_ROOM is the divisor for the chunck of code size dedicated
 * to binding branches (branches not reachable with the immediate displacement)
 * bind_size = size/BIND_ROOM;
 * we should reduce it and make MIN_PAGES bigger for such systems
 */
#if defined(__ppc__) || defined(__powerpc__)
#define BIND_ROOM 4
#endif
#if defined(TARGET_ARM64)
#define BIND_ROOM 4
#endif

static CodeChunk*
new_codechunk (CodeChunk *last, int dynamic, int size)
{
	int minsize, flags = CODE_FLAG_MMAP;
	int chunk_size, bsize = 0;
	int pagesize, valloc_granule;
	CodeChunk *chunk;
	void *ptr;

#ifdef FORCE_MALLOC
	flags = CODE_FLAG_MALLOC;
#endif

	pagesize = mono_pagesize ();
	valloc_granule = mono_valloc_granule ();

	if (dynamic) {
		chunk_size = size;
		flags = CODE_FLAG_MALLOC;
	} else {
		minsize = MAX (pagesize * MIN_PAGES, valloc_granule);
		if (size < minsize)
			chunk_size = minsize;
		else {
			/* Allocate MIN_ALIGN-1 more than we need so we can still */
			/* guarantee MIN_ALIGN alignment for individual allocs    */
			/* from mono_code_manager_reserve_align.                  */
			size += MIN_ALIGN - 1;
			size &= ~(MIN_ALIGN - 1);
			chunk_size = size;
			chunk_size += valloc_granule - 1;
			chunk_size &= ~ (valloc_granule - 1);
		}
	}
#ifdef BIND_ROOM
	if (dynamic)
		/* Reserve more space since there are no other chunks we might use if this one gets full */
		bsize = (chunk_size * 2) / BIND_ROOM;
	else
		bsize = chunk_size / BIND_ROOM;
	if (bsize < MIN_BSIZE)
		bsize = MIN_BSIZE;
	bsize += MIN_ALIGN -1;
	bsize &= ~ (MIN_ALIGN - 1);
	if (chunk_size - size < bsize) {
		chunk_size = size + bsize;
		if (!dynamic) {
			chunk_size += valloc_granule - 1;
			chunk_size &= ~ (valloc_granule - 1);
		}
	}
#endif

	if (flags == CODE_FLAG_MALLOC) {
		ptr = dlmemalign (MIN_ALIGN, chunk_size + MIN_ALIGN - 1);
		if (!ptr)
			return NULL;
	} else {
		/* Try to allocate code chunks next to each other to help the VM */
		ptr = NULL;
		if (last)
			ptr = codechunk_valloc ((guint8*)last->data + last->size, chunk_size);
		if (!ptr)
			ptr = codechunk_valloc (NULL, chunk_size);
		if (!ptr)
			return NULL;
	}

	if (flags == CODE_FLAG_MALLOC) {
#ifdef BIND_ROOM
		/* Make sure the thunks area is zeroed */
		memset (ptr, 0, bsize);
#endif
	}

	chunk = (CodeChunk *) g_malloc (sizeof (CodeChunk));
	if (!chunk) {
		if (flags == CODE_FLAG_MALLOC)
			dlfree (ptr);
		else
			mono_vfree (ptr, chunk_size, MONO_MEM_ACCOUNT_CODE);
		return NULL;
	}
	chunk->next = NULL;
	chunk->size = chunk_size;
	chunk->data = (char *) ptr;
	chunk->flags = flags;
	chunk->pos = bsize;
	chunk->bsize = bsize;
	if (code_manager_callbacks.chunk_new)
		code_manager_callbacks.chunk_new ((gpointer)chunk->data, chunk->size);
	MONO_PROFILER_RAISE (jit_chunk_created, ((mono_byte *) chunk->data, chunk->size));

	code_memory_used += chunk_size;
	mono_runtime_resource_check_limit (MONO_RESOURCE_JIT_CODE, code_memory_used);
	/*printf ("code chunk at: %p\n", ptr);*/
	return chunk;
}

/**
 * mono_code_manager_reserve_align:
 * \param cman a code manager
 * \param size size of memory to allocate
 * \param alignment power of two alignment value
 * Allocates at least \p size bytes of memory inside the code manager \p cman.
 * \returns the pointer to the allocated memory or NULL on failure
 */
void*
(mono_code_manager_reserve_align) (MonoCodeManager *cman, int size, int alignment)
{
	CodeChunk *chunk, *prev;
	void *ptr;
	guint32 align_mask = alignment - 1;

	g_assert (!cman->read_only);

	/* eventually allow bigger alignments, but we need to fix the dynamic alloc code to
	 * handle this before
	 */
	g_assert (alignment <= MIN_ALIGN);

	if (cman->dynamic) {
		++dynamic_code_alloc_count;
		dynamic_code_bytes_count += size;
	}

	if (!cman->current) {
		cman->current = new_codechunk (cman->last, cman->dynamic, size);
		if (!cman->current)
			return NULL;
		cman->last = cman->current;
	}

	for (chunk = cman->current; chunk; chunk = chunk->next) {
		// extend by dsqiu
		// char* ptr = (char*)(((uintptr_t)chunk->pos + align_mask) & ~(uintptr_t)align_mask);
		// if (ptr + size <= chunk->pos + chunk->size) {
		// extend end
		if (ALIGN_INT (chunk->pos, alignment) + size <= chunk->size) {
			// extend by dsqiu
			char* last_pos = chunk->data + chunk->pos;
			// extend end
			chunk->pos = ALIGN_INT (chunk->pos, alignment);
			/* Align the chunk->data we add to chunk->pos */
			/* or we can't guarantee proper alignment     */
			ptr = (void*)((((uintptr_t)chunk->data + align_mask) & ~(uintptr_t)align_mask) + chunk->pos);
			if (ptr != chunk->data + chunk->pos)
			{
				int a = (char*)ptr - chunk->data - chunk->pos;
				int c = a;
			}
			chunk->pos = ((char*)ptr - chunk->data) + size;
			// extend by dsqiu
			if (cman->reusable && last_pos != ptr)
			{
				mono_code_unused_insert(cman, last_pos, (char*)ptr - last_pos, chunk);
			}
			// extend end
			return ptr;
		}
	}

	// extend by dsqiu
	ptr = mono_code_unused_fetch(cman, size, alignment);
	if (ptr)
		return ptr;
	// extend end

	/* 
	 * no room found, move one filled chunk to cman->full 
	 * to keep cman->current from growing too much
	 */
	prev = NULL;
	for (chunk = cman->current; chunk; prev = chunk, chunk = chunk->next) {
		if (chunk->pos + MIN_ALIGN * 4 <= chunk->size)
			continue;
		if (prev) {
			prev->next = chunk->next;
		} else {
			cman->current = chunk->next;
		}
		chunk->next = cman->full;
		cman->full = chunk;
		break;
	}
	chunk = new_codechunk (cman->last, cman->dynamic, size);
	if (!chunk)
		return NULL;
	chunk->next = cman->current;
	cman->current = chunk;
	cman->last = cman->current;
	// extend by dsqiu
	char* last_pos = chunk->data + chunk->pos;
	// extend end
	chunk->pos = ALIGN_INT (chunk->pos, alignment);
	/* Align the chunk->data we add to chunk->pos */
	/* or we can't guarantee proper alignment     */
	ptr = (void*)((((uintptr_t)chunk->data + align_mask) & ~(uintptr_t)align_mask) + chunk->pos);
	chunk->pos = ((char*)ptr - chunk->data) + size;
	// extend by dsqiu
	if (cman->reusable && last_pos != ptr)
	{
		mono_code_unused_insert(cman, last_pos, (char*)ptr - last_pos, chunk);
	}
	// extend end
	return ptr;
}

/**
 * mono_code_manager_reserve:
 * \param cman a code manager
 * \param size size of memory to allocate
 * Allocates at least \p size bytes of memory inside the code manager \p cman.
 * \returns the pointer to the allocated memory or NULL on failure
 */
void*
(mono_code_manager_reserve) (MonoCodeManager *cman, int size)
{
	return mono_code_manager_reserve_align (cman, size, MIN_ALIGN);
}

/**
 * mono_code_manager_commit:
 * \param cman a code manager
 * \param data the pointer returned by mono_code_manager_reserve ()
 * \param size the size requested in the call to mono_code_manager_reserve ()
 * \param newsize the new size to reserve
 * If we reserved too much room for a method and we didn't allocate
 * already from the code manager, we can get back the excess allocation
 * for later use in the code manager.
 */
mono_bool
mono_code_manager_commit (MonoCodeManager *cman, void *data, int size, int newsize)
{
	g_assert (newsize <= size);

	if (cman->current && (size != newsize) && (data == cman->current->data + cman->current->pos - size)) {
		cman->current->pos -= size - newsize;
		return TRUE;
	}
	return FALSE;
}

/**
 * mono_code_manager_size:
 * \param cman a code manager
 * \param used_size pointer to an integer for the result
 * This function can be used to get statistics about a code manager:
 * the integer pointed to by \p used_size will contain how much
 * memory is actually used inside the code managed \p cman.
 * \returns the amount of memory allocated in \p cman
 */
int
mono_code_manager_size (MonoCodeManager *cman, int *used_size)
{
	CodeChunk *chunk;
	guint32 size = 0;
	guint32 used = 0;
	for (chunk = cman->current; chunk; chunk = chunk->next) {
		size += chunk->size;
		used += chunk->pos;
	}
	for (chunk = cman->full; chunk; chunk = chunk->next) {
		size += chunk->size;
		used += chunk->pos;
	}
	if (used_size)
		*used_size = used;
	return size;
}


// extend by dsqiu
mono_bool
mono_code_chunk_free(MonoCodeManager* cman, void* addr, int size)
{
	if (!cman->reusable)
		return FALSE;
	CodeChunk* chunk = mono_code_chunk_find(cman, addr);
	if(!chunk)
		return FALSE;
	// return mono_code_unused_insert(cman, addr, size, chunk);
	mono_bool res = mono_code_unused_insert(cman, addr, size, chunk);
	g_print("---------------->mono_code_chunk_free size: %d\n", size);
	mono_code_unused_status(cman);
	return res;
}

void mono_code_manager_profiler(MonoCodeManager* cman)
{
	if (!cman)
		return;
	MonoCodeManager *p;
	int count = 0;
	guint32 still_free = 0;
	CodeChunk *chunk;
	guint32 size = 0;
	guint32 used = 0;
	for (chunk = cman->current; chunk; chunk = chunk->next) {
		size += chunk->size;
		used += chunk->pos;
		still_free = chunk->size - chunk->pos;
		count += 1;
	}
	for (chunk = cman->full; chunk; chunk = chunk->next) {
		size += chunk->size;
		used += chunk->pos;
		still_free = chunk->size - chunk->pos;
		count += 1;
	}
	MonoUnusedEntity* unuseds = cman->unuseds;
	while (unuseds)
	{
		still_free += unuseds->size;
		unuseds = unuseds->next;
	}
	g_print("MonoCodeManager %p stats:\n", cman);
	g_print("Total mem allocated: %d\n", size);
	g_print("Chunk mem used: %d\n", used);
	g_print("Num chunks: %d\n", count);
	g_print("Free memory: %d\n", still_free);
	g_print("\n");
	
}
// extend end