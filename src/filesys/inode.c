#include "filesys/inode.h"
#include <list.h>
#include <debug.h>
#include <round.h>
#include <string.h>
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "threads/synch.h"

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

// inode에 direct 방식으로 저장할 블록번호의 갯수
// inode_disk 자료구조의 크기가 1 블록 크기(512Byte)
#define DIRECT_BLOCK_ENTRIES 123

// struct inode_indirect_block의 크기가 BLOCK_SECTOR_SIZE와 같도록 하는 값
#define INDIRECT_BLOCK_ENTRIES (BLOCK_SECTOR_SIZE/ sizeof(block_sector_t))

#define BUFFER_CACHE_ENTRY_NB 64

////////     Project 4.1 Buffer Cache    ////////
// data를 제외한, cache 스스로에 대한 정보 buffer head table
static struct buffer_head bh_table[BUFFER_CACHE_ENTRY_NB];

// 실제 data가 저장되는 buffer입니다.
void* p_buffer_cache;

// victim entry 선정 시 clock 알고리즘을 위한 변수
int clock_hand;

// buffer_head로 이루어진 배열 bh_table에 새로운 값을 추가하거나 뺄 때 
// 중간과정을 보이지 않게 하는 lock
static struct lock buffer_head_lock;

/////////////////////////////////////////////////

struct lock extend_lock;


///////////// READ_AHEAD /////////////
struct list read_ahead_list;
struct read_ahead
{
  block_sector_t sector;
  struct list_elem elem;
};
struct lock read_ahead_lock;
struct semaphore read_ahead_sema;


/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk
  {
     //// DELETE //// block_sector_t start;  // First data sector
    off_t length;     // File size in bytes. 할당 된 블록길이(byte) 파일 생성 시, 디스크 상에 연속된 block을 할당 받음
    unsigned magic;    // Magic number. 
    bool is_dir; // dir(=true), file(=1) 

    // 아래 순서대로 table을 사용
    // 1. 접근할 disk 블록의 번호들이 저장 -> direct 방식
    block_sector_t direct_map_table[DIRECT_BLOCK_ENTRIES]; // direct로 접근할 disk blocks
    // 2. INDIRECT로 접근 시 index block의 번호 -> data block
    block_sector_t indirect_block_sec; 
    // 3. double indirect 방식으로 접근 시, 1차 index block의 번호
    block_sector_t double_indirect_block_sec;
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

    // uint32_t : inode가 저장된 block device sector의 index
    block_sector_t sector;              /* Sector number of disk location. */
    // opener의 갯수  
    int open_cnt;                       /* Number of openers. */
    // file의 삭제 여부
    bool removed;                       /* True if deleted, false otherwise. */
    int deny_write_cnt;                 /* 0: writes ok, >0: deny writes. */
    
    // inode에 관련된 data 접근시 사용하는 lock
    struct lock extend_lock;
    
    // struct inode_disk data;             /* Inode content. */
  };

/* Returns the block device sector that contains byte offset POS
   within INODE.
   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
// complete
static block_sector_t
byte_to_sector (const struct inode_disk *inode_disk, off_t pos) 
{
  ASSERT (inode_disk != NULL);

  if(pos > inode_disk->length)
    return -1;

  // 0. Byte 단위를 block 단위로 변환,  bytes/512
  int pos_sector = pos / BLOCK_SECTOR_SIZE;

  /* 1. Direct 방식일 경우 */
  if (pos_sector < DIRECT_BLOCK_ENTRIES){
    // 바로 접근하기
    return inode_disk->direct_map_table[pos_sector];
  }

  /* 2. Indirect 방식일 경우 */
  else if (pos_sector < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
    // 1단계 table의 index block         
    block_sector_t indirect_sector = inode_disk->indirect_block_sec;
    // direct block을 다 쓴 후 남은 block들이 offset 
    int sector_ofs = (pos_sector - DIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
    
    // disk block
    block_sector_t block_sector;
    // buffer cache에서 index block 자체를 읽어옴. index 블록에서 disk 블록 확인
    // 위에서 남은 sector_ofs만큼 cache에서 block_sector_t 크기만큼 block_sector에다가 값을 read한다. 
    bc_read(indirect_sector, &block_sector, 0, sector_ofs, sizeof(block_sector_t));
    
    return block_sector; // block_sector를 return
  }

  /* 3. Double Indirect 방식일 경우 */
  else if (pos_sector < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES * (INDIRECT_BLOCK_ENTRIES+1) ){
    // 2단계 참조 table의 index block   
    block_sector_t double_indirect_sector = inode_disk->double_indirect_block_sec;
    // 1단계 참조 table의 index block
    block_sector_t indirect_sector;
    
    int indirect_sector_ofs=(pos_sector - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) / INDIRECT_BLOCK_ENTRIES;
    int sector_ofs = indirect_sector_ofs * sizeof(block_sector_t);
    // buffer cache의 double_indirect_sector에서 1차 indirect_sector에 번호 읽어오기
    bc_read(double_indirect_sector, &indirect_sector, 0, sector_ofs, sizeof(block_sector_t));
    
    int sector_ofs_ = ((pos_sector - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) - (sector_ofs/sizeof(block_sector_t)) * INDIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
    // disk block
    block_sector_t block_sector;
    // 1차 buffer cache에서 sector_ofs_만큼 buffer cache의 indirect_sector에서 block_sector로 값 읽어오기
    bc_read(indirect_sector, &block_sector, 0, sector_ofs_, sizeof(block_sector_t));
    
    return block_sector;  // block_sector를 return      
  }

  else
    return -1;  
}


/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
// in-memory inode 전역 변수 (Doubled linked list)
static struct list open_inodes;

/* Initializes the inode module. */
void
inode_init (void) 
{
  // In-memory inode를 관리하는 Double-linked list 초기화
  list_init (&open_inodes);
  clock_hand = 0;
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool
inode_create (block_sector_t sector, off_t length, uint32_t is_dir)
{
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT (length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT (sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc (1, sizeof *disk_inode);
  if (disk_inode != NULL){
    // 1. byte data를 sector 단위로 변경
    size_t sectors = bytes_to_sectors (length); // byte 길이에 해당하는 block 개수 return
    
    disk_inode->length = length;
    disk_inode->is_dir = is_dir; // inode 생성 시, inode_disk에 추가한 file과 dir 구분을 위한 field를 is_dir 값으로 설정
    disk_inode->magic = INODE_MAGIC;
    
    if (length > 0){
      
      static char zeros[BLOCK_SECTOR_SIZE];
      // free map()에서 할당할 block을 first-fit 방식으로 검색, (할당할 block 갯수, 할당을 시작할 번호)

      for (int i = 0; i < sectors; i++){
        // i <= 123
        // zeros의 데이터를 -> bc의 데이터 블록에 직접 기록
        if (i < DIRECT_BLOCK_ENTRIES) {
          // 1 크기의 data를 저장할 연속된 block을 찾고, 할당 받은 disk 블록의 시작 번호를 disk_inode에 저장
          if (free_map_allocate (1, &disk_inode->direct_map_table[i])){ // block 할당
            // disk_inode에서 direct_map_table의 i번째에 저장된 sector 번호에 0으로 채워진 block을 작성
            bc_write(disk_inode->direct_map_table[i], zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }
          else
            return false;
        }
        
        
        // 123 < i <= 123+64... direct block에 저장해야되는 경우
        // 0. zeros의 빈 블록을 -> bc의 1차 테이블 index 블록에 기록
        
        // 1. block_sector의 데이터를 -> bc의 1차 테이블 index 블록에 기록
        // 2. zeros의 빈 블록을 -> block_sector 데이터 블록에 기록
        
        // 0을 1차 테이블에 배치 후, 데이터 블록을 1차 테이블에, 0을 데이터 블록에
        // 0 -> block_sector -> indirect_block_sec 
        else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
          // i==123 이면 indirect_block_sector만 만들면됨
          if (i == DIRECT_BLOCK_ENTRIES) {
            if (free_map_allocate(1, &disk_inode->indirect_block_sec)) // indirect_block index블록 할당
              bc_write(disk_inode->indirect_block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }

          // 할당된 indirect_block 위치를 안 후
          block_sector_t indirect_block_sector = disk_inode->indirect_block_sec;
          block_sector_t block_sector;
          
          if (free_map_allocate (1, &block_sector)){ // block을 할당
            // direct block 엔트리인 123을 빼고 남은 ofs을 계산
            int sector_ofs = (i - DIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
            
            // indirect_block_sec에서 sector ofs만큼 이동시킨 cache에다가 block_sec 값을 저장
            bc_write(indirect_block_sector, &block_sector, 0, sector_ofs, sizeof(block_sector_t));
            bc_write(block_sector, zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }
          else 
            return false;
        }
        
        // 123+64 < i <= 123 + 64 + 64*64... double indirect에 저장해야되는 경우
        // 0. zeros의 데이터를 -> bc의 2차 테이블 index 블록에 기록
        // double indirect_offset에 딱 걸친 경우
          // 1. zeros의 데이터를 -> bc의 1차 테이블 index 블록에 기록
          // 1.1 offset이 0일때 bc의 1차 테이블 index 블록을 -> bc의 2차 테이블 index 블록에 기록
        // 1. bc의 2차 테이블 블록에서 bc의 1차 테이블 index 블록으로 읽어오기
        // 2. zeros의 데이터를 -> 데이터 블록에 기록
        // 3. 데이터 블록을 -> bc의 1차 테이블 index 블록에 기록
        
        // 0 -> data block -> 1차 table <- 만든 2차 테이블
        else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES*(INDIRECT_BLOCK_ENTRIES+1))
        {
          // 이제 i==123+64이면 double_indirect_block_sector를 할당해야
          if (i == DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
            if (free_map_allocate(1, &disk_inode->double_indirect_block_sec)) // double indirect_block을 할당
              bc_write(disk_inode->double_indirect_block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE); // 쓰기
          }

          // 할당된 double_indirect_block 위치를 안 후
          block_sector_t double_indirect_block_sector = disk_inode->double_indirect_block_sec;
          block_sector_t indirect_block_sector;
          int indirect_sector_ofs = ((i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) / INDIRECT_BLOCK_ENTRIES);
          int sector_ofs =  indirect_sector_ofs * sizeof(block_sector_t); // indirect_offset

          // double indirect_offset
          int sector_ofs_ = ((i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) - (sector_ofs/sizeof(block_sector_t)) * INDIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
          if (sector_ofs_ == 0){
            if (free_map_allocate(1, &indirect_block_sector)){ // sector_ofs_ 0 이면 그냥 indirect_block_sector 할당
              bc_write(indirect_block_sector, zeros, 0, 0, BLOCK_SECTOR_SIZE); 
              bc_write(double_indirect_block_sector, &indirect_block_sector, 0, sector_ofs, sizeof(block_sector_t));
            }
          }
          
          // 만든 double indirect_sector를 indirect_block sector로 가져오기
          bc_read(double_indirect_block_sector, &indirect_block_sector, 0, sector_ofs, sizeof(block_sector_t));
          block_sector_t block_sector;

          if (free_map_allocate (1, &block_sector)){ // block sector 할당
            bc_write(block_sector, zeros, 0, 0, BLOCK_SECTOR_SIZE);
            bc_write(indirect_block_sector, &block_sector, 0, sector_ofs_, sizeof(block_sector_t));
          }
          else 
            return false;
        }

        else
          return -1;
      }     
    }
    // 위에서 만든 disk_inode를 sector에다가 쓰기
    bc_write(sector, disk_inode, 0, 0, sizeof (struct inode_disk)); // inode disk도 어떤 sector에 저장
    free (disk_inode);
    success = true;
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
  // inode 자료구조 할당
  /* Allocate memory. */
  inode = malloc (sizeof *inode);
  if (inode == NULL)
    return NULL;

  // inode 자료구조 초기화
  /* Initialize. */
  list_push_front (&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;

  // inode 자료구조 초기화 시, lock 변수 초기화 부분 추가
  lock_init(&inode->extend_lock);
 
  // block_read (fs_device, inode->sector, &inode->data);
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
          struct inode_disk disk_inode; // on_disk inode

          // 1. inode의 on-disk inode 획득
          bc_read(inode->sector, &disk_inode, 0, 0, sizeof (struct inode_disk));

          // 2. disk inode 할당 해제
          free_map_release (inode->sector, 1);
          
          // 3. on-disk inode들 반환
          for (int i = 0; i < bytes_to_sectors(disk_inode.length); i++){

            if (i < DIRECT_BLOCK_ENTRIES){
              free_map_release (disk_inode.direct_map_table[i], 1);
            }
            
            else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
              block_sector_t indirect_block_sec = disk_inode.indirect_block_sec;
              block_sector_t block_sec;
                
              int sector_ofs = (i - DIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
              // indirect -> block
              bc_read(indirect_block_sec, &block_sec, 0, sector_ofs, sizeof(block_sector_t));
              free_map_release(block_sec, 1);
            }

            else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES*(INDIRECT_BLOCK_ENTRIES+1)){
              block_sector_t double_indirect_block_sec = disk_inode.double_indirect_block_sec;
              block_sector_t indirect_block_sec;
              block_sector_t block_sec;
              int indirect_sector_ofs = (i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) / INDIRECT_BLOCK_ENTRIES;
              int sector_ofs = indirect_sector_ofs * sizeof(block_sector_t);
              int sector_ofs_ = ((i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) - (sector_ofs/sizeof(block_sector_t)) * INDIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
              // double -> indirect
              bc_read(double_indirect_block_sec, &indirect_block_sec, 0, sector_ofs, sizeof(block_sector_t));
              // indirect -> block
              bc_read(indirect_block_sec, &block_sec, 0, sector_ofs_, sizeof(block_sector_t)); 
              free_map_release(block_sec, 1);
            }
          }        
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

bool
is_removed (struct inode *inode)
{
  return inode->removed;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached. */
off_t
inode_read_at (struct inode *inode, void *buffer_, off_t size, off_t offset) 
{
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  struct inode_disk inode_disk; // on_disk inode

  if (inode->sector > 4096)
    return -1;

  // 먼저 락을 취득
  lock_acquire(&inode->extend_lock);
  
  // disk inode를 buffer cache에서 읽음
  bc_read(inode->sector, &inode_disk, 0, 0, sizeof (struct inode_disk));
  while (size > 0){
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector (&inode_disk, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_disk.length - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;
    
    // sector_idx이 정해진 이후, 데이터 읽기 작업은 lock을 해제한 상태에서 수행
    bc_read(sector_idx, buffer, bytes_read, sector_ofs, chunk_size); 
    block_sector_t next_sector = byte_to_sector (&inode_disk, offset + chunk_size);
    
    if (next_sector >= 0) 
      add_cache_read_ahead(next_sector);
    
    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }

  lock_release(&inode->extend_lock);
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
  
  if (inode->sector > 4096)
    return -1;
  
  // write 금지
  if (inode->deny_write_cnt)
    return 0;
  struct inode_disk inode_disk; // on_disk inode
  bc_read(inode->sector, &inode_disk, 0, 0, sizeof (struct inode_disk));

  int old_length = inode_disk.length;
  int write_end = offset + size - 1;
  
  lock_acquire(&inode->extend_lock);
  if (write_end > old_length - 1){
    int length = write_end - (old_length - 1);
    if (length > 0){

      inode_disk.length = write_end + 1;
      static char zeros[BLOCK_SECTOR_SIZE];
      
      for (int i = bytes_to_sectors(old_length); i < bytes_to_sectors(inode_disk.length); i++){
        
        if (i < DIRECT_BLOCK_ENTRIES){
          if (free_map_allocate (1, &inode_disk.direct_map_table[i])){
            bc_write(inode_disk.direct_map_table[i], zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }
          else 
            return false;
        }
        
        else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
          
          if (i == DIRECT_BLOCK_ENTRIES){
            if (free_map_allocate(1, &inode_disk.indirect_block_sec))
              bc_write(inode_disk.indirect_block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }
          
          block_sector_t indirect_block_sec = inode_disk.indirect_block_sec;
          block_sector_t block_sec;
          
          if (free_map_allocate (1, &block_sec)){
            int sector_ofs = (i - DIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
            bc_write(indirect_block_sec, &block_sec, 0, sector_ofs, sizeof(block_sector_t));
            bc_write(block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }

          else 
            return false;
        }

        else if (i < DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES*(INDIRECT_BLOCK_ENTRIES+1)){
          if (i == DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES){
            if (free_map_allocate(1, &inode_disk.double_indirect_block_sec))
              bc_write(inode_disk.double_indirect_block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);
          }

          block_sector_t double_indirect_block_sec = inode_disk.double_indirect_block_sec;
          block_sector_t indirect_block_sec;
          int sector_ofs = ((i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) / INDIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);

          if ((i - (DIRECT_BLOCK_ENTRIES + INDIRECT_BLOCK_ENTRIES)) % INDIRECT_BLOCK_ENTRIES == 0){
            
            if (free_map_allocate(1, &indirect_block_sec)){
              bc_write(indirect_block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);
              bc_write(double_indirect_block_sec, &indirect_block_sec, 0, sector_ofs, sizeof(block_sector_t));
            }
          }
            
          bc_read(double_indirect_block_sec, &indirect_block_sec, 0, sector_ofs, sizeof(block_sector_t));
          block_sector_t block_sec;

          if (free_map_allocate (1, &block_sec)){
            int sector_ofs_ = ((i - DIRECT_BLOCK_ENTRIES - INDIRECT_BLOCK_ENTRIES) - (sector_ofs/sizeof(block_sector_t)) * INDIRECT_BLOCK_ENTRIES) * sizeof(block_sector_t);
            
            bc_write(indirect_block_sec, &block_sec, 0, sector_ofs_, sizeof(block_sector_t));
            bc_write(block_sec, zeros, 0, 0, BLOCK_SECTOR_SIZE);              
          }
          else 
            return -1;
        }

        else 
          return -1;
      }
      bc_write(inode->sector, &inode_disk, 0, 0, sizeof (struct inode_disk));      
    }
  }
  
  while (size > 0) 
    {
      /* Sector to write, starting byte offset within sector. */
      block_sector_t sector_idx = byte_to_sector (&inode_disk, offset);
      int sector_ofs = offset % BLOCK_SECTOR_SIZE;

      /* Bytes left in inode, bytes left in sector, lesser of the two. */
      off_t inode_left = inode_disk.length - offset;
      int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
      int min_left = inode_left < sector_left ? inode_left : sector_left;

      /* Number of bytes to actually write into this sector. */
      int chunk_size = size < min_left ? size : min_left;
      if (chunk_size <= 0)
        break;

      bc_write(sector_idx, buffer, bytes_written, sector_ofs, chunk_size); 
      block_sector_t next_sector = byte_to_sector (&inode_disk, offset + chunk_size);
      
      if (next_sector >= 0) 
        add_cache_read_ahead(next_sector);

      /* Advance. */
      size -= chunk_size;
      offset += chunk_size;
      bytes_written += chunk_size;
    }

  lock_release(&inode->extend_lock);
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
  // return inode->data.length;
  struct inode_disk inode_disk;
  bc_read(inode->sector, &inode_disk, 0, 0, sizeof (struct inode_disk));
  return inode_disk.length;
}


/* Buffer cache에서 요청 받은 buffer frame을 읽어와서 user buffer에 저장 */
// buffer를 이용하여 읽기 작업 수행
bool 
bc_read(block_sector_t sector_idx, void* buffer, off_t bytes_read, int sector_ofs, int chunk_size)
{
  // read data from buffer cache
  lock_acquire(&buffer_head_lock);
  struct buffer_head *head = bc_lookup(sector_idx);
  // store in use buffer
  // if no data in buffer cache, read from disk -> cache
  if (!head){
    // read from disk to cache (update buffer)
    struct buffer_head* victim_entry = bc_select_victim();
    // dirty인 경우 victim entry를 disk로 flush하기
    if (victim_entry->dirty)
      block_write(fs_device, victim_entry->sector, victim_entry->data);
    
    head = victim_entry;
    head->dirty = false;
    head->clock_bit = true;
    head->sector = sector_idx;

    // disk에서 cache로 data를 block_read하기
    // head를 다른 걸로 교체됨
    block_read(fs_device, sector_idx, head->data); 
  }
  // 읽혔으니 사용됨
  head->used = true;
  // address를 지정하였으면 lock 해제
  lock_release(&buffer_head_lock);

  memcpy (buffer + bytes_read, head->data + sector_ofs, chunk_size);
  // buffer cache data -> user buffer
  lock_release(&head->head_lock);
  return true;
}

/* cache에서 요청 받은 data를 buffer frame에 기록 */
bool 
bc_write(block_sector_t sector, void* buffer, off_t bytes_written, int sector_ofs, int chunk_size)
{
  lock_acquire(&buffer_head_lock);
  // buffer_head를 검색
  struct buffer_head *head = bc_lookup(sector);
  
  // if no data in buffer cache, read from disk -> cache
  if (!head){
    // read from disk to cache (update buffer)
    struct buffer_head* victim_entry = bc_select_victim();

    if (victim_entry->dirty)
      block_write(fs_device, victim_entry->sector, victim_entry->data);
    
    head = victim_entry;
    head->dirty = false;
    head->clock_bit = true;
    head->sector = sector;
    block_read(fs_device, sector, head->data); 
  }
  // store in use buffer
  lock_release(&buffer_head_lock);

  // write 했으니깐
  head->dirty = true;
  head->used = true;
  
  memcpy (head->data + sector_ofs, buffer + bytes_written, chunk_size);
  //  user buffer -> buffer cache data
  lock_release(&head->head_lock); 
  // bc_lookup에서 건 lock을 해제
  return true;
}


/* Buffer cache를 순회하며 DISK block의 캐싱 여부 검사*/
// 캐싱 되어있다면, buffer cache entry
struct buffer_head* 
bc_lookup(block_sector_t sector){

  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i++){

    if (bh_table[i].sector == sector){
      lock_acquire(&bh_table[i].head_lock);
      return &bh_table[i];
    }
  }

  return NULL;
}

struct buffer_head* 
bc_select_victim(void){
  struct buffer_head* victim_entry = NULL;
  while(1) {
    struct buffer_head* head = &bh_table[clock_hand];
    
    if (head->used){
      head->used = false;
      clock_hand++;
      
      if (clock_hand == BUFFER_CACHE_ENTRY_NB) 
        clock_hand = 0;
    }
    // 사용하지 않은 entry중에서
    else {
      victim_entry = head;
      clock_hand++;

      if (clock_hand == BUFFER_CACHE_ENTRY_NB) 
        clock_hand = 0;
      
      // 방출을 위해
      lock_acquire(&victim_entry->head_lock);
      return victim_entry;
    }
  }
}

void add_cache_read_ahead (block_sector_t sector){
  if (sector == -1)
    return;
  struct read_ahead *cache = malloc(sizeof(struct read_ahead));
  cache->sector = sector;

  lock_acquire(&read_ahead_lock);
  list_push_back(&read_ahead_list, &cache->elem);
  sema_up(&read_ahead_sema);
  lock_release(&read_ahead_lock);
}

static void cache_read_ahead (void *aux UNUSED)
{
  while(1)
  {
    sema_down(&read_ahead_sema);
    lock_acquire(&read_ahead_lock);
    if (!list_empty(&read_ahead_list)){
      struct read_ahead *cache = list_entry(list_pop_front(&read_ahead_list), struct read_ahead, elem);
      block_sector_t sector = cache->sector;
      bc_read(sector, NULL, 0, 0, 0);
      free(cache);
    }
    lock_release(&read_ahead_lock);
  }
}

bool inode_is_dir(struct inode* inode) {
  struct inode_disk disk_inode;
  // in-memory inode의 on-disk inode를 읽어 inode_disk에 저장
  // inode->sector -> on-disk inode에 저장
  bc_read(inode->sector, &disk_inode, 0, 0, sizeof (struct inode_disk));
  
  // on-disk inode의 is_dir로 반환
  if (disk_inode.is_dir)
    return true;
  else
    return false;
}

block_sector_t inode_to_sector(struct inode* inode)
{
  return inode->sector;
}


void bc_init()
{
  lock_init(&buffer_head_lock);

  lock_init(&read_ahead_lock);
  sema_init(&read_ahead_sema, 0);
  list_init(&read_ahead_list);

  thread_create("read_ahead", 63, cache_read_ahead, NULL);
  p_buffer_cache = malloc(BUFFER_CACHE_ENTRY_NB * BLOCK_SECTOR_SIZE);
  
  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i ++){
    // initialize
    bh_table[i].dirty = false;
    bh_table[i].clock_bit = false;
    bh_table[i].used = false;
    bh_table[i].data = p_buffer_cache + i*BLOCK_SECTOR_SIZE;
    
    bh_table[i].sector = -1; // trash value
    
    lock_init(&bh_table[i].head_lock); 
  }
}

void bc_term()
{
  for (int i = 0; i < BUFFER_CACHE_ENTRY_NB; i ++){
    // dirty하고 clock_bit는 다 flush
    if (bh_table[i].dirty && bh_table[i].clock_bit){
      block_write(fs_device, bh_table[i].sector, bh_table[i].data);
    } 
  }
  // initialize
  free(p_buffer_cache);
}