#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_CNT 123
#define INDIRECT_BLOCK_CNT 128

#define SINGLE_INDIRECT_INDEX DIRECT_CNT + 0
#define DOUBLE_INDIRECT_INDEX DIRECT_CNT + 1

#define SECTOR_CNT DIRECT_CNT + 2

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT];
    enum inode_type type;
    off_t length;                       /* File size in bytes. */
    unsigned magic; 
  };



struct multi_index {
  int level_one;
  int level_two;
  int level_three;
};


struct inode_indirect_block {
  block_sector_t blocks[INDIRECT_BLOCK_CNT];
};


static block_sector_t index_to_sector (const struct inode_disk *idisk, off_t index);
static block_sector_t byte_to_sector (const struct inode *inode, off_t pos);



static bool inode_allocate (struct inode_disk *disk_inode);
static bool inode_extend (struct inode_disk *disk_inode, off_t length);


static bool inode_deallocate (struct inode *inode);
void recursive_deallocate(block_sector_t sector, int level);


/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t
bytes_to_sectors (off_t size)
{
  return DIV_ROUND_UP (size, BLOCK_SECTOR_SIZE);
}


/* In-memory inode. */
struct inode
  {
    struct list_elem elem;              /* Element in inode list. */
    block_sector_t sector;              /* Sector number of disk location. */
    int open_cnt;                       /* Number of openers. */
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    struct inode_disk data;
  };



/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void)
{
  list_init (&open_inodes);
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, enum inode_type type)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      disk_inode->type = type;
      memset(disk_inode->sectors, 0, SECTOR_CNT);
      if (inode_allocate (disk_inode))
        {
          cache_write (fs_device, sector, disk_inode);
          success = true;
        }
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode * inode_open (block_sector_t sector)
{
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin (&open_inodes); e != list_end (&open_inodes);
       e = list_next (e))
    {
      inode = list_entry (e, struct inode, elem);
      if (inode->sector == sector)
        {
          inode_reopen (inode);
          return inode;
        }
    }

  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  cache_read (fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t
inode_get_inumber (const struct inode *inode)
{
  return inode->sector;
}

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void
inode_close (struct inode *inode)
{
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0)
    {
      /* Remove from inode list and release lock. */
      list_remove (&inode->elem);

      /* Deallocate blocks if removed. */
      if (inode->removed)
        {
          free_map_release (inode->sector, 1);
          inode_deallocate (inode);
        }

      free (inode);
    }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode)
{
  ASSERT (inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset)
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0)
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          cache_read (fs_device, sector_idx, buffer + bytes_read);
        }
      else
        {
          /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }
          cache_read (fs_device, sector_idx, bounce);
          memcpy (buffer + bytes_read, bounce + sector_ofs, chunk_size);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_read += chunk_size;
    }
  free (bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode).
   */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset)
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  // exceed file bound
  if( byte_to_sector(inode, offset + size - 1) == -1u) {
    bool success;
    success = inode_extend (& inode->data, offset + size);
    if (!success) return 0;
    inode->data.length = offset + size;
    cache_write (fs_device, inode->sector, & inode->data);
  }

  while (size > 0)
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (fs_device, sector_idx, buffer + bytes_written);
        }
      else
        {
          /* We need a bounce buffer. */
          if (bounce == NULL)
            {
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
          if (sector_ofs > 0 || chunk_size < sector_left)
            cache_read (fs_device, sector_idx, bounce);
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (fs_device, sector_idx, bounce);
        }

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  free (bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void
inode_deny_write (struct inode *inode)
{
  inode->deny_write_cnt++;
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void
inode_allow_write (struct inode *inode)
{
  ASSERT (inode->deny_write_cnt > 0);
  //ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  if (!(inode->deny_write_cnt <= inode->open_cnt)) {
    printf("inode->deny_write_cnt is %d, inode->open_cnt is %d\n", inode->deny_write_cnt, inode->open_cnt);
  }
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  return inode->data.length;
}

/* Returns whether the file is directory or not. */
bool
inode_is_directory (const struct inode *inode)
{
  return (inode->data.type == DIR_INODE);
}

/* Returns whether the file is removed or not. */
bool
inode_is_removed (const struct inode *inode)
{
  return inode->removed;
}

static
bool inode_allocate (struct inode_disk *disk_inode)
{
  return inode_extend (disk_inode, disk_inode->length);
}










static struct multi_index calculate_indices(off_t sector_index) {
  struct multi_index ret;
  ret.level_one = -1;
  ret.level_two = -1;
  ret.level_three = -1;
  if (sector_index < DIRECT_CNT) {
    ret.level_one = sector_index;
    return ret;
  } else if (sector_index < DIRECT_CNT + INDIRECT_BLOCK_CNT) {
    ret.level_one = SINGLE_INDIRECT_INDEX;
    ret.level_two = sector_index - DIRECT_CNT;
    return ret;
  } else if (sector_index < DIRECT_CNT + INDIRECT_BLOCK_CNT \
                            + INDIRECT_BLOCK_CNT * INDIRECT_BLOCK_CNT) {

    int block_for_double_indirect = sector_index - DIRECT_CNT - INDIRECT_BLOCK_CNT;
    ret.level_one = DOUBLE_INDIRECT_INDEX;
    ret.level_two = block_for_double_indirect / INDIRECT_BLOCK_CNT;
    ret.level_three = block_for_double_indirect % INDIRECT_BLOCK_CNT;
    return ret;
  }
  // NOT Reachable
  ASSERT(1==-1);
}




static block_sector_t byte_to_sector (const struct inode *inode, off_t pos) {
  ASSERT (inode != NULL);
  if (!(0 <= pos && pos < inode->data.length))
    return -1;

  off_t index = pos / BLOCK_SECTOR_SIZE;
  struct inode_disk* idisk = &inode->data;

  block_sector_t ret;
  struct inode_indirect_block indirect_block;
  struct multi_index mult = calculate_indices(index);

  ASSERT(mult.level_one != -1);
  ret = idisk->sectors[mult.level_one];
  if (mult.level_two == -1) return ret;

  cache_read(fs_device, ret, &indirect_block);
  ret = indirect_block.blocks[mult.level_two];
  if (mult.level_three == -1) return ret;

  cache_read(fs_device, ret, &indirect_block);
  ret = indirect_block.blocks[mult.level_three];
  return ret;
}

/* Note: this function extends the inode UP TO the length, NOT by the length */
static bool inode_extend (struct inode_disk *disk_inode, off_t length) {
  static char zeros[BLOCK_SECTOR_SIZE];
  struct inode_indirect_block indirect_block;
  block_sector_t* block = indirect_block.blocks;

  block_sector_t* sectors = disk_inode->sectors;

  size_t num_sectors = bytes_to_sectors(length);
  unsigned int i;
  struct multi_index mult;

  for (i=0; i < num_sectors; i++) {
    mult = calculate_indices(i);

    if (sectors[mult.level_one] == 0) {
      if (!free_map_allocate (1, &sectors[mult.level_one])) 
        return false;
      cache_write(fs_device, sectors[mult.level_one], zeros);
    }
    if (mult.level_two == -1) continue;

    cache_read(fs_device, sectors[mult.level_one], &indirect_block);
    if (block[mult.level_two] == 0) {
      if (!free_map_allocate (1, &block[mult.level_two])) return false;
      cache_write(fs_device, block[mult.level_two], zeros);
      cache_write(fs_device, sectors[mult.level_one], &indirect_block);
    }
    if (mult.level_three == -1) continue;

    block_sector_t origin = block[mult.level_two];
    cache_read(fs_device, origin, &indirect_block);
    if (block[mult.level_three] == 0) {
      if (!free_map_allocate (1, &block[mult.level_three])) return false;
      cache_write(fs_device, block[mult.level_three], zeros);
      cache_write(fs_device, origin, &indirect_block);
    }
  }
  return true;
}


void recursive_deallocate(block_sector_t sector, int level) {
  if (sector == 0) return;

  if (level == 0) {
    free_map_release(sector, 1);
  } else {
    struct inode_indirect_block indirect_block;
    size_t i;
    cache_read(fs_device, sector, &indirect_block);
    for (i=0; i< INDIRECT_BLOCK_CNT; i++)
      recursive_deallocate(indirect_block.blocks[i], level-1);
    free_map_release(sector, 1);
  }
}

static bool inode_deallocate (struct inode *inode) {
  if(inode->data.length < 0) return false;

  block_sector_t* sectors = inode->data.sectors;
  size_t i;
  for (i=0; i<DIRECT_CNT; i++) 
    recursive_deallocate(sectors[i], 0);
  recursive_deallocate(sectors[SINGLE_INDIRECT_INDEX], 1);
  recursive_deallocate(sectors[DOUBLE_INDIRECT_INDEX], 2);
  return true;
}