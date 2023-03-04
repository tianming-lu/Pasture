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
public:
	Socket sock;
	Timer timer;
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {
		printf("client 连接成功\n");
		/* 创建一个定时器，定时发送 hello world */
		timer.create(this, 5000, 3000, [this]() {
			printf("cient 发送: [hello world]\n");
			this->sock.send("hello world", 11);
		});
	};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {
		timer.close();
		sock = NULL;
	};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("client 接收: [%.*s]\n\n", len, data);
		sock.popbuf(len);
	};
};

class EchoServer: public BaseWorker		//继承BaseWorker
{
public:
	Socket sock;
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {
		sock = hsock;
	};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		sock.send(data, len);
		sock.popbuf(len);
		//sock.close();	//从容关闭，后续触发ConnectionClosed事件
		//sock.closed();	//立即关闭，不触发事件

		/*以下是等效C风格接口*/
		//HsocketSend(hsock, data, len);
		//HsocketPopBuf(hsock, len);
		//HsocketClose(hsock);   
		//HsocketClosed(hsock);  
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
	EchoClient* client = new EchoClient;
	client->auto_free(false);	//禁止actor释放对象
	client->sock.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);

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
