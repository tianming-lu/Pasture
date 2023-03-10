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

/*打开此宏定义，支持SSL安全连接，需要依赖openssl*/
//#define OPENSSL_SUPPORT
#ifdef OPENSSL_SUPPORT
#define SSL_SERVER 0
#define SSL_CLIENT 1
#endif

/*打开此宏定义，支持KCP连接，需要依赖 https://github.com/skywind3000/kcp */
//#define KCP_SUPPORT

/*HSOCKET 对应套接字进行面向对象封装 */
//#define SOCKET_OOP_FLAG

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
#include <functional>

#ifdef __WINDOWS__
#define ATOMIC_LOCK(a)  while (InterlockedExchange8(&a, 1)){Sleep(0);}
#define ATOMIC_UNLOCK(a)	InterlockedExchange8(&a, 0)
#define ATOMIC_TRYLOCK(a)	!InterlockedExchange8(&a, 1)
#else
#define ATOMIC_LOCK(a)	while (__sync_fetch_and_or(&a, 1)){sleep(0);}
#define ATOMIC_UNLOCK(a)	__sync_fetch_and_and(&a, 0)
#define ATOMIC_TRYLOCK(a)	!__sync_fetch_and_or(&a, 1)
#endif

enum PROTOCOL:char {
	TCP_PROTOCOL = 0,
	UDP_PROTOCOL = 0x01,
#ifdef OPENSSL_SUPPORT
	SSL_PROTOCOL = 0x02,
#endif
#ifdef KCP_SUPPORT
	KCP_PROTOCOL = 0x04,
#endif
	HTTP_PROTOCOL = 0x06,
	WEBSOCKET_PROTOCOL = 0x08,
	TIMER,
	EVENT,
	SIGNAL
};

extern int ActorThreadWorker;

class BaseAccepter;
class BaseWorker;

typedef struct Thread_Content ThreadStat;
typedef struct Timer_Content* HTIMER;
typedef struct Socket_Content* HSOCKET;
typedef void (*Timer_Callback) (HTIMER, BaseWorker*, void*);
typedef void (*Event_Callback)(BaseWorker*, void*);
typedef void (*Signal_Callback)(BaseWorker*, long long);
typedef void (*WorkerBind_Callback)(HSOCKET, BaseWorker*, void*);

#ifdef __cplusplus
extern "C"
{
#endif

	Reactor_API	ThreadStat* __STDCALL ThreadDistribution(BaseWorker* worker);
	Reactor_API ThreadStat* __STDCALL ThreadDistributionIndex(BaseWorker* worker, int index);
	Reactor_API void		__STDCALL ThreadUnDistribution(BaseWorker* worker);

	Reactor_API int		__STDCALL	ReactorStart(int thread_count);
	Reactor_API int		__STDCALL	AccepterRun(BaseAccepter* accepter);
	Reactor_API int		__STDCALL	AccepterStop(BaseAccepter* accepter);

	Reactor_API HSOCKET __STDCALL	HsocketListenUDP(BaseWorker* worker, const char* ip, int port);
	Reactor_API HSOCKET	__STDCALL	HsocketConnect(BaseWorker* worker, const char* ip, int port, PROTOCOL protocol);
	Reactor_API bool	__STDCALL	HsocketSend(HSOCKET hsock, const char* data, int len);
	Reactor_API bool	__STDCALL	HsocketSendTo(HSOCKET hsock, const char* ip, int port, const char* data, int len);
	Reactor_API void	__STDCALL	HsocketClose(HSOCKET hsock);
	Reactor_API void 	__STDCALL	HsocketClosed(HSOCKET hsock);
	Reactor_API int		__STDCALL	HsocketPopBuf(HSOCKET hsock, int len);

	Reactor_API void	__STDCALL	HsocketPeerAddrSet(HSOCKET hsock, const char* ip, int port);
	Reactor_API void	__STDCALL	HsocketPeerAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port);
	Reactor_API void	__STDCALL	HsocketLocalAddr(HSOCKET hsock, char* ip, size_t ipsz, int* port);

	Reactor_API	HTIMER	__STDCALL	TimerCreate(BaseWorker* worker, void* user_data, int duetime, int looptime, Timer_Callback callback);
	Reactor_API void 	__STDCALL	TimerDelete(HTIMER timer);
	Reactor_API void	__STDCALL	PostEvent(BaseWorker* worker, void* event_data, Event_Callback callback);
	Reactor_API void	__STDCALL	PostSignal(BaseWorker* worker, long long signal, Signal_Callback callback);

	Reactor_API void	__STDCALL	HsocketUnbindWorker(HSOCKET hsock, void* usr_data, WorkerBind_Callback ucall);
	Reactor_API void	__STDCALL	HsocketRebindWorker(HSOCKET hsock, BaseWorker* worker, void* user_data, WorkerBind_Callback call);
	Reactor_API int		__STDCALL	GetHostByName(const char* name, char* buf, size_t size);

#ifdef OPENSSL_SUPPORT
	Reactor_API bool __STDCALL	HsocketSSLCreate(HSOCKET hsock, int openssl_type, int verify, const char* ca_crt, const char* user_crt, const char* pri_key);
#endif

#ifdef KCP_SUPPORT
	Reactor_API int		__STDCALL	HsocketKcpCreate(HSOCKET hsock, int conv, int mode);
	Reactor_API void	__STDCALL	HsocketKcpNodelay(HSOCKET hsock, int nodelay, int interval, int resend, int nc);
	Reactor_API void	__STDCALL	HsocketKcpWndsize(HSOCKET hsock, int sndwnd, int rcvwnd);
	Reactor_API int		__STDCALL	HsocketKcpGetconv(HSOCKET hsock);
	Reactor_API void	__STDCALL	HsocketKcpUpdate(HSOCKET hsock);
	Reactor_API int		__STDCALL	HsocketKcpDebug(HSOCKET hsock, char* buf, int size);
#endif

#ifdef __cplusplus
}
#endif

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
			BaseWorker*	rebind_worker;
			WorkerBind_Callback bind_call;
			void*			call_data;
		};
	};
	char	event_type;	//投递类型accept, conect, read, write

	PROTOCOL				protocol;
	struct sockaddr_in6		peer_addr;

	char*	recv_buf;
	int		offset;
	int		size;
	
	SOCKET			fd;
	BaseWorker*		worker;
	void*			sock_data;
	void*			user_data;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content {
	PROTOCOL		protocol;
	char			once;
	char			closed;
	char			lock;
	HANDLE			timer;
	HANDLE			completion_port;
	Timer_Callback	call;
	BaseWorker*		worker;
	void*			user_data;
}*HTIMER;
#define TIMER_CTX_SIZE sizeof(Timer_Content)

typedef struct Event_Content {
	PROTOCOL		protocol;
	Event_Callback	call;
	BaseWorker*		worker;
	void*			event_data;
}*HEVENT;
#define EVENT_CTX_SIZE sizeof(Event_Content)

typedef struct Signal_Content {
	PROTOCOL			protocol;
	Signal_Callback		call;
	BaseWorker*			worker;
	long long			signal;
}*HSIGNAL;
#define SIGNAL_CTX_SIZE sizeof(Signal_Content)

#else

typedef struct Socket_Content {
	PROTOCOL			protocol;
	unsigned char		_conn_stat:7;
	unsigned char		_flag :1;
	unsigned char		_send_lock;
	struct sockaddr_in6	peer_addr;
	
	BaseWorker*	rebind_worker;
	WorkerBind_Callback	bind_call;
	void*			call_data;

	char*	recv_buf;
	int		recv_size;
	int		offset;

	char*	write_buf;
	int		write_size;
	int		write_offset;

	int	fd;
	int	epoll_fd;
	BaseWorker*		worker;
	void*			sock_data;
	void*			user_data;
}*HSOCKET;
#define SOCKET_CTX_SIZE sizeof(Socket_Content)

typedef struct Timer_Content{
	PROTOCOL		protocol;
	unsigned char	_conn_stat;
	char			once;
	int				fd;
	int				epoll_fd;
	Timer_Callback	call;
	BaseWorker*		worker;
	void*			user_data;
}TIMER_CTX, *HTIMER;

typedef struct Event_Content{
	PROTOCOL		protocol;
	int				fd;
	int 			epoll_fd;
	Event_Callback	call;
	BaseWorker*		worker;
	void*			event_data;
}EVEVT_CTX, *HEVENT;

typedef struct Signal_Content{
	PROTOCOL		protocol;
	int				fd;
	int 			epoll_fd;
	Signal_Callback	call;
	BaseWorker*		worker;
	long long		signal;
}SIGNAL_CTX, *HSIGNAL;
#endif // __WINDOWS__

/*这个是个对 HTIMER 的class封装，给定时器操作提供面向对象的操作方式，可以使用lambda表达式,定时随对象析构而关闭*/
class Timer {
private:
	HTIMER timer_handle;
	static void timer_call_back(HTIMER timer, BaseWorker* worker, void* user_data) {
		(*(std::function<void()>*)user_data)();
	}
public:
	Timer() { timer_handle = NULL; }
	~Timer() { close(); }
	bool create(BaseWorker* worker, int duetime, int looptime, std::function<void()> func) {
		std::function<void()>* ptr = new std::function<void()>(func);
		timer_handle = TimerCreate(worker, ptr, duetime, looptime, timer_call_back);
		return timer_handle;
	}
	void close() {
		if (timer_handle) {
			std::function<void()>* ptr = (std::function<void()>*)timer_handle->user_data;
			delete ptr;
			TimerDelete(timer_handle);
			timer_handle = NULL;
		}
	}
};

#ifdef SOCKET_OOP_FLAG
/*这个是个对 HSOCKET 的class封装，给套接字操作提供面向对象的操作方式,有点脱裤子放屁得感觉，用户仍然需要小心管理连接关闭以防止野指针*/
class Socket {
private:
	HSOCKET hsock_ptr;
public:
	void operator=(const HSOCKET hsock) { hsock_ptr = hsock; }
	void operator=(const Socket& conn) { hsock_ptr = conn.hsock_ptr; }
	bool operator==(const HSOCKET hsock) const { return hsock_ptr == hsock; }
	bool operator==(const Socket& conn) const { return hsock_ptr == conn.hsock_ptr; }
	Socket() { hsock_ptr = NULL; }
	Socket(HSOCKET hsock) { hsock_ptr = hsock; }
	bool	connect(BaseWorker* worker, const char* ip, int port, PROTOCOL protocol) { hsock_ptr = HsocketConnect(worker, ip, port, protocol); return hsock_ptr;}
	bool	udplisten(BaseWorker* worker, const char* ip, int port) { hsock_ptr = HsocketListenUDP(worker, ip, port); return hsock_ptr;}
	bool	send(const char* data, int len) const {return HsocketSend(hsock_ptr, data, len);}
	bool	sendto(const char* ip, int port, const char* data, int len) const { return HsocketSendTo(hsock_ptr, ip, port, data, len); }
	void	close() { HsocketClose(hsock_ptr); hsock_ptr = NULL;}
	void	closed() {HsocketClosed(hsock_ptr); hsock_ptr = NULL; }
	int		popbuf(int len) const { return HsocketPopBuf(hsock_ptr, len); }

	void	set_peer_addr(const char* ip, int port) const { HsocketPeerAddrSet(hsock_ptr, ip, port); }
	void	get_peer_addr(char* ip, size_t ipsz, int* port) const { HsocketPeerAddr(hsock_ptr, ip, ipsz, port); }
	void	get_local_addr(char* ip, size_t ipsz, int* port) const { HsocketLocalAddr(hsock_ptr, ip, ipsz, port); }
	void	unbind_worker(void* user_data, Unbind_Callback ucall) const { HsocketUnbindWorker(hsock_ptr, user_data, ucall); }
	void	rebind_worker(BaseWorker* worker, void* user_data, Rebind_Callback call) const { HsocketRebindWorker(hsock_ptr, worker, user_data, call); }
};
#endif

#define FREE_DEF  0
#define FREE_AUTO 1
#define FREE_USER 2
/*BaseWorker：可以处理一个或多个连接的网络事件，多个相关的连接绑定到同一个BaseWorker实现上下文的线程安全，这是此网络库的重要特性*/
class BaseWorker{
private:
	static void event_call_back(BaseWorker* worker, void* user_data) {
		std::function<void()>* ptr = (std::function<void()>*)user_data;
		(*ptr)();
		delete ptr;
	}
public:
	char	auto_free_flag = FREE_DEF;
	char	thread_id = -1;
	int		ref_count = 0;

	BaseWorker() { 
	};
	virtual ~BaseWorker() { ThreadUnDistribution(this); };

	/*以下函数都是比较重要且必须的函数实现*/

	/*释放资源，即使是子类也能安全释放*/
	virtual void _free() { delete this; };
	/*是否由网络库自动释放*/
	void	auto_release(bool flag) { auto_free_flag = flag ? FREE_AUTO : FREE_USER; }
	/*检查对象是否可释放*/
	void	check_release() {if (!ref_count && auto_free_flag == FREE_AUTO) {_free();}}
	/*获取网络库工作线程id*/
	int		thread_get() { return thread_id; }
	/*分配网络库工作线程id，由内部负载均衡指定*/
	void	thread_set() { ThreadDistribution(this); }
	/*分配网络库工作线程id，由用户指定*/
	void	thread_set(int index) { ThreadDistributionIndex(this, index); }
	/*解绑网络库工作线程id，如果不再占用网络线程*/
	void	thread_unset() { ThreadUnDistribution(this); }
	/*向worker的工作线程指定一项任务*/
	void	task(std::function<void()> func) {
		std::function<void()>* ptr = new std::function<void()>(func);
		PostEvent(this, ptr, event_call_back);
	}

	/*以下是处理套接字网络事件的虚函数,子类中必须有对应的实现*/

#ifndef SOCKET_OOP_FLAG
	virtual void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) = 0;
	virtual void ConnectionFailed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionClosed(HSOCKET hsock, int err) = 0;
	virtual void ConnectionRecved(HSOCKET hsock, const char* data, int len) = 0;
#else
	virtual void ConnectionMade(Socket sock, PROTOCOL protocol) = 0;
	virtual void ConnectionFailed(Socket sock, int err) = 0;
	virtual void ConnectionClosed(Socket sock, int err) = 0;
	virtual void ConnectionRecved(Socket sock, const char* data, int len) = 0;
#endif
};
#define BASEPORTOCOL_SIZE sizeof(BaseWorker)

/*
	BaseAccepter：TCP监听类
	listen("::", 8080)  开始监听本地[::]:8080端口	
	stop()	停止监听
*/
class BaseAccepter{
public:
	char			ServerAddr[40] = { 0x0 };
	unsigned short	ServerPort = 0;
	bool			Listening = false;
#ifdef __WINDOWS__
	SOCKET			Listenfd = NULL;
#else
	int				Listenfd = 0;
#endif // __WINDOWS__

	BaseAccepter() {};
	virtual ~BaseAccepter() {};
	int listen(const char* addr, unsigned short listenport) {
		snprintf(this->ServerAddr, sizeof(this->ServerAddr), "%s", addr);
		this->ServerPort = listenport;
		return AccepterRun(this);
	};
	void stop() { AccepterStop(this); };

	virtual bool	Init() = 0;
	virtual BaseWorker*	GetWorker() = 0;
};
#define BASEACCEPTER_SIZE sizeof(BaseAccepter)

template <class WORKER>
class Factory:public BaseAccepter
{
public:
	Factory() {};
	~Factory() {};
	bool	Init() { return true; };
	BaseWorker* GetWorker() { return new WORKER; };
};

#endif // !_REACTOR_H_
