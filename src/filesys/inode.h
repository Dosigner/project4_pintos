#ifndef FILESYS_INODE_H
#define FILESYS_INODE_H

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "filesys/off_t.h"
#include "devices/block.h"
#include "threads/synch.h" // lock


// unsigned long elem_type
// Each bit represents one bit in the bitmap
// 만약 요소의 비트 0이 비트맵의 비트 K를 나타낸다면, 요소의 비트 1은 비트맵의 비트 K+1을 나타낸 것이다.

// From the outside, a bitmap is an array of bits.
// From the inside. it's an array of elem_type that simulates an array of bits 
/* struct bitmap
{
    size_t bit_cnt;     // bit들의 갯수
    elem_type *bits;    // bit들을 표현하는 element들
}; */
struct bitmap;

struct buffer_head
{
    bool dirty;     // flag that show this entry dirty
    bool used;      // flag that show this entry used 
    bool clock_bit; // clock bit
    
    block_sector_t sector; // cached disk sector address

    struct lock head_lock; // 해당 cache에 쓰기 작업을 하기 전 이 lock을 획득

    void* data; // buffer cache entry를 가리키기 위한 데이터 포인터

};


void inode_init (void);
bool inode_create (block_sector_t, off_t, uint32_t);
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


///////////////////////////////
/* Buffer cache를 초기화하는 함수 */
void bc_init(void);
/* 모든 dirty entry flush 및 buffer cache 해지 */
void bc_term (void);

/* Buffer cache에서 요청 받은 buffer frame을 읽어옴 */
bool bc_read(block_sector_t, void*, off_t, int, int);

/* Buffer cache에서 buffer frame에 요청 받은 data를 기록 */
bool bc_write(block_sector_t, void*, off_t, int, int);

/* Buffer cache를 순회하며 target sector가 있는지 검색 */
struct buffer_head* bc_lookup(block_sector_t);

/* Buffer cache에서 victim(뺄 거)을 선정하여 entry head pointer를 반환 */
struct buffer_head* bc_select_victim(void);

/////////////////////////////////////
bool is_removed(struct inode*);
block_sector_t inode_to_sector(struct inode*);
bool inode_is_dir(struct inode* inode);


#endif /* filesys/inode.h */
