#include <linux/compiler.h>
#include <asm-generic/barrier.h>

#include "io_uring_public.h"

static void io_commit_sqring(struct io_ring_ctx *ctx)
{
	struct io_sq_ring *ring = ctx->sq_ring;

	if (ctx->cached_sq_head != READ_ONCE(ring->r.head)) {
		/*
		 * Ensure any loads from the SQEs are done at this point,
		 * since once we write the new head, the application could
		 * write new data to them.
		 */
		smp_store_release(&ring->r.head, ctx->cached_sq_head);
	}
}