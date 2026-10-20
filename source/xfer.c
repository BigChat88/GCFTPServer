#include <gccore.h>

#include "xfer.h"

#define WORKER_STACK (32 * 1024)
#define WORKER_PRIO  (LWP_PRIO_NORMAL - 8)

typedef struct {
	u8 **bufs;
	s32 len[XFER_NBUF];  /* data length; 0 = end marker, <0 = producer error */
	sem_t free_sem;
	sem_t full_sem;

	xfer_produce_fn produce;
	void *produce_ctx;
	xfer_consume_fn consume;
	void *consume_ctx;

	volatile bool consumer_failed;
	bool producer_failed;
	s64 total;
} pipeline_t;

/* Stops after posting an end marker, so the consumer never blocks forever */
static void run_producer(pipeline_t *p)
{
	for (int i = 0;; i = (i + 1) % XFER_NBUF) {
		s32 n;

		LWP_SemWait(p->free_sem);
		n = p->consumer_failed ? 0 : p->produce(p->produce_ctx, p->bufs[i], XFER_CHUNK);
		p->len[i] = n;
		LWP_SemPost(p->full_sem);
		if (n <= 0)
			return;
	}
}

/* Keeps draining after an error until the end marker, so the producer
 * never blocks forever on a full pipeline */
static void run_consumer(pipeline_t *p)
{
	for (int i = 0;; i = (i + 1) % XFER_NBUF) {
		s32 n;

		LWP_SemWait(p->full_sem);
		n = p->len[i];
		if (n <= 0) {
			p->producer_failed = n < 0;
			return;
		}
		if (!p->consumer_failed) {
			if (p->consume(p->consume_ctx, p->bufs[i], n))
				p->total += n;
			else
				p->consumer_failed = true;
		}
		LWP_SemPost(p->free_sem);
	}
}

static void *producer_thread(void *arg)
{
	run_producer(arg);
	return NULL;
}

static void *consumer_thread(void *arg)
{
	run_consumer(arg);
	return NULL;
}

s64 xfer_run(u8 *bufs[XFER_NBUF],
             xfer_produce_fn produce, void *produce_ctx,
             xfer_consume_fn consume, void *consume_ctx,
             bool worker_produces)
{
	pipeline_t p = {
		.bufs = bufs,
		.produce = produce, .produce_ctx = produce_ctx,
		.consume = consume, .consume_ctx = consume_ctx,
	};
	lwp_t worker;

	if (LWP_SemInit(&p.free_sem, XFER_NBUF, XFER_NBUF) != 0)
		return -1;
	if (LWP_SemInit(&p.full_sem, 0, XFER_NBUF) != 0) {
		LWP_SemDestroy(p.free_sem);
		return -1;
	}

	if (LWP_CreateThread(&worker, worker_produces ? producer_thread : consumer_thread,
	                     &p, NULL, WORKER_STACK, WORKER_PRIO) != 0) {
		LWP_SemDestroy(p.free_sem);
		LWP_SemDestroy(p.full_sem);
		return -1;
	}

	if (worker_produces)
		run_consumer(&p);
	else
		run_producer(&p);

	LWP_JoinThread(worker, NULL);
	LWP_SemDestroy(p.free_sem);
	LWP_SemDestroy(p.full_sem);

	return (p.producer_failed || p.consumer_failed) ? -1 : p.total;
}
