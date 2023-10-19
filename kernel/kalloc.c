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
struct run *alloc_steal(int);

extern char end[]; // first address after kernel.
                   // defined by kernel.ld.

struct run {
  struct run *next;
};

struct {
  struct spinlock lock;
  struct run *freelist;
} kmems[NCPU];

void
kinit()
{
  for (int i = 0; i < NCPU; i++) 
    initlock(&kmems[i].lock, "kmem_per_cpu");
    
  freerange(end, (void*)PHYSTOP);
}

void
freerange(void *pa_start, void *pa_end)
{
  char *p;
  p = (char*)PGROUNDUP((uint64)pa_start);
  for(; p + PGSIZE <= (char*)pa_end; p += PGSIZE)
    kfree(p);
}

// Free the page of physical memory pointed at by pa,
// which normally should have been returned by a
// call to kalloc().  (The exception is when
// initializing the allocator; see kinit above.)
void
kfree(void *pa)
{
  struct run *r;

  if(((uint64)pa % PGSIZE) != 0 || (char*)pa < end || (uint64)pa >= PHYSTOP)
    panic("kfree");

  // Fill with junk to catch dangling refs.
  memset(pa, 1, PGSIZE);

  r = (struct run*)pa;
  
  push_off();
  // should not be interupted
  // otherwise timer interput may switch this
  // kernel thread to another CPU after the cpuid
  // is read and before actually acquired the lock
  // for that CPU
  int current_cpu = cpuid();
  acquire(&kmems[current_cpu].lock);
  r->next = kmems[current_cpu].freelist;
  kmems[current_cpu].freelist = r;
  release(&kmems[current_cpu].lock);
  pop_off();
}

// Allocate one 4096-byte page of physical memory.
// Returns a pointer that the kernel can use.
// Returns 0 if the memory cannot be allocated.
void *
kalloc(void)
{
  struct run *r;

  push_off();
  int current_cpu = cpuid();
  
  for (int i = current_cpu; i < current_cpu + NCPU; i++) {
    r = alloc_steal(i % NCPU);  // first look at its own free list
    if (r)
      break;
  }

  pop_off();

  if(r)
    memset((char*)r, 5, PGSIZE); // fill with junk
  return (void*)r;
}

struct run *
alloc_steal(int cpuid)
{
  acquire(&kmems[cpuid].lock);
  struct run *r = kmems[cpuid].freelist;
  
  // does not have free memory
  if (r)
    kmems[cpuid].freelist = r->next;
  
  release(&kmems[cpuid].lock);

  return r;

}