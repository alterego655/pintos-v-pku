#include "vm/frame.h"
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "lib/kernel/hash.h"
#include "userprog/pagedir.h"
#include "userprog/syscall.h"
#include "vm/page.h"
#include "vm/swap.h"

/* Frame table global variables */
static struct hash frame_table;
static struct lock frame_table_lock;
static size_t frame_count;

/* Clock algorithm state */
static struct hash_iterator clock_iter;
static bool clock_iter_initialized = false;

/* Forward declarations */
static unsigned frame_hash(const struct hash_elem *e, void *aux);
static bool frame_less(const struct hash_elem *a, const struct hash_elem *b, void *aux);


/* Initialize the frame table */
void
frame_table_init(void)
{
    hash_init(&frame_table, frame_hash, frame_less, NULL);
    lock_init(&frame_table_lock);
    frame_count = 0;
}

/* Allocate a frame and track it in frame table */
void *
frame_alloc (enum palloc_flags flags, void *upage)
{
  ASSERT (flags & PAL_USER); /* Must be user pool allocation */
  
  /* Try normal allocation first */
  void *kpage = palloc_get_page (flags);
  if (kpage != NULL) 
    {
      /* Normal path - frame available */
      /* Create frame table entry to track metadata */
      struct frame_entry *fe = malloc (sizeof (struct frame_entry));
      if (fe == NULL) 
        {
          palloc_free_page (kpage);
          PANIC ("frame_alloc: cannot allocate frame entry");
        }
      
      lock_acquire (&frame_table_lock);
      /* Initialize frame entry */
      fe->kpage = kpage;
      fe->upage = upage;
      fe->owner = thread_current ();
      fe->pinned = true;
      
      /* Add to frame table */
      hash_insert (&frame_table, &fe->hash_elem);
      frame_count++;
      
      lock_release (&frame_table_lock);
      
      /* Clear the frame if requested */
      if (flags & PAL_ZERO) 
        {
          memset (kpage, 0, PGSIZE);
        }
      
      return kpage;
    }
  /* No free frames - try eviction */
  return frame_evict_and_allocate (flags, upage);
}

/* Free a frame and remove from frame table */
void
frame_free (void *kpage)
{
  if (kpage == NULL)
    return;
  
  lock_acquire (&frame_table_lock);
  
  /* Find and remove frame entry */
  struct frame_entry lookup;
  lookup.kpage = kpage;
  struct hash_elem *e = hash_delete (&frame_table, &lookup.hash_elem);
  
  if (e == NULL) 
    {
      lock_release (&frame_table_lock);
      PANIC ("frame_free: frame not found in frame table");
    }
  
  frame_count--;
  lock_release (&frame_table_lock);
  
  /* Free the frame entry */
  struct frame_entry *fe = hash_entry (e, struct frame_entry, hash_elem);
  free (fe);
  
  /* Use existing palloc infrastructure for actual deallocation */
  palloc_free_page (kpage);
}

/* Look up frame table entry for a kernel page */
struct frame_entry *
frame_lookup (void *kpage)
{
  struct frame_entry lookup;
  lookup.kpage = kpage;
  
  lock_acquire (&frame_table_lock);
  struct hash_elem *e = hash_find (&frame_table, &lookup.hash_elem);
  lock_release (&frame_table_lock);
  
  return e != NULL ? hash_entry (e, struct frame_entry, hash_elem) : NULL;
}

/* Check if a page is tracked as a user frame */
bool
frame_is_user_page (void *kpage)
{
  return frame_lookup (kpage) != NULL;
}

/* Hash function for frame table */
static unsigned
frame_hash (const struct hash_elem *e, void *aux UNUSED)
{
  struct frame_entry *fe = hash_entry (e, struct frame_entry, hash_elem);
  return hash_bytes (&fe->kpage, sizeof (fe->kpage));
}

/* Comparison function for frame table */
static bool
frame_less (const struct hash_elem *a, const struct hash_elem *b, 
            void *aux UNUSED)
{
  struct frame_entry *fe_a = hash_entry (a, struct frame_entry, hash_elem);
  struct frame_entry *fe_b = hash_entry (b, struct frame_entry, hash_elem);
  return fe_a->kpage < fe_b->kpage;
}

/* EVICTION IMPLEMENTATION */

/* Evict a frame and allocate for new page */
void *
frame_evict_and_allocate (enum palloc_flags flags, void *upage)
{
  ASSERT (flags & PAL_USER);
  
  lock_acquire (&frame_table_lock);
  
  /* Find victim frame using clock algorithm */
  struct frame_entry *victim = frame_find_victim ();
  if (victim == NULL) 
    {
      lock_release (&frame_table_lock);
      return NULL;
    }
  
  /* Pin victim to prevent concurrent access */
  victim->pinned = true;
  
  /* Can release frame table lock during eviction */
  lock_release (&frame_table_lock);
  
  /* Perform eviction */
  if (!frame_evict (victim, upage)) 
    {
      /* Eviction failed (e.g., swap full) */
      lock_acquire (&frame_table_lock);
      victim->pinned = false;
      lock_release (&frame_table_lock);
      return NULL;
    }
  
  /* Reuse evicted frame */
  void *kpage = victim->kpage;
  
  /* Clear the frame if requested */
  if (flags & PAL_ZERO) 
    {
      memset (kpage, 0, PGSIZE);
    }
  
  return kpage;
}

/* Find victim frame using clock algorithm */
struct frame_entry *
frame_find_victim (void)
{
  ASSERT (lock_held_by_current_thread (&frame_table_lock));
  
  if (hash_empty (&frame_table)) 
    {
      return NULL;
    }
  
  /* Initialize clock iterator if needed */
  if (!clock_iter_initialized) 
    {
      hash_first (&clock_iter, &frame_table);
      clock_iter_initialized = true;
    }
  
  /* Clock algorithm: find frame with accessed bit = 0 and not pinned */
  struct hash_elem *start = hash_cur (&clock_iter);
  
  do 
    {
      /* Get current frame */
      struct hash_elem *e = hash_cur (&clock_iter);
      if (e == NULL) 
        {
          /* Reached end, wrap to beginning */
          hash_first (&clock_iter, &frame_table);
          e = hash_cur (&clock_iter);
          if (e == NULL) 
            {
              return NULL; /* Empty table */
            }
        }
      
      struct frame_entry *entry = hash_entry (e, struct frame_entry, 
                                               hash_elem);
      
      /* Skip pinned frames */
      if (entry->pinned) 
        {
          hash_next (&clock_iter);
          continue;
        }
      
      /* Check accessed bit */
      if (!frame_was_accessed (entry)) 
        {
          /* Found victim - advance clock hand */
          hash_next (&clock_iter);
          return entry;
        }
      
      /* Clear accessed bit and continue */
      frame_clear_accessed (entry);
      hash_next (&clock_iter);
      
    } 
  while (hash_cur (&clock_iter) != start);
  
  /* All frames are pinned or we made a full cycle - return first unpinned */
  hash_first (&clock_iter, &frame_table);
  struct hash_elem *e = hash_cur (&clock_iter);
  while (e != NULL) 
    {
      struct frame_entry *entry = hash_entry (e, struct frame_entry, 
                                               hash_elem);
      if (!entry->pinned) 
        {
          return entry;
        }
      hash_next (&clock_iter);
      e = hash_cur (&clock_iter);
    }
  
  return NULL;
}

/* Evict a frame and prepare it for reuse */
bool
frame_evict (struct frame_entry *victim, void *new_upage)
{
  ASSERT (victim != NULL);
  ASSERT (victim->pinned);
  
  /* Remove from page table */
  struct thread *owner = victim->owner;
  
  /* Find SPT entry */
  struct spt_entry *spt_entry = spt_lookup (&owner->spt, victim->upage);
  ASSERT (spt_entry != NULL);
  ASSERT (spt_entry->status == PAGE_LOADED);
  ASSERT (spt_entry->kpage == victim->kpage);
  
  /* Decide where to write page */
  bool dirty = frame_is_dirty (victim);
  
  pagedir_clear_page (owner->pagedir, victim->upage);
  
  bool need_swap = false;
  
  switch (spt_entry->type) 
    {
    /* Stack pages always go to swap when evicted */
    case PAGE_STACK:
    /* File-backed pages: always swap when evicted */
    case PAGE_FILE:
      need_swap = true;
      break;
      
    case PAGE_MMAP:
      /* Memory-mapped pages: always write back to file if dirty */
      if (dirty) 
        {
          /* Write back to file immediately - avoid deadlock by checking lock */
          bool already_held = fs_lock_held_by_current_thread ();
          if (!already_held) 
            fs_lock_acquire ();
          
          file_write_at (spt_entry->file, victim->kpage, 
                         spt_entry->read_bytes, spt_entry->file_offset);

          if (!already_held) 
            fs_lock_release ();
  
          /* Clear dirty bit since page is now clean after write-back */
          frame_clear_dirty (victim);
        }
      need_swap = false; /* Never swap mmap pages */
      break;
      
    default:
      PANIC ("Unknown page type in eviction");
    }
  
  if (need_swap) 
    {
      /* Write to swap */
      swap_slot_t slot = swap_allocate ();
      ASSERT (slot != SWAP_ERROR);
      swap_write (slot, victim->kpage);
      spt_entry->swap_slot = slot;
      spt_entry->status = PAGE_SWAPPED;
      frame_clear_dirty (victim);    
    } 
  else 
    {
      /* File-backed clean page - no write needed */
      spt_entry->status = PAGE_NOT_LOADED;
    }
  
  /* Update SPT */
  spt_entry->kpage = NULL;
  
  /* Update frame entry for new use */
  victim->upage = new_upage;
  victim->owner = thread_current ();
      
  return true;
}

/* Check if frame was accessed */
bool
frame_was_accessed (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  
  /* Check user mapping for accessed bit */
  return pagedir_is_accessed (entry->owner->pagedir, entry->upage);
}

/* Check if frame is dirty */
bool
frame_is_dirty (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  
  /* Check user mapping for dirty bit */
  return pagedir_is_dirty (entry->owner->pagedir, entry->upage);
}

/* Clear accessed bit */
void
frame_clear_accessed (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  
  /* Clear accessed bit in user mapping */
  pagedir_set_accessed (entry->owner->pagedir, entry->upage, false);
}

/* Clear dirty bit */
void
frame_clear_dirty (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  
  /* Clear dirty bit in user mapping */
  pagedir_set_dirty (entry->owner->pagedir, entry->upage, false);
}

/* Pin frame to prevent eviction */
void
frame_pin (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  lock_acquire (&frame_table_lock);
  entry->pinned = true;
  lock_release (&frame_table_lock);
}

/* Unpin frame to allow eviction */
void
frame_unpin (struct frame_entry *entry)
{
  ASSERT (entry != NULL);
  lock_acquire (&frame_table_lock);
  entry->pinned = false;
  lock_release (&frame_table_lock);
}
