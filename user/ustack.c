#include "ustack.h"


#define MAX_ALLOCATION_SIZE 512



 struct header {
    uint len;
    uint dealloc_page;
    struct header* prev;
} ;


static struct header *base = 0; // null 
static struct header *top_header = 0;
static char *top_page; 
static char* top_stack;

//static header *freep ;


void* ustack_malloc(uint len){
   
    char *p;
    struct header *hp;

    
    if (len > MAX_ALLOCATION_SIZE){
        return (void*)-1; // Exceeded maximum allowed size
    }

    if(top_header == base){ //initialization
        p = sbrk(PGSIZE);
        if(p == (char*)-1)
            return (void*)-1;
        top_page = p + PGSIZE;    
        hp = (struct header*)p;
        hp->len = len;
        hp->prev = base;
        top_stack =(char*) hp + len;
        top_header = hp;
        return (void*)top_stack;
    }

    if (top_page - top_stack < len)
    {
        p = sbrk(PGSIZE);
        if(p == (char*)-1)
            return (void*)-1;
        top_page = p + PGSIZE;    
        hp = (struct header*)p;
        hp->len = len;
        hp->prev = top_header;
        hp->dealloc_page = 1;
        top_stack = (char*)hp + len;
        top_header = hp;

        return (void*)top_stack; // we need more space
    }

    else{
        hp = (struct header*)(top_stack );
        hp->len = len;
        hp->prev = top_header;
        top_stack = (char*)hp + len;
        top_header = hp;
        return (void*)top_stack;
    } 
}



int ustack_free(void){
    char *p;
    int top_size = top_header->len;
    top_header = top_header->prev;
    if (top_header->dealloc_page == 1)
    {
        p = sbrk(-PGSIZE);
        if(p == (char*)-1)
            return -1;
        top_page = top_page - PGSIZE;
    
       
    }
    return top_size; 

}