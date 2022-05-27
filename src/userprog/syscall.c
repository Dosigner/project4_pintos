#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"


/* Additional */
#include <list.h>
#include "threads/vaddr.h"
#include "threads/synch.h"
#include "filesys/filesys.h"     // filesys_OOO()
#include "filesys/off_t.h"
#include "filesys/file.h"
#include "devices/shutdown.h" // shutdown_power_off()
#include "userprog/process.h"


typedef void sig_func(void);

struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
  lock_init (&filesys_lock);
}

static void
syscall_handler (struct intr_frame *f UNUSED) 
{
  /* get stack pointer from intr_frame */

  /* get system call number from stack */
  int numbers = *(int*)f->esp;
  // TODO: add function that detect wrong addr (pg fault) 
  switch(numbers)
  {
    case SYS_HALT:                   /* Halt the operating system. 0*/
      // no argument
      halt();
      break;

    case SYS_EXIT:                   /* Terminate this process. 1*/
      // one argument
      addr_validation(f->esp + 4, false);
      exit(*(int*)(f->esp + 4));
      break;

    case SYS_EXEC:                   /* Start another process. 2*/
      // one argument
      addr_validation(f->esp + 4, false);
      f->eax = exec(*(const char**)(f->esp + 4));
      break;

    case SYS_WAIT:                   /* Wait for a child process to die. 3*/
      // one argument
      addr_validation(f->esp + 4, false);
      f->eax = wait(*(pid_t*)(f->esp + 4));
      break;

    case SYS_CREATE:                 /* Create a file. 4*/
      // 2 argument
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      f->eax = create(*(const char**)(f->esp + 4), *(unsigned*)(f->esp + 8));
      break;

    case SYS_REMOVE:                 /* Delete a file. 5*/
      // 1 argument
      addr_validation(f->esp + 4, false);
      f->eax = remove(*(const char**)(f->esp + 4));
      break;

    case SYS_OPEN:                   /* Open a file. 6*/
      // 1 argument
      addr_validation(f->esp + 4, false);
      f->eax = open(*(const char**)(f->esp + 4));
      break;

    case SYS_FILESIZE:               /* Obtain a file's size. 7*/
      // 1 argument
      addr_validation(f->esp + 4,false);
      f->eax = filesize(*(int*)(f->esp + 4));
      break;

    case SYS_READ:                   /* Read from a file. 8*/
      // 3 argument
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      addr_validation(f->esp + 12, false);
      lock_acquire (&filesys_lock);
      f->eax = read(*(int*)(f->esp + 4), (void*)f->esp + 8, *(unsigned*)(f->esp + 12));
      lock_release (&filesys_lock);
      break;

    case SYS_WRITE:                  /* Write to a file. 9*/
      // 3 argument
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      addr_validation(f->esp + 12, false);
      lock_acquire (&filesys_lock);
      f->eax = write(*(int*)(f->esp + 4), (void*)f->esp + 8, *(unsigned*)(f->esp + 12));
      lock_release (&filesys_lock);
      break;

    case SYS_SEEK:                   /* Change position in a file. 10*/
      // 2 argument
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      seek(*(int*)(f->esp + 4), *(unsigned*)(f->esp + 8));
      break;

    case SYS_TELL:                   /* Report current position in a file. 11*/
      // 1 argument
      addr_validation(f->esp + 4, false);
      f->eax = tell(*(int*)(f->esp + 4));
      break;

    case SYS_CLOSE:                  /* Close a file. 12*/
      // 1 argument
      addr_validation(f->esp + 4, false);
      close(*(int*)(f->esp + 4));
      break;

    case SYS_SIGACTION:              /* Register an signal handler 13*/
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      sigaction((int)*(uint32_t *)(f->esp+4),
		            *(sig_func **)(f->esp+8));
      break;

    case SYS_SENDSIG:                /* Send a signal 14*/
      addr_validation(f->esp + 4, false);
      addr_validation(f->esp + 8, false);
      sendsig((int)*(uint32_t *)(f->esp+4),
	            (int)*(uint32_t *)(f->esp+8));
      break;
      
    case SYS_YIELD:                  /* Yield current thread 15*/
      thread_yield();
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
  /* exit_status : 
     stores the status of the thread just before the current exit */
  thread_current()->exit_status = status;
  printf("%s: exit(%d)\n", thread_name(), status);
  thread_exit();
}

pid_t 
exec(const char* cmd_line)
{
  // create child process and execute program corresponds to cmd line
  tid_t tid = process_execute(cmd_line);
  return tid;
}

int 
wait(pid_t pid)
{
  // wait for termination of child process pid & retrieve child's exit status
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

int open(const char* file)
{
  if (file == NULL) return -1;
  struct file* new_file = filesys_open(file);
  if (new_file == NULL) return -1;
  thread_current()->fdt[thread_current()->next_fd] = new_file;
  int fd = thread_current()->next_fd;
  for (int i = 2; i < 128; i ++)
  {
    if (thread_current()->fdt[i] == NULL) 
    {
      thread_current()->next_fd = i;
      break;
    }
  }
  return fd;
}
int filesize(int fd)
{
  struct file* file = thread_current()->fdt[fd];
  return file_length(file);
}
int read(int fd, void *buffer, unsigned size)
{
  if (fd == 0)
  {
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    return input_getc(buf, size);
  }
  else
  {
    struct file* file = thread_current()->fdt[fd];
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    return file_read(file, buf, size);
  }
}
int write(int fd, const void *buffer, unsigned size)
{
  if (fd == 1)
  {
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    putbuf(buf, size);
    return sizeof(buf);
  }
  else
  {
    struct file* file = thread_current()->fdt[fd];
    const char* buf = *(char**)buffer;
    addr_validation(buf, true);
    return file_write(file, buf, size);
  }
}
void seek(int fd, unsigned position)
{
  struct file* file = thread_current()->fdt[fd];
  file_seek(file, position);
}
unsigned tell(int fd)
{
  struct file* file = thread_current()->fdt[fd];
  return file_tell(file);
}
void close(int fd)
{
  if(thread_current()->fdt[fd] == NULL) return -1;
  struct file* file = thread_current()->fdt[fd];
  file_close(file);
  thread_current()->fdt[fd] = NULL;
  for (int i = 2; i < 128; i ++)
  {
    if (thread_current()->fdt[i] == NULL) 
    {
      thread_current()->next_fd = i;
      break;
    }
  }
}
typedef void sig_func (void);



// child-sig
void sigaction(int signum, void(*handler)(void))
{
  // register handler to parent;
  // so parent process(sendsig()) know handler
  thread_current()->parent_thread->sig_list[signum-1] = handler;
}

/* Signal part */
void sendsig(pid_t pid, int signum)
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
  if (esp >= PHYS_BASE) 
  {
    if (read_write == true) lock_release(&filesys_lock);
    exit(-1);
  }
}