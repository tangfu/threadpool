#include "thread-pool.h"
#include "atomic.h"
#include "list.h"
#include <time.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/timerfd.h>
#include <sys/unistd.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <semaphore.h>


#define LINUX_STACKSIZE_DEFAULT	    8388608
#define VIRT_32	3221225472UL  //3G
#define VIRT_64	68719476736UL //64G

static struct tm currTM;
typedef struct tp_thread_info_s tp_thread_info;
typedef struct tp_thread_pool_internal_s tp_thread_pool_internal;
typedef struct tp_work_internal_s tp_work_internal;

struct tp_work_internal_s {
        void *( *process_job )( tp_work *this );
        tp_work_desc *para;
        int len;
        tp_param_type type;
        struct list_head list;
};
//thread info
struct tp_thread_info_s {
        pthread_t		thread_id;	//thread id num
        sem_t		sem_lock;
        tp_work			th_work;
        struct tp_thread_pool_internal_s *head;
        struct list_head original_list;
        struct list_head list[2];
        /* tp_work_desc		*th_job; */
};
struct tp_thread_pool_internal_s {
        TPBOOL( *init )( tp_thread_pool *this, tp_thread_conf *conf, const char *log_file );
        void ( *close )( tp_thread_pool *this );
        void ( *reset )( tp_thread_pool *this );
        /* TPBOOL( *process_job )( tp_thread_pool *this, tp_work *worker, tp_work_desc *job ); */
        TPBOOL( *process_job )( tp_thread_pool *this, tp_work *worker );
        TPBOOL( *get_status )( tp_thread_pool *this, tp_status *ret );
        TPBOOL( *print_tp_status )( tp_thread_pool *this );
        TPBOOL( *set_max_concurrent_num )( tp_thread_pool *this,  int num );
        void ( *disable )( tp_thread_pool *this );
        void ( *enable )( tp_thread_pool *this );
        int ( *setlog )( tp_thread_pool *this, const char *log_file );
        int ( *settype )( tp_thread_pool *this, tp_type type );
        //	int  (*get_thread_by_id)(tp_thread_pool *this, pthread_t id);
        //	TPBOOL (*add_thread)(tp_thread_pool *this, int number);
        //	TPBOOL (*delete_thread)(tp_thread_pool *this);
        //	int (*get_tp_status)(tp_thread_pool *this);

        int min_th_num;		//min thread number in the pool
        int max_th_num;         	//max thread number in the pool
        int min_spare_th_num;	//when spare_th_num is below the value,thread pool will create min_spare_th_num threads
        int max_spare_th_num;	//when spare_th_num is beyond the value,thread pool will recycle min_spare_th_num threads
        int max_th_limit;


        atomic_t concurrent_num;
        atomic_t cur_th_num;		//current thread number in the pool
        atomic_t busy_th_num;		//current running thread number in the pool
        atomic64_t exec_cnt;

        atomic_t th_flag;		//thread pool enable or shutdown;
        atomic_t th_init_flag;
        atomic_t th_concurrent_flag;
        pthread_attr_t attr;
        FILE* fd_log;

        tp_type type;
        tp_work_internal *buffer;
        struct list_head buffer_busy;
        struct list_head buffer_free;
        int max_buffer_num;		//when all threads are busy, cache queue will effect
        volatile int cur_buffer_num;
        volatile int drop_task_num;
        volatile int max_used_buffer_num;
        volatile int buffer_flag;	// 0 means have no task in buffer, 1 means have some task
        volatile int exit_flag;
        size_t stacksize;
        pthread_mutex_t buffer_lock;

        pthread_mutex_t tp_lock;			//或者用自旋琐
        pthread_t manage_thread_id;	//manage thread id num
        tp_thread_info *thread_info;	//work thread relative thread info

        int freelist_flag;
        int freelist_bak_flag;
        struct list_head *thread_free;
        struct list_head *thread_free_bak;
        struct list_head original_head;
        struct list_head freelist[2];
        /*
                struct list_head thread_busy;
                struct list_head thread_free;
        */
        struct list_head thread_unused;

        pthread_spinlock_t temp_lock;
        struct list_head temporary_free;

        struct list_head reserve_list;
        int reserve_cnt;
};



static inline int delay_time( struct timeval *timeout );
static void *tp_work_thread( void *pthread );
static void *tp_manage_thread( void *pthread );

static TPBOOL tp_init( tp_thread_pool *this, tp_thread_conf *conf, const char *log_file );
static void tp_close( tp_thread_pool *this );
/* static TPBOOL tp_process_job( tp_thread_pool *this, tp_work *worker, tp_work_desc *job ); */
static TPBOOL tp_process_job( tp_thread_pool *this, tp_work *worker );
static TPBOOL tp_add_thread( tp_thread_pool_internal *this );
static TPBOOL tp_delete_thread( tp_thread_pool_internal *this );
static int tp_get_tp_status( tp_thread_pool_internal *this );
static TPBOOL tp_get_status( tp_thread_pool *pool, tp_status *ret );
static TPBOOL tp_set_max_concurrent_num( tp_thread_pool *pool, int num );
static inline void tp_enable( tp_thread_pool *pool );
static inline void tp_disable( tp_thread_pool *pool );
static void tp_reset( tp_thread_pool *pool );
static int tp_setlog( tp_thread_pool *pool, const char *log_file );
static int tp_settype( tp_thread_pool *pool, tp_type type );
static void tp_print_status_to_log( tp_thread_pool_internal *this );

static tp_thread_info *get_free_thread( tp_thread_pool_internal *this );
static TPBOOL tp_process_buffer( tp_thread_pool_internal *this, tp_work_internal *worker );
static void amend_reserve( struct list_head *head, struct list_head *tmp0, struct list_head *tmp1 );
static void *exchange_freelist( tp_thread_pool_internal *this );
//////////////////////////////////////////////////////////////////////////////////////////////////


inline static void get_current_time( struct tm *p )
{
        time_t tval;
        time( &tval );
        localtime_r( &tval, p );
}

static void catch_quit( int sig )
{
        //fprintf(stderr,"thread %lu exit\n",pthread_self());
        ++sig;
        pthread_exit( 0 );
}

static inline int get_max_thread_num( size_t stacksize )
{
        long long int temp;

        switch( sizeof( int ) ) {
                case 8:	    //64bit
                        temp = VIRT_64;
                        temp = temp / ( long long int )stacksize - 10;
                        break;
                case 4:
                default:    //32bit
                        temp = VIRT_32;
                        temp = temp / ( long long int )stacksize - 10;
                        break;
        }

        return ( int )temp;
}

static inline TPBOOL check_tp_conf( tp_thread_conf *conf )
{
        if( conf == NULL )
                return TPFALSE;

        if( conf->max_spare_th_num > conf->min_spare_th_num && conf->max_th_num > conf->min_th_num && conf->min_th_num > conf->min_spare_th_num && conf->max_th_num > conf->max_spare_th_num )
                return TPTRUE;
        else
                return TPFALSE;
}

static inline TPBOOL tp_print_tp_status( tp_thread_pool *pool )
{
        if( pool == NULL )
                return TPFALSE;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;

        if( atomic_read( &this->th_flag ) == TPFALSE )
                return TPFALSE;

        if( this->buffer == NULL )
                fprintf( stdout, "[ THREAD INFO ]\n\tMIN_THREADS       : %d\n\tMAX_THREADS       : %d\n\tMIN_SPARE_THREADS : %d\n\tMAX_SPARE_THREADS : %d\n\tCUR_THREADS       : %d\n\tBUSY_THREADS      : %d\n\tCONCURRENT_NUM    : %d\n\tDROP_TASK_NUM     : %d\n\tSTACK_SIZE        : %d\n\tEXEC_CNT          : %lld\n", this->min_th_num, this->max_th_num, this->min_spare_th_num, this->max_spare_th_num, atomic_read( &this->cur_th_num ), atomic_read( &this->busy_th_num ), atomic_read( &this->concurrent_num ), this->drop_task_num, this->stacksize, atomic_read( &this->exec_cnt ) );
        else
                fprintf( stdout, "[ THREAD INFO ]\n\tMIN_THREADS       : %d\n\tMAX_THREADS       : %d\n\tMIN_SPARE_THREADS : %d\n\tMAX_SPARE_THREADS : %d\n\tCUR_THREADS       : %d\n\tBUSY_THREADS      : %d\n\tCONCURRENT_NUM    : %d\n\tDROP_TASK_NUM     : %d\n\tMAX_BUFFER_NUM    : %d\n\tCUR_BUFFER_NUM    : %d\n\tMAX_USED_BUFFER   : %d\n\tSTACK_SIZE        : %d\n\tEXEC_CNT          : %lld\n", this->min_th_num, this->max_th_num, this->min_spare_th_num, this->max_spare_th_num, atomic_read( &this->cur_th_num ), atomic_read( &this->busy_th_num ), atomic_read( &this->concurrent_num ), this->drop_task_num, this->max_buffer_num, this->cur_buffer_num, this->max_used_buffer_num, this->stacksize, atomic_read( &this->exec_cnt ) );

        return TPTRUE;
}

static inline void tp_print_status_to_log( tp_thread_pool_internal *this )
{
        if( this->buffer == NULL )
                fprintf( this->fd_log, "[ THREAD INFO ]\n\tMIN_THREADS       : %d\n\tMAX_THREADS       : %d\n\tMIN_SPARE_THREADS : %d\n\tMAX_SPARE_THREADS : %d\n\tCUR_THREADS       : %d\n\tBUSY_THREADS      : %d\n\tCONCURRENT_NUM    : %d\n\tDROP_TASK_NUM     : %d\n\tSTACK_SIZE        : %d\n\tEXEC_CNT          : %lld\n", this->min_th_num, this->max_th_num, this->min_spare_th_num, this->max_spare_th_num, atomic_read( &this->cur_th_num ), atomic_read( &this->busy_th_num ), atomic_read( &this->concurrent_num ), this->drop_task_num, this->stacksize , atomic_read( &this->exec_cnt ) );
        else
                fprintf( this->fd_log, "[ THREAD INFO ]\n\tMIN_THREADS       : %d\n\tMAX_THREADS       : %d\n\tMIN_SPARE_THREADS : %d\n\tMAX_SPARE_THREADS : %d\n\tCUR_THREADS       : %d\n\tBUSY_THREADS      : %d\n\tCONCURRENT_NUM    : %d\n\tDROP_TASK_NUM     : %d\n\tMAX_BUFFER_NUM    : %d\n\tCUR_BUFFER_NUM    : %d\n\tMAX_USED_BUFFER   : %d\n\tSTACK_SIZE        : %d\n\tEXEC_CNT          : %lld\n", this->min_th_num, this->max_th_num, this->min_spare_th_num, this->max_spare_th_num, atomic_read( &this->cur_th_num ), atomic_read( &this->busy_th_num ), atomic_read( &this->concurrent_num ), this->drop_task_num, this->max_buffer_num, this->cur_buffer_num, this->max_used_buffer_num, this->stacksize , atomic_read( &this->exec_cnt ) );
}


/**
  * user interface. creat thread pool.
  * para:
  * 	num: min thread number to be created in the pool
  * return:
  * 	thread pool struct instance be created successfully
  */
tp_thread_pool *create_thread_pool()
{
        tp_thread_pool_internal *this;
        //	char temp[100];
        this = ( tp_thread_pool_internal * )malloc( sizeof( tp_thread_pool_internal ) );

        if( this  ==  NULL )
                return NULL;

        memset( this, 0, sizeof( tp_thread_pool_internal ) );
        //init member function ponter
        this->init = tp_init;
        this->close = tp_close;
        this->reset = tp_reset;
        this->process_job = tp_process_job;
        //this->get_thread_by_id = tp_get_thread_by_id;
        /* this->add_thread = tp_add_thread; */
        /* this->delete_thread = tp_delete_thread; */
        /* this->get_tp_status = tp_get_tp_status; */
        this->get_status = tp_get_status;
        this->print_tp_status = tp_print_tp_status;
        this->set_max_concurrent_num = tp_set_max_concurrent_num;
        this->disable = tp_disable;
        this->enable = tp_enable;
        this->setlog = tp_setlog;
        this->settype = tp_settype;
        //init member var
        this->min_th_num = -1;
        this->max_th_num = -1;
        this->min_spare_th_num = -1;
        this->max_spare_th_num = -1;
        this->stacksize = 0;
        this->fd_log = stderr;
        atomic_set( &this->cur_th_num, -1 );
        atomic_set( &this->busy_th_num, -1 );
        atomic_set( &this->concurrent_num, -1 );
        atomic_set( &this->th_concurrent_flag, TPFALSE );
        atomic_set( &this->th_flag, TPFALSE );
        atomic_set( &this->th_init_flag, TPFALSE );
        pthread_mutex_init( &this->tp_lock, NULL );
        //sign up signal process function
        /* signal( SIGUSR2, catch_quit ); */
        return ( tp_thread_pool * )this;
}


void destroy_thread_pool( tp_thread_pool* pool )
{
        if( pool != NULL ) {
                if( atomic_read( &( ( tp_thread_pool_internal * ) pool )->th_init_flag ) == TPTRUE )
                        pool->close( pool );

                pthread_mutex_destroy( &( ( tp_thread_pool_internal * )pool )->tp_lock );
                free( ( tp_thread_pool_internal * )pool );
        }
}

static void tp_reset( tp_thread_pool *pool )
{
        if( pool == NULL )
                return;
}


/**
  * member function reality. thread pool init function.
  * para:
  * 	this: thread pool struct instance ponter
  * return:
  * 	true: successful; false: failed
  */
static TPBOOL tp_init( tp_thread_pool *pool, tp_thread_conf *conf, const char *log_file )
{
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        int i;
        pthread_mutex_lock( &this->tp_lock );

        if( atomic_read( &this->th_init_flag ) == TPTRUE ) {
                fprintf( this->fd_log, "thread_pool has already init\n" );
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        }

        this->fd_log = ( log_file == NULL ) ? stderr : fopen( log_file, "a+" );

        if( this->fd_log == NULL ) {
                fprintf( stderr, "file %s open failed\n", log_file );
                this->fd_log = stderr;
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        }

        get_current_time( &currTM );
        fprintf( this->fd_log, "\n\n\n########################################\n[%d/%02d/%02d %02d:%02d:%02d]-<threadpool_log>\n\n\n",
                 currTM.tm_year + 1900,
                 currTM.tm_mon + 1,
                 currTM.tm_mday,
                 currTM.tm_hour,
                 currTM.tm_min,
                 currTM.tm_sec );
        fprintf( this->fd_log, "tp_init: start\n" );

        if( check_tp_conf( conf ) == TPFALSE ) {
                fprintf( this->fd_log, "thread_conf is not legal\n" );
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        }

        pthread_attr_init( &this->attr );
        pthread_attr_setdetachstate( &this->attr, PTHREAD_CREATE_DETACHED );

        if( conf->stack_size == 0 ) {
                struct rlimit get;

                if( getrlimit( RLIMIT_STACK, &get ) == 0 ) {
                        this->max_th_limit = get_max_thread_num( get.rlim_cur );
                        this->stacksize = get.rlim_cur;
                } else {
                        this->max_th_limit = get_max_thread_num( LINUX_STACKSIZE_DEFAULT );
                        this->stacksize = LINUX_STACKSIZE_DEFAULT;
                }
        } else if( conf->stack_size < PTHREAD_STACK_MIN ) {
                pthread_attr_setstacksize( &this->attr, TPTHREAD_STACK_SIZE );
                this->max_th_limit = get_max_thread_num( TPTHREAD_STACK_SIZE );
                this->stacksize = TPTHREAD_STACK_SIZE;
        } else {
                this->stacksize =  conf->stack_size;
                pthread_attr_setstacksize( &this->attr, conf->stack_size );
                this->max_th_limit = get_max_thread_num( conf->stack_size );
        }

        this->type = TP_NORMAL;
        this->exit_flag = 1;
        this->min_th_num = conf->min_th_num;
        this->max_th_num = conf->max_th_num;
        this->min_spare_th_num = conf->min_spare_th_num;
        this->max_spare_th_num = conf->max_spare_th_num;
        this->freelist_flag = 0;
        this->freelist_bak_flag = 1;
        this->thread_free = &this->freelist[this->freelist_flag];
        this->thread_free_bak = &this->freelist[this->freelist_bak_flag];
        INIT_LIST_HEAD( &this->original_head );
        INIT_LIST_HEAD( &this->freelist[0] );
        INIT_LIST_HEAD( &this->freelist[1] );
        INIT_LIST_HEAD( &this->thread_unused );
        INIT_LIST_HEAD( &this->buffer_busy );
        INIT_LIST_HEAD( &this->buffer_free );
        INIT_LIST_HEAD( &this->reserve_list );
        this->reserve_cnt = 0;
        this->cur_buffer_num = 0;
        this->drop_task_num = 0;
        this->max_used_buffer_num = 0;
        this->buffer_flag = 0;

        if( ( this->max_th_limit != 0 ) && ( this->max_th_num >= this->max_th_limit ) ) {
                fprintf( this->fd_log, "thread_pool exceed max_thread_num limit [%d]\n", this->max_th_limit );
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        }

        atomic_set( &this->exec_cnt, 0 );
        atomic_set( &this->cur_th_num, this->min_th_num );
        atomic_set( &this->busy_th_num, 0 );
        atomic_set( &this->th_concurrent_flag, TPFALSE );

        if( conf->max_buffer_num <= 0 ) {
                this->max_buffer_num = 0;
                this->buffer = NULL;
        } else {
                this->max_buffer_num = conf->max_buffer_num;
                this->buffer = malloc( sizeof( tp_work_internal ) * this->max_buffer_num );

                if( this->buffer == NULL ) {
                        fprintf( this->fd_log, "threadpool buffer create failed\n" );
                        pthread_mutex_unlock( &this->tp_lock );
                        return TPFALSE;
                } else {
                        for( i = 0; i < this->max_buffer_num; ++i ) {
                                list_add_tail( &this->buffer[i].list, &this->buffer_free );
                        }
                }
        }

        //malloc mem for num thread info struct
        /* if(NULL != this->thread_info)
            free(this->thread_info); */
        this->thread_info = ( tp_thread_info* )malloc( sizeof( tp_thread_info ) * this->max_th_num );

        if( this->thread_info == NULL ) {
                fprintf( this->fd_log, "thread_info malloc failed:%m\n" );

                if( this->fd_log != stderr ) {
                        fclose( this->fd_log );
                        this->fd_log = stderr;
                }

                free( this->buffer );
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        } else {
                memset( this->thread_info, 0, sizeof( tp_thread_info )*this->max_th_num );
                /*
                INIT_LIST_HEAD( &this->thread_busy );
                INIT_LIST_HEAD( &this->thread_free );
                */
        }

        //creat work thread and init work thread info
        for( i = 0; i < this->min_th_num; i++ ) {
                sem_init( &this->thread_info[i].sem_lock,  0, 0 );
                this->thread_info[i].head = this;

                if( 0 != pthread_create( &this->thread_info[i].thread_id, &this->attr, tp_work_thread, &this->thread_info[i] ) ) {
                        fprintf( this->fd_log, "tp_init: creat work thread failed [%d]\n", i );
                        break;
                }

                list_add_tail( &this->thread_info[i].original_list, &this->original_head );
                list_add_tail( &this->thread_info[i].list[this->freelist_flag], this->thread_free );
        }

        if( i != this->min_th_num ) {
CLEAN:

                for( ; i-- > 0; ) {
                        pthread_kill( this->thread_info[i].thread_id, TPTHREAD_STOP_SIGNAL );
                        sem_destroy( &this->thread_info[i].sem_lock );
                }

                free( this->thread_info );
                fclose( this->fd_log );
                free( this->buffer );
                pthread_mutex_unlock( &this->tp_lock );
                return TPFALSE;
        } else {
                for( ; i < this->max_th_num; ++i ) {
                        this->thread_info[i].head = this;
                        list_add_tail( &this->thread_info[i].original_list, &this->thread_unused );
                }
        }

        //creat manage thread
        if( 0 != pthread_create( &this->manage_thread_id, &this->attr, tp_manage_thread, this ) ) {
                fprintf( this->fd_log, "tp_init: creat manage thread failed\n" );
                goto CLEAN;
        }

        //fprintf( this->fd_log, "tp_init: creat manage thread %lu\n", this->manage_thread_id );
        atomic_set( &this->th_flag, TPTRUE );
        atomic_set( &this->th_init_flag, TPTRUE );
        pthread_spin_init( &this->temp_lock, PTHREAD_PROCESS_SHARED );
        pthread_mutex_init( &this->buffer_lock, NULL );
        fprintf( this->fd_log, "tp_init: end\n" );
        pthread_mutex_unlock( &this->tp_lock );
        return TPTRUE;
}
/*
static void tp_reset( tp_thread_pool *pool )
{
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        tp_work_internal *temp;
        tp_thread_info *tmp;
        pthread_mutex_lock( &this->tp_lock );

        if( atomic_read( &this->th_init_flag ) ==  TPFALSE ) {
                fprintf( stderr, "thread_pool has not be init\n" );
                pthread_mutex_unlock( &this->tp_lock );
                return;
        }

        atomic_set( &this->th_flag, TPFALSE );
        int i = 0;
        fprintf( this->fd_log, "tp_reset: reset thread_pool \n" );

        if( atomic_read( &this->busy_th_num ) != 0 ) {


                while( !list_empty( &this->thread_busy ) ) {
                        tmp = list_entry( this->thread_busy.next, tp_thread_info, list );
                        pthread_kill( tmp->thread_id, TPTHREAD_STOP_SIGNAL );
                        pthread_mutex_destroy( &tmp->thread_lock );
                        pthread_cond_destroy( &tmp->thread_cond );

                        if( 0 != pthread_create( &tmp->thread_id, &this->attr, tp_work_thread, tmp ) ) {
                                ++i;
                                list_del( &tmp->list );
                                list_add_tail( &tmp->list, &this->thread_unused );
                                atomic_dec( &this->cur_th_num );
                                continue;
                        }

                        pthread_cond_init( &tmp->thread_cond, NULL );
                        pthread_mutex_init( &tmp->thread_lock, NULL );
                        list_del( &tmp->list );
                        list_add_tail( &tmp->list, &this->thread_free );
                        atomic_dec( &this->busy_th_num );
                }

                if( i != 0 )
                        printf( "tp_reset: reset failed times [%d]\n", i );

                while( !list_empty( &this->buffer_busy ) ) {
                        temp = list_entry( this->buffer_busy.next, tp_work_internal, list );

                        if( temp->len != 0 )
                                free( temp->para );

                        list_del( &temp->list );
                        list_add_tail( &temp->list, &this->buffer_free );
                }
        }

        atomic_set( &this->th_flag, TPTRUE );
        pthread_mutex_unlock( &this->tp_lock );
}
*/


/**
  * member function reality. thread pool entirely close function.
  * para:
  * 	this: thread pool struct instance ponter
  * return:
  */
static void tp_close( tp_thread_pool *pool )
{
        struct timeval timeout = {0, 500000 };
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        tp_thread_info *temp;
        pthread_mutex_lock( &this->tp_lock );

        if( atomic_read( &this->th_init_flag ) ==  TPFALSE ) {
                fprintf( stderr, "thread_pool has not be init\n" );
                return;
        }

        atomic_set( &this->th_flag, TPFALSE );

        if( this->fd_log != stderr )
                fprintf( this->fd_log, "tp_close: start \n" );

        this->exit_flag = 0;
        tp_print_status_to_log( this );
        //必须先杀死管理线程，否则会出问题
        pthread_kill( this->manage_thread_id, TPTHREAD_STOP_SIGNAL );

        if( atomic_read( &this->busy_th_num ) != 0 )
                delay_time( &timeout );

        //close work thread
        list_for_each_entry( temp, &this->original_head, original_list ) {
                pthread_kill( temp->thread_id, TPTHREAD_STOP_SIGNAL );
                sem_destroy( &temp->sem_lock );
        }
        //free thread struct
        free( this->thread_info );
        this->thread_info = NULL;

        if( this->buffer != NULL ) {
                free( this->buffer );
                this->buffer = NULL;
        }

        pthread_spin_destroy( &this->temp_lock );
        pthread_mutex_destroy( &this->buffer_lock );
        atomic_set( &this->th_init_flag, TPFALSE );

        if( this->fd_log != stderr ) {
                fprintf( this->fd_log, "tp_close: end\n########################################\n" );
                fclose( this->fd_log );
                this->fd_log = stderr;
        }

        pthread_mutex_unlock( &this->tp_lock );
        pthread_attr_destroy( &this->attr );
}

/**
  * member function reality. main interface opened.
  * after getting own worker and job, user may use the function to process the task.
  * para:
  * 	this: thread pool struct instance ponter
  *	worker: user task reality.
  *	job: user task para
  * return:
  */
/* static TPBOOL tp_process_job( tp_thread_pool *pool, tp_work *worker, tp_work_desc *job ) */
static TPBOOL tp_process_job( tp_thread_pool *pool, tp_work *worker )
{
        tp_work_internal *temp;
        tp_thread_info *tmp;
        /* int tmpid; */
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;

        if( atomic_read( &this->th_flag ) == TPFALSE )
                return TPFALSE;

        tmp = get_free_thread( this );

        //pthread_mutex_lock( &this->tp_lock );
        if( tmp != NULL ) {
                tmp->th_work = *worker;

                if( worker->len != 0 ) {
                        switch( worker->type ) {
                                case TP_MALLOC:
                                        break;
                                case TP_STATIC:
                                default:

                                        if( worker->para != NULL ) {
                                                if( ( tmp->th_work.para = malloc( worker->len ) ) == NULL ) {
                                                        //unset_bit( this->id_bitmap, i + 1 );
                                                        fprintf( this->fd_log, "tp_process_job: idle working thread %lu, malloc para failed\n", tmp->thread_id );
                                                        pthread_spin_lock( &this->temp_lock );
                                                        list_add_tail( &tmp->list[this->freelist_bak_flag], this->thread_free_bak );
                                                        pthread_spin_unlock( &this->temp_lock );
                                                        atomic_dec( &this->busy_th_num );
                                                        return TPFALSE;
                                                } else
                                                        memcpy( tmp->th_work.para, worker->para, worker->len );
                                        }

                                        break;
                        }
                }

                //fprintf( this->fd_log, "tp_process_job: informing idle working thread %lu\n", tmp->thread_id );
                sem_post( &tmp->sem_lock );
                return TPTRUE;
        } else {		// insert task buffer
                if( this->buffer == NULL ) {
                        ++this->drop_task_num;
                        return TPFALSE;
                }

                pthread_mutex_lock( &this->buffer_lock );

                if( !list_empty( &this->buffer_free ) ) {
                        temp = list_entry( this->buffer_free.next, tp_work_internal, list );
                        *( tp_work * )temp  = *worker;

                        if( worker->len != 0 ) {
                                switch( worker->type ) {
                                        case TP_MALLOC:
                                                break;
                                        case TP_STATIC:
                                        default:

                                                if( worker->len != 0 && worker->para != NULL ) {
                                                        if( ( temp->para = malloc( worker->len ) ) == NULL ) {
                                                                fprintf( this->fd_log, "tp_process_job: add worker into buffer, malloc para failed\n" );
                                                                pthread_mutex_unlock( &this->buffer_lock );
                                                                return TPFALSE;
                                                        } else
                                                                memcpy( temp->para, worker->para, worker->len );
                                                }

                                                break;
                                }
                        }

                        list_del( &temp->list );
                        list_add( &temp->list, &this->buffer_busy );

                        if( this->max_used_buffer_num < ++this->cur_buffer_num )
                                this->max_used_buffer_num = this->cur_buffer_num;
                } else {
                        ++this->drop_task_num;

                        if( ( worker->type == TP_MALLOC ) && ( worker->para != NULL ) )
                                free( worker->para );
                }

                this->buffer_flag = 1;
                pthread_mutex_unlock( &this->buffer_lock );
                return TPTRUE;
        }
}


/**
  * internal interface. real work thread.
  * para:
  * 	pthread: thread pool struct pointer
  * return:

  */
static void *tp_work_thread( void *pthread )
{
        //pthread_t curid;//current thread id
        tp_thread_info *tp_self = ( tp_thread_info * )pthread; //main thread pool struct instance
        //	enable signal SIGUSR2 can be process
        sigset_t sig_mask;
        tp_work *work = &tp_self->th_work;
        sigfillset( &sig_mask );
        sigdelset( &sig_mask, TPTHREAD_STOP_SIGNAL );
        pthread_sigmask( SIG_SETMASK, &sig_mask, NULL );
        signal( TPTHREAD_STOP_SIGNAL, catch_quit );
        //get current thread id
        //curid = pthread_self();
        tp_thread_pool_internal *this = tp_self->head;
        //get current thread's seq in the thread info struct array.
        //nseq = tp_get_thread_by_id( this, curid );//这个只有第一次会影响效率，以后处理就都在循环里面了
        //fprintf( tp_self->head->fd_log, "entering working thread %lu\n", curid );

        //wait cond for processing real job.
        while( this->exit_flag ) {
                sem_wait( &tp_self->sem_lock );
                //process
                /* work->process_job( work, job ); */
                atomic_inc( &this->exec_cnt );
                work->process_job( work );
                //thread state be set idle after work

                if( ( work->para != NULL ) ) {
                        free( work->para );
                }

                //fprintf( tp_self->head->fd_log, "\n%lu do work over\n", curid );
                //维护
                pthread_spin_lock( &this->temp_lock );
                list_add_tail( &tp_self->list[this->freelist_bak_flag], this->thread_free_bak );
                pthread_spin_unlock( &this->temp_lock );
                atomic_dec( &this->busy_th_num );
        }

        return NULL;
}



/**
  * member function reality. add new thread into the pool.
  * 调用者来维持锁，因此这里不需要考虑互斥问题
  * para:
  * 	this: thread pool struct instance ponter
  * return:
  * 	true: successful; false: failed
  */

static TPBOOL tp_add_thread( tp_thread_pool_internal *this )
{
        int i = 0 ;
        int number = this->min_spare_th_num;
        tp_thread_info *new_thread;
        struct list_head head;
        struct list_head tmp[2];
        INIT_LIST_HEAD( &head );
        INIT_LIST_HEAD( &tmp[0] );
        INIT_LIST_HEAD( &tmp[1] );

        if( !list_empty( &this->reserve_list ) ) {
                i = this->reserve_cnt;
                amend_reserve( &this->reserve_list, &tmp[0], &tmp[1] );
                pthread_mutex_lock( &this->tp_lock );
                list_splice_init( &this->reserve_list, &this->original_head );
                list_splice_init( &tmp[this->freelist_flag], this->thread_free );
                atomic_add( &this->cur_th_num, this->reserve_cnt );
                this->reserve_cnt = 0;
                pthread_mutex_unlock( &this->tp_lock );
                fprintf( this->fd_log, "tp_add_thread: add %d work thread\n", i );
        } else {
                for( ; i < number && !list_empty( &this->thread_unused ); ++i ) {
                        //malloc new thread info struct
                        new_thread = list_entry( this->thread_unused.next, tp_thread_info, original_list );
                        new_thread->head = this;
                        sem_init( &new_thread->sem_lock, 0, 0 );

                        if( 0 != pthread_create( &new_thread->thread_id, &this->attr, tp_work_thread, new_thread ) ) {
                                sem_destroy( &new_thread->sem_lock );
                                break;
                        }

                        list_del( &new_thread->original_list );
                        list_add_tail( &new_thread->original_list, &head );
                        list_add_tail( &new_thread->list[0], &tmp[0] );
                        list_add_tail( &new_thread->list[1], &tmp[1] );
                }

                pthread_mutex_lock( &this->tp_lock );
                list_splice( &head, &this->original_head );
                list_splice( &tmp[this->freelist_flag], this->thread_free );
                atomic_add( &this->cur_th_num, i );
                pthread_mutex_unlock( &this->tp_lock );
                fprintf( this->fd_log, "tp_add_thread: add %d work thread\n", i );
        }

        return TPTRUE;
}

/**
  * member function reality. delete idle thread in the pool.
  * only delete last idle thread in the pool.
  * para:
  * 	this: thread pool struct instance ponter
  * return:
  * 	true: successful; false: failed
  */
static TPBOOL tp_delete_thread( tp_thread_pool_internal *this )
{
        //current thread num can't < min thread num
        int i = 0, cnt = 0, flag;
        struct list_head tmp_head;
        INIT_LIST_HEAD( &tmp_head );
        tp_thread_info *temp;
        i = atomic_read( &this->cur_th_num ) - this->min_th_num;

        if( i <= 0 ) return TPFALSE;

        cnt = ( i > this->min_spare_th_num ) ? this->min_spare_th_num : i;
        //if last thread is busy, do nothing (because thread_info is not successful)
        pthread_mutex_lock( &this->tp_lock );
        flag = this->freelist_flag;

        for( i = 0; i < cnt ; ++i ) {
                temp = list_entry( this->thread_free->next, tp_thread_info, list[this->freelist_flag] );
                //after deleting idle thread, current thread num -1
                list_del( &temp->list[flag] );
                list_add_tail( &temp->original_list, &tmp_head );

                if( list_empty( this->thread_free ) ) {
                        if( exchange_freelist( this ) == NULL )	//都为空
                                break;

                        flag =  this->freelist_flag;
                }
        }

        pthread_mutex_unlock( &this->tp_lock );
        atomic_sub( &this->cur_th_num, i );

        if( list_empty( &this->reserve_list ) ) {
                list_splice( &tmp_head, &this->reserve_list );
                this->reserve_cnt = i;
                fprintf( this->fd_log, "tp_delete_threap: reserve %d thread\n", i );
        } else {
                list_for_each_entry( temp , &tmp_head, original_list ) {
                        pthread_kill( temp->thread_id , TPTHREAD_STOP_SIGNAL );
                        //pthread_cancel( temp->thread_id );
                        sem_destroy( &temp->sem_lock );
                }
                fprintf( this->fd_log, "tp_delete_threap: del %d thread\n", i );
        }

        return TPTRUE;
}

/**
  * get current thread pool status, return to caller
  *
  * para:
  * 	pool: thread pool struct instance ponter
  *    ret  : tp_status struct ,be provided by caller
  * return:
  * 	true: successful; false: failed
  */
static TPBOOL tp_get_status( tp_thread_pool *pool, tp_status *ret )
{
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;

        if( atomic_read( &this->th_flag ) == TPFALSE )
                return TPFALSE;

        ret->cur_th_num = atomic_read( &this->cur_th_num );
        ret->busy_th_num = atomic_read( &this->busy_th_num );
        ret->concurrent_num = atomic_read( &this->concurrent_num );
        ret->drop_task_num = this->drop_task_num;
        ret->max_buffer_num = this->max_buffer_num;
        ret->cur_buffer_num = this->cur_buffer_num;
        ret->max_used_buffer_num = this->max_used_buffer_num;
        ret->stack_size = this->stacksize;
        ret->exec_cnt = atomic_read( &this->exec_cnt );
        return TPTRUE;
}
/**
  * member function reality. get current thread pool status:idle, normal, busy, .etc.
  * para:
  * 	this: thread pool struct instance ponter
  * return:
  * 	0: idle; 1: normal or busy(don't process)
  */
static int  tp_get_tp_status( tp_thread_pool_internal *this )
{
        int i;
        //get busy thread number
        //fprintf( this->fd_log, "<- current busy thread in thread_pool:  %d ->\n", busy_num );

        /*
                i = atomic_read( &this->cur_th_num ) -  atomic_read( &this->busy_th_num );

                if( i < this->min_spare_th_num )
                        return 1;	// add thread to th_pool
                else if( i > this->max_spare_th_num )
                        return 2;	// kill thread from th_pool
                else
                        return 0;
        */

        switch( this->type ) {
                case TP_NORMAL:
                        i = atomic_read( &this->cur_th_num ) -  atomic_read( &this->busy_th_num );

                        if( i < this->min_spare_th_num )
                                return 1;	// add thread to th_pool
                        else if( i > this->max_spare_th_num )
                                return 2;	// kill thread from th_pool
                        else
                                return 0;

                        break;
                case TP_CREATE_ONLY:
                        i = atomic_read( &this->cur_th_num ) -  atomic_read( &this->busy_th_num );

                        if( i < this->min_spare_th_num )
                                return 1;
                        else
                                return 0;

                        break;
                case TP_NO_ALL:
                default:
                        return 0;
        }
}


/**
  * internal interface. manage thread pool to delete idle thread.
  * para:
  * 	pthread: thread pool struct ponter
  * return:
  */
static void *tp_manage_thread( void *pthread )
{
        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pthread; //main thread pool struct instance
        int ret, timerfd;
        struct itimerspec new_value;
        uint64_t exp;
        tp_work_internal *temp;
        sigset_t sig_mask;
        sigfillset( &sig_mask );
        const struct timespec timeout = {MANAGE_INTERVAL, 0};
        /* sigdelset( &sig_mask, SIGUSR2 ); */
        sigdelset( &sig_mask, TPTHREAD_STOP_SIGNAL );
        pthread_sigmask( SIG_SETMASK, &sig_mask, NULL );
        //1?
        new_value.it_value.tv_sec = MANAGE_INTERVAL;
        new_value.it_value.tv_nsec = 0;
        new_value.it_interval.tv_sec = MANAGE_INTERVAL;
        new_value.it_interval.tv_nsec = 0;

        if( ( timerfd = timerfd_create( CLOCK_REALTIME, 0 ) ) == -1 ) {
                perror( "tp : create timerfd failed" );
                exit( -1 );
        }

        if( timerfd_settime( timerfd, 0, &new_value, NULL ) == -1 ) {
                perror( "tp : timer_set failed" );
                exit( -1 );
        }

        //sleep( MANAGE_INTERVAL );

        do {
                ret = tp_get_tp_status( this );

                switch( ret ) {
                        case 1:
                                //pthread_mutex_lock( &this->tp_lock );
                                tp_add_thread( this );
                                //pthread_mutex_unlock( &this->tp_lock );
                                continue;
                        case 2:
                                //pthread_mutex_lock( &this->tp_lock );
                                tp_delete_thread( this );
                                //pthread_mutex_unlock( &this->tp_lock );
                                continue;
                        default:
                                break;
                }//end for if

                if( this->buffer_flag == 1 ) {	// have task in buffer
                        if( pthread_mutex_timedlock( &this->buffer_lock, &timeout ) == 0 ) {
                                ret = 0;

                                while( !list_empty( &this->buffer_busy ) ) {
                                        if( ( atomic_read( &this->th_concurrent_flag ) == TPTRUE ) && atomic_read( &this->busy_th_num ) >= atomic_read( &this->concurrent_num ) )
                                                break;

                                        temp = list_entry( this->buffer_busy.next, tp_work_internal, list );

                                        if( tp_process_buffer( this, temp ) == TPTRUE ) {
                                                list_del( &temp->list );
                                                list_add( &temp->list, &this->buffer_free );

                                                if( --this->cur_buffer_num == 0 )
                                                        this->buffer_flag = 0;
                                        } else {
                                                break;
                                        }
                                }

                                pthread_mutex_unlock( &this->buffer_lock );
                        }

                        continue;
                }

                //1?
                if( read( timerfd, &exp, sizeof( uint64_t ) ) != sizeof( uint64_t ) ) {
                        perror( "tp : read failed" );
                }
        } while( this->exit_flag );

        return NULL;
}

static inline void tp_enable( tp_thread_pool *pool )
{
        if( pool == NULL )
                return;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        pthread_mutex_lock( &this->tp_lock );
        atomic_set( &this->th_flag, TPTRUE );

        if( this->fd_log != stderr )
                fprintf( this->fd_log, "tp_enable: enable thread_pool \n" );

        pthread_mutex_unlock( &this->tp_lock );
}
static inline void tp_disable( tp_thread_pool *pool )
{
        if( pool == NULL )
                return;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        pthread_mutex_lock( &this->tp_lock );
        atomic_set( &this->th_flag, TPFALSE );

        if( this->fd_log != stderr )
                fprintf( this->fd_log, "tp_disable: disable thread_pool \n" );

        pthread_mutex_unlock( &this->tp_lock );
}


static int tp_setlog( tp_thread_pool *pool, const char *log_file )
{
        if( pool == NULL )
                return 0;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        FILE *temp;

        if( atomic_read( &this->th_init_flag ) == 0 )
                return 0;

        pthread_mutex_lock( &this->tp_lock );

        if( log_file == NULL ) {
                temp = stderr;
                fprintf( this->fd_log, "tp_setlog: stderr \n" );
                this->fd_log = temp;
        } else  {
                temp = fopen( log_file, "a+" );

                if(	temp == NULL ) {
                        fprintf( this->fd_log, "tp_setlog: set log - %s error, drop it \n", log_file );
                } else {
                        if( this->fd_log != stderr ) {
                                fprintf( this->fd_log, "tp_setlog: %s \n", log_file );
                                fclose( this->fd_log );
                        }

                        this->fd_log = temp;
                }
        }

        pthread_mutex_unlock( &this->tp_lock );
        return 1;
}
static inline int delay_time( struct timeval *timeout )
{
        if( timeout  ==  NULL )
                return -1;

        if( timeout->tv_sec  ==  0  &&  timeout->tv_usec  ==  0 )
                return 0;

        return select( 1,  NULL,  NULL, NULL,  timeout );
}

//获取的空闲线程，默认都认为是用于调度，设置成繁忙属性
__inline__ __attribute__( ( always_inline ) ) static tp_thread_info *get_free_thread( tp_thread_pool_internal *this )
{
        tp_thread_info *tmp = NULL;

        if( ( atomic_read( &this->th_concurrent_flag ) == TPTRUE ) && atomic_read( &this->busy_th_num ) >= atomic_read( &this->concurrent_num ) )  {
                return NULL;
        }

        pthread_mutex_lock( &this->tp_lock );

        if( list_empty( this->thread_free ) ) {
                if( exchange_freelist( this ) == NULL ) {
                        pthread_mutex_unlock( &this->tp_lock );
                        return NULL;
                }
        }

        tmp = list_entry( this->thread_free->next, tp_thread_info, list[this->freelist_flag] );
        list_del( &tmp->list[this->freelist_flag] );
        //list_add_tail( &tmp->list, &this->thread_busy );
        /*
                        if( !list_empty( &this->thread_unused ) ) {	// 仍然有可用的线程
                                tmp = list_entry( this->thread_unused.next, tp_thread_info, list );
                                pthread_cond_init( &tmp->thread_cond, NULL );
                                pthread_mutex_init( &tmp->thread_lock, NULL );

                                if( 0 != pthread_create( &tmp->thread_id, &this->attr, tp_work_thread, tmp ) ) {
                                        list_del( &tmp->list );
                                        list_add_tail( &tmp->list, &this->thread_busy );
                                        atomic_inc( &this->cur_th_num );
                                } else {
                                        pthread_cond_destroy( &tmp->thread_cond );
                                        pthread_mutex_destroy( &tmp->thread_lock );
                                        pthread_mutex_unlock( &this->tp_lock );
                                        return NULL;
                                }
                        }
        */
        atomic_inc( &this->busy_th_num );
        pthread_mutex_unlock( &this->tp_lock );
        return tmp;
}

__inline__ __attribute__( ( always_inline ) ) static TPBOOL tp_process_buffer( tp_thread_pool_internal *this, tp_work_internal *worker )
{
        tp_thread_info *tmp;

        if( !list_empty( this->thread_free ) ) {
                tmp = list_entry( this->thread_free->next, tp_thread_info, list[this->freelist_flag] );
                list_del( &tmp->list[this->freelist_flag] );
                //list_add_tail( &tmp->list, &this->thread_busy );
                atomic_inc( &this->busy_th_num );
                tmp->th_work = *( tp_work * )worker;
                sem_post( &tmp->sem_lock );
        }

        return TPTRUE;
}


static TPBOOL tp_set_max_concurrent_num( tp_thread_pool *pool, int num )
{
        if( pool == NULL )
                return TPFALSE;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        pthread_mutex_lock( &this->tp_lock );

        if( this->fd_log != stderr )
                fprintf( this->fd_log, "tp_set_max_concurrent_num: set %d \n", num );

        if( num > 0 && num < this->max_th_num ) {
                atomic_set( &this->th_concurrent_flag, TPTRUE );
                atomic_set( &this->concurrent_num, num );
        } else
                atomic_set( &this->th_concurrent_flag, TPFALSE );

        pthread_mutex_unlock( &this->tp_lock );
        return TPTRUE;
}

// head must be original list
static forceinline void amend_reserve( struct list_head *head, struct list_head *tmp0, struct list_head *tmp1 )
{
        tp_thread_info *temp;
        INIT_LIST_HEAD( tmp0 );
        INIT_LIST_HEAD( tmp1 );
        list_for_each_entry( temp, head, original_list ) {
                list_add( &temp->list[0], tmp0 );
                list_add( &temp->list[1], tmp1 );
        }
}


//默认已经加锁高层的mutex锁,因此绝对不可能两个线程同时到达这里
static forceinline void *exchange_freelist( tp_thread_pool_internal *this )
{
        int i, j;
        pthread_spin_lock( &this->temp_lock );

        if( list_empty( this->thread_free_bak ) ) {
                pthread_spin_unlock( &this->temp_lock );
                return NULL;
        }

        j = this->freelist_bak_flag;
        i = this->freelist_flag;
        this->freelist_flag = j;
        this->freelist_bak_flag = i;
        this->thread_free = &this->freelist[j];
        this->thread_free_bak = &this->freelist[i];
        pthread_spin_unlock( &this->temp_lock );
        return this->thread_free;
}

static int tp_settype( tp_thread_pool *pool, tp_type type )
{
        if( pool == NULL )
                return -1;

        tp_thread_pool_internal *this = ( tp_thread_pool_internal * )pool;
        this->type = type;
        return 0;
}
