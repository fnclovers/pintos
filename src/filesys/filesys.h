#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0       /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1       /* Root directory file inode sector. */
#define ERROR_SECTOR SIZE_MAX   /* Sector which idicate error. */
/* Default block sector. It cannot allocated with FREE_MAP_ALLOCATE. 0 is
   forced */
#define DEFAULT_SECTOR 0

/* Block device that contains the file system. */
struct block *fs_device;

struct thread;

void filesys_init (bool format);
void filesys_done (void);
bool filesys_create (const char *name, off_t initial_size);
bool filesys_dir_create (const char *path);
struct file *filesys_open (const char *name);
bool filesys_remove (const char *name);
bool change_working_dir (struct thread *t, const char *path);

#endif /* filesys/filesys.h */
