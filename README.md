TinySTM
=======

OVERVIEW
--------

TinySTM is a lightweight but efficient word-based STM implementation.
This distribution includes three versions of TinySTM: write-back
(updates are buffered until commit time), write-through (updates are
directly written to memory), and commit-time locking (locks are only
acquired upon commit).  The version can be selected by editing the
makefile, which documents all the different compilation options.

TinySTM compiles and runs on 32 or 64-bit architectures.  It was tested
on various flavors of Unix, on Mac OS X, and on Windows using cygwin.
It comes with a few test applications, notably a linked list, a skip
list, and a red-black tree.


INSTALLATION
------------

TinySTM requires the 'atomic\_ops' library, freely available from
[www.hpl.hp.com](http://www.hpl.hp.com/research/linux/atomic_ops/).
A stripped-down version of the library is included in the TinySTM 
distribution.  If you wish to use another version, you must set the 
environment variable LIBAO\_HOME to the installation directory of
'atomic\_ops'.

If your system does not support GCC thread-local storage, modify the
TLS parameter in the 'Makefile' file.

To compile TinySTM libraries, execute 'make' in the main directory.  To
compile test applications, execute 'make test'.  To check the compiled
library, execute 'make check'. 'make clean' will remove all compiled
files.
To compile the TinySTM GCC compatible library, execute 'make abi-gcc'.
To compile test applications, execute 'make abi-gcc-test'.


CONTACT
-------

* E-mail : [tinystm@tinystm.org](mailto:tinystm@tinystm.org)
* Web    : [http://tinystm.org](http://tinystm.org) and
 [http://www.tmware.org](http://www.tmware.org)


ACKNOWLEDGEMENT
---------------

This library was supported by the European research consortium
[VELOX](http://www.velox-project.eu).

Complementary Information
=========================

Acronyms
--------
* ab: atomic blocks
* cb: callback
* cm: contention manager ("CM\_MODULAR")
* nb: number
* wb: write-back
* wt: wrtie-through
* etl: encounter-time locking
* ctl: commit-time locking

Codebase Overview (in `src/`)
-----------------
* `atomic ops/`: atomic operations implemented in different assembly for
  different platforms.
* `atomic.h`: header file to define macros for atomic operations, either
  built-in or custom definitions (in `atomic ops/`)
* `gc.*`: garbage collector definitions, epoch-based (see `epoch_gc` macro
  defined in `Makefile`)
* `mod_*.c`: custom implementations for ab, cm, print, etc.
* `stm.c`: implementation of `stm.h`
* `stm_*.h`: different flavors of STM implementations
* `tls.h`: thread-local support functions
* `utils.h`: different custom operations on memory
* `wrappers.c`: different STM wrapper functions

Notes
-----
N.B. The following notes may *not* be applicable for flavors other than `wbetl`.
* `atomic_load_acquire(lock)` only get the reference of the lock. 
  It does not *lock* or *own* the lock.
* `likely()` and `unlikely()` functions are to provide compiler with branch
  prediction information, if there is any branch prediction support from the
  architecture
* Transactional read does not lock; transactional write locks and only
  release the locks after committing
* `mask` is used for identifying the bits that the word
  must be updated. The value to be stored may not use up the entire word.
  As such, by default its value is `~(stm_word_t) 0`.
* `stm_tx_t`'s `start` and `end` properties:

  `start` is checked against global clock to see if there are
  other concurrent transactions committed before the current transaction
  commits.

  `end` ensures that a transaction reads from the latest
  committed write.
  It is checked against the lock's timestamp. Lock's timestamp being larger
  than transaction's `end` means that a write operation in a concurrent
  transaction has been committed to that memory. 

    * In `read`, if all the other reads
      locations' locks are owned by the transaction, and they are reading from the
      latest version (`stm_wbetl_validate()`), we can extend the transaction's
      timestamp and proceed, otherwise abort.

    * In `write`, it additionally checks if current transaction has previously
      read from the same location. If it does, abort immediately. 
      
      **Question**: What if reading the old value is intentional? e.g. 
      ```
      a = x;
      x = 1;
      ```
      Given the above transaction, it may be desired to read `a=0` instead of
      `a=1`.

Q&A with Scenarios (`wbetl`)
--------
1. *version* of a lock is used to check if the latest value is read/written.

    Consider the following scenario: Say a transaction Tx1 starts very
    early, followed by Tx2 and Tx3 in this order. Tx2 modify `x`, Tx3 modify
    `y`, Tx1 first reads `y`, then reads `x`. Say Tx2 and Tx3 finishes
    earlier than Tx1 starting to read `y`. As such, Tx1 needs to update its
    latest version (`stm_wbetl_extend`). Next, Tx1 reads `x`.
    Will Tx1 get a lock with an older version of the lock updated by Tx2?

    No. Because Tx2 finishes earlier than Tx3,
    and Tx1 is extended to have the version of Tx3,
    and read `x` happens after read `y`,
    Tx1 is guaranteed to have the latest version (lock),
    thus `!(version > tx->end)`.

1. `stm_wbetl_read_invisible`.

    Consider the following scenario: Tx1 reads `x`, Tx2 writes to `x`. Tx1
    happens *after* Tx2. But according to implementation, if Tx1 starts first,
    reads `x` first, but did not commit until Tx2 committed. Shouldn't Tx1 be
    aborted?

    Yes. This "validation" happens at commit time, conducted by `stm_wbetl_validate`.


