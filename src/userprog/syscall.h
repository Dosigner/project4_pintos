#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H
#include <stdbool.h>

typedef int pid_t;

void syscall_init (void);

////////////// process related system calls //////////////
void halt(void);
void exit(int);
pid_t exec(const char *);
int wait(pid_t);
///////////////////////////////////////////////////////////


//////////////// file related system calls ////////////////
bool create(const char*, unsigned);
bool remove(const char*);
int open(const char*);
int filesize(int);
int read(int, void*, unsigned);
int write(int, const void*, unsigned);
void seek(int, unsigned);
unsigned tell(int);
void close(int);
////////////////////////////////////////////////////////////

//////////////////////////
bool chdir (const char *old_path);
bool mkdir (const char *dir);
bool readdir (int fd, char *name);
bool isdir (int fd);
int inumber (int fd);

#endif /* userprog/syscall.h */
