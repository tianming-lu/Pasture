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

class EchoClient : public BaseWorker		//继承BaseWorker
{
	HSOCKET sock = NULL;
	HTIMER timer = NULL;
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {
		printf("client 连接成功\n");
		sock = hsock;
		/* 创建一个定时器，定时发送 hello world */
		timer = TimerCreate(this, NULL, 5000, 3000, [](HTIMER timer, BaseWorker* proto, void* data) {
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

class EchoServer: public BaseWorker		//继承BaseWorker
{
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {};
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

int main(){
	int listen_port = 8000;
	//启动全局reactor
	ReactorStart();  

	/*通过模板创建一个工厂类*/
	Factory<EchoServer> factory;

	printf("factory 启动\n");
	//factory.listen("0.0.0.0", listen_port);  //仅ipv4
	factory.listen("::", listen_port);		//ipv4、ipv6双协议栈
	printf("正在监听%d端口……\n", listen_port);

	printf("创建EchoClient,并连接127.0.0.1:8000\n");
	EchoClient* client = new EchoClient();
	HsocketConnect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);

	TimeSleep(10);
	printf("factory 优雅关闭\n");
	factory.stop();

	printf("factory 重启\n");
	listen_port = 8001;
	factory.listen("::", listen_port);
	printf("正在监听%d端口……\n", listen_port);
	TimeSleep(30);
	printf("factory 关闭\n");
	factory.stop();

	while (true){
		TimeSleep(10);
	}
}
