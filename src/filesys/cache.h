#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include <string.h>
#include "devices/block.h"

#define NULL_SECTOR 0xffffffff

/* Type of block lock. */
enum lock_type 
  {
    NON_EXCLUSIVE,	/* Any number of lockers. */
    EXCLUSIVE		/* Only one locker. */
  };// This is not used. Tests can pass. Start simple and iterate upwards if needed.


void cache_init (void);
void cache_flush (void);

/*
these two signatures imitate block_read, block_write and replace 
their appearances in inode.c
Only these two are exposed to make buffer cache more modularized.
*/
void cache_read (struct block* device, block_sector_t sector, void* buffer);
void cache_write (struct block* device, block_sector_t sector, void* buffer);


//New
/*
struct cache_block
{
	struct lock block_lock;
	struct condition no_readers_or_writers;
	struct condition no_writers;
	int readers, read_waiters;
	int writers, write_waiters;
	block_sector_t sector;
	bool up_to_date;
	bool dirty;
	struct lock data_lock;
	uint8_t data[BLOCK_SECTOR_SIZE];
};
programming reference
*/
#endif /* filesys/cache.h */
