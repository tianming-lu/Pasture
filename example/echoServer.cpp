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
#include "../src/Reactor.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoProtocol: public BaseProtocol		//继承BaseProtocol
{
	void ConnectionMade(HSOCKET hsock, CONN_TYPE type) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
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
		EchoProtocol* proto = new EchoProtocol;
		//proto->Set(CLIENT_PROTOCOL);   默认为SERVER_PROTOCOL，连接关闭时proto会被自动释放，否则由用户控制释放时机
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
