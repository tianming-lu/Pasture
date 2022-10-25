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
#define OPENSSL_SERVER 0
#define OPENSSL_CLIENT 1

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

#include <map>
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
#define LONGLOCK(a)  while (InterlockedExchange(&a, 1)){Sleep(0);}
#define LONGUNLOCK(a)	InterlockedExchange(&a, 0)
#define LONGTRYLOCK(a)	!InterlockedExchange(&a, 1)
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
	CLIENT_PROTOCOL = 0,
	SERVER_PROTOCOL = 1,
	AUTO_PROTOCOL = 2
};

typedef struct {
	long	ProtocolCount;
#ifdef __WINDOWS__
	HANDLE	CompletionPort;
#else
	int		epoll_fd;
#endif
}ThreadStat;

class BaseFactory;
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
			BaseProtocol* rebind_user;
			union{
				Unbind_Callback unbind_call;
				Rebind_Callback rebind_call;
			};
			void* call_data;
		};
	};
	char		event_type;	//投递类型accept, conect, read, write

	CONN_TYPE	conn_type;
	DWORD		flag;

	char*		recv_buf;
	int			offset;
	int			size;

	int						fromlen;
	struct sockaddr_in6		peer_addr;
	BaseProtocol*			user;
	void*					user_data;	
	SOCKET					fd;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content {
	CONN_TYPE		conn_type;
	BaseProtocol*	user;
	Timer_Callback	call;
	HANDLE			timer;
	ThreadStat*		thread_stat;
	long			lock;
	long			close;
}*HTIMER;
#define TIMER_CTX_SIZE sizeof(Timer_Content)

typedef struct Event_Content {
	CONN_TYPE		conn_type;
	BaseProtocol*	user;
	Event_Callback	call;
	void*			event_data;
}*HEVENT;
#define EVENT_CTX_SIZE sizeof(Event_Content)

typedef struct Signal_Content {
	CONN_TYPE			conn_type;
	BaseProtocol*		user;
	Signal_Callback		call;
	unsigned long long	signal;
}*HSIGNAL;
#define SIGNAL_CTX_SIZE sizeof(Signal_Content)

#else

typedef struct Socket_Content {
	CONN_TYPE				conn_type;
	unsigned char			_conn_stat:7;
	unsigned char			_flag :1;
	unsigned char			_send_lock;
	struct sockaddr_in6		peer_addr;
	
	BaseProtocol* 			rebind_user;
	union{
		Unbind_Callback		unbind_call;
		Rebind_Callback		rebind_call;
	};
	void*					call_data;

	BaseProtocol*			user;
	void*					user_data;

	int				fd;
	int 			epoll_fd;

	char*			recv_buf;
	int				recv_size;
	int				offset;

	char*			write_buf;
	int				write_size;
	int				write_offset;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content{
	CONN_TYPE	conn_type;
	unsigned char _conn_stat;
	int	fd;
	int epoll_fd;
	BaseProtocol* user;
	Timer_Callback call;
}TIMER_CTX, *HTIMER;

typedef struct Event_Content{
	CONN_TYPE	conn_type;
	int			fd;
	int 		epoll_fd;
	BaseProtocol* user;
	Event_Callback call;
	void* 	event_data;
}EVEVT_CTX, *HEVENT;

typedef struct Signal_Content{
	CONN_TYPE	conn_type;
	int			fd;
	int 		epoll_fd;
	BaseProtocol* user;
	Signal_Callback call;
	unsigned long long signal;
}SIGNAL_CTX, *HSIGNAL;
#endif // __WINDOWS__

Reactor_API	ThreadStat* __STDCALL ThreadDistribution(BaseProtocol* proto);
Reactor_API void		__STDCALL ThreadUnDistribution(BaseProtocol* proto);

class BaseProtocol{
public:
	BaseFactory*	factory = NULL;
	ThreadStat*		thread_stat = NULL;
	PROTOCOL_TPYE	protoType = SERVER_PROTOCOL;
	long			sockCount = 0;

public:
	BaseProtocol() { 
		this->protoType = SERVER_PROTOCOL;
	};
	virtual ~BaseProtocol() {};
	void	SetFactory(BaseFactory* pfc, PROTOCOL_TPYE prototype) { this->factory = pfc; this->protoType = prototype; }
	void	ThreadSet() { ThreadDistribution(this); }
	void	ThreadUnset() { ThreadUnDistribution(this); }

public:
	virtual void ConnectionMade(HSOCKET hsock, CONN_TYPE type) = 0;
	virtual void ConnectionFailed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionClosed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionRecved(HSOCKET hsock, const char* data, int len) = 0;
};

class BaseFactory{
public:
	BaseFactory() {};
	virtual ~BaseFactory() {};
	void Set(const char* addr, uint16_t listenport) {snprintf(this->ServerAddr, sizeof(this->ServerAddr), addr); this->ServerPort = listenport; };

public:
	char		ServerAddr[40] = { 0x0 };
	uint16_t	ServerPort = 0;
#ifdef __WINDOWS__
	SOCKET		Listenfd = NULL;
#else
	int			Listenfd = 0;
#endif // __WINDOWS__
	virtual bool	FactoryInit() = 0;
	virtual void	FactoryInited() = 0;
	virtual void	FactoryLoop() = 0;
	virtual void	FactoryClose() = 0;
	virtual BaseProtocol* CreateProtocol() = 0;
	virtual void	DeleteProtocol(BaseProtocol* proto) = 0;
};

typedef void (*autofree)(BaseProtocol* proto);

class AutoProtocol: public BaseProtocol{
public:
	char* buf = NULL;
	int buflen = 0;
	autofree freefunc = NULL;
	AutoProtocol() {};
	virtual ~AutoProtocol() { if (buf) free(buf); };
	virtual void ConnectionMade(HSOCKET hsock) = 0;
	virtual void ConnectionFailed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionClosed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionRecved(HSOCKET hsock, const char* data, int len) = 0;
};

#ifdef __cplusplus
extern "C"
{
#endif

	Reactor_API int		__STDCALL	ReactorStart();
	Reactor_API int		__STDCALL	FactoryRun(BaseFactory* fc);
	Reactor_API int		__STDCALL	FactoryStop(BaseFactory* fc);
	Reactor_API HSOCKET __STDCALL	HsocketListenUDP(BaseProtocol* proto, int port);
	Reactor_API HSOCKET	__STDCALL	HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE iotype);
	Reactor_API bool	__STDCALL	HsocketSend(HSOCKET hsock, const char* data, int len);
	Reactor_API bool	__STDCALL	HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len);
	Reactor_API void	__STDCALL	HsocketClose(HSOCKET hsock);
	Reactor_API void 	__STDCALL	HsocketClosed(HSOCKET hsock);
	Reactor_API int		__STDCALL	HsocketPopBuf(HSOCKET hsock, int len);

	Reactor_API void	__STDCALL	HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port);
	Reactor_API void	__STDCALL	HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz);
	Reactor_API int		__STDCALL	HsocketPeerPort(HSOCKET hsock);
	Reactor_API void	__STDCALL	HsocketLocalIP(HSOCKET hsock, char* ip, size_t ipsz);
	Reactor_API int		__STDCALL	HsocketLocalPort(HSOCKET hsock);

	Reactor_API	HTIMER	__STDCALL	TimerCreate(BaseProtocol* proto, int duetime, int looptime, Timer_Callback callback);
	Reactor_API void 	__STDCALL	TimerDelete(HTIMER hsock);
	Reactor_API void	__STDCALL	PostEvent(BaseProtocol* proto, Event_Callback callback, void* event_data);
	Reactor_API void	__STDCALL	PostSignal(BaseProtocol* proto, Signal_Callback callback, unsigned long long signal);

	Reactor_API void	__STDCALL HsocketUnbindUser(HSOCKET hsock, BaseProtocol* proto, Unbind_Callback call, void* call_data);
	Reactor_API void	__STDCALL HsocketRebindUser(HSOCKET hsock, BaseProtocol* proto, Rebind_Callback call, void* call_data);
	Reactor_API int		__STDCALL GetHostByName(const char* name, char* buf, size_t size);

#ifdef OPENSSL_SUPPORT
	Reactor_API bool __STDCALL HsocketSSLCreate(HSOCKET hsock, int openssl_type, const char* ca_crt, const char* user_crt, const char* pri_key);
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

#endif // !_REACTOR_H_
