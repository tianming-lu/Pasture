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
#define LONGLOCK(a)  while (InterlockedExchange(a, 1)){Sleep(0);}
#define LONGUNLOCK(a)	InterlockedExchange(a, 0)
#define LONGTRYLOCK(a)	!InterlockedExchange(a, 1)
#else
#define LONGLOCK(a)	while (__sync_fetch_and_or(a, 1)){sleep(0);}
#define LONGUNLOCK(a)	__sync_fetch_and_and(a, 0)
#define LONGTRYLOCK(a)	!__sync_fetch_and_or(a, 1)
#endif

enum CONN_TYPE:char {
	TCP_CONN = 0,
	UDP_CONN = 0x01,
	SSL_CONN = 0x02,
	KCP_CONN = 0x04,
	HTTP_CONN = 0x06,
	WS_CONN = 0x08,
	ITMER,
};

enum SOCKET_STAT:char{
	SOCKET_CONNECTING = 0,
	SOCKET_CONNECTED,
	SOCKET_CLOSEING,
	SOCKET_CLOSED
};

enum PROTOCOL_TPYE:char {
	CLIENT_PROTOCOL = 0,
	SERVER_PROTOCOL = 1,
	AUTO_PROTOCOL = 2
};

class BaseFactory;
class BaseProtocol;

#ifdef __WINDOWS__
typedef struct _IOCP_SOCKET* HSOCKET;
#else
typedef struct _EPOLL_SOCKET* HSOCKET;
#endif

typedef void (*Timer_Callback) (HSOCKET, BaseProtocol*);

#ifdef __WINDOWS__
typedef struct _IOCP_BUFF
{
	OVERLAPPED	overlapped;
	WSABUF		databuf;
	int32_t		offset;
	int32_t		size;
	BYTE		type;
	SOCKET		fd;
	DWORD		flags;
	struct _IOCP_SOCKET* hsock;
}IOCP_BUFF, *HNETBUFF;
#else
typedef struct _EPOLL_BUFF
{
	char* buff;
	int offset;
	int size;
	uint8_t lock_flag;
}EPOLL_BUFF, * HNETBUFF;
#endif

#ifdef __WINDOWS__
typedef struct _IOCP_SOCKET
{
	SOCKET			fd;
#else
typedef struct _EPOLL_SOCKET
{
	int				fd;
#endif
	struct sockaddr_in6		peer_addr;
	BaseFactory*			factory;
	BaseProtocol*			_user;
	void*					_user_data;
	CONN_TYPE				_conn_type;
#ifdef __WINDOWS__
	char*			recv_buf;
	IOCP_BUFF*		_IocpBuff;
}IOCP_SOCKET, * HSOCKET;
#else
	uint8_t			_stat;
	EPOLL_BUFF		_recv_buf;
	EPOLL_BUFF		_send_buf;
	//timer
	Timer_Callback  _callback;
}EPOLL_SOCKET, * HSOCKET;
#endif // __WINDOWS__

class Reactor
{
public:
	Reactor() {};
	~Reactor() {};

public:
	bool	Run = true;
	int		CPU_COUNT = 1;
#ifdef __WINDOWS__
	HANDLE	ComPort = NULL;
#else
	int		eprfd = 0;
	int		epwfd = 0;
	int		eptfd = 0;
	int 	maxevent = 64;
#endif
	std::map<uint16_t, BaseFactory*>	FactoryAll;
};

class BaseProtocol
{
public:
	BaseProtocol() { 
		this->protoType = SERVER_PROTOCOL;
#ifdef __WINDOWS__
		this->mutex = CreateMutexA(NULL, false, NULL);
#else
		this->mutex = new(std::nothrow) std::mutex();
#endif
	};
	virtual ~BaseProtocol() {
		
#ifdef __WINDOWS__
		if (this->mutex) CloseHandle(this->mutex);
#else
		if (this->mutex) delete this->mutex;
#endif
	};
	void SetFactory(BaseFactory* pfc, PROTOCOL_TPYE prototype) { this->factory = pfc; this->protoType = prototype; };
	void SetNoLock() {
#ifdef __WINDOWS__
		if (this->mutex) { CloseHandle(this->mutex); this->mutex = NULL; }
#else
		if (this->mutex) {delete this->mutex; this->mutex = NULL;}
#endif
	}
	void Lock() { 
#ifdef __WINDOWS__
		if (this->mutex) WaitForSingleObject(this->mutex, INFINITE);
#else
		if (this->mutex) this->mutex->lock();
#endif
	};
	bool TryLock(){
#ifdef __WINDOWS__
		if (this->mutex) return WaitForSingleObject(this->mutex, 0) == WAIT_OBJECT_0 ? true : false;
#else
		if (this->mutex) return this->mutex->try_lock();
#endif
		return true;
	}
	void UnLock() {
#ifdef __WINDOWS__
		if (this->mutex) ReleaseMutex(this->mutex);
#else
		if (this->mutex) this->mutex->unlock();
#endif
	};

public:
	BaseFactory*	factory = NULL;
#ifdef __WINDOWS__
	HANDLE			mutex = NULL;
#else
	std::mutex*		mutex = NULL;
#endif // __WINDOWS__
	PROTOCOL_TPYE	protoType = SERVER_PROTOCOL;
	long			sockCount = 0;

public:
	virtual void ConnectionMade(HSOCKET hsock, CONN_TYPE type) = 0;
	virtual void ConnectionFailed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionClosed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionRecved(HSOCKET hsock, const char* data, int len) = 0;
};

class BaseFactory
{
public:
	BaseFactory() {};
	virtual ~BaseFactory() {};
	void Set(Reactor* rec, const char* addr, uint16_t listenport) { this->reactor = rec, snprintf(this->ServerAddr, sizeof(this->ServerAddr), addr); this->ServerPort = listenport; };

public:
	Reactor*	reactor = NULL;
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

class AutoProtocol :
	public BaseProtocol
{
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

	Reactor_API int		__STDCALL	ReactorStart(Reactor* reactor);
	Reactor_API void	__STDCALL	ReactorStop(Reactor* reactor);
	Reactor_API int		__STDCALL	FactoryRun(BaseFactory* fc);
	Reactor_API int		__STDCALL	FactoryStop(BaseFactory* fc);
	Reactor_API HSOCKET __STDCALL	HsocketListenUDP(BaseProtocol* proto, int port);
	Reactor_API HSOCKET	__STDCALL	HsocketConnect(BaseProtocol* proto, const char* ip, int port, CONN_TYPE iotype);
	Reactor_API bool	__STDCALL	HsocketSend(HSOCKET hsock, const char* data, int len);
	Reactor_API bool	__STDCALL	HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len);
	Reactor_API bool	__STDCALL	HsocketClose(HSOCKET hsock);
	Reactor_API void 	__STDCALL	HsocketClosed(HSOCKET hsock);
	Reactor_API int		__STDCALL	HsocketPopBuf(HSOCKET hsock, int len);

	Reactor_API	HNETBUFF	__STDCALL	HsocketGetBuff();
	Reactor_API	bool		__STDCALL	HsocketSetBuff(HNETBUFF netbuff, const char* data, int len);
	Reactor_API	bool		__STDCALL	HsocketSendBuff(HSOCKET hsock, HNETBUFF netbuff);

	Reactor_API void	__STDCALL	HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port);
	Reactor_API void	__STDCALL	HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz);
	Reactor_API int		__STDCALL	HsocketPeerPort(HSOCKET hsock);
	Reactor_API void	__STDCALL	HsocketLocalIP(HSOCKET hsock, char* ip, size_t ipsz);
	Reactor_API int		__STDCALL	HsocketLocalPort(HSOCKET hsock);

	Reactor_API	HSOCKET	__STDCALL	TimerCreate(BaseProtocol* proto, int duetime, int looptime, Timer_Callback callback);
	Reactor_API void 	__STDCALL	TimerDelete(HSOCKET hsock);

	Reactor_API BaseProtocol*	__STDCALL HsocketBindUser(HSOCKET hsock, BaseProtocol* proto);
	Reactor_API int				__STDCALL	GetHostByName(const char* name, char* buf, size_t size);

#ifdef OPENSSL_SUPPORT
	Reactor_API bool __STDCALL HsocketUptoSSL(HSOCKET hsock, int openssl_type, const char* ca_crt, const char* user_crt, const char* pri_key);
#endif

#ifdef KCP_SUPPORT
	Reactor_API int		__STDCALL HsocketKcpCreate(HSOCKET hsock, int conv, int mode);
	Reactor_API void	__STDCALL HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc);
	Reactor_API void	__STDCALL HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd);
	Reactor_API int		__STDCALL HsocketKcpGetconv(HSOCKET hsock);
	Reactor_API void	__STDCALL HsocketKcpEnable(HSOCKET hsock, char enable);
	Reactor_API void	__STDCALL HsocketKcpUpdate(HSOCKET hsock);
	Reactor_API int		__STDCALL HsocketKcpDebug(HSOCKET hsock, char* buf, int size);
#endif

#ifdef __cplusplus
}
#endif

#endif // !_REACTOR_H_
