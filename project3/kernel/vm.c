#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "fs.h"
#include "file.h"
#include "proc.h"

/*
 * the kernel's page table.
 */
pagetable_t kernel_pagetable;

extern char etext[];  // kernel.ld sets this to end of kernel code.

extern char trampoline[]; // trampoline.S

// Make a direct-map page table for the kernel.
pagetable_t
kvmmake(void)
{
  pagetable_t kpgtbl;

  kpgtbl = (pagetable_t) kalloc();
  memset(kpgtbl, 0, PGSIZE);

  // uart registers
  kvmmap(kpgtbl, UART0, UART0, PGSIZE, PTE_R | PTE_W);

  // virtio mmio disk interface
  kvmmap(kpgtbl, VIRTIO0, VIRTIO0, PGSIZE, PTE_R | PTE_W);

  // PLIC
  kvmmap(kpgtbl, PLIC, PLIC, 0x4000000, PTE_R | PTE_W);

  // map kernel text executable and read-only.
  kvmmap(kpgtbl, KERNBASE, KERNBASE, (uint64)etext-KERNBASE, PTE_R | PTE_X);

  // map kernel data and the physical RAM we'll make use of.
  kvmmap(kpgtbl, (uint64)etext, (uint64)etext, PHYSTOP-(uint64)etext, PTE_R | PTE_W);

  // map the trampoline for trap entry/exit to
  // the highest virtual address in the kernel.
  kvmmap(kpgtbl, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);

  // allocate and map a kernel stack for each process.
  proc_mapstacks(kpgtbl);
  
  return kpgtbl;
}

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm) != 0)
    panic("kvmmap");
}

// Initialize the kernel_pagetable, shared by all CPUs.
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch the current CPU's h/w page table register to
// the kernel's page table, and enable paging.
void
kvminithart()
{
  // wait for any previous writes to the page table memory to finish.
  sfence_vma();

  w_satp(MAKE_SATP(kernel_pagetable));

  // flush stale entries from the TLB.
  sfence_vma();
}

// Return the address of the PTE in page table pagetable
// that corresponds to virtual address va.  If alloc!=0,
// create any required page-table pages.
//
// The risc-v Sv39 scheme has three levels of page-table
// pages. A page-table page contains 512 64-bit PTEs.
// A 64-bit virtual address is split into five fields:
//   39..63 -- must be zero.
//   30..38 -- 9 bits of level-2 index.
//   21..29 -- 9 bits of level-1 index.
//   12..20 -- 9 bits of level-0 index.
//    0..11 -- 12 bits of byte offset within the page.
pte_t *
walk(pagetable_t pagetable, uint64 va, int alloc)
{
  if(va >= MAXVA)
    panic("walk");

  for(int level = 2; level > 0; level--) {
    pte_t *pte = &pagetable[PX(level, va)];
    if(*pte & PTE_V) {
      pagetable = (pagetable_t)PTE2PA(*pte);
    } else {
      if(!alloc || (pagetable = (pde_t*)kalloc()) == 0)
        return 0;
      memset(pagetable, 0, PGSIZE);
      *pte = PA2PTE(pagetable) | PTE_V;
    }
  }
  return &pagetable[PX(0, va)];
}

// Look up a virtual address, return the physical address,
// or 0 if not mapped.
// Can only be used to look up user pages.
uint64
walkaddr(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  uint64 pa;

  if(va >= MAXVA)
    return 0;

  pte = walk(pagetable, va, 0);
  if(pte == 0)
    return 0;
  if((*pte & PTE_V) == 0)
    return 0;
  if((*pte & PTE_U) == 0)
    return 0;
  pa = PTE2PA(*pte);
  return pa;
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa.
// va and size MUST be page-aligned.
// Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm)
{
  uint64 a, last;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("mappages: va not aligned");

  if((size % PGSIZE) != 0)
    panic("mappages: size not aligned");

  if(size == 0)
    panic("mappages: size");
  
  a = va;
  last = va + size - PGSIZE;
  for(;;){
    if((pte = walk(pagetable, a, 1)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
  }
  return 0;
}

// create an empty user page table.
// returns 0 if out of memory.
pagetable_t
uvmcreate()
{
  pagetable_t pagetable;
  pagetable = (pagetable_t) kalloc();
  if(pagetable == 0)
    return 0;
  memset(pagetable, 0, PGSIZE);
  return pagetable;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. It's OK if the mappings don't exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0) // leaf page table entry allocated?
      continue;   
    if((*pte & PTE_V) == 0)  // has physical page been allocated?
      continue;
    if(do_free){
      uint64 pa = PTE2PA(*pte);
      kfree((void*)pa);
    }
    *pte = 0;
  }
}

// Allocate PTEs and physical memory to grow a process from oldsz to
// newsz, which need not be page aligned.  Returns new size or 0 on error.
uint64
uvmalloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz, int xperm)
{
  char *mem;
  uint64 a;

  if(newsz < oldsz)
    return oldsz;

  oldsz = PGROUNDUP(oldsz);
  for(a = oldsz; a < newsz; a += PGSIZE){
    mem = kalloc();
    if(mem == 0){
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    memset(mem, 0, PGSIZE);
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
  }
  return newsz;
}

// Deallocate user pages to bring the process size from oldsz to
// newsz.  oldsz and newsz need not be page-aligned, nor does newsz
// need to be less than oldsz.  oldsz can be larger than the actual
// process size.  Returns the new process size.
uint64
uvmdealloc(pagetable_t pagetable, uint64 oldsz, uint64 newsz)
{
  if(newsz >= oldsz)
    return oldsz;

  if(PGROUNDUP(newsz) < PGROUNDUP(oldsz)){
    int npages = (PGROUNDUP(oldsz) - PGROUNDUP(newsz)) / PGSIZE;
    uvmunmap(pagetable, PGROUNDUP(newsz), npages, 1);
  }

  return newsz;
}

// Recursively free page-table pages.
// All leaf mappings must already have been removed.
void
freewalk(pagetable_t pagetable)
{
  // there are 2^9 = 512 PTEs in a page table.
  for(int i = 0; i < 512; i++){
    pte_t pte = pagetable[i];
    if((pte & PTE_V) && (pte & (PTE_R|PTE_W|PTE_X)) == 0){
      // this PTE points to a lower-level page table.
      uint64 child = PTE2PA(pte);
      freewalk((pagetable_t)child);
      pagetable[i] = 0;
    } else if(pte & PTE_V){
      panic("freewalk: leaf");
    }
  }
  kfree((void*)pagetable);
}

// Free user memory pages,
// then free page-table pages.
void
uvmfree(pagetable_t pagetable, uint64 sz)
{
  if(sz > 0)
    uvmunmap(pagetable, 0, PGROUNDUP(sz)/PGSIZE, 1);
  freewalk(pagetable);
}

// Given a parent process's page table, copy
// its memory into a child's page table.
// Copies both the page table and the
// physical memory.
// returns 0 on success, -1 on failure.
// frees any allocated pages on failure.
int
uvmcopy(pagetable_t old, pagetable_t new, uint64 sz)
{
  pte_t *pte;
  uint64 pa, i;
  uint flags;
  char *mem;

  for(i = 0; i < sz; i += PGSIZE){
    if((pte = walk(old, i, 0)) == 0)
      continue;   // page table entry hasn't been allocated
    if((*pte & PTE_V) == 0)
      continue;   // physical page hasn't been allocated
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags) != 0){
      kfree(mem);
      goto err;
    }
  }
  return 0;

 err:
  uvmunmap(new, 0, i / PGSIZE, 1);
  return -1;
}

// mark a PTE invalid for user access.
// used by exec for the user stack guard page.
void
uvmclear(pagetable_t pagetable, uint64 va)
{
  pte_t *pte;
  
  pte = walk(pagetable, va, 0);
  if(pte == 0)
    panic("uvmclear");
  *pte &= ~PTE_U;
}

// Copy from kernel to user.
// Copy len bytes from src to virtual address dstva in a given page table.
// Return 0 on success, -1 on error.
int
copyout(pagetable_t pagetable, uint64 dstva, char *src, uint64 len)
{
  uint64 n, va0, pa0;
  pte_t *pte;

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    if(va0 >= MAXVA)
      return -1;
  
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }

    pte = walk(pagetable, va0, 0);
    // forbid copyout over read-only user text pages.
    if((*pte & PTE_W) == 0)
      return -1;
      
    n = PGSIZE - (dstva - va0);
    if(n > len)
      n = len;
    memmove((void *)(pa0 + (dstva - va0)), src, n);

    len -= n;
    src += n;
    dstva = va0 + PGSIZE;
  }
  return 0;
}

// Copy from user to kernel.
// Copy len bytes to dst from virtual address srcva in a given page table.
// Return 0 on success, -1 on error.
int
copyin(pagetable_t pagetable, char *dst, uint64 srcva, uint64 len)
{
  uint64 n, va0, pa0;

  while(len > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0) {
      if((pa0 = vmfault(pagetable, va0, 0)) == 0) {
        return -1;
      }
    }
    n = PGSIZE - (srcva - va0);
    if(n > len)
      n = len;
    memmove(dst, (void *)(pa0 + (srcva - va0)), n);

    len -= n;
    dst += n;
    srcva = va0 + PGSIZE;
  }
  return 0;
}

// Copy a null-terminated string from user to kernel.
// Copy bytes to dst from virtual address srcva in a given page table,
// until a '\0', or max.
// Return 0 on success, -1 on error.
int
copyinstr(pagetable_t pagetable, char *dst, uint64 srcva, uint64 max)
{
  uint64 n, va0, pa0;
  int got_null = 0;

  while(got_null == 0 && max > 0){
    va0 = PGROUNDDOWN(srcva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
      return -1;
    n = PGSIZE - (srcva - va0);
    if(n > max)
      n = max;

    char *p = (char *) (pa0 + (srcva - va0));
    while(n > 0){
      if(*p == '\0'){
        *dst = '\0';
        got_null = 1;
        break;
      } else {
        *dst = *p;
      }
      --n;
      --max;
      p++;
      dst++;
    }

    srcva = va0 + PGSIZE;
  }
  if(got_null){
    return 0;
  } else {
    return -1;
  }
}

// allocate and map user memory if process is referencing a page
// that was lazily allocated in sys_sbrk().
// returns 0 if va is invalid or already mapped, or if
// out of physical memory, and physical address if successful.
//
// Modified for project 03: also dispatches to mmap_fault() when the
// faulting address lies inside the mmap region (va >= MMAPBASE).
uint64
vmfault(pagetable_t pagetable, uint64 va, int read)
{
  uint64 mem;
  struct proc *p = myproc();

  // Project 03: mmap region fault.
  // The caller passes `read` = 1 for r_scause()==13 (load fault),
  // 0 for r_scause()==15 (store/AMO fault). mmap_fault wants the
  // opposite convention: 1 means "this was a write".
  if (va >= MMAPBASE) {
    return mmap_fault(pagetable, va, !read);
  }

  if (va >= p->sz)
    return 0;
  va = PGROUNDDOWN(va);
  if(ismapped(pagetable, va)) {
    return 0;
  }
  mem = (uint64) kalloc();
  if(mem == 0)
    return 0;
  memset((void *) mem, 0, PGSIZE);
  if (mappages(p->pagetable, va, PGSIZE, mem, PTE_W|PTE_U|PTE_R) != 0) {
    kfree((void *)mem);
    return 0;
  }
  return mem;
}

int
ismapped(pagetable_t pagetable, uint64 va)
{
  pte_t *pte = walk(pagetable, va, 0);
  if (pte == 0) {
    return 0;
  }
  if (*pte & PTE_V){
    return 1;
  }
  return 0;
}

// =====================================================================
// Project 03 (Virtual Memory / mmap) implementation
// =====================================================================
//
// We keep one system-wide array of mmap_area records. A slot is "in
// use" iff p != 0; setting p reserves the slot, clearing p frees it.
//
// We use NO locking. This is safe under these assumptions, which hold
// for the test harness (pa3_test) and the xv6 configuration in the
// provided Makefile:
//   - xv6 is built with -smp 1 (single CPU), so two user processes
//     never run system calls simultaneously.
//   - Every mmap_area is identified by its owner (m->p), so a
//     concurrent operation in another process can't touch our slots.
//   - The fault path calls into the file system (ilock/readi), which
//     can sleep; while we sleep, another process may run. But that
//     other process can only touch its own slots (m->p == its proc),
//     so we still don't race with ourselves.

struct mmap_area mmap_areas[MAXMMAP];

// Convert mmap prot bits to RISC-V PTE flag bits. PTE_U is always
// set because mmap regions are user-accessible.
static int
prot_to_pteflags(int prot)
{
  int flags = PTE_U;
  if (prot & PROT_READ)
    flags |= PTE_R;
  if (prot & PROT_WRITE)
    flags |= PTE_W;
  return flags;
}

// Allocate one physical page, zero it, optionally load file data, and
// install the PTE. Returns the physical address on success, 0 on
// failure (caller treats this as an invalid access).
static uint64
mmap_install_page(pagetable_t pgtbl,
                  uint64 va,
                  int prot,
                  int flags,
                  struct file *f,
                  int file_off)
{
  char *mem = kalloc();
  if (mem == 0)
    return 0;
  memset(mem, 0, PGSIZE);

  if (!(flags & MAP_ANONYMOUS)) {
    if (f == 0 || f->type != FD_INODE) {
      kfree(mem);
      return 0;
    }
    ilock(f->ip);
    // readi may read fewer than PGSIZE bytes if the file ends early;
    // the remainder of the page stays zero, which is the desired
    // behavior for partially-mapped file pages.
    readi(f->ip, 0, (uint64)mem, (uint)file_off, PGSIZE);
    iunlock(f->ip);
  }

  int pteflags = prot_to_pteflags(prot);
  if (mappages(pgtbl, va, PGSIZE, (uint64)mem, pteflags) != 0) {
    kfree(mem);
    return 0;
  }
  return (uint64)mem;
}

// Page-fault handler for the mmap region. Called from vmfault() when
// va >= MMAPBASE. Implements slides 23-25:
//   - find the mmap_area containing va
//   - reject invalid access
//   - allocate one page, load data, install PTE
// Returns the physical address on success, 0 on failure.
uint64
mmap_fault(pagetable_t pagetable, uint64 va, int write)
{
  struct proc *p = myproc();
  uint64 page_va = PGROUNDDOWN(va);

  // Find the matching mmap_area.
  struct mmap_area *m = 0;
  for (int i = 0; i < MAXMMAP; i++) {
    struct mmap_area *cand = &mmap_areas[i];
    if (cand->p != p)
      continue;
    if (page_va >= cand->addr && page_va < cand->addr + cand->length) {
      m = cand;
      break;
    }
  }
  if (m == 0)
    return 0;

  // Permission check: writing to a read-only region is invalid.
  /* AI generated: First put this check inside mmap_install_page, but Claude
   * pointed out it only makes sense at fault time — the eager path has no
   * "write" yet. Moved up to the dispatcher. */
  if (write && !(m->prot & PROT_WRITE))
    return 0;

  // Already mapped? This shouldn't happen, but guard anyway.
  if (ismapped(pagetable, page_va))
    return 0;

  int file_off = m->offset + (int)(page_va - m->addr);
  return mmap_install_page(pagetable, page_va,
                           m->prot, m->flags, m->f, file_off);
}

// The mmap() system call. See slides 10-13.
// On success: returns the start virtual address of the mapping.
// On failure: returns 0.
/* AI generated: Claude suggested moving filedup() to after the slot is
 * filled in, so only the MAP_POPULATE rollback needs to undo it. Also
 * fixed the overlap check from `<=` to `<` (adjacent regions are OK). */
uint64
mmap(uint64 addr, int length, int prot, int flags, int fd, int offset)
{
  struct proc *p = myproc();

  // Argument validation (slide 13).
  if ((addr % PGSIZE) != 0)
    return 0;
  if (length <= 0 || (length % PGSIZE) != 0)
    return 0;
  if (prot != PROT_READ && prot != (PROT_READ | PROT_WRITE))
    return 0;

  uint64 start = MMAPBASE + addr;
  if (start + (uint64)length > TRAPFRAME)
    return 0;

  // File / fd handling.
  struct file *f = 0;
  if (flags & MAP_ANONYMOUS) {
    if (fd != -1)
      return 0;
  } else {
    if (fd < 0 || fd >= NOFILE)
      return 0;
    f = p->ofile[fd];
    if (f == 0)
      return 0;
    if ((prot & PROT_READ) && !f->readable)
      return 0;
    if ((prot & PROT_WRITE) && !f->writable)
      return 0;
    if (f->type != FD_INODE)
      return 0;
  }

  // Reject overlap with this process's existing mappings.
  uint64 end = start + (uint64)length;
  for (int i = 0; i < MAXMMAP; i++) {
    struct mmap_area *m = &mmap_areas[i];
    if (m->p != p)
      continue;
    uint64 m_end = m->addr + m->length;
    if (start < m_end && m->addr < end)
      return 0;
  }

  // Find a free slot.
  struct mmap_area *slot = 0;
  for (int i = 0; i < MAXMMAP; i++) {
    if (mmap_areas[i].p == 0) {
      slot = &mmap_areas[i];
      break;
    }
  }
  if (slot == 0)
    return 0;

  // Fill in the slot.
  slot->addr = start;
  slot->length = length;
  slot->offset = offset;
  slot->prot = prot;
  slot->flags = flags;
  slot->f = f;
  slot->p = p;
  if (f)
    filedup(f);

  // MAP_POPULATE: install every page now (slide 12).
  /* AI generated: First draft just returned 0 on kalloc failure, leaking
   * already-installed pages. Claude flagged the partial-failure case; added
   * the j-loop rollback plus fileclose + slot release. */
  if (flags & MAP_POPULATE) {
    int npages = length / PGSIZE;
    for (int i = 0; i < npages; i++) {
      uint64 page_va = start + (uint64)i * PGSIZE;
      int file_off = offset + (int)(page_va - start);
      if (mmap_install_page(p->pagetable, page_va,
                            prot, flags, f, file_off) == 0) {
        // Out of memory partway through; roll back.
        for (int j = 0; j < i; j++) {
          uint64 va_j = start + (uint64)j * PGSIZE;
          uvmunmap(p->pagetable, va_j, 1, 1);
        }
        if (f)
          fileclose(f);
        slot->p = 0;
        slot->f = 0;
        return 0;
      }
    }
  }
  // Without MAP_POPULATE the slot just sits there until fault or munmap.

  return start;
}

// The munmap() system call. See slide 26.
// Returns 1 on success, -1 on failure.
int
munmap(uint64 addr)
{
  struct proc *p = myproc();

  if ((addr % PGSIZE) != 0)
    return -1;

  // Find the slot that starts at addr.
  struct mmap_area *m = 0;
  for (int i = 0; i < MAXMMAP; i++) {
    if (mmap_areas[i].p == p && mmap_areas[i].addr == addr) {
      m = &mmap_areas[i];
      break;
    }
  }
  if (m == 0)
    return -1;

  // For each page in the region: if it was actually faulted in, free
  // it; if it was lazy and never accessed, just skip it.
  /* AI generated: Initially called uvmunmap once for the whole region, but
   * Claude pointed out lazy pages would make it panic. Per-page PTE_V check
   * skips untouched pages. */
  int npages = m->length / PGSIZE;
  for (int i = 0; i < npages; i++) {
    uint64 va = m->addr + (uint64)i * PGSIZE;
    pte_t *pte = walk(p->pagetable, va, 0);
    if (pte == 0)
      continue;
    if ((*pte & PTE_V) == 0)
      continue;
    uvmunmap(p->pagetable, va, 1, 1);  // do_free = 1 -> kfree the page
  }

  // Drop the file reference and free the slot.
  if (m->f)
    fileclose(m->f);
  m->f = 0;
  m->p = 0;

  return 1;
}

// The freemem() system call. See slide 27.
// Returns the number of free physical memory PAGES (not bytes).
int
freemem(void)
{
  return (int)(memory_available() / PGSIZE);
}

// Copy parent's mmap regions into the child after kfork()'s uvmcopy().
// uvmcopy only handles [0, p->sz), so the mmap region is copied here.
// See slides 7, 38, 45.
// Returns 0 on success, -1 on failure (caller should free the child).
int
mmap_copy(struct proc *parent, struct proc *child)
{
  for (int i = 0; i < MAXMMAP; i++) {
    struct mmap_area *pm = &mmap_areas[i];
    if (pm->p != parent)
      continue;

    // Find a free slot for the child.
    struct mmap_area *cm = 0;
    for (int j = 0; j < MAXMMAP; j++) {
      if (mmap_areas[j].p == 0) {
        cm = &mmap_areas[j];
        break;
      }
    }
    if (cm == 0)
      return -1;

    // Copy metadata. Owner is now the child. The child shares the
    // same file as the parent (we bump the ref count).
    *cm = *pm;
    cm->p = child;
    if (cm->f)
      filedup(cm->f);


  /* AI generated: First version eagerly copied every page, defeating lazy
   * allocation across fork. Claude: skip parent's lazy pages (PTE_V == 0)
   * and let the child fault them in on demand. */

    // For each page in the parent's mapping that's actually allocated,
    // give the child its own private copy. Pages not yet faulted in
    // are left lazy in the child as well.
    int npages = pm->length / PGSIZE;
    for (int k = 0; k < npages; k++) {
      uint64 va = pm->addr + (uint64)k * PGSIZE;
      pte_t *ppte = walk(parent->pagetable, va, 0);
      if (ppte == 0 || (*ppte & PTE_V) == 0)
        continue;  // parent never faulted this page in
      uint64 ppa = PTE2PA(*ppte);
      uint pteflags = PTE_FLAGS(*ppte);

      char *cmem = kalloc();
      if (cmem == 0)
        return -1;
      memmove(cmem, (char *)ppa, PGSIZE);
      if (mappages(child->pagetable, va, PGSIZE,
                   (uint64)cmem, pteflags) != 0) {
        kfree(cmem);
        return -1;
      }
    }
  }

  return 0;
}
