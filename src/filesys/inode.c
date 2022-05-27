#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"

//#include "filesys/buffer_cache.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// BLOCK_SECTOR_SIZE : 512
#define BUFFER_CACHE_ENTRY_NB 64;

////////     Project 4.1 Buffer Cache    ////////
// data를 제외한, cache 스스로에 대한 정보 buffer head table
static struct buffer_head bh_table[64];

// 실제 data가 저장되는 buffer입니다.
static uint8_t *p_buffer_cache[64];

// victim entry 선정 시 clock 알고리즘을 위한 변수
static struct buffer_head *clock_hand; 

// buffer_head 배열인 bh_table에 새로운 값을 추가하거나 뺄 때 중간과정을 보이지 않게 하는 lock
static struct lock buffer_head_lock;


////////////////////////////////////////////////

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
    block_sector_t start;               /* First data sector. */
    off_t length;                       /* File size in bytes. */
    unsigned magic;                     /* Magic number. */
    uint32_t unused[125];               /* Not used. */
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
    // inode가 저장된 block device sector의 index, block.h에 선언됨
    block_sector_t sector;              /* Sector number of disk location. */
    // opener의 갯수
    int open_cnt;                       /* Number of openers. */
    // file의 삭제 여부
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    // disk_inode data
    struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t
byte_to_sector (const struct inode *inode, off_t pos) 
{
  ASSERT (inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

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
inode_create (block_sector_t sector, off_t length)
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
      size_t sectors = bytes_to_sectors (length);
      disk_inode->length = length;
      disk_inode->magic = INODE_MAGIC;
      if (free_map_allocate (sectors, &disk_inode->start)) 
        {
          block_write (fs_device, sector, disk_inode);
          if (sectors > 0) 
            {
              static char zeros[BLOCK_SECTOR_SIZE];
              size_t i;
              
              for (i = 0; i < sectors; i++)
                //bc_write(sector_idx, buffer, bytes_read, chunk_size, sector_ofs); 
                block_write (fs_device, disk_inode->start + i, zeros);
            }
          success = true; 
        } 
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
  block_read (fs_device, inode->sector, &inode->data);
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
          free_map_release (inode->data.start,
                            bytes_to_sectors (inode->data.length)); 
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
          /* Read full sector directly into caller's buffer.*/ 
          //bc_read(sector_idx, buffer, bytes_read, chunk_size, sector_ofs);
          block_read (fs_device, sector_idx, buffer + bytes_read);
          
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
          block_read (fs_device, sector_idx, bounce);
          //bc_read(fs_device, sector_idx, bounce, chunk_size, sector_ofs);
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

  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      // sector 내에서 시작되는 offset idx
      block_sector_t sector_idx = byte_to_sector (inode, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      
      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_length (inode) - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      // 파일 크기의 offset과, sector의 남은 크기 둘 중 더 작은 거 선택하기
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      // 실제 쓰고자 하는 size하고 최소 크기와 비교해서 chunk_size
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      // full sector 작성할거니?
      if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE)
        {
          // Write full sector directly to disk.
          bc_write(sector_idx, (void*)buffer, bytes_written, chunk_size, sector_ofs);
          //block_write (fs_device, sector_idx, buffer + bytes_written);
        }
      // 아니요
      else 
        {
          if (bounce == NULL) 
            {
              // Bounce buffer 할당
              bounce = malloc (BLOCK_SECTOR_SIZE);
              if (bounce == NULL)
                break;
            }

          /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */

          /* 만약 sector 공간에 우리가 쓰고 있는 chunk의 전후에 이미 data가 있다면, 
             우리는 먼저 sector를 읽을 필요가 있다. 
             그렇지 않으면 모든 0의 sector로 시작한다. */
          // disk에서 bounce buffer로 block read하기
          if (sector_ofs > 0 || chunk_size < sector_left)
            //bc_read(fs_device, sector_idx, bounce, chunk_size, sector_ofs);
            block_read (fs_device, sector_idx, bounce); // bounce에 담아놓는다.
          else
            memset (bounce, 0, BLOCK_SECTOR_SIZE); // 아니면 아예 0으로 세팅
          // buffer에서 bounce buffer로 Partial Write하기
          memcpy (bounce + sector_ofs, buffer + bytes_written, chunk_size);
          // Bounce buffer에서 fs_device로 block write하기
          //bc_write(sector_idx, (void*)bounce, bytes_written, chunk_size, sector_ofs);
          block_write (fs_device, sector_idx, bounce);
        }
      
      // 남은 write size 계산
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }
  // bounce buffer 해지
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
  return inode->data.length;
}



////////////// Project 4.1 /////////////////////

/* 1. Buffer cache를 초기화하는 함수 */
void bc_init (void){
    
    for(int i=0; i<64; i++){
        // 1. allocation buffer cache in Memory
        p_buffer_cache[i] = malloc(BLOCK_SECTOR_SIZE);
        
        // 전역변수 buffer_head 자료구조 초기화    
        memset(&bh_table[i],0,sizeof(struct buffer_head));

        lock_init(&bh_table[i].lock);

        // p_buffer_cache가 buffer cache 영역 pointing
        bh_table[i].data = p_buffer_cache;
    }

    clock_hand = bh_table; // 첫번째 element
    lock_init(&buffer_head_lock);
}

/* 2. 모든 dirty entry flush 및 buffer cache 해지 */
// buffer cache 시스템 종료
void bc_term (void){
    // bc_flush_all_entries 함수를 호출하여 모든 buffer cache entry를 disk로 flush
    bc_flush_all_entries();

    // buffer cache로 영역 할당 해제
    for(int i=0; i<64; i++){
        free(p_buffer_cache[i]);
    }
}

/* 2.1 buffer cache를 순회하면서 dirty bit가 true인 entry를 모두 disk로 flush */
void bc_flush_all_entries (void){
    for(int i=0;i<64;i++){
        // 아직 쓰지 않은 모든 dirty cache를 써서 정리
        lock_acquire(&bh_table[i].lock);
        bc_flush_entry(&bh_table[i]);
        lock_release(&bh_table[i].lock);
    }
}

/* 인자로 주어진 entry의 dirty bit를 false로 setting하면서 해당 내역을 disk로 flush */
void bc_flush_entry (struct buffer_head * p_flush_entry){
    /* 인자로 전달받은 buffer cache entry의 data를 disk로 flush */
    if(p_flush_entry->dirty && !p_flush_entry->used){
        // buffer의 cache data (p_flush_entry의 data) -> disk로
        block_write(fs_device, p_flush_entry->sector, p_flush_entry->data);
        /* buffer_head의 dirty 값 update */
        p_flush_entry->dirty = false;
    } 
}





/* Buffer cache에서 요청 받은 buffer frame을 읽어와서 user buffer에 저장 */
// buffer를 이용하여 읽기 작업 수행
bool bc_read(block_sector_t sector_idx, void * buffer, off_t bytes_read, int chunk_size, int sector_ofs){
    
    // buffer_head를 검색
    // entry가 존재하는 지 검색
    struct buffer_head *bh_entry = bc_lookup(sector_idx);
    
    /* 읽을 데이터가 buffer cache에 없으면, disk에서 읽어 buffer cache에 캐싱 */
    if(!bh_entry) {
        if(p_buffer_cache)
        bh_entry = bc_select_victim();
        
        
        // bh_entry에 캐시에서 제거할 섹터가 있다.
        bc_flush_entry(bh_entry);
        
        // 초기화해주기
        bh_entry->dirty = false; // 깨끗하게
        bh_entry->used  = false; // 사용안한걸로
        bh_entry->sector = sector_idx; // 현재 sector_idx
        
        // address를 지정하였으므로 lock을 해제.
        lock_release(&buffer_head_lock);

        // disk에서 buffer cache로 data를 block_read
        block_read(fs_device, sector_idx, bh_entry->data); // disk block data -> buffer cache
    }
    
    /* To Do 3 : Modify read*/
    // buffer cache data를 user buffer에 복사
    bh_entry->accessed = true; // buffer_head의 clock_bit를 setting
    memcpy(buffer+ bytes_read, bh_entry->data, chunk_size); // buffer cache의 data를 buffer로 읽기
    
    // update buffer_head
   
    lock_release(&bh_entry->lock);
    return true;
}

/* Buffer cache에서 buffer frame에 요청 받은 data를 기록 */
bool bc_write (block_sector_t sector_idx, void * buffer, off_t bytes_written, int chunk_size, int sector_ofs){

    // buffer_head를 검색
    
    struct buffer_head* bh_entry = bc_lookup(sector_idx);
    if(!bh_entry){
        // 여기에 도달하면 buffer에 sector가 없다.
        bh_entry = bc_select_victim();
        
        bc_flush_entry(bh_entry);
        bh_entry->used = false;
        bh_entry->sector = sector_idx;

        // sector_idx를 지정하였으므로 해당 cache의 lock을 해제
        lock_release(&buffer_head_lock);

        // disk에서 buffer cache로 data를 block_read
        block_read(fs_device, sector_idx, bh_entry->data); // disk block data -> buffer cache
    }
    
    /*update buffer head*/
    bh_entry->accessed = true; // buffer_head의 clock_bit를 setting
    bh_entry->dirty = true; // 이 buffer는 더러워짐
    //printf("2.bh_entry\n");

    // buffer의 data를 buffer cache로 기록
    memcpy(bh_entry->data, buffer+bytes_written, chunk_size);
    //printf("3.memcpy\n");
    lock_release(&bh_entry->lock);
    return true;
};


/* Buffer cache를 순회하며 DISK block의 캐싱 여부 검사*/
// 캐싱 되어있다면, buffer cache entry
struct buffer_head * 
bc_lookup (block_sector_t sector){
    
    // buffer에서 항목을 제거하는 작업의 중간 과정이 보이지 않도록
    // lock을 걸지 않으면 한 sector에 대한 cache가 여러 개 만들어짐
    lock_acquire(&buffer_head_lock);
    // buffer_head를 순회하며, 전달받은 sector 값과 
    for(int i=0;i<64;i++){
        // 동일한 sector 값을 갖는 buffer cache entry가 있는 지 확인
        if(!bh_table[i].used && bh_table[i].sector == sector)
        {
            // cache hit
            //data 접근전 lock을 획득하고,
            
            lock_acquire(&bh_table[i].lock);
            // 처음에 lock을 해제합니다.
            lock_release(&buffer_head_lock);
            return &bh_table[i];
        }
    }
    // cache 미스 상황
    // 이 상황을 유지하기 위해 처음에 잠금 lock이 걸린 상태로 반환
    return NULL;
}



/* Buffer cache에서 victim(뺄 거)을 선정하여 entry head pointer를 반환 */
struct buffer_head * 
bc_select_victim (void){
    // clock 알고리즘을 사용하여 victim entry를 선택
    struct buffer_head *bh_entry;
    
    while(1){
        for(;clock_hand!=bh_entry+64;clock_hand++)
        {
            lock_acquire(&clock_hand->lock);
            if(clock_hand->used || !clock_hand->accessed){
                return clock_hand++;
            }
            /*update buffer head*/
            clock_hand->accessed = false;
            lock_release(&clock_hand->lock);
        }
        clock_hand = bh_table;    
    }
    // disk로 flush
    //if(bh_table[idx].dirty){
    //    bc_flush_entry(bh_entry);
    //}
    NOT_REACHED();
}