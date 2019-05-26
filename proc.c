#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"

struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);


void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;
  p->time = 1;

  release(&ptable.lock);
  if (p->pid > 2)
      createSwapFile(p);
  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  return p;
}

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  p = allocproc();
  
  initproc = p;
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;

  release(&ptable.lock);
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();

  sz = curproc->sz;
  if(n > 0){
    if((sz = allocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(curproc->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  curproc->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }


   // copying swapFile and updating pgdir to np->pgdir
   if (myproc()->pid > 2)
       update_new_page_info_array(np, curproc);

  np->sz = curproc->sz;
  np->number_of_write_protected_pages = curproc->number_of_write_protected_pages;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

void update_new_page_info_array(struct proc *np, struct proc *curproc) {
    char page_data[PGSIZE];
    for (int i = 0; i < MAX_PSYC_PAGES; i++) {
        if (curproc->allocated_page_info[i].allocated == 1) {
            copy_page_info(&curproc->allocated_page_info[i], &np->allocated_page_info[i]);
            np->allocated_page_info[i].pgdir = np->pgdir;
            np->allocated_page_info[i].creation_time = np->time;
        }
        if (np->swapped_pages[i].allocated == 1) {
            if (readFromSwapFile(curproc, page_data, np->swapped_pages[i].page_offset_in_swapfile, PGSIZE) < 0) {
                cprintf("could not read from swap file\n");
            }
            writeToSwapFile(np, np->swapped_pages[i].virtual_address, i * PGSIZE, PGSIZE);
            copy_page_info(&curproc->swapped_pages[i], &np->swapped_pages[i]);
            np->swapped_pages[i].pgdir = np->pgdir;
            np->swapped_pages[i].creation_time = np->time;
        }
    }
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  int fd;

  if(curproc == initproc)
    panic("init exiting");

  // Close all open files.
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
      fileclose(curproc->ofile[fd]);
      curproc->ofile[fd] = 0;
    }
  }

  // close swapfile
  removeSwapFile(curproc);


  begin_op();
  iput(curproc->cwd);
  end_op();
  curproc->cwd = 0;

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }

  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
#ifdef VERBOSE_PRINT
    procdump();
#endif

    sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid;
  struct proc *curproc = myproc();
  
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->number_of_PGFLT=0;
        p->number_of_total_pages_out=0;
        p->number_of_write_protected_pages=0;
        memset(p->allocated_page_info, 0 ,sizeof(struct pages_info));
        memset(p->swapped_pages, 0 ,sizeof(struct pages_info));
        p->state = UNUSED;
        release(&ptable.lock);

        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct cpu *c = mycpu();
  c->proc = 0;
  
  for(;;){
    // Enable interrupts on this processor.
    sti();

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
        continue;

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = p;
      switchuvm(p);
      p->state = RUNNING;

      swtch(&(c->scheduler), p->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

struct pages_info * find_free_page_entry(struct pages_info * pages_info_table) {
    for (int i = 0; i< MAX_PSYC_PAGES; i++){
        if (!pages_info_table[i].allocated) // if we found a page without allocation
            return &pages_info_table[i];
    }
    return 0;
}

int find_index_of_page_info(struct pages_info *pages_info_table, struct pages_info *page_info_requested){
    for (int i = 0; i< MAX_PSYC_PAGES; i++){
        if (&pages_info_table[i] == page_info_requested) // if we found a page without allocation
            return i;
    }
    return 0;
}
void init_page_info(struct proc *proc, char* a, struct pages_info *page, int index) {
    page->allocated = 1;
    page->virtual_address = a;
    page->pgdir = proc->pgdir;
    page->page_offset_in_swapfile = index * PGSIZE;
    page->creation_time =  proc->time++;
}

struct pages_info * find_page_by_virtual_address(struct proc * proc, char* a){
    for (int i = 0; i< MAX_PSYC_PAGES; i++){
        if (proc->swapped_pages[i].allocated && proc->swapped_pages[i].virtual_address == a && proc->swapped_pages[i].pgdir == proc->pgdir) // if we found a page with the right address
            return &proc->swapped_pages[i];
    }
    return 0;
}

struct pages_info *find_a_page_to_swap(struct proc *proc) {
#ifdef LIFO
    return find_page_by_LIFO(proc);
#endif
#ifdef SCFIFO
    return find_page_by_SCFIFO(proc);
#endif
    return 0;
}

struct pages_info *find_page_by_LIFO(struct proc *proc) {
    struct pages_info *max_time_page = proc->allocated_page_info;
    for (int i = 0; i < MAX_PSYC_PAGES; i++) {
        if (proc->allocated_page_info[i].allocated) {
            if (proc->allocated_page_info[i].creation_time > max_time_page->creation_time) //find maximum time - last one in
                max_time_page = &proc->allocated_page_info[i];
        } // if we found an allocated page
    }
    return max_time_page;      // TODO - maybe need to check if found, else return 0;
}

struct pages_info *find_page_by_SCFIFO(struct proc *proc) {
    int min_creation_time = proc->time + 1;
    struct pages_info *min_time_page = proc->allocated_page_info;
    for (int i = 0; i < MAX_PSYC_PAGES; i++) {
        if (proc->allocated_page_info[i].allocated) // if we found an allocated page
            if (proc->allocated_page_info[i].creation_time < min_creation_time) { // find minimum time - first one in
                min_time_page = &proc->allocated_page_info[i];
                min_creation_time = proc->allocated_page_info[i].creation_time;
            }
    }
    if (check_page_flags(min_time_page->virtual_address,
                         PTE_A)) {//checking if accessed, if so returning to end of line by updating time andn turning off Accessed bit.
        turn_off_page_flags(min_time_page->virtual_address, PTE_A);
        min_time_page->creation_time = proc->time;
        proc->time++;
        return find_page_by_SCFIFO(proc);
    }
    return min_time_page;
}

void copy_page_info(struct pages_info * src, struct pages_info * dest){
    dest->allocated = 1;
    dest->virtual_address = src->virtual_address;
    dest->pgdir = src->pgdir;
    dest->page_offset_in_swapfile = src->page_offset_in_swapfile;
    dest->creation_time = src->creation_time;
}

int number_of_allocated_memory_pages(){
    return myproc()->sz / PGSIZE;
}

int number_of_paged_out_pages() {
    struct proc *proc = myproc();
    int counter = 0;
    for (int i = 0; i < MAX_PSYC_PAGES; i++) {
        if (proc->swapped_pages[i].allocated) // if we found a page with the right address
            counter++;
    }
    return counter;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
};
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

#ifdef LIFO
  cprintf("LIFO\n");
#endif
#ifdef SCFIFO
  cprintf("SCFIFO\n");
#endif
#ifdef NONE
    cprintf("NONE\n");
#endif
  uint total_pages = (PHYSTOP - 4 * 1024 * 1024) / PGSIZE;
  uint free_pages = total_pages;
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    int allocated_pages = number_of_allocated_memory_pages();
    int paged_out_pages = number_of_paged_out_pages();
    cprintf("%d %s %d %d %d %d %d %s", p->pid, state,allocated_pages,paged_out_pages,p->number_of_write_protected_pages,
            p->number_of_PGFLT,p->number_of_total_pages_out,p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
      cprintf("\n");
      free_pages -= p->sz / PGSIZE;
  }
    cprintf("%d / %d free pages in the system\n", free_pages, total_pages);
}
