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
#include <sys/epoll.h>
#include <netinet/tcp.h>
#include <fcntl.h>
#include <time.h>
#include <unistd.h>
#include <string.h>
#include <sys/sysinfo.h>
#include <sys/timerfd.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <netdb.h>
#include <map>
#ifdef OPENSSL_SUPPORT
#include <openssl/ssl.h>
#include <openssl/err.h>
#include "sys/signal.h"
#define SSL_PACK_SIZE 15*1024
#endif

#define DATA_BUFSIZE 4096

int ActorThreadWorker = 0;

typedef struct Thread_Content {
	int	WorkerCount;
	int	epoll_fd;
	int pipefd[2];
}ThreadStat;
#define THREAD_STAT_SIZE sizeof(ThreadStat)

static int pipe_write_fd = 0;
static int epoll_listen_fd = 0;
static ThreadStat* ThreadStats;

enum SOCKET_STAT:char{
	SOCKET_UNKNOWN = 0,
	SOCKET_ACCEPTING,
	SOCKET_ACCEPTED,
	SOCKET_CONNECTING,
	SOCKET_CONNECTED,
	SOCKET_CLOSEING,
	SOCKET_CLOSED,
	SOCKET_UNBIND,
	SOCKET_REBIND
};

#define THREAD_STATES_AT(x) ThreadStats + x;
ThreadStat* ThreadDistribution(BaseWorker* worker) {
	char thread_id = worker->thread_id;
	if (thread_id > -1) return THREAD_STATES_AT(thread_id);
	ThreadStat* tsa, * tsb;
	thread_id = 0;
	tsa = THREAD_STATES_AT(0);
	for (int i = 1; i <= ActorThreadWorker; i++) {
		tsb = THREAD_STATES_AT(i);
		tsb = THREAD_STATES_AT(i);
		if (tsb->WorkerCount < tsa->WorkerCount) {
			tsa = tsb;
			thread_id = i;
		}
	}
	__sync_add_and_fetch(&tsa->WorkerCount, 1);
	worker->thread_id = thread_id;
	return tsa;
}

ThreadStat* ThreadDistributionIndex(BaseWorker* worker, int index) {
	char thread_id = worker->thread_id;
	if (thread_id > -1) return THREAD_STATES_AT(thread_id);
	if (index > -1 && index <= ActorThreadWorker) {
		ThreadStat* tsa = THREAD_STATES_AT(index);
		__sync_add_and_fetch(&tsa->WorkerCount, 1);
		worker->thread_id = index;
		return tsa;
	}
	return NULL;
}

void ThreadUnDistribution(BaseWorker* worker) {
	short thread_id = worker->thread_id;
	if (thread_id > -1) {
		ThreadStat* ts = THREAD_STATES_AT(thread_id);
		__sync_sub_and_fetch(&ts->WorkerCount, 1);
		worker->thread_id = -1;
	}
}

#ifdef OPENSSL_SUPPORT
struct SSL_Content{
	SSL_CTX*	ctx;
	SSL*		ssl;
};
#endif

#ifdef KCP_SUPPORT
#include "ikcp.h"
#include "sys/time.h"

struct Kcp_Content{
	ikcpcb* kcp;
	char* 	buf;
	int		size;
	int		offset;
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
	HSOCKET hsock = (HSOCKET)malloc(sizeof(Socket_Content));
	if (hsock == NULL) return NULL;
	memset(hsock, 0, sizeof(Socket_Content));
	return hsock;
}

static void release_hsock(HSOCKET hsock){
#if defined OPENSSL_SUPPORT || defined KCP_SUPPORT
	switch (hsock->protocol){
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL:{
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->sock_data;
		if (ssl_ctx){
			if (ssl_ctx->ssl) {SSL_shutdown(ssl_ctx->ssl); SSL_free(ssl_ctx->ssl);}
			if (ssl_ctx->ctx) SSL_CTX_free(ssl_ctx->ctx);
			free(ssl_ctx);
		}
		break;
	}
#endif
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL:{
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
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
	if (hsock->fd) close(hsock->fd);
	if (hsock->recv_buf) free(hsock->recv_buf);
	if (hsock->write_buf) free(hsock->write_buf);
	free(hsock);
}

static bool buff_ctx_init(HSOCKET hsock){
	char* rbuf = (char*)malloc(DATA_BUFSIZE);
	char* sbuf = (char*)malloc(DATA_BUFSIZE);
	if (rbuf && sbuf){
		hsock->recv_buf = rbuf;
		hsock->recv_size = DATA_BUFSIZE;
		hsock->write_buf = sbuf;
		hsock->write_size = DATA_BUFSIZE;
		return true;
	}
	if (rbuf) free(rbuf);
	if (sbuf) free(sbuf);
	return false;
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
		printf("%s:%d [%s]:%d %d\n", __func__, __LINE__, ip, port, ret);
		return -2;
	}
	if (listen(fd, 10)) {return -3;}

	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return fd;
}
/*EPOLLERR | EPOLLHUP 为默认注册事件*/
static inline void epoll_add_timer_event_signal(void* ptr, int fd, int epoll_fd) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = ptr;
	epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev);	
}

static inline void epoll_add_accept(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->epoll_fd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static inline void epoll_add_connect(HSOCKET hsock){
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->epoll_fd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static inline void epoll_add_read(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->epoll_fd, EPOLL_CTL_ADD, hsock->fd, &ev);
}

static inline void epoll_mod_read(HSOCKET hsock) {
	struct epoll_event ev;
	int event = hsock->write_offset > 0 ? EPOLLOUT : 0;
	ev.events = event| EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->epoll_fd, EPOLL_CTL_MOD, hsock->fd, &ev);	
}

static inline void epoll_mod_write(HSOCKET hsock){
	if (hsock->_flag) return;
	struct epoll_event ev;
	ev.events = EPOLLOUT | EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(hsock->epoll_fd, EPOLL_CTL_MOD, hsock->fd, &ev);
}

static inline void epoll_del_read(int fd, int epoll_fd) {
	struct epoll_event ev;
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev);
}

static inline void set_linger_for_fd(int fd){
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

static inline void delete_worker(BaseWorker* worker) {
	if (worker->ref_count == 0 && worker->auto_free_flag == FREE_AUTO) {
		worker->_free();
	}
}

static void do_close(HSOCKET hsock){
	if (hsock->_conn_stat == SOCKET_ACCEPTING){
		BaseAccepter* accepter = (BaseAccepter*)hsock->sock_data;
		accepter->Listening = false;
	}
	else if(hsock->_conn_stat == SOCKET_UNBIND){
		epoll_del_read(hsock->fd, hsock->epoll_fd);
		BaseWorker* old = hsock->worker;
		old->ref_count--;
		hsock->worker = NULL;
		WorkerBind_Callback call = hsock->bind_call;
		call(hsock, old, hsock->call_data, 0);

		delete_worker(old);
		return;
	}
	else if (hsock->_conn_stat == SOCKET_REBIND) {
		BaseWorker* worker = hsock->rebind_worker;
		if (worker){
			epoll_del_read(hsock->fd, hsock->epoll_fd);
			BaseWorker* old = hsock->worker;
			old->ref_count--;

			hsock->worker = worker;
			ThreadStat* ts = ThreadDistribution(worker);
			hsock->epoll_fd = ts->epoll_fd;
			hsock->_conn_stat = SOCKET_REBIND;
			epoll_add_connect(hsock);
			delete_worker(old);
			hsock->rebind_worker = NULL;
		}
		else{
			worker = hsock->worker;
			WorkerBind_Callback callback = hsock->bind_call;
			callback(hsock, worker, hsock->call_data, -1);
			epoll_del_read(hsock->fd, hsock->epoll_fd);
    		release_hsock(hsock);
		}
		return;
	}
	else if (hsock->_conn_stat < SOCKET_CLOSED){
		BaseWorker* worker = hsock->worker;
		int error = 0;
		socklen_t errlen = sizeof(error);
		getsockopt(hsock->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
		worker->ref_count--;
		if (hsock->_conn_stat == SOCKET_CONNECTING)
			worker->ConnectionFailed(hsock, error);
		else
    		worker->ConnectionClosed(hsock, error);
		delete_worker(worker);
	}
    epoll_del_read(hsock->fd, hsock->epoll_fd);
    release_hsock(hsock);
}

#ifdef OPENSSL_SUPPORT
static bool do_ssl_init(HSOCKET hsock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key){
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)malloc(sizeof(struct SSL_Content));
	if (!ssl_ctx) { return false; }

	memset(ssl_ctx, 0, sizeof(struct SSL_Content));
	ssl_ctx->ctx = openssl_type == SSL_CLIENT? SSL_CTX_new(SSLv23_client_method()): SSL_CTX_new(SSLv23_server_method());
	if (ssl_ctx->ctx == NULL) {
		free(ssl_ctx);
		return false;
	}
	verify? SSL_CTX_set_verify(ssl_ctx->ctx, SSL_VERIFY_PEER, NULL): SSL_CTX_set_verify(ssl_ctx->ctx, SSL_VERIFY_NONE, NULL);
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
	if (!ssl_ctx->ssl){
		SSL_CTX_free(ssl_ctx->ctx);
		free(ssl_ctx);
		return false;
	}
	SSL_set_fd(ssl_ctx->ssl, hsock->fd);
	openssl_type == SSL_CLIENT? SSL_set_connect_state(ssl_ctx->ssl): SSL_set_accept_state(ssl_ctx->ssl);
	hsock->sock_data = ssl_ctx;
	hsock->protocol = SSL_PROTOCOL;
	
	if (openssl_type == SSL_CLIENT){
		SSL_do_handshake(ssl_ctx->ssl);
	}
	return true;
}

static int do_ssl_handshak(HSOCKET hsock, struct SSL_Content* ssl_ctx){
	int ret = SSL_do_handshake(ssl_ctx->ssl);
	if (ret == 1){
		if (hsock->_conn_stat < SOCKET_CLOSED){
			BaseWorker* worker = hsock->worker;
			hsock->_conn_stat = SOCKET_CONNECTED;
			hsock->_flag = 1;
			worker->ConnectionMade(hsock, hsock->protocol);
			hsock->_flag = 0;
		}
		return 0;
	}else{
		int err = SSL_get_error(ssl_ctx->ssl, ret);
		switch (err){
		case SSL_ERROR_WANT_WRITE:
		case SSL_ERROR_WANT_READ:
			return 0;
		default:
			return -1;
		}
	}
}
#endif

static int do_connect(HSOCKET hsock){
	PROTOCOL protocol = hsock->protocol;
#ifdef OPENSSL_SUPPORT
	if (protocol == SSL_PROTOCOL){
		if (do_ssl_init(hsock, SSL_CLIENT, 0, NULL, NULL, NULL))
			return 0;
		return -1;
	}
#endif
	if (hsock->_conn_stat < SOCKET_CLOSED){
		BaseWorker* worker = hsock->worker;
		hsock->_conn_stat = SOCKET_CONNECTED;
		hsock->_flag = 1;
		worker->ConnectionMade(hsock, protocol);
		hsock->_flag = 0;
		return 0;
	}
	return -1;
}

static int do_write_udp(HSOCKET hsock){
	if (!ATOMIC_TRYLOCK(hsock->_send_lock)) return 0;
	socklen_t socklen = sizeof(hsock->peer_addr);
	int slen = 0;
	char* data;
	int ret = 0, n = 0, len = 0;
	struct sockaddr* addr;
	while (slen < hsock->write_offset){
		data = hsock->write_buf + slen + sizeof(short) + socklen;
		len = *(short*)(hsock->write_buf + slen);
		addr = (struct sockaddr*)(hsock->write_buf + slen + sizeof(short));
		if (len > 0){
			n = sendto(hsock->fd, data, len, MSG_DONTWAIT, addr, socklen);
			if(n > 0) {
				slen += sizeof(short) + socklen + len;
			}
			else{
				if(errno != EINTR && errno != EAGAIN) {
					ret = -1;
				}
				break;
			}
		}
	}
	hsock->write_offset -= slen;
	memmove(hsock->write_buf, hsock->write_buf + slen, hsock->write_offset);
	ATOMIC_UNLOCK(hsock->_send_lock);
	return ret;
}

static int do_write_tcp(HSOCKET hsock){
	if (!ATOMIC_TRYLOCK(hsock->_send_lock)) return 0;
	char* data = hsock->write_buf;
	int len = hsock->write_offset;
	if (len > 0){
		int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		if(n > 0) {
			hsock->write_offset -= n;
			memmove(data, data + n, hsock->write_offset);
		}
		else if(errno != EINTR && errno != EAGAIN) {
			ATOMIC_UNLOCK(hsock->_send_lock);
			return -1;
		}
	}
	ATOMIC_UNLOCK(hsock->_send_lock);
	return 0;
}

#ifdef OPENSSL_SUPPORT
static int do_write_ssl(HSOCKET hsock)
{
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->sock_data);
	if (!SSL_is_init_finished(ssl_ctx->ssl)){
		return do_write_tcp(hsock);
	}
	if (!ATOMIC_TRYLOCK(hsock->_send_lock)) return 0;
	char* data = hsock->write_buf;
	int len = hsock->write_offset > SSL_PACK_SIZE? SSL_PACK_SIZE : hsock->write_offset;
	if (len > 0){
		int n = SSL_write(ssl_ctx->ssl, data, len);
		if(n > 0) {
			hsock->write_offset -= n;
			memmove(data, data + n, hsock->write_offset);
		}
		else if(errno != EINTR && errno != EAGAIN){
			printf("%s:%d write ssl error n:%d len:%d sslerr:%d errno:%d\n", __func__, __LINE__, n, len, SSL_get_error(ssl_ctx->ssl, n), errno);
			ATOMIC_UNLOCK(hsock->_send_lock);
			return -1;
		}
	}
	ATOMIC_UNLOCK(hsock->_send_lock);
	return 0;
}
#endif

static int do_write(HSOCKET hsock)
{
	PROTOCOL protocol = hsock->protocol;
	if (protocol == TCP_PROTOCOL)
		return do_write_tcp(hsock);
#ifdef OPENSSL_SUPPORT
	else if (protocol == SSL_PROTOCOL)
		return do_write_ssl(hsock);
#endif
	else
		return do_write_udp(hsock);
}

#ifdef OPENSSL_SUPPORT
static int do_read_ssl(HSOCKET hsock){
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->sock_data);
	if (!SSL_is_init_finished(ssl_ctx->ssl))
		return do_ssl_handshak(hsock, ssl_ctx);
	char* buf;
	int rlen, size;
	while (1){
		buf = hsock->recv_buf + hsock->offset;
		size = hsock->recv_size - hsock->offset;
		rlen = SSL_read(ssl_ctx->ssl, buf, size);
		if (rlen > 0){
			hsock->offset += rlen;
			if (rlen == size){
				size = hsock->recv_size*2;
				buf = (char*)realloc(hsock->recv_buf , size);
				if (!buf) return -1;
				hsock->recv_buf = buf;
				hsock->recv_size = size;
				continue;
			}
			break;
		}
		else{
			int err = SSL_get_error(ssl_ctx->ssl, rlen);
        	if (err == SSL_ERROR_WANT_READ) {
            	break;
        	}
			return -1;
		}
	}
	return hsock->offset;
}
#endif

static int do_read_tcp(HSOCKET hsock)
{
	char* buf;
	int rlen, size;
	while (1){
		buf = hsock->recv_buf + hsock->offset;
		size = hsock->recv_size - hsock->offset;
		rlen = recv(hsock->fd, buf, size, MSG_DONTWAIT);
		if (rlen > 0){
			hsock->offset += rlen;
			if (rlen == size){
				size = hsock->recv_size*2;
				buf = (char*)realloc(hsock->recv_buf , size);
				if (!buf) return -1;
				hsock->recv_buf = buf;
				hsock->recv_size = size;
				continue;
			}
			break;
		}
		else if (rlen == 0){
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
	return hsock->offset;
}

#ifdef KCP_SUPPORT
static int do_read_kcp(HSOCKET hsock){
	Kcp_Content* ctx = (Kcp_Content*)(hsock->sock_data);
	ikcp_input(ctx->kcp, hsock->recv_buf, hsock->offset);
	hsock->offset = 0;

	BaseWorker* worker = hsock->worker;
	char* buf;
	int rlen, size;
	while (1) {
		buf = ctx->buf+ctx->offset;
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
			}
			break; 
		}
		ctx->offset += rlen;

		if (hsock->_conn_stat < SOCKET_CLOSED){
			hsock->_flag = 1;
			worker->ConnectionRecved(hsock, ctx->buf, ctx->offset);
			hsock->_flag = 0;
		}
	}
	return 0;
}
#endif

static int do_read_udp(HSOCKET hsock){
	size_t buflen = 0;
	int addr_len=sizeof(hsock->peer_addr);
	BaseWorker* worker = hsock->worker;
	PROTOCOL protocol = hsock->protocol;

	char* buf = NULL;
	int rlen, ret = 0;
	while (1){
		buf = hsock->recv_buf + hsock->offset;
		buflen = hsock->recv_size - hsock->offset;
		rlen = recvfrom(hsock->fd, buf, buflen, MSG_DONTWAIT, (sockaddr*)&(hsock->peer_addr), (socklen_t*)&addr_len);
		if (rlen > 0){
			hsock->offset += rlen;
			if (protocol == UDP_PROTOCOL){
				if (hsock->_conn_stat < SOCKET_CLOSED){
					hsock->_flag = 1;
					worker->ConnectionRecved(hsock, hsock->recv_buf, hsock->offset);
					hsock->_flag = 0;
				}
			}
#ifdef KCP_SUPPORT
			else if (protocol == KCP_PROTOCOL){
				ret = do_read_kcp(hsock);
				if (ret) break;
			}
#endif
			continue;
		}
		else if (rlen == 0){
			break;
		}
		else if (rlen < 0 ){
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

static int do_read(HSOCKET hsock){
	int ret = 0;
	switch (hsock->protocol)
	{
	case TCP_PROTOCOL:
		ret = do_read_tcp(hsock);
		break;
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL:
		ret = do_read_ssl(hsock);
		break;
#endif
	case UDP_PROTOCOL:
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL:
#endif
		ret = do_read_udp(hsock);
		break;
	default:
		break;
	}
	if (ret > 0){
		if (hsock->_conn_stat < SOCKET_CLOSED){
			BaseWorker* worker = hsock->worker;
			hsock->_flag = 1;
			worker->ConnectionRecved(hsock, hsock->recv_buf, hsock->offset);
			hsock->_flag = 0;
		}
		ret = 0;
	}
	return ret;
}

static int do_rebind(HSOCKET hsock){
	BaseWorker* worker = hsock->worker;
	worker->ref_count++;
	hsock->_conn_stat = SOCKET_CONNECTED;
	WorkerBind_Callback callback = hsock->bind_call;
	hsock->_flag = 1;
	callback(hsock, worker, hsock->call_data, 0);
	hsock->_flag = 0;
	return 0;
}

static void do_timer(HTIMER hsock){
	uint64_t value;
	if (hsock->_conn_stat < SOCKET_CLOSED){
		Timer_Callback call = hsock->call;
		call(hsock, hsock->worker, hsock->user_data);
		if (hsock->_conn_stat < SOCKET_CLOSED && hsock->once == 0){
			read(hsock->fd, &value, sizeof(uint64_t)); //必须读取，否则定时器异常
			return;
		}
	}
	epoll_del_read(hsock->fd, hsock->epoll_fd);
	close(hsock->fd);
	free(hsock);
}

static void do_pipe(HPIPE pipe){
	Event_Content event_ctx;
	int n = 0;
	while(1){
		n = read(pipe->fd, &event_ctx, sizeof(Event_Content));
		if(n > 0){
			if (event_ctx.protocol == EVENT){
				Event_Callback call = event_ctx.ecall;
				call(event_ctx.worker, event_ctx.event_data);
			}
			else {
				Signal_Callback call = event_ctx.scall;
				call(event_ctx.worker, event_ctx.signal);
			}
			continue;
		}
		break;
	}
}

static int do_accept(HSOCKET listenhsock){
	BaseAccepter* accepter = (BaseAccepter*)listenhsock->sock_data;
    struct sockaddr_in6 addr;
	socklen_t len = sizeof(addr);
   	int fd = 0;

	while (1){
	    fd = accept(accepter->Listenfd, (struct sockaddr *)&addr, &len);
		if (fd < 0) break;
        HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(fd);
			continue;
		}
		if (!buff_ctx_init(hsock)){
			close(fd);
			release_hsock(hsock);
			continue;
		}
		memcpy(&hsock->peer_addr, &addr, sizeof(addr));
		socket_set_keepalive(fd);
		set_linger_for_fd(fd);
		fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
		hsock->fd = fd;
		hsock->_conn_stat = SOCKET_CONNECTING;

        BaseWorker* worker = accepter->GetWorker();
		if (worker) {
			if (worker->auto_free_flag == FREE_DEF) worker->auto_free_flag = FREE_AUTO;
			ThreadStat* ts = ThreadDistribution(worker);
			hsock->epoll_fd = ts->epoll_fd;
			hsock->worker = worker;
			worker->ref_count++;
			epoll_add_connect(hsock);
		}
		else if (accepter->accepted) {
			accepter->accepted(accepter, hsock);
		}
		else{
			close(fd);
			release_hsock(hsock);
			continue;
		}
	}
	return 1;
}

static void read_work_thread(int* efd){
	int epoll_fd = *efd;
	prctl(PR_SET_NAME, epoll_fd == epoll_listen_fd ? "accepter" :"worker");

    int i = 0,n = 0, ret = 0;
    struct epoll_event *pev = (struct epoll_event*)malloc(sizeof(struct epoll_event) * 64);
	if(pev == NULL) { return ; }
    HSOCKET		hsock = NULL;
	int			events = 0;
	while (1){
		n = epoll_wait(epoll_fd, pev, 64, -1);
		for(i = 0; i < n; i++) {
            hsock = (HSOCKET)pev[i].data.ptr;
			events = pev[i].events;
			switch(*(PROTOCOL*)hsock){
				case TIMER:
					do_timer((HTIMER)hsock);
					continue;
				case EVENT:
					do_pipe((HPIPE)hsock);
					continue;
				default:{
					if (events & (EPOLLERR | EPOLLHUP)){
						do_close(hsock);
						continue;
					}
					if (events & EPOLLOUT){  //connenct or write
						if (hsock->_conn_stat == SOCKET_REBIND) //
							ret = do_rebind(hsock);
						else if (hsock->_conn_stat == SOCKET_CONNECTING)  //
							ret = do_connect(hsock);
						else if (hsock->_conn_stat > SOCKET_CONNECTING)  //
							ret = do_write(hsock);
						}
					if (ret == 0 && (events & EPOLLIN)){  //read or accpet
						if (hsock->_conn_stat == SOCKET_ACCEPTING)
							ret = do_accept(hsock);
						else 
							ret = do_read(hsock);
					}
					if (ret < 0 || (hsock->_conn_stat >= SOCKET_CLOSEING && hsock->write_offset == 0)){
						do_close(hsock);
					}
					else if (ret == 0){
						epoll_mod_read(hsock);
					}
					ret = 0;
					break;
				}
			}
		}
	}
}

static int runEpollServer(){
    int i,rc;
	pthread_attr_t rattr;
   	pthread_t rtid;
	ThreadStat* ts;
    for (i = 0; i <= ActorThreadWorker; i++){
		ts = THREAD_STATES_AT(i);
		pthread_attr_init(&rattr);
		if((rc = pthread_create(&rtid, &rattr, (void*(*)(void*))read_work_thread, &ts->epoll_fd)) != 0){
			return -1;
		}
	}
	return 0;
}

int ActorStart(int thread_count){
	ActorThreadWorker = thread_count > 0? thread_count : get_nprocs_conf();

	ThreadStats = (ThreadStat*)malloc((ActorThreadWorker+1) * sizeof(ThreadStat));
	if (!ThreadStats) return -2;

	ThreadStat* ts = NULL;
	for (int i = 0; i <= ActorThreadWorker; i++) {
		ts = THREAD_STATES_AT(i);
		ts->epoll_fd = epoll_create(64);
		ts->WorkerCount = 0;
		pipe(ts->pipefd);
		
		int piperfd = ts->pipefd[0];
		Pipe_Content* pipe_ctx = (Pipe_Content*)malloc(sizeof(Pipe_Content));
		if (!pipe_ctx) return -3;
		pipe_ctx->protocol = EVENT;
		pipe_ctx->fd = piperfd;
		fcntl(piperfd, F_SETFL, fcntl(piperfd, F_GETFL, 0)|O_NONBLOCK);

		struct epoll_event ev;
		ev.events = EPOLLIN | EPOLLET;
		ev.data.ptr = pipe_ctx;
		epoll_ctl(ts->epoll_fd, EPOLL_CTL_ADD, piperfd, &ev);
	}
	pipe_write_fd = ts->pipefd[1];
	epoll_listen_fd = ts->epoll_fd;

#ifdef OPENSSL_SUPPORT
	signal(SIGPIPE, SIG_IGN);
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
#endif
	return runEpollServer();
}

int	AccepterRun(BaseAccepter* accepter, const char* ip, int listen_port){
	if (listen_port > 0){
		accepter->Listening = true;
		accepter->Listenfd = get_listen_sock(ip, listen_port);
		if (accepter->Listenfd < 0){
			printf("%s:%d %d\n", __func__, __LINE__, accepter->Listenfd);
			accepter->Listening = false;
			return -2;
		}
		HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(accepter->Listenfd);
			accepter->Listening = false;
			return -3;
		}
		hsock->fd = accepter->Listenfd;
		ThreadStat* ts = THREAD_STATES_AT(accepter->thread_id);
		hsock->epoll_fd = ts->epoll_fd;
		hsock->sock_data = accepter;
		hsock->_conn_stat = SOCKET_ACCEPTING;
		epoll_add_accept(hsock);
	}
	return 0;
}

int AccepterStop(BaseAccepter* accepter){
	if (accepter->Listenfd > 0) {
		close(accepter->Listenfd);
		accepter->Listenfd = 0;
	}
	else {
		accepter->Listening = false;
	}
	while (accepter->Listening) {
		sleep(0);
	}
	return 0;
}

static bool EpollConnectExUDP(BaseWorker* worker, HSOCKET hsock, int listen_port){
	int fd = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
	if(fd < 0) {
		return false;
	}
	
	struct sockaddr_in6 local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin6_family = AF_INET6;
	local_addr.sin6_port = htons(listen_port);
	local_addr.sin6_addr = in6addr_any;
	socket_set_v6only(hsock->fd, 0);

	if(bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	hsock->_conn_stat = SOCKET_CONNECTED;
	hsock->fd = fd;
	epoll_add_read(hsock);
	return true;
}

HSOCKET HsocketListenUDP(BaseWorker* worker, const char* ip, int port){
	if (worker == NULL) 
		return NULL;
	HSOCKET hsock = new_hsockt();
	if (hsock == NULL) 
		return NULL;
	if (!buff_ctx_init(hsock)){
		release_hsock(hsock);
		return NULL;
	}
	hsock->peer_addr.sin6_family = AF_INET6;
	hsock->peer_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &hsock->peer_addr.sin6_addr);

	hsock->protocol = UDP_PROTOCOL;
	hsock->worker = worker;
	ThreadStat* ts = ThreadDistribution(worker);
	hsock->epoll_fd = ts->epoll_fd;

	bool ret = false;
	ret =  EpollConnectExUDP(worker, hsock, port);
	if (ret == false) {
		release_hsock(hsock);
		return NULL;
	}
	worker->ref_count++;
	return hsock;
}

static bool EpollConnectExTCP(BaseWorker* worker, HSOCKET hsock){
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
	hsock->_conn_stat = SOCKET_CONNECTING;
	epoll_add_connect(hsock);
	return  true;
}

HSOCKET HsocketConnect(BaseWorker* worker, const char* ip, int port, PROTOCOL protocol){
	if (worker == NULL) 
		return NULL;
	HSOCKET hsock = new_hsockt();
	if (hsock == NULL) 
		return NULL;
	if (!buff_ctx_init(hsock)){
		release_hsock(hsock);
		return NULL;
	}
	hsock->peer_addr.sin6_family = AF_INET6;
	hsock->peer_addr.sin6_port = htons(port);
	char v6ip[40] = { 0x0 };
	const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, dst, &hsock->peer_addr.sin6_addr);

	hsock->protocol = protocol;
	hsock->worker = worker;
	ThreadStat* ts = ThreadDistribution(worker);
	hsock->epoll_fd = ts->epoll_fd;

	bool ret = false;
	protocol = protocol;
	if (protocol == UDP_PROTOCOL)
		ret =  EpollConnectExUDP(worker, hsock, 0);
	else if (protocol == TCP_PROTOCOL
#ifdef OPENSSL_SUPPORT
		|| protocol == SSL_PROTOCOL
#endif
		)
		ret = EpollConnectExTCP(worker, hsock);
	if (ret == false) {
		release_hsock(hsock);
		return NULL;
	}
	worker->ref_count++;
	return hsock;
}

HTIMER TimerCreate(BaseWorker* worker, void* user_data, int duetime, int looptime, Timer_Callback callback){
	HTIMER hsock = (HTIMER)malloc(sizeof(Timer_Content));
	if (hsock == NULL) 
		return NULL;
	int tfd = timerfd_create(CLOCK_MONOTONIC, 0);   //创建定时器
    if(tfd == -1) {
		free(hsock);
        return NULL;
    }
	hsock->fd = tfd;
	hsock->protocol = TIMER;
	hsock->worker = worker;
	hsock->call = callback;
	ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
	hsock->epoll_fd = ts ? ts->epoll_fd : epoll_listen_fd;
	hsock->_conn_stat = 0;
	hsock->once = looptime == 0 ? 1 : 0;
	hsock->user_data = user_data;

	duetime = duetime == 0? 1 : duetime;
    struct itimerspec time_intv;
    time_intv.it_value.tv_sec = duetime/1000;
    time_intv.it_value.tv_nsec = (duetime%1000)*1000000;
    time_intv.it_interval.tv_sec = looptime/1000;
    time_intv.it_interval.tv_nsec = (looptime%1000)*1000000;

    timerfd_settime(tfd, 0, &time_intv, NULL);  //启动定时器
	epoll_add_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
    return hsock;
}

void TimerDelete(HTIMER hsock){
	hsock->_conn_stat = SOCKET_CLOSED;
}

void PostEvent(BaseWorker* worker, void* event_data, Event_Callback callback){
	Event_Content event_ctx;
	event_ctx.protocol = EVENT;
	event_ctx.ecall = callback;
	event_ctx.worker = worker;
	event_ctx.event_data = event_data;
	
	ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
	int pipewfd = ts? ts->pipefd[1] : pipe_write_fd;
	write(pipewfd, &event_ctx, sizeof(Event_Content));
}

void PostSignal(BaseWorker* worker, long long signal, Signal_Callback callback){
	Event_Content signal_ctx;
	signal_ctx.protocol = SIGNAL;
	signal_ctx.scall = callback;
	signal_ctx.worker = worker;
	signal_ctx.signal = signal;
	
	ThreadStat* ts = worker ? ThreadDistribution(worker) : NULL;
	int pipewfd = ts? ts->pipefd[1] : pipe_write_fd;
	write(pipewfd, &signal_ctx, sizeof(Event_Content));
}

static void HsocketSendUdp(HSOCKET hsock, struct sockaddr* addr, int addrlen, const char* data, short len){
	ATOMIC_LOCK(hsock->_send_lock);
	int n = sendto(hsock->fd, data, len, MSG_DONTWAIT, addr, addrlen);
	if (n <= 0){
		int needlen = sizeof(short) + addrlen + len;
		int newlen = hsock->write_offset + needlen;
		if (hsock->write_size < newlen){
        	char* newbuf = (char*)realloc(hsock->write_buf, newlen);
        	if (newbuf == NULL) {
				printf("%s:%d memory realloc error\n", __func__, __LINE__);
				ATOMIC_UNLOCK(hsock->_send_lock);
				return;
			}
        	hsock->write_buf = newbuf;
        	hsock->write_size = newlen;
    	}
		memcpy(hsock->write_buf + hsock->write_offset, (char*)&len, sizeof(short));
		memcpy(hsock->write_buf + hsock->write_offset + sizeof(short), (char*)addr, addrlen);
    	memcpy(hsock->write_buf + hsock->write_offset + sizeof(short) + addrlen, data, len);
    	hsock->write_offset += needlen;
		epoll_mod_write(hsock);
	}
	ATOMIC_UNLOCK(hsock->_send_lock);
}

bool HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len){
	if (hsock->protocol == UDP_PROTOCOL){
		struct sockaddr_in6 toaddr = { 0x0 };
		toaddr.sin6_family = AF_INET6;
		toaddr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		const char* dst = socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, dst, &toaddr.sin6_addr);
		HsocketSendUdp(hsock, (struct sockaddr*)&toaddr, sizeof(toaddr), data, len);
		return true;
	}
	return false;
}

#ifdef KCP_SUPPORT
static inline void HsocketSendKcp(HSOCKET hsock, const char* data, int len){
	Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
	ikcp_send(ctx->kcp, data, len);
}
#endif

static inline void HsocketSendCopy(HSOCKET hsock, int write_offset, const char* data, int len){
	int need_offset = write_offset + len;
	if (need_offset > 0){
		if (hsock->write_size < need_offset){
			char* newbuf = (char*)realloc(hsock->write_buf, need_offset);
			if (newbuf == NULL) {
				printf("%s:%d realloc memory error\n", __func__, __LINE__);
				return;
			}
			hsock->write_buf = newbuf;
			hsock->write_size = need_offset;
		}
		memcpy(hsock->write_buf + write_offset, data, len);
		hsock->write_offset = need_offset;
		epoll_mod_write(hsock);
	}
}

#ifdef OPENSSL_SUPPORT
static void HsocketSendSSL(HSOCKET hsock, const char* data, int len){
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->sock_data);
	ATOMIC_LOCK(hsock->_send_lock);
	int write_offset = hsock->write_offset;
	if (write_offset == 0){
		int n = SSL_write(ssl_ctx->ssl, data, len > SSL_PACK_SIZE ? SSL_PACK_SIZE : len);
		if (n > 0){
			data += n;
			len -= n;
		}
	}
	HsocketSendCopy(hsock, write_offset, data, len);
	ATOMIC_UNLOCK(hsock->_send_lock);
}
#endif

static void HsocketSendTcp(HSOCKET hsock, const char* data, int len){
	ATOMIC_LOCK(hsock->_send_lock);
	int write_offset = hsock->write_offset;
	if (write_offset == 0){
		int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n > 0){
			data += n;
			len -= n;
		}
	}
	HsocketSendCopy(hsock, write_offset, data, len);
	ATOMIC_UNLOCK(hsock->_send_lock);
}

bool HsocketSend(HSOCKET hsock, const char* data, int len){
	if (hsock == NULL || hsock->fd < 0 ||len <= 0) return false;
	switch (hsock->protocol)
	{
	case TCP_PROTOCOL:
		HsocketSendTcp(hsock, data, len);
		break;
#ifdef OPENSSL_SUPPORT
	case SSL_PROTOCOL:
		HsocketSendSSL(hsock, data, len);
		break;
#endif
	case  UDP_PROTOCOL:
		HsocketSendUdp(hsock, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr), data, len);
		break;
#ifdef KCP_SUPPORT
	case KCP_PROTOCOL:
		HsocketSendKcp(hsock, data, len);
		break;
#endif
	default:
		break;
	}
	return true;
}

void HsocketClose(HSOCKET hsock){
	if (hsock == NULL) return;
	hsock->_conn_stat = SOCKET_CLOSEING;
	epoll_mod_write(hsock);
	return;
}

void HsocketClosed(HSOCKET hsock){
	if (hsock == NULL) return;
	hsock->worker->ref_count--;
	hsock->_conn_stat = SOCKET_CLOSED;
	epoll_mod_write(hsock);
	return;
}

int HsocketPopBuf(HSOCKET hsock, int len){
#ifdef KCP_SUPPORT
	if (hsock->protocol == KCP_PROTOCOL) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->sock_data;
		ctx->offset -= len;
		memmove(ctx->buf, ctx->buf + len, ctx->offset);
		return ctx->offset;
	}
#endif
	hsock->offset -= len;
	memmove(hsock->recv_buf, hsock->recv_buf + len, hsock->offset);
	return hsock->offset;
}

void HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port){
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
	socklen_t len = sizeof(struct sockaddr_in6);
	getsockname(hsock->fd, (sockaddr*)&local, &len);
	if (ip) {
		inet_ntop(AF_INET6, &local.sin6_addr, ip, ipsz);
		if (strncmp(ip, "::ffff:", 7) == 0) {
			memmove(ip, ip + 7, ipsz - 7);
		}
	}
	if (port) *port = ntohs(local.sin6_port);
}

void __STDCALL HsocketUnbindWorker(HSOCKET hsock, void* user_data, WorkerBind_Callback call) {
	hsock->bind_call = call;
	hsock->call_data = user_data;
	hsock->_conn_stat = SOCKET_UNBIND;
}

void __STDCALL HsocketRebindWorker(HSOCKET hsock, BaseWorker* worker, void* user_data, WorkerBind_Callback call) {
	hsock->rebind_worker = worker;
	hsock->bind_call = call;
	hsock->call_data = user_data;
	hsock->_conn_stat = SOCKET_REBIND;

	if (hsock->worker) return;

	hsock->worker = worker;
	ThreadStat* ts = ThreadDistribution(worker);
	hsock->epoll_fd = ts->epoll_fd;
	epoll_add_connect(hsock);
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
bool __STDCALL HsocketSSLCreate(HSOCKET hsock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key){
	bool ret = false;
	if (hsock->protocol == TCP_PROTOCOL){
		ret = do_ssl_init(hsock, openssl_type, verify, ca_crt, user_crt, pri_key);
	}
	return ret;
}
#endif

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* kcp_data){
	HSOCKET hsock = (HSOCKET)kcp_data;
	HsocketSendUdp(hsock, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr),buf, len);
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
		if (!ctx->buf) { ikcp_release(kcp); free(ctx); return -1; }
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
