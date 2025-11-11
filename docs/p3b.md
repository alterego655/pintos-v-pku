# Project 3b: Virtual Memory

## Preliminaries

>Fill in your name and email address.

FirstName LastName <email@domain.example>

>If you have any preliminary comments on your submission, notes for the TAs, please give them here.

This implementation provides robust stack growth with proper security checks and comprehensive memory mapping support with write-back semantics. All synchronization follows the established lock hierarchy from Project 3a.

>Please cite any offline or online sources you consulted while preparing your submission, other than the Pintos documentation, course text, lecture notes, and course staff.

Implementation based on standard virtual memory techniques and UNIX mmap semantics.

## Stack Growth

#### ALGORITHMS

>A1: Explain your heuristic for deciding whether a page fault for an
>invalid virtual address should cause the stack to be extended into
>the page that faulted.

Our stack growth heuristic implements multiple security and validity checks:

**1. ESP Proximity Check (32-byte rule):**
The fault address must be within 32 bytes below the effective ESP. This supports the PUSHA instruction which can access up to 32 bytes below the stack pointer. We use saved `user_esp` for kernel faults to handle system calls correctly:
```c
if ((uint8_t *) fault_addr < (uint8_t *) effective_esp - 32) {
  return false;  /* Too far below ESP */
}
```

**2. Contiguous Growth Validation:**
The fault must be below the current stack bottom to ensure contiguous stack growth:
```c
if (fault_addr >= cur->stack_bottom) {
  return false;  /* Not below current stack */
}
```

**3. 8MB Stack Size Limit:**
We enforce the standard stack size limit:
```c
if (new_stack_size > MAX_STACK_SIZE) {
  return false;  /* Stack would exceed 8MB limit */
}
```

**4. Valid Address Range:**
The new stack page must be in valid user space and above the maximum stack region:
```c
if (new_stack_page < (void *) (PHYS_BASE - MAX_STACK_SIZE)) {
  return false;  /* Below valid stack region */
}
```

**5. Page-by-Page Growth:**
We create SPT entries for all pages between the current stack bottom and the fault address, ensuring no gaps in the stack region.

## Memory Mapped Files

#### DATA STRUCTURES

>B1: Copy here the declaration of each new or changed struct or struct member, global or static variable, typedef, or enumeration.  Identify the purpose of each in 25 words or less.

```c
/* Memory mapping entry for mmap/munmap tracking */
struct mmap_entry {
  mapid_t mapid;                      /* Unique mapping identifier */
  struct file *file;                  /* File being mapped */
  void *start_addr;                   /* Starting virtual address */
  size_t page_count;                  /* Number of pages in mapping */
  struct list_elem elem;              /* For process mmap list */
};
```
**Purpose:** Tracks each memory mapping for munmap and process cleanup.

```c
/* Map region identifier for memory mapped files */
typedef int mapid_t;
#define MAP_FAILED ((mapid_t) -1)
```
**Purpose:** Type for mapping identifiers, with error value for failed mappings.

```c
enum page_type {
    PAGE_FILE,             /* File-backed page (code/data) */
    PAGE_STACK,            /* Stack page */
    PAGE_MMAP              /* Memory-mapped file page */
};
```
**Purpose:** PAGE_MMAP distinguishes memory-mapped pages from regular file pages for different eviction behavior.

```c
/* In struct spt_entry */
mapid_t mapid;                  /* Mapping ID if this is an mmap page */
```
**Purpose:** Links SPT entries back to their mapping for munmap operations.

```c
/* In struct thread */
struct list mmap_list;          /* List of memory mappings */
mapid_t next_mapid;            /* Next mapping ID to assign */
```
**Purpose:** Per-process tracking of memory mappings for cleanup and munmap operations.

#### ALGORITHMS

>B2: Describe how memory mapped files integrate into your virtual
>memory subsystem.  Explain how the page fault and eviction
>processes differ between swap pages and other pages.

**Integration with Virtual Memory:**

Memory mapped files integrate seamlessly with our lazy loading and eviction system through the SPT and frame management:

1. **mmap() System Call:**
   - Creates `mmap_entry` to track the mapping
   - Creates SPT entries with `PAGE_MMAP` type for each page
   - Uses `file_reopen()` to get independent file reference
   - Does NOT immediately load pages (lazy loading)

2. **Page Fault Handling:**
   - Page faults on mmap regions trigger `spt_load_page()`
   - Same loading mechanism as regular file pages
   - `load_from_file_or_zero()` handles both `PAGE_FILE` and `PAGE_MMAP` identically
   - Page becomes resident in memory with dirty/accessed bit tracking

**Eviction Process Differences:**

The eviction behavior varies significantly by page type:

**Swap Pages (PAGE_STACK, PAGE_FILE):**
```c
case PAGE_STACK:
case PAGE_FILE:
  need_swap = true;  /* Always write to swap when evicted */
  /* ... swap allocation and write ... */
  spt_entry->status = PAGE_SWAPPED;
```

**Memory Mapped Pages (PAGE_MMAP):**
```c
case PAGE_MMAP:
  if (dirty) {
    /* Write back to file immediately */
    file_write_at(spt_entry->file, victim->kpage, 
                  spt_entry->read_bytes, spt_entry->file_offset);
    frame_clear_dirty(victim);
  }
  need_swap = false;  /* Never swap mmap pages */
  spt_entry->status = PAGE_NOT_LOADED;
```

**Key Differences:**
- **Swap vs File:** Stack/file pages go to swap; mmap pages write back to original file
- **Dirty Tracking:** Only dirty mmap pages require write-back; clean pages can be discarded
- **Status After Eviction:** Swapped pages become `PAGE_SWAPPED`; mmap pages become `PAGE_NOT_LOADED`
- **Reload Behavior:** Swapped pages reload from swap; mmap pages reload from file

>B3: Explain how you determine whether a new file mapping overlaps
>any existing segment.

We check for overlaps at multiple levels:

**1. SPT-Based Overlap Detection:**
```c
static bool mmap_is_valid_addr(void *addr, size_t length) {
  for (uint8_t *page = start; page < end; page += PGSIZE) {
    if (spt_lookup(&cur->spt, page) != NULL)
      return false;  /* Page already exists */
  }
  return true;
}
```

**2. Page-Aligned Address Validation:**
```c
if (addr == NULL || !is_user_vaddr(addr) || pg_ofs(addr) != 0) {
  f->eax = MAP_FAILED;
  return;
}
```

**3. Validation Sequence:**
- Check address is non-NULL, in user space, and page-aligned
- Verify file descriptor is valid and file size > 0
- Calculate required page count: `(file_size + PGSIZE - 1) / PGSIZE`
- Check each page in range `[addr, addr + page_count * PGSIZE)` against SPT
- Reject mapping if ANY page already exists

**Overlap Prevention Strategy:**
- **Code Segment:** SPT entries from executable loading prevent overlap
- **Data Segment:** Global variables have SPT entries preventing overlap  
- **Stack Region:** Stack pages in SPT prevent overlap
- **Existing Mappings:** Previous mmap calls create SPT entries preventing overlap
- **Heap Region:** Any allocated pages appear in SPT

This comprehensive SPT check catches ALL possible overlaps since every valid page in the process address space has an SPT entry.

#### RATIONALE

>B4: Mappings created with "mmap" have similar semantics to those of
>data demand-paged from executables, except that "mmap" mappings are
>written back to their original files, not to swap.  This implies
>that much of their implementation can be shared.  Explain why your
>implementation either does or does not share much of the code for
>the two situations.

**Our implementation DOES share significant code between mmap and executable file pages:**

**Shared Components:**

1. **SPT Entry Structure:** Both use identical `spt_entry` fields (file, offset, read_bytes)
2. **Page Loading:** `load_from_file_or_zero()` handles both `PAGE_FILE` and `PAGE_MMAP` identically:
   ```c
   case PAGE_FILE:
   case PAGE_MMAP:
     /* Identical file reading and zero-filling logic */
   ```
3. **Page Fault Handling:** Same `spt_load_page()` path for both types
4. **Frame Management:** Identical allocation, pinning, and tracking
5. **Dirty/Accessed Bits:** Same hardware MMU bit management

**Key Difference - Eviction Behavior:**

The only significant difference is in `frame_evict()`:
```c
switch (spt_entry->type) {
  case PAGE_FILE:
    need_swap = true;    /* Executable pages -> swap */
    break;
  case PAGE_MMAP:  
    if (dirty) {
      file_write_at(...);  /* mmap pages -> original file */
    }
    need_swap = false;
    break;
}
```

**Rationale for Code Sharing:**

1. **Identical Memory Semantics:** Both are file-backed, lazily-loaded, with same permissions
2. **Same Data Flow:** File → Memory → MMU bits → eviction decision
3. **Common Infrastructure:** Both use SPT, frame table, and page directory
4. **Maintenance Benefits:** Single code path reduces bugs and complexity

**Why Different Eviction is Necessary:**

- **Executable Consistency:** Code/data pages must remain unchanged, so swap preserves exact content
- **Mmap Semantics:** Users expect modifications to persist in the file
- **File Sharing:** Multiple processes mapping same file should see consistent updates
- **Performance:** Writing to original file avoids double I/O (swap + file write)

This design achieves maximum code reuse while respecting the semantic differences between executable and mmap pages.