#include "vm/page.h"
#include <stdio.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "filesys/file.h"
#include "vm/frame.h"

/* Hash table helper functions */
static unsigned spt_hash_func(const struct hash_elem *e, void *aux);
static bool spt_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux);
static void spt_destroy_func(struct hash_elem *e, void *aux);

/* Helper functions */
static struct spt_entry *hash_elem_to_spt_entry(const struct hash_elem *e);
static bool load_from_file_or_zero(struct spt_entry *entry, void *kpage);

/* Initialize supplemental page table */
void
spt_init(struct hash *spt)
{
    ASSERT(spt != NULL);
    hash_init(spt, spt_hash_func, spt_less_func, NULL);
}

/* Destroy supplemental page table and all entries */
void
spt_destroy(struct hash *spt)
{
    ASSERT(spt != NULL);
    hash_destroy(spt, spt_destroy_func);
}

/* Create a new SPT entry */
struct spt_entry *
spt_create_entry(void *vaddr, enum page_type type, bool writable)
{
    ASSERT(vaddr != NULL);
    ASSERT(pg_ofs(vaddr) == 0); /* Must be page-aligned */
    
    struct spt_entry *entry = malloc(sizeof(struct spt_entry));
    if (entry == NULL)
        return NULL;
        
    /* Initialize entry */
    entry->vaddr = vaddr;
    entry->type = type;
    entry->status = PAGE_NOT_LOADED;
    entry->writable = writable;
    
    /* File information - will be set later if needed */
    entry->file = NULL;
    entry->file_offset = 0;
    entry->read_bytes = 0;
    
    /* Frame information */
    entry->kpage = NULL;
    
    /* Swap information */
    entry->swap_slot = SWAP_ERROR;
    
    return entry;
}

/* Insert SPT entry into hash table */
bool
spt_insert(struct hash *spt, struct spt_entry *entry)
{
    ASSERT(spt != NULL);
    ASSERT(entry != NULL);
    
    struct hash_elem *result = hash_insert(spt, &entry->hash_elem);
    return result == NULL; /* NULL means successful insertion */
}

/* Look up SPT entry by virtual address */
struct spt_entry *
spt_lookup(struct hash *spt, const void *vaddr)
{
    ASSERT(spt != NULL);
    ASSERT(vaddr != NULL);
    
    /* Create temporary entry for lookup */
    struct spt_entry temp_entry;
    temp_entry.vaddr = pg_round_down((void *)vaddr); /* Ensure page-aligned */
    
    struct hash_elem *e = hash_find(spt, &temp_entry.hash_elem);
    return e != NULL ? hash_elem_to_spt_entry(e) : NULL;
}

/* Remove SPT entry from hash table */
void
spt_remove(struct hash *spt, void *vaddr)
{
    ASSERT(spt != NULL);
    ASSERT(vaddr != NULL);
    
    struct spt_entry *entry = spt_lookup(spt, vaddr);
    if (entry != NULL) {
        hash_delete(spt, &entry->hash_elem);
        
        /* Clean up frame if loaded */
        if (entry->status == PAGE_LOADED && entry->kpage != NULL) {
            spt_unload_page(entry);
        }
        
        free(entry);
    }
}

/* Set file data for file-backed pages */
bool
spt_set_file_data(struct spt_entry *entry, struct file *file, 
                  off_t offset, size_t read_bytes)
{
    ASSERT(entry != NULL);
    ASSERT(file != NULL);
    ASSERT(read_bytes <= PGSIZE);
    
    entry->file = file;
    entry->file_offset = offset;
    entry->read_bytes = read_bytes;
    
    return true;
}

/* Load a page into memory */
bool
spt_load_page(struct spt_entry *entry)
{
    ASSERT(entry != NULL);
    ASSERT(entry->status != PAGE_LOADED);
    
    /* Allocate frame - only zero for new pages, not swapped pages */
    enum palloc_flags flags = PAL_USER;
    if (entry->status == PAGE_NOT_LOADED)
        flags |= PAL_ZERO;
    
    void *kpage = frame_alloc(flags, entry->vaddr);
    if (kpage == NULL)
        return false;
        
    bool success = false;
    
    /* Load page data based on status */
    switch (entry->status) {
    case PAGE_NOT_LOADED:
        /* Load from file or zero-fill */
        success = load_from_file_or_zero(entry, kpage);
        break;
        
    case PAGE_SWAPPED:
        /* Load from swap */
        ASSERT(entry->swap_slot != SWAP_ERROR);
        swap_read(entry->swap_slot, kpage);
        swap_free(entry->swap_slot);
        entry->swap_slot = SWAP_ERROR;
        success = true;
        break;
        
    default:
        PANIC("Invalid page status for loading");
    }
    
    /* Install page in page table if loading succeeded */
    if (success) {
        if (pagedir_set_page(thread_current()->pagedir, entry->vaddr, 
                           kpage, entry->writable)) {
            entry->kpage = kpage;
            entry->status = PAGE_LOADED;
            /* Clear dirty bit to establish clean baseline after loading */
            struct frame_entry *fe = frame_lookup(kpage);
            if (fe != NULL) {
                frame_clear_dirty(fe);
            }
        } else {
            success = false;
        }
    }
    
    /* Clean up on failure */
    if (!success) {
        frame_free(kpage);
    }
    struct frame_entry *fe = frame_lookup(kpage);
    if (fe != NULL) 
        frame_unpin(fe);
    
    return success;
}

/* Helper function to load page from file or zero-fill */
static bool
load_from_file_or_zero(struct spt_entry *entry, void *kpage)
{
    size_t zero_bytes = PGSIZE - entry->read_bytes;
    switch (entry->type) {
    case PAGE_FILE:
        /* File-backed page */
        ASSERT(entry->file != NULL);
        
        /* Read data from file */
        if (entry->read_bytes > 0) {
            if (file_read_at(entry->file, kpage, entry->read_bytes, 
                           entry->file_offset) != (int) entry->read_bytes) {
                return false;
            }
        }
        
        /* Zero remaining bytes */
        if (zero_bytes > 0) {
            memset(kpage + entry->read_bytes, 0, zero_bytes);
        }
        
        return true;
        
    case PAGE_STACK:
        /* Stack page - zero-fill entire page */
        memset(kpage, 0, PGSIZE);
        return true;
        
    case PAGE_MMAP:
        /* Memory-mapped file - same as file-backed page */
        ASSERT(entry->file != NULL);
        
        /* Read data from file */
        if (entry->read_bytes > 0) {
            if (file_read_at(entry->file, kpage, entry->read_bytes, 
                           entry->file_offset) != (int) entry->read_bytes) {
                return false;
            }
        }
      
        if (zero_bytes > 0) {
            memset(kpage + entry->read_bytes, 0, zero_bytes);
        }
        
        return true;
        
    default:
        PANIC("Unknown page type");
    }
}

/* Unload a page from memory */
void
spt_unload_page(struct spt_entry *entry)
{
    ASSERT(entry != NULL);
    ASSERT(entry->status == PAGE_LOADED);
    ASSERT(entry->kpage != NULL);
    
    /* Remove from page table */
    pagedir_clear_page(thread_current()->pagedir, entry->vaddr);
    
    /* Free frame */
    frame_free(entry->kpage);
    
    /* Update entry status */
    entry->kpage = NULL;
    entry->status = PAGE_NOT_LOADED;
    
    /* Free swap slot if any (shouldn't happen during normal unload) */
    if (entry->swap_slot != SWAP_ERROR) {
        swap_free(entry->swap_slot);
        entry->swap_slot = SWAP_ERROR;
    }
}

/* Check if access to virtual address is valid */
bool
spt_is_valid_access(struct hash *spt, const void *vaddr, bool write)
{
    ASSERT(spt != NULL);
    ASSERT(vaddr != NULL);
    
    struct spt_entry *entry = spt_lookup(spt, vaddr);
    if (entry == NULL)
        return false;
        
    /* Check write permission */
    if (write && !entry->writable)
        return false;
        
    return true;
}

/* Print SPT entry for debugging */
void
spt_print_entry(struct spt_entry *entry)
{
    if (entry == NULL) {
        printf("SPT entry: NULL\n");
        return;
    }
    
    printf("SPT entry: vaddr=%p, type=%d, status=%d, writable=%d\n",
           entry->vaddr, entry->type, entry->status, entry->writable);
           
    if (entry->file != NULL) {
        printf("  file data: offset=%d, read_bytes=%zu, zero_bytes=%zu\n",
               (int) entry->file_offset, entry->read_bytes, PGSIZE - entry->read_bytes);
    }
    
    if (entry->kpage != NULL) {
        printf("  kpage=%p\n", entry->kpage);
    }
}

/* Hash function for SPT entries */
static unsigned
spt_hash_func(const struct hash_elem *e, void *aux UNUSED)
{
    const struct spt_entry *entry = hash_elem_to_spt_entry(e);
    return hash_bytes(&entry->vaddr, sizeof(entry->vaddr));
}

/* Less function for SPT entries */
static bool
spt_less_func(const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
    const struct spt_entry *entry_a = hash_elem_to_spt_entry(a);
    const struct spt_entry *entry_b = hash_elem_to_spt_entry(b);
    return entry_a->vaddr < entry_b->vaddr;
}

/* Destroy function for SPT entries */
static void
spt_destroy_func(struct hash_elem *e, void *aux UNUSED)
{
    struct spt_entry *entry = hash_elem_to_spt_entry(e);
    
    /* Unload page if loaded */
    if (entry->status == PAGE_LOADED && entry->kpage != NULL) {
        spt_unload_page(entry);
    }
    
    /* Free swap slot if swapped */
    if (entry->status == PAGE_SWAPPED && entry->swap_slot != SWAP_ERROR) {
        swap_free(entry->swap_slot);
    }
    
    free(entry);
}

/* Convert hash element to SPT entry */
static struct spt_entry *
hash_elem_to_spt_entry(const struct hash_elem *e)
{
    return hash_entry(e, struct spt_entry, hash_elem);
} 