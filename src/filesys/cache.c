#include "filesys/cache.h"
#include <debug.h>
#include <string.h>
#include "filesys/filesys.h"
#include "devices/timer.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <stdio.h>

#define INVALID_SECTOR ((block_sector_t) -1)


struct cache_block 
  {
    struct lock block_lock;
    // struct condition no_readers_or_writers;
    // struct condition no_writers;           
    // int readers, read_waiters;
    // int writers, write_waiters;
    block_sector_t sector;
    bool up_to_date;
    bool dirty;
    bool accessed;
    // struct lock data_lock; 
    uint8_t data[BLOCK_SECTOR_SIZE];   
  };

/* Cache. */
#define CACHE_CNT 64
struct cache_block cache[CACHE_CNT];
struct lock cache_sync;
static int hand = 0;


// static void flushd_init (void);
// static void readaheadd_init (void);
// static void readaheadd_submit (block_sector_t sector);
static void cache_write_back(struct cache_block* block);
static struct cache_block* cache_search(block_sector_t sector);
struct cache_block* cache_evict(void);


/* Initializes cache. */
void
cache_init (void) 
{
  lock_init(&cache_sync);
  int i;
  for (i = 0; i < CACHE_CNT; i++) {
    struct cache_block* cur = &cache[i];
    lock_init(&cur->block_lock);
    // cond_init(&cur->no_readers_or_writers);
    // cond_init(&cur->no_writers);
    // cur->readers = 0;
    // cur->read_waiters = 0;
    // cur->writers = 0;
    // cur->write_waiters = 0;
    cur->up_to_date = false;
    // lock_init(&cur->data_lock);
    cur->sector = NULL_SECTOR;  
    cur->dirty = false;
    
    //do not clear data. Won't access it anyway
  }
}

void cache_write_back(struct cache_block* block) {
  if (block->dirty == true) {
    block_write(fs_device, block->sector, block->data);
    block->dirty = false;
    block->sector = NULL_SECTOR;
  }
}


/* Flushes cache to disk. */
void
cache_flush (void) 
{
  lock_acquire(&cache_sync);
  int i;
  for (i = 0; i < CACHE_CNT; i++) {
    if (cache[i].sector != NULL_SECTOR)
      cache_write_back(&cache[i]);
  }
  lock_release(&cache_sync);
  // block_write (fs_device, b->sector, b->data);
}

struct cache_block* cache_search(block_sector_t sector) {
  int i;
  for (i = 0; i < CACHE_CNT; i++) {
    if (cache[i].sector == sector)
      // hand = i;
      return &cache[i];
  }
  return NULL;
}

struct cache_block* cache_evict() {
  // int local_hand = hand;
  while (1) {
    if (cache[hand].sector == NULL_SECTOR){
      printf("%d-%s: Found an empty\n", thread_current()->tid, __func__);
      // hand = local_hand;
      return &cache[hand];
    }
      

    if (cache[hand].accessed == true) {
      cache[hand].accessed = false;
    } else {
      printf("%d-%s: Kicking %u out\n", thread_current()->tid, __func__, cache[hand].sector);
      break;
    }

    hand = (hand + 1) % CACHE_CNT;
  }

  struct cache_block* chosen = &cache[hand];
  cache_write_back(chosen);
  chosen->sector = NULL_SECTOR;
  // hand = local_hand;
  return chosen;
}

/* Locks the given SECTOR into the cache and returns the cache
   block.
   If TYPE is EXCLUSIVE, then the block returned will be locked
   only by the caller.  The calling thread must not already
   have any lock on the block.
   If TYPE is NON_EXCLUSIVE, then block returned may be locked by
   any number of other callers.  The calling thread may already
   have any number of non-exclusive locks on the block. */
struct cache_block *cache_lock (block_sector_t sector, enum lock_type type){
  int i;
  struct cache_block* ret = NULL;
  bool up_to_date = false;
  try_again:
    lock_acquire(&cache_sync);
    //ret = cache_search(sector);
    int local_hand = hand;
    int first_empty = -1, first_old = -1;
    while (true){
      if (lock_try_acquire(&cache[hand].block_lock)){
        bool accessed = cache[hand].accessed;
        if (accessed){
            cache[hand].accessed = false;
        }
        if (cache[hand].sector == sector){
          ret = &cache[hand];
          if (first_old != -1){
            lock_release(&cache[first_old].block_lock);
            first_old = -1;
          } 
          if (first_empty != -1){
            lock_release(&cache[first_empty].block_lock);
            first_empty = -1;
          }
          break;
        } else if (cache[hand].sector == NULL_SECTOR && first_empty == -1){
          /* Not in cache.  Find empty slot. */
          first_empty = hand;
          if (first_old != -1){
            lock_release(&cache[first_old].block_lock);
            first_old = -1;
          }
        } else if (accessed == false && first_old == -1 && first_empty == -1){
          first_old = hand;
        } else {
          lock_release(&cache[hand].block_lock);
        }
      }
      hand = (hand + 1)%CACHE_CNT;
      if (hand == local_hand && (first_empty != -1 || first_old != -1)){
        break;
      }
    }
    if (!ret){
      if (first_empty != -1){
        // printf("%d-%s: Found an empty\n", thread_current()->tid, __func__);
        ret = &cache[first_empty];
      } else if (first_old != -1){
        // printf("%d-%s: Kicking %u out\n", thread_current()->tid, __func__, cache[first_old].sector);
        ret = &cache[first_old];
        cache_write_back(ret);
        ret->sector = NULL_SECTOR;
      } else {
        printf("%d-%s: Need a nother run...\n", thread_current()->tid, __func__);
      }
      ret->dirty = false;
    } else {
      up_to_date = true;
    }
    goto done;
    /* Wait for cache contention to die down. */
    // sometimes, you might get into a situation where you
    // cannot find a block to evict, or you cannot lock
    // the desired block. If that's the case there might
    // some contention. So the safest way to do this, is to
    // release the cache_sync lock, and sleep for 1 sec, and
    // try again the whole operation.
    lock_release (&cache_sync);
    timer_msleep (1000);
    goto try_again;
  done:
    lock_release(&cache_sync);
    ret->sector = sector;
    ret->up_to_date = up_to_date;
    return ret;
}

/* Zero out block B, without reading it from disk, and return a
   pointer to the zeroed data.
   The caller must have an exclusive lock on B. */
// void *cache_zero (struct cache_block *b){
//   // ...
//   //  memset (b->data, 0, BLOCK_SECTOR_SIZE);
//   // ...

// }

/* Marks block B as dirty, so that it will be written back to
   disk before eviction.
   The caller must have a read or write lock on B,
   and B must be up-to-date. */
// void cache_dirty (struct cache_block *b){
//   // ...
// }

/* Unlocks block B.
   If B is no longer locked by any thread, then it becomes a
   candidate for immediate eviction. */
// void cache_unlock (struct cache_block *b){
//   // ...
// }

/* If SECTOR is in the cache, evicts it immediately without
   writing it back to disk (even if dirty).
   The block must be entirely unused. */
// void cache_free (block_sector_t sector){
//   // ...
// }


/* Flush daemon. */

// static void flushd (void *aux);

// /* Initializes flush daemon. */
// static void
// flushd_init (void) 
// {
//   thread_create ("flushd", PRI_MIN, flushd, NULL);
// }

// /* Flush daemon thread. */
// static void
// flushd (void *aux UNUSED) 
// {
//   for (;;) 
//     {
//       timer_msleep (30 * 1000);
//       cache_flush ();
//     }
// }

void cache_read (struct block* device, block_sector_t sector, void* buffer)
{
  struct cache_block* buf = cache_lock(sector, EXCLUSIVE);//
  ASSERT(buf != NULL);//
  if (!buf->up_to_date){
    block_read(device, sector, buf->data);
  }
  buf->accessed = true;
  memcpy(buffer, buf->data, BLOCK_SECTOR_SIZE);
  lock_release(&buf->block_lock);
}

void cache_write (struct block* device, block_sector_t sector, void* buffer)
{
  struct cache_block* buf = cache_lock(sector, EXCLUSIVE);//
  ASSERT(buf != NULL);
  if (!buf->up_to_date){
    block_read(device, sector, buf->data);
  }
  buf->accessed = true;
  buf->dirty = true;
  memcpy (buf->data, buffer, BLOCK_SECTOR_SIZE);
  lock_release(&buf->block_lock);
}
