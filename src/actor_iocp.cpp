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

#include "actor.h"
#include <Mstcpip.h>
#include <time.h>
#include <map>

#pragma comment(lib, "Ws2_32.lib")
#ifdef OPENSSL_SUPPORT
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "libssl.lib")
#endif

#define DATA_BUFSIZE 8192
#define SOCKET_READ		0
#define SOCKET_WRITE	1
#define SOCKET_ACCEPT	2
#define SOCKET_CONNECT	3
#define SOCKET_ACCEPTED 4
#define SOCKET_CLOSE	5
#define SOCKET_UNBIND	6
#define SOCKET_REBIND	7

int  ActorThreadWorker = 0;

typedef struct Thread_Content {
	long	WorkerCount;
	HANDLE	CompletionPort;
}ThreadStat;
#define THREAD_STAT_SIZE sizeof(ThreadStat)

static HANDLE ListenCompletionPort = NULL;
static ThreadStat* ThreadStats;

static LPFN_ACCEPTEX lpfnAcceptEx = NULL;
static LPFN_CONNECTEX lpfnConnectEx = NULL;
static int sockaddr_len = sizeof(struct sockaddr_in6);
static DWORD WSARECV_FLAG = 0;

static bool HsocketSendEx(HSOCKET IocpSock, const char* data, int len);

typedef long NTSTATUS;
typedef long FILE_INFORMATION_CLASS;
typedef struct _IO_STATUS_BLOCK {
	union {
		NTSTATUS Status;
		PVOID    Pointer;
	};
	ULONG_PTR Information;
} IO_STATUS_BLOCK, * PIO_STATUS_BLOCK;
typedef struct _FILE_COMPLETION_INFORMATION {
	HANDLE Port;
	PVOID  Key;
} FILE_COMPLETION_INFORMATION, * PFILE_COMPLETION_INFORMATION;
typedef NTSTATUS(__stdcall* LPFN_NtSetInformationFile)(HANDLE, PIO_STATUS_BLOCK, PVOID, ULONG, FILE_INFORMATION_CLASS);
static LPFN_NtSetInformationFile ReplaceIoCompletionPortEx = NULL;
static long ReplaceIoCompletionPort(SOCKET fd, HANDLE CompletionPort, PVOID CompletionKey) {  //取消绑定完成端口CompletionPort为NULL,尚有未完成的IO重叠操作会导致失败 
	IO_STATUS_BLOCK block = {};
	FILE_COMPLETION_INFORMATION fileinfo;
	fileinfo.Port = CompletionPort;
	fileinfo.Key = CompletionKey;
	const FILE_INFORMATION_CLASS FileReplaceCompletionInformation = 61;
	NTSTATUS ret = ReplaceIoCompletionPortEx((HANDLE)fd, &block, &fileinfo, sizeof(fileinfo), FileReplaceCompletionInformation);
	return ret;
}

#define THREAD_STATES_AT(x) ThreadStats + x;
ThreadStat* __STDCALL ThreadDistribution(BaseWorker* worker) {
	char thread_id = worker->thread_id;
	if (thread_id > -1) return THREAD_STATES_AT(thread_id);
	ThreadStat* tsa, * tsb;
	thread_id = 0;
	tsa = THREAD_STATES_AT(0);
	for (int i = 1; i <= ActorThreadWorker; i++) {
		tsb = THREAD_STATES_AT(i);
		if (tsb->WorkerCount < tsa->WorkerCount) {
			tsa = tsb;
			thread_id = i;
		}
	}
	InterlockedIncrement(&tsa->WorkerCount);
	worker->thread_id = thread_id;
	return tsa;
}

ThreadStat* __STDCALL ThreadDistributionIndex(BaseWorker* worker, int index) {
	char thread_id = worker->thread_id;
	if (thread_id > -1) return THREAD_STATES_AT(thread_id);
	if (index > -1 && index <= ActorThreadWorker) {
		ThreadStat* ts = THREAD_STATES_AT(index);
		InterlockedIncrement(&ts->WorkerCount);
		worker->thread_id = index;
		return ts;
	}
	return NULL;
}

void __STDCALL ThreadUnDistribution(BaseWorker* worker) {
	short thread_id = worker->thread_id;
	if (thread_id > -1) {
		ThreadStat* ts = THREAD_STATES_AT(thread_id);
		InterlockedDecrement(&ts->WorkerCount);
		worker->thread_id = -1;
	}
}

#ifdef OPENSSL_SUPPORT
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/bio.h>

struct SSL_Content {
	SSL_CTX* ctx;
	SSL* ssl;
	BIO* rbio;
	BIO* wbio;

	char*	rbuf;
	int		rsize;
	int		roffset;
	char*	wbuf;
	int		wsize;
	int		woffset;
	char	wlock;
};
#define SSL_CTX_SIZE sizeof(SSL_Content)
#endif

#ifdef KCP_SUPPORT
#include "ikcp.h"
#include "time.h"

struct Kcp_Content{
	ikcpcb* kcp;
	char*	buf;
	int		size;
	int		offset;
};

#define KCP_CTX_SIZE sizeof(Kcp_Content)

static inline void itimeofday(long* sec, long* usec){
	static long mode = 0, addsec = 0;
	BOOL retval;
	static IINT64 freq = 1;
	IINT64 qpc;
	if (mode == 0) {
		QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0) ? 1 : freq;
		QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
		addsec = (long)time(NULL);
		addsec = addsec - (long)((qpc / freq) & 0x7fffffff);
		mode = 1;
	}
	retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
	retval = retval * 2;
	if (sec) *sec = (long)(qpc / freq) + addsec;
	if (usec) *usec = (long)((qpc % freq) * 1000000 / freq);
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

static inline HSOCKET NewIOCP_Socket(){
	HSOCKET hsock = (HSOCKET)malloc(sizeof(Socket_Content));
	if (hsock) {memset(hsock, 0x0, sizeof(Socket_Content));}
	else { printf("%s:%d memory malloc error\n", __func__, __LINE__); }
	return hsock;
}

static inline void ReleaseIOCP_Socket(HSOCKET IocpSock){
#if defined OPENSSL_SUPPORT || defined KCP_SUPPORT
	switch (IocpSock->protocol) {
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL: {
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)IocpSock->sock_data;
		if (ssl_ctx) {
			//BIO_free(ssl_ctx->rbio);  //貌似bio会随着SSL_free一起释放，先行释放会引起崩溃，待后续确认
			//BIO_free(ssl_ctx->wbio);
			if (ssl_ctx->wbuf) free(ssl_ctx->wbuf);
			if (ssl_ctx->rbuf) free(ssl_ctx->rbuf);
			if (ssl_ctx->ssl) { SSL_shutdown(ssl_ctx->ssl); SSL_free(ssl_ctx->ssl); }
			if (ssl_ctx->ctx) SSL_CTX_free(ssl_ctx->ctx);
			free(ssl_ctx);
		}
		break;
	}
#endif
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL: {
		Kcp_Content* ctx = (Kcp_Content*)IocpSock->sock_data;
		ikcp_release(ctx->kcp);
		free(ctx->buf);
		free(ctx);
		break;
	}
#endif
	default:
		break;
	}
#endif
	SOCKET fd = IocpSock->fd;
	if (fd != INVALID_SOCKET && fd != NULL) closesocket(fd);
	if (IocpSock->recv_buf) free(IocpSock->recv_buf);
	free(IocpSock);
}

static inline const char* socket_ip_v4_converto_v6(const char* src, char* dst, size_t size) {
	if (strchr(src, ':')) {
		return src;
	}
	else {
		snprintf(dst, size, "::ffff:%s", src);
		return dst;
	}
}

static inline void socket_set_v6only(SOCKET fd, int v6only) {
	setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, (char*)&v6only, sizeof(v6only));
}

static inline void set_linger_for_fd(SOCKET fd) {
	struct linger linger;
	linger.l_onoff = 0;
	linger.l_linger = 0;
	setsockopt(fd, SOL_SOCKET, SO_LINGER, (const char*)&linger, sizeof(struct linger));
}

static inline SOCKET get_listen_sock(const char* ip, int port){
	SOCKET listenSock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	struct sockaddr_in6 server_addr = {0x0};
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &server_addr.sin6_addr);
	//server_addr.sin6_addr = in6addr_any;
	socket_set_v6only(listenSock, 0);

	u_long nonblock = 1;
	ioctlsocket(listenSock, FIONBIO, &nonblock);
	int ret = bind(listenSock, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if (ret != 0){
		closesocket(listenSock);
		return SOCKET_ERROR;
	}
	listen(listenSock, 10);
	if (listenSock == SOCKET_ERROR){
		closesocket(listenSock);
		return SOCKET_ERROR;
	}
	return listenSock;
}

static inline void hsocket_set_keepalive(SOCKET fd) {  //这个函数使用有问题，尚未验证其正确性
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));
#define tcp_keepalive_size sizeof(struct tcp_keepalive)
	struct tcp_keepalive in_keep_alive = { 0x0 };
	unsigned long ul_bytes_return = 0;
	in_keep_alive.onoff = 1; /*打开keepalive*/
	in_keep_alive.keepaliveinterval = 1200*000*000; /*发送keepalive心跳时间间隔-单位为毫秒*/
	in_keep_alive.keepalivetime = 1200*000*000; /*多长时间没有报文开始发送keepalive心跳包-单位为毫秒*/
	int ret = WSAIoctl(fd, SIO_KEEPALIVE_VALS, (LPVOID)&in_keep_alive, tcp_keepalive_size,
		NULL, 0, &ul_bytes_return, NULL, NULL);
	if (ret == SOCKET_ERROR) {
		printf("%s:%d %d\n", __func__, __LINE__, WSAGetLastError());
	}
}

static inline int PostAcceptClient(BaseAccepter* accepter){
	HSOCKET IocpSock = NewIOCP_Socket();
	if (!IocpSock){
		return -1;
	}
	IocpSock->event_type = SOCKET_ACCEPT;
	IocpSock->worker = NULL;
	IocpSock->sock_data = accepter;
	char* recv_buf = (char*)malloc(DATA_BUFSIZE);
	if (recv_buf == NULL){
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		ReleaseIOCP_Socket(IocpSock);
		return -2;
	}
	IocpSock->recv_buf = recv_buf;
	IocpSock->size = DATA_BUFSIZE;

	SOCKET fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET){
		ReleaseIOCP_Socket(IocpSock);
		return -3;
	}
	IocpSock->fd = fd;
	u_long nonblock = 1;
	ioctlsocket(fd, FIONBIO, &nonblock);
	set_linger_for_fd(fd);
	/*调用AcceptEx函数，地址长度需要在原有的上面加上16个字节向服务线程投递一个接收连接的的请求*/
#define NetAddrLength sizeof(struct sockaddr_in6) + 16
	bool rc = lpfnAcceptEx(accepter->Listenfd, fd, recv_buf, 0, NetAddrLength, NetAddrLength, NULL, (LPOVERLAPPED)IocpSock);
	if (false == rc){
		if (WSAGetLastError() != ERROR_IO_PENDING){
			ReleaseIOCP_Socket(IocpSock);
			return -4;
		}
	}
	return 0;
}

static inline void delete_worker(BaseWorker* worker) {
	if (worker->ref_count == 0 && worker->auto_free_flag == FREE_AUTO) {
		worker->_free();
	}
}

static int do_close(HSOCKET IocpSock, char sock_io_type, int err){
	switch (sock_io_type){
	case SOCKET_ACCEPT: {
		BaseAccepter* accepter = (BaseAccepter*)(IocpSock->sock_data);
		if (PostAcceptClient(accepter)) accepter->Listening = false;
		ReleaseIOCP_Socket(IocpSock);
		return 0;
	}
	case SOCKET_WRITE: {
		HSENDBUFF IocpBuff = (HSENDBUFF)IocpSock;
		if (IocpBuff->databuf.buf != NULL) free(IocpBuff->databuf.buf);
		free(IocpBuff);
		return 0;
	}
	case SOCKET_UNBIND: {
		long ret = ReplaceIoCompletionPort(IocpSock->fd, NULL, NULL);
		if (!ret) {
			BaseWorker* old = IocpSock->worker;
			old->ref_count--;
			IocpSock->worker = NULL;
			WorkerBind_Callback call = IocpSock->bind_call;
			call(IocpSock, old, IocpSock->call_data, 0);
			delete_worker(old);
		}
		return 0;
	}
	case SOCKET_REBIND: {
		long ret = ReplaceIoCompletionPort(IocpSock->fd, NULL, NULL);
		if (!ret) {
			BaseWorker* old = IocpSock->worker;
			old->ref_count--;

			BaseWorker* worker = IocpSock->rebind_worker;
			IocpSock->worker = worker;
			IocpSock->event_type = SOCKET_REBIND;
			ThreadStat* ts = ThreadDistribution(worker);
			PostQueuedCompletionStatus(ts->CompletionPort, 0, (ULONG_PTR)IocpSock, (LPOVERLAPPED)IocpSock);

			delete_worker(old);
		}
		return 0;
	}
	default:
		break;
	}

	if (IocpSock->fd != INVALID_SOCKET){
		BaseWorker* worker = IocpSock->worker;
		worker->ref_count--;
		if (SOCKET_CONNECT != IocpSock->event_type)
			worker->ConnectionClosed(IocpSock, err);
		else
			worker->ConnectionFailed(IocpSock, err);
		delete_worker(worker);
	}
	ReleaseIOCP_Socket(IocpSock);
	return 0;
}

static inline bool ResetIocp_Buff(HSOCKET IocpSock){
	memset((LPOVERLAPPED)IocpSock, 0, sizeof(OVERLAPPED));
	IocpSock->databuf.len = IocpSock->size - IocpSock->offset;
	if (IocpSock->databuf.len == 0){
		int new_size = IocpSock->size * 2;
		char* new_ptr = (char*)realloc(IocpSock->recv_buf, new_size);
		if (new_ptr == NULL) {
			printf("%s:%d memory realloc error\n", __func__, __LINE__);
			return false;
		}
		IocpSock->recv_buf = new_ptr;
		IocpSock->size = new_size;
		IocpSock->databuf.len = IocpSock->size - IocpSock->offset;
	}
	IocpSock->databuf.buf = IocpSock->recv_buf + IocpSock->offset;
	return true;
}

static inline int PostRecvUDP(HSOCKET IocpSock){
	if (SOCKET_ERROR == WSARecvFrom(IocpSock->fd, &IocpSock->databuf, 1, NULL, &WSARECV_FLAG,
		(struct sockaddr*)&IocpSock->peer_addr, &sockaddr_len, (LPOVERLAPPED)IocpSock, NULL)){
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			return do_close(IocpSock, IocpSock->event_type, err);
		}
	}
	return 0;
}

static inline int PostRecvTCP(HSOCKET IocpSock){
	DWORD recv_len = 0;
	int ret = WSARecv(IocpSock->fd, &IocpSock->databuf, 1, &recv_len, &WSARECV_FLAG, (LPOVERLAPPED)IocpSock, NULL);
	if (SOCKET_ERROR != ret) {
		if (recv_len == 0) do_close(IocpSock, IocpSock->event_type, 0);  //接收到0个字节关闭连接
		return recv_len;
	} 
	else{
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			return do_close(IocpSock, IocpSock->event_type, err);
		}
	}
	return 0;
}

static int PostRecv(HSOCKET IocpSock){
	if (IocpSock->event_type >= SOCKET_CLOSE)
		return do_close(IocpSock, IocpSock->event_type, 0);

	IocpSock->event_type = SOCKET_READ;
	if (ResetIocp_Buff(IocpSock) == false){
		return do_close(IocpSock, IocpSock->event_type, -1);
	}
	PROTOCOL protocol = IocpSock->protocol;
	if (protocol == TCP_PROTOCOL 
#ifdef OPENSSL_SUPPORT
		|| protocol == SSL_PROTOCOL
#endif
		)
		return PostRecvTCP(IocpSock);
	return PostRecvUDP(IocpSock);
}

static void do_accept_go_on(BaseAccepter* accepter) {
	SOCKET fd, listenfd = accepter->Listenfd;
	HSOCKET IocpSock;
	ThreadStat* ts;
	BaseWorker* worker;
	struct sockaddr_in6 addr;
	int len = sizeof(addr);
	u_long nonblock = 1;
	HANDLE CompletionPort;
	char* recv_buf;

	while (1) {
		fd = accept(listenfd, (struct sockaddr*)&addr, &len);
		if (fd == INVALID_SOCKET) break;
		ioctlsocket(fd, FIONBIO, &nonblock);
		set_linger_for_fd(fd);

		IocpSock = NewIOCP_Socket();
		worker = accepter->GetWorker();
		if (!IocpSock || !worker) {
			closesocket(fd);
			goto error;
		}
		IocpSock->fd = fd;
		IocpSock->event_type = SOCKET_ACCEPTED;
		IocpSock->worker = worker;
		IocpSock->sock_data = accepter;
		recv_buf = (char*)malloc(DATA_BUFSIZE);
		if (recv_buf == NULL) {
			printf("%s:%d memory malloc error\n", __func__, __LINE__);
			goto error;
		}
		IocpSock->recv_buf = recv_buf;
		IocpSock->size = DATA_BUFSIZE;
		memcpy(&IocpSock->peer_addr, &addr, sizeof(addr));

		if (worker->auto_free_flag == FREE_DEF) worker->auto_free_flag = FREE_AUTO;
		ts = ThreadDistribution(worker);
		CompletionPort = ts->CompletionPort;
		CreateIoCompletionPort((HANDLE)fd, CompletionPort, (ULONG_PTR)IocpSock, 0);	//将监听到的套接字关联到完成端口
		SetFileCompletionNotificationModes((HANDLE)fd, FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
		PostQueuedCompletionStatus(CompletionPort, 0, (ULONG_PTR)IocpSock, (LPOVERLAPPED)IocpSock);
		continue;
	error:
		if (IocpSock) ReleaseIOCP_Socket(IocpSock);
		if (worker) worker->_free();
	}
}

static int do_accept(HSOCKET IocpSock){
	BaseAccepter* accepter = (BaseAccepter*)IocpSock->sock_data;
	
	/*连接成功后刷新套接字属性, 不然获取套接字属性可能会出错*/
	setsockopt(IocpSock->fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&(accepter->Listenfd), sizeof(accepter->Listenfd));
	SetFileCompletionNotificationModes((HANDLE)IocpSock->fd, FILE_SKIP_SET_EVENT_ON_HANDLE| FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
	//hsocket_set_keepalive(IocpSock->fd);

	BaseWorker* worker = accepter->GetWorker();
	if (worker) {
		if (worker->auto_free_flag == FREE_DEF) worker->auto_free_flag = FREE_AUTO;

		IocpSock->worker = worker;
		IocpSock->sock_data = NULL;
		IocpSock->event_type = SOCKET_ACCEPTED;
		
		SOCKET fd = IocpSock->fd;
		int nSize = sizeof(IocpSock->peer_addr);
		getpeername(fd, (struct sockaddr*)&IocpSock->peer_addr, &nSize);

		ThreadStat* ts = ThreadDistribution(worker);
		HANDLE CompletionPort = ts->CompletionPort;
		CreateIoCompletionPort((HANDLE)fd, CompletionPort, (ULONG_PTR)IocpSock, 0);	//将监听到的套接字关联到完成端口
		PostQueuedCompletionStatus(CompletionPort, 0, (ULONG_PTR)IocpSock, (LPOVERLAPPED)IocpSock);
		do_accept_go_on(accepter);
		if (PostAcceptClient(accepter)) accepter->Listening = false;
	}
	else {
		do_close(IocpSock, IocpSock->event_type, -1);
	}
	return 0;
}

#ifdef KCP_SUPPORT
static int do_read_kcp(HSOCKET IocpSock){
	Kcp_Content* ctx = (Kcp_Content*)(IocpSock->sock_data);
	ikcp_input(ctx->kcp, IocpSock->recv_buf, IocpSock->offset);
	IocpSock->offset = 0;
	char* buf;
	int rlen, size;
	while (1) {
		buf = ctx->buf + ctx->offset;
		size = ctx->size - ctx->offset;
		rlen = ikcp_recv(ctx->kcp, buf, size);
		if (rlen < 0) {
			if (rlen == -3) {
				size = ctx->size * 2;
				buf = (char*)realloc(ctx->buf, size);
				if (buf) {
					ctx->buf = buf;
					ctx->size = size;
					continue;
				}
				printf("%s:%d memory realloc error\n", __func__, __LINE__);
				return do_close(IocpSock, IocpSock->event_type, -1);
			}
			break; 
		}
		ctx->offset += rlen;
		if (IocpSock->fd != INVALID_SOCKET){
			BaseWorker* worker = IocpSock->worker;
			worker->ConnectionRecved(IocpSock, ctx->buf, ctx->offset);
			continue;
		}
		return do_close(IocpSock, IocpSock->event_type, 0);
	}
	return PostRecv(IocpSock);
}
#endif

#ifdef OPENSSL_SUPPORT
static void ssl_do_write(struct SSL_Content* ssl_ctx, HSOCKET IocpSock) {
	char* buf;
	int rlen, size;
	while (1) {
		buf = ssl_ctx->wbuf + ssl_ctx->woffset;
		size = ssl_ctx->wsize - ssl_ctx->woffset;
		rlen = BIO_read(ssl_ctx->wbio, buf, size);
		if (rlen > 0) {
			ssl_ctx->woffset += rlen;
			if (rlen == size) {
				size = ssl_ctx->wsize * 2;
				buf = (char*)realloc(ssl_ctx->wbuf, size);
				if (!buf) {
					printf("%s:%d memory realloc error\n", __func__, __LINE__);
					break;
				}
				ssl_ctx->wbuf = buf;
				ssl_ctx->wsize = size;
			}
		}
		else {
			break;
		}
	}
	if (ssl_ctx->woffset) HsocketSendEx(IocpSock, ssl_ctx->wbuf, ssl_ctx->woffset);
	ssl_ctx->woffset = 0;
}

static int ssl_do_handshake(struct SSL_Content* ssl_ctx, HSOCKET IocpSock) {
	BIO_write(ssl_ctx->rbio, IocpSock->recv_buf, IocpSock->offset);
	IocpSock->offset = 0;
	int r = SSL_do_handshake(ssl_ctx->ssl);
	ssl_do_write(ssl_ctx, IocpSock);

	if (r == 1) {
		if (IocpSock->fd != INVALID_SOCKET) {
			BaseWorker* worker = IocpSock->worker;
			worker->ConnectionMade(IocpSock, IocpSock->protocol);
			return PostRecv(IocpSock);
		}
		return do_close(IocpSock, IocpSock->event_type, 0);
	}
	else {
		int err_SSL_get_error = SSL_get_error(ssl_ctx->ssl, r);
		switch (err_SSL_get_error) {
		case SSL_ERROR_WANT_WRITE:
		case SSL_ERROR_WANT_READ:
			return PostRecv(IocpSock);
		default:
			return do_close(IocpSock, IocpSock->event_type, err_SSL_get_error);
		}
	}
}

static int do_read_ssl(HSOCKET IocpSock) {
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)IocpSock->sock_data;
	if (SSL_is_init_finished(ssl_ctx->ssl)) {
		int ret = BIO_write(ssl_ctx->rbio, IocpSock->recv_buf, IocpSock->offset);
		IocpSock->offset = 0;
		char* buf;
		int rlen, size;
		while (1) {
			buf = ssl_ctx->rbuf + ssl_ctx->roffset;
			size = ssl_ctx->rsize - ssl_ctx->roffset;
			rlen = SSL_read(ssl_ctx->ssl, buf, size);
			if (rlen > 0) {
				ssl_ctx->roffset += rlen;
				if (rlen == size) {
					size = ssl_ctx->rsize * 2;
					buf = (char*)realloc(ssl_ctx->rbuf, size);
					if (!buf) {
						printf("%s:%d memory realloc error\n", __func__, __LINE__);
						break;
					} 
					ssl_ctx->rbuf = buf;
					ssl_ctx->rsize = size;
				}
				continue;
			}
			break;
		}
		
		if (ssl_ctx->roffset > 0) {
			if (IocpSock->fd != INVALID_SOCKET) {
				BaseWorker* worker = IocpSock->worker;
				worker->ConnectionRecved(IocpSock, ssl_ctx->rbuf, ssl_ctx->roffset);
				return PostRecv(IocpSock);
			}
			return do_close(IocpSock, IocpSock->event_type, 0);
		}
		else {
			return PostRecv(IocpSock);
		}
	}
	else {
		return ssl_do_handshake(ssl_ctx, IocpSock);
	}
}
#endif

static int do_read_tcp_and_udp(HSOCKET IocpSock) {
	if (IocpSock->fd != INVALID_SOCKET) {
		BaseWorker* worker = IocpSock->worker;
		worker->ConnectionRecved(IocpSock, IocpSock->recv_buf, IocpSock->offset);
		return PostRecv(IocpSock);
	}
	return do_close(IocpSock, IocpSock->event_type, 0);
}

static int do_read(HSOCKET IocpSock) {
	switch (IocpSock->protocol) {
	case TCP_PROTOCOL:
	case UDP_PROTOCOL:
		return do_read_tcp_and_udp(IocpSock);
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL:
		return do_read_ssl(IocpSock);
#endif
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL:
		return do_read_kcp(IocpSock);
#endif
	default:
		break;
	}
	return 0;
}

#ifdef OPENSSL_SUPPORT
static bool Hsocket_SSL_init(HSOCKET IocpSock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key) {
	struct SSL_Content* ssl_ctx = (SSL_Content*)malloc(sizeof(struct SSL_Content));
	if (!ssl_ctx) {
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		return false;
	}
	memset(ssl_ctx, 0x0, sizeof(SSL_Content));
	ssl_ctx->ctx = openssl_type == SSL_CLIENT? SSL_CTX_new(SSLv23_client_method()): SSL_CTX_new(SSLv23_server_method());
	if (!ssl_ctx->ctx) { free(ssl_ctx); return false; }

	verify ? SSL_CTX_set_verify(ssl_ctx->ctx, SSL_VERIFY_PEER, NULL): SSL_CTX_set_verify(ssl_ctx->ctx, SSL_VERIFY_NONE, NULL);
	BIO* bio;
	X509* cert;
	if (ca_crt) {
		bio = BIO_new_mem_buf(ca_crt, (int)strlen(ca_crt));
		cert = PEM_read_bio_X509(bio, NULL, NULL, NULL); //PEM格式 DER格式用d2i_X509_bio(cbio, NULL);
		X509_STORE * certS = SSL_CTX_get_cert_store(ssl_ctx->ctx);
		X509_STORE_add_cert(certS, cert);
		X509_free(cert);
		BIO_free(bio);
		//SSL_CTX_load_verify_locations(ssl_ctx->ctx, ca_crt, NULL);
	}
	if (user_crt) {
		bio = BIO_new_mem_buf(user_crt, (int)strlen(user_crt));
		cert = PEM_read_bio_X509(bio, NULL, NULL, NULL); //PEM格式
		SSL_CTX_use_certificate(ssl_ctx->ctx, cert);
		X509_free(cert);
		BIO_free(bio);
		//SSL_CTX_use_certificate_file(ssl_ctx->ctx, "cacert.pem", SSL_FILETYPE_PEM);
	}
	if (pri_key) {
		bio = BIO_new_mem_buf((void*)pri_key, (int)strlen(pri_key));
		EVP_PKEY* evpkey = PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL);
		SSL_CTX_use_PrivateKey(ssl_ctx->ctx, evpkey);
		EVP_PKEY_free(evpkey);
		BIO_free(bio);
		//SSL_CTX_use_PrivateKey_file(ssl_ctx->ctx, "privkey.pem.unsecure", SSL_FILETYPE_PEM);
	}
	ssl_ctx->ssl = SSL_new(ssl_ctx->ctx);
	ssl_ctx->rbio = BIO_new(BIO_s_mem());
	ssl_ctx->wbio = BIO_new(BIO_s_mem());
	ssl_ctx->rbuf = (char*)malloc(DATA_BUFSIZE);
	ssl_ctx->wbuf = (char*)malloc(DATA_BUFSIZE);
	ssl_ctx->rsize = DATA_BUFSIZE;
	ssl_ctx->wsize = DATA_BUFSIZE;
	if (!ssl_ctx->ssl || !ssl_ctx->rbio || !ssl_ctx->wbio || !ssl_ctx->rbuf || !ssl_ctx->wbuf) {
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		if (ssl_ctx->rbio) BIO_free(ssl_ctx->rbio);//这个时候ssl还没有和bio绑定，这里要主动释放
		if (ssl_ctx->wbio) BIO_free(ssl_ctx->wbio);
		if (ssl_ctx->ssl) SSL_free(ssl_ctx->ssl);
		if (ssl_ctx->ctx) SSL_CTX_free(ssl_ctx->ctx);
		if (ssl_ctx->rbuf) free(ssl_ctx->rbuf);
		if (ssl_ctx->wbuf) free(ssl_ctx->wbuf);
		free(ssl_ctx);
		return false;
	}
	SSL_set_bio(ssl_ctx->ssl, ssl_ctx->rbio, ssl_ctx->wbio);
	openssl_type == SSL_CLIENT? SSL_set_connect_state(ssl_ctx->ssl): SSL_set_accept_state(ssl_ctx->ssl);
	IocpSock->sock_data = ssl_ctx;
	IocpSock->protocol = SSL_PROTOCOL;

	if (openssl_type == SSL_CLIENT) {
		SSL_do_handshake(ssl_ctx->ssl);
		ssl_do_write(ssl_ctx, IocpSock);
	}
	return true;
}

static int Hsocket_upto_SSL_Client(HSOCKET IocpSock) {
	if (!Hsocket_SSL_init(IocpSock, SSL_CLIENT, 0, NULL, NULL, NULL))
		return do_close(IocpSock, IocpSock->event_type, 0);
	return PostRecv(IocpSock);
}
#endif

static int do_connect(HSOCKET IocpSock) {
	//hsocket_set_keepalive(IocpSock->fd);
	setsockopt(IocpSock->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);  //连接成功后刷新套接字属性
	SetFileCompletionNotificationModes((HANDLE)IocpSock->fd, FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
#ifdef OPENSSL_SUPPORT
	if (IocpSock->protocol == SSL_PROTOCOL) {
		return Hsocket_upto_SSL_Client(IocpSock);
	}
#endif
	
	if (IocpSock->fd != INVALID_SOCKET) {
		BaseWorker* worker = IocpSock->worker;
		worker->ConnectionMade(IocpSock, IocpSock->protocol);
		return PostRecv(IocpSock);
	}
	return do_close(IocpSock, IocpSock->event_type, 0);
}

static int do_accepted(HSOCKET IocpSock){
	BaseWorker* worker = IocpSock->worker;
	worker->ref_count++;
	worker->ConnectionMade(IocpSock, IocpSock->protocol);
	return PostRecv(IocpSock);
}

static int do_rebind(HSOCKET IocpSock) {
	BaseWorker* worker = IocpSock->worker;
	ThreadStat* ts = ThreadDistribution(worker);
	CreateIoCompletionPort((HANDLE)IocpSock->fd, ts->CompletionPort, (ULONG_PTR)IocpSock, 0);
	SetFileCompletionNotificationModes((HANDLE)IocpSock->fd, FILE_SKIP_SET_EVENT_ON_HANDLE | FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
	worker->ref_count++;
	WorkerBind_Callback callback = IocpSock->bind_call;
	IocpSock->event_type = SOCKET_READ;
	callback(IocpSock, worker, IocpSock->call_data, 0);
	return PostRecv(IocpSock);
}

static void do_signal(HSIGNAL hsock) {
	Signal_Callback callback = hsock->call;
	callback(hsock->worker, hsock->signal);
	free(hsock);
}

static void do_event(HEVENT hsock) {
	Event_Callback callback = (Event_Callback)hsock->call;
	callback(hsock->worker, hsock->event_data);
	free(hsock);
}

static void do_timer(HTIMER hsock) {
	if (!hsock->closed) {
		Timer_Callback callback = (Timer_Callback)hsock->call;
		callback(hsock, hsock->worker, hsock->user_data);
		ATOMIC_UNLOCK(hsock->lock);
		if (!hsock->closed && hsock->once == 0) 
			return;
	}	
	DeleteTimerQueueTimer(NULL, hsock->timer, INVALID_HANDLE_VALUE);
	free(hsock);
}

static void ProcessIO(HSOCKET IocpSock, char sock_io_type, DWORD dwIoSize){
start:
	switch (sock_io_type){
	case SOCKET_READ:
		IocpSock->offset += dwIoSize;
		dwIoSize = do_read(IocpSock);
		break;
	case SOCKET_WRITE:
		dwIoSize = do_close(IocpSock, sock_io_type, 0);
		break;
	case SOCKET_ACCEPT:
		dwIoSize = do_accept(IocpSock);
		break;
	case SOCKET_ACCEPTED:
		dwIoSize = do_accepted(IocpSock);
		break;
	case SOCKET_CONNECT:
		dwIoSize = do_connect(IocpSock);
		break;
	case SOCKET_REBIND:
		dwIoSize = do_rebind(IocpSock);
		break;
	default:
		break;
	}
	if (dwIoSize > 0) {
		sock_io_type = IocpSock->event_type;
		goto start;
	}
}

/////////////////////////////////////////////////////////////////////////
//服务线程
DWORD WINAPI serverWorkerThread(HANDLE	CompletionPort){
	DWORD	dwIoSize = 0;
	void* CompletKey = NULL;
	void* OverLapped = NULL;		//IO数据,用于发起接收重叠操作
	bool bRet = false;
	char sock_io_type = 0;
	DWORD err = 0;
	
	LPOVERLAPPED_ENTRY entry_array, entry;
	unsigned long i, ret = 0;
	entry_array = (LPOVERLAPPED_ENTRY)malloc(sizeof(OVERLAPPED_ENTRY) * 1024);
	if (!entry_array) return -1;
	while (1) {
		bRet = GetQueuedCompletionStatusEx(CompletionPort, entry_array, 1024, &ret, INFINITE, false);
		if (bRet) {
			for (i = 0; i < ret; i++) {
				entry = entry_array + i;
				CompletKey = (void*)entry->lpCompletionKey;
				OverLapped = entry->lpOverlapped;
				dwIoSize = entry->dwNumberOfBytesTransferred;
				if (!OverLapped) {  //Overlapped为NULL，说明当前消息不是套接字IO完成消息，而是timer、event、signal
					switch (*(PROTOCOL*)CompletKey)
					{
					case TIMER:
						do_timer((HTIMER)CompletKey);
						continue;
					case EVENT:
						do_event((HEVENT)CompletKey);
						continue;
					case SIGNAL:
						do_signal((HSIGNAL)CompletKey);
						continue;
					default:
						continue;
					}
				}
				sock_io_type = ((HSENDBUFF)OverLapped)->event_type;
				if (SOCKET_CLOSE == sock_io_type || (0 == dwIoSize && (SOCKET_READ == sock_io_type || SOCKET_WRITE == sock_io_type))) {
					err = WSAGetLastError();   //GetOverlappedResult(hsock->fd, OverLapped, dwIoSize, );
					do_close((HSOCKET)OverLapped, sock_io_type, err);
				}
				else {
					ProcessIO((HSOCKET)OverLapped, sock_io_type, dwIoSize);
				}
			}
		}
	}

	//while (true){
	//	bRet = GetQueuedCompletionStatus(CompletionPort, &dwIoSize, (PULONG_PTR)&CompletKey, (LPOVERLAPPED*)&OverLapped, INFINITE);
	//	if (!OverLapped) {  //Overlapped为NULL，说明当前消息不是套接字IO完成消息，而是timer、event、signal
	//		switch (*(PROTOCOL*)CompletKey)
	//		{
	//		case TIMER:
	//			do_timer((HTIMER)CompletKey);
	//			continue;
	//		case EVENT:
	//			do_event((HEVENT)CompletKey);
	//			continue;
	//		case SIGNAL:
	//			do_signal((HSIGNAL)CompletKey);
	//			continue;
	//		default:
	//			continue;
	//		}
	//	}
	//	sock_io_type = ((HSENDBUFF)OverLapped)->event_type;
	//	if (bRet == false){
	//		err = WSAGetLastError();  //64L,121L,995L
	//		if (WAIT_TIMEOUT == err || ERROR_IO_PENDING == err) continue;
	//		do_close((HSOCKET)OverLapped, sock_io_type, err);
	//	}
	//	else if (0 == dwIoSize && (SOCKET_READ == sock_io_type || SOCKET_WRITE == sock_io_type)){
	//		err = WSAGetLastError();
	//		do_close((HSOCKET)OverLapped, sock_io_type, err);
	//	}
	//	else{
	//		ProcessIO((HSOCKET)OverLapped, sock_io_type, dwIoSize);
	//	}
	//}
	return 0;
}

static void timer_queue_callback(HTIMER hsock, BOOLEAN TimerOrWaitFired) {
	if (ATOMIC_TRYLOCK(hsock->lock)) {
		PostQueuedCompletionStatus(hsock->completion_port, 0, (ULONG_PTR)hsock, NULL);
	}
}

static int runIOCPServer(){
	HANDLE ThreadHandle = NULL;
	ThreadStat* ts;
	for (int i = 0; i <= ActorThreadWorker; i++){
		ts = THREAD_STATES_AT(i);
		ThreadHandle = CreateThread(NULL, 0, (LPTHREAD_START_ROUTINE)serverWorkerThread, ts->CompletionPort, 0, NULL);
		if (NULL == ThreadHandle) {
			return -4;
		}
		CloseHandle(ThreadHandle);
	}
	return 0;
}

int __STDCALL ActorStart(int thread_count){
	WSADATA wsData;
	if (0 != WSAStartup(0x0202, &wsData)){
		return SOCKET_ERROR;
	}

	if (thread_count > 0) {
		ActorThreadWorker = thread_count;
	}
	else {
		SYSTEM_INFO sysInfor;
		GetSystemInfo(&sysInfor);
		ActorThreadWorker = sysInfor.dwNumberOfProcessors;
	}
	
	ThreadStats = (ThreadStat*)malloc((ActorThreadWorker+1) * sizeof(ThreadStat));
	if (!ThreadStats) return -2;

	ThreadStat* ts = NULL;	
	for (int i = 0; i <= ActorThreadWorker; i++) {
		ts = THREAD_STATES_AT(i);
		ts->CompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
		ts->WorkerCount = 0;
	}
	ListenCompletionPort = ts->CompletionPort;

	HMODULE ntmodule = GetModuleHandleA("ntdll.dll");
	if (!ntmodule) {
		printf("%s:%d error\n", __func__, __LINE__);
		return -3;
	}
	ReplaceIoCompletionPortEx = (LPFN_NtSetInformationFile)GetProcAddress(ntmodule, "NtSetInformationFile");
	if (!ReplaceIoCompletionPortEx) {
		printf("%s:%d error\n", __func__, __LINE__);
		return -4;
	}

	SOCKET tempSock = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	//使用WSAIoctl获取AcceptEx和ConnectEx函数指针
	DWORD dwbytes = 0;
	GUID guidAcceptEx = WSAID_ACCEPTEX;
	if (0 != WSAIoctl(tempSock, SIO_GET_EXTENSION_FUNCTION_POINTER, &guidAcceptEx, sizeof(guidAcceptEx),
		&lpfnAcceptEx, sizeof(lpfnAcceptEx), &dwbytes, NULL, NULL)){
		return -3;
	}
	GUID GuidConnectEx = WSAID_CONNECTEX;
	if (SOCKET_ERROR == WSAIoctl(tempSock, SIO_GET_EXTENSION_FUNCTION_POINTER,&GuidConnectEx, sizeof(GuidConnectEx),
		&lpfnConnectEx, sizeof(lpfnConnectEx), &dwbytes, NULL, NULL)) {
		return -4;
	}
	closesocket(tempSock);

#ifdef OPENSSL_SUPPORT
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
#endif
	return runIOCPServer();
}

int __STDCALL AccepterRun(BaseAccepter* accepter, const char* ip, int listen_port){
	if (listen_port != 0){
		accepter->Listening = true;
		accepter->Listenfd = get_listen_sock(ip, listen_port);
		if (accepter->Listenfd == SOCKET_ERROR) {
			accepter->Listening = false;
			return -2;
		}
		CreateIoCompletionPort((HANDLE)accepter->Listenfd, ListenCompletionPort, (ULONG_PTR)accepter->Listenfd, 0);
		if (PostAcceptClient(accepter)) {
			accepter->Listening = false;
			closesocket(accepter->Listenfd);
			return -3;
		}
	}
	return 0;
}

int __STDCALL AccepterStop(BaseAccepter* accepter){
	if (accepter->Listenfd) {
		closesocket(accepter->Listenfd);
		accepter->Listenfd = NULL;
	}
	else {
		accepter->Listening = false;
	}
	while (accepter->Listening){
		Sleep(0);
	}
	return 0;
}

HTIMER	__STDCALL TimerCreate(BaseWorker* worker, void* user_data, int duetime, int looptime, Timer_Callback callback) {
	HTIMER hsock = (HTIMER)malloc(sizeof(Timer_Content));
	if (hsock) {
		hsock->protocol = TIMER;
		hsock->once = looptime == 0 ? 1 : 0;
		hsock->worker = worker;
		hsock->call = callback;
		ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
		hsock->completion_port = ts ? ts->CompletionPort : ListenCompletionPort;
		hsock->closed = 0;
		hsock->lock = 0;
		hsock->user_data = user_data;
		CreateTimerQueueTimer(&hsock->timer, NULL, (WAITORTIMERCALLBACK)timer_queue_callback, hsock, duetime, looptime, 0);
	}
	return hsock;
}

void __STDCALL TimerDelete(HTIMER hsock) {
	hsock->closed = 1;
}

void __STDCALL PostEvent(BaseWorker* worker, void* event_data, Event_Callback callback) {
	HEVENT hsock = (HEVENT)malloc(sizeof(Event_Content));
	if (hsock) {
		hsock->protocol = EVENT;
		hsock->worker = worker;
		hsock->call = callback;
		hsock->event_data = event_data;
		ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
		PostQueuedCompletionStatus(ts ? ts->CompletionPort : ListenCompletionPort, 0, (ULONG_PTR)hsock, NULL);
	}
}

void __STDCALL PostSignal(BaseWorker* worker, long long signal, Signal_Callback callback) {
	HSIGNAL hsock = (HSIGNAL)malloc(sizeof(Signal_Content));
	if (hsock) {
		hsock->protocol = SIGNAL;
		hsock->worker = worker;
		hsock->call = callback;
		hsock->signal = signal;
		ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
		PostQueuedCompletionStatus(ts ? ts->CompletionPort : ListenCompletionPort, 0, (ULONG_PTR)hsock, NULL);
	}
}

static bool IOCPConnectUDP(BaseWorker* worker, HSOCKET IocpSock, int listen_port)
{
	SOCKET fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (fd == INVALID_SOCKET) return false;
	IocpSock->fd = fd;

	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	local_addr.sin6_port = ntohs(listen_port);
	local_addr.sin6_addr = in6addr_any;
	socket_set_v6only(fd, 0);

	u_long nonblock = 1;
	ioctlsocket(fd, FIONBIO, &nonblock);
	set_linger_for_fd(fd);
	if (bind(fd, (struct sockaddr*)(&local_addr), sizeof(local_addr))) {
		return false;
	}

	ThreadStat* ts = ThreadDistribution(worker);
	CreateIoCompletionPort((HANDLE)fd, ts->CompletionPort, (ULONG_PTR)IocpSock, 0);

	if (ResetIocp_Buff(IocpSock) == false){
		return false;
	}
	IocpSock->event_type = SOCKET_READ;
	if (SOCKET_ERROR == WSARecvFrom(fd, &IocpSock->databuf, 1, NULL, &WSARECV_FLAG,
		(struct sockaddr*)&IocpSock->peer_addr, &sockaddr_len, (LPOVERLAPPED)IocpSock, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError()){
			return false;
		}
	}
	return true;
}

HSOCKET __STDCALL HsocketListenUDP(BaseWorker* worker, const char* ip, int port){
	if (worker == NULL) return NULL;
	HSOCKET IocpSock = NewIOCP_Socket();
	if (IocpSock == NULL) return NULL; 

	char* recv_buf = (char*)malloc(DATA_BUFSIZE);
	if (recv_buf == NULL) {
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	IocpSock->recv_buf = recv_buf;
	IocpSock->size = DATA_BUFSIZE;
	IocpSock->event_type = SOCKET_CONNECT;
	IocpSock->protocol = UDP_PROTOCOL;
	IocpSock->worker = worker;
	IocpSock->peer_addr.sin6_family = AF_INET6;
	IocpSock->peer_addr.sin6_port = htons(0);
	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &IocpSock->peer_addr.sin6_addr);

	bool ret = false;
	ret = IOCPConnectUDP(worker, IocpSock, port);   //UDP连接
	if (ret == false){
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	worker->ref_count++;
	return 0;
}

static bool IOCPConnectTCP(BaseWorker* worker, HSOCKET IocpSock){
	SOCKET fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (fd == INVALID_SOCKET) return false;
	IocpSock->fd = fd;

	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	socket_set_v6only(fd, 0);

	u_long nonblock = 1;
	ioctlsocket(fd, FIONBIO, &nonblock);
	set_linger_for_fd(fd);
	if (bind(fd, (struct sockaddr*)(&local_addr), sizeof(local_addr))) {
		return false;
	}
	ThreadStat* ts = ThreadDistribution(worker);
	CreateIoCompletionPort((HANDLE)fd, ts->CompletionPort, (ULONG_PTR)IocpSock, 0);

	BOOL bResult = lpfnConnectEx(fd, (struct sockaddr*)&IocpSock->peer_addr, sizeof(IocpSock->peer_addr), NULL, 0, NULL, (LPOVERLAPPED)IocpSock);
	if (!bResult){
		if (WSAGetLastError() != ERROR_IO_PENDING){
			return false;
		}
	}
	return true;
}

HSOCKET __STDCALL HsocketConnect(BaseWorker* worker, const char* ip, int port, PROTOCOL protocol){
	if (worker == NULL) return NULL;
	HSOCKET IocpSock = NewIOCP_Socket();
	if (IocpSock == NULL) return NULL;

	char* recv_buf = (char*)malloc(DATA_BUFSIZE);
	if (recv_buf == NULL) {
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	IocpSock->recv_buf = recv_buf;
	IocpSock->size = DATA_BUFSIZE;
	IocpSock->event_type = SOCKET_CONNECT;
	IocpSock->protocol = protocol;
	IocpSock->worker = worker;
	IocpSock->peer_addr.sin6_family = AF_INET6;
	IocpSock->peer_addr.sin6_port = htons(port);

	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &IocpSock->peer_addr.sin6_addr);

	bool ret = false;
	if (protocol == TCP_PROTOCOL
#ifdef OPENSSL_SUPPORT
		|| protocol == SSL_PROTOCOL
#endif
		)
		ret = IOCPConnectTCP(worker, IocpSock);   //TCP连接
	else if (protocol == UDP_PROTOCOL
#ifdef KCP_SUPPORT
		|| protocol == KCP_PROTOCOL
#endif
		)
		ret = IOCPConnectUDP(worker, IocpSock, 0);   //UDP连接
	else {
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	if (ret == false){
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	worker->ref_count++;
	return IocpSock;
}

static bool IOCPPostSendUDPEx(HSOCKET IocpSock, HSENDBUFF IocpBuff, struct sockaddr* addr, int addrlen){
	if (SOCKET_ERROR == WSASendTo(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, addr, addrlen, (LPOVERLAPPED)IocpBuff, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError())
			return false;
	}
	return true;
}

static bool IOCPPostSendTCPEx(HSOCKET IocpSock, HSENDBUFF IocpBuff){
	int send_len = 0;
	int ret = WSASend(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, (LPOVERLAPPED)IocpBuff, NULL);
	if (SOCKET_ERROR != ret) {
		if (IocpBuff->databuf.buf != NULL) free(IocpBuff->databuf.buf);
		free(IocpBuff);
	}
	else if (ERROR_IO_PENDING != WSAGetLastError()){
		return false;
	}
	return true;
}

static bool HsocketSendEx(HSOCKET IocpSock, const char* data, int len){
	HSENDBUFF IocpBuff = (HSENDBUFF)malloc(sizeof(Socket_Send_Content));
	if (!IocpBuff) {
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		return false;
	}
	memset(IocpBuff, 0x0, sizeof(Socket_Send_Content));
	if (IocpBuff == NULL) return false;
	IocpBuff->event_type = SOCKET_WRITE;
	char* buf = (char*)malloc(len);
	if (buf == NULL){
		printf("%s:%d memory malloc error\n", __func__, __LINE__);
		free(IocpBuff);
		return false;
	}
	IocpBuff->databuf.buf = buf;
	memcpy(buf, data, len);
	IocpBuff->databuf.len = len;

	bool ret = false;
	PROTOCOL protocol = IocpSock->protocol;
	if (protocol == TCP_PROTOCOL
#ifdef OPENSSL_SUPPORT
		|| protocol == SSL_PROTOCOL
#endif
		)
		ret = IOCPPostSendTCPEx(IocpSock, IocpBuff);
	else if (protocol == UDP_PROTOCOL 
#ifdef KCP_SUPPORT
		|| protocol == KCP_PROTOCOL
#endif
		)
		ret = IOCPPostSendUDPEx(IocpSock, IocpBuff, (struct sockaddr*)&IocpSock->peer_addr, sizeof(IocpSock->peer_addr));
	if (ret == false){
		free(buf);
		free(IocpBuff);
		return false;
	}
	return true;
}

#ifdef OPENSSL_SUPPORT
static bool HsocketSendSSL(HSOCKET hsock, const char* data, int len) {
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->sock_data;
	ATOMIC_LOCK(ssl_ctx->wlock);
	int ret = SSL_write(ssl_ctx->ssl, data, len);
	if (ret > 0) {
		ssl_do_write(ssl_ctx, hsock);
		ATOMIC_UNLOCK(ssl_ctx->wlock);
		return true;
	}
	ATOMIC_UNLOCK(ssl_ctx->wlock);
	printf("%s:%d ret:%d ssl_errono:%d len:%d\n", __func__, __LINE__, ret, SSL_get_error(ssl_ctx->ssl, ret), len);
	return false;
}
#endif

#ifdef KCP_SUPPORT
static bool HsocketSendKcp(HSOCKET hsock, const char* data, int len) {
	Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
	ikcp_send(ctx->kcp, data, len);
	return true;
}
#endif

bool __STDCALL HsocketSend(HSOCKET hsock, const char* data, int len) {
	if (hsock) {
		switch (hsock->protocol){
		case TCP_PROTOCOL:
		case UDP_PROTOCOL:
			return HsocketSendEx(hsock, data, len);
#ifdef OPENSSL_SUPPORT
		case SSL_PROTOCOL:
			return HsocketSendSSL(hsock, data, len);
#endif
#ifdef KCP_SUPPORT
		case KCP_PROTOCOL:
			return HsocketSendKcp(hsock, data, len);
#endif
		default:
			break;
		}
	}
	return false;
}

bool __STDCALL HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len) {
	if (hsock->protocol == UDP_PROTOCOL){
		HSENDBUFF IocpBuff = (HSENDBUFF)malloc(sizeof(Socket_Send_Content));
		if (IocpBuff == NULL) {
			printf("%s:%d memory malloc error\n", __func__, __LINE__);
			return false;
		}
		memset(IocpBuff, 0x0, sizeof(Socket_Send_Content));

		IocpBuff->databuf.buf = (char*)malloc(len);
		if (IocpBuff->databuf.buf == NULL) {
			printf("%s:%d memory malloc error\n", __func__, __LINE__);
			free(IocpBuff);
			return false;
		}
		memcpy(IocpBuff->databuf.buf, data, len);
		IocpBuff->databuf.len = len;
		memset((LPOVERLAPPED)IocpBuff, 0, sizeof(OVERLAPPED));
		IocpBuff->event_type = SOCKET_WRITE;
		
		struct sockaddr_in6 toaddr = { 0x0 };
		toaddr.sin6_family = AF_INET6;
		toaddr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, dst, &toaddr.sin6_addr);

		bool ret = IOCPPostSendUDPEx(hsock, IocpBuff, (struct sockaddr*)&toaddr, sizeof(toaddr));
		if (ret == false) {
			free(IocpBuff->databuf.buf);
			free(IocpBuff);
			return false;
		}
		return true;
	}
	return false;
}

void __STDCALL HsocketClose(HSOCKET hsock){
	if (!hsock || hsock->fd == INVALID_SOCKET || hsock->fd == NULL) return;
	closesocket(hsock->fd);
	hsock->fd = NULL;
	hsock->event_type = SOCKET_CLOSE;
	return;
}

void __STDCALL HsocketClosed(HSOCKET hsock) {
	if (!hsock || hsock->fd == INVALID_SOCKET || hsock->fd == NULL) return;
	closesocket(hsock->fd);
	hsock->fd = INVALID_SOCKET;
	hsock->worker->ref_count--;
	hsock->event_type = SOCKET_CLOSE;
}

int __STDCALL HsocketPopBuf(HSOCKET hsock, int len)
{
	switch (hsock->protocol) {
	case TCP_PROTOCOL:
	case UDP_PROTOCOL:{
		hsock->offset -= len;
		memmove(hsock->recv_buf, hsock->recv_buf + len, hsock->offset);
		return hsock->offset;
	}
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL:{
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->sock_data;
		ssl_ctx->roffset -= len;
		memmove(ssl_ctx->rbuf, ssl_ctx->rbuf + len, ssl_ctx->roffset);
		return ssl_ctx->roffset;
	}
#endif
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL:{
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ctx->offset -= len;
		memmove(ctx->buf, ctx->buf + len, ctx->offset);
		return ctx->offset;
	}
#endif
	default:
		break;
	}
	return 0;
}

void __STDCALL HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port) {
	PROTOCOL protocol = hsock->protocol;
	if (protocol == UDP_PROTOCOL 
#ifdef KCP_SUPPORT
		|| protocol == KCP_PROTOCOL
#endif
		) {
		hsock->peer_addr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, dst, &hsock->peer_addr.sin6_addr);
	}
}

void __STDCALL HsocketPeerAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port) {
	if (ip) {
		inet_ntop(AF_INET6, &hsock->peer_addr.sin6_addr, ip, ipsz);
		if (strncmp(ip, "::ffff:", 7) == 0) {
			memmove(ip, ip + 7, ipsz - 7);
		}
	}
	if (port) *port = ntohs(hsock->peer_addr.sin6_port);
}

void __STDCALL HsocketLocalAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port) {
	struct sockaddr_in6 local = { 0x0 };
	int len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	if (ip) {
		inet_ntop(AF_INET6, &local.sin6_addr, ip, ipsz);
		if (strncmp(ip, "::ffff:", 7) == 0) {
			memmove(ip, ip + 7, ipsz - 7);
		}
	}
	if (port) *port = ntohs(local.sin6_port);
}

void __STDCALL HsocketUnbindWorker(HSOCKET hsock, void* user_data, WorkerBind_Callback ucall) {
	hsock->bind_call = ucall;
	hsock->call_data = user_data;
	hsock->event_type = SOCKET_UNBIND;
}

void __STDCALL HsocketRebindWorker(HSOCKET hsock, BaseWorker* worker, void* user_data, WorkerBind_Callback call) {
	hsock->rebind_worker = worker;
	hsock->bind_call = call;
	hsock->call_data = user_data;
	hsock->event_type = SOCKET_REBIND;

	if (hsock->worker) return;

	hsock->worker = worker;
	ThreadStat* ts = ThreadDistribution(worker);
	HANDLE CompletionPort = ts->CompletionPort;
	PostQueuedCompletionStatus(CompletionPort, 0, (ULONG_PTR)hsock, (LPOVERLAPPED)hsock);
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

#ifdef OPENSSL_SUPPORT
bool __STDCALL HsocketSSLCreate(HSOCKET hsock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key) {
	bool ret = false;
	if (hsock->protocol == TCP_PROTOCOL) {
		ret = Hsocket_SSL_init(hsock, openssl_type, verify, ca_crt, user_crt, pri_key);
	}
	return ret;
}
#endif

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* hsock){
	HsocketSendEx((HSOCKET)hsock, buf, len);
	return 0;
}

int __STDCALL HsocketKcpCreate(HSOCKET hsock, int conv, int mode){
	if (hsock->protocol == UDP_PROTOCOL) {
		ikcpcb* kcp = ikcp_create(conv, hsock);
		if (!kcp) return -1;
		kcp->output = kcp_send_callback;
		kcp->stream = mode;
		Kcp_Content* ctx = (Kcp_Content*)malloc(sizeof(Kcp_Content));
		if (!ctx) { ikcp_release(kcp); return -1; }
		ctx->kcp = kcp;
		ctx->buf = (char*)malloc(DATA_BUFSIZE);
		if (!ctx->buf) { printf("%s:%d memory malloc error\n", __func__, __LINE__); ikcp_release(kcp); free(ctx); return -1; }
		ctx->size = DATA_BUFSIZE;
		ctx->offset = 0;
		hsock->sock_data = ctx;
		hsock->protocol = KCP_PROTOCOL;
	}
	return 0;
}

void __STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc){
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);
	}
}

void __STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd){
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
	}
}

int __STDCALL HsocketKcpGetconv(HSOCKET hsock){
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		return ikcp_getconv(ctx->kcp);
	}
	return 0;
}

void __STDCALL HsocketKcpUpdate(HSOCKET hsock){
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ikcp_update(ctx->kcp, iclock());
	}
}

int __STDCALL HsocketKcpDebug(HSOCKET hsock, char* buf, int size) {
	int n = 0;
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ikcpcb* kcp = ctx->kcp;
		n = snprintf(buf, size, "nsnd_buf[%d] nsnd_que[%d] nrev_buf[%d] nrev_que[%d] snd_wnd[%d] rev_wnd[%d] rmt_wnd[%d] cwd[%d]",
			kcp->nsnd_buf, kcp->nsnd_que, kcp->nrcv_buf, kcp->nrcv_que, kcp->snd_wnd, kcp->rcv_wnd, kcp->rmt_wnd, kcp->cwnd);
	}
	return n;
}
#endif
