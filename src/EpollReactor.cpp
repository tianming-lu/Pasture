/*	
*	Copyright(c) 2019 lutianming email：641471957@qq.com
*	Pasture is licensed under the Mulan PSL v1.
*	You can use this software according to the terms and conditions of the Mulan PSL v1.
*	You may obtain a copy of Mulan PSL v1 at :
*	http://license.coscl.org.cn/MulanPSL
*	THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
*	IMPLIED, INCLUDING BUT NOT LIMITED TO NON - INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
*	PURPOSE.
*
*	See the Mulan PSL v1 for more details.
*/

#include "Reactor.h"
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/prctl.h>
#include <netdb.h>

#define DATA_BUFSIZE 5120

enum{
    ACCEPT = 0,
    READ,
	WRITE,
    CONNECT,
};

#ifdef KCP_SUPPORT
#include "ikcp.h"
#include "sys/time.h"

struct Kcp_Content{
	ikcpcb* kcp;
	char* 	buf;
	long	lock;
	int		size;
	int		offset;
	char	enable;
};

static inline void itimeofday(long* sec, long* usec){
	struct timeval time;
	gettimeofday(&time, NULL);
	if (sec) *sec = time.tv_sec;
	if (usec) *usec = time.tv_usec;
}

/* get clock in millisecond 64 */
static inline IINT64 iclock64(void){
	long s, u;
	IINT64 value;
	itimeofday(&s, &u);
	value = ((IINT64)s) * 1000 + (u / 1000);
	return value;
}

static inline IUINT32 iclock(){
	return (IUINT32)(iclock64() & 0xfffffffful);
}
#endif

static HSOCKET new_hsockt(){
	HSOCKET hsock = (HSOCKET)malloc(sizeof(EPOLL_SOCKET));
	if (hsock == NULL) return NULL;
	memset(hsock, 0, sizeof(EPOLL_SOCKET));
	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	rbuf->buff = (char*)malloc(DATA_BUFSIZE);
	if (rbuf->buff == NULL){
		free(hsock);
		return NULL;
	}
	memset(rbuf->buff, 0, DATA_BUFSIZE);
	rbuf->offset = 0;
	rbuf->size = DATA_BUFSIZE;
	return hsock;
}

static void release_hsock(HSOCKET hsock){
	if (hsock){
#ifdef KCP_SUPPORT
		if (hsock->_conn_type == KCP_CONN){
			Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
			ikcp_release(ctx->kcp);
			free(ctx->buf);
			free(ctx);
		}
#endif
		if (hsock->_recv_buf.buff) free(hsock->_recv_buf.buff);
		if (hsock->_send_buf.buff) free(hsock->_send_buf.buff);
		free(hsock);
	}
}

static inline void socket_ip_v4_converto_v6(const char* src, char* dst, size_t size) {
	if (strchr(src, ':')) {
		snprintf(dst, size, src);
	}
	else {
		snprintf(dst, size, "::ffff:%s", src);
	}
}

static inline void socket_set_v6only(int fd, int v6only) {
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&v6only, sizeof(v6only));
}

static int get_listen_sock(const char* ip, int port){
	int fd = socket(AF_INET6, SOCK_STREAM, 0);
	if(fd < 0) { return -1; }

	struct sockaddr_in6 server_addr = {0x0};
	memset(&server_addr, 0, sizeof(server_addr));
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, v6ip, &server_addr.sin6_addr);
	//server_addr.sin6_addr = in6addr_any;

	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	struct linger linger = {0, 0};
	setsockopt(fd, SOL_SOCKET, SO_LINGER, (int *)&linger, sizeof(linger));

	int ret = bind(fd, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if( ret != 0) { 
		printf("%s:%d %s:%d %d\n", __func__, __LINE__, ip, port, ret);
		return -2;
	}
	if (listen(fd, 10)) {return -3;}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

static void epoll_add_accept(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eprfd, EPOLL_CTL_ADD, hsock->fd, &ev);	
}

static void epoll_add_connect(HSOCKET hsock){
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eprfd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static void epoll_add_timer(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eptfd, EPOLL_CTL_ADD, hsock->fd, &ev);	
}

static void epoll_mod_timer(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eptfd, EPOLL_CTL_MOD, hsock->fd, &ev);	
}

static void epoll_del_timer(HSOCKET hsock) {
	struct epoll_event ev;
	epoll_ctl(hsock->factory->reactor->eptfd, EPOLL_CTL_DEL, hsock->fd, &ev);
}

static void epoll_add_read(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eprfd, EPOLL_CTL_ADD, hsock->fd, &ev);	
}

static void epoll_mod_read(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->eprfd, EPOLL_CTL_MOD, hsock->fd, &ev);	
}

static void epoll_add_write(HSOCKET hsock){
	struct epoll_event ev;
	ev.events = EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static void epoll_mod_write(HSOCKET hsock){
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_MOD, hsock->fd, &ev);
}

static void epoll_del_write(HSOCKET hsock) {
	struct epoll_event ev;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_DEL, hsock->fd, &ev);
}

static void epoll_del_read(HSOCKET hsock) {
	struct epoll_event ev;
	epoll_ctl(hsock->factory->reactor->eprfd, EPOLL_CTL_DEL, hsock->fd, &ev);	
}

static void set_linger_for_fd(int fd){
    struct linger linger;
	linger.l_onoff = 0;
	linger.l_linger = 0;
	setsockopt(fd, SOL_SOCKET, SO_LINGER, (const void *) &linger, sizeof(struct linger));
}

static void socket_set_keepalive(int fd){
	int keepalive = 1; // 开启keepalive属性
	int keepidle = 60; // 如该连接在60秒内没有任何数据往来,则进行探测
	int keepinterval = 5; // 探测时发包的时间间隔为5 秒
	int keepcount = 3; // 探测尝试的次数.如果第1次探测包就收到响应了,则后2次的不再发.
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (void *)&keepalive, sizeof(keepalive));
	setsockopt(fd, SOL_TCP, TCP_KEEPIDLE, (void*)&keepidle, sizeof(keepidle));
	setsockopt(fd, SOL_TCP, TCP_KEEPINTVL, (void *)&keepinterval, sizeof(keepinterval));
	setsockopt(fd, SOL_TCP, TCP_KEEPCNT, (void *)&keepcount, sizeof(keepcount));
}

static inline void AutoProtocolFree(BaseProtocol* proto) {
	AutoProtocol* autoproto = (AutoProtocol*)proto;
	autofree func = autoproto->freefunc;
	func(autoproto);
}

static void do_close(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	uint8_t left_count = 0;
	if (hsock->_stat < SOCKET_CLOSED ){
		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED){
			int error = 0;
			socklen_t errlen = sizeof(error);
			getsockopt(hsock->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
			left_count = __sync_sub_and_fetch (&proto->sockCount, 1);
			if (hsock->_stat == SOCKET_CONNECTING)
				proto->ConnectionFailed(hsock, error);
			else
    			proto->ConnectionClosed(hsock, error);
		}
		proto->UnLock();
	}
    
	epoll_del_write(hsock);
    epoll_del_read(hsock);
    close(hsock->fd);

	if (hsock->_stat < SOCKET_CLOSED && left_count == 0 && proto != NULL) {
		switch (proto->protoType){
		case SERVER_PROTOCOL:
			fc->DeleteProtocol(proto);
			break;
		case AUTO_PROTOCOL:
			AutoProtocolFree(proto);
			break;
		default:
			break;
		}
	}
    release_hsock(hsock);
}

static void do_connect(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	epoll_add_write(hsock);
	if (hsock->_stat < SOCKET_CLOSED){
		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED){
			proto->ConnectionMade(hsock, hsock->_conn_type);
			hsock->_stat = SOCKET_CONNECTED;
		}
			
		proto->UnLock();
	}
	epoll_mod_read(hsock);
}

static int do_read_tcp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	char* buf = NULL;
	size_t buflen = 0;
	ssize_t n = 0;
	EPOLL_BUFF* rbuf = &hsock->_recv_buf;
	while (1){
		buf = rbuf->buff + rbuf->offset;
		buflen = rbuf->size - rbuf->offset;
		n = recv(hsock->fd, buf, buflen, MSG_DONTWAIT);
		if (n > 0){
			rbuf->offset += n;
			if (size_t(n) == buflen){
				size_t newsize = rbuf->size*2;
				char* newbuf = (char*)realloc(rbuf->buff , newsize);
				if (newbuf == NULL) return -1;
				rbuf->buff = newbuf;
				rbuf->size = newsize;
				continue;
			}
			break;
		}
		else if (n == 0){
			return -1;
		}
		if (errno == EINTR){
			continue;
		}
		else if (errno == EAGAIN){
			break;
		}
		else{
			return -1;
		}
	}
	return rbuf->offset;
}

#ifdef KCP_SUPPORT
static int do_read_kcp(HSOCKET hsock, EPOLL_BUFF* rbuf, BaseFactory* fc, BaseProtocol* proto){
	Kcp_Content* ctx = (Kcp_Content*)(hsock->_user_data);
	LONGLOCK(&ctx->lock);
	ikcp_input(ctx->kcp, rbuf->buff, rbuf->offset);
	rbuf->offset = 0;

	int ret = 0, n, left;
	while (1) {
		left = ctx->size - ctx->offset;
		n = ikcp_recv(ctx->kcp, ctx->buf+ctx->offset, left);
		if (n < 0) { 
			if (n == -3) {
				int newsize = ctx->size * 2;
				char* newbuf = (char*)realloc(ctx->buf, newsize);
				if (newbuf) {
					ctx->buf = newbuf;
					ctx->size = newsize;
					continue;
				}
			}
			break; 
		}
		ctx->offset += n;

		ret = -1;
		if (hsock->_stat < SOCKET_CLOSED){
			proto->Lock();
			if (hsock->_stat < SOCKET_CLOSED){
				proto->ConnectionRecved(hsock, ctx->buf, ctx->offset);
				ret = 0;
			}
			proto->UnLock();
		}
		if (ret) break;
	}
	LONGUNLOCK(&ctx->lock);
	return ret;
}
#endif

static int do_read_udp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	char* buf = NULL;
	size_t buflen = 0;
	int addr_len=sizeof(hsock->peer_addr);
	EPOLL_BUFF* rbuf = &hsock->_recv_buf;

	int n, ret = -1;
	while (1){
		buf = rbuf->buff + rbuf->offset;
		buflen = rbuf->size - rbuf->offset;
		n = recvfrom(hsock->fd, buf, buflen, MSG_DONTWAIT, (sockaddr*)&(hsock->peer_addr), (socklen_t*)&addr_len);
		if (n > 0){
			rbuf->offset += n;
			if (hsock->_conn_type == UDP_CONN){
				if (hsock->_stat < SOCKET_CLOSED){
					proto->Lock();
					if (hsock->_stat < SOCKET_CLOSED){
						proto->ConnectionRecved(hsock, rbuf->buff, rbuf->offset);
						ret = 0;
					}
					proto->UnLock();
				}
			}
#ifdef KCP_SUPPORT
			else if (hsock->_conn_type == KCP_CONN){
				ret = do_read_kcp(hsock, rbuf, fc, proto);
				if (ret) break;
			}
#endif
			continue;
		}
		else if (n == 0){
			break;
		}
		else if (n < 0 ){
			if (errno == EINTR){
				continue;
			}
			else if (errno == EAGAIN){
				break;
			}
		}
	}
	return ret;
}

static void do_read(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	int ret = 0;
	switch (hsock->_conn_type)
	{
	case TCP_CONN:
		ret = do_read_tcp(hsock, fc, proto);
		break;
	case SSL_CONN:
		ret = do_read_ssl(hsock, fc, proto);
		break;
	case UDP_CONN:
	case KCP_CONN:
		ret = do_read_udp(hsock, fc, proto);
		break;
	default:
		break;
	}
	if (ret > 0 && hsock->_stat < SOCKET_CLOSED){
		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED){
			proto->ConnectionRecved(hsock, rbuf->buff, rbuf->offset);
		}
		proto->UnLock();
	}
	else if (ret < 0){
		return do_close(hsock, fc, proto);
	}
	epoll_mod_read(hsock);
}

static void do_accpet(HSOCKET listenhsock, BaseFactory* fc){
    struct sockaddr_in6 addr;
	socklen_t len;
   	int fd = 0;

	while (1){
	    fd = accept(fc->Listenfd, (struct sockaddr *)&addr, (len = sizeof(addr), &len));
		if (fd < 0) break;
        HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(fd);
			continue;
		}
		memcpy(&hsock->peer_addr, &addr, sizeof(addr));
		socket_set_keepalive(fd);

        BaseProtocol* proto = fc->CreateProtocol();
		if (proto->factory == NULL)
			proto->SetFactory(fc, SERVER_PROTOCOL);

        hsock->fd = fd;
		hsock->factory = fc;
		hsock->_user = proto;
		
		set_linger_for_fd(fd);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
		__sync_add_and_fetch(&proto->sockCount, 1);

		epoll_add_write(hsock);
		proto->Lock();
        proto->ConnectionMade(hsock, hsock->_conn_type);
        proto->UnLock();
		hsock->_stat = SOCKET_CONNECTED;
		epoll_add_read(hsock);
	}
}

static void read_work_thread(void* args){
	prctl(PR_SET_NAME,"read");
    Reactor* reactor = (Reactor*)args;
	BaseFactory* factory = NULL;
    int i = 0,n = 0;
    struct epoll_event *pev = (struct epoll_event*)malloc(sizeof(struct epoll_event) * reactor->maxevent);
	if(pev == NULL) { return ; }
    volatile HSOCKET hsock = NULL;
	while (1){
		n = epoll_wait(reactor->eprfd, pev, reactor->maxevent, -1);
		for(i = 0; i < n; i++) {
            hsock = (HSOCKET)pev[i].data.ptr;
			factory = hsock->factory;
			if (hsock->fd == factory->Listenfd){
				do_accpet(hsock, factory);
			}
			else if (pev[i].events & EPOLLIN){
				do_read(hsock, factory, hsock->_user);
			}
			else if (pev[i].events & EPOLLOUT){
				do_connect(hsock, factory, hsock->_user);
			}
			else{
				do_close(hsock, factory, hsock->_user);
			}
		}
	}
}

static void do_write_udp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	socklen_t socklen = sizeof(hsock->peer_addr);
	EPOLL_BUFF *sbuf = &hsock->_send_buf;
	if (__sync_fetch_and_or(&sbuf->lock_flag, 1)) return;
	int slen = 0;
	int len;
	char* data;
	int n = 0;
	struct sockaddr* addr;
	while (slen < sbuf->offset){
		data = sbuf->buff + slen + sizeof(int) + socklen;
		len = *(int*)(sbuf->buff + slen);
		addr = (struct sockaddr*)(sbuf->buff + slen + sizeof(int));
		if (len > 0){
			n = sendto(hsock->fd, data, len, MSG_DONTWAIT, addr, socklen);
			if(n > 0) {
				slen += sizeof(int) + socklen + len;
			}
			else if(errno == EINTR || errno == EAGAIN) 
				break;
		}
	}
	sbuf->offset -= slen;
	memmove(sbuf->buff, sbuf->buff + slen, sbuf->offset);
	__sync_fetch_and_and(&sbuf->lock_flag, 0);

	if (hsock->_stat >= SOCKET_CLOSEING){
		shutdown(hsock->fd, SHUT_RD);
	}
}

static void do_write_tcp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto){
	EPOLL_BUFF *sbuf = &hsock->_send_buf;
	//while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
	if (__sync_fetch_and_or(&sbuf->lock_flag, 1)) return;

	char* data = sbuf->buff;
	int len = sbuf->offset;
	int not_over = 0;
	if (len > 0){
		int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		if(n > 0) {
			sbuf->offset -= n;
			memmove(data, data + n, sbuf->offset);
			if (n < len) not_over = 1;
		}
		else if(errno == EINTR || errno == EAGAIN) 
			not_over = 1;
	}
	__sync_fetch_and_and(&sbuf->lock_flag, 0);
	
	if (not_over)
		epoll_mod_write(hsock);
	else if (hsock->_stat >= SOCKET_CLOSEING){
		shutdown(hsock->fd, SHUT_RD);
	}
}

static void do_write(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	if (hsock->_conn_type == TCP_CONN)
		do_write_tcp(hsock, fc, proto);
	else
		do_write_udp(hsock, fc, proto);
}

static void write_work_thread(void* args)
{
	prctl(PR_SET_NAME,"write");
    Reactor* reactor = (Reactor*)args;
	BaseFactory* factory = NULL;
    int i = 0,n = 0;
    struct epoll_event *pev = (struct epoll_event*)malloc(sizeof(struct epoll_event) * reactor->maxevent);
	if(pev == NULL) 
	{
		return ;
	}
    volatile HSOCKET hsock = NULL;
	while (1)
	{
		n = epoll_wait(reactor->epwfd, pev, reactor->maxevent, -1);
		for(i = 0; i < n; i++) 
		{
            hsock = (HSOCKET)pev[i].data.ptr;
			factory = hsock->factory;
			if (pev[i].events & EPOLLOUT){
				do_write(hsock, factory, hsock->_user);
			}
		}
	}
}

static void do_timer_callback(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	uint64_t value;
	read(hsock->fd, &value, sizeof(uint64_t)); //必须读取，否则定时器异常
	if (hsock->_stat < SOCKET_CLOSED)
	{
		hsock->_callback(proto);
		epoll_mod_timer(hsock);
	}
	else
	{
		epoll_del_timer(hsock);
		close(hsock->fd);
		release_hsock(hsock);
	}
	
}

static void timer_work_thread(void* args)
{
	prctl(PR_SET_NAME,"timer");
    Reactor* reactor = (Reactor*)args;
	BaseFactory* factory = NULL;
    int i = 0,n = 0;
    struct epoll_event *pev = (struct epoll_event*)malloc(sizeof(struct epoll_event) * reactor->maxevent);
	if(pev == NULL) 
	{
		return ;
	}
    volatile HSOCKET hsock = NULL;
	while (1)
	{
		n = epoll_wait(reactor->eptfd, pev, reactor->maxevent, -1);
		for(i = 0; i < n; i++) 
		{
            hsock = (HSOCKET)pev[i].data.ptr;
			factory = hsock->factory;
			if (pev[i].events & EPOLLIN){
				do_timer_callback(hsock, factory, hsock->_user);
			}
		}
	}
}

static void main_work_thread(void* args)
{
	prctl(PR_SET_NAME,"actor");
	Reactor* reactor = (Reactor*)args;
    int i = 0;
	int rc;
    for (i = 0; i < reactor->CPU_COUNT; i++)
	//for (i = 0; i < 1; i++)
	{
		pthread_attr_t rattr;
   		pthread_t rtid;
		pthread_attr_init(&rattr);
		if((rc = pthread_create(&rtid, &rattr, (void*(*)(void*))read_work_thread, reactor)) != 0)
		{
			return;
		}
	}

	for (i = 0; i < 1; i++)
	{
		pthread_attr_t wattr;
   		pthread_t wtid;
		pthread_attr_init(&wattr);
		if((rc = pthread_create(&wtid, &wattr, (void*(*)(void*))write_work_thread, reactor)) != 0)
		{
			return;
		}
	}

	for (i = 0; i < reactor->CPU_COUNT; i++)
	{
		pthread_attr_t tattr;
   		pthread_t ttid;
		pthread_attr_init(&tattr);
		if((rc = pthread_create(&ttid, &tattr, (void*(*)(void*))timer_work_thread, reactor)) != 0)
		{
			return;
		}
	}
	while (reactor->Run)
	{
		std::map<uint16_t, BaseFactory*>::iterator iter;
		for (iter = reactor->FactoryAll.begin(); iter != reactor->FactoryAll.end(); ++iter)
		{
			iter->second->FactoryLoop();
		}
		usleep(100*1000);
	}
}

int ReactorStart(Reactor* reactor)
{
    reactor->eprfd = epoll_create(reactor->maxevent);
	if(reactor->eprfd < 0)
		return -1;
	reactor->epwfd = epoll_create(reactor->maxevent);
	if(reactor->epwfd < 0)
		return -1;
	reactor->eptfd = epoll_create(reactor->maxevent);
	if(reactor->eptfd < 0)
		return -1;
	reactor->CPU_COUNT = get_nprocs_conf();

	pthread_attr_t attr;
   	pthread_t tid;
	pthread_attr_init(&attr);
	int rc;

	if((rc = pthread_create(&tid, &attr, (void*(*)(void*))main_work_thread, reactor)) != 0)
	{
		return -1;
	}
	return 0;
}

int	FactoryRun(BaseFactory* fc)
{
	if (!fc->FactoryInit())
		return -1;
	
	if (fc->ServerPort > 0)
	{
		fc->Listenfd = get_listen_sock(fc->ServerAddr, fc->ServerPort);
		if (fc->Listenfd < 0){
			printf("%s:%d %d\n", __func__, __LINE__, fc->Listenfd);
			return -2;
		}
			
		HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(fc->Listenfd);
			return -3;
		}
		hsock->fd = fc->Listenfd;
		hsock->factory = fc;

		epoll_add_accept(hsock);
	}
	fc->FactoryInited();
	fc->reactor->FactoryAll.insert(std::pair<uint16_t, BaseFactory*>(fc->ServerPort, fc));
	return 0;
}

int FactoryStop(BaseFactory* fc)
{
	std::map<uint16_t, BaseFactory*>::iterator iter;
	iter = fc->reactor->FactoryAll.find(fc->ServerPort);
	if (iter != fc->reactor->FactoryAll.end())
	{
		fc->reactor->FactoryAll.erase(iter);
	}
	fc->FactoryClose();
	return 0;
}

static bool EpollConnectExUDP(BaseProtocol* proto, HSOCKET hsock, int listen_port)
{
	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if(fd < 0) {
		return false;
	}
	hsock->fd = fd;
	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	local_addr.sin6_port = htons(listen_port);
	local_addr.sin6_addr = in6addr_any;
	socket_set_v6only(hsock->fd, 0);

	if(bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) 
    {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	hsock->_stat = SOCKET_CONNECTED;
	epoll_add_write(hsock);
	epoll_add_read(hsock);
	return true;
}

HSOCKET HsocketListenUDP(BaseProtocol* proto, int port)
{
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) 
		return NULL;
	HSOCKET hsock = new_hsockt();
	if (hsock == NULL) 
		return NULL;
	hsock->peer_addr.sin6_family = AF_INET6;
	hsock->peer_addr.sin6_port = htons(port);
	inet_pton(AF_INET6, "::", &hsock->peer_addr.sin6_addr);

	hsock->factory = proto->factory;
	hsock->_conn_type = UDP_CONN;
	hsock->_user = proto;

	bool ret = false;
	ret =  EpollConnectExUDP(proto, hsock, port);
	if (ret == false) 
	{
		release_hsock(hsock);
		return NULL;
	}
	__sync_add_and_fetch(&proto->sockCount, 1);
	return hsock;
}

static bool EpollConnectExTCP(BaseProtocol* proto, HSOCKET hsock)
{
	int fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if(fd < 0) {
		return false;
	}
	socket_set_keepalive(fd);
	hsock->fd = fd;
	set_linger_for_fd(fd);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	socket_set_v6only(hsock->fd, 0);
	
	connect(fd, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr));
	hsock->_stat = SOCKET_CONNECTING;
	epoll_add_connect(hsock);
	return  true;
}

HSOCKET HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE type)
{
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) 
		return NULL;
	HSOCKET hsock = new_hsockt();
	if (hsock == NULL) 
		return NULL;
	hsock->peer_addr.sin6_family = AF_INET6;
	hsock->peer_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, v6ip, &hsock->peer_addr.sin6_addr);

	hsock->factory = proto->factory;
	hsock->_conn_type = type;
	hsock->_user = proto;

	bool ret = false;
	if (type == UDP_CONN)
		ret =  EpollConnectExUDP(proto, hsock, 0);
	else if (type == TCP_CONN || type == SSL_CONN)
		ret = EpollConnectExTCP(proto, hsock);
	if (ret == false) 
	{
		release_hsock(hsock);
		return NULL;
	}
	__sync_add_and_fetch(&proto->sockCount, 1);
	return hsock;
}

HSOCKET TimerCreate(BaseProtocol* proto, int duetime, int looptime, timer_callback callback)
{
	HSOCKET hsock = new_hsockt();
	if (hsock == NULL) 
		return NULL;
	int tfd = timerfd_create(CLOCK_MONOTONIC, 0);   //创建定时器
    if(tfd == -1) {
		release_hsock(hsock);
        return NULL;
    }
	hsock->fd = tfd;
	hsock->_conn_type = ITMER;
	hsock->_callback = callback;
	hsock->_user = proto;
	hsock->factory = proto->factory;

    struct itimerspec time_intv; //用来存储时间
    time_intv.it_value.tv_sec = duetime/1000; //设定2s超时
    time_intv.it_value.tv_nsec = (duetime%1000)*1000000;
    time_intv.it_interval.tv_sec = looptime/1000;   //每隔2s超时
    time_intv.it_interval.tv_nsec = (looptime%1000)*1000000;

    timerfd_settime(tfd, 0, &time_intv, NULL);  //启动定时器
	epoll_add_timer(hsock);
    return hsock;
}

void TimerDelete(HSOCKET hsock)
{
	hsock->_stat = SOCKET_CLOSED;
}

static void HsocketSendUdp(HSOCKET hsock, const char* data, int len)
{
	//socklen_t socklen = sizeof(hsock->peer_addr);
	//sendto(hsock->fd, data, len, 0, (struct sockaddr*)&hsock->peer_addr, socklen);
	EPOLL_BUFF *sbuf = &hsock->_send_buf;
	int needlen = sizeof(int) + sizeof(hsock->peer_addr) + len;
    if (sbuf->buff == NULL)
    {
        sbuf->buff = (char*)malloc(needlen);
    	if (sbuf->buff == NULL) return;
        sbuf->size = needlen;
        sbuf->offset = 0;
    }
	while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
	int newlen = sbuf->offset + needlen;
	if (sbuf->size < newlen)
    {
        char* newbuf = (char*)realloc(sbuf->buff, newlen);
        if (newbuf == NULL) {__sync_fetch_and_and(&sbuf->lock_flag, 0);return;}
        sbuf->buff = newbuf;
        sbuf->size = newlen;
    }
	memcpy(sbuf->buff + sbuf->offset, (char*)&len, sizeof(int));
	memcpy(sbuf->buff + sbuf->offset + sizeof(int), (char*)&hsock->peer_addr, sizeof(hsock->peer_addr));
    memcpy(sbuf->buff + sbuf->offset + sizeof(int) + sizeof(hsock->peer_addr), data, len);
    sbuf->offset += needlen;
	__sync_fetch_and_and(&sbuf->lock_flag, 0);
    epoll_mod_write(hsock);	
}

bool HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len)
{
	if (hsock->_conn_type == UDP_CONN){
		EPOLL_BUFF *sbuf = &hsock->_send_buf;
		int needlen = sizeof(int) + sizeof(hsock->peer_addr) + len;
    	if (sbuf->buff == NULL){
        	sbuf->buff = (char*)malloc(needlen);
    		if (sbuf->buff == NULL) return false;
        	sbuf->size = needlen;
        	sbuf->offset = 0;
    	}
		while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
		int newlen = sbuf->offset + needlen;
		if (sbuf->size < newlen){
        	char* newbuf = (char*)realloc(sbuf->buff, newlen);
        	if (newbuf == NULL) {__sync_fetch_and_and(&sbuf->lock_flag, 0);return false;}
        	sbuf->buff = newbuf;
        	sbuf->size = newlen;
    	}
		memcpy(sbuf->buff + sbuf->offset, (char*)&len, sizeof(int));
		struct sockaddr_in6* addr = (struct sockaddr_in6*)sbuf->buff + sizeof(int);
		addr->sin6_family = AF_INET6;
		addr->sin6_port = htons(port);

		char v6ip[40] = { 0x0 };
		socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, v6ip, &addr->sin6_addr);
    	memcpy(sbuf->buff + sbuf->offset + sizeof(int) + sizeof(hsock->peer_addr), data, len);
    	sbuf->offset += needlen;
		__sync_fetch_and_and(&sbuf->lock_flag, 0);
    	epoll_mod_write(hsock);
		return true;
	}
	return false;
}

static void HsocketSendKcp(HSOCKET hsock, const char* data, int len)
{
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	if (ctx->enable){
		ikcp_send(ctx->kcp, data, len);
	}
	else{
		HsocketSendUdp(hsock,  data, len);
	}
}

static void HsocketSendTcp(HSOCKET hsock, const char* data, int len)
{
	EPOLL_BUFF *sbuf = &hsock->_send_buf;
	if (sbuf->buff == NULL)
	{
		sbuf->buff = (char*)malloc(len);
		if (sbuf->buff == NULL) return;
		sbuf->size = len;
		sbuf->offset = 0;
	}

	while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
	if (sbuf->size < sbuf->offset + len)
	{
		char* newbuf = (char*)realloc(sbuf->buff, sbuf->offset + len);
		if (newbuf == NULL) {__sync_fetch_and_and(&sbuf->lock_flag, 0);return;}
		sbuf->buff = newbuf;
		sbuf->size = sbuf->offset + len;
	}
	memcpy(sbuf->buff + sbuf->offset, data, len);
	sbuf->offset += len;
	__sync_fetch_and_and(&sbuf->lock_flag, 0);
	epoll_mod_write(hsock);
}

bool HsocketSend(HSOCKET hsock, const char* data, int len)
{
	if (hsock == NULL || hsock->fd < 0 ||len <= 0) return false;
	switch (hsock->_conn_type)
	{
	case TCP_CONN:
	case SSL_CONN:
		HsocketSendTcp(hsock, data, len);
		break;
	case  UDP_CONN:
		HsocketSendUdp(hsock, data, len);
		break;
	case KCP_CONN:
		HsocketSendKcp(hsock, data, len);
		break;
	default:
		break;
	}
	return true;
}

EPOLL_BUFF* HsocketGetBuff()
{
	EPOLL_BUFF* epoll_Buff = (EPOLL_BUFF*)malloc(sizeof(EPOLL_BUFF));
	if (epoll_Buff){
		memset(epoll_Buff, 0x0, sizeof(EPOLL_BUFF));
		epoll_Buff->buff = (char*)malloc(DATA_BUFSIZE);
		if (epoll_Buff->buff){
			*(epoll_Buff->buff) = 0x0;
			epoll_Buff->size = DATA_BUFSIZE;
		}
	}
	return epoll_Buff;
}

bool HsocketSetBuff(EPOLL_BUFF* epoll_Buff, const char* data, int len)
{
	if (epoll_Buff == NULL) return false;
	int left = epoll_Buff->size - epoll_Buff->offset;
	if (left >= len)
	{
		memcpy(epoll_Buff->buff + epoll_Buff->offset, data, len);
		epoll_Buff->offset += len;
	}
	else
	{
		char* new_ptr = (char*)realloc(epoll_Buff->buff, epoll_Buff->size + len);
		if (new_ptr)
		{
			epoll_Buff->buff = new_ptr;
			memcpy(epoll_Buff->buff + epoll_Buff->offset, data, len);
			epoll_Buff->offset += len;
		}
	}
	return true;
}

bool HsocketSendBuff(EPOLL_SOCKET* hsock, EPOLL_BUFF* epoll_Buff)
{
	if (hsock == NULL || epoll_Buff == NULL) return false;
	if (hsock->_conn_type == UDP_CONN)
		HsocketSendUdp(hsock, epoll_Buff->buff, epoll_Buff->offset);
	else
		HsocketSendTcp(hsock, epoll_Buff->buff, epoll_Buff->offset);
	free(epoll_Buff->buff);
	free(epoll_Buff);
	return true;
}

bool HsocketClose(HSOCKET hsock)
{
	if (hsock == NULL) return false;
	hsock->_stat = SOCKET_CLOSEING;
	epoll_mod_write(hsock);
	return true;
}

void HsocketClosed(HSOCKET hsock)
{
	if (hsock == NULL) return;
	__sync_sub_and_fetch(&hsock->_user->sockCount, 1);
	hsock->_stat = SOCKET_CLOSED;
	epoll_mod_write(hsock);
	return;
}

int HsocketPopBuf(HSOCKET hsock, int len)
{
#ifdef KCP_SUPPORT
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ctx->offset -= len;
		memmove(ctx->buf, ctx->buf + len, ctx->offset);
		return ctx->offset;
	}
#endif
	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	rbuf->offset -= len;
	memmove(rbuf->buff, rbuf->buff + len, rbuf->offset);
	return rbuf->offset;
}

void HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port){
	if (hsock->_conn_type == UDP_CONN || hsock->_conn_type == KCP_CONN) {
		hsock->peer_addr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, v6ip, &hsock->peer_addr.sin6_addr);
	}
}

void HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz)
{
    inet_ntop(AF_INET6, &hsock->peer_addr.sin6_addr, ip, ipsz);
	if (strncmp(ip, "::ffff:", 7) == 0) {
		memmove(ip, ip + 7, ipsz - 7);
	}
}

int HsocketPeerPort(HSOCKET hsock) 
{
    return ntohs(hsock->peer_addr.sin6_port);
}

void __STDCALL HsocketLocalIP(HSOCKET hsock, char* ip, size_t ipsz) {
	struct sockaddr_in6 local = { 0x0 };
	socklen_t len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	inet_ntop(AF_INET6, &local.sin6_addr, ip, ipsz);
	if (strncmp(ip, "::ffff:", 7) == 0) {
		memmove(ip, ip + 7, ipsz - 7);
	}
}
int __STDCALL HsocketLocalPort(HSOCKET hsock) {
	struct sockaddr_in6 local = { 0x0 };
	socklen_t len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	return ntohs(local.sin6_port);
}

BaseProtocol* __STDCALL HsocketBindUser(HSOCKET hsock, BaseProtocol* proto) {
	BaseProtocol* old = hsock->_user;
	__sync_sub_and_fetch (&old->sockCount, 1);
	hsock->_user = proto;
	__sync_add_and_fetch(&proto->sockCount, 1);
	return old;
}

int __STDCALL GetHostByName(const char* name, char* buf, size_t size) {
	struct addrinfo* res;
	int ret = getaddrinfo(name, NULL, NULL, &res);
	if (ret != 0) return -1;
	res->ai_family == AF_INET ?
		inet_ntop(res->ai_family, &((struct sockaddr_in*)res->ai_addr)->sin_addr, buf, size) :
		inet_ntop(res->ai_family, &((struct sockaddr_in6*)res->ai_addr)->sin6_addr, buf, size);
	return 0;
}

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* user){
	HsocketSendUdp((HSOCKET)user, buf, len);
	return 0;
}

int __STDCALL HsocketKcpCreate(HSOCKET hsock, int conv, int mode){
	if (hsock->_conn_type == UDP_CONN) {
		ikcpcb* kcp = ikcp_create(conv, hsock);
		if (!kcp) return -1;
		kcp->output = kcp_send_callback;
		kcp->stream = mode;
		Kcp_Content* ctx = (Kcp_Content*)malloc(sizeof(Kcp_Content));
		if (!ctx) { ikcp_release(kcp); return -1; }
		ctx->kcp = kcp;
		ctx->buf = (char*)malloc(DATA_BUFSIZE);
		if (!ctx->buf) { ikcp_release(kcp); free(ctx); return -1; }
		ctx->size = DATA_BUFSIZE;
		ctx->enable = 1;
		hsock->_user_data = ctx;
		hsock->_conn_type = KCP_CONN;
	}
	return 0;
}

void __STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc){
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);
	}
}

void __STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd){
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
	}
}

int __STDCALL HsocketKcpGetconv(HSOCKET hsock){
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		return ikcp_getconv(ctx->kcp);
	}
	return 0;
}

void __STDCALL HsocketKcpEnable(HSOCKET hsock, char enable){
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ctx->enable = enable;
	}
}

void __STDCALL HsocketKcpUpdate(HSOCKET hsock){
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		if (LONGTRYLOCK(&ctx->lock)){
			ikcp_update(ctx->kcp, iclock());
			LONGUNLOCK(&ctx->lock);
		}
	}
}

int __STDCALL HsocketKcpDebug(HSOCKET hsock, char* buf, int size) {
	int n = 0;
	if (hsock->_conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ikcpcb* kcp = ctx->kcp;
		n = snprintf(buf, size, "nsnd_buf[%d] nsnd_que[%d] nrev_buf[%d] nrev_que[%d] snd_wnd[%d] rev_wnd[%d] rmt_wnd[%d] cwd[%d]",
			kcp->nsnd_buf, kcp->nsnd_que, kcp->nrcv_buf, kcp->nrcv_que, kcp->snd_wnd, kcp->rcv_wnd, kcp->rmt_wnd, kcp->cwnd);
	}
	return n;
}
#endif