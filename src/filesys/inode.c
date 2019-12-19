#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Size of indirect block used in a inode.
   It is linked with size of INODE_DISK which must be exactly BLOCK_SECTOR_SIZE
   bytes long. */
#define INDIRECT_BLOCK_SIZE 125
/* Size of block which one indirect block stores */
#define TABLE_SIZE (BLOCK_SECTOR_SIZE / sizeof (block_sector_t))

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long.
   Data must be stored using indirect way with increasing order.
   
     AN_INODE_DISK         A_BLOCK_TABLE
     |INDIRECT_BLOCK|  ->  |DATA_SECTOR|  ->  DATA1
     |INDIRECT_BLOCK|      |DATA_SECTOR|
     |INDIRECT_BLOCK|      |DATA_SECTOR|
     |...           |      |...        |
*/
struct inode_disk
{
  off_t length;   /* File size in bytes. */
  unsigned magic; /* Magic number. */
  int32_t is_dir; /* True if inode is directory, false otherwise. */
  /* The sector address which store block table */
  block_sector_t indirect_block[INDIRECT_BLOCK_SIZE];
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}

/* In-memory inode. */
struct inode
{
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
};

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Returns the IDX'th data sector of inode.
   If inode does not have a IDX'th data sector, behavior depends on CREATE.
   If CREATE is true, then allocate new data sector and returns it if 
   allocation is successful.  Otherwise, DEFAULT_SECTOR will be returned. */
static block_sector_t
lookup_disk_inode (block_sector_t inode_sector, size_t idx, bool create)
{
  size_t indirect_idx, table_idx; /* Index of IDX'th data sector */
  block_sector_t sector;
  block_sector_t lookup_sector = DEFAULT_SECTOR; /* IDX'th data sector */
  struct inode_disk *disk_inode = malloc (sizeof *disk_inode);
  void *zeros = malloc (BLOCK_SECTOR_SIZE);
  /* Block table which INDIRECT_BLOCK indicate */
  block_sector_t *table = malloc (sizeof (block_sector_t) * TABLE_SIZE);

  if (zeros == NULL || disk_inode == NULL || table == NULL)
    goto error;

  memset (zeros, DEFAULT_SECTOR, BLOCK_SECTOR_SIZE);

  /* Evaluate index of IDX'th data sector */
  indirect_idx = idx / TABLE_SIZE;
  table_idx = idx % TABLE_SIZE;

  /* TOO large IDX */
  if (indirect_idx >= INDIRECT_BLOCK_SIZE)
    goto error;

  /* Read indirect block */
  block_cache_read (inode_sector, disk_inode);

  if (disk_inode->indirect_block[indirect_idx] == DEFAULT_SECTOR)
    {
      if (create)
        {
          if (!free_map_allocate (1, &sector))
            goto error;
          block_cache_write (sector, zeros);
          disk_inode->indirect_block[indirect_idx] = sector;
          block_cache_write (inode_sector, disk_inode);
        }
      else
        goto error;
    }

  /* Read block table */
  block_cache_read (disk_inode->indirect_block[indirect_idx], table);

  if (create && table[table_idx] == DEFAULT_SECTOR)
    {
      if (!free_map_allocate (1, &sector))
        goto error;
      block_cache_write (sector, zeros);
      table[table_idx] = sector;
      block_cache_write (disk_inode->indirect_block[indirect_idx], table);
    }

  lookup_sector = table[table_idx];

error:
  free (table);
  free (zeros);
  free (disk_inode);
  return lookup_sector;
}

/* Frees all of allocated blocks in INODE including INODE_SECTOR. */
static void
destroy_disk_inode (block_sector_t inode_sector)
{
  size_t i, j;
  block_sector_t table[TABLE_SIZE];
  struct inode_disk disk_inode;

  block_cache_read (inode_sector, &disk_inode);

  /* Find all sectors that is not set to DEFAULT_SECTOR and release them */
  for (i = 0; i < INDIRECT_BLOCK_SIZE; i++)
    {
      if (disk_inode.indirect_block[i] != DEFAULT_SECTOR)
        {
          block_cache_read (disk_inode.indirect_block[i], table);
          for (j = 0; j < TABLE_SIZE; j++)
            {
              if (table[j] != DEFAULT_SECTOR)
                free_map_release (table[j], 1);
            }
          free_map_release (disk_inode.indirect_block[i], 1);
        }
    }
  free_map_release (inode_sector, 1);
}

/* Increase the size of inode to input LENGTH.
   Returns true if successful, false if the memory or block allocation
   fails. */
static bool
increase_inode_size (block_sector_t inode_sector, off_t length)
{
  size_t sectors, idx;
  struct inode_disk *disk_inode = malloc (sizeof *disk_inode);

  if (disk_inode == NULL)
    return false;
  block_cache_read (inode_sector, disk_inode);

  /* Resister all sectors to inode */
  sectors = bytes_to_sectors (length);
  for (idx = bytes_to_sectors (disk_inode->length); idx < sectors; idx++)
    {
      if (lookup_disk_inode (inode_sector, idx, true) == DEFAULT_SECTOR)
        {
          free (disk_inode);
          return false;
        }
    }

  /* DISK_INODE might changed due to lookup_disk_inode, so read it again */
  block_cache_read (inode_sector, disk_inode);
  disk_inode->length = length;
  block_cache_write (inode_sector, disk_inode);
  free (disk_inode);
  return true;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, bool is_dir)
{
  struct inode_disk *disk_inode = malloc (sizeof *disk_inode);

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  if (disk_inode == NULL)
    return false;

  memset (disk_inode, DEFAULT_SECTOR, sizeof *disk_inode);
  disk_inode->length = 0;
  disk_inode->magic = INODE_MAGIC;
  disk_inode->is_dir = is_dir;
  block_cache_write (sector, disk_inode);

  if (!increase_inode_size (sector, length))
    {
      destroy_disk_inode (sector);
      free (disk_inode);
      return false;
    }

  free (disk_inode);
  return true;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        destroy_disk_inode (inode->sector);

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx
        = lookup_disk_inode (inode->sector, offset / BLOCK_SECTOR_SIZE, false);
      if (sector_idx == DEFAULT_SECTOR)
        break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          block_cache_read (sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          block_cache_read (sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Write at end of file would extend the inode.
   Returns the number of bytes actually written, which may be
   less than SIZE if increase inode size fails or an error occurs. */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  /* If inode length is smaller than writing position, increase its size */
  if (inode_length (inode) < size + offset)
    {
      if (!increase_inode_size (inode->sector, size + offset))
        return 0;
    }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx
        = lookup_disk_inode (inode->sector, offset / BLOCK_SECTOR_SIZE, false);
      if (sector_idx == DEFAULT_SECTOR)
        break;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          block_cache_write (sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            block_cache_read (sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          block_cache_write (sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (struct inode *inode)
{
  struct inode_disk disk_inode;
  off_t length;

  block_cache_read (inode->sector, &disk_inode);
  length = disk_inode.length;
  return length;
}

/* Return true if INODE is valid directory inode, false otherwise 
   Fails if inode is not directory or inode is removed */
bool
inode_is_valid_dir (const struct inode *inode)
{
  struct inode_disk disk_inode;

  if (inode->removed)
    return false;

  block_cache_read (inode->sector, &disk_inode);
  return (disk_inode.is_dir != 0);
}