/*
 * nghttp3
 *
 * Copyright (c) 2019 nghttp3 contributors
 * Copyright (c) 2017 ngtcp2 contributors
 * Copyright (c) 2012 nghttp2 contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE
 * LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION
 * WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#include "nghttp3_map.h"

#include <string.h>
#include <assert.h>
#include <stdio.h>

#include "nghttp3_conv.h"

#define NGHTTP3_INITIAL_HASHBITS 4

void nghttp3_map_init(nghttp3_map *map, uint64_t seed, const nghttp3_mem *mem) {
  *map = (nghttp3_map){
    .mem = mem,
    .seed = seed,
  };
}

void nghttp3_map_free(nghttp3_map *map) {
  if (!map) {
    return;
  }

  nghttp3_mem_free(map->mem, map->keys);
}

int nghttp3_map_each(const nghttp3_map *map, int (*func)(void *data, void *ptr),
                     void *ptr) {
  int rv;
  size_t i;
  size_t tablelen;

  if (map->size == 0) {
    return 0;
  }

  tablelen = (size_t)1 << map->hashbits;

  for (i = 0; i < tablelen; ++i) {
    if (map->psl[i] == 0) {
      continue;
    }

    rv = func(map->data[i], ptr);
    if (rv != 0) {
      return rv;
    }
  }

  return 0;
}

/* Hasher from
   https://github.com/rust-lang/rustc-hash/blob/dc5c33f1283de2da64d8d7a06401d91aded03ad4/src/lib.rs
   to maximize the output's sensitivity to all input bits. */
#define NGHTTP3_MAP_HASHER 0xf1357aea2e62a9c5ull
/* 64-bit Fibonacci hashing constant, Golden Ratio constant, to get
   the high bits with the good distribution. */
#define NGHTTP3_MAP_FIBO 0x9e3779b97f4a7c15ull

static size_t map_index(const nghttp3_map *map, nghttp3_map_key_type key) {
  key += map->seed;
  key *= NGHTTP3_MAP_HASHER;
  return (size_t)((key * NGHTTP3_MAP_FIBO) >> (64 - map->hashbits));
}

#ifndef WIN32
void nghttp3_map_print_distance(const nghttp3_map *map) {
  size_t i;
  size_t idx;
  size_t tablelen;

  if (map->size == 0) {
    return;
  }

  tablelen = (size_t)1 << map->hashbits;

  for (i = 0; i < tablelen; ++i) {
    if (map->psl[i] == 0) {
      fprintf(stderr, "@%zu <EMPTY>\n", i);
      continue;
    }

    idx = map_index(map, map->keys[i]);
    fprintf(stderr, "@%zu key=%" PRIu64 " base=%zu distance=%u\n", i,
            map->keys[i], idx, map->psl[i] - 1);
  }
}
#endif /* !defined(WIN32) */

static void map_set_entry(nghttp3_map *map, size_t idx,
                          nghttp3_map_key_type key, void *data, size_t psl) {
  map->keys[idx] = key;
  map->data[idx] = data;
  map->psl[idx] = (uint8_t)psl;
}

#define NGHTTP3_SWAP(TYPE, A, B)                                               \
  do {                                                                         \
    TYPE t = (TYPE) * (A);                                                     \
                                                                               \
    *(A) = *(B);                                                               \
    *(B) = t;                                                                  \
  } while (0)

static int map_insert(nghttp3_map *map, nghttp3_map_key_type key, void *data) {
  size_t idx = map_index(map, key);
  size_t mask = ((size_t)1 << map->hashbits) - 1;
  size_t psl = 1;
  size_t kpsl;

  for (;;) {
    kpsl = map->psl[idx];

    if (kpsl == 0) {
      map_set_entry(map, idx, key, data, psl);
      ++map->size;

      return 0;
    }

    if (psl > kpsl) {
      NGHTTP3_SWAP(nghttp3_map_key_type, &key, &map->keys[idx]);
      NGHTTP3_SWAP(void *, &data, &map->data[idx]);
      NGHTTP3_SWAP(uint8_t, &psl, &map->psl[idx]);
    } else if (map->keys[idx] == key) {
      /* This check ensures that no duplicate keys are inserted.  But
         it is just a waste after first swap or if this function is
         called from map_resize.  That said, there is no difference
         with or without this conditional in performance wise. */
      return NGHTTP3_ERR_INVALID_ARGUMENT;
    }

    ++psl;
    idx = (idx + 1) & mask;
  }
}

static int map_resize(nghttp3_map *map, size_t new_hashbits) {
  size_t i;
  size_t tablelen;
  int rv;
  nghttp3_map new_map = {
    .mem = map->mem,
    .seed = map->seed,
    .hashbits = new_hashbits,
  };
  void *buf;
  (void)rv;

  tablelen = (size_t)1 << new_hashbits;

  buf = nghttp3_mem_calloc(map->mem, tablelen,
                           sizeof(nghttp3_map_key_type) + sizeof(void *) +
                             sizeof(uint8_t));
  if (buf == NULL) {
    return NGHTTP3_ERR_NOMEM;
  }

  new_map.keys = buf;
  new_map.data =
    (void *)((uint8_t *)new_map.keys + tablelen * sizeof(nghttp3_map_key_type));
  new_map.psl = (uint8_t *)new_map.data + tablelen * sizeof(void *);

  if (map->size) {
    tablelen = (size_t)1 << map->hashbits;

    for (i = 0; i < tablelen; ++i) {
      if (map->psl[i] == 0) {
        continue;
      }

      rv = map_insert(&new_map, map->keys[i], map->data[i]);

      /* map_insert must not fail because all keys are unique during
         resize. */
      assert(0 == rv);
    }
  }

  nghttp3_mem_free(map->mem, map->keys);
  map->keys = new_map.keys;
  map->data = new_map.data;
  map->psl = new_map.psl;
  map->hashbits = new_hashbits;

  return 0;
}

/* NGHTTP3_MAP_MAX_HASHBITS is the maximum number of bits used for
   hash table.  The theoretical limit of the maximum number of keys
   that can be stored is 1 << NGHTTP3_MAP_MAX_HASHBITS. */
#define NGHTTP3_MAP_MAX_HASHBITS (sizeof(size_t) * 8 - 1)

int nghttp3_map_insert(nghttp3_map *map, nghttp3_map_key_type key, void *data) {
  int rv;
  size_t tablelen;
  size_t new_hashbits;

  assert(data);

  /* tablelen is incorrect if map->hashbits == 0 which leads to
     tablelen = 1, but it is only used to check the load factor, and
     it works in this special case. */
  tablelen = (size_t)1 << map->hashbits;

  /* Load factor is 7 / 8.  Because tablelen is power of 2, (tablelen
     - (tablelen >> 3)) computes tablelen * 7 / 8. */
  if (map->size + 1 >= (tablelen - (tablelen >> 3))) {
    new_hashbits = map->hashbits ? map->hashbits + 1 : NGHTTP3_INITIAL_HASHBITS;
    if (new_hashbits > NGHTTP3_MAP_MAX_HASHBITS) {
      return NGHTTP3_ERR_NOMEM;
    }

    rv = map_resize(map, new_hashbits);
    if (rv != 0) {
      return rv;
    }
  }

  return map_insert(map, key, data);
}

void *nghttp3_map_find(const nghttp3_map *map, nghttp3_map_key_type key) {
  size_t idx;
  size_t psl = 1;
  size_t mask;

  if (map->size == 0) {
    return NULL;
  }

  idx = map_index(map, key);
  mask = ((size_t)1 << map->hashbits) - 1;

  for (;;) {
    if (psl > map->psl[idx]) {
      return NULL;
    }

    if (map->keys[idx] == key) {
      return map->data[idx];
    }

    ++psl;
    idx = (idx + 1) & mask;
  }
}

int nghttp3_map_remove(nghttp3_map *map, nghttp3_map_key_type key) {
  size_t idx;
  size_t dest;
  size_t psl = 1, kpsl;
  size_t mask;

  if (map->size == 0) {
    return NGHTTP3_ERR_INVALID_ARGUMENT;
  }

  idx = map_index(map, key);
  mask = ((size_t)1 << map->hashbits) - 1;

  for (;;) {
    if (psl > map->psl[idx]) {
      return NGHTTP3_ERR_INVALID_ARGUMENT;
    }

    if (map->keys[idx] == key) {
      dest = idx;
      idx = (idx + 1) & mask;

      for (;;) {
        kpsl = map->psl[idx];
        if (kpsl <= 1) {
          map->psl[dest] = 0;
          break;
        }

        map_set_entry(map, dest, map->keys[idx], map->data[idx], kpsl - 1);

        dest = idx;

        idx = (idx + 1) & mask;
      }

      --map->size;

      return 0;
    }

    ++psl;
    idx = (idx + 1) & mask;
  }
}

void nghttp3_map_clear(nghttp3_map *map) {
  if (map->size == 0) {
    return;
  }

  memset(map->psl, 0, sizeof(*map->psl) * ((size_t)1 << map->hashbits));
  map->size = 0;
}

size_t nghttp3_map_size(const nghttp3_map *map) { return map->size; }
