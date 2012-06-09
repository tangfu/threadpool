threadpool
==========

a threadpool library
一个线程池库

==========
1. 【特性】
    * 该线程池使用了SIGRTMAX-5信号，因此使用该库请不要使用这个信号
    * 线程池中的所有锁，都是进程私有的，所以如果要应用于多进程的话，请修改源码设置锁的pshared属性
    * 由于linux下线程的限制，初始化是计算单进程的最大线程数，同时在初始化时送入线程栈大小
    	0:			    使用默认栈大小，一般是8M
	0～PTHREAD_STACK_MIN:	    使用线程池默认的栈大小，2M
	>PTHREAD_STACK_MIN:	    使用送入的栈大小
    * 现在是假设所有传入的参数类型是TP_STATIC，那么线程池会重新分配堆上数据；线程任务执行完后，会自动清理内存，除非work->para == NULL或者work->len == 0
    * 类型是TP_MALLOC，线程池不检查work->len的值，只检查work->para是否为NULL
    * 类型是TP_STATIC，线程池检查work->len和work->para的值
    * 但一般在任何情况下，应用程序都应该将work->len给于完整，保证逻辑上的一致性


=========
2. 【用法】
    详见example文件夹
