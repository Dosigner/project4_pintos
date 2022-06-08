#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"
#include "threads/thread.h"
#include "filesys/inode.h"

/* Partition that contains the file system. */
struct block *fs_device; // filesystem device를 포인팅

static void do_format (void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
void
filesys_init (bool format) 
{
  fs_device = block_get_role (BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC ("No file system device found, can't initialize file system.");

  // 1. cache 초기화
  bc_init();
  
  // 2. In-memory inode를 관리하는 list 초기화
  inode_init ();
  
  // 3. In-memory bitmap 생성 및 초기화
  free_map_init ();

  if (format) 
    do_format ();  // bitmap의 inode 생성 및 disk에 기록, Root dir의 inode 생성
  free_map_open ();
  
  // filesystem 초기화 후, 현재 thread의 dir필드에 root dir로 설정
  thread_current()->current_dir = dir_open_root(); // root dir의 정보를 return
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void
filesys_done (void) 
{
  // buffer cache 종료
  bc_term();
  // bitmap 기록용 file의 닫기. in-memory inode를 해지 및 open_inodes list에서 제거
  free_map_close ();
}
 
/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */

// root dir에 해당 이름의 name으로 크기만큼 생성
bool
filesys_create (const char *name, off_t initial_size) 
{
  // 1. Path 분석
  char *file_name;
  char* cp_name = malloc( strlen(name)+1 );
  // 생성하고자 하는 파일경로인 name을 cp_name에 복사
  strlcpy(cp_name, name, strlen(name)+1 );
  
  // 2. Path의 dir open, file_name에 생성하고자 하는 파일의 이름이 저장
  struct dir* dir = parse_path(cp_name, &file_name);
  
  // 3. file_name이 NULL이면 없게 설정
  if (file_name == NULL)
    file_name = "";
  
  // 4. dir의 inode가 없어진건 아닌지 check
  if (is_removed(dir->inode))
    return false;
  
  block_sector_t inode_sector = 0;
  bool success = (dir != NULL
                  && free_map_allocate (1, &inode_sector) // free-map에서 inode의 block 할당
                  && inode_create (inode_sector, initial_size, 0) // free-map의 on-disk inode 생성시 is_dir 값을 0으로 설정
                  && dir_add (dir, file_name, inode_sector));     // 해당 dir entry 추가
  
  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1); // root directory inode 메모리 해지
  
  // directory close
  dir_close (dir); 
  free(cp_name);
  
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *
filesys_open (const char *name)
{
  // file 자료구조 생성 및 초기화하여 file pointer 반환
  
  // 1. name의 파일 경로를 cp_name에 복사
  char *file_name = NULL;
  char* cp_name = malloc(strlen(name)+1);
  strlcpy(cp_name, name, strlen(name) + 1);

  // 2. cp_name의 경로 분석
  struct dir* dir = parse_path(cp_name, &file_name);
  
  if (dir != NULL && is_removed(dir->inode))
    return NULL;

  struct inode *inode = NULL;

  // 경로 분석 후 빈 폴더인 경우
  if (!file_name){
    inode = dir_get_inode(dir); // dir의 inode를 바로 얻고
    // 현재 dirctory 닫고
    dir_close(dir);
    free(cp_name);

    return file_open(inode); // inode에 바로 메모리 자료구조 할당 및 초기화
  }

  // dir가 존재하면
  if (dir)
    dir_lookup (dir, file_name, &inode); // dir entry를 검색. file의 inode를 open_inodes 리스트에 추가
  
  // dir 닫기
  dir_close (dir);
  free(cp_name);

  // inode에 해당하는 file pointer를 반환
  return file_open(inode); // 메모리에 file 자료구조 할당 및 초기화
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool
filesys_remove (const char *name) 
{
  bool success;

  // 1. 생성하고자 하는 file 경로인 name을 cp_name에 복사
  char *file_name;
  char* cp_name = malloc(strlen(name)+1);
  strlcpy(cp_name, name, strlen(name)+1);

  // 2. file 생성 시 절대, 상대경로를 분석하여 dir에 file을 생성
  struct dir* dir = parse_path(cp_name, &file_name);
  struct inode *inode = NULL;

  // 그냥 dir인 경우
  if (file_name == NULL || (file_name == ".") || (file_name == "..") ){
    // 해당 dir 닫고
    dir_close(dir);
    free(cp_name);
    return false;
  }

  // dir이 비어있지 않으면
  if (dir)
    dir_lookup (dir, file_name, &inode); // dir에서 file_name 찾고 inode로 반환
  
  if (inode == NULL) printf("no inode for %s\n", name);

  // 현재 dir에서 parent inode를 찾기
  struct inode* parent_inode;
  dir_lookup (thread_current()->current_dir, "..", &parent_inode);
  
  // 부모 inode가 있으면
  if (parent_inode != NULL && inode != NULL){
    // inode와 parent inode가 같으면
    if (inode_to_sector(inode) == inode_to_sector(parent_inode)){
      // 해당 dir 닫고
      dir_close(dir);
      free(cp_name);
      return false;
    }
  }
  
  // inode가 dir일 경우
  if (inode_is_dir(inode)){
    char temp_name[NAME_MAX + 1];
    struct dir* rm_dir = dir_open(inode);
    //bool open = false;
    
    // rm_dir에서 dir을 읽기
    while (dir_readdir (rm_dir, temp_name)){
      if (temp_name[0] == '.'){
        if(strlen(temp_name) == 1) 
          continue;
        else if (temp_name[1] == '.'){
          if (strlen(temp_name) == 2)
            continue;
        }
      }
      success = true;
      break;
    }

    success = dir != NULL && dir_remove(dir, file_name);
    inode_close(inode);
    
    /*if (!open){
      success = dir != NULL && dir_remove(dir, file_name);
      inode_close(inode);
    }
    else {
      success = false;
    }*/

    dir_close(rm_dir);
  }
  else { //file인 경우 dir entry에서 file_name으로 삭제하기
    success = dir != NULL && dir_remove(dir, file_name);
    inode_close(inode);
  }

  dir_close(dir); // 디렉토리 닫기
  free(cp_name);
  return success;
}
 

//////// 경로 분석 함수 구현 ////////
struct dir* 
parse_path (char* path_name, char** file_name)
{
  char **parse = malloc(sizeof(char*) * 20);
  int end = 0;
  char *save_ptr;
  // path_name이 절대/상대경로에 따른 dir 정보 저장
  // 1. / 로 쪼개기
  char *ptr = strtok_r(path_name, "/", &save_ptr);
  while(ptr != NULL)
  {
    // ptr이 NULL이 안나올 때까지 "/" 로 token 분해하기
    parse[end] = ptr;
    end++;
    ptr = strtok_r(NULL, "/", &save_ptr);
  }

  struct dir* dir;
  
  // path_name이 0인 경우에는
  if (strlen(path_name) == 0){
    *file_name = NULL;
    return dir;
  }

  // 절대 경로인 경우
  if (path_name[0] == '/'){
    // root dir 오픈
    dir = dir_open_root();

    // root 경로이기 때문에
    if (strlen(path_name)==1){
      *file_name = NULL; // filename을 NULL로 설정하고 dir return
      return dir;
    }
  }
  // 상대 경로인 경우
  else {
    // 현재 thread의 dir가 비어있다면
    if (thread_current()->current_dir == NULL){
      // root dir 오픈하기
      dir = dir_open_root();
    }
    // 아닌 경우 현재 dir를 다시 오픈하기
    else 
      dir = dir_reopen(thread_current()->current_dir);
  }

  // parse에 마지막 원소가 file 이름 ex). dir/dir/file
  *file_name = parse[end-1];
  // parse 하나씩 분석하면서 현재 dir에서 해당 file이 있는지 확인
  for (int i = 0; i < end-1; i ++) {
    struct inode *inode = NULL;
    // dir에서 parse에 i번째 원소 이름을 찾기.
    // 찾았는데 없는 경로면
    if (!dir_lookup (dir, parse[i], &inode)){
      dir_close(dir);
      return NULL; // dir를 NULL로 설정
    }
    // 있는 경로면
    else{
      // inode가 dir인 경우
      if(inode_is_dir(inode)){
        // 다음 dir 오픈
        struct dir* sub_dir = dir_open(inode);
        // 현재 dir 닫고
        dir_close(dir);
        dir = sub_dir;
      }

      // inode가 file인 경우
      else {
        // file이면 현재 dir를 닫고
        dir_close(dir);
        return NULL; // dir를 NULL로 return
      }
    } 
  }

  free(parse);  
  return dir;
}

// bitmap의 inode 생성 및 disk에 기록, root dir의 inode 생성
static void
do_format (void)
{
  printf ("Formatting file system...");
  free_map_create ();
  // 1번 디스크 블록에 root directory의 inode를 16개 생성
  if (!dir_create (ROOT_DIR_SECTOR, 16))
    PANIC ("root directory creation failed");

  struct dir* root_dir = dir_open_root(); // root dir의 inode 생성

  dir_add (root_dir, ".", ROOT_DIR_SECTOR);
  dir_add (root_dir, "..", ROOT_DIR_SECTOR);

  dir_close(root_dir);

  free_map_close (); // bitmap 기록용 file의 닫기
  printf ("done.\n");
}