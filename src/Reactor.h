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

#ifndef _REACTOR_H_
#define _REACTOR_H_

#if !defined(__WINDOWS__) && (defined(WIN32) || defined(WIN64) || defined(_MSC_VER) || defined(_WIN32))
#define __WINDOWS__
#endif

#define API_EXPORTS
//#define OPENSSL_SUPPORT
#ifdef OPENSSL_SUPPORT
#define SSL_SERVER 0
#define SSL_CLIENT 1
#endif

//#define KCP_SUPPORT

#ifdef __WINDOWS__
#define __STDCALL __stdcall
#define __CDECL__	__cdecl
#if defined API_EXPORTS
#define Reactor_API __declspec(dllexport)
#else
#define Reactor_API __declspec(dllimport)
#endif
#else
#define __STDCALL
#define __CDECL__
#define Reactor_API
#endif // __WINDOWS__

#include <stdio.h>
#include <stdlib.h>
#ifdef __WINDOWS__
#define WIN32_LEAN_AND_MEAN
#include "windows.h"
#include <Winsock2.h>
#include <ws2ipdef.h>
#include <Ws2tcpip.h>
#include <mswsock.h>    //微软扩展的类库
#else
#include <sys/socket.h>
#include <arpa/inet.h>
#include <mutex>
#endif // __WINDOWS__

#ifdef __WINDOWS__
#define LONGLOCK(a)  while (InterlockedExchange8(&a, 1)){Sleep(0);}
#define LONGUNLOCK(a)	InterlockedExchange8(&a, 0)
#define LONGTRYLOCK(a)	!InterlockedExchange8(&a, 1)
#else
#define LONGLOCK(a)	while (__sync_fetch_and_or(&a, 1)){sleep(0);}
#define LONGUNLOCK(a)	__sync_fetch_and_and(&a, 0)
#define LONGTRYLOCK(a)	!__sync_fetch_and_or(&a, 1)
#endif

enum CONN_TYPE:char {
	TCP_CONN = 0,
	UDP_CONN = 0x01,
	SSL_CONN = 0x02,
	KCP_CONN = 0x04,
	HTTP_CONN = 0x06,
	WS_CONN = 0x08,
	TIMER,
	EVENT,
	SIGNAL
};

enum PROTOCOL_TPYE:char {
	UNKNOWN_PROTOCOL = 0,
	CLIENT_PROTOCOL,
	SERVER_PROTOCOL
};

typedef struct {
	long	ProtocolCount;
#ifdef __WINDOWS__
	HANDLE	CompletionPort;
#else
	int		epoll_fd;
#endif
}ThreadStat;

extern int ActorThreadWorker;

class BaseAccepter;
class BaseProtocol;

typedef struct Timer_Content* HTIMER;
typedef struct Socket_Content* HSOCKET;
typedef void (*Timer_Callback) (HTIMER, BaseProtocol*);
typedef void (*Event_Callback)(BaseProtocol*, void*);
typedef void (*Signal_Callback)(BaseProtocol*, unsigned long long);
typedef void (*Unbind_Callback)(HSOCKET, BaseProtocol*, BaseProtocol*, void*);
typedef void (*Rebind_Callback)(HSOCKET, BaseProtocol*, void*);

#ifdef __WINDOWS__
typedef struct Socket_Send_Content {
	OVERLAPPED	overlapped;
	WSABUF		databuf;
	char		event_type;	//投递类型accept, conect, read, write
}*HSENDBUFF;
#define SEND_CTX_SIZE sizeof(Socket_Send_Content)

typedef struct Socket_Content {
	union {
		struct {
			OVERLAPPED	overlapped;
			WSABUF		databuf;
		};
		struct {
			BaseProtocol*	rebind_proto;
			Unbind_Callback unbind_call;
			Rebind_Callback rebind_call;
			void*			call_data;
		};
	};
	char	event_type;	//投递类型accept, conect, read, write

	CONN_TYPE				conn_type;
	struct sockaddr_in6		peer_addr;

	char*	recv_buf;
	int		offset;
	int		size;
	
	SOCKET			fd;
	BaseProtocol*	proto;
	void*			sock_data;
	void*			user_data;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content {
	CONN_TYPE		conn_type;
	char			once;
	char			close;
	char			lock;
	BaseProtocol*	proto;
	Timer_Callback	call;
	HANDLE			timer;
	HANDLE			completion_port;
}*HTIMER;
#define TIMER_CTX_SIZE sizeof(Timer_Content)

typedef struct Event_Content {
	CONN_TYPE		conn_type;
	BaseProtocol*	proto;
	Event_Callback	call;
	void*			event_data;
}*HEVENT;
#define EVENT_CTX_SIZE sizeof(Event_Content)

typedef struct Signal_Content {
	CONN_TYPE			conn_type;
	BaseProtocol*		proto;
	Signal_Callback		call;
	unsigned long long	signal;
}*HSIGNAL;
#define SIGNAL_CTX_SIZE sizeof(Signal_Content)

#else

typedef struct Socket_Content {
	CONN_TYPE			conn_type;
	unsigned char		_conn_stat:7;
	unsigned char		_flag :1;
	unsigned char		_send_lock;
	struct sockaddr_in6	peer_addr;
	
	BaseProtocol*	rebind_proto;
	Unbind_Callback	unbind_call;
	Rebind_Callback	rebind_call;
	void*			call_data;

	char*	recv_buf;
	int		recv_size;
	int		offset;

	char*	write_buf;
	int		write_size;
	int		write_offset;

	int	fd;
	int	epoll_fd;
	BaseProtocol*	proto;
	void*			sock_data;
	void*			user_data;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content{
	CONN_TYPE		conn_type;
	unsigned char	_conn_stat;
	char			once;
	int				fd;
	int				epoll_fd;
	BaseProtocol*	proto;
	Timer_Callback	call;
}TIMER_CTX, *HTIMER;

typedef struct Event_Content{
	CONN_TYPE		conn_type;
	int				fd;
	int 			epoll_fd;
	BaseProtocol*	proto;
	Event_Callback	call;
	void*			event_data;
}EVEVT_CTX, *HEVENT;

typedef struct Signal_Content{
	CONN_TYPE		conn_type;
	int				fd;
	int 			epoll_fd;
	BaseProtocol*	proto;
	Signal_Callback	call;
	unsigned long long signal;
}SIGNAL_CTX, *HSIGNAL;
#endif // __WINDOWS__

#ifdef __cplusplus
extern "C"
{
#endif

	Reactor_API	ThreadStat* __STDCALL ThreadDistribution(BaseProtocol* proto);
	Reactor_API ThreadStat* __STDCALL ThreadDistributionIndex(BaseProtocol* proto, int index);
	Reactor_API void		__STDCALL ThreadUnDistribution(BaseProtocol* proto);

	Reactor_API int		__STDCALL	ReactorStart();
	Reactor_API int		__STDCALL	AccepterRun(BaseAccepter* accepter);
	Reactor_API int		__STDCALL	AccepterStop(BaseAccepter* accepter);

	Reactor_API HSOCKET __STDCALL	HsocketListenUDP(BaseProtocol* proto, int port);
	Reactor_API HSOCKET	__STDCALL	HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE iotype);
	Reactor_API bool	__STDCALL	HsocketSend(HSOCKET hsock, const char* data, int len);
	Reactor_API bool	__STDCALL	HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len);
	Reactor_API void	__STDCALL	HsocketClose(HSOCKET hsock);
	Reactor_API void 	__STDCALL	HsocketClosed(HSOCKET hsock);
	Reactor_API int		__STDCALL	HsocketPopBuf(HSOCKET hsock, int len);

	Reactor_API void	__STDCALL	HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port);
	Reactor_API void	__STDCALL	HsocketPeerAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port);
	Reactor_API void	__STDCALL	HsocketLocalAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port);

	Reactor_API	HTIMER	__STDCALL	TimerCreate(BaseProtocol* proto, int duetime, int looptime, Timer_Callback callback);
	Reactor_API void 	__STDCALL	TimerDelete(HTIMER hsock);
	Reactor_API void	__STDCALL	PostEvent(BaseProtocol* proto, Event_Callback callback, void* event_data);
	Reactor_API void	__STDCALL	PostSignal(BaseProtocol* proto, Signal_Callback callback, unsigned long long signal);

	Reactor_API void	__STDCALL HsocketUnbindProtocol(HSOCKET hsock, BaseProtocol* proto, Unbind_Callback ucall, Rebind_Callback rcall, void* call_data);
	Reactor_API void	__STDCALL HsocketRebindProtocol(HSOCKET hsock, BaseProtocol* proto, Rebind_Callback call, void* call_data);
	Reactor_API int		__STDCALL GetHostByName(const char* name, char* buf, size_t size);

#ifdef OPENSSL_SUPPORT
	Reactor_API bool __STDCALL HsocketSSLCreate(HSOCKET hsock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key);
#endif

#ifdef KCP_SUPPORT
	Reactor_API int		__STDCALL HsocketKcpCreate(HSOCKET hsock, int conv, int mode);
	Reactor_API void	__STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc);
	Reactor_API void	__STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd);
	Reactor_API int		__STDCALL HsocketKcpGetconv(HSOCKET hsock);
	Reactor_API void	__STDCALL HsocketKcpUpdate(HSOCKET hsock);
	Reactor_API int		__STDCALL HsocketKcpDebug(HSOCKET hsock, char* buf, int size);
#endif

#ifdef __cplusplus
}
#endif

class BaseProtocol{
public:
	ThreadStat*		thread_stat = NULL;
	PROTOCOL_TPYE	protocol_type = UNKNOWN_PROTOCOL;
	long			socket_count = 0;

public:
	BaseProtocol() { 
	};
	virtual ~BaseProtocol() { ThreadUnDistribution(this); };
	virtual void _free() { delete this; };
	void	Set(PROTOCOL_TPYE prototype) {this->protocol_type = prototype; }
	void	ThreadSet() { ThreadDistribution(this); }
	void	ThreadSet(int index) { ThreadDistributionIndex(this, index); }
	void	ThreadUnset() { ThreadUnDistribution(this); }

public:
	virtual void ConnectionMade(HSOCKET hsock, CONN_TYPE type) = 0;
	virtual void ConnectionFailed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionClosed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionRecved(HSOCKET hsock, const char* data, int len) = 0;
};

class BaseAccepter{
public:
	BaseAccepter() {};
	virtual ~BaseAccepter() {};
	int Listen(const char* addr, unsigned short listenport) {
		snprintf(this->ServerAddr, sizeof(this->ServerAddr), "%s", addr);
		this->ServerPort = listenport;
		return AccepterRun(this);
	};
	void Stop() { AccepterStop(this); };

public:
	char			ServerAddr[40] = { 0x0 };
	unsigned short	ServerPort = 0;
	bool			Listening = false;
#ifdef __WINDOWS__
	SOCKET		Listenfd = NULL;
#else
	int			Listenfd = 0;
#endif // __WINDOWS__
	virtual bool	Init() = 0;
	virtual void	TimeOut() = 0;
	virtual BaseProtocol*	ProtocolCreate() = 0;
};

#endif // !_REACTOR_H_
