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
#include <Ws2tcpip.h>
#include <Mstcpip.h>
#include <time.h>
#include <map>

#pragma comment(lib, "Ws2_32.lib")

#define DATA_BUFSIZE 8192
#define READ	0
#define WRITE	1
#define ACCEPT	2
#define CONNECT 3

static LPFN_ACCEPTEX lpfnAcceptEx = NULL;
static LPFN_CONNECTEX lpfnConnectEx = NULL;

static bool HsocketSendEx(IOCP_SOCKET* IocpSock, const char* data, int len);

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

static inline void* pst_malloc(size_t size){
	return GlobalAlloc(GPTR, size);
}

static inline void* pst_realloc(void* ptr, size_t size){
	return GlobalReAlloc(ptr, size, GMEM_MOVEABLE);
}

static inline void pst_free(void* ptr){
	GlobalFree(ptr);
}

static inline IOCP_SOCKET* NewIOCP_Socket(){
	return (IOCP_SOCKET*)pst_malloc(sizeof(IOCP_SOCKET));
}

static inline void ReleaseIOCP_Socket(IOCP_SOCKET* IocpSock){
#ifdef KCP_SUPPORT
	if (IocpSock->_conn_type == KCP_CONN)
	{
		Kcp_Content* ctx = (Kcp_Content*)IocpSock->_user_data;
		ikcp_release(ctx->kcp);
		pst_free(ctx->buf);
		pst_free(ctx);
	}
#endif
	pst_free(IocpSock);
}

static inline IOCP_BUFF* NewIOCP_Buff(){
	return (IOCP_BUFF*)pst_malloc(sizeof(IOCP_BUFF));
}

static inline void ReleaseIOCP_Buff(IOCP_BUFF* IocpBuff){
	pst_free(IocpBuff);
}

static SOCKET GetListenSock(const char* addr, int port){
	SOCKET listenSock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);

	struct sockaddr_in server_addr = {0x0};
	inet_pton(AF_INET, addr, &server_addr.sin_addr);
	server_addr.sin_family = AF_INET;
	server_addr.sin_port = htons(port);

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

static void HsocketSetKeepAlive(SOCKET fd) {
	int keepalive = 1;
	setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (const char*)&keepalive, sizeof(keepalive));
	struct tcp_keepalive in_keep_alive;
	unsigned long ul_in_len = sizeof(struct tcp_keepalive);
	struct tcp_keepalive out_keep_alive = { 0 };
	unsigned long ul_out_len = sizeof(struct tcp_keepalive);
	unsigned long ul_bytes_return = 0;
	in_keep_alive.onoff = 1; /*打开keepalive*/
	in_keep_alive.keepaliveinterval = 5*000; /*发送keepalive心跳时间间隔-单位为毫秒*/
	in_keep_alive.keepalivetime = 60*000; /*多长时间没有报文开始发送keepalive心跳包-单位为毫秒*/
	WSAIoctl(fd, SIO_KEEPALIVE_VALS, (LPVOID)&in_keep_alive, ul_in_len,
		(LPVOID)&out_keep_alive, ul_out_len, &ul_bytes_return, NULL, NULL);
}

static void PostAcceptClient(IOCP_SOCKET* IocpSock){
	BaseFactory* fc = IocpSock->factory;

	IOCP_BUFF* IocpBuff;
	IocpBuff = NewIOCP_Buff();
	if (IocpBuff == NULL){
		return;
	}
	IocpBuff->databuf.buf = (char*)pst_malloc(DATA_BUFSIZE);
	if (IocpBuff->databuf.buf == NULL){
		ReleaseIOCP_Buff(IocpBuff);
		return;
	}
	IocpBuff->databuf.len = DATA_BUFSIZE;
	IocpBuff->type = ACCEPT;
	IocpBuff->hsock = IocpSock;

	IocpBuff->fd = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	if (IocpBuff->fd == INVALID_SOCKET){
		ReleaseIOCP_Buff(IocpBuff);
		return;
	}

	/*调用AcceptEx函数，地址长度需要在原有的上面加上16个字节向服务线程投递一个接收连接的的请求*/
	bool rc = lpfnAcceptEx(fc->Listenfd, IocpBuff->fd,
		IocpBuff->databuf.buf, 0,
		sizeof(struct sockaddr_in) + 16, sizeof(struct sockaddr_in) + 16,
		&IocpBuff->databuf.len, &(IocpBuff->overlapped));

	if (false == rc){
		if (WSAGetLastError() != ERROR_IO_PENDING){
			ReleaseIOCP_Buff(IocpBuff);
			return;
		}
	}
	return;
}

static inline void CloseSocket(IOCP_SOCKET* IocpSock){
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

static void do_close(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff, int err){
	switch (IocpBuff->type){
	case ACCEPT:
		if (IocpBuff->databuf.buf)
			pst_free(IocpBuff->databuf.buf);
		ReleaseIOCP_Buff(IocpBuff);
		PostAcceptClient(IocpSock);
		return;
	case WRITE:
		if (IocpBuff->databuf.buf != NULL)
			pst_free(IocpBuff->databuf.buf);
		ReleaseIOCP_Buff(IocpBuff);
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
	if (IocpSock->recv_buf) pst_free(IocpSock->recv_buf);
	ReleaseIOCP_Buff(IocpBuff);
	ReleaseIOCP_Socket(IocpSock);
}

static bool ResetIocp_Buff(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	memset(&IocpBuff->overlapped, 0, sizeof(OVERLAPPED));
	if (IocpSock->recv_buf == NULL){
		if (IocpBuff->databuf.buf != NULL){
			IocpSock->recv_buf = IocpBuff->databuf.buf;
		}
		else{
			IocpSock->recv_buf = (char*)pst_malloc(DATA_BUFSIZE);
			if (IocpSock->recv_buf == NULL) return false;
			IocpBuff->size = DATA_BUFSIZE;
		}
	}
	IocpBuff->databuf.len = IocpBuff->size - IocpBuff->offset;
	if (IocpBuff->databuf.len == 0){
		IocpBuff->size += DATA_BUFSIZE;
		char* new_ptr = (char*)pst_realloc(IocpSock->recv_buf, IocpBuff->size);
		if (new_ptr == NULL) return false;
		IocpSock->recv_buf = new_ptr;
		IocpBuff->databuf.len = IocpBuff->size - IocpBuff->offset;
	}
	IocpBuff->databuf.buf = IocpSock->recv_buf + IocpBuff->offset;
	return true;
}

static inline void PostRecvUDP(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	int fromlen = sizeof(IocpSock->peer_addr);
	if (SOCKET_ERROR == WSARecvFrom(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &(IocpBuff->flags), 
		(struct sockaddr*)&IocpSock->peer_addr, &fromlen, &IocpBuff->overlapped, NULL)){
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			do_close(IocpSock, IocpBuff, err);
		}
	}
}

static inline void PostRecvTCP(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	if (SOCKET_ERROR == WSARecv(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &(IocpBuff->flags), &IocpBuff->overlapped, NULL)){
		int err = WSAGetLastError();
		if (ERROR_IO_PENDING != err){
			do_close(IocpSock, IocpBuff, err);
		}
	}
}

static void PostRecv(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	IocpBuff->type = READ;
	if (ResetIocp_Buff(IocpSock, IocpBuff) == false){
		return do_close(IocpSock, IocpBuff, 14);
	}
	if (IocpSock->_conn_type == TCP_CONN || IocpSock->_conn_type == SSL_CONN)
		return PostRecvTCP(IocpSock, IocpBuff);
	return PostRecvUDP(IocpSock, IocpBuff);
	
}

static void do_aceept(IOCP_SOCKET* IocpListenSock, IOCP_BUFF* IocpBuff){
	PostAcceptClient(IocpListenSock);

	BaseFactory* fc = IocpListenSock->factory;
	Reactor* reactor = fc->reactor;
	//连接成功后刷新套接字属性
	setsockopt(IocpBuff->fd, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT, (char*) & (IocpListenSock->fd), sizeof(IocpListenSock->fd));

	IOCP_SOCKET* IocpSock = NewIOCP_Socket();
	if (IocpSock == NULL){
		return do_close(IocpListenSock, IocpBuff, 14);
	}
	IocpSock->fd = IocpBuff->fd;
	HsocketSetKeepAlive(IocpSock->fd);

	BaseProtocol* proto = fc->CreateProtocol();	//用户指针
	if (proto == NULL){
		return do_close(IocpListenSock, IocpBuff, 14);
	}
	proto->SetFactory(fc, SERVER_PROTOCOL);
	IocpSock->factory = fc;
	IocpSock->_user = proto;	//用户指针
	IocpSock->_IocpBuff = IocpBuff;
	IocpBuff->hsock = IocpSock;

	int nSize = sizeof(IocpSock->peer_addr);
	getpeername(IocpSock->fd, (struct sockaddr*)&IocpSock->peer_addr, &nSize);

	InterlockedIncrement(&proto->sockCount);
	CreateIoCompletionPort((HANDLE)IocpSock->fd, reactor->ComPort, (ULONG_PTR)IocpSock, 0);	//将监听到的套接字关联到完成端口

	proto->Lock();
	proto->ConnectionMade(IocpSock, IocpSock->_conn_type);
	proto->UnLock();
	PostRecv(IocpSock, IocpBuff);
}

#ifdef KCP_SUPPORT
static void do_read_kcp(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
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
				char* newbuf = (char*)pst_realloc(ctx->buf, newsize);
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

static void do_read_tcp_and_udp(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff) {
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

static void do_read(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff) {
	switch (IocpSock->_conn_type) {
	case TCP_CONN:
	case UDP_CONN:
		return do_read_tcp_and_udp(IocpSock, IocpBuff);
#ifdef KCP_SUPPORT
	case KCP_CONN:
		return do_read_kcp(IocpSock, IocpBuff);
#endif
	default:
		break;
	}
}

static void do_connect(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff) {
	setsockopt(IocpSock->fd, SOL_SOCKET, SO_UPDATE_CONNECT_CONTEXT, NULL, 0);  //连接成功后刷新套接字属性
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

static void ProcessIO(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
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
	Reactor* reactor = (Reactor*)pParam;
	DWORD	dwIoSize = 0;
	IOCP_SOCKET* IocpSock = NULL;
	IOCP_BUFF* IocpBuff = NULL;		//IO数据,用于发起接收重叠操作
	bool bRet = false;
	DWORD err = 0;
	while (true){
		bRet = false;
		dwIoSize = 0;	//IO操作长度
		IocpSock = NULL;
		IocpBuff = NULL;
		err = 0;
		bRet = GetQueuedCompletionStatus(reactor->ComPort, &dwIoSize, (PULONG_PTR)&IocpSock, (LPOVERLAPPED*)&IocpBuff, INFINITE);
		if (IocpBuff != NULL) IocpSock = IocpBuff->hsock;   //强制closesocket后可能返回错误的IocpSock，从IocpBuff中获取正确的IocpSock
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
	Reactor* reactor = (Reactor*)pParam;
	for (int i = 0; i < reactor->CPU_COUNT; i++){
	//for (unsigned int i = 0; i < 1; i++){
		HANDLE ThreadHandle = CreateThread(NULL, 0, serverWorkerThread, pParam, 0, NULL);
		if (NULL == ThreadHandle) {
			return -4;
		}
		CloseHandle(ThreadHandle);
	}
	std::map<uint16_t, BaseFactory*>::iterator iter;
	while (reactor->Run){
		for (iter = reactor->FactoryAll.begin(); iter != reactor->FactoryAll.end(); ++iter){
			iter->second->FactoryLoop();
		}
		Sleep(100);
	}
	return 0;
}

int __STDCALL ReactorStart(Reactor* reactor){
	WSADATA wsData;
	if (0 != WSAStartup(0x0202, &wsData)){
		return SOCKET_ERROR;
	}

	reactor->ComPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (reactor->ComPort == NULL){
		return -2;
	}

	SYSTEM_INFO sysInfor;
	GetSystemInfo(&sysInfor);
	reactor->CPU_COUNT = sysInfor.dwNumberOfProcessors;

	SOCKET tempSock = WSASocket(AF_INET, SOCK_STREAM, 0, NULL, 0, WSA_FLAG_OVERLAPPED);
	//使用WSAIoctl获取AcceptEx函数指针
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

	HANDLE ThreadHandle;
	ThreadHandle = CreateThread(NULL, 0, mainIOCPServer, reactor, 0, NULL);
	if (NULL == ThreadHandle) {
		return -4;
	}
	CloseHandle(ThreadHandle);
	return 0;
}

void __STDCALL ReactorStop(Reactor* reactor){
	reactor->Run = false;
}

int __STDCALL FactoryRun(BaseFactory* fc){
	if (!fc->FactoryInit()) return -1;

	if (fc->ServerPort != 0){
		fc->Listenfd = GetListenSock(fc->ServerAddr, fc->ServerPort);
		if (fc->Listenfd == SOCKET_ERROR) return -2;

		IOCP_SOCKET* IcpSock = NewIOCP_Socket();
		if (IcpSock == NULL){
			closesocket(fc->Listenfd);
			return -3;
		}
		IcpSock->factory = fc;
		IcpSock->fd = fc->Listenfd;

		CreateIoCompletionPort((HANDLE)fc->Listenfd, fc->reactor->ComPort, (ULONG_PTR)IcpSock, 0);
		for (int i = 0; i < fc->reactor->CPU_COUNT; i++)
			PostAcceptClient(IcpSock);
	}
	fc->FactoryInited();
	fc->reactor->FactoryAll.insert(std::pair<uint16_t, BaseFactory*>(fc->ServerPort, fc));
	return 0;
}

int __STDCALL FactoryStop(BaseFactory* fc){
	std::map<uint16_t, BaseFactory*>::iterator iter;
	iter = fc->reactor->FactoryAll.find(fc->ServerPort);
	if (iter != fc->reactor->FactoryAll.end()){
		fc->reactor->FactoryAll.erase(iter);
	}
	fc->FactoryClose();
	return 0;
}

static bool IOCPConnectUDP(BaseFactory* fc, IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff, int listen_port)
{
	IocpSock->fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (IocpSock->fd == INVALID_SOCKET) return false;
	IocpBuff->fd = IocpSock->fd;

	struct sockaddr_in local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family = AF_INET;
	local_addr.sin_port = ntohs(listen_port);
	bind(IocpSock->fd, (struct sockaddr*)(&local_addr), sizeof(local_addr));

	if (ResetIocp_Buff(IocpSock, IocpBuff) == false){
		closesocket(IocpSock->fd);
		return false;
	}

	CreateIoCompletionPort((HANDLE)IocpSock->fd, fc->reactor->ComPort, (ULONG_PTR)IocpSock, 0);
	int fromlen = sizeof(IocpSock->peer_addr);
	IocpBuff->type = READ;
	if (SOCKET_ERROR == WSARecvFrom(IocpSock->fd, &IocpBuff->databuf, 1, NULL, &(IocpBuff->flags), 
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
	IOCP_SOCKET* IocpSock = NewIOCP_Socket();
	if (IocpSock == NULL) return NULL; 

	IOCP_BUFF* IocpBuff = NewIOCP_Buff();
	if (IocpBuff == NULL){
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	IocpBuff->type = CONNECT;
	IocpBuff->hsock = IocpSock;

	IocpSock->factory = fc;
	IocpSock->_conn_type = UDP_CONN;
	IocpSock->_user = proto;
	IocpSock->_IocpBuff = IocpBuff;
	IocpSock->peer_addr.sin_family = AF_INET;
	IocpSock->peer_addr.sin_port = htons(0);
	inet_pton(AF_INET, "0.0.0.0", &IocpSock->peer_addr.sin_addr);

	bool ret = false;
	ret = IOCPConnectUDP(fc, IocpSock, IocpBuff, port);   //UDP连接
	if (ret == false){
		ReleaseIOCP_Buff(IocpBuff);
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	InterlockedIncrement(&proto->sockCount);
	return 0;
}

static bool IOCPConnectTCP(BaseFactory* fc, IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	IocpSock->fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (IocpSock->fd == INVALID_SOCKET) return false;
	IocpBuff->fd = IocpSock->fd;
	HsocketSetKeepAlive(IocpSock->fd);
	struct sockaddr_in local_addr;
	memset(&local_addr, 0, sizeof(local_addr));
	local_addr.sin_family = AF_INET;
	bind(IocpSock->fd, (struct sockaddr*)(&local_addr), sizeof(local_addr));
	CreateIoCompletionPort((HANDLE)IocpSock->fd, fc->reactor->ComPort, (ULONG_PTR)IocpSock, 0);

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
	IOCP_SOCKET* IocpSock = NewIOCP_Socket();
	if (IocpSock == NULL) return NULL;

	IOCP_BUFF* IocpBuff = NewIOCP_Buff();
	if (IocpBuff == NULL){
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	IocpBuff->type = CONNECT;
	IocpBuff->hsock = IocpSock;

	IocpSock->factory = fc;
	IocpSock->_conn_type = conntype > ITMER ? TCP_CONN: conntype;
	IocpSock->_user = proto;
	IocpSock->_IocpBuff = IocpBuff;
	IocpSock->peer_addr.sin_family = AF_INET;
	IocpSock->peer_addr.sin_port = htons(port);
	inet_pton(AF_INET, ip, &IocpSock->peer_addr.sin_addr);

	bool ret = false;
	if (conntype == TCP_CONN || conntype == SSL_CONN)
		ret = IOCPConnectTCP(fc, IocpSock, IocpBuff);   //TCP连接
	else if (conntype == UDP_CONN || conntype == KCP_CONN)
		ret = IOCPConnectUDP(fc, IocpSock, IocpBuff, 0);   //UDP连接
	else {
		ReleaseIOCP_Buff(IocpBuff);
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	if (ret == false){
		ReleaseIOCP_Buff(IocpBuff);
		ReleaseIOCP_Socket(IocpSock);
		return NULL;
	}
	InterlockedIncrement(&proto->sockCount);
	return IocpSock;
}

static bool IOCPPostSendUDPEx(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff, struct sockaddr* addr, int addrlen){
	if (SOCKET_ERROR == WSASendTo(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, addr, addrlen, &IocpBuff->overlapped, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError())
			return false;
	}
	return true;
}

static bool IOCPPostSendTCPEx(IOCP_SOCKET* IocpSock, IOCP_BUFF* IocpBuff){
	if (SOCKET_ERROR == WSASend(IocpSock->fd, &IocpBuff->databuf, 1, NULL, 0, &IocpBuff->overlapped, NULL)){
		if (ERROR_IO_PENDING != WSAGetLastError())
			return false;
	}
	return true;
}

static bool HsocketSendEx(IOCP_SOCKET* IocpSock, const char* data, int len){
	IOCP_BUFF* IocpBuff = NewIOCP_Buff();
	if (IocpBuff == NULL) return false;

	IocpBuff->databuf.buf = (char*)pst_malloc(len);
	if (IocpBuff->databuf.buf == NULL){
		ReleaseIOCP_Buff(IocpBuff);
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
		pst_free(IocpBuff->databuf.buf);
		ReleaseIOCP_Buff(IocpBuff);
		return false;
	}
	return true;
}

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
		IOCP_BUFF* IocpBuff = NewIOCP_Buff();
		if (IocpBuff == NULL) return false;

		IocpBuff->databuf.buf = (char*)pst_malloc(len);
		if (IocpBuff->databuf.buf == NULL) {
			ReleaseIOCP_Buff(IocpBuff);
			return false;
		}
		memcpy(IocpBuff->databuf.buf, data, len);
		IocpBuff->databuf.len = len;
		memset(&IocpBuff->overlapped, 0, sizeof(OVERLAPPED));
		IocpBuff->type = WRITE;
		
		struct sockaddr_in toaddr = { 0x0 };
		toaddr.sin_family = AF_INET;
		toaddr.sin_port = htons(port);
		inet_pton(AF_INET, ip, &toaddr.sin_addr);

		bool ret = IOCPPostSendUDPEx(hsock, IocpBuff, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr));
		if (ret == false) {
			pst_free(IocpBuff->databuf.buf);
			ReleaseIOCP_Buff(IocpBuff);
			return false;
		}
		return true;
	}
	return false;
}

IOCP_BUFF* __STDCALL HsocketGetBuff(){
	IOCP_BUFF* IocpBuff = NewIOCP_Buff();
	if (IocpBuff){
		IocpBuff->databuf.buf = (char*)pst_malloc(DATA_BUFSIZE);
		if (IocpBuff->databuf.buf) { IocpBuff->size = DATA_BUFSIZE; }
		else { ReleaseIOCP_Buff(IocpBuff); return NULL; }	
	}
	return IocpBuff;
}

bool __STDCALL HsocketSetBuff(HNETBUFF netbuff, const char* data, int len){
	if (netbuff == NULL) return false;
	int left = netbuff->size - netbuff->offset;
	if (left >= len){
		memcpy(netbuff->databuf.buf + netbuff->databuf.len, data, len);
		netbuff->databuf.len += len;
	}
	else{
		int newsize = netbuff->databuf.len + len;
		char* new_ptr = (char*)pst_realloc(netbuff->databuf.buf, newsize);
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

bool __STDCALL HsocketSendBuff(HSOCKET hsock, HNETBUFF netbuff){
	if (netbuff == NULL || hsock == NULL) return false;
	memset(&netbuff->overlapped, 0, sizeof(OVERLAPPED));
	netbuff->type = WRITE;

	bool ret = false;
	if (hsock->_conn_type == TCP_CONN) ret = IOCPPostSendTCPEx(hsock, netbuff);
	else ret = IOCPPostSendUDPEx(hsock, netbuff, (struct sockaddr*)&hsock->peer_addr, sizeof(hsock->peer_addr));
	if (ret == false){
		pst_free(netbuff->databuf.buf);
		ReleaseIOCP_Buff(netbuff);
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
		IOCP_BUFF* IocpBuff = hsock->_IocpBuff;
		IocpBuff->offset -= len;
		memmove(hsock->recv_buf, hsock->recv_buf + len, IocpBuff->offset);
		return IocpBuff->offset;
	}
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
		hsock->peer_addr.sin_port = htons(port);
		inet_pton(AF_INET, ip, &hsock->peer_addr.sin_addr);
	}
}

void __STDCALL HsocketPeerIP(HSOCKET hsock, char* ip, size_t ipsz){
	inet_ntop(AF_INET, &hsock->peer_addr.sin_addr, ip, ipsz);
}
int __STDCALL HsocketPeerPort(HSOCKET hsock) {
	return ntohs(hsock->peer_addr.sin_port);
}

BaseProtocol* __STDCALL HsocketBindUser(HSOCKET hsock, BaseProtocol* proto) {
	BaseProtocol* old = hsock->_user;
	InterlockedDecrement(&old->sockCount);
	hsock->_user = proto;
	InterlockedIncrement(&proto->sockCount);
	return old;
}

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
		Kcp_Content* ctx = (Kcp_Content*)pst_malloc(sizeof(Kcp_Content));
		if (!ctx) { ikcp_release(kcp); return -1; }
		ctx->kcp = kcp;
		ctx->buf = (char*)pst_malloc(DATA_BUFSIZE);
		if (!ctx->buf) { ikcp_release(kcp); pst_free(ctx); return -1; }
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
