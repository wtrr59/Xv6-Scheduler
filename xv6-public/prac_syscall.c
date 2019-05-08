#include "types.h"
#include "defs.h"

int my_syscall(char *str){
	cprintf("%s\n",str);
	return 0xABCDABCD;
}

int sys_my_syscall(void){
	char *str;
	if(argstr(0, &str) < 0)
		return -1;
	return my_syscall(str);
}
