#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "userprog/process.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "devices/input.h"
#include "filesys/filesys.h"
#include "filesys/file.h"

/* System call number used in system call handler */
#define NUMBER *((int *) is_user_bytes (f->esp, 4))

/* System call args used in system call handler */
#define ARG0 *((int *) is_user_bytes (f->esp + 4, 4))
#define ARG1 *((int *) is_user_bytes (f->esp + 8, 4))
#define ARG2 *((int *) is_user_bytes (f->esp + 12, 4))

static void *is_user_address (uint8_t *uaddr);
static void *is_user_bytes (void *uaddr, size_t size);
static char *user_str_dup (char *str);
static void *user_bytes_dup (void *uaddr, size_t size);
static int do_sys_open (char *filename);
static void syscall_handler (struct intr_frame *);

/* Used to synchronize multiple file approach. */
static struct lock filesys_lock;

/* Sets up the syscall and registers the corresponding interrupt. */
void
syscall_init (void)
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

/* Exit proccess with EXIT_ERROR if UADDR is a invalid user address.
   Return input address. */
static void *
is_user_address (uint8_t *uaddr)
{
  if (!is_user_vaddr (uaddr))
    thread_exit ();

  return uaddr;
}

/* Exit proccess with EXIT_ERROR if UADDR to (UADDR + SIZE) is a invalid
   user address. Return input address. */
static void *
is_user_bytes (void *uaddr, size_t size)
{
  size_t i;
  for (i = 0; i < size; i++)
    is_user_address (uaddr + i);

  return uaddr;
}

/* Returns a pointer to a null-terminated byte string, which is a duplicate
   of the user string pointed to by user address STR. The returned pointer
   must be passed to free to avoid a memory leak. If an error occurs, exit
   the proccess. Error occurs when input string is not in the user address
   or allocating memory fails. */
static char *
user_str_dup (char *str)
{
  struct thread *t = thread_current ();
  size_t size = 1;
  char *default_str = str;

  while (*((char *) is_user_address ((uint8_t *) str)) != '\0')
    {
      str++;
      size++;
    }

  str = malloc (size);
  if (str == NULL)
    thread_exit ();

  /* Be careful! Page fault may occur! */
  t->free_before_exit = str;
  memcpy (str, default_str, size);
  t->free_before_exit = NULL;

  return str;
}

/* Returns a pointer to a bytes which size is SIZE. This is a duplicate
   of the user bytes pointed to by user address UADDR which has size SIZE.
   The returned pointer must be passed to free to avoid a memory leak. If
   an error occurs, exit the proccess. Error occurs when input bytes is not
   in the user address or allocating memory fails. */
static void *
user_bytes_dup (void *uaddr, size_t size)
{
  struct thread *t = thread_current ();
  void *buffer;

  is_user_bytes (uaddr, size);

  buffer = malloc (size);
  if (size != 0 && buffer == NULL)
    thread_exit ();

  /* Be careful! Page fault may occur! */
  t->free_before_exit = buffer;
  memcpy (buffer, uaddr, size);
  t->free_before_exit = NULL;

  return buffer;
}

/* Returns true if FD is valid file descriptor, false otherwise */
static bool
is_valid_fd (struct file **fds, int fd)
{
  return !(fd >= FD_MAX || fd < FD_MIN || fds[fd] == NULL);
}

/* Allocate fd from current thread's fd table. Opens the file whose name is
   specified in the parameter FILENAME and associates it with fd that can
   be identified in future operations. If an error occurs, return
   FD_ERROR.*/
static int
do_sys_open (char *filename)
{
  int fd = FD_MIN;
  struct file *f;
  struct file **fds = thread_current ()->fds;

  while (fds[fd] != NULL && fd < FD_MAX)
    fd++;

  if (fd >= FD_MAX)
    return FD_ERROR;

  lock_acquire (&filesys_lock);
  f = filesys_open (filename);
  lock_release (&filesys_lock);
  if (f == NULL)
    return FD_ERROR;

  fds[fd] = f;
  return fd;
}

/* System call handler. The system call number is in the 32-bit word at the
   caller's stack pointer, the first argument is in the 32-bit word at the
   next higher address, and so on. */
static void
syscall_handler (struct intr_frame *f)
{
  struct thread *t = thread_current ();
  char *str = NULL;
  void *buffer = NULL;
  int32_t arg0, arg1, arg2;
  int32_t number = NUMBER;

  switch (number)
    {
    /* Terminates Pintos by calling shutdown_power_off(). This should be
       seldom used, because you lose some information about possible
       deadlock situations, etc. */
    case SYS_HALT:
      shutdown_power_off ();
      break;

    /* Terminates the current user program, returning status to the kernel.
       If the process's parent waits for it, this is the status that will
       be returned. This should be only place to determine exit status */
    case SYS_EXIT:
      arg0 = ARG0;
      t->exit_status = arg0;
      thread_exit ();
      break;

    /* Runs the executable whose name is given in arg0, passing any given
       arguments, and returns the new process's program id */
    case SYS_EXEC:
      arg0 = ARG0;
      str = user_str_dup ((char *) arg0);
      lock_acquire (&filesys_lock);
      f->eax = process_execute (str);
      lock_release (&filesys_lock);
      break;

    /* Waits for a child process pid and retrieves the child's exit status.
     */
    case SYS_WAIT:
      arg0 = ARG0;
      f->eax = process_wait (arg0);
      break;

    /* Creates a new file called file initially initial_size bytes in size.
       Returns true if successful, false otherwise. */
    case SYS_CREATE:
      arg0 = ARG0;
      arg1 = ARG1;
      if (arg1 < 0)
        thread_exit ();
      str = user_str_dup ((char *) arg0);
      lock_acquire (&filesys_lock);
      f->eax = filesys_create (str, (off_t) arg1);
      lock_release (&filesys_lock);
      break;

    /* Deletes the file called arg0. Returns true if successful, false
       otherwise. */
    case SYS_REMOVE:
      arg0 = ARG0;
      str = user_str_dup ((char *) arg0);
      lock_acquire (&filesys_lock);
      f->eax = filesys_remove (str);
      lock_release (&filesys_lock);
      break;

    /* Opens the file called arg0. Returns a nonnegative integer handle
       called a "file descriptor" (fd), or -1 if the file could not be
       opened. */
    case SYS_OPEN:
      arg0 = ARG0;
      str = user_str_dup ((char *) arg0);
      f->eax = do_sys_open (str);
      break;

    /* Returns the size, in bytes, of the file open as fd. */
    case SYS_FILESIZE:
      arg0 = ARG0;
      if (!is_valid_fd (t->fds, arg0))
        thread_exit ();
      lock_acquire (&filesys_lock);
      f->eax = file_length (t->fds[arg0]);
      lock_release (&filesys_lock);
      break;

    /* Reads size bytes from the file open as fd into buffer. Returns the
       number of bytes actually read (0 at end of file), or -1 if the file
       could not be read (due to a condition other than end of file). Fd
       STDIN_FILENO reads from the keyboard using input_getc(). */
    case SYS_READ:
      arg0 = ARG0;
      arg1 = ARG1;
      arg2 = ARG2;
      buffer = user_bytes_dup ((uint8_t *) arg1, (size_t) arg2);
      if (arg0 == STDIN_FILENO)
        {
          size_t i;
          for (i = 0; i < (size_t) arg2; i++)
            *((uint8_t *) (buffer + i)) = input_getc ();
          f->eax = arg2;
          memcpy ((void *) arg1, buffer, arg2);
        }
      else
        {
          if (arg2 < 0 || !is_valid_fd (t->fds, arg0))
            {
              free (buffer);
              thread_exit ();
            }
          lock_acquire (&filesys_lock);
          f->eax = file_read (t->fds[arg0], buffer, (off_t) arg2);
          lock_release (&filesys_lock);
          memcpy ((void *) arg1, buffer, arg2);
        }
      break;

    /* Writes size bytes from buffer to the open file fd. Returns the
       number of bytes actually written, which may be less than size if
       some bytes could not be written. Fd STDOUT_FILENO writes to the
       console. */
    case SYS_WRITE:
      arg0 = ARG0;
      arg1 = ARG1;
      arg2 = ARG2;
      buffer = user_bytes_dup ((uint8_t *) arg1, (size_t) arg2);
      if (arg0 == STDOUT_FILENO)
        {
          putbuf (buffer, (size_t) arg2);
          f->eax = arg2;
        }
      else
        {
          if (!is_valid_fd (t->fds, arg0) || arg2 < 0)
            {
              free (buffer);
              thread_exit ();
            }
          lock_acquire (&filesys_lock);
          f->eax = file_write (t->fds[arg0], buffer, (off_t) arg2);
          lock_release (&filesys_lock);
        }
      break;

    /* Changes the next byte to be read or written in open file fd to
       position, expressed in bytes from the beginning of the file. */
    case SYS_SEEK:
      arg0 = ARG0;
      arg1 = ARG1;
      if (!is_valid_fd (t->fds, arg0) || arg1 < 0)
        thread_exit ();
      lock_acquire (&filesys_lock);
      file_seek (t->fds[arg0], arg1);
      lock_release (&filesys_lock);
      break;

    /* Returns the position of the next byte to be read or written in open
       file fd, expressed in bytes from the beginning of the file. */
    case SYS_TELL:
      arg0 = ARG0;
      if (!is_valid_fd (t->fds, arg0))
        thread_exit ();
      lock_acquire (&filesys_lock);
      f->eax = file_tell (t->fds[arg0]);
      lock_release (&filesys_lock);
      break;

    /* Closes file descriptor fd. Exiting or terminating a process
       implicitly closes all its open file descriptors, as if by calling
       this function for each one. */
    case SYS_CLOSE:
      arg0 = ARG0;
      if (!is_valid_fd (t->fds, arg0))
        thread_exit ();
      lock_acquire (&filesys_lock);
      file_close (t->fds[arg0]);
      lock_release (&filesys_lock);
      t->fds[arg0] = NULL;
      break;

    default:
      thread_exit ();
      break;
    }

  free (str);
  free (buffer);
}
