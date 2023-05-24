#include "kernel/types.h"
#include "user/user.h"

#define PGSIZE 4096
#include "kernel/param.h"
//#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"
#include "kernel/syscall.h"
#include "kernel/memlayout.h"
#include "kernel/riscv.h"

char * memoryarray[22];
void test1()
{
    int pid;
    if((pid=fork()) == 0)    {
        char *mem = sbrk(20 * PGSIZE);
        mem[5000] = 0;
        mem[4069 * 20] = 0;
        sleep(100);
        exit(0);
    }
    wait(0);
   printf("--------- Test1 finished ---------\n");
}

void test2()
{
  if(fork()!=0){
    printf("we in parent process\n");
    wait(0); // we should wait the child to finish
  }
  else
  {
    printf("we in child process\n");
  }
   printf("--------- Test2 finished ---------\n");
}
//copying pages into children
void test3(){
  
    if (fork() == 0)
    {
        sbrk(20*4096);
        if (fork() == 0)
        {
            if (fork() == 0)
            {
                if (fork() == 0)
            wait(0);
                exit(0);
            }
            wait(0);
            exit(0);
        }
        wait(0);
        exit(0);
    }
    wait(0);
   printf("--------- Test3 finished ---------\n");
}

char in[3];
int *pages[18];

int main(int argc, char *argv[])
{
  printf("--------- Test1 Started ---------\n");
   test1();
  printf("--------- Test2 Started ---------\n");
   test2();
   printf("--------- Test3 Started ---------\n");
   test3();
    exit(0);
}