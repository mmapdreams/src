/*	$OpenBSD$	*/

/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2015-2023 Amazon.com, Inc. or its affiliates.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 * notice, this list of conditions and the following disclaimer in
 * the documentation and/or other materials provided with the
 * distribution.
 * * Neither the name of copyright holder nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
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
 * OpenBSD platform layer for the Amazon ena-com HAL.
 *
 * This is the ONLY file in sys/dev/pci/ena-com/ that contains OpenBSD-specific
 * code; ena_com.c and ena_eth_com.c route every OS interaction through the
 * macros defined here.  Keep the rest of ena-com/ a verbatim copy of the
 * upstream (FreeBSD sys/contrib/ena-com) drop so future HAL updates are a clean
 * re-import.
 */

#ifndef ENA_PLAT_H_
#define ENA_PLAT_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/atomic.h>
#include <sys/device.h>
#include <sys/endian.h>
#include <sys/malloc.h>
#include <sys/mutex.h>
#include <sys/time.h>

#include <machine/bus.h>

#include <net/if.h>

struct ena_softc;
struct ena_com_dev;

/*
 * Logging.  OpenBSD has no device_printf(); ena_dev_name() returns the softc's
 * dv_xname.  The HAL passes its dmadev (== struct ena_softc *) as the context.
 */
enum ena_log_t {
	ENA_ERR = 0,
	ENA_WARN,
	ENA_INFO,
	ENA_DBG,
};

extern int ena_log_level;
const char *ena_dev_name(void *);

#define ena_log(dev, level, fmt, args...)				\
	do {								\
		if (ENA_ ## level <= ena_log_level)			\
			printf("%s: " fmt, ena_dev_name(dev), ##args);	\
	} while (0)

#define ena_log_raw(level, fmt, args...)				\
	do {								\
		if (ENA_ ## level <= ena_log_level)			\
			printf(fmt, ##args);				\
	} while (0)

#define ena_log_unused(dev, level, fmt, args...)			\
	do {								\
		(void)(dev);						\
	} while (0)

#define ena_log_io(dev, level, fmt, args...)				\
	ena_log_unused((dev), level, fmt, ##args)

#define ena_log_nm(dev, level, fmt, args...)				\
	ena_log((dev), level, "[nm] " fmt, ##args)

#define ena_trace(ctx, level, fmt, args...)				\
	ena_log((ctx)->dmadev, level, "%s(): " fmt, __func__, ##args)

#define ena_trc_dbg(ctx, format, arg...)	ena_trace(ctx, DBG, format, ##arg)
#define ena_trc_info(ctx, format, arg...)	ena_trace(ctx, INFO, format, ##arg)
#define ena_trc_warn(ctx, format, arg...)	ena_trace(ctx, WARN, format, ##arg)
#define ena_trc_err(ctx, format, arg...)	ena_trace(ctx, ERR, format, ##arg)

#define ENA_WARN(cond, ctx, format, arg...)				\
	do {								\
		if (unlikely((cond)))					\
			ena_trc_warn(ctx, format, ##arg);		\
	} while (0)

#ifndef unlikely
#define unlikely(x)	__predict_false(!!(x))
#endif
#ifndef likely
#define likely(x)	__predict_true(!!(x))
#endif

#define __iomem
#ifndef CACHE_LINE_SIZE
#define CACHE_LINE_SIZE 64
#endif
#define ____cacheline_aligned __aligned(CACHE_LINE_SIZE)

#define DEFAULT_ALLOC_ALIGNMENT		8
#define ENA_CDESC_RING_SIZE_ALIGNMENT	(1 << 12) /* 4K */

/* Error-pointer helpers used by the HAL's comp-ctx accessors. */
#define MAX_ERRNO 4095
#define IS_ERR_VALUE(x) unlikely((x) <= (unsigned long)MAX_ERRNO)

static inline long
IS_ERR(const void *ptr)
{
	return IS_ERR_VALUE((unsigned long)ptr);
}

static inline void *
ERR_PTR(long error)
{
	return (void *)error;
}

static inline long
PTR_ERR(const void *ptr)
{
	return (long)ptr;
}

/* Bit / mask helpers (Linux-isms the HAL relies on). */
#define GENMASK(h, l)	(((~0U) - (1U << (l)) + 1) & (~0U >> (32 - 1 - (h))))
#define GENMASK_ULL(h, l)	(((~0ULL) << (l)) & (~0ULL >> (64 - 1 - (h))))
#define BIT(x)			(1UL << (x))
#define BIT64(x)		BIT(x)
#define ENA_ABORT()		BUG()
#define BUG()			panic("ENA BUG")

#define SZ_256			(256)
#define SZ_4K			(4096)

#define ENA_COM_OK		0
#define ENA_COM_FAULT		EFAULT
#define ENA_COM_INVAL		EINVAL
#define ENA_COM_NO_MEM		ENOMEM
#define ENA_COM_NO_SPACE	ENOSPC
#define ENA_COM_TRY_AGAIN	-1
#define ENA_COM_UNSUPPORTED	EOPNOTSUPP
#define ENA_COM_NO_DEVICE	ENODEV
#define ENA_COM_PERMISSION	EPERM
#define ENA_COM_TIMER_EXPIRED	ETIMEDOUT
#define ENA_COM_EIO		EIO
#define ENA_COM_DEVICE_BUSY	EBUSY

#define ENA_NODE_ANY		(-1)

/* container_of: portable C, no in-tree global. */
#ifndef container_of
#define container_of(ptr, type, member)					\
	({								\
		const __typeof(((type *)0)->member) *__p = (ptr);	\
		(type *)((uintptr_t)__p - offsetof(type, member));	\
	})
#endif

#define min_t(type, _x, _y) ((type)(_x) < (type)(_y) ? (type)(_x) : (type)(_y))
#define max_t(type, _x, _y) ((type)(_x) > (type)(_y) ? (type)(_x) : (type)(_y))

#define ENA_MIN32(x, y)	MIN(x, y)
#define ENA_MIN16(x, y)	MIN(x, y)
#define ENA_MIN8(x, y)	MIN(x, y)
#define ENA_MAX32(x, y)	MAX(x, y)
#define ENA_MAX16(x, y)	MAX(x, y)
#define ENA_MAX8(x, y)	MAX(x, y)

#define DIV_ROUND_UP(n, d) (((n) + (d) - 1) / (d))

/* Sleep / delay.  DELAY() is a busy-wait safe at any IPL. */
#define ENA_MSLEEP(x)	delay((x) * 1000)
#define ENA_USLEEP(x)	delay(x)
#define ENA_UDELAY(x)	delay(x)
#define ENA_MIGHT_SLEEP()

/*
 * Deadline timers.  ena_time_t is nanoseconds of uptime; the HAL polls device
 * reset / admin completion against these.
 */
typedef uint64_t ena_time_t;
typedef uint64_t ena_time_high_res_t;
typedef struct ifnet ena_netdev;

#define ENA_GET_SYSTEM_TIMEOUT(timeout_us)				\
	(getnsecuptime() + (uint64_t)(timeout_us) * 1000)
#define ENA_TIME_EXPIRE(timeout)					\
	((int64_t)((timeout) - getnsecuptime()) < 0)
#define ENA_GET_SYSTEM_TIME_HIGH_RES()	(getnsecuptime())
#define ENA_GET_SYSTEM_TIMEOUT_HIGH_RES(current_time, timeout_us)	\
	((current_time) + (uint64_t)(timeout_us) * 1000)
#define ENA_TIME_EXPIRE_HIGH_RES(timeout)				\
	((int64_t)((timeout) - getnsecuptime()) < 0)
#define ENA_TIME_INIT_HIGH_RES()	(0)
#define ENA_TIME_COMPARE_HIGH_RES(time1, time2)				\
	((time1) < (time2) ? -1 : ((time1) > (time2) ? 1 : 0))

#define time_after(a, b)						\
	((long)((unsigned long)(b) - (unsigned long)(a)) < 0)

/* Spinlocks: OpenBSD mutex(9) at IPL_NET (admin completion runs at IPL_NET). */
#define ena_spinlock_t	struct mutex
#define ENA_SPINLOCK_INIT(spinlock)					\
	mtx_init(&(spinlock), IPL_NET)
#define ENA_SPINLOCK_DESTROY(spinlock)	((void)(spinlock))
#define ENA_SPINLOCK_LOCK(spinlock, flags)				\
	do {								\
		(void)(flags);						\
		mtx_enter(&(spinlock));					\
	} while (0)
#define ENA_SPINLOCK_UNLOCK(spinlock, flags)				\
	do {								\
		(void)(flags);						\
		mtx_leave(&(spinlock));					\
	} while (0)

/*
 * Wait-events: a mutex plus a "done" flag slept on with msleep_nsec().  The
 * admin completion path (ena_com_handle_admin_completion) calls SIGNAL; the
 * command issuer calls WAIT.  When the device runs admin in polling mode
 * (ena_com_set_admin_polling_mode(true)), the HAL polls instead and never
 * sleeps here -- that is the recommended bring-up mode.
 */
#define ena_wait_event_t	struct { struct mutex mtx; unsigned int done; }
#define ENA_WAIT_EVENT_INIT(waitqueue)					\
	do {								\
		mtx_init(&((waitqueue).mtx), IPL_NET);			\
		(waitqueue).done = 0;					\
	} while (0)
#define ENA_WAIT_EVENTS_DESTROY(admin_queue)	((void)(admin_queue))
#define ENA_WAIT_EVENT_CLEAR(waitqueue)					\
	do { (waitqueue).done = 0; } while (0)
#define ENA_WAIT_EVENT_WAIT(waitqueue, timeout_us)			\
	do {								\
		mtx_enter(&((waitqueue).mtx));				\
		if (!(waitqueue).done) {				\
			msleep_nsec(&(waitqueue), &((waitqueue).mtx),	\
			    PZERO, "enaadm",				\
			    USEC_TO_NSEC((uint64_t)(timeout_us)));	\
		}							\
		(waitqueue).done = 0;					\
		mtx_leave(&((waitqueue).mtx));				\
	} while (0)
#define ENA_WAIT_EVENT_SIGNAL(waitqueue)				\
	do {								\
		mtx_enter(&((waitqueue).mtx));				\
		(waitqueue).done = 1;					\
		wakeup(&(waitqueue));					\
		mtx_leave(&((waitqueue).mtx));				\
	} while (0)

#define dma_addr_t	bus_addr_t
#define u8		uint8_t
#define u16		uint16_t
#define u32		uint32_t
#define u64		uint64_t

/*
 * Coherent DMA handle.  Shaped like the FreeBSD ena_mem_handle_t but tag is
 * always the parent bus_dma_tag (OpenBSD has no per-allocation tag creation).
 */
typedef struct {
	bus_addr_t		paddr;
	caddr_t			vaddr;
	bus_dma_tag_t		tag;
	bus_dmamap_t		map;
	bus_dma_segment_t	seg;
	int			nseg;
} ena_mem_handle_t;

struct ena_bus {
	bus_space_handle_t	reg_bar_h;
	bus_space_tag_t		reg_bar_t;
	bus_space_handle_t	mem_bar_h;
	bus_space_tag_t		mem_bar_t;
};

typedef uint32_t ena_atomic32_t;

#define ENA_PRIu64	"llu"

/* Implemented in if_ena.c. */
int	ena_dma_alloc(void *dmadev, bus_size_t size, ena_mem_handle_t *dma,
	    int mapflags, bus_size_t alignment, int domain);
void	ena_dma_free(void *dmadev, bus_size_t size, ena_mem_handle_t *dma);
void	ena_rss_key_fill(void *key, size_t size);

static inline uint32_t
ena_reg_read32(struct ena_bus *bus, bus_size_t offset)
{
	uint32_t v;

	v = bus_space_read_4(bus->reg_bar_t, bus->reg_bar_h, offset);
	membar_consumer();
	return v;
}

/* Copy a TX header into LLQ device memory 64 bits at a time. */
#define ENA_MEMCPY_TO_DEVICE_64(bus, dst, src, size)			\
	do {								\
		int count, i;						\
		volatile uint64_t *to = (volatile uint64_t *)(dst);	\
		const uint64_t *from = (const uint64_t *)(src);		\
		(void)(bus);						\
		count = (size) / 8;					\
		for (i = 0; i < count; i++, from++, to++)		\
			*to = *from;					\
	} while (0)

#define memcpy_toio memcpy

/* Plain (non-coherent) scratch allocations.  free() takes the size. */
#define ENA_MEM_ALLOC(dmadev, size)					\
	malloc((size), M_DEVBUF, M_NOWAIT | M_ZERO)
#define ENA_MEM_ALLOC_NODE(dmadev, size, virt, node, dev_node)		\
	do {								\
		(void)(node);						\
		(void)(dev_node);					\
		(virt) = malloc((size), M_DEVBUF, M_NOWAIT | M_ZERO);	\
	} while (0)
#define ENA_MEM_FREE(dmadev, ptr, size)					\
	free((ptr), M_DEVBUF, (size))

#define ENA_MEM_ALLOC_COHERENT_NODE_ALIGNED(dmadev, size, virt, phys,	\
    dma, node, dev_node, alignment)					\
	do {								\
		(void)(node);						\
		(void)(dev_node);					\
		if (ena_dma_alloc((dmadev), (size), &(dma), 0,		\
		    (alignment), ENA_NODE_ANY) == 0) {			\
			(virt) = (void *)(dma).vaddr;			\
			(phys) = (dma).paddr;				\
		} else {						\
			(virt) = NULL;					\
		}							\
	} while (0)

#define ENA_MEM_ALLOC_COHERENT_NODE(dmadev, size, virt, phys, handle,	\
    node, dev_node)							\
	ENA_MEM_ALLOC_COHERENT_NODE_ALIGNED(dmadev, size, virt,		\
	    phys, handle, node, dev_node, 8)

#define ENA_MEM_ALLOC_COHERENT_ALIGNED(dmadev, size, virt, phys, dma,	\
    alignment)								\
	do {								\
		if (ena_dma_alloc((dmadev), (size), &(dma), 0,		\
		    (alignment), ENA_NODE_ANY) == 0) {			\
			(virt) = (void *)(dma).vaddr;			\
			(phys) = (dma).paddr;				\
		} else {						\
			(virt) = NULL;					\
		}							\
	} while (0)

#define ENA_MEM_ALLOC_COHERENT(dmadev, size, virt, phys, dma)		\
	ENA_MEM_ALLOC_COHERENT_ALIGNED(dmadev, size, virt, phys, dma, 8)

#define ENA_MEM_FREE_COHERENT(dmadev, size, virt, phys, dma)		\
	do {								\
		ena_dma_free((dmadev), (size), &(dma));			\
		(virt) = NULL;						\
	} while (0)

/*
 * Register R/W against BAR0 via bus_space.
 *
 * The barrier has to be a real store fence rather than a compiler barrier.
 * An LLQ descriptor is written into a write-combining BAR mapping and the
 * doorbell is a separate uncached MMIO store, so without draining the WC
 * buffers first the device can see the doorbell before the descriptors it
 * advertises.  membar_producer() compiles to nothing on amd64.
 */
#define ENA_REG_WRITE32(bus, value, offset)				\
	do {								\
		membar_sync();						\
		ENA_REG_WRITE32_RELAXED(bus, value, offset);		\
	} while (0)

#define ENA_REG_WRITE32_RELAXED(bus, value, offset)			\
	bus_space_write_4(						\
	    ((struct ena_bus *)(bus))->reg_bar_t,			\
	    ((struct ena_bus *)(bus))->reg_bar_h,			\
	    (bus_size_t)(offset), (value))

#define ENA_REG_READ32(bus, offset)					\
	ena_reg_read32((struct ena_bus *)(bus), (bus_size_t)(offset))

/*
 * Admin submission-queue DMA sync.  ena-com calls ENA_DB_SYNC() on the admin
 * SQ before ringing its doorbell (ena_com.c).  Every DMA ring is mapped
 * BUS_DMA_COHERENT (ena_dma_alloc()), so no per-access bus_dmamap_sync is
 * needed for coherency: producer/consumer ordering is supplied by the
 * membar_producer() in ENA_REG_WRITE32() and the HAL's wmb()/dma_rmb().  This
 * sync is therefore a redundant write-buffer drain, retained only because the
 * HAL references the macro.  The cacheable streaming mbuf maps are the only
 * DMA memory that needs real per-access syncs (if_ena.c RX/TX paths).
 */
#define ENA_DB_SYNC_WRITE(mem_handle) bus_dmamap_sync(			\
	(mem_handle)->tag, (mem_handle)->map, 0,			\
	(mem_handle)->map->dm_mapsize, BUS_DMASYNC_PREWRITE)
#define ENA_DB_SYNC_PREREAD(mem_handle) bus_dmamap_sync(		\
	(mem_handle)->tag, (mem_handle)->map, 0,			\
	(mem_handle)->map->dm_mapsize, BUS_DMASYNC_PREREAD)
#define ENA_DB_SYNC_POSTREAD(mem_handle) bus_dmamap_sync(		\
	(mem_handle)->tag, (mem_handle)->map, 0,			\
	(mem_handle)->map->dm_mapsize, BUS_DMASYNC_POSTREAD)
#define ENA_DB_SYNC(mem_handle) ENA_DB_SYNC_WRITE(mem_handle)

#define VLAN_HLEN	sizeof(struct ether_vlan_header)

#define prefetch(x)	(void)(x)
#define prefetchw(x)	(void)(x)

/* DMA unmap accessors used by the HAL bounce-buffer bookkeeping. */
#define dma_unmap_addr(p, name)		((p)->dma->name)
#define dma_unmap_addr_set(p, name, v)	(((p)->dma->name) = (v))
#define dma_unmap_len(p, name)		((p)->name)
#define dma_unmap_len_set(p, name, v)	(((p)->name) = (v))

/* Atomics: plain atomic_* are full barriers on amd64/arm64. */
#define ATOMIC32_INC(I32_PTR)		atomic_inc_int(I32_PTR)
#define ATOMIC32_DEC(I32_PTR)		atomic_dec_int(I32_PTR)
#define ATOMIC32_READ(I32_PTR)		atomic_load_int(I32_PTR)
#define ATOMIC32_SET(I32_PTR, VAL)	atomic_store_int(I32_PTR, VAL)

#define barrier()	__asm volatile("" ::: "memory")
#define dma_rmb()	membar_consumer()
#define mmiowb()	membar_producer()
#define mb()		membar_sync()
/*
 * The HAL calls wmb() to close a bounce buffer before copying it into the
 * write-combining LLQ window, which needs a real store fence for the same
 * reason ENA_REG_WRITE32() does.
 */
#define wmb()		membar_sync()
#define rmb()		membar_consumer()

#ifndef ACCESS_ONCE
#define ACCESS_ONCE(x)	(*(volatile __typeof(x) *)&(x))
#endif
#ifndef READ_ONCE
#define READ_ONCE(x)	({						\
			__typeof(x) __var;				\
			barrier();					\
			__var = ACCESS_ONCE(x);				\
			barrier();					\
			__var;						\
		})
#endif /* READ_ONCE */
#ifndef READ_ONCE8
#define READ_ONCE8(x)	READ_ONCE(x)
#define READ_ONCE16(x)	READ_ONCE(x)
#define READ_ONCE32(x)	READ_ONCE(x)
#endif

#define upper_32_bits(n) ((uint32_t)(((n) >> 16) >> 16))
#define lower_32_bits(n) ((uint32_t)(n))

#define ENA_FFS(x)	ffs(x)

#define ENA_RSS_FILL_KEY(key, size) ena_rss_key_fill(key, size)

#define ENA_FIELD_GET(value, mask, offset) (((value) & (mask)) >> (offset))

#include "ena_defs/ena_includes.h"

#define ENA_BITS_PER_U64(bitmap) (__builtin_popcountll(bitmap))

#define ENA_ADMIN_OS_FREEBSD 4

#endif /* ENA_PLAT_H_ */
