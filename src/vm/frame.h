
#include "threads/synch.h"
#include "vm/page.h"

#ifndef VM_FRAME_H
#define VM_FRAME_H
/* A physical frame. */
struct frame
{
  struct lock lock;           /* Prevent simultaneous access. */
  void *base;                 /* Kernel virtual base address. */
  struct page *page;          /* Mapped process page, if any. */

  bool pinned;
};


void frame_init (void);

void frame_lock(struct page *p);

void frame_free(struct frame *f);

void frame_unlock(struct frame *f);


//returns the old value of pinned
bool frame_pin(struct frame* f);
bool frame_unpin(struct frame* f);
void show_frame(int i);
#endif