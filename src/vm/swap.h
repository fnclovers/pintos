#ifndef VM_SWAP_H
#define VM_SWAP_H

#include <stdio.h>
#include "threads/vaddr.h"

/* Type to represent index of swap partition. */
typedef size_t swapid_t;

/* Number of block sector which need to store a page. */
#define PGSIZE_SECTOR (PGSIZE / BLOCK_SECTOR_SIZE)
#define SWAP_ERROR SIZE_MAX

void swap_init (void);
swapid_t page_to_swap (void *page);
bool swap_to_page (swapid_t idx, void *page);


#endif
