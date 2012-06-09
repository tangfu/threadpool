CC:=gcc
FLAGS:= -g3 -Wall -Wextra -pg
LIBLDFLAGS:=
LIBTEST:=-lthreadpool -lpthread

ALL: 
	@-astyle -n --style=linux --mode=c --pad-oper --pad-paren-in --unpad-paren --break-blocks --delete-empty-lines --min-conditional-indent=0 --max-instatement-indent=80 --indent-col1-comments --indent-switches --lineend=linux *.{c,h} >/dev/null
	@$(CC) -c $(FLAGS) thread-pool.c $(LIBLDFLAGS)
	@ar -rc libthreadpool.a thread-pool.o
	@rm *.o
#	@gcc thread-pool.c -fPIC -shared -o libthreadpool.so
	@make -C test
	@make -C example
#	produce document
	@doxygen

release: 
	@$(CC) -c -Wall -Wextra thread-pool.c $(LIBLDFLAGS)
	@ar -rc libthreadpool.a thread-pool.o
	@rm *.o

clean:
	@if [ -f libthreadpool.a ];then \
		rm libthreadpool.a example/example test/test 2>/dev/null; \
		rm -rf doc/html/* 2>/dev/null; \
	fi
