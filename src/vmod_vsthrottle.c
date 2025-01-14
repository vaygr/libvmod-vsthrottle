/*-
 * Copyright (c) 2013-2015 Varnish Software Group
 * All rights reserved.
 *
 * Author: Dag Haavi Finstad <daghf@varnish-software.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "config.h"

#include <stdlib.h>

#if !defined(HAVE_CLOCK_GETTIME)
# if defined(__APPLE__) && defined(__MACH__)
#  include <mach/clock.h>
#  include <mach/mach.h>
#  define CLOCK_MONOTONIC SYSTEM_CLOCK
# else
#  error Unsupported platform
# endif
#endif

#include "vcl.h"
#include "vrt.h"
#include "cache/cache.h"
#include "vsha256.h"

#include "vtree.h"

#include "vcc_if.h"

/* Represents a token bucket for a specific key. */
struct tbucket {
	unsigned		magic;
#define TBUCKET_MAGIC		0x53345eb9
	unsigned char		digest[SHA256_LEN];
	double			last_used;
	double			period;
	long			tokens;
	long			capacity;
	VRB_ENTRY(tbucket)	tree;
};

static int
keycmp(const struct tbucket *b1, const struct tbucket *b2)
{
	return (memcmp(b1->digest, b2->digest, sizeof b1->digest));
}

VRB_HEAD(tbtree, tbucket);
VRB_PROTOTYPE_STATIC(tbtree, tbucket, tree, keycmp);
VRB_GENERATE_STATIC(tbtree, tbucket, tree, keycmp);

/* To lessen potential mutex contention, we partition the buckets into
   N_PART partitions.  */
#define N_PART		16 /* must be 2^n */
#define N_PART_MASK	(N_PART - 1)

static unsigned n_init;
static pthread_mutex_t init_mtx = PTHREAD_MUTEX_INITIALIZER;

/* GC_INTVL: How often (in #calls per partition) we invoke the garbage
   collector. */
#define GC_INTVL	1000
static void run_gc(double now, unsigned part);

static struct vsthrottle {
	unsigned		magic;
#define VSTHROTTLE_MAGIC	0x99fdbef8
	pthread_mutex_t		mtx;
	struct tbtree		buckets;
	unsigned		gc_count;
} vsthrottle[N_PART];

#if !defined(HAVE_CLOCK_GETTIME)
#if defined(__APPLE__) && defined(__MACH__)
int
clock_gettime(clock_id_t clk_id, struct timespec *ts)
{
	kern_return_t retval = KERN_SUCCESS;
	clock_serv_t cclock;
	mach_timespec_t mts;

	host_get_clock_service(mach_host_self(), clk_id, &cclock);
	retval = clock_get_time(cclock, &mts);
	mach_port_deallocate(mach_task_self(), cclock);
	ts->tv_sec = mts.tv_sec;
	ts->tv_nsec = mts.tv_nsec;
	return (retval);
}
#endif
#endif

static struct tbucket *
tb_alloc(const unsigned char *digest, long limit, double period, double now)
{
	struct tbucket *tb = malloc(sizeof *tb);
	AN(tb);

	memcpy(tb->digest, digest, sizeof tb->digest);
	tb->magic = TBUCKET_MAGIC;
	tb->last_used = now;
	tb->period = period;
	tb->tokens = limit;
	tb->capacity = limit;

	return (tb);
}

static struct tbucket *
get_bucket(const unsigned char *digest, long limit, double period, double now)
{
	struct tbucket *b;
	struct tbucket k = { 0 };
	unsigned part = digest[0] & N_PART_MASK;
	struct vsthrottle *v = &vsthrottle[part];

	memcpy(&k.digest, digest, sizeof k.digest);
	b = VRB_FIND(tbtree, &v->buckets, &k);
	if (b) {
		CHECK_OBJ_NOTNULL(b, TBUCKET_MAGIC);
	} else {
		b = tb_alloc(digest, limit, period, now);
		AZ(VRB_INSERT(tbtree, &v->buckets, b));
	}
	return (b);
}

static void
calc_tokens(struct tbucket *b, double now)
{
	double delta = now - b->last_used;
	assert(delta >= 0);

	b->tokens += (long) ((delta / b->period) * b->capacity);
	if (b->tokens > b->capacity)
		b->tokens = b->capacity;
	/* VSL(SLT_VCL_Log, 0, "tokens: %ld", b->tokens); */
}

static double
get_ts_mono(void)
{
	struct timespec ts;
	AZ(clock_gettime(CLOCK_MONOTONIC, &ts));
	return (ts.tv_sec + 1e-9 * ts.tv_nsec);
}

VCL_BOOL
vmod_is_denied(VRT_CTX, VCL_STRING key, VCL_INT limit, VCL_DURATION period)
{
	unsigned ret = 1;
	struct tbucket *b;
	double now;
	SHA256_CTX sctx;
	struct vsthrottle *v;
	unsigned char digest[SHA256_LEN];
	unsigned part;

	(void)ctx;

	if (!key)
		return (1);

	SHA256_Init(&sctx);
	SHA256_Update(&sctx, key, strlen(key));
	SHA256_Update(&sctx, &limit, sizeof (limit));
	SHA256_Update(&sctx, &period, sizeof (period));
	SHA256_Final(digest, &sctx);

	part = digest[0] & N_PART_MASK;
	v = &vsthrottle[part];
	AZ(pthread_mutex_lock(&v->mtx));
	now = get_ts_mono();
	b = get_bucket(digest, limit, period, now);
	calc_tokens(b, now);
	if (b->tokens > 0) {
		b->tokens -= 1;
		ret = 0;
		b->last_used = now;
	}

	v->gc_count++;
	if (v->gc_count == GC_INTVL) {
		run_gc(now, part);
		v->gc_count = 0;
	}

	AZ(pthread_mutex_unlock(&v->mtx));
	return (ret);
}

/* Clean up expired entries. */
static void
run_gc(double now, unsigned part)
{
	struct tbucket *x, *y;
	struct tbtree *buckets = &vsthrottle[part].buckets;

	/* XXX: Assert mtx is held ... */
	VRB_FOREACH_SAFE(x, tbtree, buckets, y) {
		CHECK_OBJ_NOTNULL(x, TBUCKET_MAGIC);
		if (now - x->last_used > x->period) {
			VRB_REMOVE(tbtree, buckets, x);
			free(x);
		}
	}
}

static void
fini(void *priv)
{
	assert(priv == &n_init);

	AZ(pthread_mutex_lock(&init_mtx));
	assert(n_init > 0);
	n_init--;
	if (n_init == 0) {
		struct tbucket *x, *y;
		unsigned p;

		for (p = 0; p < N_PART; ++p ) {
			struct vsthrottle *v = &vsthrottle[p];
			VRB_FOREACH_SAFE(x, tbtree, &v->buckets, y) {
				CHECK_OBJ_NOTNULL(x, TBUCKET_MAGIC);
				VRB_REMOVE(tbtree, &v->buckets, x);
				free(x);
			}
		}
	}
	AZ(pthread_mutex_unlock(&init_mtx));
}

int
event_function(VRT_CTX, struct vmod_priv *priv, enum vcl_event_e e)
{
	(void)ctx;

	if (e != VCL_EVENT_LOAD)
		return (0);

	priv->priv = &n_init;
	priv->free = fini;
	AZ(pthread_mutex_lock(&init_mtx));
	if (n_init == 0) {
		unsigned p;
		for (p = 0; p < N_PART; ++p) {
			struct vsthrottle *v = &vsthrottle[p];
			v->magic = VSTHROTTLE_MAGIC;
			AZ(pthread_mutex_init(&v->mtx, NULL));
			VRB_INIT(&v->buckets);
		}
	}
	n_init++;
	AZ(pthread_mutex_unlock(&init_mtx));
	return (0);
}
