#include "redis.h"
#include "util.h"
#include "zmalloc.h"
#include "object.h"
#include <sys/uio.h>
#include <math.h>

extern struct redisServer server;
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask);
void *dupClientReplyValue(void *o);
void decrRefCountVoid(void *o);

/*
 * 如果在读入协议内容是,发现内容不符合协议,那么异步地关闭这个客户端
 */
static void setProtocolError(redisClient *c, int pos)
{
	// todo
}

/*
 * 创建一个新的客户端
 */
redisClient *createClient(int fd)
{
	redisClient *c = zmalloc(sizeof(redisClient));
	// 当 fd 不为 -1 时，创建带网络连接的客户端
	// 如果 fd 为 -1 ，那么创建无网络连接的伪客户端
	// 因为 Redis 的命令必须在客户端的上下文中使用，所以在执行 Lua 环境中的命令时
	// 需要用到这种伪终端.
	if (fd != -1)
	{
		anetNonBlock(NULL, fd);
		anetEnableTcpNoDelay(NULL, fd); // 禁用 Nagle算法

		if (server.tcpkeepalive)
		{
			anetKeepAlive(NULL, fd, server.tcpkeepalive);
		}

		// 绑定读事件到事件loop(开始接收命令请求)
		if (aeCreateFileEvent(server.el, fd, AE_READABLE,
							  readQueryFromClient, c) == AE_ERR)
		{
			close(fd);
			zfree(c);
			return NULL;
		}
	}

	/* 初始化各个属性 */
	c->fd = fd;
	c->name = NULL;
	c->bufpos = 0; // 回复缓冲区的偏移量
	c->querybuf = sdsempty();
	c->reqtype = 0;				// 命令请求的类型
	c->argc = 0;				// 命令参数的数量
	c->argv = NULL;				// 命令参数
	c->cmd = c->lastcmd = NULL; // 当前执行的命令和最近一次执行的命令

	c->bulklen = -1;	 // 读入的参数的长度
	c->multibulklen = 0; // 查询缓冲区中未读入的命令内容数量

	c->reply = listCreate(); // 回复链表
	c->reply_bytes = 0;		 //  回复链表的字节量

	listSetFreeMethod(c->reply, decrRefCountVoid);
	listSetDupMethod(c->reply, dupClientReplyValue);

	if (fd != -1)
		listAddNodeTail(server.clients, c); // 如果不是伪客户端,那么添加服务器的客户端到客户端链表之中

	return c;
}

/*================================ Parse command ==================================*/

/*
 * 处理内联命令，并创建参数对象
 *
 * 内联命令的各个参数以空格分开，并以 \r\n 结尾
 * 例子：
 *
 * <arg0> <arg1> <arg...> <argN>\r\n
 *
 * 这些内容会被用于创建参数对象，
 * 比如
 *
 * argv[0] = arg0
 * argv[1] = arg1
 * argv[2] = arg2
 */

/*
 * 这个函数相当于parse,处理命令
 */
int processInlineBuffer(redisClient *c)
{
	char *newline;
	int argc, j;
	sds *argv, aux;
	size_t querylen;

	newline = strchr(c->querybuf, '\n'); // 寻找一行的结尾

	// 如果接收到了错误的内容,出错
	if (newline == NULL)
	{
		return REDIS_ERR;
	}

	/* 处理\r\n */
	if (newline && newline != c->querybuf && *(newline - 1) == '\r')
		newline--;

	/* 然后根据空格,来分割命令的参数
	 * 比如说 SET msg hello \r\n将被分割成
	 * argv[0] = SET
	 * argv[1] = msg
	 * argv[2] = hello
	 * argc = 3
	 */
	querylen = newline - (c->querybuf);
	aux = sdsnewlen(c->querybuf, querylen);
	argv = sdssplitargs(aux, &argc);
	sdsfree(aux);

	/* 从缓冲区中删除已经读取了的内容,剩下的内容是未被读取的 */
	sdsrange(c->querybuf, querylen + 2, -1);

	if (c->argv)
		free(c->argv);
	c->argv = zmalloc(sizeof(robj *) * argc);

	// 为每个参数创建一个字符串对象
	for (c->argc = 0, j = 0; j < argc; j++)
	{
		if (sdslen(argv[j]))
		{
			// argv[j] 已经是 SDS 了
			// 所以创建的字符串对象直接指向该 SDS
			c->argv[c->argc] = createObject(REDIS_STRING, argv[j]);
			c->argc++;
		}
		else
		{
			sdsfree(argv[j]);
		}
	}
	zfree(argv);
	return REDIS_OK;
}

/*
 * 将 c->querybuf 中的协议内容转换成 c->argv 中的参数对象
 *
 * 比如 *3\r\n$3\r\nSET\r\n$3\r\nMSG\r\n$5\r\nHELLO\r\n
 * 将被转换为：
 * argv[0] = SET
 * argv[1] = MSG
 * argv[2] = HELLO
 */
int processMultibulkBuffer(redisClient *c)
{
	char *newline = NULL;
	int pos = 0, ok;
	long long ll;

	// 读入命令的参数个数
	// 比如 *3\r\n$3\r\nSET\r\n... 将令 c->multibulklen = 3
	if (c->multibulklen == 0)
	{
		newline = strchr(c->querybuf, '\r');
		// 将参数个数，也即是 * 之后， \r\n 之前的数字取出并保存到 ll 中
		// 比如对于 *3\r\n ，那么 ll 将等于 3
		ok = string2ll(c->querybuf + 1, newline - (c->querybuf + 1), &ll);

		// 参数数量之后的位置
		// 比如对于 *3\r\n$3\r\n$SET\r\n... 来说，
		// pos 指向 *3\r\n$3\r\n$SET\r\n...
		//                ^
		//                |
		//               pos
		pos = (newline - c->querybuf) + 2;

		// 设置参数数量
		c->multibulklen = ll;

		// 根据参数数量,为各个参数对象分配空间
		if (c->argv)
			zfree(c->argv);
		c->argv = zmalloc(sizeof(robj *) * c->multibulklen);
	}

	// 从c->querybuf中读入参数,并创建各个参数对象到c->argv
	while (c->multibulklen)
	{
		// 读入参数长度
		if (c->bulklen == -1)
		{ // 这里指的是命令的长度
			// 确保"\r\n"存在
			newline = strchr(c->querybuf + pos, '\r');

			// 读取长度,比如说 $3\r\nSET\r\n 会让 ll 的值变成3
			ok = string2ll(c->querybuf + pos + 1, newline - (c->querybuf + pos + 1), &ll);

			// 定位到参数的开头
			// 比如
			// $3\r\nSET\r\n...
			//       ^
			//       |
			//      pos
			pos += newline - (c->querybuf + pos) + 2;
			c->bulklen = ll;
		}

		// 为参数创建字符串对象
		if (pos == 0 &&
			c->bulklen >= REDIS_MBULK_BIG_ARG &&
			(signed)sdslen(c->querybuf) == c->bulklen + 2)
		{
			c->argv[c->argc++] = createObject(REDIS_STRING, c->querybuf);
			sdsIncrLen(c->querybuf, -2); // 去掉\r\n
			c->querybuf = sdsempty();
			c->querybuf = sdsMakeRoomFor(c->querybuf, c->bulklen + 2);
			pos = 0;
		}
		else
		{
			c->argv[c->argc++] =
				createStringObject(c->querybuf + pos, c->bulklen);
			pos += c->bulklen + 2;
		}

		// 清空参数长度
		c->bulklen = -1;

		// 减少还需读入的参数个数
		c->multibulklen--;
	}

	if (pos)
		sdsrange(c->querybuf, pos, -1); // 从querybuf中删除已被读取的内容

	// 如果本条命令的所有参数都已经读取完,那么返回
	if (c->multibulklen == 0)
		return REDIS_OK;

	// 如果还有参数未读取完,那么就是协议出错了!
	return REDIS_ERR;
}

// 在客户端执行完命令之后执行：重置客户端以准备执行下个命令
void resetClient(redisClient *c)
{
	// todo
}

void processInputBuffer(redisClient *c)
{
	// 尽可能地处理查询缓存区中的内容.如果读取出现short read, 那么可能会有内容滞留在读取缓冲区里面
	// 这些滞留的内容也许不能完整构成一个符合协议的命令,需要等待下次读事件的就绪.
	while (sdslen(c->querybuf))
	{

		if (!c->reqtype)
		{
			if (c->querybuf[0] == '*')
			{
				c->reqtype = REDIS_REQ_MULTIBULK; // 多条查询
			}
			else
			{
				c->reqtype = REDIS_REQ_INLINE; // 内联查询
			}
		}
		// 将缓冲区的内容转换成命令,以及命令参数
		if (c->reqtype == REDIS_REQ_INLINE)
		{
			if (processInlineBuffer(c) != REDIS_OK)
				break;
		}
		else if (c->reqtype == REDIS_REQ_MULTIBULK)
		{
			// 处理完成之后client -> argv 和 argc就不是空了
			if (processMultibulkBuffer(c) != REDIS_OK)
				break;
		}
		else
		{
			// todo
		}

		if (c->argc == 0)
		{
			resetClient(c); // 重置客户端
		}
		else
		{
			if (processCommand(c) == REDIS_OK)
				resetClient(c);
		}
	}
}

/*
 * 读取客户端的查询缓冲区内容
 */
void readQueryFromClient(aeEventLoop *el, int fd, void *privdata, int mask)
{
	redisClient *c = (redisClient *)privdata;
	int nread, readlen;
	size_t qblen;

	server.current_client = c; // 设置服务器的当前客户端

	readlen = REDIS_IOBUF_LEN; // 读入长度,默认为16MB

	// 获取查询缓冲区当前内容的长度
	qblen = sdslen(c->querybuf);
	// 如果有需要,更新缓冲区内容长度的峰值(peak)
	if (c->querybuf_peak < qblen)
		c->querybuf_peak = qblen;
	// 为查询缓冲区分配空间
	c->querybuf = sdsMakeRoomFor(c->querybuf, readlen);
	// 读入内容到查询缓存
	nread = read(fd, c->querybuf + qblen, readlen);

	if (nread == -1)
	{
		if (errno == EAGAIN)
		{
			nread = 0;
		}
		else
		{
			// freeClient(c);
			return;
		}
	}
	else if (nread == 0)
	{ // 对方关闭了连接
		// todo
		// freeClient(c);
		return;
	}

	if (nread)
	{
		sdsIncrLen(c->querybuf, nread);
	}
	else
	{
		// 在 nread == -1 且 errno == EAGAIN 时运行
		server.current_client = NULL;
		return;
	}
	//
	// 从查询缓存中读取内容,创建参数,并执行命令,函数会执行到缓存中的所有内容都被处理完为止
	//
	processInputBuffer(c);
	server.current_client = NULL;
}

/*
 * TCP 连接 accept 处理器
 */
#define MAX_ACCEPTS_PER_CALL 1000
static void acceptCommonHandler(int fd, int flags)
{
	// 创建客户端
	redisClient *c;
	// 在createClient函数中大概做了这么一些事情,首先是构造了一个redisClient结构,
	// 最为重要的是,完成了fd与RedisClient结构的关联,并且完成了该结构的初始化工作.
	// 并将这个结果挂到了server的clients链表上.
	// 还有一点,那就是对该fd的读事件进行了监听.
	if ((c = createClient(fd)) == NULL)
	{
		// log
		close(fd);
		return;
	}
}

/*
 * 创建一个 TCP 连接处理器
 */
void acceptTcpHandler(aeEventLoop *el, int fd, void *privdata, int mask)
{
	int cport, cfd, max = 2;
	char cip[REDIS_IP_STR_LEN];

	while (max--)
	{
		cfd = anetTcpAccept(server.neterr, fd, cip, sizeof(cip), &cport); // 获得ip地址以及端口号
		if (cfd == ANET_ERR)
		{
			if (errno != EWOULDBLOCK)
			{
				// log
				return;
			}
		}
		acceptCommonHandler(cfd, 0); // 为客户端创建客户端状态
	}
}

void addReply(redisClient *c, robj *obj)
{

	// 为客户端安装写处理器到事件循环
	if (prepareClientToWrite(c) != REDIS_OK)
		return;

	/* This is an important place where we can avoid copy-on-write
	 * when there is a saving child running, avoiding touching the
	 * refcount field of the object if it's not needed.
	 *
	 * 如果在使用子进程，那么尽可能地避免修改对象的 refcount 域。
	 *
	 * If the encoding is RAW and there is room in the static buffer
	 * we'll be able to send the object to the client without
	 * messing with its page.
	 *
	 * 如果对象的编码为 RAW ，并且静态缓冲区中有空间
	 * 那么就可以在不弄乱内存页的情况下，将对象发送给客户端。
	 */
	if (sdsEncodedObject(obj))
	{
		// 首先尝试复制内容到 c->buf 中，这样可以避免内存分配
		if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
			// 如果 c->buf 中的空间不够，就复制到 c->reply 链表中
			// 可能会引起内存分配
			_addReplyObjectToList(c, obj);
	}
	else if (obj->encoding == REDIS_ENCODING_INT)
	{
		/* Optimization: if there is room in the static buffer for 32 bytes
		 * (more than the max chars a 64 bit integer can take as string) we
		 * avoid decoding the object and go for the lower level approach. */
		// 优化，如果 c->buf 中有等于或多于 32 个字节的空间
		// 那么将整数直接以字符串的形式复制到 c->buf 中
		if (listLength(c->reply) == 0 && (sizeof(c->buf) - c->bufpos) >= 32)
		{
			char buf[32];
			int len;

			len = ll2string(buf, sizeof(buf), (long)obj->ptr);
			if (_addReplyToBuffer(c, buf, len) == REDIS_OK)
				return;
			/* else... continue with the normal code path, but should never
			 * happen actually since we verified there is room. */
		}
		// 执行到这里，代表对象是整数，并且长度大于 32 位
		// 将它转换为字符串
		obj = getDecodedObject(obj);
		// 保存到缓存中
		if (_addReplyToBuffer(c, obj->ptr, sdslen(obj->ptr)) != REDIS_OK)
			_addReplyObjectToList(c, obj);
		decrRefCount(obj);
	}
	else
	{
		redisPanic("Wrong obj->encoding in addReply()");
	}
}

int prepareClientToWrite(redisClient *c)
{

	// LUA 脚本环境所使用的伪客户端总是可写的
	if (c->flags & REDIS_LUA_CLIENT)
		return REDIS_OK;

	// 客户端是主服务器并且不接受查询，
	// 那么它是不可写的，出错
	if ((c->flags & REDIS_MASTER) &&
		!(c->flags & REDIS_MASTER_FORCE_REPLY))
		return REDIS_ERR;

	// 无连接的伪客户端总是不可写的
	if (c->fd <= 0)
		return REDIS_ERR; /* Fake client */

	// 一般情况，为客户端套接字安装写处理器到事件循环
	if (c->bufpos == 0 && listLength(c->reply) == 0 &&
		(c->replstate == REDIS_REPL_NONE ||
		 c->replstate == REDIS_REPL_ONLINE) &&
		aeCreateFileEvent(server.el, c->fd, AE_WRITABLE,
						  sendReplyToClient, c) == AE_ERR)
		return REDIS_ERR;

	return REDIS_OK;
}

/*
 * 负责传送命令回复的写处理器
 */
void sendReplyToClient(aeEventLoop *el, int fd, void *privdata, int mask)
{
	redisClient *c = privdata;
	int nwritten = 0, totwritten = 0, objlen;
	size_t objmem;
	robj *o;
	REDIS_NOTUSED(el);
	REDIS_NOTUSED(mask);

	// 一直循环，直到回复缓冲区为空
	// 或者指定条件满足为止
	while (c->bufpos > 0 || listLength(c->reply))
	{

		if (c->bufpos > 0)
		{

			// c->bufpos > 0

			// 写入内容到套接字
			// c->sentlen 是用来处理 short write 的
			// 当出现 short write ，导致写入未能一次完成时，
			// c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
			nwritten = write(fd, c->buf + c->sentlen, c->bufpos - c->sentlen);
			// 出错则跳出
			if (nwritten <= 0)
				break;
			// 成功写入则更新写入计数器变量
			c->sentlen += nwritten;
			totwritten += nwritten;

			/* If the buffer was sent, set bufpos to zero to continue with
			 * the remainder of the reply. */
			// 如果缓冲区中的内容已经全部写入完毕
			// 那么清空客户端的两个计数器变量
			if (c->sentlen == c->bufpos)
			{
				c->bufpos = 0;
				c->sentlen = 0;
			}
		}
		else
		{

			// listLength(c->reply) != 0

			// 取出位于链表最前面的对象
			o = listNodeValue(listFirst(c->reply));
			objlen = sdslen(o->ptr);
			objmem = getStringObjectSdsUsedMemory(o);

			// 略过空对象
			if (objlen == 0)
			{
				listDelNode(c->reply, listFirst(c->reply));
				c->reply_bytes -= objmem;
				continue;
			}

			// 写入内容到套接字
			// c->sentlen 是用来处理 short write 的
			// 当出现 short write ，导致写入未能一次完成时，
			// c->buf+c->sentlen 就会偏移到正确（未写入）内容的位置上。
			nwritten = write(fd, ((char *)o->ptr) + c->sentlen, objlen - c->sentlen);
			// 写入出错则跳出
			if (nwritten <= 0)
				break;
			// 成功写入则更新写入计数器变量
			c->sentlen += nwritten;
			totwritten += nwritten;

			/* If we fully sent the object on head go to the next one */
			// 如果缓冲区内容全部写入完毕，那么删除已写入完毕的节点
			if (c->sentlen == objlen)
			{
				listDelNode(c->reply, listFirst(c->reply));
				c->sentlen = 0;
				c->reply_bytes -= objmem;
			}
		}
		/* Note that we avoid to send more than REDIS_MAX_WRITE_PER_EVENT
		 * bytes, in a single threaded server it's a good idea to serve
		 * other clients as well, even if a very large request comes from
		 * super fast link that is always able to accept data (in real world
		 * scenario think about 'KEYS *' against the loopback interface).
		 *
		 * 为了避免一个非常大的回复独占服务器，
		 * 当写入的总数量大于 REDIS_MAX_WRITE_PER_EVENT ，
		 * 临时中断写入，将处理时间让给其他客户端，
		 * 剩余的内容等下次写入就绪再继续写入
		 *
		 * However if we are over the maxmemory limit we ignore that and
		 * just deliver as much data as it is possible to deliver.
		 *
		 * 不过，如果服务器的内存占用已经超过了限制，
		 * 那么为了将回复缓冲区中的内容尽快写入给客户端，
		 * 然后释放回复缓冲区的空间来回收内存，
		 * 这时即使写入量超过了 REDIS_MAX_WRITE_PER_EVENT ，
		 * 程序也继续进行写入
		 */
		if (totwritten > REDIS_MAX_WRITE_PER_EVENT &&
			(server.maxmemory == 0 ||
			 zmalloc_used_memory() < server.maxmemory))
			break;
	}

	// 写入出错检查
	if (nwritten == -1)
	{
		if (errno == EAGAIN)
		{
			nwritten = 0;
		}
		else
		{
			redisLog(REDIS_VERBOSE,
					 "Error writing to client: %s", strerror(errno));
			freeClient(c);
			return;
		}
	}

	if (totwritten > 0)
	{
		/* For clients representing masters we don't count sending data
		 * as an interaction, since we always send REPLCONF ACK commands
		 * that take some time to just fill the socket output buffer.
		 * We just rely on data / pings received for timeout detection. */
		if (!(c->flags & REDIS_MASTER))
			c->lastinteraction = server.unixtime;
	}
	if (c->bufpos == 0 && listLength(c->reply) == 0)
	{
		c->sentlen = 0;

		// 删除 write handler
		aeDeleteFileEvent(server.el, c->fd, AE_WRITABLE);

		/* Close connection after entire reply has been sent. */
		// 如果指定了写入之后关闭客户端 FLAG ，那么关闭客户端
		if (c->flags & REDIS_CLOSE_AFTER_REPLY)
			freeClient(c);
	}
}
