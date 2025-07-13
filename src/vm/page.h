#ifndef VM_PAGE_H
#define VM_PAGE_H

#include <stddef.h>
#include <stdbool.h>
#include "lib/kernel/hash.h"
#include "lib/kernel/list.h"
#include "filesys/file.h"
#include "vm/swap.h"

/* Map region identifier for memory mapped files */
#ifndef MAPID_T_DEFINED
#define MAPID_T_DEFINED
typedef int mapid_t;
#endif

/* Page types */
enum page_type {
    PAGE_EXECUTABLE,        /* Code segment page */
    PAGE_DATA,             /* Data segment page */
    PAGE_STACK,            /* Stack page */
    PAGE_MMAP              /* Memory-mapped file page */
};

/* Page load status */
enum page_status {
    PAGE_NOT_LOADED,       /* Page not in memory yet */
    PAGE_LOADED,           /* Page is in memory */
    PAGE_SWAPPED           /* Page is in swap (future use) */
};

/* Memory mapping entry for mmap/munmap tracking */
struct mmap_entry {
    mapid_t mapid;                      /* Unique mapping identifier */
    struct file *file;                  /* File being mapped */
    void *start_addr;                   /* Starting virtual address */
    size_t page_count;                  /* Number of pages in mapping */
    struct list_elem elem;              /* For process mmap list */
};

/* Supplemental page table entry */
struct spt_entry {
    void *vaddr;                    /* Virtual address (page-aligned) */
    enum page_type type;            /* Type of page */
    enum page_status status;        /* Current status */
    bool writable;                  /* Whether page is writable */
    
    /* File-backed page information */
    struct file *file;              /* File backing this page (NULL for stack) */
    off_t file_offset;              /* Offset in file */
    size_t read_bytes;              /* Bytes to read from file */
    size_t zero_bytes;              /* Bytes to zero-fill */
    
    /* Memory mapping information (for PAGE_MMAP) */
    mapid_t mapid;                  /* Mapping ID if this is an mmap page */
    
    /* Frame information */
    void *kpage;                    /* Kernel page address when loaded */
    
    /* Swap information */
    swap_slot_t swap_slot;          /* Swap slot number (if swapped) */
    
    /* Hash table and list elements */
    struct hash_elem hash_elem;     /* For SPT hash table */
    struct list_elem list_elem;     /* For process page list */
};

/* SPT operations */
void spt_init(struct hash *spt);
void spt_destroy(struct hash *spt);

/* SPT entry management */
struct spt_entry *spt_create_entry(void *vaddr, enum page_type type, bool writable);
bool spt_insert(struct hash *spt, struct spt_entry *entry);
struct spt_entry *spt_lookup(struct hash *spt, const void *vaddr);
void spt_remove(struct hash *spt, void *vaddr);

/* File-backed page setup */
bool spt_set_file_data(struct spt_entry *entry, struct file *file, 
                       off_t offset, size_t read_bytes, size_t zero_bytes);

/* Page loading */
bool spt_load_page(struct spt_entry *entry);
void spt_unload_page(struct spt_entry *entry);

/* Utility functions */
bool spt_is_valid_access(struct hash *spt, const void *vaddr, bool write);
void spt_print_entry(struct spt_entry *entry);

#endif /* vm/page.h */ 