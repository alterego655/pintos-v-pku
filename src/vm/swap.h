#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stddef.h>
#include <stdbool.h>
#include "devices/block.h"

/* Swap slot identifier */
typedef size_t swap_slot_t;
#define SWAP_ERROR ((swap_slot_t) -1)

/* Swap table operations */
void swap_init(void);
void swap_destroy(void);

/* Swap slot management */
swap_slot_t swap_allocate(void);
void swap_free(swap_slot_t slot);

/* Swap I/O operations */
void swap_read(swap_slot_t slot, void *page);
void swap_write(swap_slot_t slot, void *page);

/* Swap table queries */
size_t swap_get_free_slots(void);
size_t swap_get_total_slots(void);
bool swap_is_full(void);
bool swap_is_available(void);

#endif /* vm/swap.h */ 