#include "param.h"
#include "types.h"
#include "memlayout.h"
#include "elf.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"

#include "spinlock.h"
#include "proc.h"
int createTime();
int findpagetoswap();
int ramIntoDisc(struct proc *p, int swapI);
int EmptyRoomOnRam(struct proc* p);
int EmptyRoomToSwap(struct proc* p);
static char global_buff[PGSIZE];

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
  kvmmap(kpgtbl, PLIC, PLIC, 0x400000, PTE_R | PTE_W);

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

// Initialize the one kernel_pagetable
void
kvminit(void)
{
  kernel_pagetable = kvmmake();
}

// Switch h/w page table register to the kernel's page table,
// and enable paging.
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

// add a mapping to the kernel page table.
// only used when booting.
// does not flush TLB or enable paging.
void
kvmmap(pagetable_t kpgtbl, uint64 va, uint64 pa, uint64 sz, int perm)
{
  if(mappages(kpgtbl, va, sz, pa, perm, REG_MAP) != 0)
    panic("kvmmap");
}

// Create PTEs for virtual addresses starting at va that refer to
// physical addresses starting at pa. va and size might not
// be page-aligned. Returns 0 on success, -1 if walk() couldn't
// allocate a needed page-table page.
int
mappages(pagetable_t pagetable, uint64 va, uint64 size, uint64 pa, int perm, int state)
{
  uint64 a, last;
  pte_t *pte;

  if(size == 0)
    panic("mappages: size");
  
  a = PGROUNDDOWN(va);
  last = PGROUNDDOWN(va + size - 1);
  for(;;){
    if((pte = walk(pagetable, a, state)) == 0)
      return -1;
    if(*pte & PTE_V)
      panic("mappages: remap");
    *pte = PA2PTE(pa) | perm | PTE_V;
    if(a == last)
      break;
    a += PGSIZE;
    pa += PGSIZE;
    if (state == SPEC_MAP)
      break;
  }
  return 0;
}

// Remove npages of mappings starting from va. va must be
// page-aligned. The mappings must exist.
// Optionally free the physical memory.
void
uvmunmap(pagetable_t pagetable, uint64 va, uint64 npages, int do_free)
{
  uint64 a;
  pte_t *pte;

  if((va % PGSIZE) != 0)
    panic("uvmunmap: not aligned");

  for(a = va; a < va + npages*PGSIZE; a += PGSIZE){
    if((pte = walk(pagetable, a, 0)) == 0)
      panic("uvmunmap: walk");
    if((*pte & PTE_V) == 0){
    #ifndef NONE
    if((*pte & PTE_PG) == 0)
    #endif
    panic("uvmunmap: not mapped");  
    }
    if(PTE_FLAGS(*pte) == PTE_V)
      panic("uvmunmap: not a leaf");
    if(do_free && ((*pte & PTE_PG)==0)){
        uint64 pa = PTE2PA(*pte);
        kfree((void*)pa);
    }
    *pte = 0;
      #ifndef NONE
      struct proc *p = myproc();
      if(p->pid > 2) {
        for (int i = 0; i < MAX_PSYC_PAGES; i++){
          if(p->ram[i].adress == a) {
            p->ram[i].adress = UNUSED;
            p->ram[i].state= UNUSED;
            p->swaps[i].adress = UNUSED;
            p->swaps[i].state = UNUSED;
          }
        }
      }
    #endif
    }
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

// Load the user initcode into address 0 of pagetable,
// for the very first process.
// sz must be less than a page.
void
uvmfirst(pagetable_t pagetable, uchar *src, uint sz)
{
  char *mem;

  if(sz >= PGSIZE)
    panic("uvmfirst: more than a page");
  mem = kalloc();
  memset(mem, 0, PGSIZE);
  mappages(pagetable, 0, PGSIZE, (uint64)mem, PTE_W|PTE_R|PTE_X|PTE_U, REG_MAP);
  memmove(mem, src, sz);
}

// Allocate PTEs and physical memory to grow process from oldsz to
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
    if(mappages(pagetable, a, PGSIZE, (uint64)mem, PTE_R|PTE_U|xperm,REG_MAP) != 0){
      kfree(mem);
      uvmdealloc(pagetable, a, oldsz);
      return 0;
    }
    struct proc *p = myproc();
    int ramI = -1;
    int swapI = -1;
    #ifndef NONE
    if(p->pid > 2 && p->pagetable == pagetable) {
      ramI = EmptyRoomOnRam(p);
      // no empty room on ram
      if(ramI == -1){
        swapI = EmptyRoomToSwap(p);
      //room in swap
      if(swapI != -1) 
        ramI  = ramIntoDisc(p, swapI);
      else 
        panic("Out OF Mem");
      }  
      p->ram[ramI].state = USED;
      p->ram[ramI].adress = a;
      p->ram[ramI].creationTime = createTime();
       #if LAPA
       p->ram[ramI].accesscounter = 0xFFFFFFFF;
       #endif
       #if NFUA
       p->ram[ramI].accesscounter = 0;
       #endif
      p->ram[ramI].offsetInSF = ramI*PGSIZE;
    }
    #endif
 
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
      panic("uvmcopy: pte should exist");
    if((*pte & PTE_V) == 0){
      #ifndef NONE
        if(*pte & PTE_PG) {
          pte_t *pte1;
          pte1 = walk(new, i, 0);
          *pte1 &= ~PTE_V;
          *pte1 |= PTE_PG;
          *pte1 |= PTE_FLAGS(*pte);
        }
        else
          panic("uvmcopy: page not present");
      #else
        panic("uvmcopy: page not present");
      #endif
    }
    pa = PTE2PA(*pte);
    flags = PTE_FLAGS(*pte);
    if((mem = kalloc()) == 0)
      goto err;
    memmove(mem, (char*)pa, PGSIZE);
    if(mappages(new, i, PGSIZE, (uint64)mem, flags, REG_MAP) != 0){
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

  while(len > 0){
    va0 = PGROUNDDOWN(dstva);
    pa0 = walkaddr(pagetable, va0);
    if(pa0 == 0)
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
    if(pa0 == 0)
      return -1;
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


// this method takes page from ram and insert it to swapfile
int
ramIntoDisc(struct proc *p, int swapI)
{
  int ramI = findpagetoswap(p);
  
  uint64 pa = walkaddr(p->pagetable, p->ram[ramI].adress);
  if(writeToSwapFile(p, (void*)pa, PGSIZE*swapI, PGSIZE) == -1)
    panic("Fail");
  
  p->swaps[swapI].state = USED;
  p->swaps[swapI].adress = p->ram[ramI].adress;
  p->swaps[swapI].offsetInSF = swapI * PGSIZE;
  
  pte_t *pte;
  pte = walk(p->pagetable, p->ram[ramI].adress, 0);
  *pte |= PTE_PG;
  *pte &= ~PTE_V;
  
  kfree((void*)pa);
  return ramI;
}

void discIntoRam(uint64 va, pte_t *pte, char* pa, struct proc *p, int ramI){
  int swapI = 0;
  // find page on disc
  for ( swapI = 0; swapI < MAX_PSYC_PAGES;swapI++)
    if(p->swaps[swapI].adress == va) 
      break;
  
  // update disk & ram  
  p->swaps[swapI].adress = UNUSED;
  p->swaps[swapI].state = UNUSED;
  p->ram[ramI].adress = va;
  p->ram[ramI].state = USED;
  p->ram[ramI].creationTime = createTime();
  #if LAPA
    p->ram[ramI].accesscounter = 0xFFFFFFFF;
  #endif
  #if NFUA
    p->ram[ramI].accesscounter = 0;
  #endif
  // read from disc
  if(readFromSwapFile(p, global_buff, swapI*PGSIZE, PGSIZE) == -1)
    panic("Fail");
  
}

int EmptyRoomOnRam(struct proc* p){
  for (int i =0 ; i<MAX_PSYC_PAGES; i++){
    if (p->ram[i].state == 0)
      return i;
  }
  return -1;
}

int EmptyRoomToSwap(struct proc* p){
  for (int i =0 ; i<MAX_PSYC_PAGES; i++){
    if (p->swaps[i].state == 0)
      return i;
  }
  panic("no more memory");
  return -1;
}

void pageFault(uint64 va, pte_t *pte)
{
  struct proc *p = myproc();
  char *pa = kalloc();
  int ramI = EmptyRoomOnRam(p);
  // no empty room on ram
  if(ramI == -1) {
    //choose indx on ram
    ramI = findpagetoswap(p);
    uint ramVa = p->ram[ramI].adress;
    uint64 ramPa = walkaddr(p->pagetable, ramVa);
    
    discIntoRam(va, pte, pa, p, ramI);
    int swapI = EmptyRoomToSwap(p);

    if(writeToSwapFile(p, (void*)ramPa, PGSIZE*swapI, PGSIZE) == -1)
      panic("Fail");
    kfree((void*)ramPa);   

    p->swaps[swapI].state = USED;
    p->swaps[swapI].adress = ramVa;
    
    pte_t *swapPte = walk(p->pagetable, ramVa, 0);
    *swapPte = *swapPte | PTE_PG;
    *swapPte = *swapPte & ~PTE_V;     
  }
  //Found empty room 
  else 
    discIntoRam(va, pte, pa, p, ramI);
       
  mappages(p->pagetable, va, p->sz, (uint64)pa, PTE_W | PTE_X | PTE_R | PTE_U, SPEC_MAP);
  memmove((void*)pa, global_buff, PGSIZE);
  *pte = *pte & ~PTE_PG;
}

 
int aNFUA(){
  int index=0;
  int _minValue = myproc()->ram[0].accesscounter;    //take the first as comperator (first exists otherwise we won't enter this func)
  int i;
  for(i=0; i < MAX_PSYC_PAGES; i++){
    if(myproc()->ram[i].accesscounter< _minValue){
      _minValue=myproc()->ram[i].accesscounter;
      index=i;
    }
  }
  return index;
}

int countSetBits(uint n){ 
  int count = 0; 
  while (n){ 
      count += n & 1;                                         //AND first bit with 1 and add result to count
      n >>= 1;                                                //shift right one bit and continue
  } 
  return count; 
}

int aLAPA(){
  uint _minValue=countSetBits( myproc()->ram[0].accesscounter);
  int index=0;
  int i;
  for(i=0; i < MAX_PSYC_PAGES; i++){
    if(countSetBits(myproc()->ram[0].accesscounter)<_minValue){
      _minValue=countSetBits( myproc()->ram[0].accesscounter);
      index=i;
    }
  }
  return index;
}

int aSCFIFO(){
  pte_t * pte;
  int i = 0;
  int pageIndex;
  uint ctime;
  recheck:
  pageIndex = -1;
  ctime =  myproc()->ram[0].creationTime;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (myproc()->ram[i].state == 1 && myproc()->ram[i].creationTime <= ctime){
      pageIndex = i;
      ctime = myproc()->ram[i].creationTime;
      }
  }
  pte = walk(myproc()->pagetable, myproc()->ram[pageIndex].adress,0);
  if (*pte & PTE_A) {
    *pte &= ~PTE_A; // turn off PTE_A flag

    goto recheck;
    }
  return pageIndex;
}

int findpagetoswap(){
  #if NFUA  
   return aNFUA();
  #endif

  #if SCFIFO
   return aSCFIFO();
  #endif

  #if LAPA
   return aLAPA();
  #endif

  panic("Unrecognized paging algorithm");
}

void updateCounter(struct proc * p){
  pte_t * pte;
  int i;
  for (i = 0; i < MAX_PSYC_PAGES; i++) {
    if (p->ram[i].state == 1){
      pte = walk(p->pagetable,p->ram[i].adress,0);
      if (*pte & PTE_A) { // check if page accessed 
        *pte &= ~PTE_A; // turn off PTE_A flag
         p->ram[i].accesscounter = p->ram[i].accesscounter >> 1;
         p->ram[i].accesscounter = p->ram[i].accesscounter | (1 << 31);

      }
    } 
  }
}
struct spinlock lock;
int next = 1;

int createTime() {
  int time;
  if(next < 2)
    initlock(&lock, "nextctime");
  acquire(&lock);
  time = next;
  next ++;
  release(&lock);
  return time;
}

