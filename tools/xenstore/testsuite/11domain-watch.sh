#! /bin/sh
# Test watching from a domain.

# Watch something, write to it, check watch has fired.
[ "`echo -e 'write /test create contents' | ./xs_test 2>&1`" = "" ]
[ "`echo -e 'mkdir /dir' | ./xs_test 2>&1`" = "" ]

[ "`echo -e 'introduce 1 100 7 /my/home
1 watch /test token
async write /test create contents2
1 waitwatch
1 ackwatch token
1 unwatch /test token
asyncwait
release 1' | ./xs_test 2>&1`" = "handle is 1
1:/test:token" ]

# ignore watches while doing commands, should work.
[ "`echo -e 'introduce 1 100 7 /my/home
1 watch /dir token
async write /dir/test create contents
1 write /dir/test2 create contents2
1 write /dir/test3 create contents3
1 write /dir/test4 create contents4
1 waitwatch
1 ackwatch token
asyncwait
release 1' | ./xs_test 2>&1`" = "handle is 1
1:/dir/test:token" ]

# unwatch
[ "`echo -e 'introduce 1 100 7 /my/home
1 watch /dir token1
1 unwatch /dir token1
1 watch /dir token2
async 2 write /dir/test2 create contents
1 waitwatch
1 unwatch /dir token2
asyncwait
release 1' | ./xs_test 2>&1`" = "handle is 1
1:/dir/test2:token2" ]

# unwatch while watch pending.
[ "`echo -e 'introduce 1 100 7 /my/home
introduce 2 101 8 /my/secondhome
1 watch /dir token1
2 watch /dir token2
3 async write /dir/test create contents
2 unwatch /dir token2
1 waitwatch
1 ackwatch token1
release 1
release 2' | ./xs_test 2>&1`" = "handle is 1
handle is 2
1:/dir/test:token1" ]
