// Buffer cache.
//
// The buffer cache is a linked list of buf structures holding
// cached copies of disk block contents.  Caching disk blocks
// in memory reduces the number of disk reads and also provides
// a synchronization point for disk blocks used by multiple processes.
//
// Interface:
// * To get a buffer for a particular disk block, call bread.
// * After changing buffer data, call bwrite to write it to disk.
// * When done with the buffer, call brelse.
// * Do not use the buffer after calling brelse.
// * Only one process at a time can use a buffer,
//     so do not keep them longer than necessary.


#include "types.h"
#include "param.h"
#include "spinlock.h"
#include "sleeplock.h"
#include "riscv.h"
#include "defs.h"
#include "fs.h"
#include "buf.h"


inline struct bucket *get_bucket(struct buf *);
void check_linked_list(struct buf *);
void insert_after(struct buf *, struct buf *);
void remove(struct buf *);

// #define DEBUG_VERBOSE
// #define DEBUG_GUARD

#define NBUCKET 13
#define BHASH(dev, blockno) ((dev + blockno) % NBUCKET)

struct bucket {
  struct spinlock blk;
  struct buf head;
};

struct {
  struct spinlock lock;
  struct buf buf[NBUF];           // it is prohibited to access any buffer by buf
  struct bucket htable[NBUCKET];  // hash table: all access to buffers should be made using this table
} bcache;

void
binit(void)
{
  struct buf *b;

  initlock(&bcache.lock, "bcache");

  // initialise hash table
  for (int i = 0; i < NBUCKET; i++) {
    initlock(&bcache.htable[i].blk, "bcache.bucket");
    // initialise hash table chain
    bcache.htable[i].head.next = &bcache.htable[i].head;
    bcache.htable[i].head.prev = &bcache.htable[i].head;
  }
  
  // initialise buffers to the first entry
  for(b = bcache.buf; b < bcache.buf+NBUF; b++){
    initsleeplock(&b->lock, "buffer");
    insert_after(&bcache.htable[0].head, b);
  }

#ifdef DEBUG_GUARD
  for (int i = 0; i < NBUCKET; i++) {
    check_linked_list(&bcache.htable[i].head);
  }
#endif

}

// the caller must hold the lock of the current block
static struct buf*
search_cache(uint dev, uint blockno)
{
  struct buf *b;
  struct bucket *curbuk;

  curbuk = &bcache.htable[BHASH(dev, blockno)];

  // Is the block already cached?
  for(b = curbuk->head.next; b != &curbuk->head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      b->timestamp = ticks;   // timestamp does not need to be accurate, no need to hold a lock
#ifdef DEBUG_VERBOSE
      printf("hit in bucket %d\n", hidx);
      printf("hit:\t\t %p cnt=%d, blockno=%d\n", b, b->refcnt, b->blockno);
#endif
      return b;
    }
  }
  return 0;
}

// the caller must acquire bcache.lock
static struct buf*
try_allocate(uint dev, uint blockno)
{
  struct buf *b = 0, *buf_found = 0;
  struct bucket *buk_found = 0, *curbuk = &bcache.htable[BHASH(dev, blockno)];

  uint64 least_ts = __UINT64_MAX__;
  int found_local_best = 0;

  for(int i = 0; i < NBUCKET; i++){
    acquire(&bcache.htable[i].blk); // freeze the bucket for check

    struct buf *head = &bcache.htable[i].head;
    for (b = head->prev; b != head; b = b->prev) {
      // we uses local LRU scheme. search backwards means increasing timestamp value
      if(b->refcnt == 0 && b->timestamp < least_ts) {
        found_local_best = 1;
        if (buk_found)
          release(&buk_found->blk);
        buk_found = &bcache.htable[i];
        buf_found = b;
        least_ts = b->timestamp;
      }
      if (b->timestamp > least_ts)  // break if found more recent one since they are ordered by their ts values
        break;
    }

    if (found_local_best)
      found_local_best = 0; // hold the lock for the currently found LRU bucket
    else
      release(&bcache.htable[i].blk);
  }
  
  if (buf_found->refcnt > 0) {  // no buffer avaliable
    release(&buk_found->blk);
    return 0;
  }

  if (buk_found != curbuk) {
    remove(buf_found);            // remove from original bucket
    release(&buk_found->blk);     // resume the original bucket
  }

  buf_found->refcnt = 1;
  buf_found->dev = dev;
  buf_found->blockno = blockno;
  buf_found->valid = 0;

  if (buk_found != curbuk) {
    acquire(&curbuk->blk);
    insert_after(&curbuk->head, buf_found); // add to the current bucket
  }
  release(&curbuk->blk);  // done allocating, resume current bucket
  return buf_found;
#ifdef DEBUG_VERBOSE
  printf("found buffer in bucket %d\n", i);
  printf("allocated:\t %p cnt=%d, blockno=%d\n", buf_found, buf_found->refcnt, buf_found->blockno);
#endif

}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *buf_found = 0;
  struct bucket *curbuk = &bcache.htable[BHASH(dev, blockno)];

  acquire(&curbuk->blk);
  buf_found = search_cache(dev, blockno);
  if (buf_found) {
    release(&curbuk->blk);
    acquiresleep(&buf_found->lock);
    return buf_found;
  }
  release(&curbuk->blk);


  // Not cached.
  // serialise the finding of free buffers to avoid multiple thread asking for the same dev and blockno
  // and allocated multiple buffers for the same dev and blockno
  acquire(&bcache.lock);
  buf_found = try_allocate(dev, blockno);
  if (buf_found) {
    release(&bcache.lock);  // done finding, let other threads to find free buffers
    // the buffer is sure not to be freeed since its refcnt >= 1
    // (the current thread have not called brelse yet, so it is at least 1)
    acquiresleep(&buf_found->lock);
    return buf_found;
  }

  panic("bget: no buffer");


}

// Return a locked buf with the contents of the indicated block.
struct buf*
bread(uint dev, uint blockno)
{
  struct buf *b;

  b = bget(dev, blockno);
  if(!b->valid) {
    virtio_disk_rw(b, 0);
    b->valid = 1;
  }
  return b;
}

// Write b's contents to disk.  Must be locked.
void
bwrite(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("bwrite");
  virtio_disk_rw(b, 1);
}

// Release a locked buffer.
// Move to the head of the most-recently-used list.
void
brelse(struct buf *b)
{
  if(!holdingsleep(&b->lock))
    panic("brelse");

  releasesleep(&b->lock);
  struct bucket *buk = get_bucket(b);
  acquire(&buk->blk);
  b->refcnt--;
  b->timestamp = ticks;
#ifdef DEBUG_VERBOSE
  printf("released:\t %p cnt=%d, blockno=%d\n", b, b->refcnt, b->blockno);
#endif
  if (b->refcnt == 0) {
    remove(b);
    insert_after(&buk->head, b);
  }
  release(&buk->blk);
}

void
bpin(struct buf *b) {
  struct bucket *buk = get_bucket(b);
  acquire(&buk->blk);
  b->refcnt++;
#ifdef DEBUG_VERBOSE
  printf("bpin:\t\t %p cnt=%d, blockno=%d\n", b, b->refcnt, b->blockno);
#endif
  release(&buk->blk);
}

void
bunpin(struct buf *b) {
  struct bucket *buk = get_bucket(b);
  acquire(&buk->blk);
  b->refcnt--;
#ifdef DEBUG_VERBOSE
  printf("bpin:\t\t %p cnt=%d, blockno=%d\n", b, b->refcnt, b->blockno);
#endif
  release(&buk->blk);
}

// helper functions

inline struct bucket *
get_bucket(struct buf *b)
{
  // Question: from looking at b to lock its parent bucket is not atomic,
  //           will b->dev and b->blockno change during this period?
  return &bcache.htable[BHASH(b->dev, b->blockno)];
}

// p should not be referenced by another linked list
// the caller should hold lock of the list that contains at
void
insert_after(struct buf *at, struct buf *p)
{
  p->next = at->next;
  p->prev = at;
  at->next->prev = p;
  at->next = p;
#ifdef DEBUG_GUARD
  check_linked_list(at);
  check_linked_list(p);
#endif
}

// remove p from its original list
// the caller should hold lock of the list that contains p
void
remove(struct buf *p)
{
#ifdef DEBUG_GUARD
  struct buf *p_next = p->next;
#endif
  if (!p->prev || !p->next)
    panic("corrupted structure\n");
  p->prev->next = p->next;
  p->next->prev = p->prev;
  p->next = 0;
  p->prev = 0;
#ifdef DEBUG_GUARD
  check_linked_list(p_next);
#endif
}

void
check_linked_list(struct buf *head)
{
  struct buf *p;
  int fcnt = 0, pcnt = 0;
  for (p = head->next; p && p != head; p = p->next)
    fcnt++;
  if (p != head)
    panic("corrupted linked list\n");
  for (p = head->prev; p && p != head; p = p->prev)
    pcnt++;
  if (p != head)
    panic("corrupted linked list\n");
  if (pcnt != fcnt)
    panic("corrupted linked list\n");
  return;
}