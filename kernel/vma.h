#define NVMA 16
struct vma {
  uint64 va_low;    // the begining of the area, may change due to ummap'
  uint64 va_high;
  uint64 va_frame_high;   // the original address of the allocation
  uint64 va_frame_low;     
  int flags;        // permission
  int prot;         // protection
  // int length;       // area size 
  struct file *f;   // mapped file discriptor
  int used;         // allocated ?
  // use a chain structure in
  // proc to record the order of
  // mmap to restore the correct proc size
  struct vma *next;
};