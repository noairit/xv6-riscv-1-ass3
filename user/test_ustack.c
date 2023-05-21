#include "kernel/types.h"
#include "user/user.h"
#include "ustack.h"

int main (){
   
    void* str = ustack_malloc(40);
    printf("%p\n", str);
    void* str2 = ustack_malloc(20);
    printf("%p\n", str2);
    int count = ustack_free();
    printf("%d",count);



    return 0;
}