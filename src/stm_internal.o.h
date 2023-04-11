/*
 * File:
 *   stm_internal.h
 * Author(s):
 *   Pascal Felber <pascal.felber@unine.ch>
 *   Patrick Marlier <patrick.marlier@unine.ch>
 * Description:
 *   STM internal functions.
 *
 * Copyright (c) 2007-2014.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation, version 2
 * of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * This program has a dual license and can also be distributed
 * under the terms of the MIT license.
 */

#ifndef _STM_INTERNAL_H_
#define _STM_INTERNAL_H_

#include <pthread.h>
#include <string.h>
#include <stm.h>
#include "tls.h"
#include "utils.h"
#include "atomic.h"
#include "gc.h"

/* ################################################################### *
 * DEFINES
 * ################################################################### */

/* Designs */
#define WRITE_BACK_ETL                  0
#define WRITE_BACK_CTL                  1
#define WRITE_THROUGH                   2
#define MODULAR                         3


/* Contention managers */
#define CM_SUICIDE                      0
#define CM_DELAY                        1
#define CM_BACKOFF                      2
#define CM_MODULAR                      3







#define TX_GET                          stm_tx_t *tx = tls_get_tx()

#ifndef RW_SET_SIZE
# define RW_SET_SIZE                    4096                /* Initial size of read/write sets */
#endif /* ! RW_SET_SIZE */

#ifndef LOCK_ARRAY_LOG_SIZE
# define LOCK_ARRAY_LOG_SIZE            20                  /* Size of lock array: 2^20 = 1M */
#endif /* LOCK_ARRAY_LOG_SIZE */

#ifndef LOCK_SHIFT_EXTRA
# define LOCK_SHIFT_EXTRA               2                   /* 2 extra shift */
#endif /* LOCK_SHIFT_EXTRA */



#define NO_SIGNAL_HANDLER               "NO_SIGNAL_HANDLER"

#if defined(CTX_LONGJMP)
# define JMP_BUF                        jmp_buf
# define LONGJMP(ctx, value)            longjmp(ctx, value)
#elif defined(CTX_ITM)
/* TODO adjust size to real size. */
# define JMP_BUF                        jmp_buf
# define LONGJMP(ctx, value)            CTX_ITM(value, ctx)
#else /* !CTX_LONGJMP && !CTX_ITM */
# define JMP_BUF                        sigjmp_buf
# define LONGJMP(ctx, value)            siglongjmp(ctx, value)
#endif /* !CTX_LONGJMP && !CTX_ITM */


/* ################################################################### *
 * TYPES
 * ################################################################### */

enum {                                  /* Transaction status */
  TX_IDLE = 0,
  TX_ACTIVE = 1,                        /* Lowest bit indicates activity */
  TX_COMMITTED = (1 << 1),
  TX_ABORTED = (2 << 1),
  TX_COMMITTING = (1 << 1) | TX_ACTIVE,
  TX_ABORTING = (2 << 1) | TX_ACTIVE,
  TX_KILLED = (3 << 1) | TX_ACTIVE,
  TX_IRREVOCABLE = 0x08 | TX_ACTIVE     /* Fourth bit indicates irrevocability */
};
#define STATUS_BITS                     4
#define STATUS_MASK                     ((1 << STATUS_BITS) - 1)

# define SET_STATUS(s, v)               ((s) = (v))
# define UPDATE_STATUS(s, v)            ((s) = (v))
# define GET_STATUS(s)                  ((s))
#define IS_ACTIVE(s)                    ((GET_STATUS(s) & 0x01) == TX_ACTIVE)

/* ################################################################### *
 * LOCKS
 * ################################################################### */

/*
 * A lock is a unsigned integer of the size of a pointer.
 * The LSB is the lock bit. If it is set, this means:
 * - At least some covered memory addresses is being written.
 * - All bits of the lock apart from the lock bit form
 *   a pointer that points to the write log entry holding the new
 *   value. Multiple values covered by the same log entry and orginized
 *   in a linked list in the write log.
 * If the lock bit is not set, then:
 * - All covered memory addresses contain consistent values.
 * - All bits of the lock besides the lock bit contain a version number
 *   (timestamp).
 *   - The high order bits contain the commit time.
 *   - The low order bits contain an incarnation number (incremented
 *     upon abort while writing the covered memory addresses).
 * When visible reads are enabled, two bits are used as read and write
 * locks. A read-locked address can be read by an invisible reader.
 */

# define OWNED_BITS                     1                   /* 1 bit */
# define WRITE_MASK                     0x01                /* 1 bit */
# define OWNED_MASK                     (WRITE_MASK)
#define INCARNATION_BITS                3                   /* 3 bits */
#define INCARNATION_MAX                 ((1 << INCARNATION_BITS) - 1)
#define INCARNATION_MASK                (INCARNATION_MAX << 1)
#define LOCK_BITS                       (OWNED_BITS + INCARNATION_BITS)
#define MAX_THREADS                     8192                /* Upper bound (large enough) */
#define VERSION_MAX                     ((~(stm_word_t)0 >> LOCK_BITS) - MAX_THREADS)

#define LOCK_GET_OWNED(l)               (l & OWNED_MASK)
#define LOCK_GET_WRITE(l)               (l & WRITE_MASK)
#define LOCK_SET_ADDR_WRITE(a)          (a | WRITE_MASK)    /* WRITE bit set */
#define LOCK_GET_ADDR(l)                (l & ~(stm_word_t)OWNED_MASK)
#define LOCK_GET_TIMESTAMP(l)           (l >> (LOCK_BITS))
#define LOCK_SET_TIMESTAMP(t)           (t << (LOCK_BITS))
#define LOCK_GET_INCARNATION(l)         ((l & INCARNATION_MASK) >> OWNED_BITS)
#define LOCK_SET_INCARNATION(i)         (i << OWNED_BITS)   /* OWNED bit not set */
#define LOCK_UPD_INCARNATION(l, i)      ((l & ~(stm_word_t)(INCARNATION_MASK | OWNED_MASK)) | LOCK_SET_INCARNATION(i))

/*
 * We use the very same hash functions as TL2 for degenerate Bloom
 * filters on 32 bits.
 */

/*
 * We use an array of locks and hash the address to find the location of the lock.
 * We try to avoid collisions as much as possible (two addresses covered by the same lock).
 */
#define LOCK_ARRAY_SIZE                 (1 << LOCK_ARRAY_LOG_SIZE)
#define LOCK_MASK                       (LOCK_ARRAY_SIZE - 1)
#define LOCK_SHIFT                      (((sizeof(stm_word_t) == 4) ? 2 : 3) + LOCK_SHIFT_EXTRA)
#define LOCK_IDX(a)                     (((stm_word_t)(a) >> LOCK_SHIFT) & LOCK_MASK)
# define GET_LOCK(a)                    (_tinystm.locks + LOCK_IDX(a))

/* ################################################################### *
 * CLOCK
 * ################################################################### */

/* At least twice a cache line (not required if properly aligned and padded) */
#define CLOCK                           (_tinystm.gclock[(CACHELINE_SIZE * 2) / sizeof(stm_word_t)])

#define GET_CLOCK                       (ATOMIC_LOAD_ACQ(&CLOCK))
#define FETCH_INC_CLOCK                 (ATOMIC_FETCH_INC_FULL(&CLOCK))

/* ################################################################### *
 * CALLBACKS
 * ################################################################### */

/* The number of 7 is chosen to make fit the number and the array in a
 * same cacheline (assuming 64bytes cacheline and 64bits CPU). */
#define MAX_CB                          7

/* Declare as static arrays (vs. lists) to improve cache locality */
/* The number transaction local specific for modules. */
#ifndef MAX_SPECIFIC
# define MAX_SPECIFIC                   7
#endif /* MAX_SPECIFIC */


typedef struct r_entry {                /* Read set entry */
  stm_word_t version;                   /* Version read */
  volatile stm_word_t *lock;            /* Pointer to lock (for fast access) */
} r_entry_t;

typedef struct r_set {                  /* Read set */
  r_entry_t *entries;                   /* Array of entries */
  unsigned int nb_entries;              /* Number of entries */
  unsigned int size;                    /* Size of array */
} r_set_t;

typedef struct w_entry {                /* Write set entry */
  union {                               /* For padding... */
    struct {
      volatile stm_word_t *addr;        /* Address written */
      stm_word_t value;                 /* New (write-back) or old (write-through) value */
      stm_word_t mask;                  /* Write mask */
      stm_word_t version;               /* Version overwritten */
      volatile stm_word_t *lock;        /* Pointer to lock (for fast access) */
      union {
        struct w_entry *next;           /* WRITE_BACK_ETL || WRITE_THROUGH: Next address covered by same lock (if any) */
        stm_word_t no_drop;             /* WRITE_BACK_CTL: Should we drop lock upon abort? */
      };
    };
    char padding[CACHELINE_SIZE];       /* Padding (multiple of a cache line) */
    /* Note padding is not useful here as long as the address can be defined in the lock scheme. */
  };
} w_entry_t;

typedef struct w_set {                  /* Write set */
  w_entry_t *entries;                   /* Array of entries */
  unsigned int nb_entries;              /* Number of entries */
  unsigned int size;                    /* Size of array */
  union {
    unsigned int has_writes;            /* WRITE_BACK_ETL: Has the write set any real write (vs. visible reads) */
    unsigned int nb_acquired;           /* WRITE_BACK_CTL: Number of locks acquired */
  };
} w_set_t;

typedef struct cb_entry {               /* Callback entry */
  void (*f)(void *);                    /* Function */
  void *arg;                            /* Argument to be passed to function */
} cb_entry_t;

typedef struct stm_tx {                 /* Transaction descriptor */
  JMP_BUF env;                          /* Environment for setjmp/longjmp */
  stm_tx_attr_t attr;                   /* Transaction attributes (user-specified) */
  volatile stm_word_t status;           /* Transaction status */
  stm_word_t start;                     /* Start timestamp */
  stm_word_t end;                       /* End timestamp (validity range) */
  r_set_t r_set;                        /* Read set */
  w_set_t w_set;                        /* Write set */
  unsigned int nesting;                 /* Nesting level */
  void *data[MAX_SPECIFIC];             /* Transaction-specific data (fixed-size array for better speed) */
  struct stm_tx *next;                  /* For keeping track of all transactional threads */
} stm_tx_t;

/* This structure should be ordered by hot and cold variables */
typedef struct {
  volatile stm_word_t locks[LOCK_ARRAY_SIZE] ALIGNED;
  volatile stm_word_t gclock[512 / sizeof(stm_word_t)] ALIGNED;
  unsigned int nb_specific;             /* Number of specific slots used (<= MAX_SPECIFIC) */
  unsigned int nb_init_cb;
  cb_entry_t init_cb[MAX_CB];           /* Init thread callbacks */
  unsigned int nb_exit_cb;
  cb_entry_t exit_cb[MAX_CB];           /* Exit thread callbacks */
  unsigned int nb_start_cb;
  cb_entry_t start_cb[MAX_CB];          /* Start callbacks */
  unsigned int nb_precommit_cb;
  cb_entry_t precommit_cb[MAX_CB];      /* Commit callbacks */
  unsigned int nb_commit_cb;
  cb_entry_t commit_cb[MAX_CB];         /* Commit callbacks */
  unsigned int nb_abort_cb;
  cb_entry_t abort_cb[MAX_CB];          /* Abort callbacks */
  unsigned int initialized;             /* Has the library been initialized? */
  volatile stm_word_t quiesce;          /* Prevent threads from entering transactions upon quiescence */
  volatile stm_word_t threads_nb;       /* Number of active threads */
  stm_tx_t *threads;                    /* Head of linked list of threads */
  pthread_mutex_t quiesce_mutex;        /* Mutex to support quiescence */
  pthread_cond_t quiesce_cond;          /* Condition variable to support quiescence */
  /* At least twice a cache line (256 bytes to be on the safe side) */
  char padding[CACHELINE_SIZE];
} ALIGNED global_t;

extern global_t _tinystm;


/* ################################################################### *
 * FUNCTIONS DECLARATIONS
 * ################################################################### */

static NOINLINE void
stm_rollback(stm_tx_t *tx, unsigned int reason);

/* ################################################################### *
 * INLINE FUNCTIONS
 * ################################################################### */



/*
 * Initialize quiescence support.
 */
static INLINE void
stm_quiesce_init(void)
{
  PRINT_DEBUG("==> stm_quiesce_init()\n");

  if (pthread_mutex_init(&_tinystm.quiesce_mutex, NULL) != 0) {
    fprintf(stderr, "Error creating mutex\n");
    exit(1);
  }
  if (pthread_cond_init(&_tinystm.quiesce_cond, NULL) != 0) {
    fprintf(stderr, "Error creating condition variable\n");
    exit(1);
  }
  _tinystm.quiesce = 0;
  _tinystm.threads_nb = 0;
  _tinystm.threads = NULL;
}

/*
 * Clean up quiescence support.
 */
static INLINE void
stm_quiesce_exit(void)
{
  PRINT_DEBUG("==> stm_quiesce_exit()\n");

  pthread_cond_destroy(&_tinystm.quiesce_cond);
  pthread_mutex_destroy(&_tinystm.quiesce_mutex);
}

/*
 * Called by each thread upon initialization for quiescence support.
 */
static INLINE void
stm_quiesce_enter_thread(stm_tx_t *tx)
{
  PRINT_DEBUG("==> stm_quiesce_enter_thread(%p)\n", tx);

  pthread_mutex_lock(&_tinystm.quiesce_mutex);
  /* Add new descriptor at head of list */
  tx->next = _tinystm.threads;
  _tinystm.threads = tx;
  _tinystm.threads_nb++;
  pthread_mutex_unlock(&_tinystm.quiesce_mutex);
}

/*
 * Called by each thread upon exit for quiescence support.
 */
static INLINE void
stm_quiesce_exit_thread(stm_tx_t *tx)
{
  stm_tx_t *t, *p;

  PRINT_DEBUG("==> stm_quiesce_exit_thread(%p)\n", tx);

  /* Can only be called if non-active */
  assert(!IS_ACTIVE(tx->status));

  pthread_mutex_lock(&_tinystm.quiesce_mutex);
  /* Remove descriptor from list */
  p = NULL;
  t = _tinystm.threads;
  while (t != tx) {
    assert(t != NULL);
    p = t;
    t = t->next;
  }
  if (p == NULL)
    _tinystm.threads = t->next;
  else
    p->next = t->next;
  _tinystm.threads_nb--;
  if (_tinystm.quiesce) {
    /* Wake up someone in case other threads are waiting for us */
    pthread_cond_signal(&_tinystm.quiesce_cond);
  }
  pthread_mutex_unlock(&_tinystm.quiesce_mutex);
}

/*
 * Wait for all transactions to be block on a barrier.
 */
static NOINLINE void
stm_quiesce_barrier(stm_tx_t *tx, void (*f)(void *), void *arg)
{
  PRINT_DEBUG("==> stm_quiesce_barrier()\n");

  /* Can only be called if non-active */
  assert(tx == NULL || !IS_ACTIVE(tx->status));

  pthread_mutex_lock(&_tinystm.quiesce_mutex);
  /* Wait for all other transactions to block on barrier */
  _tinystm.threads_nb--;
  if (_tinystm.quiesce == 0) {
    /* We are first on the barrier */
    _tinystm.quiesce = 1;
  }
  while (_tinystm.quiesce) {
    if (_tinystm.threads_nb == 0) {
      /* Everybody is blocked */
      if (f != NULL)
        f(arg);
      /* Release transactional threads */
      _tinystm.quiesce = 0;
      pthread_cond_broadcast(&_tinystm.quiesce_cond);
    } else {
      /* Wait for other transactions to stop */
      pthread_cond_wait(&_tinystm.quiesce_cond, &_tinystm.quiesce_mutex);
    }
  }
  _tinystm.threads_nb++;
  pthread_mutex_unlock(&_tinystm.quiesce_mutex);
}

/*
 * Wait for all transactions to be out of their current transaction.
 */
static INLINE int
stm_quiesce(stm_tx_t *tx, int block)
{
  stm_tx_t *t;

  PRINT_DEBUG("==> stm_quiesce(%p,%d)\n", tx, block);

  if (IS_ACTIVE(tx->status)) {
    /* Only one active transaction can quiesce at a time, others must abort */
    if (pthread_mutex_trylock(&_tinystm.quiesce_mutex) != 0)
      return 1;
  } else {
    /* We can safely block because we are inactive */
    pthread_mutex_lock(&_tinystm.quiesce_mutex);
  }
  /* We own the lock at this point */
  if (block)
    ATOMIC_STORE_REL(&_tinystm.quiesce, 2);
  /* Make sure we read latest status data */
  ATOMIC_MB_FULL;
  /* Not optimal as we check transaction sequentially and might miss some inactivity states */
  for (t = _tinystm.threads; t != NULL; t = t->next) {
    if (t == tx)
      continue;
    /* Wait for all other transactions to become inactive */
    while (IS_ACTIVE(t->status))
      ;
  }
  if (!block)
    pthread_mutex_unlock(&_tinystm.quiesce_mutex);
  return 0;
}

/*
 * Check if transaction must block.
 */
static INLINE int
stm_check_quiesce(stm_tx_t *tx)
{
  stm_word_t s;

  /* Must be called upon start (while already active but before acquiring any lock) */
  assert(IS_ACTIVE(tx->status));

  /* ATOMIC_MB_FULL;  The full memory barrier is not required here since quiesce
   * is atomic. Only a compiler barrier is needed to avoid reordering. */
  ATOMIC_CB;

  if (unlikely(ATOMIC_LOAD_ACQ(&_tinystm.quiesce) == 2)) {
    s = ATOMIC_LOAD(&tx->status);
    SET_STATUS(tx->status, TX_IDLE);
    while (ATOMIC_LOAD_ACQ(&_tinystm.quiesce) == 2) {
    }
    SET_STATUS(tx->status, GET_STATUS(s));
    return 1;
  }
  return 0;
}

/*
 * Release threads blocked after quiescence.
 */
static INLINE void
stm_quiesce_release(stm_tx_t *tx)
{
  ATOMIC_STORE_REL(&_tinystm.quiesce, 0);
  pthread_mutex_unlock(&_tinystm.quiesce_mutex);
}

/*
 * Reset clock and timestamps
 */
static INLINE void
rollover_clock(void *arg)
{
  PRINT_DEBUG("==> rollover_clock()\n");

  /* Reset clock */
  CLOCK = 0;
  /* Reset timestamps */
  memset((void *)_tinystm.locks, 0, LOCK_ARRAY_SIZE * sizeof(stm_word_t));
}

/*
 * Check if stripe has been read previously.
 */
static INLINE r_entry_t *
stm_has_read(stm_tx_t *tx, volatile stm_word_t *lock)
{
  r_entry_t *r;
  int i;

  PRINT_DEBUG("==> stm_has_read(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, lock);


  /* Look for read */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    if (r->lock == lock) {
      /* Return first match*/
      return r;
    }
  }
  return NULL;
}

/*
 * Check if address has been written previously.
 */
static INLINE w_entry_t *
stm_has_written(stm_tx_t *tx, volatile stm_word_t *addr)
{
  w_entry_t *w;
  int i;

  PRINT_DEBUG("==> stm_has_written(%p[%lu-%lu],%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);


  /* Look for write */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    if (w->addr == addr) {
      return w;
    }
  }
  return NULL;
}

/*
 * (Re)allocate read set entries.
 */
static NOINLINE void
stm_allocate_rs_entries(stm_tx_t *tx, int extend)
{
  PRINT_DEBUG("==> stm_allocate_rs_entries(%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, extend);

  if (extend) {
    /* Extend read set */
    tx->r_set.size *= 2;
    tx->r_set.entries = (r_entry_t *)xrealloc(tx->r_set.entries, tx->r_set.size * sizeof(r_entry_t));
  } else {
    /* Allocate read set */
    tx->r_set.entries = (r_entry_t *)xmalloc_aligned(tx->r_set.size * sizeof(r_entry_t));
  }
}

/*
 * (Re)allocate write set entries.
 */
static NOINLINE void
stm_allocate_ws_entries(stm_tx_t *tx, int extend)
{

  PRINT_DEBUG("==> stm_allocate_ws_entries(%p[%lu-%lu],%d)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, extend);

  if (extend) {
    /* Extend write set */
    /* Transaction must be inactive for WRITE_THROUGH or WRITE_BACK_ETL */
    tx->w_set.size *= 2;
    tx->w_set.entries = (w_entry_t *)xrealloc(tx->w_set.entries, tx->w_set.size * sizeof(w_entry_t));
  } else {
    /* Allocate write set */
    tx->w_set.entries = (w_entry_t *)xmalloc_aligned(tx->w_set.size * sizeof(w_entry_t));
  }
  /* Ensure that memory is aligned. */
  assert((((stm_word_t)tx->w_set.entries) & OWNED_MASK) == 0);

}


# include "stm_wbetl.h"


/*
 * Initialize the transaction descriptor before start or restart.
 */
static INLINE void
int_stm_prepare(stm_tx_t *tx)
{

  /* Read/write set */
  /* has_writes / nb_acquired are the same field. */
  tx->w_set.has_writes = 0;
  /* tx->w_set.nb_acquired = 0; */
  tx->w_set.nb_entries = 0;
  tx->r_set.nb_entries = 0;

 start:
  /* Start timestamp */
  tx->start = tx->end = GET_CLOCK; /* OPT: Could be delayed until first read/write */
  if (unlikely(tx->start >= VERSION_MAX)) {
    /* Block all transactions and reset clock */
    stm_quiesce_barrier(tx, rollover_clock, NULL);
    goto start;
  }


  /* Set status */
  UPDATE_STATUS(tx->status, TX_ACTIVE);

  stm_check_quiesce(tx);
}

/*
 * Rollback transaction.
 */
static NOINLINE void
stm_rollback(stm_tx_t *tx, unsigned int reason)
{

  PRINT_DEBUG("==> stm_rollback(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  assert(IS_ACTIVE(tx->status));



  stm_wbetl_rollback(tx);



  /* Set status to ABORTED */
  SET_STATUS(tx->status, TX_ABORTED);

  /* Abort for extending the write set */
  if (unlikely(reason == STM_ABORT_EXTEND_WS)) {
    stm_allocate_ws_entries(tx, 1);
  }

  /* Reset nesting level */
  tx->nesting = 1;

  /* Callbacks */
  if (likely(_tinystm.nb_abort_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_abort_cb; cb++)
      _tinystm.abort_cb[cb].f(_tinystm.abort_cb[cb].arg);
  }



  /* Don't prepare a new transaction if no retry. */
  if (tx->attr.no_retry || (reason & STM_ABORT_NO_RETRY) == STM_ABORT_NO_RETRY) {
    tx->nesting = 0;
    return;
  }

  /* Reset field to restart transaction */
  int_stm_prepare(tx);

  /* Jump back to transaction start */
  /* Note: ABI usually requires 0x09 (runInstrumented+restoreLiveVariable) */
  reason |= STM_PATH_INSTRUMENTED;
  LONGJMP(tx->env, reason);
}

/*
 * Store a word-sized value (return write set entry or NULL).
 */
static INLINE w_entry_t *
stm_write(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  w_entry_t *w;

  PRINT_DEBUG2("==> stm_write(t=%p[%lu-%lu],a=%p,d=%p-%lu,m=0x%lx)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  assert(IS_ACTIVE(tx->status));


  w = stm_wbetl_write(tx, addr, value, mask);

  return w;
}

static INLINE stm_word_t
int_stm_RaR(stm_tx_t *tx, volatile stm_word_t *addr)
{
  stm_word_t value;
  value = stm_wbetl_RaR(tx, addr);
  return value;
}

static INLINE stm_word_t
int_stm_RaW(stm_tx_t *tx, volatile stm_word_t *addr)
{
  stm_word_t value;
  value = stm_wbetl_RaW(tx, addr);
  return value;
}

static INLINE stm_word_t
int_stm_RfW(stm_tx_t *tx, volatile stm_word_t *addr)
{
  stm_word_t value;
  value = stm_wbetl_RfW(tx, addr);
  return value;
}

static INLINE void
int_stm_WaR(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  stm_wbetl_WaR(tx, addr, value, mask);
}

static INLINE void
int_stm_WaW(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  stm_wbetl_WaW(tx, addr, value, mask);
}

static INLINE stm_tx_t *
int_stm_init_thread(void)
{
  stm_tx_t *tx;

  PRINT_DEBUG("==> stm_init_thread()\n");

  /* Avoid initializing more than once */
  if ((tx = tls_get_tx()) != NULL)
    return tx;


  /* Allocate descriptor */
  tx = (stm_tx_t *)xmalloc_aligned(sizeof(stm_tx_t));
  /* Set attribute */
  tx->attr = (stm_tx_attr_t)0;
  /* Set status (no need for CAS or atomic op) */
  tx->status = TX_IDLE;
  /* Read set */
  tx->r_set.nb_entries = 0;
  tx->r_set.size = RW_SET_SIZE;
  stm_allocate_rs_entries(tx, 0);
  /* Write set */
  tx->w_set.nb_entries = 0;
  tx->w_set.size = RW_SET_SIZE;
  /* has_writes / nb_acquired are the same field. */
  tx->w_set.has_writes = 0;
  /* tx->w_set.nb_acquired = 0; */
  stm_allocate_ws_entries(tx, 0);
  /* Nesting level */
  tx->nesting = 0;
  /* Transaction-specific data */
  memset(tx->data, 0, MAX_SPECIFIC * sizeof(void *));
  /* Store as thread-local data */
  tls_set_tx(tx);
  stm_quiesce_enter_thread(tx);

  /* Callbacks */
  if (likely(_tinystm.nb_init_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_init_cb; cb++)
      _tinystm.init_cb[cb].f(_tinystm.init_cb[cb].arg);
  }

  return tx;
}

static INLINE void
int_stm_exit_thread(stm_tx_t *tx)
{

  PRINT_DEBUG("==> stm_exit_thread(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Avoid finalizing again a thread */
  if (tx == NULL)
    return;

  /* Callbacks */
  if (likely(_tinystm.nb_exit_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_exit_cb; cb++)
      _tinystm.exit_cb[cb].f(_tinystm.exit_cb[cb].arg);
  }


  stm_quiesce_exit_thread(tx);

  xfree(tx->r_set.entries);
  xfree(tx->w_set.entries);
  xfree(tx);

  tls_set_tx(NULL);
}

static INLINE sigjmp_buf *
int_stm_start(stm_tx_t *tx, stm_tx_attr_t attr)
{
  PRINT_DEBUG("==> stm_start(%p)\n", tx);

  /* TODO Nested transaction attributes are not checked if they are coherent
   * with parent ones.  */

  /* Increment nesting level */
  if (tx->nesting++ > 0)
    return NULL;

  /* Attributes */
  tx->attr = attr;

  /* Initialize transaction descriptor */
  int_stm_prepare(tx);

  /* Callbacks */
  if (likely(_tinystm.nb_start_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_start_cb; cb++)
      _tinystm.start_cb[cb].f(_tinystm.start_cb[cb].arg);
  }

  return &tx->env;
}

static INLINE int
int_stm_commit(stm_tx_t *tx)
{

  PRINT_DEBUG("==> stm_commit(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Decrement nesting level */
  if (unlikely(--tx->nesting > 0))
    return 1;

  /* Callbacks */
  if (unlikely(_tinystm.nb_precommit_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_precommit_cb; cb++)
      _tinystm.precommit_cb[cb].f(_tinystm.precommit_cb[cb].arg);
  }

  assert(IS_ACTIVE(tx->status));


  /* A read-only transaction can commit immediately */
  if (unlikely(tx->w_set.nb_entries == 0))
    goto end;

  /* Update transaction */
  stm_wbetl_commit(tx);

 end:




  /* Set status to COMMITTED */
  SET_STATUS(tx->status, TX_COMMITTED);

  /* Callbacks */
  if (likely(_tinystm.nb_commit_cb != 0)) {
    unsigned int cb;
    for (cb = 0; cb < _tinystm.nb_commit_cb; cb++)
      _tinystm.commit_cb[cb].f(_tinystm.commit_cb[cb].arg);
  }

  return 1;
}

static INLINE stm_word_t
int_stm_load(stm_tx_t *tx, volatile stm_word_t *addr)
{
  return stm_wbetl_read(tx, addr);
}

static INLINE void
int_stm_store(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value)
{
  stm_write(tx, addr, value, ~(stm_word_t)0);
}

static INLINE void
int_stm_store2(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  stm_write(tx, addr, value, mask);
}

static INLINE int
int_stm_active(stm_tx_t *tx)
{
  assert (tx != NULL);
  return IS_ACTIVE(tx->status);
}

static INLINE int
int_stm_aborted(stm_tx_t *tx)
{
  assert (tx != NULL);
  return (GET_STATUS(tx->status) == TX_ABORTED);
}

static INLINE int
int_stm_irrevocable(stm_tx_t *tx)
{
  assert (tx != NULL);
  return 0;
}

static INLINE int
int_stm_killed(stm_tx_t *tx)
{
  assert (tx != NULL);
  return (GET_STATUS(tx->status) == TX_KILLED);
}

static INLINE sigjmp_buf *
int_stm_get_env(stm_tx_t *tx)
{
  assert (tx != NULL);
  /* Only return environment for top-level transaction */
  return tx->nesting == 0 ? &tx->env : NULL;
}

static INLINE int
int_stm_get_stats(stm_tx_t *tx, const char *name, void *val)
{
  assert (tx != NULL);

  if (strcmp("read_set_size", name) == 0) {
    *(unsigned int *)val = tx->r_set.size;
    return 1;
  }
  if (strcmp("write_set_size", name) == 0) {
    *(unsigned int *)val = tx->w_set.size;
    return 1;
  }
  if (strcmp("read_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->r_set.nb_entries;
    return 1;
  }
  if (strcmp("write_set_nb_entries", name) == 0) {
    *(unsigned int *)val = tx->w_set.nb_entries;
    return 1;
  }
  if (strcmp("read_only", name) == 0) {
    *(unsigned int *)val = tx->attr.read_only;
    return 1;
  }
  return 0;
}

static INLINE void
int_stm_set_specific(stm_tx_t *tx, int key, void *data)
{
  assert (tx != NULL && key >= 0 && key < _tinystm.nb_specific);
  ATOMIC_STORE(&tx->data[key], data);
}

static INLINE void *
int_stm_get_specific(stm_tx_t *tx, int key)
{
  assert (tx != NULL && key >= 0 && key < _tinystm.nb_specific);
  return (void *)ATOMIC_LOAD(&tx->data[key]);
}

#endif /* _STM_INTERNAL_H_ */

