#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#define _GNU_SOURCE
#define __USE_GNU
#include <netinet/in.h>
#include <termios.h>
#include <sys/epoll.h>
#include <assert.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <sys/ioctl.h>
#include <light_api.h>

#ifndef __linux__
#ifdef __FreeBSD__
#include <sys/socket.h>
#else
#include <net/socket.h>
#endif
#endif
#include <sys/time.h>
#include <sched.h>
// #include <fcntl.h>
#include <sys/syscall.h>
#include <dlfcn.h>
#include <stdarg.h>
#include <unistd.h>

#define RTE_ARG_COUNT 7
#define APP_LOG_LEVEL 2
// #define LIGHT_DEBUG
#define OPEN_FAST_PATH

#define F_DUPFD		0	/* dup */
#define F_GETFD		1	/* get close_on_exec */
#define F_SETFD		2	//set/clear close_on_exec 
#define F_GETFL		3	/* get file->f_flags */
#define F_SETFL		4	/* set file->f_flags */

static void light_init() __attribute__((constructor));
static int light_sock_enable = 1;
static int inited = 0;
int file_fd = -1;
int file_prefd = -1;
int file_open = 0; //whether it has been open.
int file_read = 0; //whether it has been read.
char file_content[100];

static int (*real_open)(const char *, int, ...);
static int (*real_open64)(const char *, int, ...);
static int (*real_openat)(int, const char, int, ...);
static int (*real_close)(int);
static int (*real_socket)(int, int, int);
static int (*real_bind)(int, const struct sockaddr*, socklen_t);
static int (*real_listen)(int, int);
static int (*real_connect)(int, const struct sockaddr*,socklen_t);
static int (*real_accept)(int, struct sockaddr *, socklen_t *);
static ssize_t (*real_recv)(int, void *, size_t, int);
static ssize_t (*real_send)(int, const void *, size_t, int);
static int (*real_shutdown)(int, int);
static ssize_t (*real_writev)(int, const struct iovec *, int);
static ssize_t (*real_write)(int, const void *, size_t );
static ssize_t (*real_read)(int, void *, size_t );
static ssize_t (*real_pread64)(int, void *, size_t, off_t);
static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static int (*real_ioctl)(int, int, ...);
static int (*real_fcntl)(int, int, ...);
static int (*real_epoll_create)(int);
static int (*real_epoll_ctl)(int, int, int, struct epoll_event *);
static int (*real_epoll_wait)(int, struct epoll_event *, int, int);
static pid_t (*real_fork)();
static ssize_t (*real_recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
static ssize_t (*real_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static int (*real_execl)(const char *path, const char *arg, ...); 
static int (*real_execlp)(const char *file, const char *arg, ...); 
static int (*real_execle)(const char *path, const char *arg, ...);
static int (*real_execv)(const char *path, char *const argv[]); 
static int (*real_execvp)(const char *file, char *const argv[]); 
static int (*real_execvpe)(const char *file, char *const argv[], char *const envp[]);
static int (*real_dup)(int oldfd);
static int (*real_dup2)(int oldfd, int newfd);
static int (*real_dup3)(int oldfd, int newfd, int flags);
static int (*real_pthread_create)(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine) (void *), void *arg);

static void light_init()
{
#define INIT_FUNCTION(func) \
        real_##func = dlsym(RTLD_NEXT, #func); \
        assert(real_##func)

    INIT_FUNCTION(socket);
    INIT_FUNCTION(bind);
    INIT_FUNCTION(connect);
    INIT_FUNCTION(open);
    INIT_FUNCTION(open64);
    INIT_FUNCTION(openat);
    INIT_FUNCTION(close);
    INIT_FUNCTION(listen);
    INIT_FUNCTION(accept);
    INIT_FUNCTION(recv);
    INIT_FUNCTION(send);
    INIT_FUNCTION(shutdown);
    INIT_FUNCTION(writev);
    INIT_FUNCTION(write);
    INIT_FUNCTION(read);
    INIT_FUNCTION(pread64);
    INIT_FUNCTION(setsockopt);
    INIT_FUNCTION(epoll_create);
    INIT_FUNCTION(epoll_ctl);
    INIT_FUNCTION(epoll_wait);
    INIT_FUNCTION(ioctl);
    INIT_FUNCTION(fcntl);
    INIT_FUNCTION(fork);
    INIT_FUNCTION(sendto);
    INIT_FUNCTION(recvfrom);
    INIT_FUNCTION(execl);
    INIT_FUNCTION(execlp);
    INIT_FUNCTION(execle);
    INIT_FUNCTION(execv);
    INIT_FUNCTION(execvp);
    INIT_FUNCTION(execvpe);
    INIT_FUNCTION(dup);
    INIT_FUNCTION(dup2);
    INIT_FUNCTION(dup3);
    INIT_FUNCTION(pthread_create);
#undef INIT_FUNCTION

    int t_argc = RTE_ARG_COUNT;
    char *t_argv[RTE_ARG_COUNT] = {"light_app", "-n", "1", "--proc-type", "secondary", "--syslog", "local7"};
    if (light_sock_enable == 0) {
        fprintf(stderr, "light_sock_enable=%d  0 means disable the light api and adopt traditional api\n",light_sock_enable);
    } else {
        fprintf(stderr, "light_sock_enable=%d  1 means use light_api\n",light_sock_enable);
    }
    int ret = light_app_init(t_argc, t_argv, "app_nginx", APP_LOG_LEVEL);
    if (ret < 0) {
        fprintf(stderr, "light_app_init fail\n");
        return;
    }
    assert(0 == ret);
    file_prefd = real_open64("/srv/www/htdocs/index.html", 2048, 0);
    inited = 1;
}

// static long long socket_times=0;
// static long long socket_time= 0;
// static long long bind_times=0;
// static long long bind_time= 0;
// static long long connect_times=0;
// static long long connect_time= 0;
// static long long close_times=0;
// static long long close_time= 0;
// static long long listen_times=0;
// static long long listen_time= 0;
// static long long accept_times=0;
// static long long accept_time= 0;
// static long long accept4_times=0;
// static long long accept4_time= 0;
// static long long recv_times=0;
// static long long recv_time= 0;
// static long long send_times=0;
// static long long send_time= 0;
// static long long shutdown_times=0;
// static long long shutdown_time= 0;
// static long long writev_times=0;
// static long long writev_time= 0;
// static long long write_times=0;
// static long long write_time= 0;
// static long long read_times=0;
// static long long read_time= 0;
// static long long setsockopt_times=0;
// static long long setsockopt_time= 0;
// static long long epoll_create_times=0;
// static long long epoll_create_time= 0;
// static long long epoll_ctl_times=0;
// static long long epoll_ctl_time= 0;
static long long epoll_wait_times=0;
// static long long epoll_wait_time= 0;
// static long long ioctl_times=0;
// static long long ioctl_time= 0;
// static long long fcntl_times=0;
// static long long fcntl_time= 0;
// static struct timeval tpstart,tpend;

long getCurrentTime()
{
    struct timeval tv;
    gettimeofday(&tv,NULL);
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

#define THRESH 20000000
void print_stastics() {
    // printf("Function\tTimes\tTime\n");
    // printf("socket\t%lld\t%lld\n",socket_times,socket_time );
    // printf("bind\t%lld\t%lld\n",bind_times,bind_time );
    // printf("connect\t%lld\t%lld\n",connect_times,connect_time );
    // printf("close\t%lld\t%lld\n",close_times,close_time );
    // printf("listen\t%lld\t%lld\n",listen_times,listen_time );
    // printf("accept\t%lld\t%lld\n",accept_times,accept_time );
    // printf("accept4\t%lld\t%lld\n",accept4_times,accept4_time );
    // printf("recv\t%lld\t%lld\n",recv_times,recv_time );
    // printf("send\t%lld\t%lld\n",send_times,send_time );
    // printf("shutdown\t%lld\t%lld\n",shutdown_times,shutdown_time );
    // printf("writev\t%lld\t%lld\n",writev_times,writev_time );
    // printf("write\t%lld\t%lld\n",write_times,write_time );
    // printf("read\t%lld\t%lld\n",read_times,read_time );
    // printf("setsockopt\t%lld\t%lld\n",setsockopt_times,setsockopt_time );
    // printf("epoll_create\t%lld\t%lld\n",epoll_create_times,epoll_create_time );
    // printf("epoll_ctl\t%lld\t%lld\n",epoll_ctl_times,epoll_ctl_time );
    // printf("epoll_wait\t%lld\t%lld\n",epoll_wait_times,epoll_wait_time );
    // printf("ioctl\t%lld\t%lld\n",ioctl_times,ioctl_time );
    // printf("fcntl\t%lld\t%lld\n",fcntl_times,fcntl_time );
    // printf("\n\n\n");
}

int socket(int domain, int type, int protocol)
{
    //socket_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter socket, domain = %d, type = %d, protocol = %d, pid = %d\n", domain, type, protocol, getpid());
    #endif
    if((0 == light_sock_enable)||(0 == inited)) {
        fprintf(stderr, "  use real_socket\n");
        ret = real_socket(domain, type, protocol);
    } else {
        ret = light_open_socket(domain, type, protocol);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  socket return value = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //socket_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen)
{
    //bind_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter bind, sockfd = %d, pid = %d\n", sockfd, getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        fprintf(stderr, "  use real_bind\n");
        ret = real_bind(sockfd,addr, addrlen);
    } else {
        //API needs adjusting const
        ret = light_bind(sockfd, addr, addrlen);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  bind return value = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //bind_time +=1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int getsockname (__attribute__((unused))int __fd, __attribute__((unused))struct  sockaddr*  __addr, __attribute__((unused)) socklen_t *__restrict __len)
{
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "!!!Warning!!! Enter getsockname, not supported, __fd = %d\n", __fd);
    #endif
    return -1;
}


int connect (__attribute__((unused))int __fd, __attribute__((unused))const struct  sockaddr* __addr, __attribute__((unused)) socklen_t __len)
{
    //connect_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "@ Enter connect, incompelete, fd = %d\n", __fd);
    #endif
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_connect(__fd,__addr,__len);
    }else{
        //API needs adjusting const
        ret = light_connect(__fd,__addr, __len);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  connect return value = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //connect_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int getpeername (__attribute__((unused))int __fd, __attribute__((unused)) struct  sockaddr* __addr, __attribute__((unused)) socklen_t *__restrict __len)
{
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "!!!Warning!!! Enter getpeername, not supported, fd = %d\n", __fd);
    #endif
    return -1;
}

 int getsockopt(__attribute__((unused))int sockfd, __attribute__((unused))int level, __attribute__((unused))int optname,
        __attribute__((unused))void *optval, __attribute__((unused))socklen_t *optlen)
{
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "!!!Warning!!! Enter getsockopt, not supported, sockfd = %d\n", sockfd);
    #endif
    return -1;
}

int setsockopt(int sockfd, int level, int optname, const void *optval, socklen_t optlen)
{
    //setsockopt_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "@ Enter setsockopt, sockfd = %d, level = %d, optname = %d, pid = %d\n", sockfd, level, optname, getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_setsockopt(sockfd, level, optname, optval, optlen);
     }else{
        //the api needs adjusting const
        ret= light_setsockopt(sockfd, level, optname, optval, optlen);
    #ifdef LIGHT_DEBUG
       fprintf(stderr, "  setsockopt Return value = %d\n", ret);
    #endif
     }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //setsockopt_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}

int listen(int sockfd, int backlog)
{
    //listen_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter listen, sockfd = %d, backlog = %d, pid = %d\n", sockfd, backlog, getpid());
    #endif

    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_listen (sockfd, backlog);
    }else{
        // The API needs adjusting
        ret = light_listen_socket(sockfd, backlog);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  listen Return value = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //listen_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


ssize_t send (int sockfd, const void *buf, size_t n, int flags)
{
    //send_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter send sockfd = %d, size = %zu, flag = %d, pid = %d\n", sockfd, n, flags, getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_send(sockfd, buf, n, 0); //Here the flag is set 0 by default

    }else{
        //API needs adjusting const
        ret = light_send(sockfd, buf, n, 0);
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //send_time +=1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "send Return value = %d, errno = %d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}



ssize_t recv(int sockfd, void *buf, size_t len, int flags)
{
    //recv_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter recv sockfd = %d, len = %zu, flags = %d, time = %ld, pid = %d\n", sockfd, len, flags, getCurrentTime(), getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_recv(sockfd, buf,len,0); // flag is set 0 by default
    }else{
        ret = light_recv(sockfd, buf,len,0);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  recv Return value = %d, errno = %d, pid = %d\n", ret, errno, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //recv_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}




ssize_t sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!Warning!!! Enter sendto, incompelete, sockfd = %d, pid = %d\n", sockfd, getpid());
    #endif
    if((0 == light_sock_enable)||(0 == inited)) {
        return real_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
    } else {
        return light_sendto(sockfd, buf, len, flags, dest_addr, addrlen);
    }
}


 ssize_t recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!Warning!!! Enter recvfrom, incompelete, sockfd = %d, pid = %d\n", sockfd, getpid());
    #endif
    if((0 == light_sock_enable)||(0 == inited)) {
        return real_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
    } else {
        return light_recvfrom(sockfd, buf, len, flags, src_addr, addrlen);
    }
}

int accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen)
{
    //accept_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter accept, sockfd = %d, time = %ld, pid = %d\n", sockfd, getCurrentTime(), getpid());
    #endif

    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_accept(sockfd, addr, addrlen);
    }else{
        //API needs adjusting socklen_t
        ret = light_accept(sockfd, addr, addrlen);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  accept return = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //accept_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int accept4(int sockfd, struct sockaddr *addr, socklen_t *addrlen, int flags)
{
    //accept4_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter accept4, sockfd = %d, pid=%d\n", sockfd, getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_accept(sockfd, addr, addrlen);
    }else{
        // API needs adjusting socklen_t
        ret = light_accept4(sockfd, addr, (socklen_t *)addrlen,flags);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  accept4 return=%d, pid=%d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //accept4_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int shutdown (int fd, int how)
{
    //shutdown_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@@Enter shutdown fd = %d, how = %d, pid = %d\n", fd, how, getpid());
    #endif

    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_shutdown(fd, how);
    }else{
        ret = light_shutdown(fd, how);
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //shutdown_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  shutdown Return value = %d, pid = %d\n", ret, getpid());
    #endif
    return ret;
}


int open(const char *pathname, int flags, ...)
{
    //close_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter open\n");
        printf("pathname = %s\n", pathname);
    #endif
    // if (fd < 0)
    // {
    //     fprintf(stderr, "!!!ERROR!!!, fd illegal, fd = %d\n", fd);
    // }
    va_list args;
    va_start(args, flags);
    ret = real_open(pathname, flags, args); // allow unlimited args
    va_end(args);
    // if((0 == light_sock_enable)||(0 == inited)) {
    //     ret = real_open(fd);
    // }else{
    //     ret = light_open(fd);
    // }
#ifdef LIGHT_DEBUG
        fprintf(stderr, "  open Return value = %d\n", ret);
#endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //close_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int open64(const char *pathname, int flags, ...)
{
    //close_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret, var;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter open64, time = %ld\n", getCurrentTime());
        printf("pathname = %s, flags = %d\n", pathname, flags);
    #endif
    // if (fd < 0)
    // {
    //     fprintf(stderr, "!!!ERROR!!!, fd illegal, fd = %d\n", fd);
    // }
    #ifdef OPEN_FAST_PATH
    if (strcmp(pathname, "/srv/www/htdocs/index.html") == 0){
    	if (file_open == 0){
    		// printf("  flags = %d\n");
    		va_list args;
		    va_start(args, flags);
		    var = va_arg(args, int);
		    #ifdef LIGHT_DEBUG
		    printf("  pathname = %s, flags = %d, var = %d\n", pathname, flags, var);
		    #endif
		    ret = real_open64(pathname, flags, args); // allow unlimited args
		    // printf("  pathname = %s, flags = %d, var = %d\n", pathname, flags, var);
		    va_end(args);
    		file_open = 1;
    		file_fd = ret;
    		// printf("  flags = %d\n");
    	}else{
    		// ret = light_open_socket(AF_INET, SOCK_STREAM, 0);
	    	// ret = 2029;
	    	ret = file_prefd;
	    	file_fd = ret;
	    	#ifdef LIGHT_DEBUG
	    	printf("  open64 on fast path\n");
	    	#endif
    	}
    	#ifdef LIGHT_DEBUG
    	printf("  (update) file_fd = %d\n", file_fd);
    	#endif
    }else{
    	va_list args;
	    va_start(args, flags);
	    ret = real_open64(pathname, flags, args); // allow unlimited args
	    va_end(args);
    }
    #else
    va_list args;
	va_start(args, flags);
	ret = real_open64(pathname, flags, args); // allow unlimited args
	va_end(args);
    #endif
    // if((0 == light_sock_enable)||(0 == inited)) {
    //     ret = real_open(fd);
    // }else{
    //     ret = light_open(fd);
    // }
#ifdef LIGHT_DEBUG
        fprintf(stderr, "  open64 Return value = %d, time = %ld\n", ret, getCurrentTime());
#endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //close_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


// int open(const char *pathname, int flags, mode_t mode)
// {
//     //close_times++;
//     //gettimeofday(&tpstart,NULL); // 开始时间
//     int ret;
//     // #ifdef LIGHT_DEBUG
//         fprintf(stderr, "@ Enter open\n");
//         printf("pathname = %s\n", pathname);
//     // #endif
//     // if (fd < 0)
//     // {
//     //     fprintf(stderr, "!!!ERROR!!!, fd illegal, fd = %d\n", fd);
//     // }
//     // va_list args;
//     // va_start(args, flags);
//     ret = real_open(pathname, flags, mode); // allow unlimited args
//     // va_end(args);
//     // if((0 == light_sock_enable)||(0 == inited)) {
//     //     ret = real_open(fd);
//     // }else{
//     //     ret = light_open(fd);
//     // }
// #ifdef LIGHT_DEBUG
//         fprintf(stderr, "  open Return value = %d\n", ret);
// #endif
//     //gettimeofday(&tpend,NULL);   // 结束时间
//     //close_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
//     return ret;
// }


int openat(int dirfd, const char *pathname, int flags, ...)
{
    //close_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter openat\n");
    #endif
    // if (fd < 0)
    // {
    //     fprintf(stderr, "!!!ERROR!!!, fd illegal, fd = %d\n", fd);
    // }
    va_list args;
    va_start(args, flags);
    ret = real_openat(dirfd, pathname, flags, args); // allow unlimited args
    va_end(args);
    // if((0 == light_sock_enable)||(0 == inited)) {
    //     ret = real_open(fd);
    // }else{
    //     ret = light_open(fd);
    // }
#ifdef LIGHT_DEBUG
        fprintf(stderr, "  openat Return value = %d\n", ret);
#endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //close_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int close(int fd)
{
    //close_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter close, fd = %d\n", fd);
    #endif
    if (fd < 0)
    {
        fprintf(stderr, "!!!ERROR!!!, fd illegal, fd = %d\n", fd);
    }

    if (fd == file_prefd){
    	#ifdef LIGHT_DEBUG
    	printf("  do not close file_prefd\n");
    	#endif
    	return 0;
    }

    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_close(fd);
    }else{
        ret = light_close(fd);
    }
#ifdef LIGHT_DEBUG
        fprintf(stderr, "  close Return value = %d\n", ret);
#endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //close_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}


int epoll_create (int size)
{
    //epoll_create_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter epoll_create, size = %d, pid: %d\n", size, getpid());
    #endif
    int ret;
    if(0 == light_sock_enable || 0 == inited){
        ret = real_epoll_create(size);
    }else{
        ret = light_epoll_create(size);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  epoll create Return value = %d, pid: %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //epoll_create_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}

int epoll_create1 (__attribute__((unused))int __flags)
{

    #ifdef LIGHT_DEBUG
    fprintf(stderr, "!!!Warning!!! Enter epoll_create1, not supported\n" );
    #endif
    return -1;
}


int epoll_ctl(int epfd, int op, int fd, struct epoll_event *event)
{
    //epoll_ctl_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter epoll_ctl, epfd = %d, op = %d, fd = %d, pid: %d\n", epfd, op, fd, getpid());
    #endif
    int ret;
    if(0 == light_sock_enable || 0 == inited){
        ret = real_epoll_ctl(epfd, op, fd, event);
    }else{
        ret = light_epoll_ctl(epfd, op, fd, event);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  epoll_ctl Return value = %d, pid = %d\n", ret, getpid());
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //epoll_ctl_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}

 int epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout)
{
    epoll_wait_times++;
    // if (epoll_wait_times % 100 == 0)
    if (1)
    {
        #ifdef LIGHT_DEBUG
            fprintf(stderr, "@ Enter epoll_wait, epfd = %d, maxevents = %d, timeout = %d, pid: %d\n", epfd, maxevents, timeout, getpid());
        #endif
    }
    //gettimeofday(&tpstart,NULL); // 开始时间
    int ret;
    if(0 == light_sock_enable || 0 == inited){
        ret = real_epoll_wait(epfd, events, maxevents, timeout);
    }else{
        ret = light_epoll_wait(epfd, events, maxevents, timeout);
    }
    if (epoll_wait_times % 100 == 0 || ret > 0) {
        #ifdef LIGHT_DEBUG
            fprintf(stderr, "@ Epoll wait  Return value = %d, epfd = %d, time = %ld, pid: %d\n", ret, epfd,  getCurrentTime(), getpid());
        #endif
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //epoll_wait_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    // if(epoll_wait_times == THRESH){
    //     print_stastics();
    // }
    return ret;
}


int epoll_pwait (__attribute__((unused))int __epfd, __attribute__((unused))struct epoll_event *__events, __attribute__((unused))int __maxevents,
    __attribute__((unused))int __timeout, __attribute__((unused))const __sigset_t *__ss)
{
    #ifdef LIGHT_DEBUG
    fprintf(stderr, "!!!Warning!!! Enter epoll_pwait, not supported\n" );
    #endif
    return -1;
}

ssize_t read(int sock, void *buf, size_t nbytes){
    //read_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter Read, sock = %d, size = %zu, time = %ld, pid = %d\n", sock, nbytes, getCurrentTime(), getpid());
    #endif
    int ret;
    if(0 == light_sock_enable || 0 == inited){
        ret = real_read(sock, buf, nbytes);
    } else {
        ret = light_read(sock,buf,nbytes);
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //read_time +=1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  read Return value = %d, errno = %d\n", ret, errno);
    #endif
    return ret;
}


ssize_t pread64(int fd, void * buf, size_t count, off_t offset){
    //read_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter pread64, fd = %d, size = %zu, time = %ld, pid = %d\n", fd, count, getCurrentTime(), getpid());
    #endif
    int ret;
    // if(0 == light_sock_enable || 0 == inited){
    //     ret = real_read(sock, buf, nbytes);
    // } else {
    //     ret = light_read(sock,buf,nbytes);
    // }
    #ifdef OPEN_FAST_PATH
    if (fd == file_fd){
    	if (file_read == 0){
    		ret = real_pread64(fd, file_content, count, offset);
    		file_read = 1;
    		memcpy(buf, file_content, count);
    		#ifdef LIGHT_DEBUG
    		printf("  read web file for the first time\n");
    		#endif
    	}else{
    		memcpy(buf, file_content, count);
    		ret = count;
    		#ifdef LIGHT_DEBUG
    		printf("  read web file on the fast path\n");
    		#endif
    	}
    }else{
    	ret = real_pread64(fd, buf, count, offset);
    }
    #else
    ret = real_pread64(fd, buf, count, offset);
    #endif
    
    //gettimeofday(&tpend,NULL);   // 结束时间
    //read_time +=1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  pread64 Return value = %d, errno = %d, time = %ld\n", ret, errno, getCurrentTime());
    #endif
    return ret;
}


ssize_t write(int sockfd, const void *buf, size_t nbytes) {
    //write_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter Write, sockfd = %d, size = %zu, time = %ld, pid = %d\n", sockfd, nbytes, getCurrentTime(), getpid());
    #endif
    int ret;
    if(0 == light_sock_enable || 0 == inited){
        ret = real_write(sockfd, buf, nbytes);
    } else {
        ret = light_write(sockfd, buf, nbytes);
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //write_time +=1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  write Return value = %d, errno = %d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    //writev_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter writev, fd = %d, iovcnt = %d, time = %ld, pid = %d\n", fd, iovcnt, getCurrentTime(), getpid());
    #endif
    int ret;
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_writev(fd, iov, iovcnt);
    }else{
        ret = light_writev(fd,iov,iovcnt);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  writev return value = %d\n", ret);
    #endif
    //gettimeofday(&tpend,NULL);   // 结束时间
    //writev_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    return ret;
}

int ioctl (int fd, unsigned long int request, ...)
{
    //ioctl_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter ioctl, fd = %d, request = %ld, pid = %d\n", fd, request, getpid());
    #endif
    int ret = 0;
    if((0 == light_sock_enable)||(0 == inited)) {
            va_list args;
            va_start(args,request);
            ret = real_ioctl(fd, request, args); // allow unlimited args
            va_end(args);
    } else {
            va_list args;
            size_t size=1;
            va_start(args,size);
            void *argp = va_arg(args, void*);
            va_end(args);
            //gettimeofday(&tpend,NULL);   // 结束时间
            //ioctl_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
            ret = light_ioctl(fd,request,argp);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  ioctl Return value = %d\n", ret);
    #endif
    return ret;
}

int fcntl (int fd, int cmd, ...)
{
    //fcntl_times++;
    //gettimeofday(&tpstart,NULL); // 开始时间
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter fcntl, fd = %d, cmd = %d, pid = %d\n", fd, cmd, getpid());
    #endif
    int ret = 0;
    int isEffective = 0;
    va_list args;
    size_t size = 1;
    va_start(args, size);
    void *argp = va_arg(args, void*);
    va_end(args);
    if((0 == light_sock_enable)||(0 == inited)) {
        ret = real_fcntl(fd, cmd, argp); // allow unlimited args
    }
    else{
        switch(cmd){
            case F_GETFL:
            case F_SETFL:
            {
                isEffective = 1;
                ret = light_fcntl(fd, cmd, argp);
                break;
            }
        }
    }
    //gettimeofday(&tpend,NULL);   // 结束时间
    //fcntl_time += 1000000*(tpend.tv_sec-tpstart.tv_sec)+tpend.tv_usec-tpstart.tv_usec;
    
    #ifdef LIGHT_DEBUG
        if (isEffective == 0)
        {
            fprintf(stderr, "  Enter fcntl but cmd not supported, sockfd = %d, cmd = %d\n", fd, cmd);
        }
        fprintf(stderr, "  fcntl Return value = %d errno = %d\n", ret, errno);
    #endif
    return ret;

}

pid_t fork() 
{
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter fork, pid = %d\n", getpid());
    #endif
    if (0 == light_sock_enable || 0 == inited) {
        ret = real_fork();
    } else {
        ret = light_fork();
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  fork return value=%d, pid = %d\n", ret, getpid());
    #endif
    return ret;
}

int execl(const char *path, const char *arg, ...)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execl, path=%s, arg=%s, pid = %d\n", path, arg, getpid());
    #endif
    va_list ap;
    int ret;
    va_start(ap, arg);
    ret = real_execl(path, arg, ap);
    va_end(ap);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execl return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int execle(const char *path, const char *arg, ...)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execle, path=%s, arg=%s, pid = %d\n", path, arg, getpid());
    #endif
    va_list ap;
    int ret;
    va_start(ap, arg);
    ret = real_execle(path, arg, ap);
    va_end(ap);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execle return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int execlp(const char *file, const char *arg, ...)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execlp, file=%s, arg=%s, pid = %d\n", file, arg, getpid());
    #endif
    va_list ap;
    int ret;
    va_start(ap, arg);
    ret = real_execlp(file, arg, ap);
    va_end(ap);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execlp return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int execv(const char *path, char *const argv[])
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execv, path=%s, pid = %d\n", path, getpid());
    #endif
    int ret = real_execv(path, argv);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execv return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int execvp(const char *file, char *const argv[])
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execvp, file=%s, pid = %d\n", file, getpid());
    #endif
    int ret = real_execvp(file, argv);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execvp return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int execvpe(const char *file, char *const argv[], char *const envp[])
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter execvp, file=%s, pid = %d\n", file, getpid());
    #endif
    int ret = real_execvpe(file, argv, envp);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "execvpe return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int dup(int oldfd)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter dup, oldfd=%d, pid = %d\n", oldfd, getpid());
    #endif
    int ret = real_dup(oldfd);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "dup return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int dup2(int oldfd, int newfd)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter dup2, oldfd=%d, newfd=%d, pid = %d\n", oldfd, newfd, getpid());
    #endif
    int ret = real_dup2(oldfd, newfd);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "dup2 return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int dup3(int oldfd, int newfd, int flags)
{
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "!!!ERROR!!! Enter dup3, oldfd=%d, newfd=%d, flags=%d, pid = %d\n", oldfd, newfd, flags, getpid());
    #endif
    int ret = real_dup3(oldfd, newfd, flags);
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "dup3 return value = %d, errno=%d, pid = %d\n", ret, errno, getpid());
    #endif
    return ret;
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                          void *(*start_routine) (void *), void *arg)
{
    int ret;
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "@ Enter pthread_create, pid = %d\n", getpid());
    #endif
    if (0 == light_sock_enable || 0 == inited) {
        ret = real_pthread_create(thread, attr, start_routine, arg);
    } else {
        ret = light_pthread_create(thread, attr, start_routine, arg);
    }
    #ifdef LIGHT_DEBUG
        fprintf(stderr, "  pthread_create return value=%d, pid = %d\n", ret, getpid());
    #endif
    return ret;
}
