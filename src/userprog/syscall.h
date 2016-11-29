#include "threads/thread.h"

#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include "filesys/file.h"

struct process_control_block {
  tid_t pid;
  char* pname;
  //struct list_elem allelem;
  struct list_elem elem;
  int signal;
  int exec_signal;
  struct semaphore exec;
  struct semaphore wait;
};


void syscall_init (void);
void sys_exit(int status);
void close_file(int fd);
void close_all_files(void);
struct file_desc* search_fd(int fd);


void remove_all_mappings(void);



//replacement of file_read, etc.
int sys_file_read(struct file* f, void* dst, off_t size);
int sys_file_read_at(struct file* f, void* dst, off_t size, off_t offset);
int sys_file_write(struct file* f, const void* src, off_t size);
int sys_file_write_at(struct file* f, const void* src, off_t size, off_t offset);

bool sys_filesys_create (const char *name, off_t initial_size);
struct file* sys_filesys_open (const char* name);
bool sys_filesys_remove (const char *name);

struct file* sys_file_reopen (struct file * f);
void sys_file_close (struct file * f);


void sys_file_deny_write (struct file * f);
void sys_file_allow_write (struct file * f);

void sys_file_seek(struct file* f, off_t offset);
off_t sys_file_tell(struct file* f);
off_t sys_file_length(struct file* f);
#endif /* userprog/syscall.h */
