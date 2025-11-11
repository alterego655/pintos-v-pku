# Project 3a: Virtual Memory

## Preliminaries

>Fill in your name and email address.

FirstName LastName <email@domain.example>

>If you have any preliminary comments on your submission, notes for the TAs, please give them here.

This implementation provides a complete virtual memory system with lazy loading, frame eviction using clock algorithm, swap support, and memory-mapped files. The design emphasizes correctness and follows Pintos coding standards.

>Please cite any offline or online sources you consulted while preparing your submission, other than the Pintos documentation, course text, lecture notes, and course staff.

Operating System Concepts (Silberschatz) for clock algorithm details and VM design principles.

## Page Table Management

#### DATA STRUCTURES

>A1: Copy here the declaration of each new or changed struct or struct member, global or static variable, typedef, or enumeration.  Identify the purpose of each in 25 words or less.

```c
/* Supplemental page table entry */
struct spt_entry {
  void *vaddr;                    /* Virtual address (page-aligned) */
  void *kpage;                    /* Kernel page address when loaded */
  struct file *file;              /* File backing this page (NULL for stack) */
  off_t file_offset;              /* Offset in file */
  size_t read_bytes;              /* Bytes to read from file */
  swap_slot_t swap_slot;          /* Swap slot number (if swapped) */
  enum page_type type;            /* Type of page */
  enum page_status status;        /* Current status */
  mapid_t mapid;                  /* Mapping ID if this is an mmap page */
  bool writable;                  /* Whether page is writable */
  struct hash_elem hash_elem;     /* For SPT hash table */
};
```
**Purpose:** Tracks all information about a virtual page including location, type, and metadata for lazy loading.

```c
enum page_type {
  PAGE_FILE,             /* File-backed page (code/data) */
  PAGE_STACK,            /* Stack page */
  PAGE_MMAP              /* Memory-mapped file page */
};

enum page_status {
  PAGE_NOT_LOADED,       /* Page not in memory yet */
  PAGE_LOADED,           /* Page is in memory */
  PAGE_SWAPPED           /* Page is in swap */
};
```
**Purpose:** Enumerations to track page type and current load status for proper handling.

```c
struct hash spt;   /* In struct thread - Supplemental page table */
```
**Purpose:** Hash table in each thread storing all virtual pages for fast O(1) lookup.

#### ALGORITHMS

>A2: In a few paragraphs, describe your code for accessing the data stored in the SPT about a given page.

The SPT is implemented as a hash table keyed by virtual address. When we need to access SPT data for a page:

1. **Lookup Process**: `spt_lookup()` takes the virtual address, rounds it down to page boundary using `pg_round_down()`, creates a temporary SPT entry with that address, and uses `hash_find()` to locate the real entry.

2. **Hash Function**: The hash function `spt_hash_func()` uses `hash_bytes()` on the virtual address to distribute entries evenly across hash buckets.

3. **Access Points**: SPT access occurs during page faults (to load pages), system calls (to validate addresses), process cleanup (to free resources), and memory mapping operations (to track file-backed pages).

The design provides O(1) average-case lookup time and handles all page types uniformly through the same interface.

>A3: How does your code coordinate accessed and dirty bits between kernel and user virtual addresses that alias a single frame, or alternatively how do you avoid the issue?

We avoid the aliasing issue by **never accessing user pages through kernel virtual addresses**. Our design ensures:

1. **Single Mapping**: Each frame is mapped only at its user virtual address, never through kernel aliases.

2. **Direct Hardware Access**: We use `pagedir_is_accessed()` and `pagedir_is_dirty()` to read hardware bits directly from the user page table entry.

3. **Consistent Interface**: Frame eviction code always checks accessed/dirty bits through the user virtual address stored in `frame_entry->upage`, ensuring we see the actual hardware-maintained bits.

4. **No Kernel Aliases**: All user data access goes through proper user virtual addresses with appropriate validation, eliminating coordination issues entirely.

#### SYNCHRONIZATION

>A4: When two user processes both need a new frame at the same time, how are races avoided?

Frame allocation races are prevented through the **frame table lock**:

1. **Global Frame Table Lock**: `frame_table_lock` protects the entire frame allocation process.

2. **Atomic Allocation**: `frame_alloc()` first tries `palloc_get_page()` under lock. If successful, it immediately inserts the frame entry into the frame table before releasing the lock.

3. **Atomic Eviction**: If no free frames exist, `frame_evict_and_allocate()` acquires the lock, finds a victim using the clock algorithm, pins it, releases the lock during eviction, then reuses the frame.

4. **Victim Pinning**: During eviction, we set `victim->pinned = true` to prevent other processes from selecting the same victim while eviction is in progress.

This ensures only one process can allocate or evict frames at a time, preventing race conditions.

#### RATIONALE

>A5: Why did you choose the data structure(s) that you did for representing virtual-to-physical mappings?

We chose a **hash table for the SPT** for several reasons:

1. **Performance**: Hash tables provide O(1) average-case lookup, essential for page fault handling which must be fast.

2. **Memory Efficiency**: Unlike arrays indexed by virtual address, hash tables only store entries for actually used pages, saving significant memory.

3. **Flexibility**: Hash tables easily accommodate sparse virtual address spaces where processes use non-contiguous virtual memory regions.

4. **Scalability**: Performance doesn't degrade as the virtual address space size increases, unlike linear structures.

5. **Pintos Integration**: Pintos provides robust hash table implementation with proper memory management, reducing implementation complexity and bugs.

The hash table keyed by virtual address provides the optimal balance of speed, memory usage, and implementation simplicity for our needs.

## Paging To And From Disk

#### DATA STRUCTURES

>B1: Copy here the declaration of each new or changed struct or struct member, global or static variable, typedef, or enumeration.  Identify the purpose of each in 25 words or less.

```c
/* Frame table entry - stores metadata about an allocated user frame */
struct frame_entry {
  void *kpage;                 /* Kernel virtual address (hash key) */
  void *upage;                 /* User virtual address mapped to this frame */
  struct thread *owner;        /* Thread that owns this frame */
  bool pinned;                 /* Cannot be evicted (I/O in progress) */
  struct hash_elem hash_elem;  /* Hash table element */
};
```
**Purpose:** Tracks frame metadata for eviction algorithm including owner and pin status for synchronization.

```c
static struct hash frame_table;      /* Global frame table */
static struct lock frame_table_lock; /* Synchronizes frame operations */
static struct hash_iterator clock_iter; /* Clock hand for eviction */
static bool clock_iter_initialized;  /* Clock iterator state */
```
**Purpose:** Global frame management with hash table storage, lock protection, and clock algorithm state.

```c
typedef size_t swap_slot_t;
#define SWAP_ERROR ((swap_slot_t) -1)

static struct bitmap *swap_bitmap;    /* Bitmap of used swap slots */
static struct lock swap_lock;         /* Synchronizes swap operations */
static size_t swap_slot_count;        /* Total number of swap slots */
```
**Purpose:** Swap space management using bitmap allocation with error handling and synchronization.

#### ALGORITHMS

>B2: When a frame is required but none is free, some frame must be evicted. Describe your code for choosing a frame to evict.

Our eviction uses the **Clock (Second Chance) Algorithm**:

1. **Clock Hand**: `clock_iter` maintains position in frame table as circular iterator.

2. **Algorithm Steps**: 
   - Start from current clock position
   - For each frame: check if pinned (skip if true)
   - Check accessed bit using `frame_was_accessed()` 
   - If accessed=0: select as victim and advance clock hand
   - If accessed=1: clear bit with `frame_clear_accessed()` and continue

3. **Hardware Integration**: Uses `pagedir_is_accessed()` to read actual MMU-maintained accessed bits, providing accurate LRU approximation.

4. **Fallback**: If full cycle completes (all frames accessed), select first unpinned frame.

5. **Frame Pinning**: Skips frames with `pinned=true` to avoid evicting pages during I/O operations.

This approximates LRU by giving recently accessed pages a "second chance" while efficiently finding truly unused pages.

>B3: When a process P obtains a frame that was previously used by a process Q, how do you adjust the page table (and any other data structures) to reflect the frame Q no longer has?

The frame handover process in `frame_evict()` ensures complete cleanup:

1. **Q's Page Table**: Call `pagedir_clear_page(Q->pagedir, old_upage)` to remove mapping from Q's page table.

2. **Q's SPT Update**: Update Q's SPT entry:
   - Set `spt_entry->kpage = NULL`
   - Change `spt_entry->status` to `PAGE_NOT_LOADED` or `PAGE_SWAPPED`
   - Store swap slot if written to swap

3. **Frame Metadata**: Update frame table entry:
   - Change `frame_entry->upage` to P's virtual address
   - Change `frame_entry->owner` to process P
   - Keep same `frame_entry->kpage` (reusing physical frame)

4. **P's Setup**: When P's page fault completes:
   - P's SPT entry gets updated with new `kpage`
   - P's page table gets new mapping via `pagedir_set_page()`

This ensures Q loses all access to the frame while P gains exclusive access.

#### SYNCHRONIZATION

>B5: Explain the basics of your VM synchronization design. In particular, explain how it prevents deadlock. (Refer to the textbook for an explanation of the necessary conditions for deadlock.)

Our synchronization prevents deadlock by avoiding **circular wait**:

1. **Lock Hierarchy**: Fixed ordering prevents circular dependencies:
   - Frame table lock → File system lock → Swap lock
   - Never acquire locks in reverse order

2. **Minimal Lock Scope**: 
   - Frame table lock only during frame allocation/deallocation
   - Released during actual eviction I/O operations
   - Swap lock only during bitmap operations

3. **No Hold-and-Wait**: Process releases frame table lock before acquiring file system lock during eviction, preventing hold-and-wait condition.

4. **Pinning Mechanism**: Instead of holding locks during I/O, we use `frame->pinned` flag to prevent eviction without blocking other operations.

5. **Atomic Operations**: Critical sections are kept minimal - frame allocation, SPT updates, and bitmap operations are atomic.

The design ensures mutual exclusion while preventing all four deadlock conditions through careful lock ordering and minimal hold times.

>B6: A page fault in process P can cause another process Q's frame to be evicted. How do you ensure that Q cannot access or modify the page during the eviction process? How do you avoid a race between P evicting Q's frame and Q faulting the page back in?

**During Eviction Protection**:
1. **Immediate Page Table Removal**: `pagedir_clear_page()` removes Q's mapping before any I/O, causing immediate page faults for Q's access attempts.

2. **Frame Pinning**: Set `victim->pinned = true` before releasing frame table lock, preventing Q from loading page back into same frame.

3. **SPT Status Update**: Mark Q's SPT entry as `PAGE_SWAPPED` or `PAGE_NOT_LOADED` after successful eviction.

**Race Prevention**:
1. **Atomic State Transition**: Q's page table clearing and SPT status update happen atomically under frame table lock.

2. **Pin-Before-Release**: Frame is pinned before releasing lock, so if Q faults during eviction, it must allocate a different frame.

3. **Write-Before-Reuse**: Eviction completes swap write before reusing frame for P, ensuring Q's data is safely stored.

If Q faults during P's eviction, Q will get a different frame and load from swap, while P completes eviction safely.

>B7: Suppose a page fault in process P causes a page to be read from the file system or swap. How do you ensure that a second process Q cannot interfere by e.g. attempting to evict the frame while it is still being read in?

**Frame Protection During Loading**:

1. **Initial Pinning**: In `frame_alloc()`, newly allocated frames start with `pinned = true`.

2. **Pin Duration**: Frame remains pinned throughout entire loading process:
   - During `swap_read()` operations
   - During `file_read_at()` operations  
   - During page table installation

3. **Eviction Immunity**: `frame_find_victim()` skips all pinned frames, so Q cannot select P's loading frame as eviction victim.

4. **Unpin After Complete**: Only after successful `pagedir_set_page()` and full SPT update do we set `fe->pinned = false`.

5. **Cleanup on Failure**: If loading fails, frame is freed entirely rather than left in inconsistent state.

This ensures P's frame is completely loaded and mapped before becoming available for eviction by other processes.

>B8: Explain how you handle access to paged-out pages that occur during system calls. Do you use page faults to bring in pages (as in user programs), or do you have a mechanism for "locking" frames into physical memory, or do you use some other design? How do you gracefully handle attempted accesses to invalid virtual addresses?

**System Call Page Access Strategy**:

1. **Proactive Loading**: Before accessing user data, system calls use `is_valid_read_range()` which:
   - Validates address is in user space
   - Checks SPT for page existence  
   - Calls `ensure_page_loaded()` to load missing pages
   - Returns false for invalid addresses

2. **Page Fault Integration**: If proactive loading misses pages, normal page fault handler (`page_fault_handler()`) handles the fault and loads the page.

3. **Frame Pinning During I/O**: `pin_user_pages_for_copy()` temporarily pins frames during system call data transfer to prevent eviction mid-operation.

4. **Graceful Invalid Access Handling**:
   - Invalid addresses return false from validation functions
   - System calls terminate with appropriate error codes
   - No kernel panics for bad user addresses

5. **No Permanent Locking**: Pages are only pinned during active system call operations, not permanently locked in memory.

This hybrid approach ensures system calls work reliably while maintaining normal virtual memory flexibility.

#### RATIONALE

>B9: A single lock for the whole VM system would make synchronization easy, but limit parallelism. On the other hand, using many locks complicates synchronization and raises the possibility for deadlock but allows for high parallelism. Explain where your design falls along this continuum and why you chose to design it this way.

**Our Design: Moderate Parallelism with Safety**

**Lock Granularity**:
- **Frame table lock**: Protects frame allocation/deallocation
- **Swap lock**: Protects swap bitmap operations  
- **File system lock**: Protects file I/O (existing Pintos design)

**Design Rationale**:

1. **Safety First**: We prioritized correctness over maximum parallelism. VM bugs are catastrophic, so we chose well-understood locking patterns.

2. **Natural Boundaries**: Locks align with natural resource boundaries (frames, swap slots, files) making reasoning easier.

3. **Short Critical Sections**: Most locks are held briefly - frame allocation, bitmap updates, page table modifications.

4. **I/O Parallelism**: Frame table lock is released during actual disk I/O, allowing other frame operations to proceed.

5. **Pinning Over Locking**: Use frame pinning rather than extended lock holding to prevent interference during I/O.

**Trade-off**: We accept some serialization (frame allocation is serialized) in exchange for implementation simplicity and correctness. This is appropriate for an educational OS where understanding is more important than peak performance.

The design scales reasonably for typical workloads while remaining comprehensible and debuggable.