// cafeglsl.rpl: CafeGLSL (https://github.com/Exzap/CafeGLSL, MIT) as its own
// Cafe OS module, so its ~5 MB of code stays out of Cafe Godot's .rpx, whose
// .text must stay under 32 MB. Godot loads it with OSDynLoad and calls the
// exports in exports.def through function pointers.
//
// Memory: the app (Cafe Godot) owns the process heap. MEMAllocFromDefaultHeapEx
// there ends in Godot's malloc, so this module allocates through it, keeping
// each block's size in a 32-byte header (wut's RPL realloc asks
// MEMGetSizeForMBlockExpHeap, which is wrong for those blocks).
#include <coreinit/dynload.h>
#include <coreinit/memdefaultheap.h>
#include <reent.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define HEADER 32 // Keeps 32-byte alignment for plain mallocs.

typedef struct {
	void *base;  // What MEMAllocFromDefaultHeapEx returned.
	size_t size; // Usable size.
} BlockInfo;

static void *block_alloc(size_t size, size_t align) {
	if (align < HEADER) {
		align = HEADER;
	}
	uint8_t *base = (uint8_t *)MEMAllocFromDefaultHeapEx(size + align + HEADER, (int)align);
	if (base == NULL) {
		return NULL;
	}
	uint8_t *ptr = base + align; // align >= HEADER, so the header fits before ptr.
	BlockInfo *info = (BlockInfo *)(ptr - sizeof(BlockInfo));
	info->base = base;
	info->size = size;
	return ptr;
}

static BlockInfo *block_info(void *ptr) {
	return (BlockInfo *)((uint8_t *)ptr - sizeof(BlockInfo));
}

void *_malloc_r(struct _reent *r, size_t size) {
	(void)r;
	return block_alloc(size, HEADER);
}

void _free_r(struct _reent *r, void *ptr) {
	(void)r;
	if (ptr) {
		MEMFreeToDefaultHeap(block_info(ptr)->base);
	}
}

void *_calloc_r(struct _reent *r, size_t n, size_t size) {
	(void)r;
	size_t total = n * size;
	void *p = block_alloc(total, HEADER);
	if (p) {
		memset(p, 0, total);
	}
	return p;
}

void *_realloc_r(struct _reent *r, void *ptr, size_t size) {
	(void)r;
	if (ptr == NULL) {
		return block_alloc(size, HEADER);
	}
	if (size == 0) {
		_free_r(r, ptr);
		return NULL;
	}
	size_t old = block_info(ptr)->size;
	if (size <= old) {
		return ptr;
	}
	void *p = block_alloc(size, HEADER);
	if (p) {
		memcpy(p, ptr, old);
		_free_r(r, ptr);
	}
	return p;
}

void *_memalign_r(struct _reent *r, size_t align, size_t size) {
	(void)r;
	return block_alloc(size, align);
}

size_t _malloc_usable_size_r(struct _reent *r, void *ptr) {
	(void)r;
	return ptr ? block_info(ptr)->size : 0;
}

void *malloc(size_t size) { return _malloc_r(NULL, size); }
void free(void *ptr) { _free_r(NULL, ptr); }
void *calloc(size_t n, size_t size) { return _calloc_r(NULL, n, size); }
void *realloc(void *ptr, size_t size) { return _realloc_r(NULL, ptr, size); }
void *memalign(size_t align, size_t size) { return _memalign_r(NULL, align, size); }
void *aligned_alloc(size_t align, size_t size) { return _memalign_r(NULL, align, size); }
int posix_memalign(void **out, size_t align, size_t size) {
	void *p = _memalign_r(NULL, align, size);
	if (!p) {
		return 12; // ENOMEM
	}
	*out = p;
	return 0;
}
size_t malloc_usable_size(void *ptr) { return _malloc_usable_size_r(NULL, ptr); }

// wut's RPL start calls this; ours replaces wut_malloc.o (which would also
// bring its own _malloc_r family).
void __init_wut_malloc(void) {
}
void __fini_wut_malloc(void) {
}

// C++ static constructors (Mesa): run once on load, unless __eabi already did.
extern void __init(void);
static int ctors_ran = 0;
__attribute__((constructor)) static void mark_ctors_ran(void) {
	ctors_ran = 1;
}

int rpl_entry(OSDynLoad_Module module, OSDynLoad_EntryReason reason) {
	(void)module;
	if (reason == OS_DYNLOAD_LOADED && !ctors_ran) {
		__init();
	}
	return 0;
}

// Module entry, replacing wut's __rpl_start (-Wl,--entry=cafeglsl_start). With
// __rpl_start the module hung during OSDynLoad_Acquire; these same steps work.
extern void __init_wut(void);
extern void __eabi(void);
int cafeglsl_start(OSDynLoad_Module module, OSDynLoad_EntryReason reason) {
	if (reason == OS_DYNLOAD_LOADED) {
		__init_wut();
		__eabi();
	}
	return rpl_entry(module, reason);
}
