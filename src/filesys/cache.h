#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdio.h>

/* Cache size */
#define CACHE_SIZE 64

void cache_init (void);
void block_cache_read (block_sector_t sector, void *buffer);
void block_cache_write (block_sector_t sector, const void *buffer);
void destroy_block_cache (void);

#endif