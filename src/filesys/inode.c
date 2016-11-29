#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "filesys/cache.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include <stdio.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

#define DIRECT_CNT 123
#define INDIRECT_CNT 1
#define DBL_INDIRECT_CNT 1
#define SECTOR_CNT (DIRECT_CNT + INDIRECT_CNT + DBL_INDIRECT_CNT)

#define PTRS_PER_SECTOR ((off_t) (BLOCK_SECTOR_SIZE / sizeof (block_sector_t)))
#define INODE_SPAN ((DIRECT_CNT                                              \
  + PTRS_PER_SECTOR * INDIRECT_CNT                        \
  + PTRS_PER_SECTOR * PTRS_PER_SECTOR * DBL_INDIRECT_CNT) \
  * BLOCK_SECTOR_SIZE)

#define THRESHOLD_DOUBLE_INDIRECT (DIRECT_CNT + INDIRECT_CNT * PTRS_PER_SECTOR)
/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t sectors[SECTOR_CNT]; /* Sectors. */
    off_t length;                       /* File size in bytes. */
    enum inode_type type;               /* FILE_INODE or DIR_INODE. */
    unsigned magic;                     /* Magic number. */
  };

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
    struct lock lock;                   /* Protects the inode. *///new

    /* Denying writes. */
    struct lock deny_write_lock;        /* Protects members below. *///new
    struct condition no_writers_cond;   /* Signaled when no writers. *///new
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    int writer_cnt;                     /* Number of writers. *///new
  };


/* List of open inodes, so that opening a single inode twice
returns the same `struct inode'. */
static struct list open_inodes;

/* Controls access to open_inodes list. */
static struct lock open_inodes_lock;

static void deallocate_inode(const struct inode *);

//use this for create...
static void extend_file_help (off_t length, struct inode_disk* data);

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  list_init (&open_inodes);
  lock_init(&open_inodes_lock);
}

static size_t inode_create_help(block_sector_t* table, size_t n, size_t capacity){
  size_t i = 0;
  static char zeros1[BLOCK_SECTOR_SIZE];
  size_t actually_filled = (n > capacity) ? capacity : n;
  while (i < actually_filled){
    free_map_allocate(1, &table[i]);
    cache_write(fs_device, table[i++], zeros1);
  }
  while (i<capacity){
    table[i++] = NULL_SECTOR;
  }
  return actually_filled;
}

bool inode_create (block_sector_t sector, off_t length){
  struct inode_disk *disk_inode = NULL;
  bool success = false;
  //printf("%d-%s: Creasting an inode of length %d\n", thread_current()->tid, __func__, length);
  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      disk_inode->length = 0;
      disk_inode->magic = INODE_MAGIC;
      int i = 0;
      disk_inode->sectors[DIRECT_CNT] = NULL_SECTOR;
      disk_inode->sectors[DIRECT_CNT + INDIRECT_CNT] = NULL_SECTOR;

      extend_file_help(length, disk_inode);
      
      //printf("%d-%s: Writing to sector %u\n", thread_current()->tid, __func__, sector);
      cache_write(fs_device, sector, disk_inode);
      //printf("%d-%s: Success\n", thread_current()->tid, __func__);
      success = true;
      free (disk_inode);
    }
  return success;
}
/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create1 (block_sector_t sector, off_t length)
{
  struct inode_disk *disk_inode = NULL;
  block_sector_t *indirect_block = NULL, *doubly_indirect_block = NULL;
  bool success = false;
  // printf("%d-%s: Creasting an inode of length %d\n", thread_current()->tid, __func__, length);
  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL)
    {
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      unsigned int i = 0;
      //printf("%d-%s: In direct mapping. Cur index %d\n", thread_current()->tid, __func__, i);
      i += inode_create_help(disk_inode->sectors, sectors - i, DIRECT_CNT);
      if (i < sectors){
        //need to add a level of indirection
        //printf("%d-%s: In indirect mapping. Cur index %d\n", thread_current()->tid, __func__, i);
        free_map_allocate(1, &disk_inode->sectors[DIRECT_CNT]);
        indirect_block = calloc(1, sizeof(block_sector_t) * PTRS_PER_SECTOR);
        i += inode_create_help(indirect_block, sectors - i, PTRS_PER_SECTOR);
        //printf("%d-%s: 1 Writing to sector %u\n", thread_current()->tid, __func__, disk_inode->sectors[DIRECT_CNT]);
        cache_write(fs_device, disk_inode->sectors[DIRECT_CNT], indirect_block);
        if(i < sectors){
          //printf("%d-%s: In double-indirect mapping. Cur index %d\n", thread_current()->tid, __func__, i);
          //allocate the double indirect.
          free_map_allocate(1, &disk_inode->sectors[DIRECT_CNT+INDIRECT_CNT]);
          int cur_indirect_index = 0;
          doubly_indirect_block = calloc(1, sizeof(block_sector_t) * PTRS_PER_SECTOR);
          
          while (i < sectors){
            free_map_allocate(1, &doubly_indirect_block[cur_indirect_index]);
            i += inode_create_help(indirect_block, sectors - i, PTRS_PER_SECTOR);
            //printf("%d-%s: 2 Writing to sector %u\n", thread_current()->tid, __func__, doubly_indirect_block[cur_indirect_index]);
            cache_write(fs_device, doubly_indirect_block[cur_indirect_index++], indirect_block);
          }
          while (cur_indirect_index < PTRS_PER_SECTOR){
            doubly_indirect_block[cur_indirect_index++] = NULL_SECTOR;
          }
          //printf("%d-%s: 3 Writing to sector %u\n", thread_current()->tid, __func__, disk_inode->sectors[DIRECT_CNT + INDIRECT_CNT]);
          cache_write(fs_device, disk_inode->sectors[DIRECT_CNT+INDIRECT_CNT], doubly_indirect_block);
        } else {
          disk_inode->sectors[DIRECT_CNT + INDIRECT_CNT] = NULL_SECTOR;
        }
      } else {
        disk_inode->sectors[DIRECT_CNT] = NULL_SECTOR;
        disk_inode->sectors[DIRECT_CNT+INDIRECT_CNT] = NULL_SECTOR;
      }
      //when everything is done
      //printf("%d-%s: Writing to sector %u\n", thread_current()->tid, __func__, sector);
      cache_write(fs_device, sector, disk_inode);
      //printf("%d-%s: Success\n", thread_current()->tid, __func__);
      if (indirect_block){
        free(indirect_block);
      }
      if (doubly_indirect_block){
        free(doubly_indirect_block);
      }
      success = true;
      free (disk_inode);
    }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *
inode_open (block_sector_t sector)
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
  
  return inode;
}

/* Reopens and returns INODE. */
struct inode *
inode_reopen (struct inode *inode)
{
  if (inode != NULL)
    {
      lock_acquire (&open_inodes_lock);
      inode->open_cnt++;
      lock_release (&open_inodes_lock);
    }
  return inode;
}

/* Returns the type of INODE. */
enum inode_type inode_get_type(const struct inode *inode){
  // read from cache ..
  struct inode_disk temp;
  //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, inode->sector);
  cache_read(fs_device, inode->sector, &temp);
  return temp.type;
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
          deallocate_inode(inode);
        }

      free (inode); 
    }
}


/* Deallocates SECTOR and anything it points to recursively.
LEVEL is 2 if SECTOR is doubly indirect,
or 1 if SECTOR is indirect,
or 0 if SECTOR is a data sector. */
static void
deallocate_recursive(block_sector_t sector, int level)
{
  if (sector == NULL_SECTOR){
    return;
  }
  if (level > 0){
    block_sector_t indirect_block[PTRS_PER_SECTOR];
    //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, sector);
    cache_read(fs_device, sector, &indirect_block);
    int i;
    for (i = 0; i < PTRS_PER_SECTOR; i++){
      deallocate_recursive(indirect_block[i], level - 1);
    }
  }
  free_map_release(sector, 1);
}

/* Deallocates the blocks allocated for INODE. */
static void
deallocate_inode(const struct inode *inode)
{
  struct inode_disk data;
  //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, inode->sector);
  cache_read(fs_device, inode->sector, &data);
  int i = 0;
  while (i < DIRECT_CNT){
    deallocate_recursive(data.sectors[i++], 0);
  }
  deallocate_recursive(data.sectors[i++], 1);
  deallocate_recursive(data.sectors[i++], 2);

  free_map_release(inode->sector, 1);
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void
inode_remove (struct inode *inode) 
{
  ASSERT (inode != NULL);
  inode->removed = true;
}


/* Translates SECTOR_IDX into a sequence of block indexes in
   OFFSETS and sets *OFFSET_CNT to the number of offsets. */
static void calculate_indices (off_t sector_idx, size_t offsets[], size_t *offset_cnt){
  if (sector_idx < DIRECT_CNT){
    /* Handle direct blocks. */
    offsets[0] = sector_idx;
    *offset_cnt = 0;
  } else if (sector_idx < THRESHOLD_DOUBLE_INDIRECT){
    /* Handle indirect blocks. */
    offsets[1] = (sector_idx - DIRECT_CNT) % PTRS_PER_SECTOR;
    //offsets[0] = DIRECT_CNT + (sector_idx - DIRECT_CNT) / PTRS_PER_SECTOR;
    offsets[0] = DIRECT_CNT;
    *offset_cnt = 1;
  } else {
    /* Handle doubly indirect blocks. */
    offsets[2] = (sector_idx - THRESHOLD_DOUBLE_INDIRECT) % PTRS_PER_SECTOR;
    offsets[1] = (sector_idx - THRESHOLD_DOUBLE_INDIRECT) / PTRS_PER_SECTOR;
    //offsets[0] = DIRECT_CNT + INDIRECT_CNT + (sector_idx - THRESHOLD_DOUBLE_INDIRECT) / PTRS_PER_SECTOR / PTRS_PER_SECTOR;
    offsets[0] = DIRECT_CNT + INDIRECT_CNT;
    *offset_cnt = 2;
  }
}

/* Retrieves the data block for the given byte OFFSET in INODE,
   setting *DATA_BLOCK to the block.
   Returns true if successful, false on failure.
   If ALLOCATE is false, then missing blocks will be successful
   with *DATA_BLOCk set to a null pointer.
   If ALLOCATE is true, then missing blocks will be allocated.
   The block returned will be locked, normally non-exclusively,
   but a newly allocated block will have an exclusive lock. */
static bool get_data_block (struct inode *inode, off_t offset, bool allocate, struct cache_block **data_block){
  size_t offsets[3], offset_cnt;
  // calculate_indices ...
  
  return false;
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

  struct inode_disk data;
  block_sector_t table[PTRS_PER_SECTOR];//MUST replace this once we fix cache
  //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, inode->sector);
  cache_read(fs_device, inode->sector, &data);
  while (size > 0) 
    {
      /* Disk sector to read, starting byte offset within sector. */
      block_sector_t sector_idx = offset / BLOCK_SECTOR_SIZE;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually copy out of this sector. */
      int chunk_size = size < min_left ? size : min_left;

      size_t offsets[3], offset_cnt;
      if (chunk_size <= 0)
        break;

      calculate_indices(sector_idx, offsets, &offset_cnt);
      if (offset_cnt > 0){
        //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, data.sectors[offsets[0]]);
        cache_read(fs_device, data.sectors[offsets[0]], &table);
        if (offset_cnt > 1){
          //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, table[offsets[1]]);
          cache_read(fs_device, table[offsets[1]], &table);
        }
      }
      block_sector_t index = (offset_cnt == 0) ? data.sectors[offsets[0]] : table[offsets[offset_cnt]];
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Read full sector directly into caller's buffer. */
          //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, index);
          cache_read (fs_device, index, buffer + bytes_read);
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
          //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, index);
          cache_read (fs_device, index, bounce);
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

static void extend_file_help (off_t length, struct inode_disk* data){
  block_sector_t *indirect_block = NULL, *doubly_indirect_block = NULL;
  block_sector_t* start = NULL;
  size_t capacity = 0;
  if (data->length >= length){
    return;
  }
  size_t sectors = bytes_to_sectors (length), old_sectors = bytes_to_sectors(data->length);
  //printf("%d-%s: Extending an inode of length %d to %d\n", thread_current()->tid, __func__, data->length, length);
  if (sectors > old_sectors){
    unsigned int i = old_sectors;
    int cur_indirect_index = 0;
    while (i < sectors){
      if (i < DIRECT_CNT){
        start = &data->sectors[i];
        capacity = DIRECT_CNT - i;
        //printf("%d-%s: In direct mapping. Cur index %d\n", thread_current()->tid, __func__, i);
        i += inode_create_help(start, sectors - i, capacity);
        if (i == sectors){
          //no indirect
          data->sectors[DIRECT_CNT] = NULL_SECTOR;
          data->sectors[DIRECT_CNT + INDIRECT_CNT] = NULL_SECTOR;
        }
      } else if (i < THRESHOLD_DOUBLE_INDIRECT){
        if (!indirect_block){
          indirect_block = calloc(1, sizeof(block_sector_t) * PTRS_PER_SECTOR);
        }
        if (data->sectors[DIRECT_CNT] == NULL_SECTOR){
          //before this, all direct, so we need to allocate
          free_map_allocate(1, &data->sectors[DIRECT_CNT]);
        } else {
          cache_read(fs_device, data->sectors[DIRECT_CNT], indirect_block);  
        }
        start = &indirect_block[(i - DIRECT_CNT)];
        capacity = PTRS_PER_SECTOR - (i - DIRECT_CNT);
        //printf("%d-%s: In indirect mapping. Cur index %d\n", thread_current()->tid, __func__, i);
        i += inode_create_help(start, sectors - i, capacity);
        if (i == sectors){
          //no doubly indirect
          data->sectors[DIRECT_CNT + INDIRECT_CNT] = NULL_SECTOR;
        }
        //printf("%d-%s: 1 Writing to sector %u\n", thread_current()->tid, __func__, data->sectors[DIRECT_CNT]);
        cache_write(fs_device, data->sectors[DIRECT_CNT], indirect_block);
      } else {
        //printf("%d-%s: In double-indirect mapping. Cur index %d\n", thread_current()->tid, __func__, i);
        if (!doubly_indirect_block){
          doubly_indirect_block = calloc(1, sizeof(block_sector_t) * PTRS_PER_SECTOR);
        }
        if (data->sectors[DIRECT_CNT + INDIRECT_CNT] == NULL_SECTOR){
          free_map_allocate(1, &data->sectors[DIRECT_CNT + INDIRECT_CNT]);
          int j = 0;
          while (j < PTRS_PER_SECTOR){
            doubly_indirect_block[j++] = NULL_SECTOR;
          }
        } else {
          cache_read(fs_device, data->sectors[DIRECT_CNT + INDIRECT_CNT], doubly_indirect_block);
        }
        cur_indirect_index = (i - THRESHOLD_DOUBLE_INDIRECT) / PTRS_PER_SECTOR;
        while (i < sectors){
          if (!indirect_block){
            indirect_block = calloc(1, sizeof(block_sector_t) * PTRS_PER_SECTOR);
          } 
          if (doubly_indirect_block[cur_indirect_index] == NULL_SECTOR){
            free_map_allocate(1, &doubly_indirect_block[cur_indirect_index]);
            //printf("%d-%s: Allocated sector %u to cur index %u\n", thread_current()->tid, __func__, doubly_indirect_block[cur_indirect_index], cur_indirect_index);
          } else {
            cache_read(fs_device, doubly_indirect_block[cur_indirect_index], indirect_block);
          }
          start = &indirect_block[(i - THRESHOLD_DOUBLE_INDIRECT) % PTRS_PER_SECTOR];
          capacity = PTRS_PER_SECTOR - ((i - THRESHOLD_DOUBLE_INDIRECT) % PTRS_PER_SECTOR);
          i += inode_create_help(start, sectors - i, capacity);
          //printf("%d-%s: 2 Writing to sector %u\n", thread_current()->tid, __func__, doubly_indirect_block[cur_indirect_index]);
          cache_write(fs_device, doubly_indirect_block[cur_indirect_index++], indirect_block);
        }
        while (cur_indirect_index < PTRS_PER_SECTOR){
          doubly_indirect_block[cur_indirect_index++] = NULL_SECTOR;
        }
        //printf("%d-%s: 3 Writing to sector %u\n", thread_current()->tid, __func__, data->sectors[DIRECT_CNT + INDIRECT_CNT]);
        cache_write(fs_device, data->sectors[DIRECT_CNT + INDIRECT_CNT], doubly_indirect_block);
      }
    }
  }

  if (indirect_block){
    free(indirect_block);
  }
  if (doubly_indirect_block){
    free(doubly_indirect_block);
  }
  data->length = length;
  return;
}

/* Extends INODE to be at least LENGTH bytes long. */
static void extend_file (struct inode *inode, off_t length){
  struct inode_disk data;
  cache_read (fs_device, inode->sector, &data);
  extend_file_help(length, &data);
  cache_write(fs_device, inode->sector, &data);
  return;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t
inode_write_at (struct inode *inode, const void *buffer_, off_t size,
                off_t offset) 
{
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  // if (inode_length(inode) < offset + size){
  //   // PANIC("BAD");
  //   extend_file(inode, offset + size);
  // }
  
  struct inode_disk data;
  block_sector_t table[PTRS_PER_SECTOR];//MUST replace this once we fix cache
  //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, inode->sector);
  cache_read(fs_device, inode->sector, &data);
  
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = offset / BLOCK_SECTOR_SIZE;
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      size_t offsets[3], offset_cnt;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;
      
      calculate_indices(sector_idx, offsets, &offset_cnt);
      if (offset_cnt > 0){
        //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, data.sectors[offsets[0]]);
        cache_read(fs_device, data.sectors[offsets[0]], &table);
        if (offset_cnt > 1){
          //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, table[offsets[1]]);
          cache_read(fs_device, table[offsets[1]], &table);
        }
      }
      block_sector_t index = (offset_cnt == 0) ? data.sectors[offsets[0]] : table[offsets[offset_cnt]];
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          /* Write full sector directly to disk. */
          cache_write (fs_device, index, buffer + bytes_written);
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
          if (sector_ofs > 0 || chunk_size < sector_left) {
            //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, index);
            cache_read (fs_device, index, bounce);
          }
          else{
            memset (bounce, 0, BLOCK_SECTOR_SIZE);
          }
            
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          cache_write (fs_device, index, bounce);
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
  ASSERT (inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t
inode_length (const struct inode *inode)
{
  struct inode_disk temp;
  //printf("%d-%s: Reading from sector %u\n", thread_current()->tid, __func__, inode->sector);
  cache_read(fs_device, inode->sector, &temp);
  return temp.length;
  // return inode->data.length;
}

/* Returns the number of openers. */
int
inode_open_cnt(const struct inode *inode)
{
  int open_cnt;

  lock_acquire(&open_inodes_lock);
  open_cnt = inode->open_cnt;
  lock_release(&open_inodes_lock);

  return open_cnt;
}

/* Locks INODE. */
void
inode_lock(struct inode *inode)
{
  lock_acquire(&inode->lock);
}

/* Releases INODE's lock. */
void
inode_unlock(struct inode *inode)
{
  lock_release(&inode->lock);
}
