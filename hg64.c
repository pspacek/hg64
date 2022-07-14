/*
 * hg64 - 64-bit histograms
 *
 * Written by Tony Finch <dot@dotat.at> <fanf@isc.org>
 * You may do anything with this. It has no warranty.
 * <https://creativecommons.org/publicdomain/zero/1.0/>
 * SPDX-License-Identifier: CC0-1.0
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "hg64.h"

#define OUTARG(ptr, val) (((ptr) != NULL) && !(*(ptr) = (val)))

/*
 * This data structure is a sparse array of buckets. Keys are usually
 * 12 bits, but the size and accuracy can be reduced by changing
 * MANBITS. (4 is a good alternative)
 *
 * Each bucket stores a count of values belonging to the bucket's key.
 *
 * The upper MANBITS of the key index the `pack` array. Each `pack`
 * contains a packed sparse array of buckets indexed by the lower
 * `log2(PACKSIZE) == 6` bits of the key.
 *
 * A pack uses `popcount()` to avoid storing missing buckets. The
 * `bmp` is a bitmap indicating which buckets are present. There is
 * also a `total` of all the buckets in the `pack` so that we can work
 * with quantiles faster.
 *
 * Values are `uint64_t`. They are mapped to buckets using a simplified
 * floating-point format. The upper six bits of the key are the
 * exponent, indicating the position of the most significant bit in
 * the value. The lower MANBITS of the key are the mantissa; any less
 * significant bits in the value are discarded, which rounds the value
 * to its bucket's nominal value. Like IEEE 754, the most significant
 * bit is not included in the mantissa, except for very small values
 * (less than MANSIZE) which use a denormal format. Because of this, the
 * number of packs is a few less than a power of 2.
 */

#ifndef KEYBITS
#define KEYBITS 12
#endif

#define MANBITS (KEYBITS - 6)
#define MANSIZE (1 << MANBITS)
#define PACKS (MANSIZE - MANBITS)
#define PACKSIZE 64
#define KEYS (PACKS * PACKSIZE)

struct hg64 {
	uint64_t total;
	size_t buckets;
	struct pack {
		uint64_t total;
		uint64_t bmp;
		uint64_t *bucket;
	} pack[PACKS];
};

/**********************************************************************/

hg64 *
hg64_create(void) {
	hg64 *hg = malloc(sizeof(*hg));
	*hg = (hg64){ 0 };
	return(hg);
}

void
hg64_destroy(hg64 *hg) {
	for(unsigned p = 0; p < PACKS; p++) {
		free(hg->pack[p].bucket);
	}
	*hg = (hg64){ 0 };
	free(hg);
}

uint64_t
hg64_population(hg64 *hg) {
	return(hg->total);
}

size_t
hg64_buckets(hg64 *hg) {
	return(hg->buckets);
}

size_t
hg64_size(hg64 *hg) {
	return(sizeof(*hg) + hg->buckets * sizeof(uint64_t));
}

/**********************************************************************/

static inline uint64_t
interpolate(uint64_t min, uint64_t max, uint64_t mul, uint64_t div) {
	double frac = (div == 0) ? 1 : (double)mul / (double)div;
	return((uint64_t)((max - min) * frac) + min);
}

static inline unsigned
popcount(uint64_t bmp) {
	return(__builtin_popcountll((unsigned long long)bmp));
}

static inline uint64_t
get_range(unsigned key) {
	unsigned shift = PACKSIZE - key / MANSIZE - 1;
	return(UINT64_MAX/4 >> shift);
}

static inline uint64_t
get_minval(unsigned key) {
	unsigned exponent = key / MANSIZE - 1;
	uint64_t mantissa = key % MANSIZE + MANSIZE;
	return(key < MANSIZE ? key : mantissa << exponent);
}

static inline uint64_t
get_maxval(unsigned key) {
	return(get_minval(key) + get_range(key));
}

static inline unsigned
get_key(uint64_t value) {
	if(value < MANSIZE) {
		return(value); /* denormal */
	} else {
		unsigned clz = __builtin_clzll((unsigned long long)value);
		unsigned exponent = PACKSIZE - MANBITS - clz;
		unsigned mantissa = value >> (exponent - 1);
		unsigned key = exponent * MANSIZE + mantissa % MANSIZE;
		return(key);
	}
}

/*
 * Here we have fun indexing into a pack, and expanding if if necessary.
 */
static inline uint64_t *
get_bucket(hg64 *hg, unsigned key, bool nullable) {
	struct pack *pack = &hg->pack[key / PACKSIZE];
	uint64_t bit = 1ULL << (key % PACKSIZE);
	uint64_t mask = bit - 1;
	uint64_t bmp = pack->bmp;
	unsigned pos = popcount(bmp & mask);
	if(bmp & bit) {
		return(&pack->bucket[pos]);
	}
	if(nullable) {
		return(NULL);
	}
	unsigned pop = popcount(bmp);
	size_t need = pop + 1;
	size_t move = pop - pos;
	size_t size = sizeof(uint64_t);
	uint64_t *ptr = realloc(pack->bucket, need * size);
	memmove(ptr + pos + 1, ptr + pos, move * size);
	hg->buckets += 1;
	pack->bmp |= bit;
	pack->bucket = ptr;
	pack->bucket[pos] = 0;
	return(&pack->bucket[pos]);
}

static inline uint64_t
get_count(hg64 *hg, unsigned key) {
	uint64_t *bucket = get_bucket(hg, key, true);
	return(bucket == NULL ? 0 : *bucket);
}

static inline void
bump_count(hg64 *hg, unsigned key, uint64_t count) {
	hg->total += count;
	hg->pack[key / PACKSIZE].total += count;
	*get_bucket(hg, key, false) += count;
}

static inline uint64_t
get_subtotal(hg64 *hg, unsigned key) {
	return(hg->pack[key / PACKSIZE].total);
}

/**********************************************************************/

void
hg64_inc(hg64 *hg, uint64_t value) {
	hg64_add(hg, value, 1);
}

void
hg64_add(hg64 *hg, uint64_t value, uint64_t count) {
	bump_count(hg, get_key(value), count);
}

bool
hg64_get(hg64 *hg, unsigned key,
	     uint64_t *pmin, uint64_t *pmax, uint64_t *pcount) {
	if(key < KEYS) {
		OUTARG(pmin, get_minval(key));
		OUTARG(pmax, get_maxval(key));
		OUTARG(pcount, get_count(hg, key));
		return(true);
	} else {
		return(false);
	}
}

void
hg64_merge(hg64 *target, hg64 *source) {
	for(unsigned key = 0; key < KEYS; key++) {
		bump_count(target, key, get_count(source, key));
	}
}

/**********************************************************************/

uint64_t
hg64_value_at_rank(hg64 *hg, uint64_t rank) {
	if(rank >= hg->total) {
		return(UINT64_MAX);
	}

	unsigned key = 0;
	while(key < KEYS) {
		uint64_t subtotal = get_subtotal(hg, key);
		if(rank < subtotal) {
			break;
		}
		rank -= subtotal;
		key += PACKSIZE;
	}
	assert(key < KEYS);

	unsigned stop = key + PACKSIZE;
	while(key < stop) {
		uint64_t count = get_count(hg, key);
		if(rank < count) {
			break;
		}
		rank -= count;
		key += 1;
	}
	assert(key < stop);

	uint64_t min = get_minval(key);
	uint64_t max = get_maxval(key);
	uint64_t count = get_count(hg, key);
	return(interpolate(min, max, rank, count));
}

uint64_t
hg64_rank_of_value(hg64 *hg, uint64_t value) {
	unsigned key = get_key(value);
	unsigned k0 = key - key % PACKSIZE;
	uint64_t rank = 0;

	for(unsigned k = 0; k < k0; k += PACKSIZE) {
		rank += get_subtotal(hg, k);
	}
	for(unsigned k = k0; k < key; k += 1) {
		rank += get_count(hg, k);
	}

	uint64_t count = get_count(hg, key);
	uint64_t min = get_minval(key);
	uint64_t max = get_maxval(key);
	return(interpolate(rank, rank + count, value - min, max - min));
}

uint64_t
hg64_value_at_quantile(hg64 *hg, double q) {
	double rank = (q < 0.0 ? 0.0 : q > 1.0 ? 1.0 : q) * hg->total;
	return(hg64_value_at_rank(hg, (uint64_t)rank));
}

double
hg64_quantile_of_value(hg64 *hg, uint64_t value) {
	uint64_t rank = hg64_rank_of_value(hg, value);
	return((double)rank / (double)hg->total);
}

/**********************************************************************/

void
hg64_mean_variance(hg64 *hg, double *pmean, double *pvar) {
	/* XXX this is not numerically stable */
	double sum = 0.0;
	double squares = 0.0;
	for(unsigned key = 0; key < KEYS; key++) {
		uint64_t value = (get_minval(key) + get_maxval(key)) / 2;
		uint64_t count = get_count(hg, key);
		double total = (double)value * (double)count;
		sum += total;
		squares += total * (double)value;
	}

	double mean = sum / hg->total;
	double square_of_mean = mean * mean;
	double mean_of_squares = squares / hg->total;
	double variance = mean_of_squares - square_of_mean;
	OUTARG(pmean, mean);
	OUTARG(pvar, variance);
}

/**********************************************************************/

static void
validate_value(uint64_t value) {
		unsigned key = get_key(value);
		uint64_t min = get_minval(key);
		uint64_t max = get_maxval(key);
		assert(value >= min);
		assert(value <= max);
}

void
hg64_validate(hg64 *hg) {
	uint64_t min = 0, max = 1ULL << 16, step = 1ULL;
	for(uint64_t value = 0; value < max; value += step) {
		validate_value(value);
	}
	min = 1ULL << 30, max = 1ULL << 40, step = 1ULL << 20;
	for(uint64_t value = min; value < max; value += step) {
		validate_value(value);
	}
	max = UINT64_MAX, min = max >> 8, step = max >> 10;
	for(uint64_t value = max; value > min; value -= step) {
		validate_value(value);
	}
	for(unsigned key = 1; key < KEYS; key++) {
		assert(get_maxval(key - 1) < get_minval(key));
	}

	uint64_t total = 0;
	size_t buckets = 0;
	for(unsigned p = 0; p < PACKS; p++) {
		uint64_t subtotal = 0;
		struct pack *pack = &hg->pack[p];
		unsigned count = popcount(pack->bmp);
		for(unsigned pos = 0; pos < count; pos++) {
			assert(pack->bucket[pos] != 0);
			subtotal += pack->bucket[pos];
		}
		assert((subtotal == 0) == (pack->bucket == NULL));
		assert((subtotal == 0) == (pack->bmp == 0));
		assert(subtotal == pack->total);
		total += subtotal;
		buckets += count;
	}
	assert(hg->total == total);
	assert(hg->buckets == buckets);
}

/**********************************************************************/