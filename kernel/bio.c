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


#define NBUCKETS 7


struct {
  struct spinlock lock;
  struct buf buf[NBUF/NBUCKETS];

  // Linked list of all buffers, through prev/next.
  // Sorted by how recently the buffer was used.
  // head.next is most recent, head.prev is least.
  struct buf head;
} bcache_list[NBUCKETS];

void
binit(void)
{
  struct buf *b;

  char name[9];
  for (int i = 0; i < NBUCKETS; i++)
  {
    snprintf(name, sizeof(name), "bcache%d", i);
    initlock(&bcache_list[i].lock, name);

      // Create linked list of buffers
    bcache_list[i].head.prev = &bcache_list[i].head;
    bcache_list[i].head.next = &bcache_list[i].head;
    for(b = bcache_list[i].buf; b < bcache_list[i].buf+(NBUF/NBUCKETS); b++){
      b->next = bcache_list[i].head.next;
      b->prev = &bcache_list[i].head;
      initsleeplock(&b->lock, "buffer");
      bcache_list[i].head.next->prev = b;
      bcache_list[i].head.next = b;
    }
  }
}

// Look through buffer cache for block on device dev.
// If not found, allocate a buffer.
// In either case, return locked buffer.
static struct buf*
bget(uint dev, uint blockno)
{
  struct buf *b;

  uint bucket_num = blockno % NBUCKETS;

  acquire(&bcache_list[bucket_num].lock);

  // Is the block already cached?
  for(b = bcache_list[bucket_num].head.next; b != &bcache_list[bucket_num].head; b = b->next){
    if(b->dev == dev && b->blockno == blockno){
      b->refcnt++;
      release(&bcache_list[bucket_num].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }

  // Not cached.
  // Recycle the least recently used (LRU) unused buffer.
  for(b = bcache_list[bucket_num].head.prev; b != &bcache_list[bucket_num].head; b = b->prev){
    if(b->refcnt == 0) {
      b->dev = dev;
      b->blockno = blockno;
      b->valid = 0;
      b->refcnt = 1;
      release(&bcache_list[bucket_num].lock);
      acquiresleep(&b->lock);
      return b;
    }
  }
  panic("bget: no buffers");
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

  uint bucket_num = b->blockno % NBUCKETS;

  acquire(&bcache_list[bucket_num].lock);

  b->refcnt--;
  if (b->refcnt == 0) {
    // no one is waiting for it.
    b->next->prev = b->prev;
    b->prev->next = b->next;
    b->next = bcache_list[bucket_num].head.next;
    b->prev = &bcache_list[bucket_num].head;
    bcache_list[bucket_num].head.next->prev = b;
    bcache_list[bucket_num].head.next = b;
  }
  release(&bcache_list[bucket_num].lock);
}

void
bpin(struct buf *b) {
  uint bucket_num = b->blockno % NBUCKETS;

  acquire(&bcache_list[bucket_num].lock);
  b->refcnt++;
  release(&bcache_list[bucket_num].lock);
}

void
bunpin(struct buf *b) {
  uint bucket_num = b->blockno % NBUCKETS;

  acquire(&bcache_list[bucket_num].lock);
  b->refcnt--;
  release(&bcache_list[bucket_num].lock);
}


