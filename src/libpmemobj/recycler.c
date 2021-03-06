/*
 * Copyright 2016-2018, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * recycler.c -- implementation of run recycler
 */

#include "heap.h"
#include "recycler.h"
#include "vec.h"
#include "out.h"
#include "util.h"
#include "sys_util.h"
#include "ravl.h"
#include "valgrind_internal.h"

#define THRESHOLD_MUL 2

/*
 * recycler_element_cmp -- compares two recycler elements
 */
static int
recycler_element_cmp(const void *lhs, const void *rhs)
{
	const struct recycler_element *l = lhs;
	const struct recycler_element *r = rhs;

	int64_t diff = (int64_t)l->max_free_block - (int64_t)r->max_free_block;
	if (diff != 0)
		return diff > 0 ? 1 : -1;

	diff = (int64_t)l->free_space - (int64_t)r->free_space;
	if (diff != 0)
		return diff > 0 ? 1 : -1;

	diff = (int64_t)l->zone_id - (int64_t)r->zone_id;
	if (diff != 0)
		return diff > 0 ? 1 : -1;

	diff = (int64_t)l->chunk_id - (int64_t)r->chunk_id;
	if (diff != 0)
		return diff > 0 ? 1 : -1;

	return 0;
}

struct recycler {
	struct ravl *runs;
	struct palloc_heap *heap;

	/*
	 * How many unaccounted units there *might* be inside of the memory
	 * blocks stored in the recycler.
	 * The value is not meant to be accurate, but rather a rough measure on
	 * how often should the memory block scores be recalculated.
	 */
	size_t unaccounted_units;
	size_t nallocs;
	size_t recalc_threshold;
	int recalc_inprogress;

	VEC(, struct recycler_element) recalc;
	VEC(, struct memory_block_reserved *) pending;

	os_mutex_t lock;
};

/*
 * recycler_new -- creates new recycler instance
 */
struct recycler *
recycler_new(struct palloc_heap *heap, size_t nallocs)
{
	struct recycler *r = Malloc(sizeof(struct recycler));
	if (r == NULL)
		goto error_alloc_recycler;

	r->runs = ravl_new_sized(recycler_element_cmp,
		sizeof(struct recycler_element));
	if (r->runs == NULL)
		goto error_alloc_tree;

	r->heap = heap;
	r->nallocs = nallocs;
	r->recalc_threshold = nallocs * THRESHOLD_MUL;
	r->unaccounted_units = 0;
	r->recalc_inprogress = 0;
	VEC_INIT(&r->recalc);
	VEC_INIT(&r->pending);

	os_mutex_init(&r->lock);

	return r;

error_alloc_tree:
	Free(r);
error_alloc_recycler:
	return NULL;
}

/*
 * recycler_delete -- deletes recycler instance
 */
void
recycler_delete(struct recycler *r)
{
	VEC_DELETE(&r->recalc);

	struct memory_block_reserved *mr;
	VEC_FOREACH(mr, &r->pending) {
		Free(mr);
	}
	VEC_DELETE(&r->pending);
	os_mutex_destroy(&r->lock);
	ravl_delete(r->runs);
	Free(r);
}

/*
 * recycler_element_new -- calculates how many free bytes does a run have and
 *	what's the largest request that the run can handle, returns that as
 *	recycler element struct
 */
struct recycler_element
recycler_element_new(struct palloc_heap *heap, const struct memory_block *m)
{
	/*
	 * Counting of the clear bits can race with a concurrent deallocation
	 * that operates on the same run. This race is benign and has absolutely
	 * no effect on the correctness of this algorithm. Ideally, we would
	 * avoid grabbing the lock, but helgrind gets very confused if we
	 * try to disable reporting for this function.
	 */
	os_mutex_t *lock = m->m_ops->get_lock(m);
	util_mutex_lock(lock);

	struct chunk_run *run = heap_get_chunk_run(heap, m);

	uint16_t free_space = 0;
	uint16_t max_block = 0;

	for (int i = 0; i < MAX_BITMAP_VALUES; ++i) {
		uint64_t value = ~run->bitmap[i];
		if (value == 0)
			continue;

		uint16_t free_in_value = util_popcount64(value);
		free_space = (uint16_t)(free_space + free_in_value);

		/*
		 * If this value has less free blocks than already found max,
		 * there's no point in searching.
		 */
		if (free_in_value < max_block)
			continue;

		/* if the entire value is empty, no point in searching */
		if (free_in_value == BITS_PER_VALUE) {
			max_block = BITS_PER_VALUE;
			continue;
		}

		/*
		 * Find the biggest free block in the bitmap.
		 * This algorithm is not the most clever imaginable, but it's
		 * easy to implement and fast enough.
		 */
		uint16_t n = 0;
		while (value != 0) {
			value &= (value << 1ULL);
			n++;
		}

		if (n > max_block)
			max_block = n;
	}

	util_mutex_unlock(lock);

	return (struct recycler_element){
		.free_space = free_space,
		.max_free_block = max_block,
		.chunk_id = m->chunk_id,
		.zone_id = m->zone_id,
	};
}

/*
 * recycler_put -- inserts new run into the recycler
 */
int
recycler_put(struct recycler *r, const struct memory_block *m,
	struct recycler_element element)
{
	int ret = 0;

	util_mutex_lock(&r->lock);

	ret = ravl_emplace_copy(r->runs, &element);

	util_mutex_unlock(&r->lock);

	return ret;
}

/*
 * recycler_pending_check -- iterates through pending memory blocks, checks
 *	the reservation status, and puts it in the recycler if the there
 *	are no more unfulfilled reservations for the block.
 */
static void
recycler_pending_check(struct recycler *r)
{
	struct memory_block_reserved *mr = NULL;
	size_t pos;
	VEC_FOREACH_BY_POS(pos, &r->pending) {
		mr = VEC_ARR(&r->pending)[pos];
		if (mr->nresv == 0) {
			struct recycler_element e = recycler_element_new(
				r->heap, &mr->m);
			if (ravl_emplace_copy(r->runs, &e) != 0) {
				ERR("unable to track run %u due to OOM",
					mr->m.chunk_id);
			}
			Free(mr);
			VEC_ERASE_BY_POS(&r->pending, pos);
		}
	}
}

/*
 * recycler_get -- retrieves a chunk from the recycler
 */
int
recycler_get(struct recycler *r, struct memory_block *m)
{
	int ret = 0;

	util_mutex_lock(&r->lock);

	recycler_pending_check(r);

	struct recycler_element e = { .max_free_block = m->size_idx, 0, 0, 0};
	struct ravl_node *n = ravl_find(r->runs, &e,
		RAVL_PREDICATE_GREATER_EQUAL);
	if (n == NULL) {
		ret = ENOMEM;
		goto out;
	}

	struct recycler_element *ne = ravl_data(n);
	m->chunk_id = ne->chunk_id;
	m->zone_id = ne->zone_id;

	ravl_remove(r->runs, n);

	struct chunk_header *hdr = heap_get_chunk_hdr(r->heap, m);
	m->size_idx = hdr->size_idx;

	memblock_rebuild_state(r->heap, m);

out:
	util_mutex_unlock(&r->lock);

	return ret;
}

/*
 * recycler_pending_put -- places the memory block in the pending container
 */
void
recycler_pending_put(struct recycler *r,
	struct memory_block_reserved *m)
{
	if (VEC_PUSH_BACK(&r->pending, m) != 0)
		ASSERT(0); /* XXX: fix after refactoring */
}

/*
 * recycler_recalc -- recalculates the scores of runs in the recycler to match
 *	the updated persistent state
 */
struct empty_runs
recycler_recalc(struct recycler *r, int force)
{
	struct empty_runs runs;
	VEC_INIT(&runs);

	uint64_t units = r->unaccounted_units;

	if (r->recalc_inprogress || (!force && units < (r->recalc_threshold)))
		return runs;

	if (!util_bool_compare_and_swap32(&r->recalc_inprogress, 0, 1))
		return runs;

	util_mutex_lock(&r->lock);

	/* If the search is forced, recalculate everything */
	uint64_t search_limit = force ? UINT64_MAX : units;

	uint64_t found_units = 0;
	struct memory_block nm = MEMORY_BLOCK_NONE;
	struct ravl_node *n;
	struct recycler_element empty = {0, 0, 0, 0};
	do {
		if ((n = ravl_find(r->runs, &empty,
			RAVL_PREDICATE_GREATER_EQUAL)) == NULL)
			break;

		struct recycler_element *ne = ravl_data(n);
		nm.chunk_id = ne->chunk_id;
		nm.zone_id = ne->zone_id;

		uint32_t existing_free_space = ne->free_space;

		ravl_remove(r->runs, n);

		memblock_rebuild_state(r->heap, &nm);

		struct recycler_element e = recycler_element_new(r->heap, &nm);

		ASSERT(e.free_space >= existing_free_space);
		uint64_t free_space_diff = e.free_space - existing_free_space;
		found_units += free_space_diff;

		if (e.free_space == r->nallocs) {
			memblock_rebuild_state(r->heap, &nm);
			if (VEC_PUSH_BACK(&runs, nm) != 0)
				ASSERT(0); /* XXX: fix after refactoring */
		} else {
			VEC_PUSH_BACK(&r->recalc, e);
		}
	} while (found_units < search_limit);

	struct recycler_element *e;
	VEC_FOREACH_BY_PTR(e, &r->recalc) {
		ravl_emplace_copy(r->runs, e);
	}

	VEC_CLEAR(&r->recalc);

	util_mutex_unlock(&r->lock);

	util_fetch_and_sub64(&r->unaccounted_units, units);
	int ret = util_bool_compare_and_swap32(&r->recalc_inprogress, 1, 0);
	ASSERTeq(ret, 1);

	return runs;
}

/*
 * recycler_inc_unaccounted -- increases the number of unaccounted units in the
 *	recycler
 */
void
recycler_inc_unaccounted(struct recycler *r, const struct memory_block *m)
{
	util_fetch_and_add64(&r->unaccounted_units, m->size_idx);
}
