#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "threads/malloc.h"

/* The information of command needs to excute command. */
struct command_info
{
  struct thread *parent_thread; /* List of child process. */
  struct semaphore sema;        /* Sema to synchronize parent and child. */
  bool success;                 /* Success of excution. */
  size_t size;                  /* Size of COMMAND. */
  /* Analized command. This should be sequence of args. 
     Example command: "/bin/ls -l foo bar\0"
     command = '/bin/ls\0-l\0foo\0bar\0'
  */
  char *command;
};

static size_t parse_command (char *command, const char *cmd_line,
                             size_t max_size);
static void construct_esp (struct command_info *cmd, void **esp);
static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);

/* Find every consecutive blanks in CMD_LINE and replace it with NULL
   character.  Result stored on COMMAND, that must be sequences of args
   that is splited with NULL character.  So that, starting pointer of
   command can be used to reprensent first argument which means the name of
   program.  If size of result is more than MAX_SIZE, only MAX_SIZE will be
   stored on COMMAND.  Return with the size of COMMAND. */
static size_t
parse_command (char *command, const char *cmd_line, size_t max_size)
{
  bool accept_state = false;
  size_t size = 0;

  while (size < max_size && *cmd_line != '\0')
    {
      if (accept_state && *cmd_line == ' ')
        {
          command[size++] = '\0';
          accept_state = false;
        }
      else if (*cmd_line != ' ')
        {
          command[size++] = *cmd_line;
          accept_state = true;
        }

      cmd_line++;
    }

  if (accept_state == true)
    {
      if (size < max_size)
        command[size++] = '\0';
      else
        command[max_size - 1] = '\0';
    }

  return size;
}

/* Starts a new thread running a user program loaded from input
   CMD_LINE.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *cmd_line)
{
  struct command_info cmd;
  tid_t tid;

  /* Analized CMD_LINE and store it to CMD. If CMD_LINE used
     directly, there's a race between the caller and load(). */
  cmd.command = palloc_get_page (0);
  if (cmd.command == NULL)
    return TID_ERROR;

  cmd.size = parse_command (cmd.command, cmd_line, PGSIZE/4);
  cmd.parent_thread = thread_current ();
  sema_init (&cmd.sema, 0);

  /* Create a new thread to execute CMD. */
  tid = thread_create (cmd.command, PRI_DEFAULT, start_process, &cmd);
  if (tid == TID_ERROR)
    {
      palloc_free_page (cmd.command);
      return TID_ERROR;
    }

  /* Wait to execute child and return TID of child. */
  sema_down (&cmd.sema);
  palloc_free_page (cmd.command);
  if (!cmd.success)
    return TID_ERROR;

  return tid;
}

/* Put the arguments for the initial function on the input *ESP stack
   before it allows the user program to begin executing. The arguments are
   passed in the same way as the normal calling convention.

   Example command: "/bin/ls -l foo bar"

     Address	   Name	          Data	      Type
     0xbffffffc	 argv[3][...]	  bar\0	      char[4]
     0xbffffff8	 argv[2][...]	  foo\0	      char[4]
     0xbffffff5	 argv[1][...]	  -l\0	      char[3]
     0xbfffffed	 argv[0][...]	  /bin/ls\0	  char[8]
     0xbfffffec	 word-align	    0       	  uint8_t
     0xbfffffe8	 argv[4]	      0	          char *
     0xbfffffe4	 argv[3]	      0xbffffffc	char *
     0xbfffffe0	 argv[2]	      0xbffffff8  char *
     0xbfffffdc	 argv[1]     	  0xbffffff5	char *
     0xbfffffd8	 argv[0]     	  0xbfffffed	char *
     0xbfffffd4	 argv	          0xbfffffd8	char **
     0xbfffffd0	 argc	          4	          int
     0xbfffffcc	 return address	0	          void (*)

*/
static void
construct_esp (struct command_info *cmd, void **esp)
{
  char *default_esp = *esp - 2;
  int argc = 0;

  /* First, push the sequences of args that is splited with NULL character,
     at the top of the stack. */
  *esp -= cmd->size;
  memcpy (*esp, cmd->command, cmd->size);

  /* Second, because word-aligned accesses are faster than unaligned
     accesses, so for best performance round the stack pointer down to a
     multiple of 4. */
  while ((uintptr_t) *esp % 4 != 0)
    {
      *--(*((uint8_t **) esp)) = 0;
    }

  /* Third, push the address of each string plus a null pointer sentinel,
     on the stack, in right-to-left order. */
  uintptr_t **int_esp = (uintptr_t **) esp;
  *--(*int_esp) = 0;

  while (true)
    {
      if (*default_esp-- == '\0')
        {
          *--(*int_esp) = (uintptr_t) default_esp + 2;
          argc++;
          if (*default_esp == '\0')
            break;
        }
    }

  /* Fourth, push argv (the address of argv[0]) and argc, in that order. */
  --(*int_esp);
  **int_esp = (uintptr_t) *int_esp + 4;
  *--(*int_esp) = argc;

  /* Finally, push a fake "return address" 0: although the entry function
     will never return, its stack frame must have the same structure as
     any other. */
  *--(*int_esp) = 0;
}

/* A thread function that loads a user process and starts it
   running. It also initialize the user process. */
static void
start_process (void *cmd_)
{
  struct command_info *cmd = cmd_;
  struct intr_frame if_;
  struct thread *t = thread_current ();
  struct thread *parent_t = cmd->parent_thread;
  char *file_name = cmd->command;
  bool success = false;

  /* Open executable file. */
  if ((t->execute_file = filesys_open (file_name)) == NULL)
    goto done;

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  if (!load (file_name, &if_.eip, &if_.esp))
    goto done;
  construct_esp (cmd, &if_.esp);

  /* Initialize child thread. */
  if ((t->fds = calloc (FD_MAX, sizeof (struct file *))) == NULL)
    goto done;

  success = true;
  t->has_parent_process = true;
  /* Cannot write execute file while program is running */
  file_deny_write (t->execute_file);
  list_push_back (&parent_t->child_process_list, &t->child_process_elem);
  sema_init (&t->parent_sema, 0);
  sema_init (&t->zombie_sema, 0);

done:
  cmd->success = success;
  sema_up (&cmd->sema);
  /* If load failed, quit. */
  if (!success)
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting. */
int
process_wait (tid_t child_tid)
{
  struct thread *t = thread_current ();
  struct thread *child_t;
  struct list_elem *e;
  int exit_status = EXIT_ERROR;

  if (child_tid == TID_ERROR)
    return exit_status;

  /* Find child in list and return child's exit status */
  for (e = list_begin (&t->child_process_list);
       e != list_end (&t->child_process_list); e = list_next (e))
    {
      child_t = list_entry (e, struct thread, child_process_elem);
      if (child_tid == child_t->tid)
        {
          list_remove (&child_t->child_process_elem);
          /* Wait until child process quit */
          sema_down (&child_t->parent_sema);
          exit_status = child_t->exit_status;
          /* Destroy child process descriptor */
          sema_up (&child_t->zombie_sema);
          break;
        }
    }

  return exit_status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  struct list_elem *e;
  int i;

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL)
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }

  /* Destroy all child process descriptor */
  for (e = list_begin (&cur->child_process_list);
       e != list_end (&cur->child_process_list); e = list_next (e))
    sema_up (
      &list_entry (e, struct thread, child_process_elem)->zombie_sema);

  /* Close files open by current thread */
  if (cur->fds != NULL)
    {
      for (i = FD_MIN; i < FD_MAX; i++)
        file_close (cur->fds[i]);
      free (cur->fds);
    }
  file_close (cur->execute_file);
  free (cur->free_before_exit);

  if (cur->has_parent_process)
    {
      printf ("%s: exit(%d)\n", thread_name (), cur->exit_status);
      /* Sema up parent's sema so that parent proccess stop waiting child
         proccess */
      sema_up (&cur->parent_sema);
      /* Do not destroy current thread while parent read its exit status */
      sema_down (&cur->zombie_sema);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *file_name, void (**eip) (void), void **esp)
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = t->execute_file;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL)
    goto done;
  process_activate ();

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done;
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++)
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type)
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file))
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                       - read_bytes);
                }
              else
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file)
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp) 
{
  uint8_t *kpage;
  bool success = false;

  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}
