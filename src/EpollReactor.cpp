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
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <netdb.h>
#include <vector>
#ifdef OPENSSL_SUPPORT
#include <openssl/ssl.h>
#include <openssl/err.h>
#define SSL_PACK_SIZE 15*1024
#endif

#define DATA_BUFSIZE 4096

int ActorThreadWorker = 0;
int epoll_listen_fd = 0;
ThreadStat* ListenThreadStat = NULL;
std::vector<ThreadStat*> ThreadStats;
std::map<uint16_t, BaseAccepter*> Accepters;

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

ThreadStat* ThreadDistribution(BaseProtocol* proto) {
	if (proto->thread_stat) return proto->thread_stat;
	ThreadStat* tsa, * tsb;
	tsa = ThreadStats[0];
	for (int i = 1; i < ActorThreadWorker; i++) {
		tsb = ThreadStats[i];
		tsa = tsa->ProtocolCount <= tsb->ProtocolCount ? tsa : tsb;
	}
	__sync_add_and_fetch(&tsa->ProtocolCount, 1);
	proto->thread_stat = tsa;
	return tsa;
}

ThreadStat* ThreadDistributionIndex(BaseProtocol* proto, int index) {
	if (proto->thread_stat) return proto->thread_stat;
	ThreadStat* tsa = ThreadStats[index];
	__sync_add_and_fetch(&tsa->ProtocolCount, 1);
	proto->thread_stat = tsa;
	return tsa;
}

void ThreadUnDistribution(BaseProtocol* proto) {
	ThreadStat* ts = proto->thread_stat;
	__sync_sub_and_fetch (&ts->ProtocolCount, 1);
	proto->thread_stat = NULL;
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
	switch (hsock->conn_type)
	{
	case TIMER:
	case EVENT:
	case SIGNAL:
		free(hsock);
		return;
#ifdef OPENSSL_SUPPORT
	case SSL_CONN:{
		struct SSL_Content* ssl_ctx = (struct SSL_Content*)hsock->user_data;
		if (ssl_ctx){
			if (ssl_ctx->ssl) {SSL_shutdown(ssl_ctx->ssl); SSL_free(ssl_ctx->ssl);}
			if (ssl_ctx->ctx) SSL_CTX_free(ssl_ctx->ctx);
			free(ssl_ctx);
		}
		break;
	}
#endif
#ifdef KCP_SUPPORT
	case KCP_CONN:{
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		ikcp_release(ctx->kcp);
		free(ctx->buf);
		free(ctx);
		break;
	}
#endif
	default:
		break;
	}
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

static inline void epoll_mod_timer_event_signal(void* ptr, int fd, int epoll_fd) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = ptr;
	epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev);	
}

static inline void epoll_add_accept(HSOCKET hsock) {
	struct epoll_event ev;
	ev.events = EPOLLIN | EPOLLET;
	ev.data.ptr = hsock;
	epoll_ctl(epoll_listen_fd, EPOLL_CTL_ADD, hsock->fd, &ev);	
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

static inline void delete_protocol(BaseProtocol* proto, BaseAccepter* accpeter) {
	if (proto->sockCount == 0) {
		switch (proto->protoType) {
		case CLIENT_PROTOCOL:
			break;
		case SERVER_PROTOCOL:
			ThreadUnDistribution(proto);
			accpeter->ProtocolDelete(proto);
			break;
		case AUTO_PROTOCOL:
			ThreadUnDistribution(proto);
			delete proto;
			break;
		default:
			break;
		}
	}
}

static void do_close(HSOCKET hsock){
	if(hsock->_conn_stat == SOCKET_UNBIND){
		epoll_del_read(hsock->fd, hsock->epoll_fd);
		BaseProtocol* old = hsock->user;
		old->sockCount--;
		Unbind_Callback call = hsock->unbind_call;
		if (call){
			call(hsock, old, hsock->rebind_user, hsock->call_data);
		}
		else{
			BaseProtocol* user = hsock->rebind_user;
			hsock->user = user;
			hsock->epoll_fd = user->thread_stat->epoll_fd;
			hsock->_conn_stat = SOCKET_REBIND;
			epoll_add_connect(hsock);
		}
		delete_protocol(old, old->accepter);
		return;
	}
	else if (hsock->_conn_stat < SOCKET_CLOSED){
		BaseProtocol* proto = hsock->user;
		int error = 0;
		socklen_t errlen = sizeof(error);
		getsockopt(hsock->fd, SOL_SOCKET, SO_ERROR, (void *)&error, &errlen);
		proto->sockCount--;
		if (hsock->_conn_stat == SOCKET_CONNECTING)
			proto->ConnectionFailed(hsock, error);
		else
    		proto->ConnectionClosed(hsock, error);
		delete_protocol(proto, proto->accepter);
	}
    epoll_del_read(hsock->fd, hsock->epoll_fd);
    close(hsock->fd);
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
	hsock->user_data = ssl_ctx;
	hsock->conn_type = SSL_CONN;
	
	if (openssl_type == SSL_CLIENT){
		SSL_do_handshake(ssl_ctx->ssl);
	}
	return true;
}

static int do_ssl_handshak(HSOCKET hsock, struct SSL_Content* ssl_ctx){
	int ret = SSL_do_handshake(ssl_ctx->ssl);
	if (ret == 1){
		if (hsock->_conn_stat < SOCKET_CLOSED){
			BaseProtocol* proto = hsock->user;
			hsock->_conn_stat = SOCKET_CONNECTED;
			hsock->_flag = 1;
			proto->ConnectionMade(hsock, hsock->conn_type);
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
#ifdef OPENSSL_SUPPORT
	if (hsock->conn_type == SSL_CONN){
		if (do_ssl_init(hsock, SSL_CLIENT, 0, NULL, NULL, NULL))
			return 0;
		return -1;
	}
#endif
	if (hsock->_conn_stat < SOCKET_CLOSED){
		BaseProtocol* proto = hsock->user;
		hsock->_conn_stat = SOCKET_CONNECTED;
		hsock->_flag = 1;
		proto->ConnectionMade(hsock, hsock->conn_type);
		hsock->_flag = 0;
		return 0;
	}
	return -1;
}

static int do_write_udp(HSOCKET hsock){
	if (__sync_fetch_and_or(&hsock->_send_lock, 1)) return 0;
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
	__sync_fetch_and_and(&hsock->_send_lock, 0);
	return ret;
}

static int do_write_tcp(HSOCKET hsock){
	if (__sync_fetch_and_or(&hsock->_send_lock, 1)) return 0;
	char* data = hsock->write_buf;
	int len = hsock->write_offset;
	if (len > 0){
		int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		if(n > 0) {
			hsock->write_offset -= n;
			memmove(data, data + n, hsock->write_offset);
		}
		else if(errno != EINTR && errno != EAGAIN) {
			__sync_fetch_and_and(&hsock->_send_lock, 0);
			return -1;
		}
	}
	__sync_fetch_and_and(&hsock->_send_lock, 0);
	return 0;
}

#ifdef OPENSSL_SUPPORT
static int do_write_ssl(HSOCKET hsock)
{
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->user_data);
	if (!SSL_is_init_finished(ssl_ctx->ssl)){
		return do_write_tcp(hsock);
	}
	if (__sync_fetch_and_or(&hsock->_send_lock, 1)) return 0;
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
			__sync_fetch_and_and(&hsock->_send_lock, 0);
			return -1;
		}
	}
	__sync_fetch_and_and(&hsock->_send_lock, 0);
	return 0;
}
#endif

static int do_write(HSOCKET hsock)
{
	if (hsock->conn_type == TCP_CONN)
		return do_write_tcp(hsock);
#ifdef OPENSSL_SUPPORT
	else if (hsock->conn_type == SSL_CONN)
		return do_write_ssl(hsock);
#endif
	else
		return do_write_udp(hsock);
}

#ifdef OPENSSL_SUPPORT
static int do_read_ssl(HSOCKET hsock){
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->user_data);
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
	Kcp_Content* ctx = (Kcp_Content*)(hsock->user_data);
	ikcp_input(ctx->kcp, hsock->recv_buf, hsock->offset);
	hsock->offset = 0;

	BaseProtocol* proto = hsock->user;
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
			proto->ConnectionRecved(hsock, ctx->buf, ctx->offset);
			hsock->_flag = 0;
		}
	}
	return 0;
}
#endif

static int do_read_udp(HSOCKET hsock){
	size_t buflen = 0;
	int addr_len=sizeof(hsock->peer_addr);
	BaseProtocol* proto = hsock->user;

	char* buf = NULL;
	int rlen, ret = 0;
	while (1){
		buf = hsock->recv_buf + hsock->offset;
		buflen = hsock->recv_size - hsock->offset;
		rlen = recvfrom(hsock->fd, buf, buflen, MSG_DONTWAIT, (sockaddr*)&(hsock->peer_addr), (socklen_t*)&addr_len);
		if (rlen > 0){
			hsock->offset += rlen;
			if (hsock->conn_type == UDP_CONN){
				if (hsock->_conn_stat < SOCKET_CLOSED){
					hsock->_flag = 1;
					proto->ConnectionRecved(hsock, hsock->recv_buf, hsock->offset);
					hsock->_flag = 0;
				}
			}
#ifdef KCP_SUPPORT
			else if (hsock->conn_type == KCP_CONN){
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
	switch (hsock->conn_type)
	{
	case TCP_CONN:
		ret = do_read_tcp(hsock);
		break;
#ifdef OPENSSL_SUPPORT
	case SSL_CONN:
		ret = do_read_ssl(hsock);
		break;
#endif
	case UDP_CONN:
	case KCP_CONN:
		ret = do_read_udp(hsock);
		break;
	default:
		break;
	}
	if (ret > 0){
		if (hsock->_conn_stat < SOCKET_CLOSED){
			BaseProtocol* proto = hsock->user;
			hsock->_flag = 1;
			proto->ConnectionRecved(hsock, hsock->recv_buf, hsock->offset);
			hsock->_flag = 0;
		}
		ret = 0;
	}
	return ret;
}

static int do_rebind(HSOCKET hsock){
	BaseProtocol* proto = hsock->user;
	proto->sockCount++;
	hsock->_conn_stat = SOCKET_CONNECTED;
	Rebind_Callback callback = hsock->rebind_call;
	hsock->_flag = 1;
	callback(hsock, proto, hsock->call_data);
	hsock->_flag = 0;
	return 0;
}

static void do_timer(HTIMER hsock){
	uint64_t value;
	if (hsock->_conn_stat < SOCKET_CLOSED){
		Timer_Callback call = hsock->call;
		call(hsock, hsock->user);
		if (hsock->_conn_stat < SOCKET_CLOSED){
			read(hsock->fd, &value, sizeof(uint64_t)); //必须读取，否则定时器异常
			epoll_mod_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
			return;
		}
	}
	epoll_del_read(hsock->fd, hsock->epoll_fd);
	close(hsock->fd);
	free(hsock);
}

static void do_event(HEVENT hsock){
	Event_Callback call = hsock->call;
	call(hsock->user, hsock->event_data);
	epoll_del_read(hsock->fd, hsock->epoll_fd);
	close(hsock->fd);
	free(hsock);
}

static void do_signal(HSIGNAL hsock){
	Signal_Callback call = hsock->call;
	call(hsock->user, hsock->signal);
	epoll_del_read(hsock->fd, hsock->epoll_fd);
	close(hsock->fd);
	free(hsock);
}

static int do_accept(HSOCKET listenhsock){
	BaseAccepter* accpeter = (BaseAccepter*)listenhsock->user_data;
    struct sockaddr_in6 addr;
	socklen_t len = sizeof(addr);
   	int fd = 0;

	while (1){
	    fd = accept(accpeter->Listenfd, (struct sockaddr *)&addr, &len);
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

        BaseProtocol* proto = accpeter->ProtocolCreate();
		if (!proto->accepter) proto->AccepterSet(accpeter, SERVER_PROTOCOL);
		ThreadStat* ts = ThreadDistribution(proto);

        hsock->fd = fd;
		hsock->user = proto;
		hsock->epoll_fd = ts->epoll_fd;
		hsock->_conn_stat = SOCKET_CONNECTING;
		set_linger_for_fd(fd);
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
		proto->sockCount++;
		epoll_add_connect(hsock);
	}
	return 1;
}

static void read_work_thread(ThreadStat* ts){
	prctl(PR_SET_NAME, ts == ListenThreadStat ? "accepter" :"worker");
    int i = 0,n = 0, ret = 0;
    struct epoll_event *pev = (struct epoll_event*)malloc(sizeof(struct epoll_event) * 64);
	if(pev == NULL) { return ; }
    HSOCKET		hsock = NULL;
	int			events = 0;
	while (1){
		n = epoll_wait(ts->epoll_fd, pev, 64, -1);
		for(i = 0; i < n; i++) {
            hsock = (HSOCKET)pev[i].data.ptr;
			events = pev[i].events;
			switch(*(CONN_TYPE*)hsock){
				case TIMER:
					do_timer((HTIMER)hsock);
					continue;
				case EVENT:
					do_event((HEVENT)hsock);
					continue;
				case SIGNAL:
					do_signal((HSIGNAL)hsock);
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

static void accepter_timer_callback(HTIMER timer, BaseProtocol* proto) {
	std::map<uint16_t, BaseAccepter*>::iterator iter;
	for (iter = Accepters.begin(); iter != Accepters.end(); ++iter) {
		iter->second->TimeOut();
	}
}

static void accepters_timer_run(){
	HTIMER hsock = (HTIMER)malloc(sizeof(Timer_Content));
	if (hsock == NULL) 
		return;
	int tfd = timerfd_create(CLOCK_MONOTONIC, 0);   //创建定时器
    if(tfd == -1) {
		free(hsock);
        return;
    }
	hsock->fd = tfd;
	hsock->conn_type = TIMER;
	hsock->user = NULL;
	hsock->call = accepter_timer_callback;
	hsock->epoll_fd = ListenThreadStat->epoll_fd;
	hsock->_conn_stat = 0;

    struct itimerspec time_intv; //用来存储时间
    time_intv.it_value.tv_sec = 0;
    time_intv.it_value.tv_nsec = 1*1000;
    time_intv.it_interval.tv_sec = 1;
    time_intv.it_interval.tv_nsec = 0;
	timerfd_settime(tfd, 0, &time_intv, NULL);  //启动定时器

	epoll_add_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
}

static int runEpollServer(){
    int i = 0;
	int rc;

	pthread_attr_t rattr;
   	pthread_t rtid;
	pthread_attr_init(&rattr);
	if((rc = pthread_create(&rtid, &rattr, (void*(*)(void*))read_work_thread, ListenThreadStat)) != 0){
		return -1;
	}

    for (i = 0; i < ActorThreadWorker; i++){
		pthread_attr_init(&rattr);
		if((rc = pthread_create(&rtid, &rattr, (void*(*)(void*))read_work_thread, ThreadStats[i])) != 0){
			return -1;
		}
	}
	accepters_timer_run();
	return 0;
}

int ReactorStart(){
    epoll_listen_fd = epoll_create(64);
	if(epoll_listen_fd < 0)
		return -1;
	ListenThreadStat = (ThreadStat*)malloc(sizeof(ThreadStat));
	if (!ListenThreadStat) {
		return -2;
	}
	ListenThreadStat->epoll_fd = epoll_listen_fd;

	ActorThreadWorker = get_nprocs_conf();
	ThreadStats.reserve(ActorThreadWorker);
	ThreadStat* ts;
	for (int i = 0; i < ActorThreadWorker; i++) {
		ts = (ThreadStat*)malloc(sizeof(ThreadStat));
		if (!ts) return -2;
		ts->epoll_fd = epoll_create(64);
		ts->ProtocolCount = 0;
		ThreadStats.push_back(ts);
	}

#ifdef OPENSSL_SUPPORT
	SSL_library_init();
	SSL_load_error_strings();
	OpenSSL_add_all_algorithms();
#endif
	return runEpollServer();
}

int	AccepterRun(BaseAccepter* accepter){
	if (!accepter->Init())
		return -1;
	if (accepter->ServerPort > 0){
		accepter->Listenfd = get_listen_sock(accepter->ServerAddr, accepter->ServerPort);
		if (accepter->Listenfd < 0){
			printf("%s:%d %d\n", __func__, __LINE__, accepter->Listenfd);
			return -2;
		}
		HSOCKET hsock = new_hsockt();
		if (hsock == NULL) {
			close(accepter->Listenfd);
			return -3;
		}
		hsock->fd = accepter->Listenfd;
		hsock->user_data = accepter;
		hsock->_conn_stat = SOCKET_ACCEPTING;
		epoll_add_accept(hsock);
	}
	Accepters.insert(std::pair<uint16_t, BaseAccepter*>(accepter->ServerPort, accepter));
	return 0;
}

int AccepterClose(BaseAccepter* accpeter){
	std::map<uint16_t, BaseAccepter*>::iterator iter;
	iter = Accepters.find(accpeter->ServerPort);
	if (iter != Accepters.end()){
		Accepters.erase(iter);
	}
	accpeter->Close();
	return 0;
}

static bool EpollConnectExUDP(BaseProtocol* proto, HSOCKET hsock, int listen_port){
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

	if(bind(fd, (struct sockaddr *)&local_addr, sizeof(local_addr)) < 0) {
		close(fd);
		return false;
	}
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0)|O_NONBLOCK);
	hsock->_conn_stat = SOCKET_CONNECTED;
	epoll_add_read(hsock);
	return true;
}

HSOCKET HsocketListenUDP(BaseProtocol* proto, int port){
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) 
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
	inet_pton(AF_INET6, "::", &hsock->peer_addr.sin6_addr);

	hsock->conn_type = UDP_CONN;
	hsock->user = proto;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;

	bool ret = false;
	ret =  EpollConnectExUDP(proto, hsock, port);
	if (ret == false) {
		release_hsock(hsock);
		return NULL;
	}
	proto->sockCount++;
	return hsock;
}

static bool EpollConnectExTCP(BaseProtocol* proto, HSOCKET hsock){
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

HSOCKET HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE type){
	if (proto == NULL || (proto->sockCount == 0 && proto->protoType == SERVER_PROTOCOL)) 
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
	socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
	inet_pton(AF_INET6, v6ip, &hsock->peer_addr.sin6_addr);

	hsock->conn_type = type;
	hsock->user = proto;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;

	bool ret = false;
	if (type == UDP_CONN)
		ret =  EpollConnectExUDP(proto, hsock, 0);
	else if (type == TCP_CONN || type == SSL_CONN)
		ret = EpollConnectExTCP(proto, hsock);
	if (ret == false) {
		release_hsock(hsock);
		return NULL;
	}
	proto->sockCount++;
	return hsock;
}

HTIMER TimerCreate(BaseProtocol* proto, int duetime, int looptime, Timer_Callback callback){
	HTIMER hsock = (HTIMER)malloc(sizeof(Timer_Content));
	if (hsock == NULL) 
		return NULL;
	int tfd = timerfd_create(CLOCK_MONOTONIC, 0);   //创建定时器
    if(tfd == -1) {
		free(hsock);
        return NULL;
    }
	hsock->fd = tfd;
	hsock->conn_type = TIMER;
	hsock->user = proto;
	hsock->call = callback;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;
	hsock->_conn_stat = 0;

	duetime = duetime == 0? 1 : duetime;
    struct itimerspec time_intv; //用来存储时间
    time_intv.it_value.tv_sec = duetime/1000; //设定2s超时
    time_intv.it_value.tv_nsec = (duetime%1000)*1000000;
    time_intv.it_interval.tv_sec = looptime/1000;   //每隔2s超时
    time_intv.it_interval.tv_nsec = (looptime%1000)*1000000;

    timerfd_settime(tfd, 0, &time_intv, NULL);  //启动定时器
	epoll_add_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
    return hsock;
}

void TimerDelete(HTIMER hsock){
	hsock->_conn_stat = SOCKET_CLOSED;
}

void PostEvent(BaseProtocol* proto, Event_Callback callback, void* event_data){
	HEVENT hsock = (HEVENT)malloc(sizeof(Event_Content));
	if (hsock == NULL) return;
	int efd = eventfd(1, 0);
    if(efd == -1) {
		free(hsock);
		return;
    }
	hsock->fd = efd;
	hsock->conn_type = EVENT;
	hsock->user = proto;
	hsock->call = callback;
	hsock->event_data = event_data;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;
	epoll_add_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
}

void PostSignal(BaseProtocol* proto, Signal_Callback callback, unsigned long long signal){
	HSIGNAL hsock = (HSIGNAL)malloc(sizeof(Signal_Content));
	if (hsock == NULL) return;
	int efd = eventfd(1, 0);
    if(efd == -1) {
		free(hsock);
		return;
    }
	hsock->fd = efd;
	hsock->conn_type = SIGNAL;
	hsock->user = proto;
	hsock->call = callback;
	hsock->signal = signal;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;
	epoll_add_timer_event_signal(hsock, hsock->fd, hsock->epoll_fd);
}

static void HsocketSendUdp(HSOCKET hsock, struct sockaddr* addr, int addrlen, const char* data, short len){
	while (__sync_fetch_and_or(&hsock->_send_lock, 1)) usleep(0);
	int n = sendto(hsock->fd, data, len, MSG_DONTWAIT, addr, addrlen);
	if (n <= 0){
		int needlen = sizeof(short) + addrlen + len;
		int newlen = hsock->write_offset + needlen;
		if (hsock->write_size < newlen){
        	char* newbuf = (char*)realloc(hsock->write_buf, newlen);
        	if (newbuf == NULL) {
				printf("%s:%d memory realloc error\n", __func__, __LINE__);
				__sync_fetch_and_and(&hsock->_send_lock, 0);
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
	__sync_fetch_and_and(&hsock->_send_lock, 0);
}

bool HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len){
	if (hsock->conn_type == UDP_CONN){
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
	Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
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
	struct SSL_Content* ssl_ctx = (struct SSL_Content*)(hsock->user_data);
	while (__sync_fetch_and_or(&hsock->_send_lock, 1)) usleep(0);
	int write_offset = hsock->write_offset;
	if (write_offset == 0){
		int n = SSL_write(ssl_ctx->ssl, data, len > SSL_PACK_SIZE ? SSL_PACK_SIZE : len);
		if (n > 0){
			data += n;
			len -= n;
		}
	}
	HsocketSendCopy(hsock, write_offset, data, len);
	__sync_fetch_and_and(&hsock->_send_lock, 0);
}
#endif

static void HsocketSendTcp(HSOCKET hsock, const char* data, int len){
	while (__sync_fetch_and_or(&hsock->_send_lock, 1)) usleep(0);
	int write_offset = hsock->write_offset;
	if (write_offset == 0){
		int n = send(hsock->fd, data, len, MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n > 0){
			data += n;
			len -= n;
		}
	}
	HsocketSendCopy(hsock, write_offset, data, len);
	__sync_fetch_and_and(&hsock->_send_lock, 0);
}

bool HsocketSend(HSOCKET hsock, const char* data, int len){
	if (hsock == NULL || hsock->fd < 0 ||len <= 0) return false;
	switch (hsock->conn_type)
	{
	case TCP_CONN:
		HsocketSendTcp(hsock, data, len);
		break;
#ifdef OPENSSL_SUPPORT
	case SSL_CONN:
		HsocketSendSSL(hsock, data, len);
		break;
#endif
	case  UDP_CONN:
		HsocketSendUdp(hsock, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr), data, len);
		break;
#ifdef KCP_SUPPORT
	case KCP_CONN:
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
	hsock->user->sockCount--;
	hsock->_conn_stat = SOCKET_CLOSED;
	epoll_mod_write(hsock);
	return;
}

int HsocketPopBuf(HSOCKET hsock, int len){
#ifdef KCP_SUPPORT
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
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
	if (hsock->conn_type == UDP_CONN || hsock->conn_type == KCP_CONN) {
		hsock->peer_addr.sin6_port = htons(port);
		char v6ip[40] = { 0x0 };
		socket_ip_v4_converto_v6(ip, v6ip, sizeof(v6ip));
		inet_pton(AF_INET6, v6ip, &hsock->peer_addr.sin6_addr);
	}
}

void HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz){
    inet_ntop(AF_INET6, &hsock->peer_addr.sin6_addr, ip, ipsz);
	if (strncmp(ip, "::ffff:", 7) == 0) {
		memmove(ip, ip + 7, ipsz - 7);
	}
}

int HsocketPeerPort(HSOCKET hsock) {
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

void __STDCALL HsocketUnbindUser(HSOCKET hsock, BaseProtocol* proto, Unbind_Callback ucall, Rebind_Callback rcall, void* call_data) {
	hsock->unbind_call = ucall;
	hsock->rebind_call = rcall;
	hsock->rebind_user = proto;
	hsock->call_data = call_data;
	hsock->_conn_stat = SOCKET_UNBIND;
}

void __STDCALL HsocketRebindUser(HSOCKET hsock, BaseProtocol* proto, Rebind_Callback call, void* call_data) {
	hsock->rebind_call = call;
	hsock->user = proto;
	hsock->call_data = call_data;
	hsock->epoll_fd = proto->thread_stat->epoll_fd;
	hsock->_conn_stat = SOCKET_REBIND;
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
	if (hsock->conn_type == TCP_CONN){
		ret = do_ssl_init(hsock, openssl_type, verify, ca_crt, user_crt, pri_key);
	}
	return ret;
}
#endif

#ifdef KCP_SUPPORT
static int kcp_send_callback(const char* buf, int len, ikcpcb* kcp, void* user){
	HSOCKET hsock = (HSOCKET)user;
	HsocketSendUdp(hsock, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr),buf, len);
	return 0;
}

int __STDCALL HsocketKcpCreate(HSOCKET hsock, int conv, int mode){
	if (hsock->conn_type == UDP_CONN) {
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
		hsock->user_data = ctx;
		hsock->conn_type = KCP_CONN;
	}
	return 0;
}

void __STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc){
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		ikcp_nodelay(ctx->kcp, nodelay, interval, resend, nc);
	}
}

void __STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd){
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		ikcp_wndsize(ctx->kcp, sndwnd, rcvwnd);
	}
}

int __STDCALL HsocketKcpGetconv(HSOCKET hsock){
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		return ikcp_getconv(ctx->kcp);
	}
	return 0;
}

void __STDCALL HsocketKcpUpdate(HSOCKET hsock){
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		ikcp_update(ctx->kcp, iclock());
	}
}

int __STDCALL HsocketKcpDebug(HSOCKET hsock, char* buf, int size) {
	int n = 0;
	if (hsock->conn_type == KCP_CONN) {
		Kcp_Content* ctx = (Kcp_Content*)hsock->user_data;
		ikcpcb* kcp = ctx->kcp;
		n = snprintf(buf, size, "nsnd_buf[%d] nsnd_que[%d] nrev_buf[%d] nrev_que[%d] snd_wnd[%d] rev_wnd[%d] rmt_wnd[%d] cwd[%d]",
			kcp->nsnd_buf, kcp->nsnd_que, kcp->nrcv_buf, kcp->nrcv_que, kcp->snd_wnd, kcp->rcv_wnd, kcp->rmt_wnd, kcp->cwnd);
	}
	return n;
}
#endif
