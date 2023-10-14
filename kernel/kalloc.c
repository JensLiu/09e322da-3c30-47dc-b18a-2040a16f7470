// Physical memory allocator, for user processes,
// kernel stacks, page-table pages,
// and pipe buffers. Allocates whole 4096-byte pages.

#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "riscv.h"
#include "defs.h"

void freerange(void *pa_start, void *pa_end);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
  // NOTE: semantic: 
  //       the number of physical pages that are referenced by USER pagetables.
  //       a. on kalloc, the default number is 1 -> satisfy the use cases
  //          -> in uvmfirst: kalloc (refcnt=1) -> mappages (refcnt unchanged)
  //          -> in uvmalloc: kalloc (refcnt=1) -> mappages (refcnt unchanged)
  //          -> in pagetable creation: kalloc (refcnt=1) -> so that it can be freed by kfree (refcnt -= 1 -> 0)
  //          -> in cow page fault: kalloc (refcnt=1) a new page -> mapped immediately to a PTE 
  //       b. kfree decrease refcnt by 1
  //       d. uvmcopy maps a child process pagetable to the parent's PA: increment refcnt by 1
  uint64 phymem_ref[NPAGES];
} kmem;

void
kinit()
{
  initlock(&kmem.lock, "kmem");
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE) {
    kmem.phymem_ref[PA2PGNUM((uint64) p)] = 1;  // illusion that there are freeable memory -> otherwise kfree will panic
    kfree(p);
  }
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  // NOTE: the sematic is hence changed:
  //       a. it does NOT mean to free the page at PA.
  //       b. rather, it acts like a garbage collector.
  //       c. it decreases the reference count
  //       d. only when the refcnt decreases to 0, the
  //          page is freed.

  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  acquire(&kmem.lock);
  int refcnt = kmem.phymem_ref[PA2PGNUM((uint64) pa)];
  if (refcnt < 1) {
    panic("kfree");
  }
  refcnt = --kmem.phymem_ref[PA2PGNUM((uint64) pa)];
  release(&kmem.lock);

  // do not free the memory when there is no page table reference
  // NOTE: (change of semantic) do not panic at refcnt > 1, it does not literaly free the PA when called
  //       implementing a kfree that only work when refcnt = 1 overly complecates the process, since
  //       every function that call kfree should first check if refcnt > 1, if so, just call dec_phymem_cnt,
  //       otherwise, call kfree.
  //       -> why not embeding it in kfree, so that no extra modification is needed! 
  
  // free when the count reaches zero
  if (refcnt > 0)
    return;
  
  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  acquire(&kmem.lock);
  r->next = kmem.freelist;
  kmem.freelist = r;
  release(&kmem.lock);
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  acquire(&kmem.lock);
  r = kmem.freelist;
  if(r) {
    kmem.freelist = r->next;
    if (kmem.phymem_ref[PA2PGNUM((uint64) r)] > 0) {
      panic("kalloc: used memory");
    }
    kmem.phymem_ref[PA2PGNUM((uint64) r)] = 1;
  }
  release(&kmem.lock);

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

int
inc_phymem_ref(uint64 pa)
{
  int refcnt;
  acquire(&kmem.lock);
  if (pa >= PHYSTOP || kmem.phymem_ref[PA2PGNUM(pa)] < 1)
    panic("inc_phymem_ref");
  refcnt = ++kmem.phymem_ref[PA2PGNUM(pa)];
  release(&kmem.lock);
  return refcnt;
}

int
dec_phymem_ref(uint64 pa)
{
  int refcnt;
  acquire(&kmem.lock);
  if (pa >= PHYSTOP || kmem.phymem_ref[PA2PGNUM(pa)] < 0)
    panic("dec_phymem_ref");
  refcnt = --kmem.phymem_ref[PA2PGNUM(pa)];
  release(&kmem.lock);
  return refcnt;
}

int
get_phymem_ref_cnt(uint64 pa)
{
  int refcnt;
  acquire(&kmem.lock);
  refcnt = kmem.phymem_ref[PA2PGNUM(pa)];
  release(&kmem.lock);
  return refcnt;
}