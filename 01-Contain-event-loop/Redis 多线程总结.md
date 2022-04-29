# Reactor I/O模型与Redis IO事件源码

## :raised_hand:Reactor I/O模型

本质上 ,Reactor I/O 模型等价于 I/O多路复用(I/O Multiplexing) + 非阻塞I/O(Non-blocking I/O)

> 特点

- 一个主线程
  - 负责做event-loop和I/O读写（通过`select(),poll(),epoll(),kqueue()`来进行I/O事件的监听）
- 多个工作线程
  - 执行业务逻辑

> 非阻塞体现在哪儿？

非阻塞指的是避免在I/O线程在`read()`和`write()`或者调用其他I/O操作时时阻塞，通过多路复用I/O器我们就可以实现在有读事件或者写事件产生的时候交给工作线程去处理，进而做到非阻塞，而主线程也就可以保证一个线程来处理多个socket的能力

在Reactor模型中，阻塞仅存在于调用`select()`,`poll()`或者`epoll_wait()`时

> Reactor 工作流程

- Server 端完成在 `bind&listen` 之后，将 listenfd 注册到 epollfd 中，最后进入 event-loop 事件循环。循环过程中会调用 `select/poll/epoll_wait` 阻塞等待，若有在 listenfd 上的新连接事件则解除阻塞返回，并调用 `socket.accept` 接收新连接 connfd，并将 connfd 加入到 epollfd 的 I/O 复用（监听）队列。
- 当 connfd 上发生可读/可写事件也会解除 `select/poll/epoll_wait` 的阻塞等待，然后进行 I/O 读写操作，这里读写 I/O 都是非阻塞 I/O，这样才不会阻塞 event-loop 的下一个循环。然而，这样容易割裂业务逻辑，不易理解和维护。
- 调用 `read` 读取数据之后进行解码并放入队列中，等待工作线程处理。
- 工作线程处理完数据之后，返回到 event-loop 线程，由这个线程负责调用 `write` 把数据写回 client。

![MultiReactors.png](https://raw.githubusercontent.com/panjf2000/illustrations/master/go/multi-reactors.png?imageView2/2/w/1280/format/jpg/interlace/1/q/100?imageView2/2/w/1280/format/jpg/interlace/1/q/100)

## :floppy_disk:Redis Reactor模型的实现

### Redis 3.0 Reactor

在Redis3.0版本中，Redis Reactor I/O模型主要为单Reactor，即单线程处理[^1]。

![img](https://img.taohuawu.club/gallery/single-threaded-redis.png?imageView2/2/w/1280/format/jpg/interlace/1/q/100)

> 基本概念

在进行分析之前，我们需要明确几个基本的对象和函数需要进行明确：

- client：Redis会为每一个客户端连接生成一个client对象，对象中包含了这次分享中几个关键的信息
  - queryBuf：输入缓冲区
  - buf，reply：输出缓冲区（reply为链表形式）
- `aeApiPoll()` I/O多路复用函数，是redis事件循环eventLoop中根据不同多路复用函数包装实现（`epoll`,`kqueue`等）的，例如Linux平台下一旦redis选择了使用epoll,那么在`aeApiPoll()`函数中就会调用`epoll_wait()`来等待可读可写事件的发生
- `acceptTcpHandler()`连接应答处理器，当有新的连接到来的时候，会利用底层的`accept()`函数来处理连接，并将函数`readQueryFromClient()`注册为可读事件的处理器
- `readQueryFromClient()`可读事件的处理器，主要负责将client中想要执行的命令读取出来存放到输入缓冲区queryBuf中
- `beforeSleep()`主要在`aeApiPoll()`之前执行，主要操作包含将输出缓冲区内的残留信息写回client
- `sendReplyToClient()`写事件的处理器，`beforeSleep()`中写回的操作就是由该函数完成的

> Redis 3.0 单Reactor模型

![img](https://img.taohuawu.club/gallery/single-reactor.png?imageView2/2/w/1280/format/jpg/interlace/1/q/100)

3.0版本的Reactor模型就是一个主线程在执行eventLoop，并由主线程来处理客户端的读取，命令的执行以及写回客户端的操作。

> 单Reactor模型的缺点

举一个例子：

- Redis在执行删除DEL KEY的时候,如果KEY的数量特别大，那么就会阻塞单线程的EventLoop，造成其他可读可写事件不能够及时触发执行器执行，这是Reactor模型不能容忍的

- 尽管作者曾试图在进行DEL KEY操作时进行小批量删除的操作，但是如果在删除的时候仍有客户端不断的写入比本次删除量更多的KEY时，那么问题就会变为KEYS永远不能被删除干净，有可能会造成脏读现象的产生

  

### Redis 6.0 Reactor

我们来看一下6.0版本的Redis IO的总体事件处理流程概览图[^1]

<img src="https://img.taohuawu.club/gallery/multiple-threaded-redis.png?imageView2/2/w/1280/format/jpg/interlace/1/q/100" alt="img" style="width:4000px "/>

#### 多线程处理事件主要流程解析

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
9. 在事件循环（Event Loop）中，主线程执行 `beforeSleep() -->`  `handleClientsWithPendingWritesUsingThreads()`，利用 Round-Robin 轮询负载均衡策略，把 `clients_pending_write` 队列中的连接均匀地分配给 I/O 线程各自的本地 FIFO 任务队列 `io_threads_list[id]` 和主线程自己，I/O 线程通过调用 `writeToClient` 把 `client` 的写出缓冲区里的数据回写到客户端，主线程忙轮询，等待所有 I/O 线程完成写出任务；
10. 主线程和所有 I/O 线程都完成了写出任务， 主线程结束忙轮询，遍历 `clients_pending_write` 队列，如果 `client` 的写出缓冲区还有数据遗留，则注册 `sendReplyToClient()` 到该连接的写就绪事件，等待客户端可写时在事件循环中再继续回写残余的响应数据。

其大致时序图如下[^2]：

<img src="https://img-blog.csdnimg.cn/20201111100007836.jpg?x-oss-process=image/watermark,type_ZmFuZ3poZW5naGVpdGk,shadow_10,text_aHR0cHM6Ly9ibG9nLmNzZG4ubmV0L3dlaXhpbl80NTUwNTMxMw==,size_16,color_FFFFFF,t_70#pic_center" alt="在这里插入图片描述" style="width:4000px " />


[^1]: https://strikefreedom.top/multiple-threaded-network-model-in-redis Redis多线程网络模型全面揭秘

[^2]: https://blog.csdn.net/weixin_45505313/article/details/108562756 Redis 6.0 源码阅读笔记(2)-Redis 多线程原理

