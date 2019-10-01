#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "devices/timer.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* The right most interger bit in fixed point format.
   It can be used to represent 1 in fixed point */
#define FIXED_U (1 << 14)

/* Convert interger INT to fixed point */
#define INT_TO_FIXED_POINT(INT) (fixed_t) ((INT) * (FIXED_U))

/* Convert fixed point FIXED_POINT to interger */
#define FIXED_POINT_TO_INT(FIXED_POINT) (int) ((FIXED_POINT) / (FIXED_U))

/* Queues of processes in THREAD_READY state, that is, processes that are ready
   to run but not actually running. N-th element in READY_QUEUES only stores
   processes that has (PRI_MIN + N) priority.  NOTE THAT interupt must disabled
   to approach this list because it can be changed inside in interupt. It may
   use many memory, so try to allocate heap memory if (PRI_MAX - PRI_MIN) is
   too large */
static struct list ready_queues[PRI_MAX - PRI_MIN + 1];

/* Number of processes in THREAD_READY state.  If process is added or removed
   from READY_QUEUES, this also have to modified. */
size_t ready_queues_size;

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread *idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread *initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame 
  {
    void *eip;                  /* Return address. */
    thread_func *function;      /* Function to call. */
    void *aux;                  /* Auxiliary data for function. */
  };

/* Statistics. */
static long long idle_ticks;    /* # of timer ticks spent idle. */
static long long kernel_ticks;  /* # of timer ticks in kernel threads. */
static long long user_ticks;    /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4            /* # of timer ticks to give each thread. */
static unsigned thread_ticks;   /* # of timer ticks since last yield. */

/* If false (default), use round-robin scheduler.
   If true, use multi-level feedback queue scheduler.
   Controlled by kernel command-line option "-o mlfqs". */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *running_thread (void);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static bool is_thread (struct thread *) UNUSED;
static void *alloc_frame (struct thread *, size_t size);
static void schedule (void);
void thread_schedule_tail (struct thread *prev);
static tid_t allocate_tid (void);
static void thread_priority_caculate (struct thread *t);
static void _mlfqs_set_priority (struct thread *t, void *aux UNUSED);
static void _mlfqs_set_recent_cpu (struct thread *t, void *aux UNUSED);
static bool is_waiting_priority_less (const struct list_elem *a,
                                      const struct list_elem *b,
                                      void *aux UNUSED);

/* Average number of threads ready to run over the past minute */
static fixed_t load_avg = 0;

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void
thread_init (void)
{
  int i;

  ASSERT (intr_get_level () == INTR_OFF);

  lock_init (&tid_lock);
  list_init (&all_list);

  ready_queues_size = 0;
  for (i = 0; i <= PRI_MAX - PRI_MIN; i++)
    list_init (&ready_queues[i]);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread ();
  init_thread (initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid ();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void
thread_start (void)
{
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init (&idle_started, 0);
  thread_create ("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable ();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down (&idle_started);
}

/* Recalculating priority of the input thread T based on RECENT_CPU and NICE
   value.
   Priority can be determined by this formula:
   priority = PRI_MAX - (recent_cpu / 4) - (nice * 2)

   Advanced scheduler must used and interrupt must disabled to call this
   function */
static void
_mlfqs_set_priority (struct thread *t, void *aux UNUSED)
{
  int priority;
  fixed_t pri_max = INT_TO_FIXED_POINT (PRI_MAX);
  fixed_t nice = INT_TO_FIXED_POINT (t->nice);

  ASSERT (thread_mlfqs);
  ASSERT (intr_get_level () == INTR_OFF);

  priority = FIXED_POINT_TO_INT (pri_max - (t->recent_cpu / 4) - (nice * 2));

  if (priority < PRI_MIN)
    priority = PRI_MIN;
  else if (priority > PRI_MAX)
    priority = PRI_MAX;

  if (t->priority != priority)
    {
      t->priority = priority;
      if (t->status == THREAD_READY && t != idle_thread)
        {
          /* If priority of thread changes, change queue */
          list_remove (&t->elem);
          list_push_back (&ready_queues[t->priority - PRI_MIN], &t->elem);
        }
    }
}

/* Recalculating the value RECENT_CPU of input thread T based on LOAD_AVG and
   NICE value.  RECENT_CPU is an estimate of the CPU time the thread has used
   recently.
   Priority can be determined by this formula:
   recent_cpu = (2*load_avg)/(2*load_avg + 1) * recent_cpu + nice

   Advanced scheduler must used to call this function. */
static void
_mlfqs_set_recent_cpu (struct thread *t, void *aux UNUSED)
{
  fixed_t nice = INT_TO_FIXED_POINT (t->nice);

  ASSERT (thread_mlfqs);

  t->recent_cpu
    = ((int64_t) 2 * load_avg) * t->recent_cpu / (2 * load_avg + FIXED_U)
      + nice;
}

/* Called by the timer interrupt handler at each timer tick.
   LOAD_AVG, RECENT_CPU, PRIORILY of threads will be updated when
   advanced scheduler is used.  This function runs in an external
   interrupt context. */
void
thread_tick (void)
{
  struct thread *t = thread_current ();
  /* Number of processes in THREAD_READY or THREAD_RUNNING state */
  int ready_threads = 0;

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pagedir != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return ();

  if (thread_mlfqs)
    {
      if (t != idle_thread)
        {
          /* On each timer tick, the RECENT_CPU of running thread is added
             by 1 */
          t->recent_cpu += FIXED_U;
          ready_threads++;
        }

      /* Recalculate priority of every threads every fourth clock tick */
      if (timer_ticks () % 4 == 0)
        thread_foreach (_mlfqs_set_priority, NULL);

      /* Update LOAD_AVG and recalculate RECENT_CPU when the system tick counter
         reaches a multiple of a second */
      if (timer_ticks () % TIMER_FREQ == 0)
        {
          ready_threads += ready_queues_size;
          load_avg = (59 * load_avg + ready_threads * FIXED_U) / 60;
          thread_foreach (_mlfqs_set_recent_cpu, NULL);
        }
    }
}

/* Prints thread statistics. */
void
thread_print_stats (void)
{
  printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
          idle_ticks, kernel_ticks, user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   If the priority of the new thread is higher, the current thread
   can be scheduled */
tid_t
thread_create (const char *name, int priority,
               thread_func *function, void *aux) 
{
  struct thread *t;
  struct kernel_thread_frame *kf;
  struct switch_entry_frame *ef;
  struct switch_threads_frame *sf;
  tid_t tid;

  ASSERT (function != NULL);

  /* Allocate thread. */
  t = palloc_get_page (PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread (t, name, priority);
  tid = t->tid = allocate_tid ();

  /* Stack frame for kernel_thread(). */
  kf = alloc_frame (t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame (t, sizeof *ef);
  ef->eip = (void (*) (void)) kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame (t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* Add to run queue. */
  thread_unblock (t);

  /* If the priority of the new thread is higher,
     yield the current thread */
  if (thread_get_priority () < t->priority)
    thread_yield ();

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. */
void
thread_block (void)
{
  ASSERT (!intr_context ());
  ASSERT (intr_get_level () == INTR_OFF);

  thread_current ()->status = THREAD_BLOCKED;
  schedule ();
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void
thread_unblock (struct thread *t)
{
  enum intr_level old_level;

  ASSERT (is_thread (t));

  old_level = intr_disable ();
  ASSERT (t->status == THREAD_BLOCKED);
  list_push_back (&ready_queues[t->priority - PRI_MIN], &t->elem);
  ready_queues_size++;
  t->status = THREAD_READY;
  intr_set_level (old_level);
}

/* Returns the name of the running thread. */
const char *
thread_name (void)
{
  return thread_current ()->name;
}

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. */
struct thread *
thread_current (void)
{
  struct thread *t = running_thread ();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT (is_thread (t));
  ASSERT (t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t
thread_tid (void)
{
  return thread_current ()->tid;
}

/* Deschedules the current thread and destroys it.  Never
   returns to the caller. */
void
thread_exit (void)
{
  ASSERT (!intr_context ());

#ifdef USERPROG
  process_exit ();
#endif

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_schedule_tail(). */
  intr_disable ();
  list_remove (&thread_current ()->allelem);
  thread_current ()->status = THREAD_DYING;
  schedule ();
  NOT_REACHED ();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. */
void
thread_yield (void)
{
  struct thread *cur = thread_current ();
  enum intr_level old_level;

  ASSERT (!intr_context ());

  old_level = intr_disable ();
  if (cur != idle_thread)
    {
      list_push_back (&ready_queues[cur->priority - PRI_MIN], &cur->elem);
      ready_queues_size++;
    }
  cur->status = THREAD_READY;
  schedule ();
  intr_set_level (old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void
thread_foreach (thread_action_func *func, void *aux)
{
  struct list_elem *e;

  ASSERT (intr_get_level () == INTR_OFF);

  for (e = list_begin (&all_list); e != list_end (&all_list);
       e = list_next (e))
    {
      struct thread *t = list_entry (e, struct thread, allelem);
      func (t, aux);
    }
}

/* This function helps thread_priority_donate and set the priority of
   threads for given input T based on priority inheritance. To caculate
   priority of thread T, this function chooses the largest priority among the
   threads that is blocked by the T and T's DEFAULT_PRIORITY. This function
   must be called with interrupts off. */
static void
thread_priority_caculate (struct thread *t)
{
  struct list_elem *e, *max_wait_elem;
  struct lock *lock;
  int max_waiting_priority, max_priority;

  ASSERT (t != NULL);
  ASSERT (!thread_mlfqs);
  ASSERT (intr_get_level () == INTR_OFF);

  max_priority = t->default_priority;

  for (e = list_begin (&t->holding_locks); e != list_end (&t->holding_locks);
       e = list_next (e))
    {
      /* For every lock which input T is holding, caculate the maximium
         priority of waiting elem */
      lock = list_entry (e, struct lock, elem);
      if (!list_empty (&lock->waiting_threads))
        {
          max_wait_elem
            = list_max (&lock->waiting_threads, is_waiting_priority_less, NULL);
          max_waiting_priority
            = list_entry (max_wait_elem, struct thread, waiting_elem)
                ->priority;

          /* If priority is larger than current's, donate it. */
          if (max_waiting_priority > max_priority)
            max_priority = max_waiting_priority;
        }
    }

  if (t->priority != max_priority)
    {
      t->priority = max_priority;
      if (t->status == THREAD_READY)
        {
          /* If priority is changed, change queue. */
          list_remove (&t->elem);
          list_push_back (&ready_queues[t->priority - PRI_MIN], &t->elem);
        }
    }
}

/* Evaluate the priority of threads for given input T and the every holder
   of lock which T is waiting for.  To caculate priority of thread, this
   function chooses the largest priority among the threads that is blocked
   by the it and its DEFAULT_PRIORITY.  Thus, priority of every thread
   should be exact except input T and the every holder of lock which T is
   waiting for. If advanced scheduler is used, this function do nothing.
   This function must be called with interrupts off if advanced scheduler
   is not used. */
void
thread_priority_donate (struct thread *t)
{
  if (thread_mlfqs)
    return;

  ASSERT (intr_get_level () == INTR_OFF);

  while (t != NULL)
    {
      thread_priority_caculate (t);

      if (t->waiting_lock == NULL)
        break;
      t = t->waiting_lock->holder;
    }
}

/* Sets the current thread's priority to NEW_PRIORITY.
   Note that if NEW_PRIORITY is smaller than current's, the current thread
   can be scheduled.
   Advanced scheduler must not used to call this function */
void
thread_set_priority (int new_priority)
{
  enum intr_level old_level;
  struct thread *t = thread_current ();
  int current_priority = t->priority;

  ASSERT (PRI_MIN <= new_priority && new_priority <= PRI_MAX);
  ASSERT (!thread_mlfqs);

  old_level = intr_disable ();
  t->default_priority = new_priority;
  thread_priority_donate (t);
  intr_set_level (old_level);

  if (current_priority > new_priority)
    thread_yield ();
}

/* Returns the current thread's priority. */
int
thread_get_priority (void)
{
  return thread_current ()->priority;
}

/* Sets the current thread's nice value to NICE and recalculates the
   thread's priority based on the new value.  If the running thread no
   longer has the highest priority, yields. */
void
thread_set_nice (int nice)
{
  enum intr_level old_level;
  struct thread *t = thread_current ();
  int current_priority = t->priority;

  ASSERT (thread_mlfqs);
  ASSERT (NICE_MIN <= nice && nice <= NICE_MAX);

  old_level = intr_disable ();
  t->nice = nice;
  _mlfqs_set_priority (t, NULL);
  intr_set_level (old_level);

  if (current_priority > t->priority)
    thread_yield ();
}

/* Returns the current thread's nice value. */
int
thread_get_nice (void)
{
  ASSERT (thread_mlfqs);

  return thread_current ()->nice;
}

/* Returns 100 times the system load average. */
int
thread_get_load_avg (void)
{
  ASSERT (thread_mlfqs);

  return FIXED_POINT_TO_INT (100 * load_avg);
}

/* Returns 100 times the current thread's recent_cpu value. */
int
thread_get_recent_cpu (void)
{
  ASSERT (thread_mlfqs);

  return FIXED_POINT_TO_INT (100 * thread_current ()->recent_cpu);
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void
idle (void *idle_started_ UNUSED)
{
  struct semaphore *idle_started = idle_started_;
  idle_thread = thread_current ();
  sema_up (idle_started);

  for (;;)
    {
      /* Let someone else run. */
      intr_disable ();
      thread_block ();

      /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
      asm volatile ("sti; hlt" : : : "memory");
    }
}

/* Function used as the basis for a kernel thread. */
static void
kernel_thread (thread_func *function, void *aux) 
{
  ASSERT (function != NULL);

  intr_enable ();       /* The scheduler runs with interrupts off. */
  function (aux);       /* Execute the thread function. */
  thread_exit ();       /* If function() returns, kill the thread. */
}

/* Returns the running thread. */
struct thread *
running_thread (void)
{
  uint32_t *esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm ("mov %%esp, %0" : "=g" (esp));
  return pg_round_down (esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool
is_thread (struct thread *t)
{
  return t != NULL && t->magic == THREAD_MAGIC;
}

/* Does basic initialization of T as a blocked thread named
   NAME. */
static void
init_thread (struct thread *t, const char *name, int priority)
{
  enum intr_level old_level;

  ASSERT (t != NULL);
  ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT (name != NULL);

  memset (t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy (t->name, name, sizeof t->name);
  t->stack = (uint8_t *) t + PGSIZE;
  t->priority = priority;
  t->default_priority = priority;
  t->nice = NICE_DEFAULT;
  t->magic = THREAD_MAGIC;
  t->waiting_lock = NULL;
  list_init (&t->holding_locks);

  old_level = intr_disable ();
  list_push_back (&all_list, &t->allelem);
  intr_set_level (old_level);
}

/* Allocates a SIZE-byte frame at the top of thread T's stack and
   returns a pointer to the frame's base. */
static void *
alloc_frame (struct thread *t, size_t size)
{
  /* Stack data is always allocated in word-size units. */
  ASSERT (is_thread (t));
  ASSERT (size % sizeof (uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/* Chooses and returns the next thread to be scheduled.  Should return a
   thread from the run queue that has largest priority in READY_LIST,
   unless the run queue is empty.  (If the running thread can continue
   running, then it will be in the run queue.)  If the run queue is empty,
   return idle_thread. */
static struct thread *
next_thread_to_run (void)
{
  struct list_elem *priority_max = NULL;
  int i;

  if (ready_queues_size == 0)
    return idle_thread;
  else
    {
      for (i = PRI_MAX - PRI_MIN; i >= 0; i--)
        {
          if (!list_empty (&ready_queues[i]))
            {
              priority_max = list_pop_front (&ready_queues[i]);
              ready_queues_size--;
              break;
            }
        }

      ASSERT (priority_max != NULL);
      return list_entry (priority_max, struct thread, elem);
    }
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_schedule() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void
thread_schedule_tail (struct thread *prev)
{
  struct thread *cur = running_thread ();

  ASSERT (intr_get_level () == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate ();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread)
    {
      ASSERT (prev != cur);
      palloc_free_page (prev);
    }
}

/* Schedules a new process.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_schedule_tail()
   has completed. */
static void
schedule (void)
{
  struct thread *cur = running_thread ();
  struct thread *next = next_thread_to_run ();
  struct thread *prev = NULL;

  ASSERT (intr_get_level () == INTR_OFF);
  ASSERT (cur->status != THREAD_RUNNING);
  ASSERT (is_thread (next));

  if (cur != next)
    prev = switch_threads (cur, next);
  thread_schedule_tail (prev);
}

/* Returns a tid to use for a new thread. */
static tid_t
allocate_tid (void)
{
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire (&tid_lock);
  tid = next_tid++;
  lock_release (&tid_lock);

  return tid;
}

/* Returns true if priority of lock holding thread A is less than B, or
   false if priority of thread A is greater than or equal to B. */
static bool
is_waiting_priority_less (const struct list_elem *a, const struct list_elem *b,
                          void *aux UNUSED)
{
  struct thread *thread_a = list_entry (a, struct thread, waiting_elem);
  struct thread *thread_b = list_entry (b, struct thread, waiting_elem);
  return thread_a->priority < thread_b->priority;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. */
uint32_t thread_stack_ofs = offsetof (struct thread, stack);
