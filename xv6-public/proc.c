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

struct MLFQ_struct mlfq_s;
struct STRIDE_struct stride_s;
struct proc_list proc_l[NPROC];

static struct proc *initproc;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

double min_pass = 0;

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
  p->data.stride.swtch = stride_s.switch_num;
  p->sched_state = DEFAULT;
  release(&ptable.lock);
  
  if(p->pid == 1)
      push_list(p,0);
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

  np->sz = curproc->sz;
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
  if(np->pid != 1)
    push_list(np,0);

  release(&ptable.lock);

  return pid;
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

int
getlev(void){
    struct proc *p = myproc();
    if(p->sched_state == MLFQ)
        return p->data.mlfq.level-1;
    else
        return -1;
}

void
print_mlfq(void){
    struct proc_list *pl;
    cprintf("boost[%d]\nfirst[%d] : ",mlfq_s.boosting_period,mlfq_s.first.proc_num);
    pl = mlfq_s.first.start;
    while(pl != 0){
        cprintf("%d[%d][%d] ",pl->p->pid,pl->p->data.mlfq.level,pl->p->data.mlfq.exec_count);
        pl = pl->next;
    }
    cprintf("\nsecond[%d] : ",mlfq_s.second.proc_num);
    pl = mlfq_s.second.start;
    while(pl != 0){
        cprintf("%d[%d][%d] ",pl->p->pid,pl->p->data.mlfq.level,pl->p->data.mlfq.exec_count);
        pl = pl->next;
    }
    cprintf("\nthird[%d] : ",mlfq_s.third.proc_num);
    pl = mlfq_s.third.start;
    while(pl != 0){
        cprintf("%d[%d][%d] ",pl->p->pid,pl->p->data.mlfq.level,pl->p->data.mlfq.exec_count);
        pl = pl->next;
    }
    cprintf("\n\n");
}

void
mlfq_boosting(void){
    struct proc *p = 0;
    struct proc_list *pl = mlfq_s.first.start;

    while(pl != 0){
        pl->p->data.mlfq.exec_count = 0;
        pl = pl->next;
    }
    while(mlfq_s.third.proc_num > 0){
        p = mlfq_s.third.start->p;
        pop_list(p, 3);
        push_list(p, 1);
        p->data.mlfq.level = 1;
        p->data.mlfq.exec_count = 0;
    }

    while(mlfq_s.second.proc_num > 0){
        p = mlfq_s.second.start->p;
        pop_list(p, 2);
        push_list(p, 1);
        p->data.mlfq.level = 1;
        p->data.mlfq.exec_count = 0;
    }
    
    mlfq_s.boosting_period = 0;
}

struct proc*
mlfq_start(void){
    struct proc *p = 0;
    struct proc_list *temp = 0;
    struct proc_header *proc_h = 0;

    int cur_level = 0;
    int time_quantum[3] = {1,2,4};
    int time_allot[2] = {5,10};
    

    if(mlfq_s.first.proc_num > 0){
        proc_h = &mlfq_s.first;
        cur_level = 0;
    }else if(mlfq_s.second.proc_num > 0){
ltwo:        
        proc_h = &mlfq_s.second;
        cur_level = 1;
        goto out;
    }else if(mlfq_s.third.proc_num > 0){
lthree:
        proc_h = &mlfq_s.third;
        cur_level = 2;
        goto out;
    }

out:
    temp = proc_h->start;
    while(temp != 0){
        if(temp->p->state == RUNNABLE)
            break;
        temp = temp->next;
    }

    if(temp == 0){
        if(cur_level == 0)
            goto ltwo;
        else if(cur_level == 1)
            goto lthree;
        else if(cur_level == 2)
            return 0;
    }
    
    p = temp->p;
    p->data.mlfq.exec_count++;
    mlfq_s.pass += 50;
    mlfq_s.boosting_period++;
    //print_mlfq();

    //time_allotment check
    if(cur_level <= 1){
        if(p->data.mlfq.exec_count % time_allot[cur_level] == 0){
            //move down
            pop_list(p, cur_level+1);
            push_list(p, cur_level+2);
            p->data.mlfq.exec_count = 0;
            p->data.mlfq.level = cur_level+2;
            return p;
        }
    }

    //time_quantum check
    if(p->data.mlfq.exec_count % time_quantum[cur_level] == 0){
        //move back
        pop_list(p, cur_level+1);
        push_list(p, cur_level+1);
        if(cur_level == 2)
            p->data.mlfq.exec_count -= time_quantum[cur_level];
    }

    return p;
}

struct proc*
stride_start(void){

    struct proc_list* temp;
    int flap = 0;

again:
    flap = 0;

    temp = stride_s.list.start;

runnable:
    if(temp == 0){
        flap = 1;
        goto flp;
    }
    while(temp->p->data.stride.swtch != stride_s.switch_num){
        if(temp->next == 0){
            flap = 1;
            goto flp;
        }
        temp = temp->next;
    }
    if(temp->p->state != RUNNABLE){
        temp = temp->next;
        goto runnable;
    }

flp:
    if(flap == 1){
        stride_s.switch_num = 1 - stride_s.switch_num;
        stride_s.pass += stride_s.stride;
        stride_s.stride = return_stride();
        goto again;
    }else{
        temp->p->data.stride.swtch = 1 - stride_s.switch_num;
        return temp->p;
    }

    panic("stride_sched error");
}


int mlfq_total_num(void){
    return mlfq_s.first.proc_num + mlfq_s.second.proc_num + mlfq_s.third.proc_num;
}

double
return_stride(void){
    
    struct proc *p = 0;
    int share_percent = 0;
    int dp_count = 0;
    int mlfq_exist = mlfq_total_num() > 0 ? 1 : 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
            continue;
        if(p->sched_state == SHARE)
            share_percent += p->data.share.share;
        if(p->sched_state == DEFAULT)
            dp_count++;
    }
    if(dp_count == 0)
        return 0;
    return 1000/(((100-share_percent)-(20*mlfq_exist))/dp_count);
}

void init_mlfq(void){
    mlfq_s.pass = 0;
    mlfq_s.boosting_period = 0;
    mlfq_s.first.proc_num = 0;
    mlfq_s.second.proc_num = 0;
    mlfq_s.third.proc_num = 0;

    for(int i = 0; i < NPROC; i++){
        mlfq_s.first.start = 0;
        mlfq_s.first.end = 0;
        mlfq_s.second.start = 0;
        mlfq_s.second.end = 0;
        mlfq_s.third.start = 0;
        mlfq_s.third.end = 0;
    }
}

void init_stride(void){
    
    stride_s.list.start = 0;
    stride_s.list.end = 0;

    stride_s.pass = 0;
    stride_s.stride = 100;
    stride_s.switch_num = 0;
}

void push_list(struct proc *p, int num){
    int i = 0;
    struct proc_header *proc_h = 0;

    while(proc_l[i].use != 0)
        i++;
    if(i == NPROC)
        panic("use check error");

    proc_l[i].p = p;
    proc_l[i].next = 0;
    proc_l[i].use = 1;
    
    if(num == 0)
        proc_h = &stride_s.list;
    else if(num == 1)
        proc_h = &mlfq_s.first;
    else if(num == 2)
        proc_h = &mlfq_s.second;
    else if(num == 3)
        proc_h = &mlfq_s.third;

        if(proc_h->proc_num == 0){
            proc_h->start = &proc_l[i];
            proc_h->end = &proc_l[i];
        }else{
            proc_h->end->next = &proc_l[i];
            proc_h->end = &proc_l[i];
        }

        proc_h->proc_num++;
}

void pop_list(struct proc *p, int num){
    struct proc_header *proc_h = 0;
    struct proc_list *proc_temp = 0;

    if(num == 0)
        proc_h = &stride_s.list;
    else if(num == 1)
        proc_h = &mlfq_s.first;
    else if(num == 2)
        proc_h = &mlfq_s.second;
    else if(num == 3)
        proc_h = &mlfq_s.third;

    if(proc_h->start->p->pid == p->pid){
        proc_h->start->use = 0;
        proc_h->start = proc_h->start->next;
        if(proc_h->proc_num == 1)
            proc_h->end = 0;

    }else{
        proc_temp = proc_h->start;
        if(proc_temp->next == 0){
            cprintf("%d %d \n",proc_temp->p->data.mlfq.level,proc_temp->p->pid);
            cprintf("num:%d p->pid:%d h->pid%d\n",num,p->pid,proc_h->start->p->pid);
            print_mlfq();
            panic("no matching process");
        }

        while(proc_temp->next->p->pid != p->pid)
            proc_temp = proc_temp->next;
        
        proc_temp->next->use = 0;
        if(proc_temp->next->next == 0)
            proc_h->end = proc_temp;
        proc_temp->next = proc_temp->next->next;
    }

    proc_h->proc_num--;
}

void init_list(void){
    int i = 0;
    for(i = 0; i < NPROC; i++){
        proc_l[i].p = 0;
        proc_l[i].next = 0;
        proc_l[i].use = 0;
    }
}

void clear_list(void){
    int i = 0;

    for(i = 0; i< NPROC; i++){
        if(proc_l[i].p == 0 || proc_l[i].p->state == SLEEPING || proc_l[i].p->state == RUNNABLE)
            continue;

        if(proc_l[i].p->sched_state == DEFAULT)
            pop_list(proc_l[i].p, 0);
        else
            pop_list(proc_l[i].p, proc_l[i].p->data.mlfq.level);

        proc_l[i].p = 0;
        proc_l[i].next = 0;
        proc_l[i].use = 0;
    }
}

struct proc*
choice(void){

    struct proc *p = 0;

    int d_exist = 0;
    int m_exist = 0;
    int s_exist = 0;
    int order[3];
    int temp_pass = 0;
    struct proc *temp = 0;

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->state != RUNNABLE)
            continue;
        
        if(p->sched_state == DEFAULT)
            d_exist = 1;
        else if(p->sched_state == SHARE){
            s_exist = 1;
            if(temp == 0)
                temp = p;
            if(temp->data.share.pass > p->data.share.pass)
                temp = p;
        }else
            m_exist = 1;
    }

    if(s_exist != 0){
        temp_pass = temp->data.share.pass;
    }else
        temp_pass = 1;

    if (temp_pass > mlfq_s.pass){
        if(mlfq_s.pass < stride_s.pass){
            if(stride_s.pass > temp_pass){
                order[0] = 3*m_exist;
                order[1] = 2*s_exist;
                order[2] = 1*d_exist;
            }else{
                order[0] = 3*m_exist;
                order[1] = 1*d_exist;
                order[2] = 2*s_exist;
            }
        }else{
            order[0] = 1*d_exist;
            order[1] = 3*m_exist;
            order[2] = 2*s_exist;
        }
    } else {
        if(temp_pass < stride_s.pass){
            if(mlfq_s.pass < stride_s.pass){
                order[0] = 2*s_exist;
                order[1] = 3*m_exist;
                order[2] = 1*d_exist;
            }else{
                order[0] = 2*s_exist;
                order[1] = 1*d_exist;
                order[2] = 3*m_exist;
            }
        }else{
            order[0] = 1*d_exist;
            order[1] = 2*s_exist;
            order[2] = 3*m_exist;
        }
    }
    int i = 0;
    for(i = 0; i < 3; i++){
        if(order[i] == 0)
            continue;
        
        if(order[i] == 3){
            p = mlfq_start();
            min_pass = mlfq_s.pass;
            break;
        }else if(order[i] == 2){
            p = temp;
            p->data.share.strd = 1000/p->data.share.share;
            p->data.share.pass += p->data.share.strd;
            min_pass = p->data.share.pass;
            break;
        }else if(order[i] == 1){
            p = stride_start();
            min_pass = stride_s.pass;
            break;
        }
    }

    return p;
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
  struct proc *p = 0;
  struct cpu *c = mycpu();
  c->proc = 0;

  for(;;){
    struct proc *temp = 0;
    // Enable interrupts on this processor.
    sti();
    

    // Loop over process table looking for process to run.
    acquire(&ptable.lock);

    //pick the process which is the lowest pass and count default process
    temp = choice();
    
    if(temp != 0)
        p = temp;
    
    if(p != 0 && p->state == RUNNABLE){
        //cprintf("pid:%d mode:%d \n",p->pid,p->sched_state);
        //if(p->sched_state == SHARE)
        //    cprintf("%d ",(int)p->data.share.pass);
        //cprintf("%d %d\n",(int)stride_s.pass,(int)mlfq_s.pass);
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
        if(p->state == UNUSED){
            if(p->sched_state == MLFQ){
                pop_list(p,p->data.mlfq.level);
            }else if(p->sched_state == DEFAULT){
                pop_list(p,0);
            }
        }

        if(mlfq_s.boosting_period == 100)
            mlfq_boosting();
    }
    release(&ptable.lock);
  }
}

int cpu_share(int percent){
    struct proc *p = myproc();
    int share_percent = 0;

    acquire(&ptable.lock);

    for(int i = 0; i < NPROC; i++)
        if(ptable.proc[i].sched_state == SHARE)
            share_percent += ptable.proc[i].data.share.share;

    if(share_percent + percent > 20 || percent <= 0){
        release(&ptable.lock);
        return 1;

    }else{

        if(p->sched_state == DEFAULT){
            pop_list(p, 0);
        }else if(p->sched_state == MLFQ){
            pop_list(p, p->data.mlfq.level);
        }

        p->sched_state = SHARE;
        p->data.share.share = percent;
        p->data.share.pass = stride_s.pass;
        release(&ptable.lock);
        return 0;
    }

    release(&ptable.lock);
    return 2;
}

int run_MLFQ(void){
   struct proc *p = myproc();

   acquire(&ptable.lock);
   
   if(p->sched_state == MLFQ){
        release(&ptable.lock);
        return 1;
   }


   if(p->sched_state == DEFAULT)
       pop_list(p,0);

   p->sched_state = MLFQ;
   p->data.mlfq.level = 1;
   p->data.mlfq.exec_count = 0;

   push_list(p,1);
   
   release(&ptable.lock);
   return 0;
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

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}
