#include "types.h"
#include "defs.h"

void my_yield(void){
    yield();
}

void sys_my_yield(void){
    my_yield();
}
