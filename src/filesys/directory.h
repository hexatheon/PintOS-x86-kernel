#ifndef FILESYS_DIRECTORY_H
#define FILESYS_DIRECTORY_H

#include <stdbool.h>
#include <stddef.h>
#include "devices/block.h"
#include "filesys/inode.h"

/* Maximum length of a file name component.
   This is the traditional UNIX maximum length.
   After directories are implemented, this maximum length may be
   retained, but much longer full path names must be allowed. */
#define NAME_MAX 14

struct inode;
struct dir;


bool dir_create (block_sector_t sector, size_t entry_cnt);
char* pick_filename(const char* path);
char* pick_pure_directory_path(const char* path);
struct dir *dir_traverse (const char* path);
struct dir *dir_open (struct inode *);
struct dir *dir_open_root (void);



struct dir *dir_reopen (struct dir *);
void dir_close (struct dir *);
struct inode *dir_get_inode (struct dir *);

bool dir_lookup (const struct dir *, const char *name, struct inode **);

bool dir_add (struct dir *, const char *name, block_sector_t, enum inode_type type);

bool dir_remove (struct dir *, const char *name);
bool dir_readdir (struct dir *, char name[NAME_MAX + 1]);


int dir_entry_size(void);
#endif
