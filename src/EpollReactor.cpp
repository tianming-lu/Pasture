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
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>

#define DATA_BUFSIZE 5120

enum{
    ACCEPT = 0,
    READ,
	WRITE,
    CONNECT,
};

static HSOCKET new_hsockt()
{
	HSOCKET hsock = (HSOCKET)malloc(sizeof(EPOLL_SOCKET));
	if (hsock == NULL) return NULL;
	memset(hsock, 0, sizeof(EPOLL_SOCKET));
	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	rbuf->buff = (char*)malloc(DATA_BUFSIZE);
	if (rbuf->buff == NULL)
	{
		free(hsock);
		return NULL;
	}
	memset(rbuf->buff, 0, DATA_BUFSIZE);
	rbuf->offset = 0;
	rbuf->size = DATA_BUFSIZE;
	return hsock;
}

static void release_hsock(HSOCKET hsock)
{
	if (hsock)
	{
#ifdef KCP_SUPPORT
	if (hsock->_conn_type == KCP_CONN)
	{
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

static int get_listen_sock(const char* ip, int port)
{
	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	//addr.sin_addr.s_addr = htonl(INADDR_ANY);
	inet_pton(AF_INET, ip, &addr.sin_addr);
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	if(fd < 0) {
		return -1;
	}

	int optval = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
	struct linger linger = {0, 0};
	setsockopt(fd, SOL_SOCKET, SO_LINGER, (int *)&linger, sizeof(linger));

	if(bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) 
    {
		return -1;
	}

	if (listen(fd, 10)) 
    {
		return -1;
	}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}

static void epoll_add_accept(HSOCKET hsock) 
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epfd, EPOLL_CTL_ADD, hsock->fd, &ev);	
}

static void epoll_add_connect(HSOCKET hsock)
{
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epfd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static void epoll_add_read(HSOCKET hsock) 
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epfd, EPOLL_CTL_ADD, hsock->fd, &ev);	
}

static void epoll_mod_read(HSOCKET hsock) 
{
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epfd, EPOLL_CTL_MOD, hsock->fd, &ev);	
}

static void epoll_add_write(HSOCKET hsock)
{
	struct epoll_event ev;
	ev.events = EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static void epoll_mod_write(HSOCKET hsock)
{
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLERR | EPOLLET | EPOLLONESHOT;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_MOD, hsock->fd, &ev);
}

static void epoll_del_write(HSOCKET hsock) 
{
	struct epoll_event ev;
	epoll_ctl(hsock->factory->reactor->epwfd, EPOLL_CTL_DEL, hsock->fd, &ev);
}

static void epoll_del_read(HSOCKET hsock) 
{
	struct epoll_event ev;
	epoll_ctl(hsock->factory->reactor->epfd, EPOLL_CTL_DEL, hsock->fd, &ev);	
}

static void set_linger_for_fd(int fd)
{
	return;
    struct linger linger;
	linger.l_onoff = 0;
	linger.l_linger = 0;
	setsockopt(fd, SOL_SOCKET, SO_LINGER, (const void *) &linger, sizeof(struct linger));
}

static inline void AutoProtocolFree(BaseProtocol* proto) {
	AutoProtocol* autoproto = (AutoProtocol*)proto;
	autofree func = autoproto->freefunc;
	func(autoproto);
}

static void do_close(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	uint8_t left_count = 0;
	if (hsock->_stat < SOCKET_CLOSED )
	{

		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED)
		{
			left_count = __sync_sub_and_fetch (&proto->sockCount, 1);
			if (hsock->_stat == SOCKET_CONNECTING)
				proto->ConnectionFailed(hsock, errno);
			else
    			proto->ConnectionClosed(hsock, errno);

		}
		proto->UnLock();
	}
    
    epoll_del_read(hsock);
    close(hsock->fd);

	//if (left_count == 0 && proto->protoType == SERVER_PROTOCOL) fc->DeleteProtocol(proto);
	if (left_count == 0 && proto != NULL) {
		switch (proto->protoType)
		{
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

static void do_connect(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	epoll_add_write(hsock);
	if (hsock->_stat < SOCKET_CLOSED)
	{
		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED)
			proto->ConnectionMade(hsock);
		proto->UnLock();
	}
	hsock->_stat = SOCKET_CONNECTED;
	epoll_mod_read(hsock);
}

static int do_read_udp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	char* buf = NULL;
	size_t buflen = 0;
	int n = -1;
	int addr_len=sizeof(struct sockaddr_in);
	EPOLL_BUFF* rbuf = &hsock->_recv_buf;

	buf = rbuf->buff + rbuf->offset;
	buflen = rbuf->size - rbuf->offset;
AGAIN:
	n = recvfrom(hsock->fd, buf, buflen, MSG_DONTWAIT, (sockaddr*)&(hsock->peer_addr), (socklen_t*)&addr_len);
	if (n > 0)
	{
		rbuf->offset += n;
		return 0;
	}
	else if (n == 0)
	{
		return -1;
	}
	else if (errno == EINTR)
	{
		goto AGAIN;
	}
	else if (errno == EAGAIN)
	{
		return 0;
	}
	return -1;
}

static int do_read_tcp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	char* buf = NULL;
	size_t buflen = 0;
	ssize_t n = 0;
	EPOLL_BUFF* rbuf = &hsock->_recv_buf;
	while (1)
	{
		buf = rbuf->buff + rbuf->offset;
		buflen = rbuf->size - rbuf->offset;
		n = recv(hsock->fd, buf, buflen, MSG_DONTWAIT);
		if (n > 0)
		{
			rbuf->offset += n;
			if (size_t(n) == buflen)
			{
				size_t newsize = rbuf->size*2;
				char* newbuf = (char*)realloc(rbuf->buff , newsize);
				if (newbuf == NULL) return -1;
				rbuf->buff = newbuf;
				rbuf->size = newsize;
				continue;
			}
			break;
		}
		else if (n == 0)
		{
			return -1;
		}
		if (errno == EINTR)
		{
			continue;
		}
		else if (errno == EAGAIN)
		{
			break;
		}
		else
		{
			return -1;
		}
	}
	return 0;
}

static void do_timer_callback(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	uint64_t value;
	read(hsock->fd, &value, sizeof(uint64_t)); //必须读取，否则定时器异常
	if (hsock->_stat < SOCKET_CLOSED)
	{
		hsock->_callback(proto);
		epoll_mod_read(hsock);
	}
	else
	{
		epoll_del_read(hsock);
		close(hsock->fd);
		release_hsock(hsock);
	}
	
}

#ifdef KCP_SUPPORT
static void Kcp_recved(HSOCKET hsock, EPOLL_BUFF* rbuf, BaseFactory* fc, BaseProtocol* proto)
{
	Kcp_Content* ctx = (Kcp_Content*)(hsock->_user_data);
	ikcp_input(ctx->kcp, rbuf->buff, rbuf->offset);
	HsocketSkipBuf(hsock, rbuf->offset);

	int n = 0;
	int left = 0;
	while (1) {
		left = ctx->size - ctx->offset;
		if (left < ikcp_peeksize(ctx->kcp))
		{
			char* newbuf = (char*)realloc(ctx->buf, ctx->size + DATA_BUFSIZE);
			if (!newbuf) return;
			ctx->buf = newbuf;
			left = ctx->size - ctx->offset;
		}
		n = ikcp_recv(ctx->kcp, ctx->buf, left);
		if (n < 0) break;
		ctx->offset += n;

		if (hsock->_stat < SOCKET_CLOSED)
		{
			proto->Lock();
			if (hsock->_stat < SOCKET_CLOSED)
			{
				proto->ConnectionRecved(hsock, ctx->buf, ctx->offset);
				proto->UnLock();
			}
			else
			{
				proto->UnLock();
				return do_close(hsock, fc, proto);
			}
		}
		else
		{
			return do_close(hsock, fc, proto);
		}
	}
	epoll_mod_read(hsock);
}
#endif

static void do_read(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	if (hsock->_conn_type == ITMER)
		return do_timer_callback(hsock, fc, proto);

	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	//if (__sync_fetch_and_or(&rbuf->lock_flag, 1)) return;
	int ret = 0;
	if(hsock->_conn_type == TCP_CONN)
		ret = do_read_tcp(hsock, fc, proto);
	else
		ret = do_read_udp(hsock, fc, proto);
	
#ifdef KCP_SUPPORT
	if (hsock->_conn_type == KCP_CONN)
	{
		return Kcp_recved(hsock, rbuf, fc, proto);
	}
#endif

	if (ret == 0 && hsock->_stat < SOCKET_CLOSED)
	{
		proto->Lock();
		if (hsock->_stat < SOCKET_CLOSED)
			proto->ConnectionRecved(hsock, rbuf->buff, rbuf->offset);
		proto->UnLock();

	}
	else
	{
		return do_close(hsock, fc, proto);
	}
	
	epoll_mod_read(hsock);
	//__sync_fetch_and_and(&rbuf->lock_flag, 0);
}

static void do_accpet(HSOCKET listenhsock, BaseFactory* fc)
{
    struct sockaddr_in addr;
	socklen_t len;
   	int fd = 0;

	while (1)
	{
	    fd = accept(fc->Listenfd, (struct sockaddr *)&addr, (len = sizeof(addr), &len));
		if (fd < 0)
			break;
        HSOCKET hsock = new_hsockt();
		if (hsock == NULL) 
		{
			close(fd);
			continue;
		}
		memcpy(&hsock->peer_addr, &addr, sizeof(addr));

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
        proto->ConnectionMade(hsock);
        proto->UnLock();
		hsock->_stat = SOCKET_CONNECTED;
		epoll_add_read(hsock);
	}
}

static void sub_work_thread(void* args)
{
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
		n = epoll_wait(reactor->epfd, pev, reactor->maxevent, -1);
		for(i = 0; i < n; i++) 
		{
            hsock = (HSOCKET)pev[i].data.ptr;
			factory = hsock->factory;
			if (hsock->fd == factory->Listenfd){
				do_accpet(hsock, factory);
			}
			else if (pev[i].events == EPOLLIN){
				do_read(hsock, factory, hsock->_user);
			}
			else if (pev[i].events == EPOLLOUT){
				do_connect(hsock, factory, hsock->_user);
			}
			else{
				do_close(hsock, factory, hsock->_user);
			}
		}
	}
}

static void do_write_udp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	if (hsock->_stat < SOCKET_CLOSED)
	{
		socklen_t socklen = sizeof(struct sockaddr);
		EPOLL_BUFF *sbuf = &hsock->_send_buf;
		if (__sync_fetch_and_or(&sbuf->lock_flag, 1)) return;
		int slen = 0;
		int len;
		char* data;
		int n = 0;
		struct sockaddr* addr;
		while (slen < sbuf->offset)
		{
			data = sbuf->buff + slen + sizeof(int) + sizeof(struct sockaddr);
			len = *(int*)(sbuf->buff + slen);
			addr = (struct sockaddr*)(sbuf->buff + slen + sizeof(int));
			if (len > 0)
			{
				n = sendto(hsock->fd, data, len, MSG_DONTWAIT, addr, socklen);
				if(n > 0) 
				{
					slen += sizeof(int) + sizeof(struct sockaddr) + len;
				}
				else if(errno == EINTR || errno == EAGAIN) 
					break;
			}
		}
		sbuf->offset -= slen;
		memmove(sbuf->buff, sbuf->buff + slen, sbuf->offset);
		__sync_fetch_and_and(&sbuf->lock_flag, 0);

		if (hsock->_stat == SOCKET_CLOSEING){
			epoll_del_write(hsock);
			shutdown(hsock->fd, SHUT_RD);
		}
	}
}

static void do_write_tcp(HSOCKET hsock, BaseFactory* fc, BaseProtocol* proto)
{
	if (hsock->_stat < SOCKET_CLOSED)
	{
		EPOLL_BUFF *sbuf = &hsock->_send_buf;
		//while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
		if (__sync_fetch_and_or(&sbuf->lock_flag, 1)) return;

		char* data = sbuf->buff;
		int len = sbuf->offset;
		int not_over = 0;
		if (len > 0)
		{
			int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
			if(n > 0) 
			{
				sbuf->offset -= n;
				memmove(data, data + n, sbuf->offset);
				if (n < len)
					not_over = 1;
			}
			else if(errno == EINTR || errno == EAGAIN) 
				not_over = 1;
		}
		__sync_fetch_and_and(&sbuf->lock_flag, 0);
	
		if (not_over)
			epoll_mod_write(hsock);
		else if (hsock->_stat >= SOCKET_CLOSEING){
			epoll_del_write(hsock);
			shutdown(hsock->fd, SHUT_RD);
		}
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
			if (pev[i].events == EPOLLOUT){
				do_write(hsock, factory, hsock->_user);
			}
		}
	}
}

static void main_work_thread(void* args)
{
	Reactor* reactor = (Reactor*)args;
    int i = 0;
	int rc;
    for (; i < reactor->CPU_COUNT*2; i++)
	{
		pthread_attr_t attr;
   		pthread_t tid;
		pthread_attr_init(&attr);
		if((rc = pthread_create(&tid, &attr, (void*(*)(void*))sub_work_thread, reactor)) != 0)
		{
			return;
		}

		pthread_attr_t wattr;
   		pthread_t wtid;
		pthread_attr_init(&wattr);
		if((rc = pthread_create(&wtid, &wattr, (void*(*)(void*))write_work_thread, reactor)) != 0)
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
		usleep(20*1000);
	}
}

int ReactorStart(Reactor* reactor)
{
    reactor->epfd = epoll_create(reactor->maxevent);
	if(reactor->epfd < 0)
		return -1;
	reactor->epwfd = epoll_create(reactor->maxevent);
	if(reactor->epwfd < 0)
		return -1;

	reactor->CPU_COUNT = get_nprocs_conf();
	pthread_attr_t attr;
   	pthread_t tid;
	pthread_attr_init(&attr);
	pthread_attr_setstacksize(&attr, 1024*1024*16);
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
		if (fc->Listenfd < 0)
			return -2;
		HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(fc->Listenfd);
			return -3;
		}
		hsock->fd = fc->Listenfd;
		hsock->factory = fc;

		epoll_add_accept(hsock);
	}
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

static bool EpollConnectExUDP(BaseProtocol* proto, HSOCKET hsock)
{
	int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if(fd < 0) {
		return false;
	}
	hsock->fd = fd;
	sockaddr_in local_addr;
	memset(&local_addr, 0, sizeof(sockaddr_in));
	local_addr.sin_family = AF_INET;
	
	if(bind(fd, (struct sockaddr *)&local_addr, sizeof(sockaddr_in)) < 0) 
    {
		close(fd);
		release_hsock(hsock);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	epoll_add_write(hsock);
	epoll_add_read(hsock);
	return true;
}

static bool EpollConnectExTCP(BaseProtocol* proto, HSOCKET hsock)
{
	int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if(fd < 0) {
		return false;
	}
	hsock->fd = fd;
	set_linger_for_fd(fd);
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
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
	hsock->peer_addr.sin_family = AF_INET;
	hsock->peer_addr.sin_port = htons(port);
	inet_pton(AF_INET, ip, &hsock->peer_addr.sin_addr);

	hsock->factory = proto->factory;
	hsock->_conn_type = type;
	hsock->_user = proto;

	bool ret = false;
	if (type == UDP_CONN)
		ret =  EpollConnectExUDP(proto, hsock);
	else
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
	epoll_add_read(hsock);
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
	int needlen = sizeof(int) + sizeof(struct sockaddr) + len;
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
	memcpy(sbuf->buff + sbuf->offset + sizeof(int), (char*)&hsock->peer_addr, sizeof(struct sockaddr));
    memcpy(sbuf->buff + sbuf->offset + sizeof(int) + sizeof(struct sockaddr), data, len);
    sbuf->offset += needlen;
	__sync_fetch_and_and(&sbuf->lock_flag, 0);
    epoll_mod_write(hsock);	
}

bool HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len)
{
	//socklen_t socklen = sizeof(hsock->peer_addr);
	//sendto(hsock->fd, data, len, 0, (struct sockaddr*)&hsock->peer_addr, socklen);
	EPOLL_BUFF *sbuf = &hsock->_send_buf;
	int needlen = sizeof(int) + sizeof(struct sockaddr) + len;
    if (sbuf->buff == NULL)
    {
        sbuf->buff = (char*)malloc(needlen);
    	if (sbuf->buff == NULL) return false;
        sbuf->size = needlen;
        sbuf->offset = 0;
    }
	while (__sync_fetch_and_or(&sbuf->lock_flag, 1)) usleep(0);
	int newlen = sbuf->offset + needlen;
	if (sbuf->size < newlen)
    {
        char* newbuf = (char*)realloc(sbuf->buff, newlen);
        if (newbuf == NULL) {__sync_fetch_and_and(&sbuf->lock_flag, 0);return false;}
        sbuf->buff = newbuf;
        sbuf->size = newlen;
    }
	memcpy(sbuf->buff + sbuf->offset, (char*)&len, sizeof(int));
	struct sockaddr_in* addr = (struct sockaddr_in*)sbuf->buff + sizeof(int);
	addr->sin_family = AF_INET;
	addr->sin_port = htons(port);
	inet_pton(AF_INET, ip, &addr->sin_addr);
	//memcpy(sbuf->buff + sbuf->offset + sizeof(int), (char*)&hsock->peer_addr, sizeof(struct sockaddr));
    memcpy(sbuf->buff + sbuf->offset + sizeof(int) + sizeof(struct sockaddr), data, len);
    sbuf->offset += needlen;
	__sync_fetch_and_and(&sbuf->lock_flag, 0);
    epoll_mod_write(hsock);
	return true;
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
#ifdef KCP_SUPPORT
	if (hsock->_conn_type == KCP_CONN)
	{
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
		ikcp_send(ctx->kcp, data, len);
		return true;
	}
#endif
	if (hsock->_conn_type == UDP_CONN)
		HsocketSendUdp(hsock, data, len);
	else
		HsocketSendTcp(hsock, data, len);
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
	if (hsock == NULL || hsock->fd < 0) return false;
	//shutdown(hsock->fd, SHUT_RD);
	//close(hsock->fd);  直接关闭epoll没有事件通知，所以需要shutdown

	hsock->_stat = SOCKET_CLOSEING;
	epoll_mod_write(hsock);
	return true;
}

void HsocketClosed(HSOCKET hsock)
{
	if (hsock == NULL || hsock->fd < 0) return;
	__sync_sub_and_fetch(&hsock->_user->sockCount, 1);
	hsock->_stat = SOCKET_CLOSED;
	epoll_mod_write(hsock);
	return;
}

int HsocketSkipBuf(HSOCKET hsock, int len)
{
	EPOLL_BUFF *rbuf = &hsock->_recv_buf;
	rbuf->offset -= len;
	memmove(rbuf->buff, rbuf->buff + len, rbuf->offset);
	return rbuf->offset;
}

void HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz)
{
    inet_ntop(AF_INET, &hsock->peer_addr.sin_addr, ip, ipsz);
}

int HsocketPeerPort(HSOCKET hsock) 
{
    return ntohs(hsock->peer_addr.sin_port);
}

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* user)
{
	HsocketSendUdp((HSOCKET)user, buf, len);
	return 0;
}

int __STDCALL HsocketKcpCreate(HSOCKET hsock, int conv)
{
	ikcpcb* kcp = ikcp_create(conv, hsock);
	if (!kcp) return -1;
	kcp->output = kcp_send_callback;
	Kcp_Content* ctx = (Kcp_Content*)malloc(sizeof(Kcp_Content));
	if (!ctx) { ikcp_release(kcp); return -1; }
	ctx->kcp = kcp;
	ctx->buf = (char*)malloc(sizeof(DATA_BUFSIZE));
	if (!ctx->buf) { ikcp_release(kcp); free(ctx); return -1; }
	ctx->size = DATA_BUFSIZE;
	hsock->_user_data = ctx;
	hsock->_conn_type = KCP_CONN;
	return 0;
}

int __STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc)
{
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);
	return 0;
}

int __STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd)
{
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
	return 0;
}

int __STDCALL HsocketKcpGetconv(HSOCKET hsock)
{
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	return ikcp_getconv(ctx->kcp);
}

void __STDCALL HsocketKcpEnable(HSOCKET hsock, char enable)
{
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	ctx->enable = enable;
}
#endif