#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"
#include "vm.h"

uint64
sys_exit(void)
{
  int n;
  argint(0, &n);
  kexit(n);
  return 0;  // not reached
}

uint64
sys_getpid(void)
{
  return myproc()->pid;
}

uint64
sys_fork(void)
{
  return kfork();
}

uint64
sys_wait(void)
{
  uint64 p;
  argaddr(0, &p);
  return kwait(p);
}

uint64
sys_sbrk(void)
{
  uint64 addr;
  int t;
  int n;

  argint(0, &n);
  argint(1, &t);
  addr = myproc()->sz;

  if(t == SBRK_EAGER || n < 0) {
    if(growproc(n) < 0) {
      return -1;
    }
  } else {
    // Lazily allocate memory for this process: increase its memory
    // size but don't allocate memory. If the processes uses the
    // memory, vmfault() will allocate it.
    if(addr + n < addr)
      return -1;
    if(addr + n > TRAPFRAME)
      return -1;
    myproc()->sz += n;
  }
  return addr;
}

uint64
sys_pause(void)
{
  int n;
  uint ticks0;

  argint(0, &n);
  if(n < 0)
    n = 0;
  acquire(&tickslock);
  ticks0 = ticks;
  while(ticks - ticks0 < n){
    if(killed(myproc())){
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64
sys_kill(void)
{
  int pid;

  argint(0, &pid);
  return kkill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64
sys_uptime(void)
{
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

//Added for project 01
uint64 sys_getnice(void) {
    int pid;
    argint(0, &pid);
    return getnice(pid);
}

uint64 sys_setnice(void) {
    int pid, value;
    argint(0, &pid);
    argint(1, &value);
    return setnice(pid, value);
}

uint64 sys_ps(void) {
    int pid;
    argint(0, &pid);
    ps(pid);
    return 0;
}

uint64 sys_meminfo(void) {
    return meminfo();
}

uint64 sys_waitpid(void) {
    int pid;
    argint(0, &pid);
    return waitpid(pid);
}

// Added for project 03 (Virtual Memory / mmap)
// uint64 mmap(uint64 addr, int length, int prot, int flags,
//             int fd, int offset) -- slide 11
uint64 sys_mmap(void)
{
    uint64 addr;
    int length, prot, flags, fd, offset;
    argaddr(0, &addr);
    argint(1, &length);
    argint(2, &prot);
    argint(3, &flags);
    argint(4, &fd);
    argint(5, &offset);
    return mmap(addr, length, prot, flags, fd, offset);
}

// int munmap(uint64 addr) -- slide 26
uint64 sys_munmap(void)
{
    uint64 addr;
    argaddr(0, &addr);
    return (uint64)munmap(addr);
}

// int freemem(void) -- slide 27
uint64 sys_freemem(void)
{
    return (uint64)freemem();
}

// proj4 README
uint64
sys_swapstat(void)
{
  uint64 ra_addr, wa_addr;
  int r, w;
  argaddr(0, &ra_addr);
  argaddr(1, &wa_addr);
  swapstat(&r, &w);
  if(copyout(myproc()->pagetable, ra_addr, (char *)&r, sizeof(r)) < 0)
    return -1;
  if(copyout(myproc()->pagetable, wa_addr, (char *)&w, sizeof(w)) < 0)
    return -1;
  return 0;
}
