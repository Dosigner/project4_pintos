#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"


/* Additional */
#include <list.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "filesys/filesys.h"     // filesys_OOO()
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "filesys/inode.h"
#include "devices/shutdown.h" // shutdown_power_off()
#include "userprog/process.h"

#include "filesys/directory.h"

typedef void sig_func(void);

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

struct inode_disk
  {
     //// DELETE //// block_sector_t start;  // First data sector
    off_t length;     // File size in bytes. 할당 된 블록길이(byte) 파일 생성 시, 디스크 상에 연속된 block을 할당 받음
    unsigned magic;    // Magic number. 
    bool is_dir; // dir(=true), file(=1) 

    // 아래 순서대로 table을 사용
    // 1. 접근할 disk 블록의 번호들이 저장 -> direct 방식
    block_sector_t direct_map_table[123]; // direct로 접근할 disk blocks
    // 2. INDIRECT로 접근 시 index block의 번호 -> data block
    block_sector_t indirect_block_sec; 
    // 3. double indirect 방식으로 접근 시, 1차 index block의 번호
    block_sector_t double_indirect_block_sec;
  };


void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  int numbers = *(int*)f->esp;
  // TODO: add function that detect wrong addr (pg fault) 
  switch(numbers)
  {
    case SYS_HALT: 
      halt();
      break;

    case SYS_EXIT:   
      addr_validation(f->esp+4, false);
      exit(*(int*)(f->esp+4));
      break;

    case SYS_EXEC:  
      addr_validation(f->esp+4, false);
      f->eax = exec(*(const char**)(f->esp+4));
      break;

    case SYS_WAIT:      
      addr_validation(f->esp+4, false);
      f->eax = wait(*(pid_t*)(f->esp+4));
      break;

    case SYS_CREATE:             
      addr_validation(f->esp+4, false);
      addr_validation(f->esp+8, false);
      f->eax = create(*(const char**)(f->esp + 4), *(unsigned*)(f->esp + 8));
      break;

    case SYS_REMOVE:     
      addr_validation(f->esp+4, false);
      f->eax = remove(*(const char**)(f->esp + 4));
      break;

    case SYS_OPEN:                
      addr_validation(f->esp+4, false);
      f->eax = open(*(const char**)(f->esp + 4));
      break;

    case SYS_FILESIZE: 
      addr_validation(f->esp+4,false);
      f->eax = filesize(*(int*)(f->esp + 4));
      break;

    case SYS_READ:             

      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      addr_validation(f->esp + 12, false);
      lock_acquire (&filesys_lock);
      f->eax = read(*(int*)(f->esp + 4), (void*)f->esp + 8, *(unsigned*)(f->esp + 12));
      lock_release (&filesys_lock);
      break;

    case SYS_WRITE: 
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      addr_validation(f->esp + 12, false);
      lock_acquire (&filesys_lock);
      f->eax = write(*(int*)(f->esp + 4), (void*)f->esp + 8, *(unsigned*)(f->esp + 12));
      lock_release (&filesys_lock);
      break;

    case SYS_SEEK:       
      addr_validation(f->esp+4, false);
      addr_validation(f->esp+8, false);
      seek(*(int*)(f->esp+4), *(unsigned*)(f->esp+8));
      break;

    case SYS_TELL:             
      addr_validation(f->esp+4, false);
      f->eax = tell(*(int*)(f->esp+4));
      break;

    case SYS_CLOSE:               
      addr_validation(f->esp+4, false);
      close(*(int*)(f->esp+4));
      break;

    case SYS_SIGACTION:             
      addr_validation(f->esp+4, false);
      addr_validation(f->esp+8, false);
      sigaction((int)*(uint32_t *)(f->esp+4),
		            *(sig_func **)(f->esp+8));
      break;

    case SYS_SENDSIG:                /* Send a signal 14*/
      addr_validation(f->esp+4, false);
      addr_validation(f->esp+8, false);
      sendsig((int)*(uint32_t *)(f->esp+4),
	            (int)*(uint32_t *)(f->esp+8));
      break;
      
    case SYS_YIELD:                  /* Yield current thread 15*/
      thread_yield();
      break;

    ////////////// file system call ////////////////
    case SYS_CHDIR:
      addr_validation(f->esp+4, false);
      f->eax = chdir(*(const char**)(f->esp+4));
      break;
    
    case SYS_MKDIR:
      addr_validation(f->esp+4, false);
      f->eax = mkdir(*(const char**)(f->esp+4));
      break;

    case SYS_READDIR:
      addr_validation(f->esp+4, false);
      addr_validation(f->esp+8, false);
      f->eax = readdir((int)*(uint32_t *)(f->esp+4), *(char**)(f->esp+8));
      break;

    case SYS_ISDIR: //fd 리스트에서 fd에 대한 file 정보 얻기
      addr_validation(f->esp+4, false);
      f->eax = isdir((int)*(uint32_t *)(f->esp+4));
      break;
    
    case SYS_INUMBER:
      addr_validation(f->esp+4, false);
      f->eax = inumber((int)*(uint32_t *)(f->esp+4));
      break;

  }
}


/* 1. Completed */
void 
halt(void)
{
  shutdown_power_off (); // shutdown pintos
}

/* 2. Completed */
void 
exit(int status)
{
  thread_current()->exit_status = status;
  printf("%s: exit(%d)\n", thread_name(), status);
  thread_exit();
}

pid_t 
exec(const char* cmd_line)
{
  tid_t tid = process_execute(cmd_line);
  return tid;
}

int 
wait(pid_t pid)
{
  return process_wait (pid);
}


/*============ file related system calls ==============*/
/* 3. Completed */
bool 
create(const char *file, unsigned initial_size)
{
  // file : name and path information for the file to create
  // initial_size : Size of file to create
  if (file == NULL)
  {
    exit(-1);
    return false;
  }
  // Create a file that corresponds to the file name and size
  return filesys_create(file, initial_size);
}

/* 4. Completed */
bool 
remove(const char *file)
{
  // Remove the file corresponding to the file name
  return filesys_remove(file);
}

int 
open(const char* file)
{
  if (file == NULL) 
    return -1;
  struct file* new_file = filesys_open(file);
  
  if (new_file == NULL) 
    return -1;
  
  thread_current()->fdt[thread_current()->next_fd] = new_file;
  
  int fd = thread_current()->next_fd;

  for (int i = 2; i < 128; i ++){
    if (thread_current()->fdt[i] == NULL){
      thread_current()->next_fd = i;
      break;
    }
  }
  return fd;
}

int 
filesize(int fd)
{
  struct file* file = thread_current()->fdt[fd];
  return file_length(file);
}

int 
read(int fd, void *buffer, unsigned size)
{
  if (fd == 0){
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    return input_getc(buf, size);
  }
  else{
    struct file* file = thread_current()->fdt[fd];
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    
    return file_read(file, buf, size);
  }
}

int 
write(int fd, const void *buffer, unsigned size)
{
  if (fd == 1){
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    putbuf(buf, size);
    return sizeof(buf);
  }
  else{
    struct file* file = thread_current()->fdt[fd];
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    return file_write(file, buf, size);
  }
}


void 
seek(int fd, unsigned position)
{
  struct file* file = thread_current()->fdt[fd];
  file_seek(file, position);
}

unsigned 
tell(int fd)
{
  struct file* file = thread_current()->fdt[fd];
  return file_tell(file);
}

void 
close(int fd)
{
  if(thread_current()->fdt[fd] == NULL) 
    return -1;
  struct file* file = thread_current()->fdt[fd];
  file_close(file);
  thread_current()->fdt[fd] = NULL;

  for (int i = 2; i < 128; i ++){
    if (thread_current()->fdt[i] == NULL){
      thread_current()->next_fd = i;
      break;
    }
  }
}


void 
sigaction(int signum, void(*handler)(void))
{
  thread_current()->parent_thread->sig_list[signum-1] = handler;
}


void 
sendsig(pid_t pid, int signum)
{
  struct thread *cur = thread_current();
  struct list_elem *e;
  for(e=list_begin(&cur->child_list);
      e!=list_end(&cur->child_list);e=list_next(e)){
    
    struct thread *child_t = list_entry(e, struct thread, child_elem);
    
    if(child_t->tid == pid){
      if(cur->sig_list[signum-1])
        printf("Signum: %d, Action: %p\n",signum, cur->sig_list[signum-1]);
    }
  }
}

void
addr_validation (void *esp, bool read_write)
{
  if (esp >= PHYS_BASE){
    if (read_write == true)
      lock_release(&filesys_lock);
    exit(-1);
  }
}

//////////////////////////////
/* process의 현재 작업 directory를 dir로 변경 */
bool chdir(const char *dir)
{
  if (strlen(dir)==0)
    return false;
  struct inode_disk disk_inode;
  // 현재 dir의 inode의 sector에서 만든 disk_inode로 읽어오기
  bc_read(inode_to_sector(thread_current()->current_dir->inode), &disk_inode, 0, 0, sizeof (struct inode_disk));
   
  // dir 만큼 할당하기
  char* dir_copy = malloc(sizeof(dir));
  strlcpy(dir_copy, dir, strlen(dir) + 1);


  // dir copy 이름에서 directory하고 dir_name 뽑아오기
  char *dir_name;
  struct dir* directory = parse_path(dir_copy, &dir_name);
  
  struct inode *inode = NULL;
  
  // 만약에 dir_name이 directory에 없다면
  if (!dir_lookup (directory, dir_name, &inode)){
    // 닫자
    dir_close(directory);
    free(dir_copy);
    return false;
  }
  // 만약에 있으면
  else {
    // inode가 아니면
    if (!inode_is_dir(inode)){
      // 닫자
      dir_close(directory);
      free(dir_copy);
      return false;
    }
    else {
      // 현재 dir 닫고, 위에서 dir_lookup으로 저장된 inode를 현재로 교체한다.
      dir_close(directory);
      dir_close(thread_current()->current_dir);
      thread_current()->current_dir = dir_open(inode);
    }
  }

  free(dir_copy);
  return true;
}

bool 
mkdir(const char *dir){
  block_sector_t inode_sector = 0;
  if (strlen(dir)==0) 
    return false;
  
  // name 경로 분석
  char* dir_copy = malloc(sizeof(dir));
  strlcpy(dir_copy, dir, strlen(dir) + 1);

  char *dir_name;
  struct dir* directory = parse_path(dir_copy, &dir_name);
  
  // bitmap에서 inode sector 번호 할당
  bool success = (directory != NULL
                  && free_map_allocate (1, &inode_sector)
                  && inode_create (inode_sector, 0, 1)
                  && dir_add (directory, dir_name, inode_sector));

  // 해당 inode_sector 열어서 새로운 dir로 오픈
  struct dir* new_dir = dir_open(inode_open(inode_sector));

  if(success){
    struct inode_disk disk_inode;
    // dir entry에 '.', '..' file entry 추가하기
    if (dir_add (new_dir, ".", inode_sector) && dir_add (new_dir, "..", inode_to_sector(directory->inode))) 
      success = true;
    else 
      success = false;
    dir_close (new_dir);
  }

  if (!success && inode_sector != 0) 
    free_map_release (inode_sector, 1);

  dir_close (directory);
  free(dir_copy);
  return success;
}

bool 
readdir(int fd, char *name)
{
  struct file* file = process_get_file(fd);
  bool success = false;

  if (!file) 
    return false;
  else {
    struct dir* dir = file;
    // '.', '..' 파일 외의 file을 변수에 저장
    while (dir_readdir (dir, name)){
      
      if (name[0] == '.') {
        if(strlen(name) == 1) 
          continue;
        else if (name[1] == '.'){
          if (strlen(name) == 2)
            continue;
        }
      }
      success = true;
      break;
    }
  }
  return success;
}


// Completed
/* inode의 directory 여부 판단 */
bool
isdir(int fd){
  // fd list에서 fd에 대한 정보를 얻기
  struct file *f = process_get_file(fd);
  if(f==NULL)
    exit(-1);
  // fd의 in-memory inode가 directory인지 여부 return
  // true : directory, false : file
  return inode_is_dir(file_get_inode(f));
}

// Completed
/* fd와 관련된 file 또는 directory의 inode number를 return */
int
inumber(int fd){
  // file descriptor를 이용하여 file 찾기
  struct file *f=process_get_file(fd);
  if(f==NULL)
    exit(-1);
  // fd와 관련된 file or directory의 inode number를 return
  return inode_get_inumber(file_get_inode(f));
}
