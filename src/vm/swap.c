#include "vm/swap.h"
#include <debug.h>
#include <bitmap.h>
#include "devices/block.h"

/* Swap block device */
static struct block *swap_block;
/* Free map to represent swap partition. */
struct bitmap *swap_bitmap;

/* Initializes the swap partition. */
void
swap_init (void)
{
  swap_block = block_get_role (BLOCK_SWAP);
  if (swap_block == NULL)
    return;
  swap_bitmap = bitmap_create (block_size (swap_block) / PGSIZE_SECTOR);

  if (swap_bitmap == NULL)
    {
      swap_block = NULL;
      return;
    }
  bitmap_set_all (swap_bitmap, false);
}

/* Store input PAGE to swap partition. Returns index of swap which PAGE is
  stored if success, or return SWAP_ERROR if the initialize swap partition
  fails or no more address to store PAGE remains. This funtion is not
  thread-safe. */
swapid_t
page_to_swap (void *page)
{
  int i;

  ASSERT (pg_ofs (page) == 0);

  if (swap_block == NULL)
    return SWAP_ERROR;

  swapid_t idx = bitmap_scan_and_flip (swap_bitmap, 0, 1, false);
  if (idx == BITMAP_ERROR)
    return SWAP_ERROR;

  for (i = 0; i < PGSIZE_SECTOR; i++)
    block_write (swap_block, idx * PGSIZE_SECTOR + i,
                 page + i * BLOCK_SECTOR_SIZE);

  return idx;
}

/* Store IDX index of swap partition to input PAGE and deallocate IDX index
   of swap partition. If page is NULL, it just deallocate IDX index of swap
   partition. Returns true on success, false if initialize swap partition
   fails. This funtion is not thread-safe.*/
bool
swap_to_page (swapid_t idx, void *page)
{
  int i;

  ASSERT (pg_ofs (page) == 0);

  if (swap_block == NULL || idx == BITMAP_ERROR)
    return false;

  ASSERT (bitmap_test (swap_bitmap, idx));
  bitmap_set (swap_bitmap, idx, false);

  if (page == NULL)
    return true;

  for (i = 0; i < PGSIZE_SECTOR; i++)
    block_read (swap_block, idx * PGSIZE_SECTOR + i,
                page + i * BLOCK_SECTOR_SIZE);

  return true;
}
