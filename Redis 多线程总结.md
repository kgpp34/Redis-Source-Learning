# Redis IO事件源码

我们来看一下6.0版本的Redis IO的总体事件处理流程概览图[^1]

<img src="https://img.taohuawu.club/gallery/multiple-threaded-redis.png?imageView2/2/w/1280/format/jpg/interlace/1/q/100" alt="img" style="zoom: 200% width=500;"  />



### 🎨多线程处理事件主要流程解析

1. 初始化Redis服务器`server.c -> main() --> initServer()`,
   1. 开启主线程的事件循环`EventLoop`
   1. 绑定端口
   1. 创建时间事件并绑定时间事件handler为`serverCron()`
   1. 为当前所有的文件事件创建TCP connection的handler为`acceptTcpHandler()`，等待新的连接建立
   1. 注册事件循环中的beforeSleepProc和afterSleepProc为`beforeSleep()`和`afterSleep()`
2. 客户端client与server建立连接
3. `accepTcpHandler()`函数被调用
4. 主线程将`readQueryFromClient()`函数绑定到客户端可读事件处理器上，`createClient() -> connSetReadHandler()`
5. 客户端触发读就绪事件，如果配置了多线程，主线程将调用`postponeClientRead()`将client放入`clients_pending_read`队列的头部
6. 随后主线程会执行`beforeSleep() -> handleClientsWithPendingReadsUsingThreads()`,将`clients_pending_read`中的client依照Round Robin轮询的方式放入本地的任务队列`io_thread_list[id]`的尾端和主线程自己的任务队列。同时设置`io_threads_pending`队列中各个IO线程需要处理的读事件的个数（猜测是来激活IO线程进行工作）
   1. I/O 线程通过 socket 读取客户端的请求命令，存入 `client->querybuf` 并解析第一个命令，**但不执行命令**，主线程忙轮询，等待所有 I/O 线程完成读取任务；

7. 主线程和所有 I/O 线程都完成了读取任务，主线程结束忙轮询，遍历 `clients_pending_read` 队列，**执行所有客户端连接的请求命令**，先调用 `processCommandAndResetClient` 执行第一条已经解析好的命令，然后调用 `processInputBuffer` 解析并执行客户端连接的所有命令，在其中使用 `processInlineBuffer` 或者 `processMultibulkBuffer` 根据 Redis 协议解析命令，最后调用 `processCommand` 执行命令
8. 根据请求命令的类型（SET, GET, DEL, EXEC 等），分配相应的命令执行器去执行，最后调用 `addReply()` 函数族的一系列函数将响应数据写入到对应 `client` 的写出缓冲区：`client->buf` 或者 `client->reply` ，`client->buf` 是首选的写出缓冲区，固定大小 16KB，一般来说可以缓冲足够多的响应数据，但是如果客户端在时间窗口内需要响应的数据非常大，那么则会自动切换到 `client->reply` 链表上去，使用链表理论上能够保存无限大的数据（受限于机器的物理内存），最后把 `client` 添加进一个 LIFO 队列 `clients_pending_write`
9. 在事件循环（Event Loop）中，主线程执行 `beforeSleep()` --> `handleClientsWithPendingWritesUsingThreads()`，利用 Round-Robin 轮询负载均衡策略，把 `clients_pending_write` 队列中的连接均匀地分配给 I/O 线程各自的本地 FIFO 任务队列 `io_threads_list[id]` 和主线程自己，I/O 线程通过调用 `writeToClient` 把 `client` 的写出缓冲区里的数据回写到客户端，主线程忙轮询，等待所有 I/O 线程完成写出任务；
10. 主线程和所有 I/O 线程都完成了写出任务， 主线程结束忙轮询，遍历 `clients_pending_write` 队列，如果 `client` 的写出缓冲区还有数据遗留，则注册 `sendReplyToClient()` 到该连接的写就绪事件，等待客户端可写时在事件循环中再继续回写残余的响应数据。


[^1]: https://strikefreedom.top/multiple-threaded-network-model-in-redis Redis多线程网络模型全面揭秘

