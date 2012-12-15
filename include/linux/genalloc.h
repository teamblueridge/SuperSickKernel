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
 * This source code is licensed under the GNU General Public License,
 * Version 2.  See the file COPYING for more details.
 */

#ifndef __GENALLOC_H__
#define __GENALLOC_H__

struct gen_pool;

struct gen_pool *__must_check gen_pool_create(unsigned order, int nid);

void gen_pool_destroy(struct gen_pool *pool);

unsigned long __must_check
gen_pool_alloc_aligned(struct gen_pool *pool, size_t size,
		       unsigned alignment_order);

/**
 * gen_pool_alloc() - allocate special memory from the pool
 * @pool:	Pool to allocate from.
 * @size:	Number of bytes to allocate from the pool.
 *
 * Allocate the requested number of bytes from the specified pool.
 * Uses a first-fit algorithm.
 */
static inline unsigned long __must_check
gen_pool_alloc(struct gen_pool *pool, size_t size)
{
	return gen_pool_alloc_aligned(pool, size, 0);
}

void gen_pool_free(struct gen_pool *pool, unsigned long addr, size_t size);

extern phys_addr_t gen_pool_virt_to_phys(struct gen_pool *pool, unsigned long);
extern int gen_pool_add_virt(struct gen_pool *, unsigned long, phys_addr_t,
			     size_t, int);
/**
 * gen_pool_add - add a new chunk of special memory to the pool
 * @pool: pool to add new memory chunk to
 * @addr: starting address of memory chunk to add to pool
 * @size: size in bytes of the memory chunk to add to pool
 * @nid: node id of the node the chunk structure and bitmap should be
 *       allocated on, or -1
 *
 * Add a new chunk of special memory to the specified pool.
 *
 * Returns 0 on success or a -ve errno on failure.
 */
static inline int __must_check gen_pool_add(struct gen_pool *pool, unsigned long addr,
			       size_t size, int nid)
{
	return gen_pool_add_virt(pool, addr, -1, size, nid);
}
#endif /* __GENALLOC_H__ */
