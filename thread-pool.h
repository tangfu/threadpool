/**
 * @file thread-pool.h
 * @brief	
 *
 *  一个线程池的库
 *
 * @author tangfu - abctangfuqiang2008@163.com
 * @version 1.0
 * @date 2012-06-09
 */

#ifndef __THREAD_POOL_H__
#define	__THREAD_POOL_H__


#include <pthread.h>
#include <signal.h>
#include <stdint.h>


#define TPTHREAD_STOP_SIGNAL SIGRTMAX-5
#define TPTHREAD_STACK_SIZE 	2097152


#ifndef TPBOOL
typedef int TPBOOL;
#endif

#ifndef TPTRUE
#define TPTRUE 1
#endif

#ifndef TPFALSE
#define TPFALSE 0
#endif

#define BUSY_THRESHOLD 0.5	//(busy thread)/(all thread threshold)
#define MANAGE_INTERVAL 1	//tp manage thread sleep interval

//typedef struct tp_work_desc_s tp_work_desc;
typedef void tp_work_desc;
typedef struct tp_work_s tp_work;
typedef struct tp_thread_pool_s tp_thread_pool;
typedef struct tp_thread_conf_s tp_thread_conf;
typedef struct tp_status_s tp_status;
//typedef enum tp_param_type_s { NEED_FREE = 0, NONEED_FREE} tp_param_type;
typedef enum tp_param_type_s { TP_STATIC = 0,  TP_MALLOC } tp_param_type;
typedef enum tp_behavior_s {TP_NORMAL = 0 , TP_CREATE_ONLY, TP_NO_ALL} tp_type;

struct tp_thread_conf_s {
        int min_th_num;			///min thread number in the pool
        int max_th_num;         ///max thread number in the pool
        int min_spare_th_num;	///when spare_th_num is below the value,thread pool will create min_spare_th_num threads
        int max_spare_th_num;	///when spare_th_num is beyond the value,thread pool will recycle min_spare_th_num threads
        size_t stack_size;
        int max_buffer_num;
};


//base thread struct
struct tp_work_s {
        //main process function. user interface
        // void *( *process_job )( tp_work *this, tp_work_desc *job );
        void *( *process_job )( tp_work *this );
        tp_work_desc *para;
        int len;
        tp_param_type type;
};


struct tp_status_s {
        int cur_th_num;		//current thread number in the pool
        int busy_th_num;		//current running thread number in the pool
        int drop_task_num;
        int max_buffer_num;
        int cur_buffer_num;
        int max_used_buffer_num;
        int concurrent_num;
        int stack_size;
        int64_t exec_cnt;
};

//main thread pool struct
struct tp_thread_pool_s {

        /**
         * @brief	init
         *
         * 初始化线程池库
         *
         * @param	this		库对象指针
         * @param	conf		线程池的相关参数
         * @param	log_file	日志文件
         */
        TPBOOL( *init )( tp_thread_pool *this, tp_thread_conf *conf, const char *log_file );
        void ( *close )( tp_thread_pool *this );
        void ( *reset )( tp_thread_pool *this );
        ///线程池库的调度函数
        // TPBOOL( *process_job )( tp_thread_pool *this, tp_work *worker, tp_work_desc *job );
        TPBOOL( *process_job )( tp_thread_pool *this, tp_work *worker );
        ///获取线程池的信息，返回给调用着
        TPBOOL( *get_status )( tp_thread_pool *this, tp_status *ret );
        ///打印线程池的状态，包括当前繁忙线程数，总线程数等
        TPBOOL( *print_tp_status )( tp_thread_pool *this );
        ///设置线程池的最大并发数(num 在0~max_th_num之间有效，其他值相当于取消并发数设置)
        TPBOOL( *set_max_concurrent_num )( tp_thread_pool *this, int num );
        ///禁止线程池调度线程
        void ( *disable )( tp_thread_pool *this );
        ///线程池使能线程调度
        void ( *enable )( tp_thread_pool *this );
        ///重新设置日志，可以设置为NULL
        int ( *setlog )( tp_thread_pool *this, const char *log_file );
        ///
        int ( *settype )( tp_thread_pool *this, tp_type type );
};

#ifdef __cplusplus
extern "C" {
#endif

        // void catch_quit( int sig );
        tp_thread_pool *create_thread_pool();
        void destroy_thread_pool( tp_thread_pool* pool );

#ifdef __cplusplus
}
#endif

#endif		/* __THREAD_POOL_H__  */
