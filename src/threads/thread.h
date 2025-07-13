#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "fix-point.h"
#ifdef USERPROG
#include "lib/kernel/hash.h"
#endif

/** States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING, /**< Running thread. */
  THREAD_READY,   /**< Not running but ready to run. */
  THREAD_BLOCKED, /**< Waiting for an event to trigger. */
  THREAD_DYING    /**< About to be destroyed. */
};

/** Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /**< Error value for tid_t. */

/** Map region identifier for memory mapped files. */
#ifndef MAPID_T_DEFINED
#define MAPID_T_DEFINED
typedef int mapid_t;
#endif
#define MAP_FAILED ((mapid_t) -1) /**< Error value for mapid_t. */

/** Per-child status record for parent-child process management.
    Lives in heap memory outside either thread's kernel stack. */
struct child_status {
  tid_t child_tid;                  /**< Child's thread ID - fixed at creation */
  bool load_done;                   /**< True when child finishes start_process() loading */
  bool load_ok;                     /**< True if child loaded successfully */
  bool child_exited;                /**< True when child calls exit() */
  bool parent_exited_first;         /**< True when parent calls exit() first */
  bool parent_waited;               /**< True if parent has already called wait() once */
  int exit_code;                    /**< Exit status set in exit() */
  struct semaphore load_sema;       /**< 0 -> parent blocks until child finishes loading */
  struct semaphore exit_sema;       /**< 0 -> parent blocks in wait() until child exits */
  struct list_elem elem;            /**< List element for parent's children list */
  char process_name[16];
};

/** Thread priorities. */
#define PRI_MIN 0      /**< Lowest priority. */
#define PRI_DEFAULT 31 /**< Default priority. */
#define PRI_MAX 63     /**< Highest priority. */

#define FD_CNT 128 /**< Maximum number of file descriptors per process. */

/** A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. */
/** The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread {
  /* Owned by thread.c. */
  tid_t tid;                 /**< Thread identifier. */
  enum thread_status status; /**< Thread state. */
  char name[16];             /**< Name (for debugging purposes). */
  uint8_t *stack;            /**< Saved stack pointer. */
  int priority;              /**< Priority. */
  int base_priority;
  struct list_elem allelem;  /**< List element for all threads list. */

  /* Shared between thread.c and synch.c. */
  struct list_elem elem; /**< List element. */

#ifdef USERPROG
  /* Owned by userprog/process.c. */
  uint32_t *pagedir; /**< Page directory. */
  struct hash spt;   /**< Supplemental page table. */
  
  /* Stack growth tracking */
  void *stack_bottom;        /**< Bottom of stack (highest address) */
  size_t stack_size;         /**< Current stack size in bytes */
  void *user_esp;            /**< Last known user ESP for kernel faults */
  
  /* Memory mapping tracking */
  struct list mmap_list;     /**< List of memory mappings */
  mapid_t next_mapid;        /**< Next mapping ID to assign */
  
  bool is_user_process;
  struct file *fd_table[FD_CNT];
  struct file *exec_file;
  /* Parent-child process management */
  struct list children;              /**< List of child_status structs for this parent */
  struct child_status *self_status;  /**< Pointer to this thread's status record (if child) */
#endif

  int64_t wait_ticks;
  struct list locks;
  struct lock *waiting_lock;
  bool is_donating;

  /* Owned by thread.c. */
  unsigned magic; /**< Detects stack overflow. */
  int nice;                       /* Niceness between -20 and 20 */
  fp_t recent_cpu;       /* Recent CPU usage (fixed-point) */ 
};

/** If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
extern bool thread_mlfqs;

void thread_init(void);
void thread_start(void);

void thread_tick(void);
void thread_print_stats(void);

bool thread_priority_compare(const struct list_elem *a, const struct list_elem *b, void *aux);

void update_cpu_usage(void);
void update_priority(void);
void update_recent_cpu(void);

void recalculate_priority(struct thread *t, void *aux UNUSED);
void recalculate_recent_cpu(struct thread *t, void *aux UNUSED);

typedef void thread_func(void *aux);
tid_t thread_create(const char *name, int priority, thread_func *, void *);

void thread_block(void);
void thread_unblock(struct thread *);

struct thread *thread_current(void);
tid_t thread_tid(void);
const char *thread_name(void);

void thread_exit(void) NO_RETURN;
void thread_yield(void);

void check_yield(void);

/** Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread *t, void *aux);
void thread_foreach(thread_action_func *, void *);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

#endif /**< threads/thread.h */
