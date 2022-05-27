#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h" // lock

#include "filesys/inode.h"


// unsigned long elem_type
// Each bit represents one bit in the bitmap
// 만약 요소의 비트 0이 비트맵의 비트 K를 나타낸다면, 
// 요소의 비트 1은 비트맵의 비트 K+1을 나타낸 것이다.

// From the outside, a bitmap is an array of bits.
// From the inside. it's an array of elem_type that simulates an array of bits 
/* struct bitmap
{
    size_t bit_cnt;     // bit들의 갯수
    elem_type *bits;    // bit들을 표현하는 element들
}; */

/* buffer cache의 각 entry를 관리 */
struct buffer_head{
    struct inode* inode;

    bool dirty; // flag that show this entry dirty
    bool used; // flag that show this entry used
    bool accessed; // clock bit

    block_sector_t sector; // cached disk sector address
    
    struct lock lock; // 해당 cache에 쓰기 작업을 하기 전 이 lock을 획득
    
    void* data; // buffer cache entry를 가리키기 위한 데이터 포인터
};

struct bitmap;



void inode_init (void);
bool inode_create (block_sector_t, off_t);
struct inode *inode_open (block_sector_t);
struct inode *inode_reopen (struct inode *);
block_sector_t inode_get_inumber (const struct inode *);
void inode_close (struct inode *);
void inode_remove (struct inode *);
off_t inode_read_at (struct inode *, void *, off_t size, off_t offset);
off_t inode_write_at (struct inode *, const void *, off_t size, off_t offset);
void inode_deny_write (struct inode *);
void inode_allow_write (struct inode *);
off_t inode_length (const struct inode *);

/* Buffer cache를 초기화하는 함수 */
void bc_init (void);
/* 모든 dirty entry flush 및 buffer cache 해지 */
void bc_term (void);

/* buffer cache를 순회하면서 dirty bit가 true인 entry를 모두 disk로 flush */
void bc_flush_all_entries (void);

/* 인자로 주어진 entry의 dirty bit를 false로 setting하면서 해당 내역을 disk로 flush */
void bc_flush_entry (struct buffer_head * p_flush_entry);

/* Buffer cache에서 요청 받은 buffer frame을 읽어옴 */
bool bc_read  (block_sector_t sector_idx, void * buffer, off_t bytes_read, int chunk_size, int sector_ofs);
/* Buffer cache에서 buffer frame에 요청 받은 data를 기록 */
bool bc_write (block_sector_t sector_idx, void * buffer, off_t bytes_written, int chunk_size, int sector_ofs);

/* Buffer cache를 순회하며 target sector가 있는지 검색 */
struct buffer_head * bc_lookup (block_sector_t sector);
/* Buffer cache에서 victim(뺄 거)을 선정하여 entry head pointer를 반환 */
struct buffer_head * bc_select_victim (void);

#endif /* filesys/inode.h */
