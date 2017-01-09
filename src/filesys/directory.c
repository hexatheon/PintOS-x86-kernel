#include "filesys/directory.h"
#include <stdio.h>
#include <string.h>
#include <list.h>
#include "threads/thread.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"

/* A directory. */
struct dir 
  {
    struct inode *inode;                /* Backing store. */
    off_t pos;                          /* Current position. */
    uint8_t position_occupier_not_used;  /* struct dir and struct file are thus interchangeable. See sys_readdir */
  };

/* A single directory entry. */
struct dir_entry 
  {
    block_sector_t inode_sector;        /* Sector number of header. */
    char name[NAME_MAX + 1];            /* Null terminated file name. */
    bool in_use;                        /* In use or free? */
  };

static bool check_dir_empty (struct inode *inode);
static char* strdup(const char* src);

int dir_entry_size() {
  return sizeof(struct dir_entry);
}






static char* strdup(const char* src) {
  int length = strlen(src);
  char* copy = malloc(sizeof (char) * (length + 1));
  memcpy(copy, src, sizeof(char) * (length + 1));
  return copy;
}

char* pick_filename(const char* path) {
  char* copy = strdup(path);
  int i;
  ASSERT(strlen(path)==strlen(copy));
  for (i = strlen(copy); (copy[i] != '/' && i != 0) ; i--) {
  }
  char* name;
  if (copy[i] != '/') {
    name = &copy[i];
  } else {
    name = &copy[i+1];
  }
  char* ret = strdup(name);
  free(copy);
  return ret;
}


char* pick_pure_directory_path(const char* path) {
  char* copy = strdup(path);
  int i;
  ASSERT(strlen(path)==strlen(copy));
  for (i = strlen(copy); (copy[i] != '/' && i != 0) ; i--) {
  }

  if (copy[i] == '/') {
    copy[i+1] = '\0';
  } else {
    copy[i] = '\0';
  }
  char* ret = strdup(copy);
  free(copy);
  return ret;
}


// traverse the directory path one by one.
struct dir* dir_traverse (const char *path) {
  char* copy = strdup(path);
  struct dir *curr;

  if(copy[0] == '/') {
    curr = dir_open_root();
  } else {
    struct thread *t = thread_current();
    if (t->cwd == NULL)
      curr = dir_open_root();
    else {
      curr = dir_reopen( t->cwd );
    }
  }

  char *token, *save_ptr;
  for (token = strtok_r(copy, "/", &save_ptr); 
       token != NULL;
       token = strtok_r(NULL, "/", &save_ptr))
  {
    struct inode *inode = NULL;
    if(!dir_lookup(curr, token, &inode)) goto FAIL;
    struct dir *next = dir_open(inode);
    dir_close(curr);
    curr = next;
  }

  if (inode_is_removed (dir_get_inode(curr))) goto FAIL;

  return curr;

FAIL:
  free(copy);
  dir_close(curr);
  return NULL;
}


/* Creates a directory with space for ENTRY_CNT entries in the
   given SECTOR.  Returns true if successful, false on failure. */
bool dir_create (block_sector_t sector, size_t entry_cnt)
{
  bool success = inode_create (sector, entry_cnt * sizeof (struct dir_entry), DIR_INODE);
  if(!success) 
    return false;

// set 1st entry to be parent. Put your own inode sector here, since dir_create
// is called during formatting. Parent of root is root itself. Reserve the space.
  struct dir *dir = dir_open(inode_open(sector));
  struct dir_entry e;
  e.inode_sector = sector;
  strlcpy (e.name, "..", sizeof e.name);
  if (inode_write_at(dir->inode, &e, sizeof e, 0) != sizeof e) 
    success = false;
  dir_close (dir);

  return success;
}



/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *
dir_open (struct inode *inode) 
{
  struct dir *dir = calloc (1, sizeof *dir);
  if (inode != NULL && dir != NULL)
    {
      dir->inode = inode;
      dir->pos = 0;
      return dir;
    }
  else
    {
      inode_close (inode);
      free (dir);
      return NULL; 
    }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *
dir_open_root (void)
{
  return dir_open(inode_open (ROOT_DIR_SECTOR));
}



/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *
dir_reopen (struct dir *dir) 
{
  return dir_open(inode_reopen (dir->inode));
}

/* Destroys DIR and frees associated resources. */
void
dir_close (struct dir *dir) 
{
  if (dir != NULL)
    {
      inode_close (dir->inode);
      free (dir);
    }
}

/* Returns the inode encapsulated by DIR. */
struct inode *
dir_get_inode (struct dir *dir) 
{
  return dir->inode;
}

/* Searches DIR for a file with the given NAME.
   If successful, returns true, sets *EP to the directory entry
   if EP is non-null, and sets *OFSP to the byte offset of the
   directory entry if OFSP is non-null.
   otherwise, returns false and ignores EP and OFSP. */
static bool
lookup (const struct dir *dir, const char *name,
        struct dir_entry *ep, off_t *ofsp) 
{
  struct dir_entry e;
  size_t ofs;
  
  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) 
    if (e.in_use && !strcmp (name, e.name)) 
      {
        if (ep != NULL)
          *ep = e;
        if (ofsp != NULL)
          *ofsp = ofs;
        return true;
      }
  return false;
}





/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool
dir_lookup (const struct dir *dir, 
            const char *name,
            struct inode **inode)
{
  struct dir_entry e;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);
  *inode = NULL;

  if (strcmp (name, ".") == 0) {
    *inode = inode_reopen (dir->inode);
  } else if (lookup (dir, name, &e, NULL)) {
    *inode = inode_open (e.inode_sector);
  }

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.
   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool
dir_add (struct dir *dir, 
         const char *name, 
         block_sector_t inode_sector, 
         enum inode_type type)
{
  struct dir_entry e;
  off_t ofs;
  bool success = false;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (*name == '\0' || strlen (name) > NAME_MAX)
    return false;

  if (lookup (dir, name, NULL, NULL))
    goto done;

  // update the first entry of child directory to be parent
  if (type == DIR_INODE)
  {
    struct dir *child_dir = dir_open( inode_open(inode_sector) );
    if(child_dir == NULL) goto done;

    e.inode_sector = inode_get_inumber( dir_get_inode(dir) );
    strlcpy (e.name, "..", sizeof e.name);
    if (inode_write_at(child_dir->inode, &e, sizeof e, 0) != sizeof e) {
      dir_close (child_dir);
      goto done;
    }
    dir_close (child_dir);
  }

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = 0; inode_read_at (dir->inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy (e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at (dir->inode, &e, sizeof e, ofs) == sizeof e;

 done:
  return success;
}



bool check_dir_empty (struct inode *inode) {
  struct dir_entry e;
  off_t ofs;

  for (ofs = sizeof e;
       inode_read_at (inode, &e, sizeof e, ofs) == sizeof e;
       ofs += sizeof e) {
    if (e.in_use)
      return false;
  }
  return true;
}


/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool dir_remove (struct dir *dir, const char *name) 
{
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT (dir != NULL);
  ASSERT (name != NULL);

  if (!strcmp (name, ".") || !strcmp (name, ".."))
    return false;

  /* Find directory entry. */
  if (!lookup (dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open (e.inode_sector);
  if (inode == NULL)
    goto done;


  /* Verify that it is not an in-use or non-empty directory. */
  if (inode_is_directory(inode)) {
    bool empty = check_dir_empty (inode);
    if (!empty )
      goto done;
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at (dir->inode, &e, sizeof e, ofs) != sizeof e) 
    goto done;

  /* Remove inode. */
  inode_remove (inode);
  success = true;

 done:
  inode_close (inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool
dir_readdir (struct dir *dir, char name[NAME_MAX + 1])
{
  //printf("enter dir_readdir\n");
  struct dir_entry e;


  while (inode_read_at (dir->inode, &e, sizeof e, dir->pos) == sizeof e)
    {
      dir->pos += sizeof e;
      if (e.in_use && !!strcmp(e.name, ".."))
        {
          strlcpy (name, e.name, NAME_MAX + 1);
          //printf("readdir_name: %s at sector %d\n", e.name, e.inode_sector);
          return true;
        }
    }
  return false;
}
