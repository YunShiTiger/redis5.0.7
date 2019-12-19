/*
 * 处理父子进程之间管道通信方面的方法
 */

#include "server.h"
#include <unistd.h>

/* Open a child-parent channel used in order to move information about the RDB / AOF saving process from the child to the parent (for instance the amount of copy on write memory used) */
/* 开启父子进程进行通信的管道 */
void openChildInfoPipe(void) {
    if (pipe(server.child_info_pipe) == -1) {
        /* On error our two file descriptors should be still set to -1, but we call anyway cloesChildInfoPipe() since can't hurt. */
        closeChildInfoPipe();
    } else if (anetNonBlock(NULL,server.child_info_pipe[0]) != ANET_OK) {
        closeChildInfoPipe();
    } else {
        memset(&server.child_info_data,0,sizeof(server.child_info_data));
    }
}

/* Close the pipes opened with openChildInfoPipe(). */
/* 关闭父子进程进行通信的管道 */
void closeChildInfoPipe(void) {
    if (server.child_info_pipe[0] != -1 || server.child_info_pipe[1] != -1) {
        close(server.child_info_pipe[0]);
        close(server.child_info_pipe[1]);
        server.child_info_pipe[0] = -1;
        server.child_info_pipe[1] = -1;
    }
}

/* Send COW data to parent. The child should call this function after populating the corresponding fields it want to sent (according to the process type). */
/* 子进程通过管道给父进程发送消息 */
void sendChildInfo(int ptype) {
	//检测子进程端管道是否正常
    if (server.child_info_pipe[1] == -1) 
		return;
	//配置需要发送想信息数据
    server.child_info_data.magic = CHILD_INFO_MAGIC;
    server.child_info_data.process_type = ptype;
	//获取对应的需要发送的字节数量
    ssize_t wlen = sizeof(server.child_info_data);
	//通过管道向父进程发送信息处理
    if (write(server.child_info_pipe[1],&server.child_info_data,wlen) != wlen) {
        /* Nothing to do on error, this will be detected by the other side. */
    }
}

/* Receive COW data from parent. */
/* 父进程通过管道获取子进程发送的消息 */
void receiveChildInfo(void) {
	//检测父进程端管道是否正常
    if (server.child_info_pipe[0] == -1) 
		return;
	//配置需要获取的字节数量
    ssize_t wlen = sizeof(server.child_info_data);
	//通过管道读取对应字节的数据
    if (read(server.child_info_pipe[0],&server.child_info_data,wlen) == wlen && server.child_info_data.magic == CHILD_INFO_MAGIC) {
		//解析子进程给父进程发送的信息
		if (server.child_info_data.process_type == CHILD_INFO_TYPE_RDB) {
            server.stat_rdb_cow_bytes = server.child_info_data.cow_size;
        } else if (server.child_info_data.process_type == CHILD_INFO_TYPE_AOF) {
            server.stat_aof_cow_bytes = server.child_info_data.cow_size;
        }
    }
}



