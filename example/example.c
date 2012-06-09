/*
 * =====================================================================================
 *
 *       Filename:  example.c
 *
 *    Description:  example
 *
 *        Version:  1.0
 *        Created:  2011年06月20日 20时46分12秒
 *       Revision:  none
 *       Compiler:  gcc
 *
 *         Author:  YOUR NAME (tangfu),
 *        Company:
 *
 * =====================================================================================
 */

#include "thread-pool.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
void *task1( tp_work *work )
{
        int i = 3;

        while( i ) {
                printf( "%d\n", --i );
                sleep( 1 );
        }

        return NULL;
}

int main()
{
        size_t size;
        pthread_attr_t attr;
        pthread_attr_init( &attr );
        tp_thread_pool *ttp = create_thread_pool();
        tp_thread_conf conf = {20, 100, 10, 50, 0, 0};
        //tp_thread_conf conf = {500, 1000, 10, 50, 0,0};
        pthread_attr_getstacksize( &attr, &size );
        printf( "stack size: %u\n", size );
        ttp->init( ttp, &conf, "log.txt" );
        tp_work work;
        work.process_job = task1;
        work.para = NULL;
        work.len = 0;
        ttp->process_job( ttp, &work );
        sleep( 5 );
        destroy_thread_pool( ttp );
        return 0;
}
