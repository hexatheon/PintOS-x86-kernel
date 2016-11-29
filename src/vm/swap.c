#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "vm/page.h"
#include "threads/palloc.h"
#include "threads/malloc.h"
#include <bitmap.h>
#include "devices/block.h"
#include <stdio.h>
#include "threads/vaddr.h"
#include "vm/swap.h"
#include "vm/frame.h"
#include "threads/thread.h"
/*

Managing the swap table

You should handle picking an unused swap slot for evicting a page from its
frame to the swap partition. And handle freeing a swap slot which its page
is read back.

You can use the BLOCK_SWAP block device for swapping, obtaining the struct
block that represents it by calling block_get_role(). Also to attach a swap
disk, please see the documentation.

and to attach a swap disk for a single run, use this option ‘--swap-size=n’

*/




// we just provide swap_init() for swap.c
// the rest is your responsibility

/* The swap device. */
static struct block *swap_device;

/* Used swap pages. */
static struct bitmap *swap_bitmap;

/* Protects swap_bitmap. */
static struct lock swap_lock;

/* Number of sectors per page. */
#define PAGE_SECTORS (PGSIZE / BLOCK_SECTOR_SIZE)


void swap_init (void)
{
  swap_device = block_get_role (BLOCK_SWAP);
  if (swap_device == NULL)
    {
      printf ("no swap device--swap disabled\n");
      swap_bitmap = bitmap_create (0);
    }
  else
    swap_bitmap = bitmap_create (block_size (swap_device)
                                 / PAGE_SECTORS);
  if (swap_bitmap == NULL)
    PANIC ("couldn't create swap bitmap");
  lock_init (&swap_lock);
  //printf("%d-%s: SWAP capacity %d\n", thread_current()->tid,__func__, block_size (swap_device) / PAGE_SECTORS);
}

bool swap_in (struct page *p){  
  struct frame* f = p->frame;
  ASSERT(p->frame != NULL);//so I know where to write

  bool already_held = lock_held_by_current_thread(&f->lock);
  if (!already_held){
    ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
    lock_acquire(&f->lock);
    ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
  }
  ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
  lock_acquire(&swap_lock);
  ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
  ASSERT(bitmap_test(swap_bitmap, p->sector / PAGE_SECTORS) == true);
  lock_release(&swap_lock);

  //printf("%d-%s: reading from %d of a page in thread %d\n", thread_current()->tid, __func__, p->sector, p->thread->tid);
  int i;
  for (i = 0; i < PAGE_SECTORS; i++){
    block_read(swap_device, p->sector + i, f->base + i * BLOCK_SECTOR_SIZE);  
  }
  //printf("%d-%s: read from %d of a page in thread %d\n", thread_current()->tid, __func__, p->sector, p->thread->tid);
  if (!already_held){
    lock_release(&f->lock);
  }
  return true;
}

bool swap_out (struct page *p) {
  struct frame* f = p->frame;
  ASSERT(f != NULL);//so I know where to read


  bool already_held = lock_held_by_current_thread(&f->lock);
  if (!already_held){
    ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
    lock_acquire(&f->lock);
    ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
  }

  if (p->sector == 0xffffffff){
    ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
    lock_acquire(&swap_lock);
    ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
    block_sector_t available = bitmap_scan_and_flip(swap_bitmap, 0, 1, false);
    lock_release(&swap_lock);
    if (available == BITMAP_ERROR){
      PANIC("We are out of swap space");
    } else {
      int temp = available;
      available *= PAGE_SECTORS;
      //printf("%d-%s: Using bitmap at %d - %d, or %d\n", thread_current()->tid, __func__, available, available + PAGE_SECTORS - 1, temp);
      p->sector = available;
    }
  }
  ASSERT(bitmap_test(swap_bitmap, p->sector / PAGE_SECTORS) == true);

  //printf("%d-%s: writing to %d of a page in thread %d\n", thread_current()->tid, __func__, p->sector, p->thread->tid);
  int i;
  for (i = 0; i < PAGE_SECTORS; i++){
    //printf("%d-%s: %d\n", thread_current()->tid, __func__,i);
    block_write(swap_device, p->sector + i, f->base + i * BLOCK_SECTOR_SIZE);  
  }  
  //printf("%d-%s: written to %d of a page in thread %d\n", thread_current()->tid, __func__, p->sector, p->thread->tid);
  if (!already_held){
    lock_release(&f->lock);
  }
  return true;
}

//assumed page is locked
bool swap_free(struct page *p){
  if (p->sector == 0xffffffff){
    return true;
  }
  ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
  lock_acquire(&swap_lock);
  ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
  ASSERT(bitmap_test(swap_bitmap, p->sector / PAGE_SECTORS) == true);
  bitmap_set(swap_bitmap, p->sector / PAGE_SECTORS, false);
  lock_release(&swap_lock);
  //printf("%d-%s: Setting the sector of tid %d to %d\n", thread_current()->tid, __func__, p->thread->tid, 0xffffffff);
  p->sector = 0xffffffff;
  return true;
}