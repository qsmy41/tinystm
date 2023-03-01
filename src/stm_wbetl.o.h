/*
 * File:
 *   stm_wbetl.h
 * Author(s):
 *   Pascal Felber <pascal.felber@unine.ch>
 *   Patrick Marlier <patrick.marlier@unine.ch>
 * Description:
 *   STM internal functions for Write-back ETL.
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

#ifndef _STM_WBETL_H_
#define _STM_WBETL_H_

#include "stm_internal.h"
#include "atomic.h"


static INLINE int
stm_wbetl_validate(stm_tx_t *tx)
{
  r_entry_t *r;
  int i;
  stm_word_t l;

  PRINT_DEBUG("==> stm_wbetl_validate(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  /* Validate reads */
  r = tx->r_set.entries;
  for (i = tx->r_set.nb_entries; i > 0; i--, r++) {
    /* Read lock */
    l = ATOMIC_LOAD(r->lock);
    /* Unlocked and still the same version? */
    if (LOCK_GET_OWNED(l)) {
      /* Do we own the lock? */
      w_entry_t *w = (w_entry_t *)LOCK_GET_ADDR(l);
      /* Simply check if address falls inside our write set (avoids non-faulting load) */
      if (!(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries))
      {
        /* Locked by another transaction: cannot validate */
        return 0;
      }
      /* We own the lock: OK */
    } else {
      if (LOCK_GET_TIMESTAMP(l) != r->version) {
        /* Other version: cannot validate */
        return 0;
      }
      /* Same version: OK */
    }
  }
  return 1;
}

/*
 * Extend snapshot range.
 */
static INLINE int
stm_wbetl_extend(stm_tx_t *tx)
{
  stm_word_t now;

  PRINT_DEBUG("==> stm_wbetl_extend(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);


  /* Get current time */
  now = GET_CLOCK;
  /* No need to check clock overflow here. The clock can exceed up to MAX_THREADS and it will be reset when the quiescence is reached. */

  /* Try to validate read set */
  if (stm_wbetl_validate(tx)) {
    /* It works: we can extend until now */
    tx->end = now;
    return 1;
  }
  return 0;
}

static INLINE void
stm_wbetl_rollback(stm_tx_t *tx)
{
  w_entry_t *w;
  int i;

  PRINT_DEBUG("==> stm_wbetl_rollback(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);

  assert(IS_ACTIVE(tx->status));

  /* Drop locks */
  i = tx->w_set.nb_entries;
  if (i > 0) {
    w = tx->w_set.entries;
    for (; i > 0; i--, w++) {
      if (w->next == NULL) {
        /* Only drop lock for last covered address in write set */
        ATOMIC_STORE(w->lock, LOCK_SET_TIMESTAMP(w->version));
      }
    }
    /* Make sure that all lock releases become visible */
    ATOMIC_MB_WRITE;
  }
}

/*
 * Load a word-sized value (invisible read).
 */
static INLINE stm_word_t
stm_wbetl_read_invisible(stm_tx_t *tx, volatile stm_word_t *addr)
{
  volatile stm_word_t *lock;
  stm_word_t l, l2, value, version;
  r_entry_t *r;
  w_entry_t *w;

  PRINT_DEBUG2("==> stm_wbetl_read_invisible(t=%p[%lu-%lu],a=%p)\n", tx, (unsigned long)tx->start, (unsigned long)tx->end, addr);

  assert(IS_ACTIVE(tx->status));

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Note: we could check for duplicate reads and get value from read set */

  /* Read lock, value, lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (unlikely(LOCK_GET_WRITE(l))) {
    /* Locked */
    /* Do we own the lock? */
    // if tx owns the write lock on the memory addr, find the latest write value and return it
    w = (w_entry_t *)LOCK_GET_ADDR(l);
    /* Simply check if address falls inside our write set (avoids non-faulting load) */
    if (likely(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries)) {
      /* Yes: did we previously write the same address? */
      while (1) {
        if (addr == w->addr) {
          /* Yes: get value from write set (or from memory if mask was empty) */
          value = (w->mask == 0 ? ATOMIC_LOAD(addr) : w->value);
          break;
        }
        // why do we need the following?
        if (w->next == NULL) {
          /* No: get value from memory */
          value = ATOMIC_LOAD(addr);
          break;
        }
        w = w->next;
      }
      /* No need to add to read set (will remain valid) */
      return value;
    }


    /* Conflict: CM kicks in (we could also check for duplicate reads and get value from read set) */
# if defined(IRREVOCABLE_ENABLED) && defined(IRREVOCABLE_IMPROVED)
    if (tx->irrevocable && ATOMIC_LOAD(&_tinystm.irrevocable) == 1)
      ATOMIC_STORE(&_tinystm.irrevocable, 2);
# endif /* defined(IRREVOCABLE_ENABLED) && defined(IRREVOCABLE_IMPROVED) */
    if (unlikely(tx->irrevocable)) {
      /* Spin while locked */
      goto restart;
    }
    // if tx does not own the write lock, "conflicting access", thus abort
    /* Abort */
    stm_rollback(tx, STM_ABORT_RW_CONFLICT);
    return 0;
  } else {
    /* Not locked */
    // why do we need the following?
    value = ATOMIC_LOAD_ACQ(addr);
    l2 = ATOMIC_LOAD_ACQ(lock);
    if (unlikely(l != l2)) {
      l = l2;
      goto restart_no_load;
    }
    /* In irrevocable mode, no need check timestamp nor add entry to read set */
    if (unlikely(tx->irrevocable))
      goto return_value;
    /* Check timestamp */
    version = LOCK_GET_TIMESTAMP(l);
    /* Valid version? */
    if (unlikely(version > tx->end)) {
      /* No: try to extend first (except for read-only transactions: no read set) */
      if (tx->attr.read_only || !stm_wbetl_extend(tx)) {
        /* Not much we can do: abort */
        stm_rollback(tx, STM_ABORT_VAL_READ);
        return 0;
      }
      /* Verify that version has not been overwritten (read value has not
       * yet been added to read set and may have not been checked during
       * extend) */
      l2 = ATOMIC_LOAD_ACQ(lock);
      if (l != l2) {
        l = l2;
        goto restart_no_load;
      }
      /* Worked: we now have a good version (version <= tx->end) */
    }
  }
  /* We have a good version: add to read set (update transactions) and return value */

  if (!tx->attr.read_only) {
    /* Add address and version to read set */
    if (tx->r_set.nb_entries == tx->r_set.size)
      stm_allocate_rs_entries(tx, 1);
    r = &tx->r_set.entries[tx->r_set.nb_entries++];
    r->version = version;
    r->lock = lock;
  }
 return_value:
  return value;
}


static INLINE stm_word_t
stm_wbetl_read(stm_tx_t *tx, volatile stm_word_t *addr)
{
  return stm_wbetl_read_invisible(tx, addr);
}

static INLINE w_entry_t *
stm_wbetl_write(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  volatile stm_word_t *lock;
  stm_word_t l, version;
  w_entry_t *w;
  w_entry_t *prev = NULL;

  PRINT_DEBUG2("==> stm_wbetl_write(t=%p[%lu-%lu],a=%p,d=%p-%lu,m=0x%lx)\n",
               tx, (unsigned long)tx->start, (unsigned long)tx->end, addr, (void *)value, (unsigned long)value, (unsigned long)mask);

  /* Get reference to lock */
  lock = GET_LOCK(addr);

  /* Try to acquire lock */
 restart:
  l = ATOMIC_LOAD_ACQ(lock);
 restart_no_load:
  if (unlikely(LOCK_GET_OWNED(l))) {
    /* Locked */


    /* Do we own the lock? */
    w = (w_entry_t *)LOCK_GET_ADDR(l);
    /* Simply check if address falls inside our write set (avoids non-faulting load) */
    if (likely(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries)) {
      /* Yes */
      if (mask == 0) {
        /* No need to insert new entry or modify existing one */
        return w;
      }
      prev = w;
      /* Did we previously write the same address? */
      while (1) {
        if (addr == prev->addr) {
          /* No need to add to write set */
          if (mask != ~(stm_word_t)0) {
            if (prev->mask == 0)
              prev->value = ATOMIC_LOAD(addr);
            // overwrite prev->value's bits whose corresponding mask val is 1
            value = (prev->value & ~mask) | (value & mask);
          }
          prev->value = value;
          prev->mask |= mask;
          return prev;
        }
        if (prev->next == NULL) {
          /* Remember last entry in linked list (for adding new entry) */
          break;
        }
        prev = prev->next;
      }
      /* Get version from previous write set entry (all entries in linked list have same version) */
      version = prev->version;
      /* Must add to write set */
      if (tx->w_set.nb_entries == tx->w_set.size)
        // how does the following "abort the transaction"
        // and subsequently restart the transaction?
        stm_rollback(tx, STM_ABORT_EXTEND_WS);
      w = &tx->w_set.entries[tx->w_set.nb_entries];
      goto do_write;
    }
    /* Conflict: CM kicks in */
#if defined(IRREVOCABLE_ENABLED) && defined(IRREVOCABLE_IMPROVED)
    if (tx->irrevocable && ATOMIC_LOAD(&_tinystm.irrevocable) == 1)
      ATOMIC_STORE(&_tinystm.irrevocable, 2);
#endif /* defined(IRREVOCABLE_ENABLED) && defined(IRREVOCABLE_IMPROVED) */
    if (tx->irrevocable) {
      /* Spin while locked */
      goto restart;
    }
    /* Abort */
    stm_rollback(tx, STM_ABORT_WW_CONFLICT);
    return NULL;
  }
  /* Not locked */
  /* Handle write after reads (before CAS) */
  version = LOCK_GET_TIMESTAMP(l);
  /* In irrevocable mode, no need to revalidate */
  if (unlikely(tx->irrevocable))
    goto acquire_no_check;
 acquire:
  if (unlikely(version > tx->end)) {
    /* We might have read an older version previously */
    if (unlikely(stm_has_read(tx, lock) != NULL)) {
      /* Read version must be older (otherwise, tx->end >= version) */
      /* Not much we can do: abort */
      stm_rollback(tx, STM_ABORT_VAL_WRITE);
      return NULL;
    }
  }
  /* Acquire lock (ETL) */
 acquire_no_check:
  if (unlikely(tx->w_set.nb_entries == tx->w_set.size))
    stm_rollback(tx, STM_ABORT_EXTEND_WS);
  w = &tx->w_set.entries[tx->w_set.nb_entries];
  if (unlikely(ATOMIC_CAS_FULL(lock, l, LOCK_SET_ADDR_WRITE((stm_word_t)w)) == 0))
    goto restart;
  /* We own the lock here (ETL) */
do_write:
  /* Add address to write set */
  w->addr = addr;
  w->mask = mask;
  w->lock = lock;
  if (unlikely(mask == 0)) {
    /* Do not write anything */
#ifndef NDEBUG
    w->value = 0;
#endif /* ! NDEBUG */
  } else {
    /* Remember new value */
    if (mask != ~(stm_word_t)0)
      value = (ATOMIC_LOAD(addr) & ~mask) | (value & mask);
    w->value = value;
  }
  w->version = version;
  w->next = NULL;
  if (prev != NULL) {
    /* Link new entry in list */
    prev->next = w;
  }
  tx->w_set.nb_entries++;
  tx->w_set.has_writes++;

  return w;
}

static INLINE stm_word_t
stm_wbetl_RaR(stm_tx_t *tx, volatile stm_word_t *addr)
{
  /* Possible optimization: avoid adding to read set again */
  return stm_wbetl_read(tx, addr);
}

static INLINE stm_word_t
stm_wbetl_RaW(stm_tx_t *tx, volatile stm_word_t *addr)
{
  stm_word_t l;
  w_entry_t *w;

  l = ATOMIC_LOAD_ACQ(GET_LOCK(addr));
  /* Does the lock owned? */
  assert(LOCK_GET_WRITE(l));
  /* Do we own the lock? */
  w = (w_entry_t *)LOCK_GET_ADDR(l);
  assert(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries);

  /* Read directly from write set entry. */
  return w->value;
}

static INLINE stm_word_t
stm_wbetl_RfW(stm_tx_t *tx, volatile stm_word_t *addr)
{
  /* Acquire lock as write. */
  stm_wbetl_write(tx, addr, 0, 0);
  /* Now the lock is owned, read directly from memory is safe. */
  /* TODO Unsafe with CM_MODULAR */
  return *addr;
}

static INLINE void
stm_wbetl_WaR(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  /* Probably no optimization can be done here. */
  stm_wbetl_write(tx, addr, value, mask);
}

static INLINE void
stm_wbetl_WaW(stm_tx_t *tx, volatile stm_word_t *addr, stm_word_t value, stm_word_t mask)
{
  stm_word_t l;
  w_entry_t *w;

  l = ATOMIC_LOAD_ACQ(GET_LOCK(addr));
  /* Does the lock owned? */
  assert(LOCK_GET_WRITE(l));
  /* Do we own the lock? */
  w = (w_entry_t *)LOCK_GET_ADDR(l);
  assert(tx->w_set.entries <= w && w < tx->w_set.entries + tx->w_set.nb_entries);
  /* in WaW, mask can never be 0 */
  assert(mask != 0);
  while (1) {
    if (addr == w->addr) {
      /* No need to add to write set */
      if (mask != ~(stm_word_t)0) {
        if (w->mask == 0)
          w->value = ATOMIC_LOAD(addr);
        value = (w->value & ~mask) | (value & mask);
      }
      w->value = value;
      w->mask |= mask;
      return;
    }
    /* The entry must exist */
    assert (w->next != NULL);
    w = w->next;
  }
}

static INLINE int
stm_wbetl_commit(stm_tx_t *tx)
{
  w_entry_t *w;
  stm_word_t t;
  int i;

  PRINT_DEBUG("==> stm_wbetl_commit(%p[%lu-%lu])\n", tx, (unsigned long)tx->start, (unsigned long)tx->end);


  /* Update transaction */
  /* Verify if there is an irrevocable transaction once all locks have been acquired */
# ifdef IRREVOCABLE_IMPROVED
  /* FIXME: it is bogus. the status should be changed to idle otherwise stm_quiesce will not progress */
  if (unlikely(!tx->irrevocable)) {
    do {
      t = ATOMIC_LOAD(&_tinystm.irrevocable);
      /* If the irrevocable transaction have encountered an acquired lock, abort */
      if (t == 2) {
        stm_rollback(tx, STM_ABORT_IRREVOCABLE);
        return 0;
      }
    } while (t);
  }
# else /* ! IRREVOCABLE_IMPROVED */
  if (!tx->irrevocable && ATOMIC_LOAD(&_tinystm.irrevocable)) {
    stm_rollback(tx, STM_ABORT_IRREVOCABLE);
    return 0;
  }
# endif /* ! IRREVOCABLE_IMPROVED */

  /* Get commit timestamp (may exceed VERSION_MAX by up to MAX_THREADS) */
  t = FETCH_INC_CLOCK + 1;
  if (unlikely(tx->irrevocable))
    goto release_locks;

  /* Try to validate (only if a concurrent transaction has committed since tx->start) */
  if (unlikely(tx->start != t - 1 && !stm_wbetl_validate(tx))) {
    /* Cannot commit */
    stm_rollback(tx, STM_ABORT_VALIDATE);
    return 0;
  }

  release_locks:

  /* Install new versions, drop locks and set new timestamp */
  w = tx->w_set.entries;
  for (i = tx->w_set.nb_entries; i > 0; i--, w++) {
    if (w->mask != 0)
      ATOMIC_STORE(w->addr, w->value);
    /* Only drop lock for last covered address in write set */
    // it is possible to have >= 2 write sets covered by the same lock,
    // so it is necessary to specify only release lock after storing all of them,
    // i.e. release at the last write covered by the lock
    if (w->next == NULL) {
        ATOMIC_STORE_REL(w->lock, LOCK_SET_TIMESTAMP(t));
    }
  }

 end:
  return 1;
}

#endif /* _STM_WBETL_H_ */

