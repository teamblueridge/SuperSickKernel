/*
 * Basic general purpose allocator for managing special purpose
 * memory, for example, memory that is not managed by the regular
 * kmalloc/kfree interface.  Uses for this includes on-device special
 * memory, uncached memory etc.
 *
 * It is safe to use the allocator in NMI handlers and other special
 * unblockable contexts that could otherwise deadlock on locks.  This
 * is implemented by using atomic operations and retries on any
 * conflicts.  The disadvantage is that there may be livelocks in
 * extreme cases.  For better scalability, one allocator can be used
 * for each CPU.
 *
 * The lockless operation only works if there is enough memory
 * available.  If new memory is added to the pool a lock has to be
 * still taken.  So any user relying on locklessness has to ensure
 * that sufficient memory is preallocated.
 *
 * The basic atomic operation of this allocator is cmpxchg on long.
 * On architectures that don't have NMI-safe cmpxchg implementation,
 * the allocator can NOT be used in NMI handler.  So code uses the
 * allocator in NMI handler should depend on
 * CONFIG_ARCH_HAVE_NMI_SAFE_CMPXCHG.
 *
 * Copyright 2005 (C) Jes Sorensen <jes@trained-monkey.org>
 *
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#include <linux/slab.h>
#include <linux/module.h>
#include <linux/bitmap.h>
#include <linux/rculist.h>
#include <linux/interrupt.h>
#include <linux/genalloc.h>

static int set_bits_ll(unsigned long *addr, unsigned long mask_to_set)
{
	unsigned long val, nval;

	nval = *addr;
	do {
		val = nval;
		if (val & mask_to_set)
			return -EBUSY;
		cpu_relax();
	} while ((nval = cmpxchg(addr, val, val | mask_to_set)) != val);

	return 0;
}

static int clear_bits_ll(unsigned long *addr, unsigned long mask_to_clear)
{
	unsigned long val, nval;

	nval = *addr;
	do {
		val = nval;
		if ((val & mask_to_clear) != mask_to_clear)
			return -EBUSY;
		cpu_relax();
	} while ((nval = cmpxchg(addr, val, val & ~mask_to_clear)) != val);

	return 0;
}

/*
 * bitmap_set_ll - set the specified number of bits at the specified position
 * @map: pointer to a bitmap
 * @start: a bit position in @map
 * @nr: number of bits to set
 *
 * Set @nr bits start from @start in @map lock-lessly. Several users
 * can set/clear the same bitmap simultaneously without lock. If two
 * users set the same bit, one user will return remain bits, otherwise
 * return 0.
 */
static int bitmap_set_ll(unsigned long *map, int start, int nr)
{
	unsigned long *p = map + BIT_WORD(start);
	const int size = start + nr;
	int bits_to_set = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_set = BITMAP_FIRST_WORD_MASK(start);

	while (nr - bits_to_set >= 0) {
		if (set_bits_ll(p, mask_to_set))
			return nr;
		nr -= bits_to_set;
		bits_to_set = BITS_PER_LONG;
		mask_to_set = ~0UL;
		p++;
	}
	if (nr) {
		mask_to_set &= BITMAP_LAST_WORD_MASK(size);
		if (set_bits_ll(p, mask_to_set))
			return nr;
	}

	return 0;
}

/*
 * bitmap_clear_ll - clear the specified number of bits at the specified position
 * @map: pointer to a bitmap
 * @start: a bit position in @map
 * @nr: number of bits to set
 *
 * Clear @nr bits start from @start in @map lock-lessly. Several users
 * can set/clear the same bitmap simultaneously without lock. If two
 * users clear the same bit, one user will return remain bits,
 * otherwise return 0.
 */
static int bitmap_clear_ll(unsigned long *map, int start, int nr)
{
	unsigned long *p = map + BIT_WORD(start);
	const int size = start + nr;
	int bits_to_clear = BITS_PER_LONG - (start % BITS_PER_LONG);
	unsigned long mask_to_clear = BITMAP_FIRST_WORD_MASK(start);

	while (nr - bits_to_clear >= 0) {
		if (clear_bits_ll(p, mask_to_clear))
			return nr;
		nr -= bits_to_clear;
		bits_to_clear = BITS_PER_LONG;
		mask_to_clear = ~0UL;
		p++;
	}
	if (nr) {
		mask_to_clear &= BITMAP_LAST_WORD_MASK(size);
		if (clear_bits_ll(p, mask_to_clear))
			return nr;
	}

	return 0;
}

/* General purpose special memory pool descriptor. */
struct gen_pool {
	rwlock_t lock;			/* protects chunks list */
	struct list_head chunks;	/* list of chunks in this pool */
	unsigned order;			/* minimum allocation order */
};

/* General purpose special memory pool chunk descriptor. */
struct gen_pool_chunk {
	spinlock_t lock;		/* protects bits */
	struct list_head next_chunk;	/* next chunk in pool */
	phys_addr_t phys_addr;		/* physical starting address of memory chunk */
	unsigned long start;		/* start of memory chunk */
	unsigned long size;		/* number of bits */
	unsigned long bits[0];		/* bitmap for allocating memory chunk */
};

/**
 * gen_pool_create() - create a new special memory pool
 * @order:	Log base 2 of number of bytes each bitmap bit
 *		represents.
 * @nid:	Node id of the node the pool structure should be allocated
 *		on, or -1.  This will be also used for other allocations.
 *
 * Create a new special memory pool that can be used to manage special purpose
 * memory not managed by the regular kmalloc/kfree interface.
 */
struct gen_pool *__must_check gen_pool_create(unsigned order, int nid)
{
	struct gen_pool *pool;

	if (WARN_ON(order >= BITS_PER_LONG))
		return NULL;

	pool = kmalloc_node(sizeof *pool, GFP_KERNEL, nid);
	if (pool) {
		rwlock_init(&pool->lock);
		INIT_LIST_HEAD(&pool->chunks);
		pool->order = order;
	}
	return pool;
}
EXPORT_SYMBOL(gen_pool_create);

/**
 * gen_pool_add_virt - add a new chunk of special memory to the pool
 * @pool: pool to add new memory chunk to
 * @virt: virtual starting address of memory chunk to add to pool
 * @phys: physical starting address of memory chunk to add to pool
 * @size: size in bytes of the memory chunk to add to pool
 * @nid: node id of the node the chunk structure and bitmap should be
 *       allocated on, or -1
 *
 * Add a new chunk of special memory to the specified pool.
 *
 * Returns 0 on success or a -ve errno on failure.
 */
int __must_check gen_pool_add_virt(struct gen_pool *pool, unsigned long virt, phys_addr_t phys,
		 size_t size, int nid)
{
	struct gen_pool_chunk *chunk;
	size_t nbytes;

	if (WARN_ON(!virt || virt + size < virt ||
	    (virt & ((1 << pool->order) - 1))))
		return -EINVAL;

	size = size >> pool->order;
	if (WARN_ON(!size))
		return -EINVAL;

	nbytes = sizeof *chunk + BITS_TO_LONGS(size) * sizeof *chunk->bits;
	chunk = kzalloc_node(nbytes, GFP_KERNEL, nid);
	if (!chunk)
		return -ENOMEM;

	spin_lock_init(&chunk->lock);
	chunk->phys_addr = phys;
	chunk->start = virt >> pool->order;
	chunk->size  = size;

	write_lock(&pool->lock);
	list_add(&chunk->next_chunk, &pool->chunks);
	write_unlock(&pool->lock);

	return 0;
}
EXPORT_SYMBOL(gen_pool_add_virt);

/**
 * gen_pool_virt_to_phys - return the physical address of memory
 * @pool: pool to allocate from
 * @addr: starting address of memory
 *
 * Returns the physical address on success, or -1 on error.
 */
phys_addr_t gen_pool_virt_to_phys(struct gen_pool *pool, unsigned long addr)
{
	struct list_head *_chunk;
	struct gen_pool_chunk *chunk;

	read_lock(&pool->lock);
	list_for_each(_chunk, &pool->chunks) {
		chunk = list_entry(_chunk, struct gen_pool_chunk, next_chunk);

		if (addr >= chunk->start &&
		    addr < (chunk->start + chunk->size))
			return chunk->phys_addr + addr - chunk->start;
	}
	read_unlock(&pool->lock);

	return -1;
}
EXPORT_SYMBOL(gen_pool_virt_to_phys);

/**
 * gen_pool_destroy() - destroy a special memory pool
 * @pool:	Pool to destroy.
 *
 * Destroy the specified special memory pool. Verifies that there are no
 * outstanding allocations.
 */
void gen_pool_destroy(struct gen_pool *pool)
{
	struct gen_pool_chunk *chunk;
	int bit;

	while (!list_empty(&pool->chunks)) {
		chunk = list_entry(pool->chunks.next, struct gen_pool_chunk,
				   next_chunk);
		list_del(&chunk->next_chunk);

		bit = find_next_bit(chunk->bits, chunk->size, 0);
		BUG_ON(bit < chunk->size);

		kfree(chunk);
	}
	kfree(pool);
}
EXPORT_SYMBOL(gen_pool_destroy);

/**
 * gen_pool_alloc_aligned() - allocate special memory from the pool
 * @pool:	Pool to allocate from.
 * @size:	Number of bytes to allocate from the pool.
 * @alignment_order:	Order the allocated space should be
 *			aligned to (eg. 20 means allocated space
 *			must be aligned to 1MiB).
 *
 * Allocate the requested number of bytes from the specified pool.
 * Uses a first-fit algorithm.
 */
unsigned long __must_check
gen_pool_alloc_aligned(struct gen_pool *pool, size_t size,
		       unsigned alignment_order)
{
	unsigned long addr, align_mask = 0, flags, start;
	struct gen_pool_chunk *chunk;

	if (size == 0)
		return 0;

	if (alignment_order > pool->order)
		align_mask = (1 << (alignment_order - pool->order)) - 1;

	size = (size + (1UL << pool->order) - 1) >> pool->order;

	read_lock(&pool->lock);
	list_for_each_entry(chunk, &pool->chunks, next_chunk) {
		if (chunk->size < size)
			continue;

		spin_lock_irqsave(&chunk->lock, flags);
		start = bitmap_find_next_zero_area_off(chunk->bits, chunk->size,
						       0, size, align_mask,
						       chunk->start);
		if (start >= chunk->size) {
			spin_unlock_irqrestore(&chunk->lock, flags);
			continue;
		}

		bitmap_set(chunk->bits, start, size);
		spin_unlock_irqrestore(&chunk->lock, flags);
		addr = (chunk->start + start) << pool->order;
		goto done;
	}

	addr = 0;
done:
	read_unlock(&pool->lock);
	return addr;
}
EXPORT_SYMBOL(gen_pool_alloc_aligned);

/**
 * gen_pool_free() - free allocated special memory back to the pool
 * @pool:	Pool to free to.
 * @addr:	Starting address of memory to free back to pool.
 * @size:	Size in bytes of memory to free.
 *
 * Free previously allocated special memory back to the specified pool.
 */
void gen_pool_free(struct gen_pool *pool, unsigned long addr, size_t size)
{
	struct gen_pool_chunk *chunk;
	unsigned long flags;

	if (!size)
		return;

	addr = addr >> pool->order;
	size = (size + (1UL << pool->order) - 1) >> pool->order;

	BUG_ON(addr + size < addr);

	read_lock(&pool->lock);
	list_for_each_entry(chunk, &pool->chunks, next_chunk)
		if (addr >= chunk->start &&
		    addr + size <= chunk->start + chunk->size) {
			spin_lock_irqsave(&chunk->lock, flags);
			bitmap_clear(chunk->bits, addr - chunk->start, size);
			spin_unlock_irqrestore(&chunk->lock, flags);
			goto done;
		}
	BUG_ON(1);
done:
	read_unlock(&pool->lock);
}
EXPORT_SYMBOL(gen_pool_free);
