// Primitives the rest of tinygen is written in: sized integers, strings, growable
// arrays, hash maps and the per-thread arena.
//
// tinygen is a unity build: tinygen.c includes every other .c file, so everything
// that is not public API is static.
#ifndef TG_BASE_H
#define TG_BASE_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <setjmp.h>

typedef int8_t    i8;
typedef int16_t   i16;
typedef int32_t   i32;
typedef int64_t   i64;
typedef uint8_t   u8;
typedef uint16_t  u16;
typedef uint32_t  u32;
typedef uint64_t  u64;
typedef ptrdiff_t isize;
typedef size_t    usize;
typedef float     f32;
typedef double    f64;

#define tg_cast(T) (T)
#define tg_count_of(a) ((isize)(sizeof(a)/sizeof((a)[0])))

#if defined(_MSC_VER)
	#if defined(__clang__) // clang-cl
		#define TG_INTERNAL static __attribute__((unused))
	#else
		#define TG_INTERNAL static
	#endif
	#define TG_THREAD_LOCAL __declspec(thread)
	#define TG_NORETURN __declspec(noreturn)
	#define TG_FORCEINLINE static __forceinline
#else
	#define TG_INTERNAL static __attribute__((unused))
	#define TG_THREAD_LOCAL __thread
	#define TG_NORETURN __attribute__((noreturn))
	#define TG_FORCEINLINE static inline __attribute__((always_inline, unused))
#endif

TG_NORETURN TG_INTERNAL void tg_assert_fail(char const *file, int line, char const *cond, char const *msg) {
	fprintf(stderr, "tinygen: %s:%d: assertion failed: %s%s%s\n", file, line, cond, msg ? ": " : "", msg ? msg : "");
	abort();
}

#define TG_ASSERT(c) do { if (!(c)) tg_assert_fail(__FILE__, __LINE__, #c, NULL); } while (0)
#define TG_ASSERT_MSG(c, msg) do { if (!(c)) tg_assert_fail(__FILE__, __LINE__, #c, (msg)); } while (0)
#define TG_PANIC(msg) tg_assert_fail(__FILE__, __LINE__, "panic", (msg))

#define tg_max(a, b) ((a) > (b) ? (a) : (b))
#define tg_min(a, b) ((a) < (b) ? (a) : (b))
#define tg_clamp(x, lo, hi) tg_min(tg_max((x), (lo)), (hi))

TG_INTERNAL i64 tg_align_up(i64 x, i64 a) { return (x + a - 1) & ~(a - 1); }
TG_INTERNAL bool tg_is_pow2(i64 x) { return x > 0 && (x & (x - 1)) == 0; }
TG_INTERNAL i64 tg_next_pow2(i64 x) { i64 p = 1; while (p < x) p <<= 1; return p; }

////////////////////////////////////////////////////////////////
// Strings: not owned, not zero terminated
////////////////////////////////////////////////////////////////

typedef struct tgString {
	char const *text;
	isize       len;
} tgString;

#define tg_str_lit(s) tg_str_make((s), (isize)(sizeof(s) - 1))

TG_INTERNAL tgString tg_str(char const *s) { tgString r; r.text = s; r.len = s ? (isize)strlen(s) : 0; return r; }
TG_INTERNAL tgString tg_str_make(char const *s, isize len) { tgString r; r.text = s; r.len = len; return r; }
TG_INTERNAL bool tg_str_eq(tgString a, tgString b) { return a.len == b.len && (a.len == 0 || memcmp(a.text, b.text, (usize)a.len) == 0); }
TG_INTERNAL bool tg_str_has_prefix(tgString s, tgString p) { return s.len >= p.len && memcmp(s.text, p.text, (usize)p.len) == 0; }

// MurmurHash64A, eight bytes a step: mangled names are long.
TG_INTERNAL u64 tg_hash_bytes(void const *data, isize len) {
	u64 const k_m = 0xc6a4a7935bd1e995ull;
	u64 h = 0x9747b28cull ^ ((u64)len * k_m);
	u8 const *p = (u8 const *)data;
	for (isize i = 0; i + 8 <= len; i += 8) {
		u64 k;
		memcpy(&k, p + i, 8);
		k *= k_m;
		k ^= k >> 47;
		k *= k_m;
		h ^= k;
		h *= k_m;
	}
	p += len & ~(isize)7;
	switch (len & 7) {
	case 7: h ^= (u64)p[6] << 48; /* fallthrough */
	case 6: h ^= (u64)p[5] << 40; /* fallthrough */
	case 5: h ^= (u64)p[4] << 32; /* fallthrough */
	case 4: h ^= (u64)p[3] << 24; /* fallthrough */
	case 3: h ^= (u64)p[2] << 16; /* fallthrough */
	case 2: h ^= (u64)p[1] << 8;  /* fallthrough */
	case 1: h ^= (u64)p[0]; h *= k_m;
	}
	h ^= h >> 47;
	h *= k_m;
	h ^= h >> 47;
	return h;
}

TG_INTERNAL u64 tg_hash_u64(u64 x) {
	x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
	x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
	x ^= x >> 33;
	return x;
}

////////////////////////////////////////////////////////////////
// Arena: a bump allocator for everything that lives while one procedure is
// built and lowered. Reset keeps the memory. Memory comes back zeroed.
////////////////////////////////////////////////////////////////

typedef struct tgArenaChunk {
	u8 *  base;
	isize size;
} tgArenaChunk;

typedef struct tgArena {
	tgArenaChunk *chunks;
	isize          chunk_count;
	isize          chunk_cap;
	isize          curr;
	isize          used;
	isize          chunk_size; // 0: 4 MB
} tgArena;

typedef struct tgArenaMark {
	isize curr;
	isize used;
} tgArenaMark;

TG_INTERNAL void *tg_arena_alloc_raw(tgArena *a, isize size, isize align) {
	if (align < 1) align = 1;
	for (;;) {
		if (a->curr < a->chunk_count) {
			tgArenaChunk *c = &a->chunks[a->curr];
			isize start = (a->used + align - 1) & ~(align - 1);
			if (start + size <= c->size) {
				a->used = start + size;
				return c->base + start;
			}
			a->curr += 1;
			a->used = 0;
			continue;
		}
		if (a->chunk_count == a->chunk_cap) {
			a->chunk_cap = a->chunk_cap ? 2*a->chunk_cap : 16;
			a->chunks = (tgArenaChunk *)realloc(a->chunks, (usize)a->chunk_cap * sizeof(tgArenaChunk));
			TG_ASSERT(a->chunks != NULL);
		}
		tgArenaChunk c;
		c.size = tg_max(size + align, a->chunk_size ? a->chunk_size : (isize)(4 << 20));
		c.base = (u8 *)malloc((usize)c.size);
		TG_ASSERT(c.base != NULL);
		a->chunks[a->chunk_count++] = c;
	}
}

TG_INTERNAL void *tg_arena_alloc(tgArena *a, isize size, isize align) {
	void *p = tg_arena_alloc_raw(a, size, align);
	memset(p, 0, (usize)size);
	return p;
}

// The new size of an allocation; the part past the old size is not zeroed. The last
// allocation grows in place.
TG_INTERNAL void *tg_arena_resize(tgArena *a, void *old_memory, isize old_size, isize size, isize align) {
	if (old_memory != NULL && a->curr < a->chunk_count) {
		tgArenaChunk *c = &a->chunks[a->curr];
		isize start = (u8 *)old_memory - c->base;
		if (start >= 0 && start + old_size == a->used && start + size <= c->size) {
			a->used = start + size;
			return old_memory;
		}
	}
	u8 *p = (u8 *)tg_arena_alloc_raw(a, size, align);
	if (old_memory != NULL) memmove(p, old_memory, (usize)old_size);
	return p;
}

TG_INTERNAL void tg_arena_reset(tgArena *a) { a->curr = 0; a->used = 0; }
TG_INTERNAL tgArenaMark tg_arena_mark(tgArena *a) { tgArenaMark m; m.curr = a->curr; m.used = a->used; return m; }
// Frees everything allocated since the mark.
TG_INTERNAL void tg_arena_release(tgArena *a, tgArenaMark m) { a->curr = m.curr; a->used = m.used; }

TG_INTERNAL void tg_arena_free(tgArena *a) {
	for (isize i = 0; i < a->chunk_count; i++) free(a->chunks[i].base);
	free(a->chunks);
	memset(a, 0, sizeof(*a));
}

// The arena of the procedure being built or lowered on this thread.
static TG_THREAD_LOCAL tgArena  tg_arena_own;
static TG_THREAD_LOCAL tgArena *tg_arena_cur;

TG_INTERNAL tgArena *tg_arena(void) { return tg_arena_cur ? tg_arena_cur : &tg_arena_own; }

#define tg_alloc_item(T) ((T *)tg_arena_alloc(tg_arena(), (isize)sizeof(T), 16))
#define tg_alloc_items(T, n) ((T *)tg_arena_alloc(tg_arena(), (isize)sizeof(T)*(isize)(n), 16))

// A copy of the string in the current arena, zero terminated.
TG_INTERNAL tgString tg_str_dup_arena(tgString s) {
	char *p = (char *)tg_arena_alloc(tg_arena(), s.len + 1, 1);
	if (s.len) memcpy(p, s.text, (usize)s.len);
	return tg_str_make(p, s.len);
}

////////////////////////////////////////////////////////////////
// Growable arrays
//
// Declare an array type with TG_ARRAY(Name, T). Its memory comes from `arena`
// when that is set, else from the heap. Index with a.data[i]. Growing leaves the new
// capacity as it is: items past the count are not zeroed until they are added.
////////////////////////////////////////////////////////////////

#define TG_ARRAY(Name, T) typedef struct Name { T *data; isize count; isize cap; tgArena *arena; } Name

TG_INTERNAL void tg__array_grow(void **data, isize *cap, tgArena *arena, isize need, isize elem_size, isize elem_align) {
	if (need <= *cap) return;
	isize new_cap = *cap ? *cap * 2 : 8;
	while (new_cap < need) new_cap *= 2;
	if (arena) {
		*data = tg_arena_resize(arena, *data, *cap * elem_size, new_cap * elem_size, elem_align);
	} else {
		void *p = realloc(*data, (usize)(new_cap * elem_size));
		TG_ASSERT(p != NULL);
		*data = p;
	}
	*cap = new_cap;
}

// The capacity check is inline: appends call this for every item.
#define tg_array_reserve(a, n) \
	((n) > (a)->cap ? tg__array_grow((void **)&(a)->data, &(a)->cap, (a)->arena, (n), (isize)sizeof(*(a)->data), 16) : (void)0)

#define tg_array_add(a, v) \
	(tg_array_reserve((a), (a)->count + 1), (a)->data[(a)->count++] = (v))

// Appends n zeroed items and returns a pointer to the first.
#define tg_array_push_zero(a, n) \
	(tg_array_reserve((a), (a)->count + (n)), \
	 memset(&(a)->data[(a)->count], 0, sizeof(*(a)->data) * (usize)(n)), \
	 (a)->count += (n), &(a)->data[(a)->count - (n)])

#define tg_array_add_elems(a, ptr, n) \
	(tg_array_reserve((a), (a)->count + (n)), \
	 memcpy(&(a)->data[(a)->count], (ptr), sizeof(*(a)->data) * (usize)(n)), \
	 (a)->count += (n))

// Sets the count; new items are zeroed.
#define tg_array_resize(a, n) do { \
	isize tg__n = (n); \
	tg_array_reserve((a), tg__n); \
	if (tg__n > (a)->count) memset(&(a)->data[(a)->count], 0, sizeof(*(a)->data) * (usize)(tg__n - (a)->count)); \
	(a)->count = tg__n; \
} while (0)

#define tg_array_pop(a)   ((a)->data[--(a)->count])
#define tg_array_last(a)  (&(a)->data[(a)->count - 1])
#define tg_array_clear(a) ((a)->count = 0)
#define tg_array_free(a)  do { if (!(a)->arena) free((a)->data); (a)->data = NULL; (a)->count = (a)->cap = 0; } while (0)

// A view of items owned elsewhere.
#define TG_SLICE(Name, T) typedef struct Name { T *data; isize count; } Name

// A copy of an array's items in the current arena, as a slice of type SliceT.
#define tg_slice_from_array(SliceT, a, out) do { \
	(out)->count = (a)->count; \
	(out)->data = NULL; \
	if ((a)->count) { \
		*(void **)&(out)->data = tg_arena_alloc(tg_arena(), (isize)sizeof(*(a)->data) * (a)->count, 16); \
		memcpy((out)->data, (a)->data, sizeof(*(a)->data) * (usize)(a)->count); \
	} \
} while (0)

TG_ARRAY(tgBytes, u8);
TG_ARRAY(tgI32Array, i32);
TG_ARRAY(tgI64Array, i64);
TG_ARRAY(tgU32Array, u32);
TG_ARRAY(tgBoolArray, bool);
TG_ARRAY(tgIsizeArray, isize);
TG_ARRAY(tgStringArray, tgString);
TG_SLICE(tgI32Slice, i32);
TG_SLICE(tgI64Slice, i64);
TG_SLICE(tgU8Slice, u8);

////////////////////////////////////////////////////////////////
// Hash maps: u64 keys (integers, pointers) and string keys, i64 values.
// Open addressing with linear probing. The heap holds them.
////////////////////////////////////////////////////////////////

typedef struct tgMapSlot {
	u64  key;
	i64  value;
	bool used;
} tgMapSlot;

typedef struct tgMap {
	tgMapSlot *slots;
	isize       cap;   // a power of two, or 0
	isize       count;
} tgMap;

TG_INTERNAL void tg_map_set(tgMap *m, u64 key, i64 value);

TG_INTERNAL void tg__map_grow(tgMap *m) {
	tgMapSlot *old = m->slots;
	isize old_cap = m->cap;
	m->cap = old_cap ? old_cap * 2 : 16;
	m->slots = (tgMapSlot *)calloc((usize)m->cap, sizeof(tgMapSlot));
	TG_ASSERT(m->slots != NULL);
	m->count = 0;
	for (isize i = 0; i < old_cap; i++) {
		if (old[i].used) tg_map_set(m, old[i].key, old[i].value);
	}
	free(old);
}

TG_INTERNAL i64 *tg_map_get(tgMap *m, u64 key) {
	if (m->cap == 0) return NULL;
	isize mask = m->cap - 1;
	for (isize i = (isize)(tg_hash_u64(key) & (u64)mask);; i = (i + 1) & mask) {
		tgMapSlot *s = &m->slots[i];
		if (!s->used) return NULL;
		if (s->key == key) return &s->value;
	}
}

TG_INTERNAL void tg_map_set(tgMap *m, u64 key, i64 value) {
	if ((m->count + 1) * 4 >= m->cap * 3) tg__map_grow(m);
	isize mask = m->cap - 1;
	for (isize i = (isize)(tg_hash_u64(key) & (u64)mask);; i = (i + 1) & mask) {
		tgMapSlot *s = &m->slots[i];
		if (!s->used) { s->used = true; s->key = key; s->value = value; m->count++; return; }
		if (s->key == key) { s->value = value; return; }
	}
}

TG_INTERNAL void tg_map_clear(tgMap *m) {
	if (m->slots) memset(m->slots, 0, (usize)m->cap * sizeof(tgMapSlot));
	m->count = 0;
}

TG_INTERNAL void tg_map_free(tgMap *m) { free(m->slots); memset(m, 0, sizeof(*m)); }

// String keys: the entries are dense, in insertion order, and the table only holds a tag of
// each hash and the entry's index. Probing reads 8 bytes a slot, and growing rebuilds only
// the table.
typedef struct tgStrMapEntry {
	tgString key;   // in the map's arena
	u64      hash;
	i64      value;
} tgStrMapEntry;

typedef struct tgStrMapSlot {
	u32 tag;   // the hash's high half
	u32 entry; // index + 1, 0 when empty
} tgStrMapSlot;

typedef struct tgStrMap {
	tgStrMapSlot * slots;
	isize          cap;     // a power of two, or 0
	tgStrMapEntry *entries;
	isize          count;
	isize          entry_cap;
	tgArena        keys;
} tgStrMap;

TG_INTERNAL u64 tg_strmap_hash(tgString key) { return tg_hash_bytes(key.text, key.len); }

// Lookups and inserts by a hash from tg_strmap_hash, for a caller that does both.
TG_INTERNAL i64 *tg_strmap_get_hashed(tgStrMap *m, tgString key, u64 h) {
	if (m->cap == 0) return NULL;
	isize mask = m->cap - 1;
	u32 tag = (u32)(h >> 32);
	for (isize i = (isize)(h & (u64)mask);; i = (i + 1) & mask) {
		tgStrMapSlot s = m->slots[i];
		if (s.entry == 0) return NULL;
		if (s.tag == tag) {
			tgStrMapEntry *e = &m->entries[s.entry - 1];
			if (e->hash == h && tg_str_eq(e->key, key)) return &e->value;
		}
	}
}

TG_INTERNAL i64 *tg_strmap_get(tgStrMap *m, tgString key) {
	return tg_strmap_get_hashed(m, key, tg_strmap_hash(key));
}

TG_INTERNAL void tg__strmap_place(tgStrMap *m, u64 h, u32 entry) {
	isize mask = m->cap - 1;
	for (isize i = (isize)(h & (u64)mask);; i = (i + 1) & mask) {
		if (m->slots[i].entry == 0) {
			m->slots[i].tag = (u32)(h >> 32);
			m->slots[i].entry = entry;
			return;
		}
	}
}

TG_INTERNAL void tg_strmap_set_hashed(tgStrMap *m, tgString key, u64 h, i64 value) {
	i64 *found = tg_strmap_get_hashed(m, key, h);
	if (found) { *found = value; return; }
	if ((m->count + 1) * 4 >= m->cap * 3) {
		free(m->slots);
		m->cap = m->cap ? m->cap * 2 : 16;
		m->slots = (tgStrMapSlot *)calloc((usize)m->cap, sizeof(tgStrMapSlot));
		TG_ASSERT(m->slots != NULL);
		for (isize i = 0; i < m->count; i++) tg__strmap_place(m, m->entries[i].hash, (u32)(i + 1));
	}
	if (m->count == m->entry_cap) {
		m->entry_cap = m->entry_cap ? m->entry_cap * 2 : 16;
		m->entries = (tgStrMapEntry *)realloc(m->entries, (usize)m->entry_cap * sizeof(tgStrMapEntry));
		TG_ASSERT(m->entries != NULL);
	}
	if (m->keys.chunk_size == 0) m->keys.chunk_size = 64 << 10;
	char *text = (char *)tg_arena_alloc_raw(&m->keys, key.len + 1, 1);
	if (key.len) memcpy(text, key.text, (usize)key.len);
	text[key.len] = 0;
	tgStrMapEntry *e = &m->entries[m->count];
	e->key = tg_str_make(text, key.len);
	e->hash = h;
	e->value = value;
	m->count += 1;
	tg__strmap_place(m, h, (u32)m->count);
}

TG_INTERNAL void tg_strmap_set(tgStrMap *m, tgString key, i64 value) {
	tg_strmap_set_hashed(m, key, tg_strmap_hash(key), value);
}

TG_INTERNAL void tg_strmap_free(tgStrMap *m) {
	free(m->slots);
	free(m->entries);
	tg_arena_free(&m->keys);
	memset(m, 0, sizeof(*m));
}

////////////////////////////////////////////////////////////////
// Sorting
////////////////////////////////////////////////////////////////

typedef int (*tgCompareProc)(void const *a, void const *b);
#define tg_sort(data, count, cmp) qsort((data), (usize)(count), sizeof(*(data)), (cmp))

#endif
