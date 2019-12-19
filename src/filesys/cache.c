#include "filesys/cache.h"
#include <string.h>
#include "filesys/filesys.h"
#include "threads/malloc.h"

/* Cache which contains a SECTOR.  BUFFER contains the recent data of
   SECTOR.  If sector is modified from a disk, IS_MODIFIED is set to true.
   If sector is used recently, IS_DIRTY is set to true. */
struct cache
{
  block_sector_t sector;
  bool is_modified;
  bool is_dirty;
  void *buffer;
};

static struct cache cache[CACHE_SIZE];
/* "clock needle idx" to implement clock algorism. */
static size_t current_idx = 0;

static size_t find_cache_sector (block_sector_t sector);
static size_t swap_cache_sector (block_sector_t sector,
                                 const void *buffer);

/* Initializes the cache module. */
void
cache_init (void)
{
  size_t idx;

  for (idx = 0; idx < CACHE_SIZE; idx++)
    {
      cache[idx].buffer = malloc (BLOCK_SECTOR_SIZE);
      if (cache[idx].buffer == NULL)
        PANIC ("Cannot allocate disk cache");

      cache[idx].sector = ERROR_SECTOR;
      cache[idx].is_modified = false;
      cache[idx].is_dirty = false;
    }
}

/* Find index of input SECTOR.  Returns found cache index if success,
   ERROR_SECTOR otherwise */
static size_t
find_cache_sector (block_sector_t sector)
{
  size_t idx;

  for (idx = 0; idx < CACHE_SIZE; idx++)
    {
      if (cache[idx].sector == sector)
        return idx;
    }
  return ERROR_SECTOR;
}

/* Swap pre-allocated cahce with input SECTOR and BUFFER.  Returns allocated
   cache index. */
static size_t
swap_cache_sector (block_sector_t sector, const void *buffer)
{
  size_t idx;

  while (cache[current_idx].is_dirty)
    {
      cache[current_idx].is_dirty = false;
      if (++current_idx == CACHE_SIZE)
        current_idx = 0;
    }

  if (cache[current_idx].is_modified)
    block_write (fs_device, cache[current_idx].sector,
                 cache[current_idx].buffer);

  cache[current_idx].sector = sector;
  memcpy (cache[current_idx].buffer, buffer, BLOCK_SECTOR_SIZE);
  cache[current_idx].is_modified = false;

  idx = current_idx;
  if (++current_idx == CACHE_SIZE)
    current_idx = 0;

  return idx;
}

/* Reads sector SECTOR into BUFFER, which must have room for
   BLOCK_SECTOR_SIZE bytes. */
void
block_cache_read (block_sector_t sector, void *buffer)
{
  size_t idx;

  idx = find_cache_sector (sector);
  if (idx != ERROR_SECTOR)
    {
      memcpy (buffer, cache[idx].buffer, BLOCK_SECTOR_SIZE);
      cache[idx].is_dirty = true;
    }
  else
    {
      block_read (fs_device, sector, buffer);
      idx = swap_cache_sector (sector, buffer);
      cache[idx].is_dirty = true;
    }
}

/* Writes sector SECTOR into BUFFER, which must have room for
   BLOCK_SECTOR_SIZE bytes. */
void
block_cache_write (block_sector_t sector, const void *buffer)
{
  size_t idx;

  idx = find_cache_sector (sector);
  if (idx != ERROR_SECTOR)
    {
      memcpy (cache[idx].buffer, buffer, BLOCK_SECTOR_SIZE);
      cache[idx].is_dirty = true;
      cache[idx].is_modified = true;
    }
  else
    {
      idx = swap_cache_sector (sector, buffer);
      cache[idx].is_dirty = true;
      cache[idx].is_modified = true;
    }
}

/* Free all caches and writes to disk if it is modified. */
void
destroy_block_cache (void)
{
  size_t idx;

  for (idx = 0; idx < CACHE_SIZE; idx++)
    {
      if (cache[idx].is_modified)
        block_write (fs_device, cache[idx].sector, cache[idx].buffer);
      free (cache[idx].buffer);
    }
}
