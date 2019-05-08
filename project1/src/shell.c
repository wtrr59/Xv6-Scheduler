//
//  @Author Huijung Woo
//
//  @Created by Ubuntu on 28/03/2019.
//  @Copyright © 2019 Ubuntu. All rights reserved.
//


#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/wait.h>

#define BUF_SIZE 512
#define COMMAND_SIZE 4096

int Parse_with_semi(char *buf, char * command_line[], int count);
int Parse_with_space(char *command_line, char * command_list[]);


int main(int argc, const char * argv[]) {
    
    FILE* fp;
    
    //buf : fgets 시 명령어를 입력받는 버퍼
    //command_show : Batch Mode에서 명령어 라인을 보여주기 위해 buf를 그대로 저장
    //command_line : 세미콜론 단위로 parsing을 해서 하나의 명령어 단위로 저장
    //               ex) ls -al ; pwd ; cat file -> 1) ls -al 2) pwd 3) cat file
    //command_list : 공백단위로 parsing을 해서 배열을 만듬  ex) 1) ls 2) -al
    
    char buf[BUF_SIZE];
    char *command_line[COMMAND_SIZE];
    char *command_show[COMMAND_SIZE];
    char *command_list[COMMAND_SIZE];
    
    //명령어 배열들을 관리하기 위한 index들
    int command_count = 0;
    int null_index = 0;
    int i = 0;
    
    pid_t cur_pid;
    
    
    //2가지 배열 동적할당
    
    for (i = 0; i < COMMAND_SIZE; i++) {
        command_line[i] = (char*)malloc(sizeof(char)*COMMAND_SIZE);
        if (command_line[i] == NULL) {
            printf("malloc error\n");
            return 2;
        }
    }
    
    for (i = 0; i < COMMAND_SIZE; i++) {
        command_show[i] = (char*)malloc(sizeof(char)*COMMAND_SIZE);
        if (command_show[i] == NULL) {
            printf("malloc error\n");
            return 2;
        }
    }
    
    
    i = 0;
    
    
    //Interactive Mode
    
    if (argc == 1) {
        fp = stdin;   //파일 포인터에 표준입력을 저장
        
        printf("prompt> ");
        
        while (fgets(buf, sizeof(buf), fp) != NULL) {
            if (strcmp(buf, "quit\n") == 0) {
                return 0;
            }
            
            //세미콜론을 기준으로 parsing하여 배열에 저장
            //command_count에 총 명령어 개수를 저장
            
            command_count = Parse_with_semi(buf, command_line, command_count);
            
            for (i = 0; i < command_count; i++) {
                
                //명령어를 한줄씩 넣어서 공백을 기준으로 parsing 후 배열에 저장
                //null_index에 개수 저장
                
                null_index = Parse_with_space(command_line[i], command_list);
                
                //만약 명령어 한줄이 공백하나일 경우 뛰어 넘음
                if (null_index == 0 || strcmp(command_list[0],"\n") == 0){
                    continue;
                }
                
                //execvp를 실행하기 위해 마지막에 NULL 포인터를 넣어줌
                command_list[null_index] = NULL;
                
                
                //자식프로세스와 부모프로세스로 나눔
                cur_pid = fork();
                
                if (cur_pid < 0) {
                    printf("fork() error\n");
                    exit(0);
                    
                //부모프로세스일 경우 wait
                } else if (cur_pid > 0) {
                    wait(NULL);
                    
                //자식프로세스일 경우 execvp 후 실행이 안될경우 에러메세지 후 종료
                } else {
                    execvp(command_list[0], command_list);
                    printf("%s is wrong command\n",command_list[0]);
                    exit(0);
                }
                
            }
            
            //파일 포인터 플러쉬 및 command 인덱스 초기화
            fflush(fp);
            command_count = 0;
            printf("prompt> ");
        }
        
    //Batch Mode
        
    } else if(argc == 2) {
        
        //batch 파일 내의 명령어 라인 한줄의 인덱스를 저장할 배열과 인덱스
        int line_cut_arr[COMMAND_SIZE+1];
        int line_cut_index = 0;
        
        //batch 파일 open
        fp = fopen(argv[1], "r");
        if (fp == NULL){
            printf("File is not exist\n");
            return 2;
        }

        
        //fgets를 통해 명령어 라인 한줄 한줄 읽기
        //batch 파일을 모두 읽은 후 한번에 명령어 실행
        while (fgets(buf, sizeof(buf), fp) != NULL) {
            
            //만약 그냥 줄바꿈 문자가 있을 경우 뛰어넘음
            if (strcmp(buf,"\n") == 0) {
                continue;
            }
            
            //명령어 라인 한줄 저장 및 시작 인덱스 저장
            // ex)
            // (0)ls -al ; (1)pwd ; (2)cat file    line_cut_arr[0] = 0
            //                                     command_show[0] = "ls -al ; pwd ; cat file"
            // (3)ls                               line_cut_arr[1] = 3
            //                                     command_show[1] = "ls"
            
            strcpy(command_show[line_cut_index], buf);
            line_cut_arr[line_cut_index++] = command_count;
            
            //세미콜론을 기준으로 명령어 라인 한줄을 자르기
            //명령어 라인 개수 command_count 에 저장
            //ex) command_line[0] = ls -al
            //    command_line[1] = pwd
            //    command_line[2] = cat file
            
            command_count = Parse_with_semi(buf, command_line, command_count);
        }
        
        //명령어가 저장되는 마지막 인덱스 저장 후 인덱스 0으로 초기화
        //ex) line_cut_arr[2] = 3
        
        line_cut_arr[line_cut_index++] = command_count;
        line_cut_index = 0;
        
        
        //명령어 라인 개수만큼 반복문 실행
        
        for (i = 0; i < command_count; i++) {
            
            //공백을 기준으로 parse
            //ex) ls -al -> command_list[0] = ls , command_list[1] = -al
            //null_index -> 공백을 기준으로 나뉜 명령어 개수 리턴받아 후에 NULL 포인터를 입력
            
            null_index = Parse_with_space(command_line[i], command_list);
            
            
            //만약 공백이 들어오거나 줄바꿈 문자가 들어오면 뛰어넘음
            
            if (null_index == 0 || strcmp(command_list[0],"\n") == 0) {
                continue;
            }
            
            
            //현재 명령어 인덱스와 명령어라인 시작 인덱스가 같으면 실행할 명령어 라인을 출력
            
            if (line_cut_arr[line_cut_index] == i) {
                printf("%s\n",strtok(command_show[line_cut_index++],"\n"));
            }
            
            
            //execvp 실행을 위해 NULL 포인터 저장
            
            command_list[null_index] = NULL;
            
            
            //자식 프로세스와 부모 프로세스로 나눔
            
            cur_pid = fork();
            
            if (cur_pid < 0){
                printf("fork() error\n");
                exit(0);
                
                
            //부모 프로세스일 경우 wait
                
            } else if (cur_pid > 0)
                wait(NULL);
            
            
            //자식프로세스일 경우 execvp 후 실행이 안될경우 에러메세지 후 종료
            
            else {
                execvp(command_list[0], command_list);
                printf("[%s is wrong command]\n",command_list[0]);
                exit(0);
            }
        }
        
        //파일 포인터 close
        fclose(fp);
        
    } else {
        printf("only one batch file\n");
        return 1;
    }
    
    for (i = 0; i < COMMAND_SIZE; i++) {
        free(command_line[i]);
        free(command_show[i]);
    }
    
    return 0;
}

/**
 *  세미콜론을 기준으로 파싱을 하기위해 사용
 *  @buf[in]      fgets를 통해 입력받은 명령어 한줄
 *  @command_line[out]     명령어들을 세미콜론으로 파싱하여 저장
 *  @count[in,out]  현재 명령어 개수를 입력받음
 *  @return         세미콜론으로 인해 나뉘어진 명령어 수를 리턴
 */
int Parse_with_semi(char *buf, char * command_line[], int count){
    
    char *ptr = strtok(buf, ";");
    
    while (ptr != NULL){
        if (strcmp(ptr, ";") != 0) {
            strcpy(command_line[count++],ptr);
        }
        
        ptr = strtok(NULL, ";");
    }
    
    ptr = strtok(command_line[count-1], "\n");
    if (ptr != NULL) {
        command_line[count-1] = ptr;
    }
    
    return count;
}

/**
 *  공백을 기준으로 파싱을 하기위해 사용
 *  @command_line[in]      명령어 한줄 [ex) ls -al] 을 갖고 있는 스트링 배열
 *  @command_list[out]     명령어들을 공백으로 파싱하여 저장
 *  @return         공백으로 인해 나뉘어진 명령어 수를 리턴
 */

int Parse_with_space(char *command_line, char * command_list[]){
    
    int count = 0;
    
    char *ptr = strtok(command_line, " ");
    
    // 세미콜론을 기준으로 파싱
    while (ptr != NULL){
        if (strcmp(ptr, " ") != 0) {
            command_list[count++] = ptr;
        }
        
        ptr = strtok(NULL, " ");
    }
    
    return count;
}
