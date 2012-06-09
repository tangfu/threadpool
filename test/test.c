#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "thread-pool.h"
#include <stdarg.h>
#include <setjmp.h>
#include <cmockery.h>
#include <signal.h>

tp_thread_pool *ttp, *ttp2;

void task()
{
        printf( "task\n" );
}

void *task2( tp_work *this )
{
        printf( "task2\n" );
        //	sleep(3);
        //	return NULL;
        //	return (void *)mock();
        return NULL;
}

void test_init( void **state )
{
        tp_thread_conf conf = {20, 100, 10, 50, 0, 0}, conf2 = {50, 40, 10, 20, 0, 0};
        ttp = create_thread_pool();
        assert_false( ttp == NULL );
        assert_int_equal( ttp->init( ttp, &conf, "log.txt" ), TPTRUE );
        assert_int_equal( ttp->init( ttp, &conf, "log.txt" ), TPFALSE );
        ttp->close( ttp );
        assert_int_equal( ttp->init( ttp, &conf, "log.txt" ), TPTRUE );
        ttp->close( ttp );
        assert_int_equal( ttp->init( ttp, &conf2, "log.txt" ), TPFALSE );
        assert_int_equal( ttp->init( ttp, &conf, "log.txt" ), TPTRUE );
}

void test_run( void **state )
{
        tp_work work;
        work.process_job = task2;
        work.para = NULL;
        work.len = 0;
        /* will_return(task2, 0x70342302); */
        assert_int_equal( ttp->process_job( ttp, &work ), TPTRUE );
}

int main()
{
        /*
        	tp_thread_conf conf = {20,100,10,50};
        	ttp = creat_thread_pool();
        	ttp->init(ttp,&conf,"log.txt");
        	tp_work work;
        	work.process_job = task2;

        	sleep(3);
        	ttp->process_job(ttp,&work,NULL);

        	sleep(10);
        	fprintf(stderr,"success");
        	destroy_thread_pool(ttp);
        	return 1;
        */
        /*
        sigset_t sig_mask;
        sigfillset( &sig_mask );
        sigdelset( &sig_mask, SIGINT );
        sigprocmask( SIG_SETMASK, &sig_mask, NULL );
        */
        UnitTest tests[] = {
                unit_test( test_init ),
                unit_test( test_run )
        };
        return run_tests( tests );
}
