#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/thread.h"
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  inode_init ();
  free_map_init ();
  cache_init();

  if (format) 
    do_format ();

  free_map_open ();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  free_map_close ();
  cache_flush();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create (const char *path, 
                     off_t initial_size, 
                     enum inode_type type)
{
  block_sector_t inode_sector = 0;

  char* filename = pick_filename(path);
  char* direct = pick_pure_directory_path(path);
  struct dir* dir = dir_traverse (direct);
  free(direct);


  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, initial_size, type)
                  && dir_add (dir, filename, inode_sector, type));

  if (!success && inode_sector != 0)
    free_map_release (inode_sector, 1);
  dir_close (dir);
  free(filename);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  int l = strlen(name);
  if (l == 0) 
    return NULL;

  char* filename = pick_filename(name);
  char* direct = pick_pure_directory_path(name);
  struct dir* dir = dir_traverse (direct);
  free(direct);

  if (dir == NULL) 
    goto FAIL;

  struct inode *inode = NULL;
  if (strlen(filename) > 0) {
    dir_lookup (dir, filename, &inode);
    dir_close (dir);
  } else { // dir needed
    inode = dir_get_inode (dir);
  }

  if (inode == NULL || inode_is_removed (inode))
    goto FAIL;

  free(filename);
  return file_open (inode);

FAIL:
  free(filename);
  return NULL;
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */

bool filesys_remove (const char *name)
{
  char* filename = pick_filename(name);
  char* direct = pick_pure_directory_path(name);
  struct dir* dir = dir_traverse (direct);

  bool success = (dir != NULL && dir_remove (dir, filename));
  dir_close (dir);

  free(direct);
  free(filename);
  return success;
}

bool filesys_chdir (const char *name)
{
  struct dir *dir = dir_traverse (name);

  if(dir == NULL)
    return false;

  dir_close (thread_current()->cwd);
  thread_current()->cwd = dir;
  return true;
}



/* Formats the file system. */
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");
  free_map_close ();
  printf ("done.\n");
}
