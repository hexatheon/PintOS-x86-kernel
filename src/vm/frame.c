#include "vm/frame.h"
#include "vm/page.h"
#include "vm/swap.h"
#include "threads/synch.h"
#include <debug.h>
#include <list.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "threads/malloc.h"
#include "threads/palloc.h"
#include <stdio.h>
#include <bitmap.h>
#include "userprog/pagedir.h"
#include "threads/thread.h"
/*
Managing the frame table

The main job is to obtain a free frame to map a page to. To do so:

1. Easy situation is there is a free frame in frame table and it can be
obtained. If there is no free frame, you need to choose a frame to evict
using your page replacement algorithm based on setting accessed and dirty
bits for each page. See section 4.1.5.1 and A.7.3 to know details of
replacement algorithm(accessed and dirty bits) If no frame can be evicted
without allocating a swap slot and swap is full, you should panic the
kernel.

2. remove references from any page table that refers to.

3.write the page to file system or swap.

*/
// we just provide frame_init() for swap.c
// the rest is your responsibility
static struct frame *frames;
static size_t frame_cnt;

static struct lock scan_lock;
static size_t hand;

static struct semaphore pin_sema; 

static struct frame *try_frame_alloc_and_lock(struct page *page);

void frame_init (void)
{
  void *base;

  lock_init (&scan_lock);

  frames = malloc (sizeof *frames * init_ram_pages);
  if (frames == NULL){
    PANIC ("out of memory allocating page frames");
  }

  while ((base = palloc_get_page (PAL_USER)) != NULL)
    {
      struct frame *f = &frames[frame_cnt++];
      lock_init (&f->lock);
      f->base = base;
      f->page = NULL;
      f->pinned = false;
    }

  sema_init(&pin_sema, 1);
}

bool frame_pin(struct frame* f){
    // ////printf("%d-%s: Actually pining\n", thread_current()->tid, __func__);
    sema_down(&pin_sema);
    bool ret = false;
    if (f){
        ret = f->pinned;
        f->pinned = true;
    }
    sema_up(&pin_sema);
    return ret;
}

bool frame_unpin(struct frame* f){
    bool ret = false;
    sema_down(&pin_sema);
    if (f){
        ret = f->pinned;
        f->pinned = false;
    }
    sema_up(&pin_sema);
    return ret;
}

void show_frame(int i){
    int tid = thread_current()->tid;
    struct frame* f = &frames[i];
    //printf("%d-%s: ========showing of frame %d (base = %p) =======\n", tid, __func__, i, f->base);
    //printf("%d-%s: ==   first 8 bytes: %x   ==\n", tid, __func__, *(unsigned int*) f->base);
    show_page(f->page);
}
static struct frame *try_frame_alloc_and_lock (struct page *page){
    //returns the hand at which the frame was evicted
    size_t ret = 0;
    struct frame* cur = NULL;
    int done = 0; // 1 means succesfully evicted, 2 find a free frame

    sema_down(&pin_sema);
    ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
    lock_acquire(&scan_lock);
    ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
    int n_round = 0;
    while (n_round++ < 2 && done == 0){
        ret = hand;
        while (true){
            cur = &frames[ret];
            if (lock_try_acquire(&cur->lock)){
                if (cur->page){
                    if (!cur->pinned){
                        uint32_t* pd = cur->page->thread->pagedir;
                        bool accessed = pagedir_is_accessed(pd, cur->page->addr);
                        // //printf("%d-%s: Round %d frame %d: This page is (dirty,accessed)==(%d,%d)\n", thread_current()->tid, __func__, n_round, ret,(pagedir_is_dirty(pd, cur->page->addr) ? 1 : 0), (accessed ? 1 : 0));

                        if (!accessed){
                            if (n_round == 2){
                                cur->pinned = true;//??
                                done = 1;
                                break;        
                            }
                        } else {
                            ASSERT(n_round < 2);//all accessed must be reset in the first round
                            pagedir_set_accessed(pd, cur->page->addr, false);
                        } 
                    } else {
                        //printf("%d-%s: Met a pinned frame...\n", thread_current()->tid, __func__);
                    }
                    lock_release(&cur->lock);
                } else {
                    //this frame is free
                    //printf("%d-%s: Pinned a frame\n", thread_current()->tid, __func__);
                    cur->pinned = true;
                    done = 2;
                    break;
                }
            }
            ret = (ret + 1) % frame_cnt;
            if (ret == hand){
                break;
            }
        }
    }
    ASSERT(lock_held_by_current_thread(&scan_lock));
    lock_release(&scan_lock);
    //now another frame_lock() can start try_frame_alloc_and_lock
    //but pin_sema is not upped yet, so page fault cannot happen here
    if (done > 0){
        hand = (ret + 1) % frame_cnt;
        // show_frame(ret);
        if (done == 1){
            page_out(cur->page);
        }
    }
    sema_up(&pin_sema);
    //printf("%d-%s: returning %d \n", thread_current()->tid, __func__, ret);
    return cur;
}

void frame_lock (struct page *p) {
    ASSERT(p != NULL);
    struct frame* f = try_frame_alloc_and_lock(p);
    f->page = p;
    p->frame = f;
    ASSERT(lock_held_by_current_thread(&f->lock));
    lock_release(&f->lock);
}

void frame_free(struct frame *f) {
    ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
    lock_acquire(&f->lock);
    ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
    //actually free
    if (!f){
        return;
    }
    palloc_free_page(f->base);//OK even if f->base == NULL
    f->base = NULL;

    lock_release(&f->lock);
}

//DO SOMETHING ABOTU f->page!
void frame_unlock(struct frame *f) {
    bool already_held = lock_held_by_current_thread(&f->lock);
    if (!already_held){
        ////printf("%d-%s: Trying to get a lock\n", thread_current()->tid, __func__);
        lock_acquire(&f->lock);
        ////printf("%d-%s: Got the lock\n", thread_current()->tid, __func__);
    }
    

    memset(f->base, 0xcc, PGSIZE);
    ASSERT(f->page != NULL);
    f->page->frame = NULL;
    f->page = NULL;

    if (!already_held){
        lock_release(&f->lock);
    }
    
}
