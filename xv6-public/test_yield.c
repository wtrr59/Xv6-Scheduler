#include "types.h"
#include "stat.h"
#include "user.h"

int main(){
    
    int p;
    int i = 0;
    printf(1,"%d\n",cpu_share(20));

    p = fork();

    while(1){
        
        if(p < 0){
            exit();
        }else if(p == 0){
            printf(1, "child %d\n",i++);
            run_MLFQ();
            my_yield();
        }else{
            printf(1, "parent %d\n",i++);
            if(p <= 5){
                p = fork();
                printf(1, "fork\n");
            }
            my_yield();
        }
    }
    exit();
}
