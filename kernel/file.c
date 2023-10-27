//
// Support functions for system calls that involve file descriptors.
//

#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "param.h"
#include "fs.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "file.h"
#include "stat.h"
#include "proc.h"

struct devsw devsw[NDEV];
struct {
  struct spinlock lock;
  struct file file[NFILE];
} ftable;

void
fileinit(void)
{
  initlock(&ftable.lock, "ftable");
}

// Allocate a file structure.
struct file*
filealloc(void)
{
  struct file *f;

  acquire(&ftable.lock);
  for(f = ftable.file; f < ftable.file + NFILE; f++){
    if(f->ref == 0){
      f->ref = 1;
      release(&ftable.lock);
      return f;
    }
  }
  release(&ftable.lock);
  return 0;
}

// Increment ref count for file f.
struct file*
filedup(struct file *f)
{
  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("filedup");
  f->ref++;
  release(&ftable.lock);
  return f;
}

// Close file f.  (Decrement ref count, close when reaches 0.)
void
fileclose(struct file *f)
{
  struct file ff;

  acquire(&ftable.lock);
  if(f->ref < 1)
    panic("fileclose");
  if(--f->ref > 0){
    release(&ftable.lock);
    return;
  }
  ff = *f;
  f->ref = 0;
  f->type = FD_NONE;
  release(&ftable.lock);

  if(ff.type == FD_PIPE){
    pipeclose(ff.pipe, ff.writable);
  } else if(ff.type == FD_INODE || ff.type == FD_DEVICE){
    begin_op();
    iput(ff.ip);
    end_op();
  }
}

// Get metadata about file f.
// addr is a user virtual address, pointing to a struct stat.
int
filestat(struct file *f, uint64 addr)
{
  struct proc *p = myproc();
  struct stat st;
  
  if(f->type == FD_INODE || f->type == FD_DEVICE){
    ilock(f->ip);
    stati(f->ip, &st);
    iunlock(f->ip);
    if(copyout(p->pagetable, addr, (char *)&st, sizeof(st)) < 0)
      return -1;
    return 0;
  }
  return -1;
}

// Read from file f.
// addr is a user virtual address.
int
fileread(struct file *f, uint64 addr, int n)
{
  int r = 0;

  if(f->readable == 0)
    return -1;

  if(f->type == FD_PIPE){
    r = piperead(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].read)
      return -1;
    r = devsw[f->major].read(1, addr, n);
  } else if(f->type == FD_INODE){
    ilock(f->ip);
    if((r = readi(f->ip, 1, addr, f->off, n)) > 0)
      f->off += r;
    iunlock(f->ip);
  } else {
    panic("fileread");
  }

  return r;
}

// Write to file f.
// addr is a user virtual address.
int
filewrite(struct file *f, uint64 addr, int n)
{
  int r, ret = 0;

  if(f->writable == 0)
    return -1;

  if(f->type == FD_PIPE){
    ret = pipewrite(f->pipe, addr, n);
  } else if(f->type == FD_DEVICE){
    if(f->major < 0 || f->major >= NDEV || !devsw[f->major].write)
      return -1;
    ret = devsw[f->major].write(1, addr, n);
  } else if(f->type == FD_INODE){
    // write a few blocks at a time to avoid exceeding
    // the maximum log transaction size, including
    // i-node, indirect block, allocation blocks,
    // and 2 blocks of slop for non-aligned writes.
    // this really belongs lower down, since writei()
    // might be writing a device like the console.
    int max = ((MAXOPBLOCKS-1-1-2) / 2) * BSIZE;
    int i = 0;
    while(i < n){
      int n1 = n - i;
      if(n1 > max)
        n1 = max;

      begin_op();
      ilock(f->ip);
      if ((r = writei(f->ip, 1, addr + i, f->off, n1)) > 0)
        f->off += r;
      iunlock(f->ip);
      end_op();

      if(r != n1){
        // error from writei
        break;
      }
      i += r;
    }
    ret = (i == n ? n : -1);
  } else {
    panic("filewrite");
  }

  return ret;
}

// according to the address, return its
// corresponding vma structure
struct vma *
proc_getvma(uint64 addr, struct proc *p)
{
  if (!p)
    p = myproc();
  
  uint64 va = PGROUNDDOWN(addr);
  
  struct vma *vp;
  for (vp = p->vma_head.next; vp != 0; vp = vp->next) {
    if (vp && vp->va_low <= va && va < vp->va_high)
      return vp;
  }
  return 0;
}

// add the vma structure to the process
void
proc_setvma(struct vma *vmap, struct proc *p)
{
  if (!vmap)
    panic("setvma: invalid vma pointer\n");
  if (!p)
    p = myproc();
  
  vmap->next = p->vma_head.next;
  p->vma_head.next = vmap;
}

// remove the vma structure from the process
// absorb the frame size of non-top frames
// to avoid holes in the vma area and allow
// the recycle of addresses
void
proc_unsetvma(struct vma *target_p, struct proc *p)
{
  if (!p)
    p = myproc();
  struct vma *vp;
  for (vp = &p->vma_head; vp != 0; vp = vp->next) {
    if (vp->next == target_p) {
      vp->next = vp->next->next;
      if (target_p->va_frame_low != p->vma_ptr) {
        // if not the top frame, absorb its frame size
        vp->va_frame_high = target_p->va_frame_high;
      }
      return;
    }
  }
  panic("unset vma: invalid vma\n");
}

void
proc_file_writeback(uint64 va0, uint64 va1, uint64 off, struct file *f, struct proc *p)
{
  if (!p)
    p = myproc();
  
  pte_t *pte = 0;
  begin_op();
  for (uint64 va = va0; va < va1; va += PGSIZE, off += PGSIZE) {
    pte = walk(p->pagetable, va, 0);
    if (!pte) {
      panic("file writeback: invalid pte\n");
    }
    uint64 pa = PTE2PA(*pte);
    if (pa != 0 && PTE_FLAGS(*pte) & PTE_D) // skip unmapped, unwritten page
      writei(f->ip, 0, pa, off, PGSIZE);
  }
  end_op();
}

void
dup_vma(struct proc *np, struct proc *p)
{
  struct vma *vp;
  for (vp = p->vma_head.next; vp != 0; vp = vp->next) {
    struct vma *nvp = vma_alloc();
    *nvp = *vp;
    nvp->next = np->vma_head.next;
    np->vma_head.next = nvp;
    filedup(nvp->f);
  }
  np->vma_ptr = p->vma_ptr;
}

#include "fcntl.h"
int
proc_handle_mmap(uint64 addr, struct proc *p)
{
  if (!p)
    p = myproc();
  
  if (!(addr < MAXVA - 2*PGSIZE && addr >= p->vma_ptr)) {
    printf("address invalid, not a mmap fault\n");
    return -1;
  }

  // round down address to page alligned to identify its page
  addr = PGROUNDDOWN(addr);

  struct vma *vmap = proc_getvma(addr, p);
  if (!vmap) {
    printf("unable to find vma\n");
    return -1;
  }
  
  // map one page of content
  struct inode* ip = vmap->f ? vmap->f->ip : 0;
  if (!ip) {
    printf("invalid inode");
    return -1;
  }

  // allocate page, vabase is page alligned
  pte_t *pte = walk(p->pagetable, addr, 1);
  if (pte == 0)
    panic("mmap handler: cannot map\n");
  uint64 pa = (uint64)kalloc();
  memset((void *)pa, 0, PGSIZE);
  *pte |= PA2PTE(pa);
  *pte |= PTE_V;
  *pte |= PTE_U;
  *pte |= (vmap->prot & PROT_READ) ? PTE_R : 0;
  *pte |= (vmap->prot & PROT_WRITE) ? PTE_W : 0;
  
  // calculate file offset
  uint64 foff = addr - vmap->va_low;  // addr and vmap->va are page alligned
  ilock(ip);
  readi(ip, 0, pa, foff, PGSIZE);
  iunlock(ip);
  return 0;
}