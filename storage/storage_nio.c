/**
* Copyright (C) 2008 Happy Fish / YuQing
*
* FastDFS may be copied only under the terms of the GNU General
* Public License V3, which may be found in the FastDFS source kit.
* Please visit the FastDFS Home Page http://www.csource.org/ for more detail.
**/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include "shared_func.h"
#include "sched_thread.h"
#include "logger.h"
#include "sockopt.h"
#include "fast_task_queue.h"
#include "tracker_types.h"
#include "tracker_proto.h"
#include "storage_global.h"
#include "storage_service.h"
#include "ioevent_loop.h"
#include "storage_dio.h"
#include "storage_nio.h"

static void client_sock_read(int sock, short event, void *arg);
static void client_sock_write(int sock, short event, void *arg);
static void client_sock_write_ex(int sock, short event, void *arg);
static int storage_nio_init(struct fast_task_info *pTask);

void add_to_deleted_list(struct fast_task_info *pTask)
{
	((StorageClientInfo *)pTask->arg)->canceled = true;
	pTask->next = pTask->thread_data->deleted_list;
	pTask->thread_data->deleted_list = pTask;
}

//add by yb 20170825 start
static int storage_cmp_by_client_id(const void *p1, const void *p2)
{
	if(strcmp((*((FDFSClientInfo **)p1))->ip_addr,(*((FDFSClientInfo **)p1))->ip_addr) == 0\
			&& ((*((FDFSClientInfo **)p1))->port == (*((FDFSClientInfo **)p1))->port))
	{
		return 0;
	}
	else
	{
	    return -1;
	}
}


static int storage_remove_client(FDFSStorCliManager *pStorCliManager, FDFSClientInfo *pClient)
{
	FDFSClientInfo **ppEnd;
	FDFSClientInfo **pp;

	ppEnd = pStorCliManager->clients + pStorCliManager->current_count;
    for (pp=pStorCliManager->clients; pp<ppEnd; pp++)
    {
        if (*pp == pClient)
        {
            break;
        }
    }

    for (pp=pp + 1; pp<ppEnd; pp++)
    {
        *(pp - 1) = *pp;
    }

    *(pp - 1) = pClient;

    ppEnd = pStorCliManager->sorted_clients + pStorCliManager->current_count;
	for (pp=pStorCliManager->sorted_clients; pp<ppEnd; pp++)
	{
		if (*pp == pClient)
		{
			break;
		}
	}

	 for (pp=pp + 1; pp<ppEnd; pp++)
	{
		*(pp - 1) = *pp;
	}

	*(pp - 1) = pClient;

    memset(pClient,0,sizeof(FDFSClientInfo));
    pStorCliManager->current_count--;

    return 0;
}

static FDFSClientInfo* tracker_mem_get_client(FDFSStorCliManager *pStorCliManager\
		,const char *ip,const int port)
{
	FDFSClientInfo target_cli;
	FDFSClientInfo **pCliStor;
	FDFSClientInfo *pTarget_cli;
	memset(&target_cli,0,sizeof(FDFSClientInfo));
	strcpy(target_cli.ip_addr,ip);
	target_cli.port = port;
	pTarget_cli = &target_cli;
	pCliStor = (FDFSClientInfo**)bsearch(&pTarget_cli,\
			pStorCliManager->sorted_clients,pStorCliManager->current_count\
			,sizeof(FDFSClientInfo*),storage_cmp_by_client_id);

	if(pCliStor != NULL)
	{
		return *pCliStor;
	}
	else
	{
		return NULL;
	}
}

//end
void task_finish_clean_up(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pClientInfo->clean_func != NULL)
	{
		pClientInfo->clean_func(pTask);
	}

    //add by yb 20170816 start
	int nClientPort;
	FDFSClientInfo *pFound;
	int result;
	getPeerIpport(pTask->event.fd,&nClientPort);

	if ((result=pthread_mutex_lock(&mem_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_lock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));

	}

	pFound = tracker_mem_get_client(&g_stor_cli_manager,pTask->client_ip,nClientPort);
	if(pFound != NULL)
	{
		if(storage_remove_client(&g_stor_cli_manager,pFound) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
							"storage clients remove  %s:%d failed.", \
							__LINE__,pTask->client_ip,nClientPort);
		}
		else
		{
	        ++g_stat_storage_client_connect_chg_count;
		}
	}
	else
	{
		 logDebug("file: "__FILE__", line: %d, " \
		            "storage clients not find %s:%d", \
		            __LINE__,pTask->client_ip,nClientPort);
	}

	if ((result=pthread_mutex_unlock(&mem_thread_lock)) != 0)
	{
		logError("file: "__FILE__", line: %d, " \
			"call pthread_mutex_unlock fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
	}
    //end

	ioevent_detach(&pTask->thread_data->ev_puller, pTask->event.fd);
	close(pTask->event.fd);
	pTask->event.fd = -1;

	if (pTask->event.timer.expires > 0)
	{
		fast_timer_remove(&pTask->thread_data->timer,
			&pTask->event.timer);
		pTask->event.timer.expires = 0;
	}

	memset(pTask->arg, 0, sizeof(StorageClientInfo));
	free_queue_push(pTask);

    __sync_fetch_and_sub(&g_storage_stat.connection.current_count, 1);
    ++g_stat_change_count;
}

static int set_recv_event(struct fast_task_info *pTask)
{
	int result;

	if (pTask->event.callback == client_sock_read)
	{
		return 0;
	}

	pTask->event.callback = client_sock_read;
	if (ioevent_modify(&pTask->thread_data->ev_puller,
		pTask->event.fd, IOEVENT_READ, pTask) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		add_to_deleted_list(pTask);

		logError("file: "__FILE__", line: %d, "\
			"ioevent_modify fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}
	return 0;
}

static int set_send_event(struct fast_task_info *pTask)
{
	int result;

	if (pTask->event.callback == client_sock_write)
	{
		return 0;
	}

	pTask->event.callback = client_sock_write;
	if (ioevent_modify(&pTask->thread_data->ev_puller,
		pTask->event.fd, IOEVENT_WRITE, pTask) != 0)
	{
		result = errno != 0 ? errno : ENOENT;
		add_to_deleted_list(pTask);

		logError("file: "__FILE__", line: %d, "\
			"ioevent_modify fail, " \
			"errno: %d, error info: %s", \
			__LINE__, result, STRERROR(result));
		return result;
	}
	return 0;
}

void storage_recv_notify_read(int sock, short event, void *arg)
{
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	long task_addr;
	int64_t remain_bytes;
	int bytes;
	int result;

	//进入死循环，不断处理客户端或tracker来的请求，并处理请求
	while (1)
	{
		//从可读管道描述符中读取信息
		if ((bytes=read(sock, &task_addr, sizeof(task_addr))) < 0)
		{
			if (!(errno == EAGAIN || errno == EWOULDBLOCK))
			{
				logError("file: "__FILE__", line: %d, " \
					"call read failed, " \
					"errno: %d, error info: %s", \
					__LINE__, errno, STRERROR(errno));
			}

			break;
		}
		else if (bytes == 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"call read failed, end of file", __LINE__);
			break;
		}

		 //管道写入的是一个fast_task_info 结构的信息
		 //先把信息格式化
		pTask = (struct fast_task_info *)task_addr;
		pClientInfo = (StorageClientInfo *)pTask->arg;
		pFileContext = &(pClientInfo->file_context);

		if (pTask->event.fd < 0)  //quit flag
		{
			return;
		}

		logDebug("[storage_recv_notify_read] pClientInfo->stage=%d", pClientInfo->stage);		//10 1010

		if (pClientInfo->stage & FDFS_STORAGE_STAGE_DIO_THREAD)
		{
			pClientInfo->stage &= ~FDFS_STORAGE_STAGE_DIO_THREAD;		//1010 & 0111 = 0010	FDFS_STORAGE_STAGE_NIO_SEND
		}

		//根据客户端的状态进行处理
		switch (pClientInfo->stage)
		{
		    //初始化状态,此状态是由在storage_accept_loop函数中和客户端建立tcp连接后，初始化的。
			case FDFS_STORAGE_STAGE_NIO_INIT:
				result = storage_nio_init(pTask);
				break;
			//完成FDFS_STORAGE_STAGE_NIO_INIT这个阶段后，进入接收阶段
			case FDFS_STORAGE_STAGE_NIO_RECV:
				 //把目前的偏移量设置为0
				pTask->offset = 0;
				//接收的总长度是total_length-total_offset总长度减去总偏移量
				remain_bytes = pClientInfo->total_length - \
					       pClientInfo->total_offset;
				//pTask->length 是数据长度，为剩余的字节数和pTask->size中的两者中最大的数据
				if (remain_bytes > pTask->size)
				{
					pTask->length = pTask->size;
				}
				else
				{
					pTask->length = remain_bytes;
				}

				if (set_recv_event(pTask) == 0)
				{
					 //从pClientInfo->sock中读取数据并处理相应的命令
					client_sock_read(pTask->event.fd,
						IOEVENT_READ, pTask);
				}
				result = 0;
				break;
			case FDFS_STORAGE_STAGE_NIO_SEND:
				
				logDebug("[FDFS_STORAGE_STAGE_NIO_SEND] op=%d, start=%d, end=%d, fd=%d, open_flags=%d", \
					pFileContext->op, pFileContext->start, pFileContext->end, pFileContext->fd, pFileContext->open_flags);
				if(pFileContext->op == FDFS_STORAGE_FILE_OP_SENDFILE)
					result = storage_send_add_event_ex(pTask);
				else
					result = storage_send_add_event(pTask);
				break;
			case FDFS_STORAGE_STAGE_NIO_CLOSE:
				result = EIO;   //close this socket
				break;
			default:
				logError("file: "__FILE__", line: %d, " \
					"invalid stage: %d", __LINE__, \
					pClientInfo->stage);
				result = EINVAL;
				break;
		}

		if (result != 0)
		{
			add_to_deleted_list(pTask);
		}
	}
}

//该初始化函数主要是把客户端和storage服务端的已建立成功的socket描述符添加到事件监控队列中。
//用来接收并处理客户端的请求。并设置读、写事件处理函数。
static int storage_nio_init(struct fast_task_info *pTask)
{
	StorageClientInfo *pClientInfo;
	struct storage_nio_thread_data *pThreadData;

	pClientInfo = (StorageClientInfo *)pTask->arg;
	pThreadData = g_nio_thread_data + pClientInfo->nio_thread_index;

	pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_RECV;
	return ioevent_set(pTask, &pThreadData->thread_data,
			pTask->event.fd, IOEVENT_READ, client_sock_read,
			g_fdfs_network_timeout);
}

int storage_send_add_event(struct fast_task_info *pTask)
{
	pTask->offset = 0;

	/* direct send */
	client_sock_write(pTask->event.fd, IOEVENT_WRITE, pTask);
	/*client_sock_write_ex(pTask->event.fd, IOEVENT_WRITE, pTask);*/

	return 0;
}

/**add by xiaods start 20190814**/
int storage_send_add_event_ex(struct fast_task_info *pTask)
{
	pTask->offset = 0;

	/* direct send */
	client_sock_write_ex(pTask->event.fd, IOEVENT_WRITE, pTask);

	return 0;
}
/**add by xiaods end 20190814**/

static void client_sock_read(int sock, short event, void *arg)
{
	int bytes;
	int recv_bytes;
	struct fast_task_info *pTask;
    StorageClientInfo *pClientInfo;
    int flags = 1;

	pTask = (struct fast_task_info *)arg;
	pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pClientInfo->canceled)
	{
		return;
	}

	if (pClientInfo->stage != FDFS_STORAGE_STAGE_NIO_RECV)
	{
		if (event & IOEVENT_TIMEOUT) {
			pTask->event.timer.expires = g_current_time +
				g_fdfs_network_timeout;
			fast_timer_add(&pTask->thread_data->timer,
				&pTask->event.timer);
		}

		return;
	}

	if (event & IOEVENT_TIMEOUT)
	{
		if (pClientInfo->total_offset == 0 && pTask->req_count > 0)
		{
			pTask->event.timer.expires = g_current_time +
				g_fdfs_network_timeout;
			fast_timer_add(&pTask->thread_data->timer,
				&pTask->event.timer);
		}
		else
		{
			logError("file: "__FILE__", line: %d, " \
				"client ip: %s, recv timeout, " \
				"recv offset: %d, expect length: %d", \
				__LINE__, pTask->client_ip, \
				pTask->offset, pTask->length);

			task_finish_clean_up(pTask);
		}

		return;
	}

	if (event & IOEVENT_ERROR)
	{
		logDebug("file: "__FILE__", line: %d, " \
			"client ip: %s, recv error event: %d, "
			"close connection", __LINE__, pTask->client_ip, event);

		task_finish_clean_up(pTask);
		return;
	}

	fast_timer_modify(&pTask->thread_data->timer,
		&pTask->event.timer, g_current_time +
		g_fdfs_network_timeout);
	while (1)
	{
		if (pClientInfo->total_length == 0) //recv header
		{
			recv_bytes = sizeof(TrackerHeader) - pTask->offset;
		}
		else
		{
			recv_bytes = pTask->length - pTask->offset;
		}



		bytes = recv(sock, pTask->data + pTask->offset, recv_bytes, 0);
		/*logDebug("file: "__FILE__", line: %d, offset: %d, recv_bytes: %d, bytes: %d", __LINE__, pTask->offset, recv_bytes, bytes);*/
		//add by guanyf 2017-12-12
		setsockopt(sock, IPPROTO_TCP, TCP_QUICKACK, (char *)&flags, sizeof(flags));

		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
			}
			else if (errno == EINTR)
			{
				continue;
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, STRERROR(errno));

				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logDebug("file: "__FILE__", line: %d, " \
				"client ip: %s, recv failed, " \
				"connection disconnected.", \
				__LINE__, pTask->client_ip);

			task_finish_clean_up(pTask);
			return;
		}

		if (pClientInfo->total_length == 0) //header
		{
			//logDebug("file: "__FILE__", line: %d, sizeof(TrackerHeader): %d", __LINE__, sizeof(TrackerHeader));		//10
			if (pTask->offset + bytes < sizeof(TrackerHeader))
			{
				pTask->offset += bytes;
				return;
			}


			pClientInfo->total_length=buff2long(((TrackerHeader *) \
						pTask->data)->pkg_len);
			/*logDebug("file: "__FILE__", line: %d, pClientInfo->total_length: %d", __LINE__, pClientInfo->total_length);	*/

			if (pClientInfo->total_length < 0)
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, pkg length: " \
					"%"PRId64" < 0", \
					__LINE__, pTask->client_ip, \
					pClientInfo->total_length);

				task_finish_clean_up(pTask);
				return;
			}

			pClientInfo->total_length += sizeof(TrackerHeader);
			if (pClientInfo->total_length > pTask->size)
			{
				pTask->length = pTask->size;
			}
			else
			{
				pTask->length = pClientInfo->total_length;
			}
		}

		pTask->offset += bytes;

		if (pTask->offset >= pTask->length) //recv current pkg done
		{
			if (pClientInfo->total_offset + pTask->length >= \
					pClientInfo->total_length)
			{
				/* current req recv done */
				//接受处理完成，客户端状态变为发送，发送处理结果给客户端
				pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_SEND;
				pTask->req_count++;
			}

			if (pClientInfo->total_offset == 0)
			{
				//接收客户端信息处理
				pClientInfo->total_offset = pTask->length;
				storage_deal_task(pTask);
			}
			else
			{
				pClientInfo->total_offset += pTask->length;

				/* continue write to file */
				//把任务放到io任务队列中，并发送信号，通知io处理函数进行处理。
				//io队列接收到通知后，会调用io处理函数进行处理。
				storage_dio_queue_push(pTask);
			}

			return;
		}
	}

	return;
}

static void client_sock_write(int sock, short event, void *arg)
{
	int bytes;
	struct fast_task_info *pTask;
        StorageClientInfo *pClientInfo;

	pTask = (struct fast_task_info *)arg;
        pClientInfo = (StorageClientInfo *)pTask->arg;
	if (pClientInfo->canceled)
	{
		return;
	}

	if (event & IOEVENT_TIMEOUT)
	{
		logError("file: "__FILE__", line: %d, " \
			"send timeout", __LINE__);

		task_finish_clean_up(pTask);
		return;
	}

	if (event & IOEVENT_ERROR)
	{
		logDebug("file: "__FILE__", line: %d, " \
			"client ip: %s, recv error event: %d, "
			"close connection", __LINE__, pTask->client_ip, event);

		task_finish_clean_up(pTask);
		return;
	}

	while (1)
	{
		fast_timer_modify(&pTask->thread_data->timer,
			&pTask->event.timer, g_current_time +
			g_fdfs_network_timeout);
		bytes = send(sock, pTask->data + pTask->offset, \
				pTask->length - pTask->offset,  0);			//发送文件时，一次发送头部和文件内容
		logDebug("%08X sended %d bytes, pTask->offset= %d, pTask->length= %d\n", \
			(int)pTask, bytes, pTask->offset, pTask->length);	//1949 0 1949
		if (bytes < 0)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				set_send_event(pTask);
			}
			else if (errno == EINTR)
			{
				continue;
			}
			else
			{
				logError("file: "__FILE__", line: %d, " \
					"client ip: %s, recv failed, " \
					"errno: %d, error info: %s", \
					__LINE__, pTask->client_ip, \
					errno, STRERROR(errno));

				task_finish_clean_up(pTask);
			}

			return;
		}
		else if (bytes == 0)
		{
			logWarning("file: "__FILE__", line: %d, " \
				"send failed, connection disconnected.", \
				__LINE__);

			task_finish_clean_up(pTask);
			return;
		}

		pTask->offset += bytes;
		if (pTask->offset >= pTask->length)
		{
			if (set_recv_event(pTask) != 0)
			{
				return;
			}

			pClientInfo->total_offset += pTask->length;
			logDebug("pClientInfo->total_offset= %d, pClientInfo->total_length= %d\n",\
				pClientInfo->total_offset, pClientInfo->total_length);
			if (pClientInfo->total_offset>=pClientInfo->total_length)
			{
				if (pClientInfo->total_length == sizeof(TrackerHeader)
					&& ((TrackerHeader *)pTask->data)->status == EINVAL)
				{
					logDebug("file: "__FILE__", line: %d, "\
						"close conn: #%d, client ip: %s", \
						__LINE__, pTask->event.fd,
						pTask->client_ip);
					task_finish_clean_up(pTask);
					return;
				}

				/*  reponse done, try to recv again */
				pClientInfo->total_length = 0;
				pClientInfo->total_offset = 0;
				pTask->offset = 0;
				pTask->length = 0;

				pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_RECV;
			}
			else  //continue to send file content
			{
				pTask->length = 0;

				/* continue read from file */
				storage_dio_queue_push(pTask);
			}

			return;
		}
	}
}


/*add by xiaods start 20190813*/
static void client_sock_write_ex(int sock, short event, void *arg)
{
	//改为sendfile发送文件
	int bytes;
	int64_t total_send_bytes;
	struct fast_task_info *pTask;
	StorageClientInfo *pClientInfo;
	StorageFileContext *pFileContext;
	int result = 0;

	pTask = (struct fast_task_info *)arg;
	pClientInfo = (StorageClientInfo *)pTask->arg;
	pFileContext = &(pClientInfo->file_context);
	if (pClientInfo->canceled)
	{
		return;
	}

	if (event & IOEVENT_TIMEOUT)
	{
		logError("file: "__FILE__", line: %d, " \
			"send timeout", __LINE__);

		task_finish_clean_up(pTask);
		return;
	}

	if (event & IOEVENT_ERROR)
	{
		logDebug("file: "__FILE__", line: %d, " \
			"client ip: %s, recv error event: %d, "
			"close connection", __LINE__, pTask->client_ip, event);

		task_finish_clean_up(pTask);
		return;
	}
	do
	{
		//发送头部
		if ((result = tcpsenddata_nb(sock, pTask->data, \
			sizeof(TrackerHeader), g_fdfs_network_timeout)) != 0)
		{
			logError("file: "__FILE__", line: %d, " \
				"send data to client %s fail, " \
				"errno: %d, error info: %s", __LINE__, \
				pTask->client_ip, \
				result, STRERROR(result));
			break;
		}

		//发送文件
		if ((result = tcpsendfile_ex(sock, pFileContext->filename, pFileContext->offset, \
			pFileContext->end - pFileContext->start, g_fdfs_network_timeout, \
			&total_send_bytes)) != 0)
		{
			break;
		}

		pClientInfo->total_length = 0;
		pClientInfo->total_offset = 0;
		pTask->offset = 0;
		pTask->length = 0;

		pClientInfo->stage = FDFS_STORAGE_STAGE_NIO_RECV;
	} while (0);
	
}
/*add by xiaods end 20190813*/