#include "vm/vm.h"
#include <string.h>
#include <debug.h>
#include "userprog/pagedir.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "vm/swap.h"

/* Virtaul memory page which represent a page.
   VM is mapped with UPAGE that has PAGEDIR.
   If IDX is not SWAP_ERROR, page can be loaded in swap that has index IDX.
   Else if FILE is not NULL, page can be loaded reading PAGE_READ_BYTES
   bytes file starting at offset OFS.  If size of PAGE_READ_BYTES is
   smaller than page size, set the remain memory with zero.  Else if FILE
   is NULL, set all memory with zero.
   UPAGE must be page-aligned and because file pointer FILE is shared with
   other VM, do not modify it. */
struct vm
{
  struct hash_elem vm_hash_elem;
  struct list_elem vm_elem;
  struct list_elem vm_frame_elem;
  struct file *file;
  off_t ofs;
  swapid_t idx;
  size_t page_read_bytes;
  uint32_t *pagedir;
  uintptr_t upage;
  /* True if virtaul memory is writable, false otherwise. */
  bool memory_writable;
  /* True if virtaul memory memory is modified, false otherwise. */
  bool is_modified;
  /* True if virtaul memory should be written when virtaul memory is
     modified, false otherwise. */
  bool write_on_close;
};

/* Map of virtaul memory page which holds list of virtual memory. Each
   element share same file pointer FILE. */
struct vm_map
{
  struct list vm_list; /* List of VM mapped by this. */
  struct list_elem vm_map_elem;
  struct file *file;
  mapid_t mapid; /* VM map identifier. */
};

static unsigned vm_hash_func (const struct hash_elem *e, void *aux UNUSED);
static bool vm_less_func (const struct hash_elem *a,
                          const struct hash_elem *b, void *aux UNUSED);
static void vm_destroy (struct hash_elem *e, void *aux UNUSED);
static void *alloc_upage (void);
static bool load_vm (struct vm *vm);
static void destroy_vm (struct vm *vm);
static struct vm *search_vm (struct vm_table *vm_table, void *uaddr);

/* List of virtaul memory page which loaded in memory. */
static struct list frame_list;
static struct lock memory_lock;

/* "clock needle" to implement clock algorism. */
static struct vm *current_frame = NULL;

/* Initializes the virtaul memory module. */
void
vm_init (void)
{
  list_init (&frame_list);
  lock_init (&memory_lock);
  swap_init ();
}

/* Computes and returns the hash value for hash element E, given
   auxiliary data AUX. This funtion helps hash funtions. */
static unsigned
vm_hash_func (const struct hash_elem *e, void *aux UNUSED)
{
  return hash_entry (e, struct vm, vm_hash_elem)->upage;
}

/* Compares the value of two hash elements A and B, given
   auxiliary data AUX.  Returns true if A is less than B, or
   false if A is greater than or equal to B. This funtion helps hash
   funtions. */
static bool
vm_less_func (const struct hash_elem *a, const struct hash_elem *b,
              void *aux UNUSED)
{
  struct vm *vm_a = hash_entry (a, struct vm, vm_hash_elem);
  struct vm *vm_b = hash_entry (b, struct vm, vm_hash_elem);
  return vm_a->upage < vm_b->upage;
}

/* Help to destroy hash table.  Destroy hash element VM. */
static void
vm_destroy (struct hash_elem *e, void *aux UNUSED)
{
  struct vm *vm = hash_entry (e, struct vm, vm_hash_elem);
  destroy_vm (vm);
}

/* Allocate virtaul memory table and initialize it.  Returns an empty
   virtaul memory table on success, returns a null pointer if an allocation
   fails or initialize hash table fails. */
struct vm_table *
alloc_vm_table (void)
{
  struct vm_table *vm_table = malloc (sizeof (struct vm_table));
  if (vm_table == NULL)
    return NULL;

  if (!hash_init (&vm_table->vm_hash, vm_hash_func, vm_less_func, NULL))
    {
      free (vm_table);
      return NULL;
    }
  list_init (&vm_table->vm_map_list);
  vm_table->next_mapid = 1;
  return vm_table;
}

/* Distroy virtaul memory table. Free all element of virtaul memory table.
   It will close all file open by VM_TABLE and free input VM_TABLE and its
   element. If virtaul memory is loaded in memory or swap partition,
   deallocate it. */
void
distroy_vm_table (struct vm_table *vm_table)
{
  struct list_elem *e;
  struct vm_map *vm_map;

  if (vm_table == NULL)
    return;

  while (!list_empty (&vm_table->vm_map_list))
    {
      e = list_pop_front (&vm_table->vm_map_list);
      vm_map = list_entry (e, struct vm_map, vm_map_elem);
      munmap (vm_table, vm_map);
    }

  hash_destroy (&vm_table->vm_hash, vm_destroy);
  free (vm_table);
}

/* Adds a mapping from user virtual address UPAGE to a zeroed page. Returns
   true on success, false if UPAGE is already mapped or if memory
   allocation fails. */
bool
mmap_a_page (struct vm_table *vm_table, uint32_t *pagedir, uint8_t *upage)
{
  ASSERT (is_user_vaddr (upage));
  ASSERT (pg_ofs (upage) == 0);

  struct vm *vm;

  vm = malloc (sizeof (struct vm));
  if (vm == NULL)
    return false;
  vm->upage = (uintptr_t) upage;
  vm->file = NULL;
  vm->idx = SWAP_ERROR;
  vm->pagedir = pagedir;
  vm->memory_writable = true;
  vm->is_modified = false;
  vm->write_on_close = false;
  if (hash_insert (&vm_table->vm_hash, &vm->vm_hash_elem) != NULL)
    {
      free (vm);
      return false;
    }

  return true;
}

/* Map a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.
   Virtaul memory should be written when virtaul memory is modified if
   WRITE_ON_CLOSE is true, false otherwise.

   Returns a virtaul memory map mapped with this segment, or return a null
   pointer if the range of pages mapped overlaps any existing set of mapped
   pages. */
struct vm_map *
mmap_segment (struct vm_table *vm_table, struct file *file, off_t ofs,
              uint32_t *pagedir, uint8_t *upage, uint32_t read_bytes,
              uint32_t zero_bytes, bool writable, bool write_on_close)
{
  ASSERT (is_user_vaddr (upage));
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  size_t page_read_bytes;
  struct vm *vm;

  /* Create empty virtaul memory map */
  struct vm_map *vm_map = malloc (sizeof (struct vm_map));
  if (vm_map == NULL)
    return NULL;
  vm_map->file = file_reopen (file);
  if (vm_map->file == NULL)
    {
      free (vm_map);
      return NULL;
    }
  vm_map->mapid = vm_table->next_mapid++;
  list_init (&vm_map->vm_list);
  list_push_back (&vm_table->vm_map_list, &vm_map->vm_map_elem);

  while (read_bytes > 0 || zero_bytes > 0)
    {
      /* Calculate how to fill this page. */
      page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;

      vm = malloc (sizeof (struct vm));
      if (vm == NULL)
        {
          munmap (vm_table, vm_map);
          return NULL;
        }
      vm->upage = (uintptr_t) upage;
      vm->file = vm_map->file;
      vm->ofs = ofs;
      vm->idx = SWAP_ERROR;
      vm->page_read_bytes = page_read_bytes;
      vm->pagedir = pagedir;
      vm->memory_writable = writable;
      vm->is_modified = false;
      vm->write_on_close = write_on_close;
      if (hash_insert (&vm_table->vm_hash, &vm->vm_hash_elem) != NULL)
        {
          free (vm);
          munmap (vm_table, vm_map);
          return NULL;
        }
      list_push_back (&vm_map->vm_list, &vm->vm_elem);

      /* Advance. */
      ofs += page_read_bytes;
      read_bytes -= page_read_bytes;
      zero_bytes -= PGSIZE - page_read_bytes;
      upage += PGSIZE;
    }

  return vm_map;
}

/* Search vm_map from VM_TABLE with given MAPID. Returns a virtaul memory
   map if success, null pointer otherwise */
struct vm_map *
mapid_to_vm_map (struct vm_table *vm_table, mapid_t mapid)
{
  struct list_elem *e;
  struct vm_map *vm_map;
  for (e = list_begin (&vm_table->vm_map_list);
       e != list_end (&vm_table->vm_map_list); e = list_next (e))
    {
      vm_map = list_entry (e, struct vm_map, vm_map_elem);
      if (vm_map->mapid == mapid)
        return vm_map;
    }
  return NULL;
}

/* Get mapid number of given VM_MAP. If VM_MAP is a null pointer, returns
   MAP_FAILED. */
mapid_t
vm_map_to_mapid (struct vm_map *vm_map)
{
  if (vm_map == NULL)
    return MAP_FAILED;

  return vm_map->mapid;
}

/* Distroy virtaul memory map. It will close all file open by VM_MAP and
   free input VM_MAP and its element. If virtaul memory is loaded in memory
   or swap partition, deallocate it. */
void
munmap (struct vm_table *vm_table, struct vm_map *vm_map)
{
  struct list_elem *e;
  struct vm *vm;
  while (!list_empty (&vm_map->vm_list))
    {
      e = list_pop_front (&vm_map->vm_list);
      vm = list_entry (e, struct vm, vm_elem);
      hash_delete (&vm_table->vm_hash, &vm->vm_hash_elem);
      destroy_vm (vm);
    }

  list_remove (&vm_map->vm_map_elem);
  file_close (vm_map->file);
  free (vm_map);
}

/* Search a virtaul memory page from VM_TABLE which maps UPAGE. Returns
   found virtaul memory page if success, null pointer otherwise */
static struct vm *
search_vm (struct vm_table *vm_table, void *upage)
{
  struct vm tmp_vm;
  struct hash_elem *find_hash_elem;

  tmp_vm.upage = (uintptr_t) upage;
  find_hash_elem = hash_find (&vm_table->vm_hash, &tmp_vm.vm_hash_elem);

  if (find_hash_elem == NULL)
    return NULL;
  return hash_entry (find_hash_elem, struct vm, vm_hash_elem);
}

/* Search and load virtaul memory page to real memory which maps UADDR.
   Returns true on success, false if UADDR is already in memory or if
   memory allocation fails. */
bool
load_uaddr (struct vm_table *vm_table, void *uaddr)
{
  ASSERT (is_user_vaddr (uaddr));
  ASSERT (vm_table != NULL);

  struct vm *vm = search_vm (vm_table, pg_round_down (uaddr));

  if (vm == NULL
      || pagedir_get_page (vm->pagedir, (void *) vm->upage) != NULL)
    return false;

  return load_vm (vm);
}
/* Using clock algorism, search page that are going to replace. And set
   grobal variable "clock needle" to current page. This funtion should call
   with memory_lock because it directly access to frame list. Returns a
   virtaul memory page if success, null pointer otherwise */
static struct vm *
get_next_frame (void)
{
  struct list_elem *next_elem;
  struct vm *start_vm;

  ASSERT (lock_held_by_current_thread (&memory_lock));

  if (list_empty (&frame_list))
    return NULL;

  start_vm = list_entry (list_begin (&frame_list), struct vm, vm_frame_elem);

  if (current_frame == NULL)
    current_frame = start_vm;

  while (pagedir_is_dirty (current_frame->pagedir,
                           (void *) current_frame->upage))
    {
      current_frame->is_modified = true;
      pagedir_set_dirty (current_frame->pagedir,
                         (void *) current_frame->upage, false);

      next_elem = list_next (&current_frame->vm_frame_elem);
      if (next_elem == list_end (&frame_list))
        current_frame = start_vm;
      else
        current_frame = list_entry (next_elem, struct vm, vm_frame_elem);
    }

  return current_frame;
}

/* Remove a input virtual memoory page VM from frame list. If remove VM is
   same with clock needle, move forward clock needle once. Please use this
   funtion to remove VM from frame list because frame list is related to
   grobal variable "clock needle". This funtion should call with
   memory_lock because it directly access to frame list. */
static void
remove_frame (struct vm *vm)
{
  struct list_elem *next_elem;

  ASSERT (lock_held_by_current_thread (&memory_lock));

  if (vm == current_frame)
    {
      next_elem = list_next (&current_frame->vm_frame_elem);
      if (next_elem != list_end (&frame_list))
        current_frame = list_entry (next_elem, struct vm, vm_frame_elem);
      else
        current_frame = list_entry (list_begin (&frame_list),
                                         struct vm, vm_frame_elem);
    }

  list_remove (&vm->vm_frame_elem);

  if (list_empty (&frame_list))
    current_frame = NULL;
}

/* Allocate a user page. If there is no more page pool, try to allocate
   user page by switching with load memory. Existing memory will stored
   swap or file if it need. Returns kpage on success, null pointer
   otherwise. This funtion helps load_vm. */
static void *
alloc_upage (void)
{
  struct vm *vm;
  void *kpage;

  ASSERT (lock_held_by_current_thread (&memory_lock));

  /* First, try to allocate user page in page pool. */
  kpage = palloc_get_page (PAL_USER);
  if (kpage != NULL)
    return kpage;

  /* Second, try to allocate user page by switching with load memory. */
  vm = get_next_frame ();
  if (vm == NULL)
    return NULL;

  if (pagedir_is_dirty (vm->pagedir, (void *) vm->upage))
    vm->is_modified = true;

  kpage = pagedir_get_page (vm->pagedir, (void *) vm->upage);

  if (vm->is_modified)
    {
      if (vm->write_on_close)
        {
          if (file_write_at (vm->file, kpage, vm->page_read_bytes, vm->ofs)
              != (int) vm->page_read_bytes)
            return NULL;
          vm->is_modified = false;
        }
      else
        {
          vm->idx = page_to_swap (kpage);
          if (vm->idx == SWAP_ERROR)
            return NULL;
        }
    }

  remove_frame (vm);
  pagedir_clear_page (vm->pagedir, (void *) vm->upage);
  return kpage;
}

/* Load input virtual memory VM to memory and adds a mapping to the page
   table. Returns true on success, false otherwise. */
static bool
load_vm (struct vm *vm)
{
  lock_acquire (&memory_lock);
  uint8_t *kpage = alloc_upage ();
  if (kpage == NULL)
    goto error;

  /* First, try to load from swap partition. */
  if (vm->idx != SWAP_ERROR)
    {
      if (!swap_to_page (vm->idx, kpage))
        goto error;
      vm->idx = SWAP_ERROR;
    }
  /* Second, try to load from file. */
  else if (vm->file != NULL)
    {
      if (file_read_at (vm->file, kpage, vm->page_read_bytes, vm->ofs)
          != (int) vm->page_read_bytes)
        goto error;
      memset (kpage + vm->page_read_bytes, 0, PGSIZE - vm->page_read_bytes);
    }
  /* Third, fill memory with zero. */
  else
    memset (kpage, 0, PGSIZE);

  if (!pagedir_set_page (vm->pagedir, (void *) vm->upage, kpage,
                         vm->memory_writable))
    goto error;

  if (current_frame != NULL)
    list_insert (&current_frame->vm_frame_elem, &vm->vm_frame_elem);
  else
    list_push_back (&frame_list, &vm->vm_frame_elem);

  lock_release (&memory_lock);
  return true;

error:
  palloc_free_page (kpage);
  lock_release (&memory_lock);
  return false;
}

/* Free all element related to given virtual memory page except file
   structure. If the virtaul memory page is loaded in memory or swap
   partition, deallocate it. Be sure to remove virtual memory from hash
   table and mmap list before calling this funtion. */
static void
destroy_vm (struct vm *vm)
{
  uint8_t *kpage;

  lock_acquire (&memory_lock);
  if (pagedir_get_page (vm->pagedir, (void *) vm->upage) != NULL)
    {
      if (pagedir_is_dirty (vm->pagedir, (void *) vm->upage))
        vm->is_modified = true;

      kpage = pagedir_get_page (vm->pagedir, (void *) vm->upage);
      if (vm->is_modified && vm->write_on_close)
        file_write_at (vm->file, kpage, vm->page_read_bytes, vm->ofs);

      remove_frame (vm);
      pagedir_clear_page (vm->pagedir, (void *) vm->upage);
      palloc_free_page (kpage);
    }
  else if (vm->idx != SWAP_ERROR)
    swap_to_page (vm->idx, NULL);

  free (vm);
  lock_release (&memory_lock);
}
