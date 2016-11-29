#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/vaddr.h"
#include "devices/shutdown.h"
#include "userprog/process.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include "vm/page.h"
#include "userprog/pagedir.h"



struct file_desc {
  int fd;
  struct file *file;
  struct list_elem elem;
};

static struct lock stupid_lock;
//will make it reader writer lock in the future

static int get_user(const uint8_t *uaddr);

void syscall_init (void);
static void syscall_handler (struct intr_frame *f);

static bool check_pointer(void* head, int size);
static bool check_string(void* head);
static bool check_buffer(char* buf, int size);
static int load_args(uint32_t* argv, void* src, uint8_t size);

void sys_seek(int fd, unsigned position);
int sys_filesize(int fd);
int sys_tell(int fd);
int sys_open(const char *file_name);

static int sys_read (int handle, void *udst_, unsigned size);
static int sys_write (int handle, void *usrc_, unsigned size);

static int sys_file_read_gen(struct file* f, void* dst, off_t size, off_t offset, bool is_at);
static int sys_file_write_gen(struct file* f, const void* src, off_t size, off_t offset, bool is_at);

static struct mapping *lookup_mapping (int handle);
static void unmap (struct mapping *m);
static int sys_mmap (int handle, void *addr);
static int sys_munmap (int mapping);

static int get_user(const uint8_t *uaddr) {
  if(!is_user_vaddr(uaddr))
    return -1;
  int result;
  asm ("movl $1f, %0; movzbl %1, %0; 1:"
       : "=&a" (result) : "m" (*uaddr));
  return result;
}


static bool check_pointer(void* head, int size) {
  int i;
  for (i = 0; i < size; i++) {
    if (get_user(((uint8_t *)head) + i) == -1) {
      return false;
    }
  }
  return true;
}

static bool check_string(void* head) {
  char iter = 1;
  while (iter != '\0') {
    iter = get_user((uint8_t*)head);
    if (iter == -1) {
      return false;
    }
    head++;
  }
  return true;
}

static bool check_buffer(char* buf, int size) {
  if ((!check_pointer(buf, 1)) || (!check_pointer(buf + size, 1))) {
    return false;
  } else {
    return true;
  }
}


static int load_args(uint32_t* argv, void* src, uint8_t size) {
  if (!check_pointer(src, size * 4))
    return -1;
  int i = 0;
  for (i=0; i<size; i++) {
    argv[i] = *(int*) (src + 4*i);
  }
  return 1;
}


void syscall_init (void) {
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");

  lock_init(&stupid_lock);
}


void sys_exit(int status) {
  struct process_control_block *pcb = thread_current()->pcb;
  ////printf("%d-%s: down-ing exec\n", thread_current()->tid, __func__);
  // if (thread_tid() != 1){
  //   sema_down(&pcb->exec);
  // }
  ////printf("%d-%s: downed exec\n", thread_current()->tid, __func__);
  pcb->signal = status;
  thread_exit();
}


static void syscall_handler (struct intr_frame *f) {
  uint32_t sys_call_num;
  uint32_t argv[3];
  memset(argv, 0, 3);
  if (!check_pointer(f->esp, 4)) {
    sys_exit(-1);
    return;
  } else {
    sys_call_num = *(int*)(f->esp);
  }

  switch (sys_call_num) {
    case SYS_EXIT:
      if (load_args(argv, f->esp+4, 1) == -1)
        sys_exit(-1);
      sys_exit(argv[0]);
      break;

    case SYS_WRITE:
      if (load_args(argv, f->esp+4, 3) == -1){
        sys_exit(-1);
      }
      if (!check_buffer((char*)argv[1], argv[2])) {
        sys_exit(-1);
      }
      f->eax = sys_write(argv[0], (void*) argv[1], argv[2]);
      break;

    case SYS_READ:
      if (load_args(argv, f->esp+4, 3) == -1) {
        sys_exit(-1);
      }
      if (!check_buffer((char*)argv[1], argv[2])) {
        sys_exit(-1);
      }
      f->eax = sys_read(argv[0], (void*) argv[1], argv[2]);
      break;

    case SYS_HALT:
      shutdown();
      break;
    case SYS_CREATE:
      if ((load_args(argv, f->esp+4, 2) == -1)||(!check_string((char*)argv[0])))
        sys_exit(-1);
      f->eax = sys_filesys_create((char*)argv[0], argv[1]);
      break;
    case SYS_REMOVE:
      if ((load_args(argv, f->esp+4, 1) == -1)||(!check_string((char*)argv[0])))
        sys_exit(-1);
      f->eax = sys_filesys_remove((char*)argv[0]);
      break;

    case SYS_OPEN:
      if ((load_args(argv, f->esp+4, 1) == -1)||(!check_string((char*)argv[0])))
        sys_exit(-1);
      f->eax = sys_open((char*)argv[0]);
      break;

    case SYS_CLOSE:
      if (load_args(argv, f->esp+4, 1) == -1)
        sys_exit(-1);
      close_file(argv[0]);
      break;

    case SYS_WAIT:
      if (load_args(argv, f->esp+4, 1) == -1)
        sys_exit(-1);
      f->eax = process_wait(argv[0]);
      break;

    case SYS_EXEC:
      if ((load_args(argv, f->esp+4, 1) == -1)||(!check_string((char*)argv[0])))
        sys_exit(-1);
      char *str = (char*)argv[0];
      if ( strlen(str) >= PGSIZE ){
        // string size cannot be larger than PSIZE
        sys_exit(-1);
      }
      if (strlen(str) == 0 || str[0] == ' '){
        //command string should be non-empty and not starting with a space.
        sys_exit(-1);
      }
      f->eax = process_execute(str);
      break;

    case SYS_FILESIZE:
      if (load_args(argv, f->esp+4, 1) == -1)
        sys_exit(-1);
      f->eax = sys_filesize(argv[0]);
      break;

    case SYS_TELL:
      if (load_args(argv, f->esp+4, 1) == -1)
        sys_exit(-1);
      f->eax = sys_tell(argv[0]);
      break;

    case SYS_SEEK:
      if (load_args(argv, f->esp+4, 2) == -1)
        sys_exit(-1);
      sys_seek(argv[0], argv[1]);
      break;

    case SYS_MMAP:
      if (load_args(argv, f->esp+4, 2) == -1) {
        sys_exit(-1);
      }
      f->eax = sys_mmap(argv[0], (void*) argv[1]);
      break;

    case SYS_MUNMAP:
      if (load_args(argv, f->esp+4, 1) == -1) {
        sys_exit(-1);
      }
      sys_munmap(argv[0]);
      break;

    default:
      sys_exit(-1);
      return;
  }
}



int sys_open (const char *file_name) {
  static int next_fd = 2;
  struct file* f = sys_filesys_open (file_name);
  if (f == NULL)
    return -1;
  struct file_desc* F = malloc (sizeof(struct file_desc));
  if (F == NULL)
    return -1;
  F->fd = next_fd++;
  F->file = f;

  list_push_back(&thread_current()->file_descriptors, &F->elem);

  return F->fd;
}

int sys_filesize (int fd) {
  if (search_fd(fd) != NULL){
    struct file_desc* F = search_fd(fd);
    return sys_file_length(F->file);
  }
  return -1;
}

int sys_tell (int fd) {
  if (search_fd(fd) != NULL){
    struct file_desc* F = search_fd(fd);
    return sys_file_tell(F->file);
  }
  return -1;
}

void sys_seek (int fd, unsigned position){
  if (search_fd(fd) != NULL){
    struct file_desc* F = search_fd(fd);
    sys_file_seek(F->file, position);
  }
}

struct file_desc* search_fd(int fd) {
  struct list_elem *e;
  struct file_desc *fe = NULL;
  struct list *fd_table = &thread_current()->file_descriptors;

  for (e = list_begin (fd_table); e != list_end (fd_table);
       e = list_next (e))
    {
      struct file_desc *tmp = list_entry (e, struct file_desc, elem);
      if(tmp->fd == fd){
        fe = tmp;
        break;
      }
    }

  return fe;
}

void close_file(int fd) {
  struct file_desc* F = search_fd(fd);
  if (F != NULL){
    sys_file_close(F->file);
    list_remove(&F->elem);
    free(F);
  }
}

void close_all_files(void) {
  struct list* fd = &thread_current()->file_descriptors;
  struct list_elem *e = list_begin (fd);
  while (e != list_end (fd))
    {
      struct file_desc *tmp = list_entry (e, struct file_desc, elem);
      e = list_next (e);
      close_file(tmp->fd);
    }
  sys_file_close (thread_current()->bin_file);
}


static int
sys_read (int handle, void *udst_, unsigned size)
{
  struct file_desc* F = search_fd(handle);
  int ret = -1;
  if (F) {
    page_lock(udst_, true);//writing to this memory
    ret = sys_file_read(F->file, (char*) udst_, size);  
    page_unlock(udst_);
  }
  return ret;
}

static int
sys_write (int handle, void *usrc_, unsigned size)
{
  int ret = -1;
  if (handle == STDOUT_FILENO) {
    page_lock(usrc_, false);//reading from this memory
    putbuf((char*)usrc_, size);
    page_unlock(usrc_);
    ret = size;
  } else {
    struct file_desc* F = search_fd(handle);
    if (F){
      page_lock(usrc_, false);//reading from this memory
      ret = sys_file_write(F->file, (char*) usrc_, size);
      page_unlock(usrc_);  
    }
  }
  return ret;
}


/*
Managing the memory mapped files

Files can be mapped to virtual address using mmap() system call.  You have
to implement mmap and munmap functions to handle mapping files to
memory. Your implementation should be able to track what memory is used by
memory mapped files. So storing a list of mapped memories for each process
and files who are mapped to those memories is suggested.


*/
/* Binds a mapping id to a region of memory and a file. */
struct mapping
{
  struct list_elem elem;      /* List element. */
  int handle;                 /* Mapping id. */
  struct file *file;          /* File. */
  uint8_t *base;              /* Start of memory mapping. */
  size_t page_cnt;            /* Number of pages mapped. */
};


void remove_all_mappings(void){
  struct thread* t = thread_current();
  struct list *mappings = &t->mappings;
  struct list_elem *e = list_begin(mappings);
  while (e != list_end(mappings)){
    struct mapping* m = list_entry(e, struct mapping, elem);
    e = list_next(e);
    unmap(m);
  }
  return;
}

static struct mapping *lookup_mapping (int handle) {
  struct list* l = &thread_current()->mappings;
  struct list_elem* e;
  for (e = list_begin(l); e != list_end(l) ; e = list_next(e)){
    struct mapping* m = list_entry(e, struct mapping, elem);
    if (m->handle == handle){
      return m;
    }
  }
  return NULL;
}

 // Remove mapping M from the virtual address space,                              
 //   writing back any pages that have changed. 
static void unmap (struct mapping *m) {
  ASSERT(m != NULL);
  //////printf("%d-%s: Removing a mapping w/ handle == %d\n", thread_current()->tid, __func__, m->handle);
  struct thread* t = thread_current();
  void* upage = m->base;
  int i = m->page_cnt;
  while (i-- > 0){
    void* kpage = pagedir_get_page(t->pagedir, upage);
    //////printf("%d-%s: Freeing upage == %p\n", thread_current()->tid, __func__, upage);
    if (kpage){
      //needs to write. if it is not present then assume 
      //the content was already written back
      struct page* p = page_lookup(t->pages, upage);
      ASSERT(p != NULL);
      ASSERT(p->file == m->file);
      if (pagedir_is_dirty(t->pagedir, upage)){
        sys_file_write_at(m->file, kpage, p->file_bytes, p->file_offset); 
      }
    }    
    page_deallocate(upage);
    upage += PGSIZE;
  }
  
  list_remove(&m->elem);
  //////printf("%d-%s: Removed a mapping w/ handle == %d\n", thread_current()->tid, __func__, m->handle);
  sys_file_close(m->file);
  //////printf("%d-%s: Closed file\n", thread_current()->tid, __func__);
  free(m);
  return;
}

static int sys_mmap (int handle, void *addr) {
  //////printf("%d-%s: building a mapping. handle == %d and addr == %p\n", thread_current()->tid, __func__, handle, addr);
  struct thread* t = thread_current();
  struct file_desc* fd = search_fd(handle);
  //////printf("%d-%s: fd == %p\n", thread_current()->tid, __func__, fd);
  if (fd == NULL){
    return -1;
  }
  struct file* nf = sys_file_reopen(fd->file);
  //////printf("%d-%s: nf == %p\n", thread_current()->tid, __func__, nf);
  if (nf == NULL){
    return -1;
  }
  off_t len = sys_file_length(nf);

  if (len == 0 || addr == 0 || pg_ofs (addr) != 0 || handle < 2){
    //////printf("%d-%s: Bad. len == %d\n", thread_current()->tid, __func__, len);
    return -1;
  }
  struct mapping* mmap = malloc(sizeof(struct mapping));
  mmap->handle = handle;
  mmap->file = nf;//??use the new file?
  mmap->base = addr;
  mmap->page_cnt = len / PGSIZE + ((len % PGSIZE == 0) ? 0 : 1);

  off_t offset = 0;
  void* upage = addr;
  while (offset < len){

    struct page* p = page_allocate(upage, false);
    if (p == NULL){
      return -1;
    }
    p->file = nf;
    p->file_offset = offset;
    p->file_bytes = (len - offset > PGSIZE) ? PGSIZE : len - offset;
    // p->private = false;

    offset += p->file_bytes;
    upage += PGSIZE;
  }
  list_push_back(&t->mappings, &mmap->elem);
  //////printf("%d-%s: Built a mapping with id == %d\n", thread_current()->tid, __func__, handle);
  return handle;
}

static int sys_munmap (int mapping) {
  unmap (lookup_mapping (mapping));
  return 0;
}


static int sys_file_read_gen(struct file* f, void* dst, off_t size, off_t offset, bool is_at){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  int ret;
  if (is_at){
    ret = file_read_at(f, dst, size, offset);
  } else {
    ret = file_read(f, dst, size);
  }
  //printf("%d-%s: Checking byte %x\n", thread_current()->tid,__func__, 0xff & (*(char*)(dst + 0xe40)));
  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}
static int sys_file_write_gen(struct file* f, const void* src, off_t size, off_t offset, bool is_at){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }
  //printf("%d-%s: Checking byte %x\n", thread_current()->tid,__func__, 0xff & (*(char*)(src + 0xe40)));
  int ret;
  if (is_at){
    ret = file_write_at(f, src, size, offset);
  } else {
    ret = file_write(f, src, size);
  }

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}
int sys_file_read(struct file* f, void* dst, off_t size){
  return sys_file_read_gen(f, dst, size, 0, false);
}
int sys_file_read_at(struct file* f, void* dst, off_t size, off_t offset){
  return sys_file_read_gen(f, dst, size, offset, true);
}
int sys_file_write(struct file* f, const void* src, off_t size){
  return sys_file_write_gen(f, src, size, 0, false);
}
int sys_file_write_at(struct file* f, const void* src, off_t size, off_t offset){
  return sys_file_write_gen(f, src, size, offset, true);
}


bool sys_filesys_create (const char *name, off_t initial_size){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  bool ret = filesys_create (name, initial_size);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}

struct file* sys_filesys_open (const char* name){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  struct file* ret = filesys_open(name);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}

bool sys_filesys_remove (const char *name){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  bool ret = filesys_remove (name);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}
struct file* sys_file_reopen (struct file * f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  struct file* ret = file_reopen(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}
void sys_file_close (struct file * f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  file_close(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
}
void sys_file_deny_write (struct file * f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  file_deny_write(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
}
void sys_file_allow_write (struct file * f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  file_allow_write(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
}
void sys_file_seek(struct file* f, off_t offset){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  file_seek(f, offset);

  if (!already_held){
    lock_release(&stupid_lock);
  }
}
off_t sys_file_tell(struct file* f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  off_t ret = file_tell(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}
off_t sys_file_length(struct file* f){
  bool already_held = lock_held_by_current_thread(&stupid_lock);
  if (!already_held){
    lock_acquire(&stupid_lock);  
  }

  off_t ret = file_length(f);

  if (!already_held){
    lock_release(&stupid_lock);
  }
  return ret;
}