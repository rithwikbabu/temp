#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "proc.h"
#include "x86.h"
#include "traps.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"

// Interrupt descriptor table (shared by all CPUs).
struct gatedesc idt[256];
extern uint vectors[];  // in vectors.S: array of 256 entry pointers
struct spinlock tickslock;
uint ticks;

void
tvinit(void)
{
  int i;

  for(i = 0; i < 256; i++)
    SETGATE(idt[i], 0, SEG_KCODE<<3, vectors[i], 0);
  SETGATE(idt[T_SYSCALL], 1, SEG_KCODE<<3, vectors[T_SYSCALL], DPL_USER);

  initlock(&tickslock, "time");
}

void
idtinit(void)
{
  lidt(idt, sizeof(idt));
}

//PAGEBREAK: 41
void
trap(struct trapframe *tf)
{
  if(tf->trapno == T_SYSCALL){
    if(myproc()->killed)
      exit();
    myproc()->tf = tf;
    syscall();
    if(myproc()->killed)
      exit();
    return;
  }

  switch(tf->trapno){
  case T_IRQ0 + IRQ_TIMER:
    if(cpuid() == 0){
      acquire(&tickslock);
      ticks++;
      wakeup(&ticks);
      release(&tickslock);
    }
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE:
    ideintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_IDE+1:
    // Bochs generates spurious IDE1 interrupts.
    break;
  case T_IRQ0 + IRQ_KBD:
    kbdintr();
    lapiceoi();
    break;
  case T_IRQ0 + IRQ_COM1:
    uartintr();
    lapiceoi();
    break;
  case T_IRQ0 + 7:
  case T_IRQ0 + IRQ_SPURIOUS:
    cprintf("cpu%d: spurious interrupt at %x:%x\n",
            cpuid(), tf->cs, tf->eip);
    lapiceoi();
    break;
  // ################################################################################
  case T_PGFLT: {
    uint va = PGROUNDDOWN(rcr2());
    struct proc *p = myproc();

    struct mapping* map = 0;
    struct mapping* m = p->mappings;
    while (m) {
      if (va >= m->addr && va < (m->addr + m->length)) {
        map = m;
        break;
      }
      m = m->next;
    }

    pte_t* pte = walkpgdir(p->pgdir, (void*)va, 0);

    if (map) {
      char* mem = kalloc();
        if (!mem) {
            cprintf("trap: Memory allocation failed for lazy allocation!\n");
            myproc()->killed = 1;
            break;
        }
        memset(mem, 0, PGSIZE);
        mappages(myproc()->pgdir, (char*)va, PGSIZE, V2P(mem), PTE_W | PTE_U);
        ref_cnts[V2P(mem) / PGSIZE]++;

        if (map->flag == 0) {
            struct file* f = myproc()->ofile[map->fd];
            uint offset = va - map->addr;
            readi(f->ip, mem, offset, PGSIZE);
        }
        map->count++;
        
    } else if (*pte & PTE_ORIGIN_W) {
      uint pa = PTE_ADDR(*pte);

      if (ref_cnts[pa / PGSIZE] > 1) {
        // copy-on-write
        ref_cnts[pa / PGSIZE]--;
        char* mem = kalloc();

        if (!mem) {
          cprintf("trap: Memory allocation failed for copy-on-write.\n");
          p->killed = 1;
          break;
        }

        memmove(mem, (char*)P2V(pa), PGSIZE);
        uint flags = PTE_FLAGS(*pte);
        *pte = 0;
        mappages(p->pgdir, (void*)va, PGSIZE, V2P(mem), flags | PTE_W);
        ref_cnts[V2P(mem) / PGSIZE] = 1;

      } else if (ref_cnts[pa / PGSIZE] == 1) {
        // single reference, enable write
        *pte |= PTE_W;

      } else {  
        // invalid reference count
        cprintf("trap: Invalid ref_cnts for page fault!\n");
        *pte = 0;
        p->killed = 1;
      }

      lcr3(V2P(p->pgdir));  // Reload CR3 to apply changes
    } else {
      cprintf("Segmentation Fault\n");
      p->killed = 1;
    }

    break;
  }
  // ################################################################################

  //PAGEBREAK: 13
  default:
    if(myproc() == 0 || (tf->cs&3) == 0){
      // In kernel, it must be our mistake.
      cprintf("unexpected trap %d from cpu %d eip %x (cr2=0x%x)\n",
              tf->trapno, cpuid(), tf->eip, rcr2());
      panic("trap");
    }
    // In user space, assume process misbehaved.
    cprintf("pid %d %s: trap %d err %d on cpu %d "
            "eip 0x%x addr 0x%x--kill proc\n",
            myproc()->pid, myproc()->name, tf->trapno,
            tf->err, cpuid(), tf->eip, rcr2());
    myproc()->killed = 1;
  }

  // Force process exit if it has been killed and is in user space.
  // (If it is still executing in the kernel, let it keep running
  // until it gets to the regular system call return.)
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();

  // Force process to give up CPU on clock tick.
  // If interrupts were on while locks held, would need to check nlock.
  if(myproc() && myproc()->state == RUNNING &&
     tf->trapno == T_IRQ0+IRQ_TIMER)
    yield();

  // Check if the process has been killed since we yielded
  if(myproc() && myproc()->killed && (tf->cs&3) == DPL_USER)
    exit();
}
