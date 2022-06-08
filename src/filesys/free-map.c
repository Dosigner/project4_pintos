#include "filesys/free-map.h"
#include <bitmap.h>
#include <debug.h>
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"

static struct file *free_map_file;   /* Free map file. */
static struct bitmap *free_map;      /* Free map, one bit per sector. */

/* Initializes the free map. */
void
free_map_init (void) 
{
  // filesystem 크기의 bitmap 생성 후, 각 bit의 값을 false로 초기화
  free_map = bitmap_create (block_size (fs_device));
  if (free_map == NULL)
    PANIC ("bitmap creation failed--file system device is too large");
  /* FREE_MAP_SECTOR = 0, ROOT_DIR_SECTOR = 1*/
  bitmap_mark (free_map, FREE_MAP_SECTOR); // 전달받은 disk block 번호의 bit를 true
  bitmap_mark (free_map, ROOT_DIR_SECTOR);
}

/* Allocates CNT consecutive sectors from the free map and stores
   the first into *SECTORP.
   Returns true if successful, false if not enough consecutive
   sectors were available or if the free_map file could not be
   written. */
bool  // free-map에서 할당할 block을 first-fit 방식으로 검색, cnt : 할당하고자 하는 block 개수, sectorp : 할당 받은 block의 시작 번호
free_map_allocate (size_t cnt, block_sector_t *sectorp)
{
  // bitmap에서 할당할 연속된 block을 찾고, 할당할 block의 bitmap을 true로 설정
  block_sector_t sector = bitmap_scan_and_flip (free_map, 0, cnt, false);
  if (sector != BITMAP_ERROR
      && free_map_file != NULL
      && !bitmap_write (free_map, free_map_file))
    {
      bitmap_set_multiple (free_map, sector, cnt, false); 
      sector = BITMAP_ERROR;
    }
  if (sector != BITMAP_ERROR)
    *sectorp = sector;
  return sector != BITMAP_ERROR;
}

/* Makes CNT sectors starting at SECTOR available for use. */
void
free_map_release (block_sector_t sector, size_t cnt)
{
  ASSERT (bitmap_all (free_map, sector, cnt));
  bitmap_set_multiple (free_map, sector, cnt, false);
  bitmap_write (free_map, free_map_file);
}

/* Opens the free map file and reads it from disk. */
void // disk에 저장되어 있는 bitmap 정보 읽기
free_map_open (void) 
{
  // 메모리에 file 자료구조 할당 후, target file의 inode 포인팅 및 초기화
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  if (!bitmap_read (free_map, free_map_file)) // disk에 기록된 bitmap의 data 읽기
    PANIC ("can't read free map");
}

/* Writes the free map to disk and closes the free map file. */
void
free_map_close (void) 
{
  // bitmap 기록용 file의 close : in-memory inode를 해지 및 open_inodes list에서 제거
  file_close (free_map_file); 
}

/* Creates a new free map file on disk and writes the free map to
   it. */
void // bitmap의 inode 생성, bitmap의 data를 disk에 기록
free_map_create (void) 
{
  /* Create inode. */
  // bitmap의 inode를 0번 block에 생성
  if (!inode_create (FREE_MAP_SECTOR, bitmap_file_size (free_map),0))
    PANIC ("free map creation failed");
  /* Write bitmap to file. */
  free_map_file = file_open (inode_open (FREE_MAP_SECTOR));
  if (free_map_file == NULL)
    PANIC ("can't open free map");
  // bitmap의 data를 disk에 기록
  // write bitmap to file
  if (!bitmap_write (free_map, free_map_file))
    PANIC ("can't write free map");
}
