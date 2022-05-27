#ifndef FILESYS_BUFFER_CACHE_H
#define FILESYS_BUFFER_CACHE_H

#include "filesys/filesys.h"
#include "filesys/off_t.h" // off_t
#include "threads/synch.h" // lock

#include <stdbool.h> // bool
#include "filesys/inode.h"


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

#endif
