#ifndef VM_VM_H
#define VM_VM_H

#include <hash.h>
#include <list.h>
#include "filesys/file.h"

#define MAP_FAILED -1

/* Map region identifier. */
typedef int mapid_t;

/* Table of virtaul memory which holds hash table virtual memory and list
   of vm_map. */
struct vm_table
{
  struct hash vm_hash;
  struct list vm_map_list;
  mapid_t next_mapid; /* Next mapid value which uniquely identifies vm_map */
};

struct vm_map;

void vm_init (void);
struct vm_table *alloc_vm_table (void);
void distroy_vm_table (struct vm_table *vm_table);
struct vm_map *mapid_to_vm_map (struct vm_table *vm_table, mapid_t mapid);
mapid_t vm_map_to_mapid (struct vm_map *vm_map);
bool mmap_a_page (struct vm_table *vm_table,
                  uint32_t *pagedir, uint8_t *upage);
struct vm_map *mmap_segment (struct vm_table *vm_table, struct file *file,
                             off_t ofs, uint32_t *pagedir, uint8_t *upage,
                             uint32_t read_bytes, uint32_t zero_bytes,
                             bool writable, bool write_on_close);
void munmap (struct vm_table *vm_table, struct vm_map *mapped_list);
bool load_uaddr (struct vm_table *vm_table, void *uaddr);

#endif
