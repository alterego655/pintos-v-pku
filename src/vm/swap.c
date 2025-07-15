#include "vm/swap.h"
#include <stdio.h>
#include <bitmap.h>
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/block.h"

/* Sectors per page (each page is PGSIZE bytes, each sector is BLOCK_SECTOR_SIZE bytes) */
#define SECTORS_PER_PAGE (PGSIZE / BLOCK_SECTOR_SIZE)

/* Swap table state */
static struct bitmap *swap_bitmap;    /* Bitmap of used swap slots */
static struct block *swap_device;     /* Swap block device */
static struct lock swap_lock;         /* Synchronizes swap operations */
static size_t swap_slot_count;        /* Total number of swap slots */

/* Initialize swap table */
void
swap_init (void)
{
  /* Get swap device */
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device == NULL) 
    {
      printf ("Warning: No swap device found. Swap disabled.\n");
      swap_bitmap = NULL;
      swap_slot_count = 0;
      return;
    }
  
  /* Calculate number of swap slots */
  block_sector_t swap_sectors = block_size (swap_device);
  swap_slot_count = swap_sectors / SECTORS_PER_PAGE;
  
  printf ("Swap: %zu slots (%zu sectors, %zu MB)\n", 
          swap_slot_count, (size_t) swap_sectors, 
          (size_t) (swap_sectors * BLOCK_SECTOR_SIZE / 1024 / 1024));
  
  /* Create bitmap to track used slots */
  swap_bitmap = bitmap_create (swap_slot_count);
  if (swap_bitmap == NULL) 
    {
      PANIC ("Failed to create swap bitmap");
    }
      
  /* All slots initially free */
  bitmap_set_all (swap_bitmap, false);
  
  /* Initialize synchronization */
  lock_init (&swap_lock);
}

/* Destroy swap table */
void
swap_destroy (void)
{
  if (swap_bitmap != NULL) 
    {
      bitmap_destroy (swap_bitmap);
      swap_bitmap = NULL;
    }
  swap_device = NULL;
  swap_slot_count = 0;
}

/* Allocate a free swap slot */
swap_slot_t
swap_allocate (void)
{
  if (swap_bitmap == NULL) 
    {
      return SWAP_ERROR; /* No swap device */
    }
  
  lock_acquire (&swap_lock);
  
  /* Find first free slot */
  size_t slot = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  
  lock_release (&swap_lock);
  
  if (slot == BITMAP_ERROR) 
    {
      return SWAP_ERROR; /* Swap full */
    }
  
  return slot;
}

/* Free a swap slot */
void
swap_free (swap_slot_t slot)
{
  ASSERT (slot != SWAP_ERROR);
  ASSERT (swap_bitmap != NULL);
  ASSERT (slot < swap_slot_count);
  
  lock_acquire (&swap_lock);
  
  /* Mark slot as free */
  ASSERT (bitmap_test (swap_bitmap, slot)); /* Should be in use */
  bitmap_set (swap_bitmap, slot, false);
  
  lock_release (&swap_lock);
}

/* Write page to swap slot */
void
swap_write (swap_slot_t slot, void *page)
{
  ASSERT (slot != SWAP_ERROR);
  ASSERT (swap_device != NULL);
  ASSERT (slot < swap_slot_count);
  ASSERT (page != NULL);
  ASSERT (pg_ofs (page) == 0); /* Page-aligned */
  
  /* Calculate starting sector */
  block_sector_t sector = slot * SECTORS_PER_PAGE;
  
  /* Write page to device sector by sector */
  for (int i = 0; i < SECTORS_PER_PAGE; i++) 
    {
      block_write (swap_device, sector + i, 
                   (uint8_t *) page + i * BLOCK_SECTOR_SIZE);
    }
}

/* Read page from swap slot */
void
swap_read (swap_slot_t slot, void *page)
{
  ASSERT (slot != SWAP_ERROR);
  ASSERT (swap_device != NULL);
  ASSERT (slot < swap_slot_count);
  ASSERT (page != NULL);
  ASSERT (pg_ofs (page) == 0); /* Page-aligned */
  
  /* Calculate starting sector */
  block_sector_t sector = slot * SECTORS_PER_PAGE;
  
  /* Read page from device sector by sector */
  for (int i = 0; i < SECTORS_PER_PAGE; i++) 
    {
      block_read (swap_device, sector + i,
                  (uint8_t *) page + i * BLOCK_SECTOR_SIZE);
    }
}
