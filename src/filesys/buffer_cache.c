#include "threads/synch.h" // lock
#include "filesys/buffer_cache.h"
#include <debug.h>
#include <string.h>

#define BUFFER_CACHE_ENTRY_NB 64;

// data를 제외한, cache 스스로에 대한 정보 buffer head table
static struct buffer_head bh_table[BUFFER_CACHE_ENTRY_NB];

// 실제 data가 저장되는 buffer입니다.
static uint8_t *p_buffer_cache[BUFFER_CACHE_ENTRY_NB];

// victim entry 선정 시 clock 알고리즘을 위한 변수
static struct buffer_head *clock_hand; 

// buffer_head 배열인 bh_table에 새로운 값을 추가하거나 뺄 때 중간과정을 보이지 않게 하는 lock
static struct lock buffer_head_lock;

/* 1. Buffer cache를 초기화하는 함수 */
void bc_init (void){
    for(int i=0; i<BUFFER_CACHE_ENTRY_NB; i++){

        // 1. allocation buffer cache in Memory
        p_buffer_cache[i] = malloc(BLOCK_SECTOR_SIZE);
        
        // 전역변수 buffer_head 자료구조 초기화    
        memset(bh_table[i],0,sizeof(struct buffer_head));

        lock_init(&bh_table[i]->lock);

        // p_buffer_cache가 buffer cache 영역 pointing
        bh_table[i]->data = p_buffer_cache;
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
    for(int i=0; i<BUFFER_CACHE_ENTRY_NB; i++){
        free(p_buffer_cache[i]);
    }
}

/* 2.1 buffer cache를 순회하면서 dirty bit가 true인 entry를 모두 disk로 flush */
void bc_flush_all_entries (void){
    for(int i=0;i<BUFFER_CACHE_ENTRY_NB;i++){
        // 아직 쓰지 않은 모든 dirty cache를 써서 정리
        lock_acquire(&bh_table[i]->lock);
        bc_flush_entry(&bh_table[i]);
        lock_release(&bh_table[i]->lock);
    }
}

/* 인자로 주어진 entry의 dirty bit를 false로 setting하면서 해당 내역을 disk로 flush */
void bc_flush_entry (struct buffer_head * p_flush_entry){
    /* 인자로 전달받은 buffer cache entry의 data를 disk로 flush */
    if(p_flush_entry->dirty && p_flush_entry->used){
        // buffer의 cache data (p_flush_entry의 data) -> disk로
        block_write(fs_device, p_flush_entry->sector, p_flush_entry->data);
        /* buffer_head의 dirty 값 update */
        p_flush_entry->dirty = false;
    } 
}





/* Buffer cache에서 요청 받은 buffer frame을 읽어와서 user buffer에 저장 */
// buffer를 이용하여 읽기 작업 수행
bool bc_read(block_sector_t sector_idx, void * buffer, off_t bytes_read, int chunk_size, int sector_ofs){

    bool success = false;

    // buffer_head를 검색
    // entry가 존재하는 지 검색
    struct buffer_head *bh_entry = bc_lookup(sector_idx);
    
    /* 읽을 데이터가 buffer cache에 없으면, disk에서 읽어 buffer cache에 캐싱 */
    if(!bh_entry) {
        bh_entry = bc_select_victim();
        // bh_entry에 캐시에서 제거할 섹터가 있다.
        bc_flush_entry(bh_entry);
        bh_entry->dirty = false;
        bh_entry->used  = true;
        bh_entry->sector = sector_idx;
        // address를 지정하였으므로 lock을 해제.
        lock_release(&buffer_head_lock);

        // disk에서 buffer cache로 data를 block_read
        block_read(fs_device, sector_idx, bh_entry->data); // disk block data -> buffer cache
    }
    /* To Do 3 : Modify read*/
    // buffer cache data를 user buffer에 복사
    memcpy(buffer+ bytes_read, bh_entry->data+sector_ofs, chunk_size); // buffer cache의 data를 buffer로 읽기
    
    // update buffer_head
    bh_entry->accessed = true; // buffer_head의 clock_bit를 setting
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
        bh_entry->used = true;
        bh_entry->sector = sector_idx;

        // sector_idx를 지정하였으므로 해당 cache의 lock을 해제
        lock_release(&buffer_head_lock);

        // disk에서 buffer cache로 data를 block_read
        block_read(fs_device, sector_idx, bh_entry->data); // disk block data -> buffer cache
    }
    /*update buffer head*/
    bh_entry->accessed = true; // buffer_head의 clock_bit를 setting
    bh_entry->dirty = true; // 이 buffer는 더러워짐

    // buffer의 data를 buffer cache로 기록
    /*sector_idx를 buffer_head에서 검색하여 buffer에 복사 (구현)*/
    memcpy(bh_entry->data+sector_ofs, buffer+bytes_written, chunk_size);
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
    for(int i=0;i<BUFFER_CACHE_ENTRY_NB;i++){
        // 동일한 sector 값을 갖는 buffer cache entry가 있는 지 확인
        if(bh_table[i].used && bh_table[i].sector == sector)
        {
            // cache hit
            //data 접근전 lock을 획득하고,
            lock_acquire(&bh_table[i]->lock);
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
    int idx;

    while(1){
        for(;clock_hand!=bh_entry+BUFFER_CACHE_ENTRY_NB;
            clock_hand++)
        {
            lock_acquire(&clock_hand->lock);
            if(!clock_hand->used || !clock_hand->accessed){
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







