#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <stddef.h>
#include <stdbool.h>
#include "threads/palloc.h"
#include "threads/thread.h"
#include "lib/kernel/hash.h"

/* Frame table entry - stores metadata about an allocated user frame */
struct frame_entry {
    void *kpage;                 /* Kernel virtual address (hash key) */
    void *upage;                 /* User virtual address mapped to this frame */
    struct thread *owner;        /* Thread that owns this frame */
    bool pinned;                 /* Cannot be evicted (I/O in progress) */
    struct hash_elem hash_elem;  /* Hash table element */
};

/* Frame table operations */
void frame_table_init(void);
void *frame_alloc(enum palloc_flags flags, void *upage);
void frame_free(void *kpage);

/* Frame table queries */
struct frame_entry *frame_lookup(void *kpage);
bool frame_is_user_page(void *kpage);

/* Eviction support */
void *frame_evict_and_allocate(enum palloc_flags flags, void *upage);
struct frame_entry *frame_find_victim(void);
bool frame_evict(struct frame_entry *victim, void *new_upage);

/* Accessed and dirty bit management */
bool frame_was_accessed(struct frame_entry *entry);
bool frame_is_dirty(struct frame_entry *entry);
void frame_clear_accessed(struct frame_entry *entry);
void frame_clear_dirty(struct frame_entry *entry);

/* Frame pinning for synchronization */
void frame_pin(struct frame_entry *entry);
void frame_unpin(struct frame_entry *entry);

/* Debug functions */
void frame_print_stats(void);

#endif /* vm/frame.h */ 