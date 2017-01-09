
#include <debug.h>
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "filesys/cache.h"
//#include "devices/timer.h"
//#include "threads/malloc.h"
//#include "threads/thread.h"

//#define INVALID_SECTOR ((block_sector_t) 0xffffffff)


struct cache_block  {
    block_sector_t sector;
    bool dirty;
    bool accessed;
    uint8_t data[BLOCK_SECTOR_SIZE];
};

/* Cache. */
#define CACHE_CNT 64
struct cache_block cache[CACHE_CNT];
struct lock cache_sync;
static int hand = 0;

static void cache_write_back(struct cache_block* block);
static struct cache_block* cache_search(block_sector_t sector);
struct cache_block* cache_evict();

/* Initializes cache. */
void cache_init (void) 
{
	lock_init(&cache_sync);
	int i;
	for (i = 0; i < CACHE_CNT; i++) {
		struct cache_block* cur = &cache[i];
		cur->sector = NULL_SECTOR;	
		cur->dirty = false;
	}
}

void cache_write_back(struct cache_block* block) {
  if (block->dirty == true) {
    block_write(fs_device, block->sector, block->data);
    block->dirty = false;
    block->sector = NULL_SECTOR;
  }
}


/* Flushes all cache to disk. */
void cache_flush (void) {
  lock_acquire(&cache_sync);
  int i;
  for (i = 0; i < CACHE_CNT; i++) {
    if (cache[i].sector != NULL_SECTOR)
      cache_write_back(&cache[i]);
  }
  lock_release(&cache_sync);
}


struct cache_block* cache_search(block_sector_t sector) {
  int i;
  for (i = 0; i < CACHE_CNT; i++) {
    if (cache[i].sector == sector)
      return &cache[i];
  }
  return NULL;
}

struct cache_block* cache_evict() {
  while (1) {
    if (cache[hand].sector == NULL_SECTOR)
      return &cache[hand];

    if (cache[hand].accessed == true) {
      cache[hand].accessed = false;
    } else {
      break;
    }

    hand = (hand + 1) % CACHE_CNT;
  }

  struct cache_block* chosen = &cache[hand];
  cache_write_back(chosen);
  chosen->sector = NULL_SECTOR;
  return chosen;
}

void cache_read (struct block* device, block_sector_t sector, void* buffer)
{
  lock_acquire(&cache_sync);
  struct cache_block* buf = cache_search(sector);
  if (buf == NULL) {
    buf = cache_evict(sector);
    buf->sector = sector;
    buf->dirty = false;
    block_read(device, sector, buf->data);
  }
  buf->accessed = true;
  memcpy(buffer, buf->data, BLOCK_SECTOR_SIZE);
  lock_release(&cache_sync);
}

void cache_write (struct block* device, block_sector_t sector, void* buffer)
{
  lock_acquire(&cache_sync);
  struct cache_block* buf = cache_search(sector);
  if (buf == NULL) {
    buf = cache_evict(sector);
    buf->sector = sector;
    block_read(device, sector, buf->data);
  }
  buf->accessed = true;
  buf->dirty = true;
  memcpy (buf->data, buffer, BLOCK_SECTOR_SIZE);
  lock_release(&cache_sync);
}