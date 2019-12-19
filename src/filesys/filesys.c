#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format)
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  cache_init ();
  inode_init ();
  free_map_init ();

  if (format)
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void)
{
  destroy_block_cache ();
  free_map_close ();
}

/* Creates a file that is specified by NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool
filesys_create (const char *name, off_t initial_size)
{
  block_sector_t inode_sector = 0;
  char filename[NAME_MAX + 1];
  struct dir *dir = parse_path (thread_current ()->working_dir, name, filename);
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, false)
                  && dir_add (dir, filename, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);

  return success;
}

/* Creates a directory that is specified by PATH and add preserved
   directory to created DIR.  Returns true if successful, false otherwise.
   Fails if a file named PATH already exists, or if internal memory
   allocation fails. */
bool
filesys_dir_create (const char *path)
{
  block_sector_t inode_sector = 0;
  block_sector_t parent_sector;
  struct dir *parent_dir;
  char pathname[NAME_MAX + 1];
  struct dir *dir = parse_path (thread_current ()->working_dir, path, pathname);
  bool success = (dir != NULL && free_map_allocate (1, &inode_sector)
                  && dir_create (inode_sector, DEFAULT_DIR_ENTRY)
                  && dir_add (dir, pathname, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);

  /* Add preserved directory to created DIR */
  if (success)
    {
      parent_dir = dir_reopen (dir);
      parent_sector = inode_get_inumber (dir_get_inode (parent_dir));
      if (!dir_follow (dir, pathname)
          || !dir_add (dir, ".", inode_sector)
          || !dir_add (dir, "..", parent_sector))
        {
          dir_remove (parent_dir, pathname);
          dir_close (parent_dir);
          return false;
        }
      dir_close (parent_dir);
    }
  dir_close (dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  char filename[NAME_MAX + 1];
  struct dir *dir = parse_path (thread_current ()->working_dir, name, filename);
  struct inode *inode = NULL;

  if (dir != NULL)
    dir_lookup (dir, filename, &inode);
  dir_close (dir);

  return file_open (inode);
}

/* Deletes the file that is specified by NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name)
{
  char filename[NAME_MAX + 1];
  struct dir *dir = parse_path (thread_current ()->working_dir, name, filename);
  bool success = dir != NULL && dir_remove (dir, filename);
  dir_close (dir);

  return success;
}

/* Change current thread's directory that is specified by PATH.
   Returns true if successful, false otherwise.
   Fails if a directory specified PATH doesn't exists. */
bool
change_working_dir (struct thread *t, const char *path)
{
  char pathname[NAME_MAX + 1];
  struct dir *dir = parse_path (t->working_dir, path, pathname);

  if (dir == NULL)
    return false;

  if (!dir_follow (dir, pathname))
    {
      dir_close (dir);
      return false;
    }

  dir_close (t->working_dir);
  t->working_dir = dir;
  return true;
}

/* Formats the file system. */
static void
do_format (void)
{
  struct dir *dir;

  printf ("Formatting file system...");
  free_map_create ();

  /* Create root directory */
  if (!dir_create (ROOT_DIR_SECTOR, DEFAULT_DIR_ENTRY))
    PANIC ("root directory creation failed");

  /* Add preserved directory to root directory */
  dir = dir_open_root ();
  if (!dir_add (dir, ".", ROOT_DIR_SECTOR)
      || !dir_add (dir, "..", ROOT_DIR_SECTOR))
    PANIC ("Adding preserved directory to root directory failed");
  dir_close (dir);

  free_map_close ();
  printf ("done.\n");
}
