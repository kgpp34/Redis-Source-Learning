#ifndef __AE_EPOLL_H_
#define __AE_EPOLL_H_
#include "ae.h"
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>

//
// aeApiState 事件状态
//
typedef struct aeApiState
{
	// epoll_event 实例描述符
	int epfd;
	// 事件槽
	struct epoll_event *events; // epoll_events貌似是系统的一个结构

	/**
	 * @brief 
	 * typedef union epoll_data {
			void *ptr;
			int fd;
			__uint32_t u32;
			__uint64_t u64;
		} epoll_data_t;

		struct epoll_event {
			__uint32_t events; /* Epoll events /
			epoll_data_t data; / User data variable /
		};
	 * 
	 */

} aeApiState;

int aeApiPoll(aeEventLoop *eventLoop, struct timeval *tvp);
int aeApiCreate(aeEventLoop *eventLoop);
int aeApiResize(aeEventLoop *eventLoop, int setsize);
void aeApiFree(aeEventLoop *eventLoop);
int aeApiAddEvent(aeEventLoop *eventLoop, int fd, int mask);
void aeApiDelEvent(aeEventLoop *eventLoop, int fd, int delmask);
char *aeApiName(void);
#endif
