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
#include <Mstcpip.h>
#include <time.h>
#include <map>

#pragma comment(lib, "Ws2_32.lib")
#ifdef OPENSSL_SUPPORT
#pragma comment(lib, "libcrypto.lib")
#pragma comment(lib, "libssl.lib")
#endif

#define DATA_BUFSIZE 8192
#define READ	0
#define WRITE	1
#define ACCEPT	2
#define CONNECT 3

HANDLE CompletionPort = NULL;
DWORD  CompletionPortWorker = 0;
std::map<uint16_t, BaseFactory*> Factorys;

static LPFN_ACCEPTEX lpfnAcceptEx = NULL;
static LPFN_CONNECTEX lpfnConnectEx = NULL;
static DWORD WSA_RECV_FLAGS = 0;

typedef struct {
	HANDLE timer;
	Timer_Callback call;
	long lock;
	long close;
}IOCP_TIMER, * HTIMER;

static bool HsocketSendEx(SOCKET_CTX* IocpSock, const char* data, int len);

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
};
#endif

#ifdef KCP_SUPPORT
#include "ikcp.h"
#include "time.h"

struct Kcp_Content{
	ikcpcb* kcp;
	char*	buf;
	long	lock;
	int		size;
	int		offset;
	char	enable;
};

static inline void itimeofday(long* sec, long* usec){
	static long mode = 0, addsec = 0;
	BOOL retval;
	static IINT64 freq = 1;
	IINT64 qpc;
	if (mode == 0) {
		retval = QueryPerformanceFrequency((LARGE_INTEGER*)&freq);
		freq = (freq == 0) ? 1 : freq;
		retval = QueryPerformanceCounter((LARGE_INTEGER*)&qpc);
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

static inline SOCKET_CTX* New_Socket_Ctx(){
	HSOCKET hsock = (HSOCKET)malloc(sizeof(SOCKET_CTX));
	if (hsock) {memset(hsock, 0x0, sizeof(SOCKET_CTX));}
	return hsock;
}

static inline void Release_Socket_Ctx(SOCKET_CTX* IocpSock){
#ifdef OPENSSL_SUPPORT
	if (IocpSock->_conn_type == SSL_CONN){
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)IocpSock->_user_data;
		//BIO_free(ssl_ctx->rbio);  //貌似bio会随着SSL_free一起释放，先行释放会引起崩溃，待后续确认
		//BIO_free(ssl_ctx->wbio);
		if (ssl_ctx->wbuf) free(ssl_ctx->wbuf);
		if (ssl_ctx->rbuf) free(ssl_ctx->rbuf);
		if (ssl_ctx->ssl) { SSL_shutdown(ssl_ctx->ssl); SSL_free(ssl_ctx->ssl); }
		if (ssl_ctx->ctx) SSL_CTX_free(ssl_ctx->ctx);
		free(ssl_ctx);
	}
#endif
#ifdef KCP_SUPPORT
	if (IocpSock->_conn_type == KCP_CONN)
	{
		Kcp_Content* ctx = (Kcp_Content*)IocpSock->_user_data;
		ikcp_release(ctx->kcp);
		free(ctx->buf);
		free(ctx);
	}
#endif
	free(IocpSock);
}

static inline BUFF_CTX* New_Buff_Ctx(){
	BUFF_CTX* buff = (BUFF_CTX*)malloc(sizeof(BUFF_CTX));
	if (buff) { memset(buff, 0x0, sizeof(BUFF_CTX)); }
	return buff;
}

static inline void Release_Buff_Ctx(BUFF_CTX* buff){
	free(buff);
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

static inline SOCKET get_listen_sock(const char* ip, int port){
	SOCKET listenSock = WSASocket(AF_INET6, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	struct sockaddr_in6 server_addr = {0x0};
	server_addr.sin6_family = AF_INET6;
	server_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &server_addr.sin6_addr);
	//server_addr.sin6_addr = in6addr_any;
	socket_set_v6only(listenSock, 0);

	int ret = bind(listenSock, (struct sockaddr*)&server_addr, sizeof(server_addr));
	if (ret != 0){
		return SOCKET_ERROR;
	}
	listen(listenSock, 5);
	if (listenSock == SOCKET_ERROR){
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

static inline void PostAcceptClient(BaseFactory* factroy){
	SOCKET_CTX* IocpSock = New_Socket_Ctx();
	if (!IocpSock) return;
	IocpSock->_user = factroy->CreateProtocol();
	if (!IocpSock->_user) { Release_Socket_Ctx(IocpSock); return; }
	IocpSock->factory = factroy;

	BUFF_CTX* IocpBuff;
	IocpBuff = New_Buff_Ctx();
	if (IocpBuff == NULL){
		factroy->DeleteProtocol(IocpSock->_user);
		Release_Socket_Ctx(IocpSock);
		return;
	}
	IocpBuff->databuf.buf = (char*)malloc(DATA_BUFSIZE);
	if (IocpBuff->databuf.buf == NULL){
		factroy->DeleteProtocol(IocpSock->_user);
		Release_Buff_Ctx(IocpBuff);
		Release_Socket_Ctx(IocpSock);
		return;
	}
	IocpBuff->databuf.len = DATA_BUFSIZE;
	IocpBuff->size = DATA_BUFSIZE;
	IocpBuff->type = ACCEPT;
	IocpBuff->hsock = IocpSock;
	IocpSock->recv_buf = IocpBuff->databuf.buf;
	IocpSock->_IocpBuff = IocpBuff;
	IocpSock->fd = WSASocket(AF_INET6, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (IocpSock->fd == INVALID_SOCKET){
		factroy->DeleteProtocol(IocpSock->_user);
		free(IocpSock->recv_buf);
		Release_Buff_Ctx(IocpBuff);
		Release_Socket_Ctx(IocpSock);
		return;
	}

	/*调用AcceptEx函数，地址长度需要在原有的上面加上16个字节向服务线程投递一个接收连接的的请求*/
	bool rc = lpfnAcceptEx(factroy->Listenfd, IocpSock->fd,
		IocpBuff->databuf.buf, 0,
		sizeof(struct sockaddr_in6) + 16, sizeof(struct sockaddr_in6) + 16,
		&IocpBuff->databuf.len, &(IocpBuff->overlapped));

	if (false == rc){
		if (WSAGetLastError() != ERROR_IO_PENDING){
			Release_Buff_Ctx(IocpBuff);
			return;
		}
	}
	return;
}

static inline void CloseSocket(SOCKET_CTX* IocpSock){
	SOCKET fd = InterlockedExchange(&IocpSock->fd, INVALID_SOCKET);
	if (fd != INVALID_SOCKET && fd != NULL){
		//CancelIo((HANDLE)fd);	//取消等待执行的异步操作
		closesocket(fd);
	}
}

static inline void AutoProtocolFree(BaseProtocol* proto) {
	AutoProtocol* autoproto = (AutoProtocol*)proto;
	autofree func = autoproto->freefunc;
	func(autoproto);
}

static void do_close(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff, int err){
	switch (IocpBuff->type){
	case ACCEPT:
		if (IocpBuff->databuf.buf)
			free(IocpBuff->databuf.buf);
		Release_Buff_Ctx(IocpBuff);
		PostAcceptClient(IocpSock->factory);
		return;
	case WRITE:
		if (IocpBuff->databuf.buf != NULL)
			free(IocpBuff->databuf.buf);
		Release_Buff_Ctx(IocpBuff);
		return;
	default:
		break;
	}
	BaseProtocol* proto = IocpSock->_user;
	int left_count = 99;
	if (IocpSock->fd != INVALID_SOCKET){
		proto->Lock();
		if (IocpSock->fd != INVALID_SOCKET){
			left_count = InterlockedDecrement(&proto->sockCount);
			if (READ == IocpBuff->type)
				proto->ConnectionClosed(IocpSock, err);
			else
				proto->ConnectionFailed(IocpSock, err);
		}
		proto->UnLock();
	}
	
	if (IocpSock->fd != INVALID_SOCKET && left_count == 0 && proto != NULL) {
		switch (proto->protoType){
		case SERVER_PROTOCOL:
			IocpSock->factory->DeleteProtocol(proto);
			break;
		case AUTO_PROTOCOL:
			AutoProtocolFree(proto);
			break;
		default:
			break;
		}
	}
	CloseSocket(IocpSock);
	if (IocpSock->recv_buf) free(IocpSock->recv_buf);
	Release_Buff_Ctx(IocpBuff);
	Release_Socket_Ctx(IocpSock);
}

static bool ResetIocp_Buff(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	memset(&IocpBuff->overlapped, 0, sizeof(OVERLAPPED));
	if (IocpSock->recv_buf == NULL){
		if (IocpBuff->databuf.buf != NULL){
			IocpSock->recv_buf = IocpBuff->databuf.buf;
		}
		else{
			IocpSock->recv_buf = (char*)malloc(DATA_BUFSIZE);
			if (IocpSock->recv_buf == NULL) return false;
			IocpBuff->size = DATA_BUFSIZE;
		}
	}
	IocpBuff->databuf.len = IocpBuff->size - IocpBuff->offset;
	if (IocpBuff->databuf.len == 0){
		IocpBuff->size += DATA_BUFSIZE;
		char* new_ptr = (char*)realloc(IocpSock->recv_buf, IocpBuff->size);
		if (new_ptr == NULL) return false;
		IocpSock->recv_buf = new_ptr;
		IocpBuff->databuf.len = IocpBuff->size - IocpBuff->offset;
	}
	IocpBuff->databuf.buf = IocpSock->recv_buf + IocpBuff->offset;
	return true;
}

static inline void PostRecvUDP(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	int fromlen = sizeof(IocpSock->peer_addr);
	if (SOCKET_ERROR == WSARecvFrom(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &WSA_RECV_FLAGS,
		(struct sockaddr*)&IocpSock->peer_addr, &fromlen, &IocpBuff->overlapped, NULL)){
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			do_close(IocpSock, IocpBuff, err);
		}
	}
}

static inline void PostRecvTCP(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	if (SOCKET_ERROR == WSARecv(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &WSA_RECV_FLAGS, &IocpBuff->overlapped, NULL)){
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			do_close(IocpSock, IocpBuff, err);
		}
	}
}

static void PostRecv(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	IocpBuff->type = READ;
	if (ResetIocp_Buff(IocpSock, IocpBuff) == false){
		return do_close(IocpSock, IocpBuff, 14);
	}
	if (IocpSock->_conn_type == TCP_CONN || IocpSock->_conn_type == SSL_CONN)
		return PostRecvTCP(IocpSock, IocpBuff);
	return PostRecvUDP(IocpSock, IocpBuff);
	
}

static void do_aceept(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	BaseFactory* factory = IocpSock->factory;
	PostAcceptClient(factory);

	//连接成功后刷新套接字属性
	setsockopt(IocpSock->fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*)&(factory->Listenfd), sizeof(factory->Listenfd));
	//hsocket_set_keepalive(IocpSock->fd);

	IocpSock->_user_data = NULL;
	BaseProtocol* proto = IocpSock->_user;
	if (!proto->factory) proto->SetFactory(factory, SERVER_PROTOCOL);

	int nSize = sizeof(IocpSock->peer_addr);
	getpeername(IocpSock->fd, (struct sockaddr*)&IocpSock->peer_addr, &nSize);

	InterlockedIncrement(&proto->sockCount);
	CreateIoCompletionPort((HANDLE)IocpSock->fd, CompletionPort, (ULONG_PTR)IocpSock, 0);	//将监听到的套接字关联到完成端口

	proto->Lock();
	proto->ConnectionMade(IocpSock, IocpSock->_conn_type);
	proto->UnLock();
	PostRecv(IocpSock, IocpBuff);
}

#ifdef KCP_SUPPORT
static void do_read_kcp(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	Kcp_Content* ctx = (Kcp_Content*)(IocpSock->_user_data);
	LONGLOCK(&ctx->lock);
	ikcp_input(ctx->kcp, IocpSock->recv_buf, IocpBuff->offset);
	IocpBuff->offset = 0;
	ikcp_update(ctx->kcp, iclock());
	//LONGUNLOCK(&ctx->lock);

	BaseProtocol* proto = IocpSock->_user;
	int n;
	while (1) {
		n = ikcp_recv(ctx->kcp, ctx->buf + ctx->offset, ctx->size - ctx->offset);
		if (n < 0) {
			if (n == -3) {
				int newsize = ctx->size * 2;
				char* newbuf = (char*)realloc(ctx->buf, newsize);
				if (newbuf) {
					ctx->buf = newbuf;
					ctx->size = newsize;
					continue;
				}
				LONGUNLOCK(&ctx->lock);
				return do_close(IocpSock, IocpBuff, -1);
			}
			break; 
		}
		ctx->offset += n;
		if (IocpSock->fd != INVALID_SOCKET){
			proto->Lock();
			if (IocpSock->fd != INVALID_SOCKET){
				proto->ConnectionRecved(IocpSock, ctx->buf, ctx->offset);
				proto->UnLock();
				continue;
			}
			proto->UnLock();
		}
		LONGUNLOCK(&ctx->lock);
		return do_close(IocpSock, IocpBuff, 0);
	}
	LONGUNLOCK(&ctx->lock);
	PostRecv(IocpSock, IocpBuff);
}
#endif

#ifdef OPENSSL_SUPPORT
static void ssl_do_write(struct SSL_Content* ssl_ctx, SOCKET_CTX* IocpSock) {
	while (1) {
		int left = ssl_ctx->wsize - ssl_ctx->woffset;
		if (left == 0) {
			int newsize = ssl_ctx->wsize * 2;
			char* newbuf = (char*)realloc(ssl_ctx->wbuf, newsize);
			if (newbuf) {
				ssl_ctx->wbuf = newbuf;
				ssl_ctx->wsize = newsize;
				left = ssl_ctx->wsize - ssl_ctx->woffset;
			}
		}
		int r_len = BIO_read(ssl_ctx->wbio, ssl_ctx->wbuf + ssl_ctx->woffset, left);
		if (r_len > 0)  ssl_ctx->woffset += r_len; else break;
	}
	HsocketSendEx(IocpSock, ssl_ctx->wbuf, ssl_ctx->woffset);
	ssl_ctx->woffset = 0;
}

static void ssl_do_handshake(struct SSL_Content* ssl_ctx, SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	BIO_write(ssl_ctx->rbio, IocpSock->recv_buf, IocpBuff->offset);
	IocpBuff->offset = 0;
	int r = SSL_do_handshake(ssl_ctx->ssl);
	ssl_do_write(ssl_ctx, IocpSock);
	
	BaseProtocol* proto = IocpSock->_user;
	if (r == 1) {
		if (IocpSock->fd != INVALID_SOCKET) {
			proto->Lock();
			if (IocpSock->fd != INVALID_SOCKET) {
				proto->ConnectionMade(IocpSock, IocpSock->_conn_type);
				proto->UnLock();
				PostRecv(IocpSock, IocpBuff);
				return;
			}
			proto->UnLock();
		}
		do_close(IocpSock, IocpBuff, 0);
		return;
	}
	else {
		int err_SSL_get_error = SSL_get_error(ssl_ctx->ssl, r);
		switch (err_SSL_get_error) {
		case SSL_ERROR_WANT_WRITE:
		case SSL_ERROR_WANT_READ:
			PostRecv(IocpSock, IocpBuff);
			return;
		default:
			do_close(IocpSock, IocpBuff, err_SSL_get_error);
			return;
		}
	}
}

static void do_read_ssl(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)IocpSock->_user_data;
	if (SSL_is_init_finished(ssl_ctx->ssl)) {
		BIO_write(ssl_ctx->rbio, IocpSock->recv_buf, IocpBuff->offset);
		IocpBuff->offset = 0;
		while (1) {
			char* buf = ssl_ctx->rbuf + ssl_ctx->roffset;
			int left = ssl_ctx->rsize - ssl_ctx->roffset;
			if (left == 0) {
				int new_size = ssl_ctx->rsize * 2;
				buf = (char*)realloc(ssl_ctx->rbuf, new_size);
				if (buf) {
					ssl_ctx->rbuf = buf;
					ssl_ctx->rsize = new_size;
					buf = buf + ssl_ctx->roffset;
					left = new_size - ssl_ctx->roffset;
				}
			}
			int rlen = SSL_read(ssl_ctx->ssl, ssl_ctx->rbuf + ssl_ctx->roffset, ssl_ctx->rsize - ssl_ctx->roffset);
			if (rlen > 0) {
				ssl_ctx->roffset += rlen;
				continue;
			}
			break;
		}
		BaseProtocol* proto = IocpSock->_user;
		if (ssl_ctx->roffset > 0) {
			if (IocpSock->fd != INVALID_SOCKET) {
				proto->Lock();
				if (IocpSock->fd != INVALID_SOCKET) {
					proto->ConnectionRecved(IocpSock, ssl_ctx->rbuf, ssl_ctx->roffset);
					proto->UnLock();
					PostRecv(IocpSock, IocpBuff);
					return;
				}
				proto->UnLock();
			}
			do_close(IocpSock, IocpBuff, 0);
		}
		else {
			PostRecv(IocpSock, IocpBuff);
		}
	}
	else {
		ssl_do_handshake(ssl_ctx, IocpSock, IocpBuff);
	}
}
#endif

static void do_read_tcp_and_udp(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	BaseProtocol* proto = IocpSock->_user;
	if (IocpSock->fd != INVALID_SOCKET) {
		proto->Lock();
		if (IocpSock->fd != INVALID_SOCKET) {
			proto->ConnectionRecved(IocpSock, IocpSock->recv_buf, IocpBuff->offset);
			proto->UnLock();
			PostRecv(IocpSock, IocpBuff);
			return;
		}
		proto->UnLock();
	}
	do_close(IocpSock, IocpBuff, 0);
}

static void do_read(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	switch (IocpSock->_conn_type) {
	case TCP_CONN:
	case UDP_CONN:
		return do_read_tcp_and_udp(IocpSock, IocpBuff);
#ifdef OPENSSL_SUPPORT
	case SSL_CONN:
		return do_read_ssl(IocpSock, IocpBuff);
#endif
#ifdef KCP_SUPPORT
	case KCP_CONN:
		return do_read_kcp(IocpSock, IocpBuff);
#endif
	default:
		break;
	}
}

#ifdef OPENSSL_SUPPORT
static bool Hsocket_SSL_init(SOCKET_CTX* IocpSock, int openssl_type, const char* ca_crt, const char* user_crt, const char* pri_key) {
	struct SSL_Content* ssl_ctx = (SSL_Content*)malloc(sizeof(struct SSL_Content));
	if (!ssl_ctx) return false;
	memset(ssl_ctx, 0x0, sizeof(SSL_Content));
	if (openssl_type == OPENSSL_CLIENT) ssl_ctx->ctx = SSL_CTX_new(SSLv23_client_method());
	else ssl_ctx->ctx = SSL_CTX_new(SSLv23_server_method());
	if (!ssl_ctx->ctx) { free(ssl_ctx); return false; }

	if (openssl_type == OPENSSL_SERVER) {
		SSL_CTX_set_verify( ssl_ctx->ctx,SSL_VERIFY_NONE, NULL);

		BIO* cbio = BIO_new_mem_buf(user_crt, (int)strlen(user_crt));
		X509* cert = PEM_read_bio_X509(cbio, NULL, NULL, NULL); //PEM格式
		SSL_CTX_use_certificate(ssl_ctx->ctx, cert);
		BIO_free(cbio);
		//SSL_CTX_use_certificate_file(ssl_ctx->ctx, "cacert.pem", SSL_FILETYPE_PEM);

		BIO* b = BIO_new_mem_buf((void*)pri_key, (int)strlen(pri_key));
		EVP_PKEY* evpkey = PEM_read_bio_PrivateKey(b, NULL, NULL, NULL);
		SSL_CTX_use_PrivateKey(ssl_ctx->ctx, evpkey);
		BIO_free(b);
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
	if (openssl_type == OPENSSL_CLIENT) SSL_set_connect_state(ssl_ctx->ssl);
	else SSL_set_accept_state(ssl_ctx->ssl);
	IocpSock->_user_data = ssl_ctx;
	IocpSock->_conn_type = SSL_CONN;

	if (openssl_type == OPENSSL_CLIENT) {
		int ret = SSL_do_handshake(ssl_ctx->ssl);
		ssl_do_write(ssl_ctx, IocpSock);
	}
	return true;
}

static void Hsocket_upto_SSL_Client(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	if (!Hsocket_SSL_init(IocpSock, OPENSSL_CLIENT, NULL, NULL, NULL))
		return do_close(IocpSock, IocpBuff, 0);
	PostRecv(IocpSock, IocpBuff);
}
#endif

static void do_connect(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff) {
	//hsocket_set_keepalive(IocpSock->fd);
	setsockopt(IocpSock->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);  //连接成功后刷新套接字属性
#ifdef OPENSSL_SUPPORT
	if (IocpSock->_conn_type == SSL_CONN) {
		return Hsocket_upto_SSL_Client(IocpSock, IocpBuff);
	}
#endif
	BaseProtocol* proto  = IocpSock->_user;
	if (IocpSock->fd != INVALID_SOCKET) {
		proto->Lock();
		if (IocpSock->fd != INVALID_SOCKET) {
			proto->ConnectionMade(IocpSock, IocpSock->_conn_type);
			proto->UnLock();
			PostRecv(IocpSock, IocpBuff);
			return;
		}
		proto->UnLock();
	}
	do_close(IocpSock, IocpBuff, 0);
}

static void do_timer(HSOCKET hsock, DWORD close) {
	HTIMER timer_ctx = (HTIMER)hsock->_user_data;
	if (!close) {
		if (!timer_ctx->close) {
			Timer_Callback callback = timer_ctx->call;
			callback(hsock, hsock->_user);
		}
		LONGUNLOCK(&timer_ctx->lock);
	}
	else {
		DeleteTimerQueueTimer(NULL, timer_ctx->timer, INVALID_HANDLE_VALUE);
		free(timer_ctx);
		Release_Socket_Ctx(hsock);
	}
}

static void ProcessIO(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	switch (IocpBuff->type){
	case READ:
		do_read(IocpSock, IocpBuff);
		break;
	case WRITE:
		do_close(IocpSock, IocpBuff, 0);
		break;
	case ACCEPT:
		do_aceept(IocpSock, IocpBuff);
		break;
	case CONNECT:
		do_connect(IocpSock, IocpBuff);
		break;
	default:
		break;
	}
}

/////////////////////////////////////////////////////////////////////////
//服务线程
DWORD WINAPI serverWorkerThread(LPVOID pParam){
	DWORD	dwIoSize = 0;
	SOCKET_CTX* IocpSock = NULL;
	BUFF_CTX* IocpBuff = NULL;		//IO数据,用于发起接收重叠操作
	bool bRet = false;
	DWORD err = 0;
	while (true){
		bRet = false;
		dwIoSize = 0;	//IO操作长度
		IocpSock = NULL;
		IocpBuff = NULL;
		err = 0;
		bRet = GetQueuedCompletionStatus(CompletionPort, &dwIoSize, (PULONG_PTR)&IocpSock, (LPOVERLAPPED*)&IocpBuff, INFINITE);
		if (IocpBuff != NULL) IocpSock = IocpBuff->hsock;   //强制closesocket后可能返回错误的IocpSock，从IocpBuff中获取正确的IocpSock
		else if (IocpSock->_conn_type == TIMER) {
			do_timer(IocpSock, dwIoSize);
			continue;
		}
		if (bRet == false){
			err = WSAGetLastError();  //64L,121L,995L
			if (IocpBuff == NULL || WAIT_TIMEOUT == err || ERROR_IO_PENDING == err) continue;
			do_close(IocpSock, IocpBuff, err);
			continue;
		}
		else if (0 == dwIoSize && (READ == IocpBuff->type || WRITE == IocpBuff->type)){
			do_close(IocpSock, IocpBuff, err);
			continue;
		}
		else{
			IocpBuff->offset += dwIoSize;
			ProcessIO(IocpSock, IocpBuff);
		}
	}
	return 0;
}

DWORD WINAPI mainIOCPServer(LPVOID pParam){
	HANDLE ThreadHandle = NULL;
	for (DWORD i = 0; i < CompletionPortWorker; i++){
	//for (unsigned int i = 0; i < 1; i++){
		ThreadHandle = CreateThread(NULL, 0, serverWorkerThread, pParam, 0, NULL);
		if (NULL == ThreadHandle) {
			return -4;
		}
		CloseHandle(ThreadHandle);
	}
	std::map<uint16_t, BaseFactory*>::iterator iter;
	while (true){
		for (iter = Factorys.begin(); iter != Factorys.end(); ++iter){
			iter->second->FactoryLoop();
		}
		Sleep(100);
	}
	return 0;
}

int __STDCALL ReactorStart(){
	WSADATA wsData;
	if (0 != WSAStartup(0x0202, &wsData)){
		return SOCKET_ERROR;
	}

	CompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (!CompletionPort){
		return -2;
	}

	SYSTEM_INFO sysInfor;
	GetSystemInfo(&sysInfor);
	CompletionPortWorker = sysInfor.dwNumberOfProcessors;

	SOCKET tempSock = WSASocket(AF_INET6, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
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

	HANDLE ThreadHandle;
	ThreadHandle = CreateThread(NULL, 0, mainIOCPServer, NULL, 0, NULL);
	if (NULL == ThreadHandle) {
		return -4;
	}
	CloseHandle(ThreadHandle);
	return 0;
}

int __STDCALL FactoryRun(BaseFactory* fc){
	if (!fc->FactoryInit()) return -1;

	if (fc->ServerPort != 0){
		fc->Listenfd = get_listen_sock(fc->ServerAddr, fc->ServerPort);
		if (fc->Listenfd == SOCKET_ERROR) return -2;

		CreateIoCompletionPort((HANDLE)fc->Listenfd, CompletionPort, (ULONG_PTR)0, 0);
		for (DWORD i = 0; i < CompletionPortWorker; i++)
			PostAcceptClient(fc);
	}
	fc->FactoryInited();
	Factorys.insert(std::pair<uint16_t, BaseFactory*>(fc->ServerPort, fc));
	return 0;
}

int __STDCALL FactoryStop(BaseFactory* fc){
	std::map<uint16_t, BaseFactory*>::iterator iter;
	iter = Factorys.find(fc->ServerPort);
	if (iter != Factorys.end()){
		Factorys.erase(iter);
	}
	fc->FactoryClose();
	return 0;
}

static void timer_queue_callback(PVOID lpParam, BOOLEAN TimerOrWaitFired) {
	HSOCKET hsock = (HSOCKET)lpParam;
	HTIMER timer_ctx = (HTIMER)hsock->_user_data;
	if (!timer_ctx->close) {
		if (LONGTRYLOCK(&timer_ctx->lock)) {
			PostQueuedCompletionStatus(CompletionPort, 0, (ULONG_PTR)hsock, NULL);
		}
	}
	else {
		if (LONGTRYLOCK(&timer_ctx->lock)) {
			PostQueuedCompletionStatus(CompletionPort, 1, (ULONG_PTR)hsock, NULL);
		}
	}
}

HSOCKET	__STDCALL TimerCreate(BaseProtocol* proto, int duetime, int looptime, Timer_Callback callback) {
	BaseFactory* factory = proto->factory;
	HSOCKET hsock = New_Socket_Ctx();
	hsock->_conn_type = TIMER;
	hsock->factory = factory;
	hsock->_user = proto;
	HTIMER timer_ctx = (HTIMER)malloc(sizeof(IOCP_TIMER));
	if (!timer_ctx) return NULL;
	memset(timer_ctx, 0x0, sizeof(IOCP_TIMER));
	timer_ctx->call = callback;
	CreateTimerQueueTimer(&timer_ctx->timer, NULL, (WAITORTIMERCALLBACK)timer_queue_callback, hsock, duetime, looptime, 0);
	hsock->_user_data = timer_ctx;
	return hsock;
}

void __STDCALL TimerDelete(HSOCKET hsock) {
	HTIMER timer_ctx = (HTIMER)hsock->_user_data;
	LONGLOCK(&timer_ctx->close);
}

static bool IOCPConnectUDP(BaseFactory* fc, SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff, int listen_port)
{
	IocpSock->fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if (IocpSock->fd == INVALID_SOCKET) return false;

	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	local_addr.sin6_port = ntohs(listen_port);
	local_addr.sin6_addr = in6addr_any;
	socket_set_v6only(IocpSock->fd, 0);
	bind(IocpSock->fd, (struct sockaddr*)(&local_addr), sizeof(local_addr));

	if (ResetIocp_Buff(IocpSock, IocpBuff) == false){
		closesocket(IocpSock->fd);
		return false;
	}

	CreateIoCompletionPort((HANDLE)IocpSock->fd, CompletionPort, (ULONG_PTR)IocpSock, 0);
	int fromlen = sizeof(IocpSock->peer_addr);
	IocpBuff->type = READ;
	if (SOCKET_ERROR == WSARecvFrom(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &WSA_RECV_FLAGS,
		(struct sockaddr*)&IocpSock->peer_addr, &fromlen, &IocpBuff->overlapped, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError()){
			closesocket(IocpSock->fd);
			return false;
		}
	}
	return true;
}

HSOCKET __STDCALL HsocketListenUDP(BaseProtocol* proto, int port){
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) return NULL;
	BaseFactory* fc = proto->factory;
	SOCKET_CTX* IocpSock = New_Socket_Ctx();
	if (IocpSock == NULL) return NULL; 

	BUFF_CTX* IocpBuff = New_Buff_Ctx();
	if (IocpBuff == NULL){
		Release_Socket_Ctx(IocpSock);
		return NULL;
	}
	IocpBuff->type = CONNECT;
	IocpBuff->hsock = IocpSock;

	IocpSock->factory = fc;
	IocpSock->_conn_type = UDP_CONN;
	IocpSock->_user = proto;
	IocpSock->_IocpBuff = IocpBuff;
	IocpSock->peer_addr.sin6_family = AF_INET6;
	IocpSock->peer_addr.sin6_port = htons(0);
	inet_pton(AF_INET6, "::", &IocpSock->peer_addr.sin6_addr);

	bool ret = false;
	ret = IOCPConnectUDP(fc, IocpSock, IocpBuff, port);   //UDP连接
	if (ret == false){
		Release_Buff_Ctx(IocpBuff);
		Release_Socket_Ctx(IocpSock);
		return NULL;
	}
	InterlockedIncrement(&proto->sockCount);
	return 0;
}

static bool IOCPConnectTCP(BaseFactory* fc, SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	IocpSock->fd = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
	if (IocpSock->fd == INVALID_SOCKET) return false;
	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	socket_set_v6only(IocpSock->fd, 0);
	bind(IocpSock->fd, (struct sockaddr*)(&local_addr), sizeof(local_addr));
	CreateIoCompletionPort((HANDLE)IocpSock->fd, CompletionPort, (ULONG_PTR)IocpSock, 0);

	PVOID lpSendBuffer = NULL;
	DWORD dwSendDataLength = 0;
	DWORD dwBytesSent = 0;
	BOOL bResult = lpfnConnectEx(IocpSock->fd,
		(struct sockaddr*)&IocpSock->peer_addr,	// [in] 对方地址
		sizeof(IocpSock->peer_addr),		// [in] 对方地址长度
		lpSendBuffer,			// [in] 连接后要发送的内容，这里不用
		dwSendDataLength,		// [in] 发送内容的字节数 ，这里不用
		&dwBytesSent,			// [out] 发送了多少个字节，这里不用
		&(IocpBuff->overlapped));
	if (!bResult){
		if (WSAGetLastError() != ERROR_IO_PENDING){
			closesocket(IocpSock->fd);
			return false;
		}
	}
	return true;
}

HSOCKET __STDCALL HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE conntype){
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) return NULL;
	BaseFactory* fc = proto->factory;
	SOCKET_CTX* IocpSock = New_Socket_Ctx();
	if (IocpSock == NULL) return NULL;

	BUFF_CTX* IocpBuff = New_Buff_Ctx();
	if (IocpBuff == NULL){
		Release_Socket_Ctx(IocpSock);
		return NULL;
	}
	IocpBuff->type = CONNECT;
	IocpBuff->hsock = IocpSock;

	IocpSock->factory = fc;
	IocpSock->_conn_type = conntype > TIMER ? TCP_CONN: conntype;
	IocpSock->_user = proto;
	IocpSock->_IocpBuff = IocpBuff;
	IocpSock->peer_addr.sin6_family = AF_INET6;
	IocpSock->peer_addr.sin6_port = htons(port);

	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &IocpSock->peer_addr.sin6_addr);

	bool ret = false;
	if (conntype == TCP_CONN || conntype == SSL_CONN)
		ret = IOCPConnectTCP(fc, IocpSock, IocpBuff);   //TCP连接
	else if (conntype == UDP_CONN || conntype == KCP_CONN)
		ret = IOCPConnectUDP(fc, IocpSock, IocpBuff, 0);   //UDP连接
	else {
		Release_Buff_Ctx(IocpBuff);
		Release_Socket_Ctx(IocpSock);
		return NULL;
	}
	if (ret == false){
		Release_Buff_Ctx(IocpBuff);
		Release_Socket_Ctx(IocpSock);
		return NULL;
	}
	InterlockedIncrement(&proto->sockCount);
	return IocpSock;
}

static bool IOCPPostSendUDPEx(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff, struct sockaddr* addr, int addrlen){
	if (SOCKET_ERROR == WSASendTo(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, addr, addrlen, &IocpBuff->overlapped, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError())
			return false;
	}
	return true;
}

static bool IOCPPostSendTCPEx(SOCKET_CTX* IocpSock, BUFF_CTX* IocpBuff){
	if (SOCKET_ERROR == WSASend(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, &IocpBuff->overlapped, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError())
			return false;
	}
	return true;
}

static bool HsocketSendEx(SOCKET_CTX* IocpSock, const char* data, int len){
	BUFF_CTX* IocpBuff = New_Buff_Ctx();
	if (IocpBuff == NULL) return false;

	IocpBuff->databuf.buf = (char*)malloc(len);
	if (IocpBuff->databuf.buf == NULL){
		Release_Buff_Ctx(IocpBuff);
		return false;
	}
	memcpy(IocpBuff->databuf.buf, data, len);
	IocpBuff->databuf.len = len;
	memset(&IocpBuff->overlapped, 0, sizeof(OVERLAPPED));
	IocpBuff->type = WRITE;

	bool ret = false;
	if (IocpSock->_conn_type == TCP_CONN || IocpSock->_conn_type == SSL_CONN)
		ret = IOCPPostSendTCPEx(IocpSock, IocpBuff);
	else if (IocpSock->_conn_type == UDP_CONN || IocpSock->_conn_type == KCP_CONN)
		ret = IOCPPostSendUDPEx(IocpSock, IocpBuff, (struct sockaddr*)&IocpSock->peer_addr, sizeof(IocpSock->peer_addr));
	if (ret == false){
		free(IocpBuff->databuf.buf);
		Release_Buff_Ctx(IocpBuff);
		return false;
	}
	return true;
}

#ifdef OPENSSL_SUPPORT
static bool HsocketSendSSL(HSOCKET hsock, const char* data, int len) {
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->_user_data;
	SSL_write(ssl_ctx->ssl, data, len);
	ssl_do_write(ssl_ctx, hsock);
	return true;
}
#endif

#ifdef KCP_SUPPORT
static bool HsocketSendKcp(HSOCKET hsock, const char* data, int len) {
	Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
	if (ctx->enable)
	{
		ikcp_send(ctx->kcp, data, len);
		return true;
	}
	return HsocketSendEx(hsock, data, len);
}
#endif

bool __STDCALL HsocketSend(HSOCKET hsock, const char* data, int len) {
	if (hsock) {
		switch (hsock->_conn_type){
		case TCP_CONN:
		case UDP_CONN:
			return HsocketSendEx(hsock, data, len);
#ifdef OPENSSL_SUPPORT
		case SSL_CONN:
			return HsocketSendSSL(hsock, data, len);
#endif
#ifdef KCP_SUPPORT
		case KCP_CONN:
			return HsocketSendKcp(hsock, data, len);
#endif
		default:
			break;
		}
	}
	return false;
}

bool __STDCALL HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len) {
	if (hsock->_conn_type == UDP_CONN){
		BUFF_CTX* IocpBuff = New_Buff_Ctx();
		if (IocpBuff == NULL) return false;

		IocpBuff->databuf.buf = (char*)malloc(len);
		if (IocpBuff->databuf.buf == NULL) {
			Release_Buff_Ctx(IocpBuff);
			return false;
		}
		memcpy(IocpBuff->databuf.buf, data, len);
		IocpBuff->databuf.len = len;
		memset(&IocpBuff->overlapped, 0, sizeof(OVERLAPPED));
		IocpBuff->type = WRITE;
		
		struct sockaddr_in6 toaddr = { 0x0 };
		toaddr.sin6_family = AF_INET6;
		toaddr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, dst, &toaddr.sin6_addr);

		bool ret = IOCPPostSendUDPEx(hsock, IocpBuff, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr));
		if (ret == false) {
			free(IocpBuff->databuf.buf);
			Release_Buff_Ctx(IocpBuff);
			return false;
		}
		return true;
	}
	return false;
}

BUFF_CTX* __STDCALL HsocketGetBuff(){
	BUFF_CTX* IocpBuff = New_Buff_Ctx();
	if (IocpBuff){
		IocpBuff->databuf.buf = (char*)malloc(DATA_BUFSIZE);
		if (IocpBuff->databuf.buf) { IocpBuff->size = DATA_BUFSIZE; }
		else { Release_Buff_Ctx(IocpBuff); return NULL; }	
	}
	return IocpBuff;
}

bool __STDCALL HsocketSetBuff(HBUFF netbuff, const char* data, int len){
	if (netbuff == NULL) return false;
	int left = netbuff->size - netbuff->offset;
	if (left >= len){
		memcpy(netbuff->databuf.buf + netbuff->databuf.len, data, len);
		netbuff->databuf.len += len;
	}
	else{
		int newsize = netbuff->databuf.len + len;
		char* new_ptr = (char*)realloc(netbuff->databuf.buf, newsize);
		if (new_ptr) {
			netbuff->databuf.buf = new_ptr;
			netbuff->size = newsize;
			memcpy(netbuff->databuf.buf + netbuff->databuf.len, data, len);
			netbuff->databuf.len += len;
		}
		else 
			return false;
	}
	return true;
}

bool __STDCALL HsocketSendBuff(HSOCKET hsock, HBUFF netbuff){
	if (netbuff == NULL || hsock == NULL) return false;
	memset(&netbuff->overlapped, 0, sizeof(OVERLAPPED));
	netbuff->type = WRITE;

	bool ret = false;
	if (hsock->_conn_type == TCP_CONN) ret = IOCPPostSendTCPEx(hsock, netbuff);
	else ret = IOCPPostSendUDPEx(hsock, netbuff, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr));
	if (ret == false){
		free(netbuff->databuf.buf);
		Release_Buff_Ctx(netbuff);
		return false;
	}
	return true;
}

bool __STDCALL HsocketClose(HSOCKET hsock){
	if (hsock == NULL || hsock->fd == INVALID_SOCKET || hsock->fd == NULL) return false;
	SOCKET fd = InterlockedExchange(&hsock->fd, NULL);
	if (fd != INVALID_SOCKET && fd != NULL){
		closesocket(fd);
	}
	return true;
}

void __STDCALL HsocketClosed(HSOCKET hsock) {
	SOCKET fd = InterlockedExchange(&hsock->fd, INVALID_SOCKET);
	if (fd != INVALID_SOCKET && fd != NULL){
		InterlockedDecrement(&hsock->_user->sockCount);
		//CancelIoEx((HANDLE)fd, NULL);	//取消等待执行的异步操作
		closesocket(fd);
	}
}

int __STDCALL HsocketPopBuf(HSOCKET hsock, int len)
{
	switch (hsock->_conn_type) {
	case TCP_CONN:
	case UDP_CONN:{
		BUFF_CTX* IocpBuff = hsock->_IocpBuff;
		IocpBuff->offset -= len;
		memmove(hsock->recv_buf, hsock->recv_buf + len, IocpBuff->offset);
		return IocpBuff->offset;
	}
#ifdef OPENSSL_SUPPORT
	case SSL_CONN:{
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->_user_data;
		ssl_ctx->roffset -= len;
		memmove(ssl_ctx->rbuf, ssl_ctx->rbuf + len, ssl_ctx->roffset);
		return ssl_ctx->roffset;
	}
#endif
#ifdef KCP_SUPPORT
	case KCP_CONN:{
		Kcp_Content* ctx = (Kcp_Content*)hsock->_user_data;
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
	if (hsock->_conn_type == UDP_CONN || hsock->_conn_type == KCP_CONN) {
		InterlockedExchange16((short*) & hsock->peer_addr.sin6_port, htons(port));
		//hsock->peer_addr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, dst, &hsock->peer_addr.sin6_addr);
	}
}

void __STDCALL HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz){
	inet_ntop(AF_INET6, &hsock->peer_addr.sin6_addr, ip, ipsz);
	if (strncmp(ip, "::ffff:", 7) == 0) {
		memmove(ip, ip + 7, ipsz - 7);
	}
}
int __STDCALL HsocketPeerPort(HSOCKET hsock) {
	return ntohs(hsock->peer_addr.sin6_port);
}

void __STDCALL HsocketLocalIP(HSOCKET hsock, char* ip, size_t ipsz) {
	struct sockaddr_in6 local = { 0x0 };
	int len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	inet_ntop(AF_INET6, &local.sin6_addr, ip, ipsz);
	if (strncmp(ip, "::ffff:", 7) == 0) {
		memmove(ip, ip + 7, ipsz - 7);
	}
}
int __STDCALL HsocketLocalPort(HSOCKET hsock) {
	struct sockaddr_in6 local = { 0x0 };
	int len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	return ntohs(local.sin6_port);
}

BaseProtocol* __STDCALL HsocketBindUser(HSOCKET hsock, BaseProtocol* proto) {
	BaseProtocol* old = hsock->_user;
	InterlockedDecrement(&old->sockCount);
	hsock->_user = proto;
	InterlockedIncrement(&proto->sockCount);
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

#ifdef OPENSSL_SUPPORT
bool __STDCALL HsocketUptoSSL(HSOCKET hsock, int openssl_type, const char* ca_crt, const char* user_crt, const char* pri_key) {
	bool ret = false;
	if (hsock->_conn_type == TCP_CONN) {
		ret = Hsocket_SSL_init(hsock, openssl_type, ca_crt, user_crt, pri_key);
	}
	return ret;
}
#endif

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* user){
	HsocketSendEx((HSOCKET)user, buf, len);
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
		ctx->lock = 0;
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
		if (LONGTRYLOCK(&ctx->lock)) {
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
