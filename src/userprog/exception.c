#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/syscall.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "userprog/pagedir.h"
#include "threads/pte.h"
#include "threads/vaddr.h"
//#include "userprog/process.h"

/* Number of page faults processed. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);


/* Registers handlers for interrupts that can be caused by user
   programs.

   In a real Unix-like OS, most of these interrupts would be
   passed along to the user process in the form of signals, as
   described in [SV-386] 3-24 and 3-25, but we don't implement
   signals.  Instead, we'll make them simply kill the user
   process.

   Page faults are an exception.  Here they are treated the same
   way as other exceptions, but this will need to change to
   implement virtual memory.

   Refer to [IA32-v3a] section 5.15 "Exception and Interrupt
   Reference" for a description of each of these exceptions. */
void
exception_init (void)
{
  /* These exceptions can be raised explicitly by a user program,
     e.g. via the INT, INT3, INTO, and BOUND instructions.  Thus,
     we set DPL==3, meaning that user programs are allowed to
     invoke them via these instructions. */
  intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
  intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
  intr_register_int (5, 3, INTR_ON, kill,
                     "#BR BOUND Range Exceeded Exception");

  /* These exceptions have DPL==0, preventing user processes from
     invoking them via the INT instruction.  They can still be
     caused indirectly, e.g. #DE can be caused by dividing by
     0.  */
  intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
  intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
  intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
  intr_register_int (7, 0, INTR_ON, kill,
                     "#NM Device Not Available Exception");
  intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
  intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
  intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
  intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
  intr_register_int (19, 0, INTR_ON, kill,
                     "#XF SIMD Floating-Point Exception");

  /* Most exceptions can be handled with interrupts turned on.
     We need to disable interrupts for page faults because the
     fault address is stored in CR2 and needs to be preserved. */
  intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* Prints exception statistics. */
void
exception_print_stats (void)
{
  printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* Handler for an exception (probably) caused by a user process. */
static void
kill (struct intr_frame *f)
{
  /* This interrupt is one (probably) caused by a user process.
     For example, the process might have tried to access unmapped
     virtual memory (a page fault).  For now, we simply kill the
     user process.  Later, we'll want to handle page faults in
     the kernel.  Real Unix-like operating systems pass most
     exceptions back to the process via signals, but we don't
     implement them. */

  /* The interrupt frame's code segment value tells us where the
     exception originated. */
  switch (f->cs)
    {
    case SEL_UCSEG:
      /* User's code segment, so it's a user exception, as we
         expected.  Kill the user process.  */
      printf ("%s: dying due to interrupt %#04x (%s).\n",
              thread_name (), f->vec_no, intr_name (f->vec_no));
      intr_dump_frame (f);
      sys_exit (-1);

    case SEL_KCSEG:
      /* Kernel's code segment, which indicates a kernel bug.
         Kernel code shouldn't throw exceptions.  (Page faults
         may cause kernel exceptions--but they shouldn't arrive
         here.)  Panic the kernel to make the point.  */
      intr_dump_frame (f);
      PANIC ("Kernel bug - unexpected interrupt in kernel");

    default:
      /* Some other code segment?  Shouldn't happen.  Panic the
         kernel. */
      printf ("Interrupt %#04x (%s) in unknown segment %04x\n",
             f->vec_no, intr_name (f->vec_no), f->cs);
      sys_exit (-1);
    }
}

/* Page fault handler.  This is a skeleton that must be filled in
   to implement virtual memory.  Some solutions to project 2 may
   also require modifying this code.

   At entry, the address that faulted is in CR2 (Control Register
   2) and information about the fault, formatted as described in
   the PF_* macros in exception.h, is in F's error_code member.  The
   example code here shows how to parse that information.  You
   can find more information about both of these in the
   description of "Interrupt 14--Page Fault Exception (#PF)" in
   [IA32-v3a] section 5.15 "Exception and Interrupt Reference". */
static void
page_fault (struct intr_frame *f)
{
  bool not_present;  /* True: not-present page, false: writing r/o page. */
  bool write;        /* True: access was write, false: access was read. */
  bool user;         /* True: access by user, false: access by kernel. */
  void *fault_addr;  /* Fault address. */

  //////printf("%d-%s: Now handling a pagefault\n", thread_current()->tid,__func__);
  /* Obtain faulting address, the virtual address that was
     accessed to cause the fault.  It may point to code or to
     data.  It is not necessarily the address of the instruction
     that caused the fault (that's f->eip).
     See [IA32-v2a] "MOV--Move to/from Control Registers" and
     [IA32-v3a] 5.15 "Interrupt 14--Page Fault Exception
     (#PF)". */
  asm ("movl %%cr2, %0" : "=r" (fault_addr));

  /* Turn interrupts back on (they were only off so that we could
     be assured of reading CR2 before it changed). */
  intr_enable ();

  /* Count page faults. */
  page_fault_cnt++;

  /* Determine cause. */
  not_present = (f->error_code & PF_P) == 0;
  write = (f->error_code & PF_W) != 0;
  user = (f->error_code & PF_U) != 0;

  //!!validate memory access
  //if user just not present, then page_in
  //if kernel, problem
  //kernel can access user, tha't fine
  //user cannot access kernel

  // printf ("%d-%s: Page fault at %p: %s error %s page in %s context.\n",thread_current()->tid, __func__,
  //         fault_addr,
  //         not_present ? "not present" : "rights violation",
  //         write ? "writing" : "reading",
  //         user ? "user" : "kernel");

  void* fpage = pg_round_down(fault_addr);
  struct thread* t = thread_current();
  struct page* p = page_lookup(t->pages, fpage);
  void* stackptr = (user) ? f->esp : t->user_esp;//??
  // void* stackptr = (f->esp < t->user_esp) ? f->esp : t->user_esp;//??
  //printf("%d-%s: f->esp == %p and t->user_esp == %p\n", thread_current()->tid, __func__, f->esp, t->user_esp);
  if (!p){
    //if not stack growth, kill
    //printf("%d-%s: stackptr == %p and fault_addr == %p\n", thread_current()->tid, __func__, stackptr, fault_addr);
    if (valid_stack_access(stackptr, fault_addr)){
      //valid stack growth
      //printf("%d-%s: ---- valid stack growth\n", thread_current()->tid, __func__);
      // if (!page_lookup(t->pages, stackptr)){
      //   fpage = pg_round_down(stackptr);
      //   //If the stack pointer itself is not allocated,
      //   //first allocate this stack pointer
      // }
      p = page_allocate(fpage, false);
      if (!p){
        PANIC("Cannot allocate a new page for stack growth");
      }
      //still return, let the next page fault page_in this
      return;
    } else {
      // if (!user){
      //   f->eip = (void (*)(void))f->eax;
      //   f->eax= 0xffffffff;
      // } else {
      //   kill (f);  
      // }
      // printf("%d-%s: exitting\n", thread_current()->tid, __func__);
      sys_exit(-1);
      return;
    }
    
  }
  //p exists already
  if (write && p->read_only){
    sys_exit(-1);
    // if (!user){
    //   sys_exit(-1);
    // } else {
    //   kill (f);
    // }
    return;
  }
  
  //this is a valid access
  if (not_present && p){
    // show_page(p);
    if (!page_in(fpage)){
      thread_exit ();
      PANIC("Failed to get in page somehow");
    }
    bool success = false;

    success = pagedir_set_page (t->pagedir, fpage, p->frame->base, !p->read_only);

    if (!success){
      ////////printf("%d-%s: Failed to install a page\n", thread_current()->tid, __func__);
      page_deallocate(fpage);
      PANIC("in page_fault() ????");
    }

    ////////printf("%d-%s: Got a addr? %p->%p\n", thread_current()->tid, __func__, fault_addr, pagedir_get_page(t->pagedir, fault_addr));
    if (pagedir_get_page(t->pagedir, fault_addr) == NULL){
      PANIC("Dit not find a frame");
    }
    pagedir_set_accessed(t->pagedir, fpage, true);
    // if (!pagedir_is_dirty(t->pagedir, fpage) && write){
    //   pagedir_set_dirty(t->pagedir, fpage, true);
    // }

  } else if (!user){
    //present and p is valid
    f->eip = (void (*)(void))f->eax;
    f->eax= 0xffffffff;
    return;
  }
  // printf("%d-%s: Uninned a frame\n", thread_current()->tid, __func__);
  frame_unpin(p->frame);


  //sys_exit(-1);

  /* To implement virtual memory, delete the rest of the function
     body, and replace it with code that brings in the page to
     which fault_addr refers. */
  // printf ("Page fault at %p: %s error %s page in %s context.\n",
  //         fault_addr,
  //         not_present ? "not present" : "rights violation",
  //         write ? "writing" : "reading",
  //         user ? "user" : "kernel");
  // kill (f);
}

