#include <linux/bvec.h>
#include <linux/spinlock_types.h>
#include <linux/percpu-refcount.h>
#include <linux/sched.h>
#include <linux/sched/user.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/net.h>
#include <linux/mm_types.h>
#include <linux/wait.h>
#include <linux/mutex_types.h>
#include <linux/refcount_types.h>
#include <linux/blkdev.h>
#include <linux/poll.h>

#include "internal.h"
#include "io_uring_private.h"

#define IORING_MAX_ENTRIES	4096
#define IORING_MAX_FIXED_FILES	1024

struct io_uring {
	u32 head ____cacheline_aligned_in_smp;
	u32 tail ____cacheline_aligned_in_smp;
};

/*
 * This data is shared with the application through the mmap at offset
 * IORING_OFF_SQ_RING.
 *
 * The offsets to the member fields are published through struct
 * io_sqring_offsets when calling io_uring_setup.
 */
struct io_sq_ring {
	/*
	 * Head and tail offsets into the ring; the offsets need to be
	 * masked to get valid indices.
	 *
	 * The kernel controls head and the application controls tail.
	 */
	struct io_uring		r;
	/*
	 * Bitmask to apply to head and tail offsets (constant, equals
	 * ring_entries - 1)
	 */
	u32			ring_mask;
	/* Ring size (constant, power of 2) */
	u32			ring_entries;
	/*
	 * Number of invalid entries dropped by the kernel due to
	 * invalid index stored in array
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * After a new SQ head value was read by the application this
	 * counter includes all submissions that were dropped reaching
	 * the new SQ head (and possibly more).
	 */
	u32			dropped;
	/*
	 * Runtime flags
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application.
	 *
	 * The application needs a full memory barrier before checking
	 * for IORING_SQ_NEED_WAKEUP after updating the sq tail.
	 */
	u32			flags;
	/*
	 * Ring buffer of indices into array of io_uring_sqe, which is
	 * mmapped by the application using the IORING_OFF_SQES offset.
	 *
	 * This indirection could e.g. be used to assign fixed
	 * io_uring_sqe entries to operations and only submit them to
	 * the queue when needed.
	 *
	 * The kernel modifies neither the indices array nor the entries
	 * array.
	 */
	u32			array[];
};

/*
 * This data is shared with the application through the mmap at offset
 * IORING_OFF_CQ_RING.
 *
 * The offsets to the member fields are published through struct
 * io_cqring_offsets when calling io_uring_setup.
 */
struct io_cq_ring {
	/*
	 * Head and tail offsets into the ring; the offsets need to be
	 * masked to get valid indices.
	 *
	 * The application controls head and the kernel tail.
	 */
	struct io_uring		r;
	/*
	 * Bitmask to apply to head and tail offsets (constant, equals
	 * ring_entries - 1)
	 */
	u32			ring_mask;
	/* Ring size (constant, power of 2) */
	u32			ring_entries;
	/*
	 * Number of completion events lost because the queue was full;
	 * this should be avoided by the application by making sure
	 * there are not more requests pending thatn there is space in
	 * the completion queue.
	 *
	 * Written by the kernel, shouldn't be modified by the
	 * application (i.e. get number of "new events" by comparing to
	 * cached value).
	 *
	 * As completion events come in out of order this counter is not
	 * ordered with any other data.
	 */
	u32			overflow;
	/*
	 * Ring buffer of completion events.
	 *
	 * The kernel writes completion events fresh every time they are
	 * produced, so the application is allowed to modify pending
	 * entries.
	 */
	struct io_uring_cqe	cqes[];
};

struct io_mapped_ubuf {
	u64		ubuf;
	size_t		len;
	struct		bio_vec *bvec;
	unsigned int	nr_bvecs;
};

struct async_list {
	spinlock_t		lock;
	atomic_t		cnt;
	struct list_head	list;

	struct file		*file;
	off_t			io_end;
	size_t			io_pages;
};

struct io_ring_ctx {
	struct {
		struct percpu_ref	refs;
	} ____cacheline_aligned_in_smp;

	struct {
		unsigned int		flags;
		bool			compat;
		bool			account_mem;

		/* SQ ring */
		struct io_sq_ring	*sq_ring;
		unsigned		cached_sq_head;
		unsigned		sq_entries;
		unsigned		sq_mask;
		unsigned		sq_thread_idle;
		struct io_uring_sqe	*sq_sqes;
	} ____cacheline_aligned_in_smp;

	/* IO offload */
	struct workqueue_struct	*sqo_wq;
	struct task_struct	*sqo_thread;	/* if using sq thread polling */
	struct mm_struct	*sqo_mm;
	wait_queue_head_t	sqo_wait;
	unsigned		sqo_stop;

	struct {
		/* CQ ring */
		struct io_cq_ring	*cq_ring;
		unsigned		cached_cq_tail;
		unsigned		cq_entries;
		unsigned		cq_mask;
		struct wait_queue_head	cq_wait;
		struct fasync_struct	*cq_fasync;
	} ____cacheline_aligned_in_smp;

	/*
	 * If used, fixed file set. Writers must ensure that ->refs is dead,
	 * readers must ensure that ->refs is alive as long as the file* is
	 * used. Only updated through io_uring_register(2).
	 */
	struct file		**user_files;
	unsigned		nr_user_files;

	/* if used, fixed mapped user buffers */
	unsigned		nr_user_bufs;
	struct io_mapped_ubuf	*user_bufs;

	struct user_struct	*user;

	struct completion	ctx_done;

	struct {
		struct mutex		uring_lock;
		wait_queue_head_t	wait;
	} ____cacheline_aligned_in_smp;

	struct {
		spinlock_t		completion_lock;
		bool			poll_multi_file;
		/*
		 * ->poll_list is protected by the ctx->uring_lock for
		 * io_uring instances that don't use IORING_SETUP_SQPOLL.
		 * For SQPOLL, only the single threaded io_sq_thread() will
		 * manipulate the list, hence no extra locking is needed there.
		 */
		struct list_head	poll_list;
		struct list_head	cancel_list;
	} ____cacheline_aligned_in_smp;

	struct async_list	pending_async[2];

#if defined(CONFIG_UNIX)
	struct socket		*ring_sock;
#endif
};

struct sqe_submit {
	const struct io_uring_sqe	*sqe;
	unsigned short			index;
	bool				has_user;
	bool				needs_lock;
	bool				needs_fixed_file;
};

/*
 * First field must be the file pointer in all the
 * iocb unions! See also 'struct kiocb' in <linux/fs.h>
 */
struct io_poll_iocb {
	struct file			*file;
	struct wait_queue_head		*head;
	__poll_t			events;
	bool				done;
	bool				canceled;
	struct wait_queue_entry		wait;
};

/*
 * NOTE! Each of the iocb union members has the file pointer
 * as the first entry in their struct definition. So you can
 * access the file pointer through any of the sub-structs,
 * or directly as just 'ki_filp' in this struct.
 */
struct io_kiocb {
	union {
		struct file		*file;
		struct kiocb		rw;
		struct io_poll_iocb	poll;
	};

	struct sqe_submit	submit;

	struct io_ring_ctx	*ctx;
	struct list_head	list;
	unsigned int		flags;
	refcount_t		refs;
#define REQ_F_NOWAIT		1	/* must not punt to workers */
#define REQ_F_IOPOLL_COMPLETED	2	/* polled IO has completed */
#define REQ_F_FIXED_FILE	4	/* ctx owns file */
#define REQ_F_SEQ_PREV		8	/* sequential with previous */
#define REQ_F_PREPPED		16	/* prep already done */
	u64			user_data;
	u64			error;

	struct work_struct	work;
};

#define IO_PLUG_THRESHOLD		2
#define IO_IOPOLL_BATCH			8

struct io_submit_state {
	struct blk_plug		plug;

	/*
	 * io_kiocb alloc cache
	 */
	void			*reqs[IO_IOPOLL_BATCH];
	unsigned		int free_reqs;
	unsigned		int cur_req;

	/*
	 * File reference cache
	 */
	struct file		*file;
	unsigned int		fd;
	unsigned int		has_refs;
	unsigned int		used_refs;
	unsigned int		ios_left;
};

struct io_poll_table {
	struct poll_table_struct pt;
	struct io_kiocb *req;
	int error;
};