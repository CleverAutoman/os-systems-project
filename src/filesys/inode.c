#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "filesys/buffer-cache.h"
#include "threads/malloc.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44
#define DIRECT_INDEX_BOUND 12
#define INDIRECT_INDEX_BOUND (DIRECT_INDEX_BOUND + 128) // 128 = sizeof(block) / sizeof(uint32_t)
#define DOUBLY_INDEX_BOUND                                                                         \
  (INDIRECT_INDEX_BOUND + 128 * 128) // 128 = sizeof(block) / sizeof(uint32_t)

#define DIRECT_INDEX_BOUND_BYTE (DIRECT_INDEX_BOUND * 512)
#define INDIRECT_INDEX_BOUND_BYTE (INDIRECT_INDEX_BOUND * 512)
#define DOUBLY_INDEX_BOUND_BYTE (DOUBLY_INDEX_BOUND * 512)
#define NO_SECTOR 0xFFFFFFFF

#define DIRECT_INDEX_BOUND 12
#define INDIRECT_INDEX_BOUND (DIRECT_INDEX_BOUND + 128) // 128 = sizeof(block) / sizeof(uint32_t)
#define DOUBLY_INDEX_BOUND                                                                         \
  (INDIRECT_INDEX_BOUND + 128 * 128) // 128 = sizeof(block) / sizeof(uint32_t)

#define DIRECT_INDEX_BOUND_BYTE (DIRECT_INDEX_BOUND * 512)
#define INDIRECT_INDEX_BOUND_BYTE (INDIRECT_INDEX_BOUND * 512)
#define DOUBLY_INDEX_BOUND_BYTE (DOUBLY_INDEX_BOUND * 512)

/* 
    0 -> 11: direct index;
    12 -> 139: indirect index;
    140 -> : doubly-indirect index;
*/

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */

/* 
    0 -> 11: direct index;
    12 -> 139: indirect index;
    140 -> : doubly-indirect index;
  */
struct inode_disk {
  block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  block_sector_t direct[DIRECT_INDEX_BOUND];
  block_sector_t indirect;
  block_sector_t doubly_indirect;
  uint32_t unused[111]; /* Not used. 125 - 2 - 12 */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/* Allocate free block from free_map and filled with zeroes 
   will be called when calling 0XFFFFFFFF sector_id 
*/
static bool allocate_free_index_block(block_sector_t* sector_id) {
  bool success = false;
  /* Allocate block for new data */
  if (free_map_allocate(1, sector_id)) {
    uint32_t zeros[BLOCK_SECTOR_SIZE / 4];
    /* Write 0xFFFFFFFF into zeros */
    for (int i = 0; i < BLOCK_SECTOR_SIZE / 4; i++) {
      zeros[i] = NO_SECTOR;
    }
    // block_write(fs_device, *sector_id, zeros);

    struct cache_entry* ce1 = acquire_entry(fs_device, *sector_id);
    write_entry(ce1, zeros);
    release_entry(ce1);
    success = true;
  }
  return success;
}

static bool allocate_free_data_block(block_sector_t* sector_id) {
  bool success = false;
  /* Allocate block for new data */
  if (free_map_allocate(1, sector_id)) {
    uint32_t zeros[BLOCK_SECTOR_SIZE / 4];
    memset(zeros, 0, BLOCK_SECTOR_SIZE);

    // block_write(fs_device, *sector_id, zeros);
    struct cache_entry* ce1 = acquire_entry(fs_device, *sector_id);
    write_entry(ce1, zeros);
    release_entry(ce1);

    success = true;
  }
  return success;
}

static void release_free_block(block_sector_t sector) { free_map_release(sector, 1); }

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
    POS. 
   Offset Byte -> index
*/
static block_sector_t byte_to_sector(const struct inode* inode, off_t pos, int* sector_ofs,
                                     bool is_read) {
  // printf("pos=%lld len=%lld\n", (long long)pos, (long long)inode->data.length);
  /* TODO: Get the bst with doubly-index */
  ASSERT(inode != NULL);
  block_sector_t b_id = NO_SECTOR;
  struct cache_entry* ce;

  if (pos < DIRECT_INDEX_BOUND_BYTE) {
    /* Pos is in direct index range */
    uint8_t db_id = pos / BLOCK_SECTOR_SIZE;
    *sector_ofs = pos % BLOCK_SECTOR_SIZE;

    /* Need to update direct index if write */
    b_id = inode->data.direct[db_id];
    // printf("direct b_id == %d\n", b_id);
    if (b_id == NO_SECTOR) {
      if (is_read) {
        return NO_SECTOR;
      } else {
        allocate_free_data_block(&inode->data.direct[db_id]);
        // printf("allocate id: %u, val: %u\n", db_id, inode->data.direct[db_id]);

        struct cache_entry* ce1 = acquire_entry(fs_device, inode->sector);
        ASSERT(sizeof(inode->data) == BLOCK_SECTOR_SIZE);
        write_entry(ce1, &inode->data);
        release_entry(ce1);
        // block_write(fs_device, inode->sector, &inode->data);
        b_id = inode->data.direct[db_id];
      }
    }
    // printf("bid: %d, dbid: %d, from direct llist\n", b_id, db_id);

  } else if (pos < INDIRECT_INDEX_BOUND_BYTE) {
    /* Pos is in indirect index range */

    /* 1. If indirect is no_sector && is_read, return secotr not found 
          else create new indirect block 
    */
    // block_sector_t* bounce = calloc(BLOCK_SECTOR_SIZE, 1);
    uint32_t* bounce = NULL;
    if (inode->data.indirect == NO_SECTOR) {
      if (is_read) {
        return NO_SECTOR;
      } else {
        // printf("ready to allocate1\n");
        allocate_free_index_block(&inode->data.indirect);
        // printf("allocate indirect: %u\n", inode->data.indirect);

        struct cache_entry* ce1 = acquire_entry(fs_device, inode->sector);
        ASSERT(sizeof(inode->data) == BLOCK_SECTOR_SIZE);
        write_entry(ce1, &inode->data);
        release_entry(ce1);
        // block_write(fs_device, inode->sector, &inode->data);
      }
    }
    // printf("acquire1\n");
    struct cache_entry* ce2 = acquire_entry(fs_device, inode->data.indirect);
    bounce = ce2->data;
    // block_read(fs_device, inode->data.indirect, bounce); // bounce is indirect table
    pos -= DIRECT_INDEX_BOUND_BYTE;

    /* 2. Find id of actual data block */
    /* Only write cache if ce is invalid */

    // ce = cache_acquire(inode->data.indirect);
    // cache_read_ptr(ce);
    // bounce = ce->data;

    uint8_t db_id = pos / BLOCK_SECTOR_SIZE;
    *sector_ofs = pos % BLOCK_SECTOR_SIZE;

    /* 3. If b_id is not allocated & is not read, create new one */
    if (bounce[db_id] == NO_SECTOR) {
      if (is_read) {
        // printf("release1\n");
        release_entry(ce2);
        return NO_SECTOR;
      } else {
        /* Create new data block and link to direct block */

        /* Allocating free data block need to release lock, 
          so we need to do double-check before assigning new value */
        block_sector_t prev = bounce[db_id];
        release_entry(ce2);
        /* Create new data block first */
        block_sector_t new_id;
        allocate_free_data_block(&new_id);
        /* Re-acquire cache entry with same indirect sector */
        ce2 = acquire_entry(fs_device, inode->data.indirect);
        bounce = ce2->data;
        if (prev != bounce[db_id]) { // already occupied by other thread
          /* Free this data block */
          b_id = bounce[db_id];
          release_entry(ce2);
          /* Also no lock held before releasing data block */
          release_free_block(new_id);
        } else {
          b_id = bounce[db_id] = new_id;
          write_entry(ce2, bounce);
          release_entry(ce2);
        }
        // printf("allocate id: %u, val: %u\n", db_id, bounce[db_id]);
        // block_write(fs_device, inode->data.indirect, bounce);
      }
    } else {
      b_id = bounce[db_id];
      release_entry(ce2);
    }

    // cache_release(ce);
    // if (bounce) {
    //   free(bounce);
    // }
    // printf("bid: %d, dbid: %d, from indirect llist\n", b_id, db_id);
  } else if (pos < DOUBLY_INDEX_BOUND_BYTE) {
    /* Pos is in doubly indirect index range */

    /* 1. If indirect is no_sector && is_read, return secotr not found 
          else create new indirect block 
    */
    // block_sector_t* bounce = calloc(BLOCK_SECTOR_SIZE, 1);
    uint32_t* bounce = NULL;
    if (inode->data.doubly_indirect == NO_SECTOR) {
      if (is_read) {
        return NO_SECTOR;
      } else {
        allocate_free_index_block(&inode->data.doubly_indirect);
        // block_write(fs_device, inode->sector, &inode->data);
        struct cache_entry* ce1 = acquire_entry(fs_device, inode->sector);
        ASSERT(sizeof(inode->data) == BLOCK_SECTOR_SIZE);
        write_entry(ce1, &inode->data);
        release_entry(ce1);
      }
    }
    // block_read(fs_device, inode->data.doubly_indirect, bounce); // first block
    // printf("acquire2\n");
    struct cache_entry* ce2 = acquire_entry(fs_device, inode->data.doubly_indirect);
    bounce = ce2->data;
    pos -= INDIRECT_INDEX_BOUND_BYTE;

    uint32_t i1_id = pos / (BLOCK_SECTOR_SIZE * 128);
    uint32_t i1_off = pos % (BLOCK_SECTOR_SIZE * 128);

    /* 3. Find id of indirect index and create if needed */
    block_sector_t i2_id = bounce[i1_id];
    /* If doubly_indirect is no_sector, allocate new one */
    if (i2_id == NO_SECTOR) {
      if (is_read) {
        // printf("release3\n");
        release_entry(ce2);
        return NO_SECTOR;

      } else {
        block_sector_t prev = bounce[i1_id];
        release_entry(ce2);

        block_sector_t new_id;
        /* Create new data block and link to direct block */
        allocate_free_index_block(&new_id);

        ce2 = acquire_entry(fs_device, inode->data.doubly_indirect);
        bounce = ce2->data;
        if (prev != bounce[i1_id]) {
          i2_id = bounce[i1_id];
          release_entry(ce2);
          /* No lock held when releasing */
          release_free_block(new_id);
        } else {
          i2_id = bounce[i1_id] = new_id;
          write_entry(ce2, bounce);
          release_entry(ce2);
        }
        // block_write(fs_device, inode->data.doubly_indirect, bounce); // write back first block
      }
    } else {
      release_entry(ce2);
    }

    // printf("acquire3\n");
    struct cache_entry* ce3 = acquire_entry(fs_device, i2_id);
    bounce = ce3->data;
    // block_read(fs_device, i2_id, bounce); // second block

    // cache_release(ce);

    /* 4. Find id of actual data block and create if needed */
    uint32_t db_id = i1_off / BLOCK_SECTOR_SIZE;
    *sector_ofs = i1_off % BLOCK_SECTOR_SIZE;
    if (bounce[db_id] == NO_SECTOR) {
      if (is_read) {
        // printf("release6\n");
        release_entry(ce3);
        return NO_SECTOR;
      } else {
        /* Create new data block and link to direct block */
        block_sector_t prev = bounce[db_id];
        release_entry(ce3);

        block_sector_t new_id;
        allocate_free_data_block(&new_id);

        ce3 = acquire_entry(fs_device, i2_id);
        bounce = ce3->data;
        if (prev != bounce[db_id]) {
          b_id = bounce[db_id];
          release_entry(ce3);
          /* No lock held when releasing */
          release_free_block(new_id);
        } else {
          b_id = bounce[db_id] = new_id;
          write_entry(ce3, bounce);
          release_entry(ce3);
        }

        // block_write(fs_device, i2_id, bounce);
        // cache_write(ce, bounce, BLOCK_SECTOR_SIZE);
      }
    } else {
      b_id = bounce[db_id];
      release_entry(ce3);
    }

    // printf("bid: %u, dbid: %d, from doulbuy llist\n", b_id, db_id);
    // cache_release(ce);

    // if (bounce) {
    //   free(bounce);
    // }
  } else {
    printf("file size limit reached\n");
  }
  // printf("reached 4\n");
  // if (!is_read) {
  //   // struct cache_entry* inode_ce = cache_acquire(inode->sector);
  //   // cache_write(inode_ce, &inode->data, BLOCK_SECTOR_SIZE);
  //   // cache_release(inode_ce);
  //   block_write(fs_device, inode->sector, &inode->data);
  // }

  // printf("reached 5\n");
  return b_id;
  // size_t tmp = DIV_ROUND_UP(pos, BLOCK_SECTOR_SIZE);
  // if (pos < inode->data.length)
  //   return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  // else
  //   return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) { list_init(&open_inodes); }

/* Only support one block at first, 
   grow dynamicly when space is limited */
bool inode_create_with_zero_first(block_sector_t sector, off_t size) {
  // printf("enterd create\n");
  /* TODO: Initialize inode with only 1 block */
  struct inode_disk* disk_inode = NULL;
  bool success = false;

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    disk_inode->length = size;
    disk_inode->magic = INODE_MAGIC;

    /* Initialize direct arr*/
    for (int i = 0; i < DIRECT_INDEX_BOUND; i++) {
      disk_inode->direct[i] = NO_SECTOR;
    }

    /* Initialize indirect and doubly indirect */
    disk_inode->doubly_indirect = disk_inode->indirect = NO_SECTOR;

    /* Only write one free inode space to block */

    // block_write(fs_device, sector, disk_inode);
    struct cache_entry* ce1 = acquire_entry(fs_device, sector);
    write_entry(ce1, disk_inode);
    release_entry(ce1);

    success = true;
    free(disk_inode);
  }
  // printf("end create\n");
  return success;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. 
   
   if length == 0: only create block with metadata;
   if length == Block_Size: create one block;
   else: create normal block
   */
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk* disk_inode = NULL;
  struct cache_entry* ce = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    if (free_map_allocate(sectors, &disk_inode->start)) {
      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;

        for (i = 0; i < sectors; i++) {
          /* Write data */
          // block_write(fs_device, disk_inode->start + i, zeros);

          struct cache_entry* ce1 = acquire_entry(fs_device, disk_inode->start + i);
          write_entry(ce1, zeros);
          release_entry(ce1);

          /* Write idnex */
          disk_inode->direct[i] = disk_inode->start + i;
        }

        disk_inode->indirect = NO_SECTOR;
        disk_inode->doubly_indirect = NO_SECTOR;

        // block_write(fs_device, sector, disk_inode);

        struct cache_entry* ce1 = acquire_entry(fs_device, sector);
        write_entry(ce1, disk_inode);
        release_entry(ce1);
      }
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode* inode_open(block_sector_t sector) {
  // printf("inode open with sector: %u\n", sector);
  struct list_elem* e;
  struct inode* inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  // block_read(fs_device, inode->sector, &inode->data);
  struct cache_entry* ce1 = acquire_entry(fs_device, inode->sector);
  read_entry(ce1, &inode->data);
  release_entry(ce1);
  return inode;
}

/* Reopens and returns INODE. */
struct inode* inode_reopen(struct inode* inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode* inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode* inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      free_map_release(inode->data.start, bytes_to_sectors(inode->data.length));
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode* inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
  // printf("entering inode_read_At size = %lu, offset = %lu\n", size, offset);
  uint8_t* buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t* bounce = NULL;
  // uint8_t* bounce = malloc(BLOCK_SECTOR_SIZE);
  // if (!bounce) {
  //   return -1;
  // }
  // struct cache_entry* ce = NULL;
  uint8_t* p = NULL;

  while (size > 0) {
    // printf("keep reading with size = %lu\n", size);
    /* Disk sector to read, starting byte offset within sector. */
    int sector_ofs;
    block_sector_t sector_idx = byte_to_sector(inode, offset, &sector_ofs, true);
    // printf("sector idx == %lu\n", sector_idx);
    /* Break if sector_idx == NO_SECTOR */

    // printf("keep reading with size = %lu, and id = %lu\n", size, sector_idx);
    /* Number of bytes to actually copy out of this sector: Block Size - sector_ofs */
    int chunk_size = BLOCK_SECTOR_SIZE - sector_ofs;
    chunk_size = chunk_size < size ? chunk_size : size;

    if (sector_idx == NO_SECTOR) { // Copy zeroes if found NO_SECTOR sign
      // if (offset >= inode->data.length)
      //   break;
      memset(buffer + bytes_read, 0, chunk_size);
      bytes_read += chunk_size;
      break;

    } else if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      // block_read(fs_device, sector_idx, bounce);
      struct cache_entry* ce = acquire_entry(fs_device, sector_idx);
      bounce = ce->data;

      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
      release_entry(ce);
    } else {

      // block_read(fs_device, sector_idx, bounce);
      struct cache_entry* ce = acquire_entry(fs_device, sector_idx);
      bounce = ce->data;

      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
      release_entry(ce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  // if (bounce)
  //   free(bounce);

  // printf("bytes_read: %lu\n", bytes_read);
  return bytes_read;
}

// off_t inode_read_at(struct inode* inode, void* buffer_, off_t size, off_t offset) {
//   uint8_t* buffer = buffer_;
//   off_t bytes_read = 0;
//   uint8_t* bounce = NULL;

//   while (size > 0) {
//     /* Disk sector to read, starting byte offset within sector. */
//     block_sector_t sector_idx = byte_to_sector(inode, offset);
//     int sector_ofs = offset % BLOCK_SECTOR_SIZE;

//     /* Bytes left in inode, bytes left in sector, lesser of the two. */
//     off_t inode_left = inode_length(inode) - offset;
//     int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
//     int min_left = inode_left < sector_left ? inode_left : sector_left;

//     /* Number of bytes to actually copy out of this sector. */
//     int chunk_size = size < min_left ? size : min_left;
//     if (chunk_size <= 0)
//       break;

//     if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
//       /* Read full sector directly into caller's buffer. */
//       block_read(fs_device, sector_idx, buffer + bytes_read);
//     } else {
//       /* Read sector into bounce buffer, then partially copy
//              into caller's buffer. */
//       if (bounce == NULL) {
//         bounce = malloc(BLOCK_SECTOR_SIZE);
//         if (bounce == NULL)
//           break;
//       }
//       block_read(fs_device, sector_idx, bounce);
//       memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
//     }

//     /* Advance. */
//     size -= chunk_size;
//     offset += chunk_size;
//     bytes_read += chunk_size;
//   }
//   free(bounce);

//   return bytes_read;
// }

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode* inode, const void* buffer_, off_t size, off_t offset) {
  const uint8_t* buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t* bounce = NULL;
  // uint8_t* bounce = malloc(BLOCK_SECTOR_SIZE);
  // if (bounce == NULL)
  //   return -1;
  // struct cache_entry* ce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  // printf("size before while: %lu\n", size);

  // printf("size before while: %lu\n", size);

  while (size > 0) {
    // printf("keep writing file\n");
    /* Sector to write, starting byte offset within sector. */
    int sector_ofs;
    block_sector_t sector_idx = byte_to_sector(inode, offset, &sector_ofs, false);

    if (sector_idx == NO_SECTOR) {
      break;
    }
    // printf("byte to sectot success with sector_idx: %u\n", sector_idx);
    int chunk_size = BLOCK_SECTOR_SIZE - sector_ofs;
    chunk_size = chunk_size < size ? chunk_size : size;

    /* Number of bytes to actually write into this sector. */

    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      struct cache_entry* ce = acquire_entry(fs_device, sector_idx);
      // printf("acquire success1\n");
      write_entry(ce, buffer + bytes_written);

      release_entry(ce);
      // printf("release success1\n");

      // block_write(fs_device, sector_idx, buffer + bytes_written);
    } else {
      /* If the sector contains data before or after the chunk
               we're writing, then we need to read in the sector
               first.  Otherwise we start with a sector of all zeros. */
      struct cache_entry* ce = acquire_entry(fs_device, sector_idx);
      // printf("acquire success2\n");
      if (sector_ofs > 0 || chunk_size < BLOCK_SECTOR_SIZE) {
        bounce = ce->data;
        memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
        write_entry(ce, bounce);
        release_entry(ce);
        // printf("release success2\n");
      } else {
        bounce = calloc(BLOCK_SECTOR_SIZE, 1);
        // memset(bounce, 0, BLOCK_SECTOR_SIZE);
        memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
        write_entry(ce, bounce);
        release_entry(ce);
        // printf("release success2.\n");
        free(bounce);
      }

      // memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      // block_write(fs_device, sector_idx, bounce);
    }
    // }

    /* Advance. */
    off_t end_pos = offset + chunk_size; // offset 还是本轮写前的值
    if (end_pos > inode->data.length)
      inode->data.length = end_pos;
    // printf("file inode addr: %p,  node size after: %lu, total size left: %lu\n", inode, inode->data.length, size);
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }

  // if (bounce)
  //   free(bounce);
  // block_write(fs_device, inode->sector, &inode->data);
  struct cache_entry* ce1 = acquire_entry(fs_device, inode->sector);
  ASSERT(sizeof(inode->data) == BLOCK_SECTOR_SIZE);
  write_entry(ce1, &inode->data);
  release_entry(ce1);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener. */
void inode_deny_write(struct inode* inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode* inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode* inode) { return inode->data.length; }
