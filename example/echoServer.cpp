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

// 这是一个回显服务的示例代码
//

#include <iostream>
#include <functional>
#include "../src/Reactor.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoClient : public BaseProtocol		//继承BaseProtocol
{
	HSOCKET sock = NULL;
	HTIMER timer = NULL;
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {
		printf("client 连接成功\n");
		sock = hsock;
		/* 创建一个定时器，定时发送 hello world */
		timer = TimerCreate(this, 5000, 3000, [](HTIMER timer, BaseProtocol* proto) {
			EchoClient* client = (EchoClient*)proto;
			HsocketSend(client->sock, "hello world", 11);
			printf("cient 发送: [hello world]\n");
		});
	};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {
		TimerDelete(timer);
		timer = NULL;
		sock = NULL;
	};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("client 接收: [%.*s]\n\n", len, data);
		HsocketPopBuf(hsock, len);
	};
};

class EchoServer: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		HsocketSend(hsock, data, len); 
		HsocketPopBuf(hsock, len);
		//HsocketClose(hsock);   //从容关闭，等待关闭通知
		//HsocketClosed(hsock);  //立即关闭，无通知
	};
};

class EchoAccepter: public BaseAccepter		//继承BaseFactory
{
public:
	bool	Init() { 
		return true; 
	};
	void	TimeOut() {
	};
	BaseProtocol* ProtocolCreate() {    //accept建立新连接时创建一个EchoProtocol对象
		EchoServer* proto = new EchoServer;
		//proto->AutoFree(false);   //连接关闭时proto不要被自动释放，由用户控制释放时机
		//proto->ThreadSet();   //自动分配工作线程
		//proto->ThreadSet(0);   //分配到指定工作线程， 0 ~ ActorThreadWorker - 1 
		return proto;
	};
};

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart();  

	//创建监听器并监听指定端口
	EchoAccepter* accepter = new EchoAccepter();

	printf("accepter 启动\n");
	//accepter->Listen("0.0.0.0", listen_port);  //仅ipv4
	accepter->Listen("::", listen_port);		//ipv4、ipv6双协议栈
	printf("正在监听%d端口……\n", listen_port);


	printf("创建EchoClient,并连接127.0.0.1:8000\n");
	EchoClient* client = new EchoClient();
	HsocketConnect(client, "127.0.0.1", 8000, TCP_CONN);

	TimeSleep(10);
	printf("accepter 优雅关闭\n");
	accepter->Stop();

	//安全起见，等待accpeter完全停止
	while (true){
		if (accepter->Listening == false) {
			printf("accepter 已关闭\n");
			break;
		}
	}

	printf("accepter 重启\n");
	listen_port = 8001;
	accepter->Listen("::", listen_port);
	printf("正在监听%d端口……\n", listen_port);
	//TimeSleep(30);
	//printf("accepter 关闭\n");
	//accepter->Stop();

	while (true){
		TimeSleep(10);
	}
}
