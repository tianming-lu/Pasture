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
#include "../src/actor.h"
#include "../driver/http_client.h"

#ifdef __WINDOWS__
#define TimeSleep(x) Sleep(x*1000)
#else
#define TimeSleep(x) sleep(x)
#endif // __WINDOWS__

class EchoClient : public BaseWorker		//继承BaseWorker
{
public:
	~EchoClient() { printf("%s:%d\n", __func__, __LINE__); };
	HSOCKET sock1 = nullptr;
	HSOCKET sock2 = nullptr;
	Timer timer;
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {
		if (hsock == this->sock1) {
			printf("sock1 连接成功\n");
		}
		if (hsock == this->sock2) {
			printf("sock2 连接成功\n");
		}
	};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {
		if (hsock == this->sock1) {
			sock1 = nullptr;
		}
		if (hsock == this->sock2) {
			sock2 = nullptr;
		}
		if (sock1 == nullptr && sock2 == nullptr) {
			timer.close();
		}
	};
	void ConnectionRecved(HSOCKET sock, const char* data, int len) {
		printf("client 接收: [%.*s]\n", len, data);
		HsocketPopBuf(sock, len);
	};
};

class EchoServer: public BaseWorker		//继承BaseWorker
{
public:
	void ConnectionMade(HSOCKET hsock, PROTOCOL protocol) {};
	void ConnectionFailed(HSOCKET hsock, int err) {};
	void ConnectionClosed(HSOCKET hsock, int err) {};
	void ConnectionRecved(HSOCKET hsock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		HsocketSend(hsock, data, len);
		HsocketPopBuf(hsock, len);
		//HsocketClose(sock);	//从容关闭，后续触发ConnectionClosed事件
		//HsocketClosed(sock);	//立即关闭，不触发事件
	};
};

struct Test_Content {
	PROTOCOL			protocol : 3;
	PROTOCOL			user_protocol : 5;
};

int main(){

	Test_Content ctx;
	memset(&ctx, 0x0, sizeof(ctx));
	ctx.user_protocol = 0;
	ctx.protocol = SIGNAL;
	PROTOCOL pro = *(PROTOCOL*)&ctx;
	printf("%s:%d %d %x\n", __func__, __LINE__, pro, pro);

	int listen_port = 8000;
	//启动全局reactor
	ActorStart(0);  
	http_client_driver_regist();

	/*通过模板创建一个工厂类*/
	//Factory<EchoServer> factory;

	//printf("factory 启动\n");
	////factory.listen("0.0.0.0", listen_port);  //仅ipv4
	//factory.listen("::", listen_port);		//ipv4、ipv6双协议栈
	//printf("正在监听%d端口……\n", listen_port);

	char ss[] = "ff\r\n";
	char* p = ss;
	long number = strtol(p, &p, 16);
	printf("%s:%d %ld  %p  %p\n", __func__, __LINE__, number, ss, p);

	printf("创建EchoClient,并连接127.0.0.1:8000\n");
	EchoClient *client = new EchoClient();
	client->auto_release(true);
	client->task([&client, listen_port]() {  //向client所在的线程(非当前线程)安排个任务，用于实现线程安全
		//这一段在woker工作的线程执行
		
		//client.sock1 = HsocketConnect(&client, "127.0.0.1", listen_port, TCP_PROTOCOL); 
		//client.sock2 = HsocketConnect(&client, "127.0.0.1", listen_port, TCP_PROTOCOL);
		//HttpClient->request("GET", "http://www.baidu.com", NULL, NULL, [](HttpResponse& res, int stat) {
		//	printf("%s:%d %d %d %s %zd %s\n", __func__, __LINE__, stat, res.state_code, res.state.c_str(), res.content.size(), res.header["Content-Length"].c_str());
		//	});

		HttpClient->request(client, "GET", "http://www.baidu.com", NULL, NULL, [](HttpTask* task, BaseWorker* worker, HttpResponse* res, int stat) {
			printf("%s:%d %d %d %s %zd %s\n", __func__, __LINE__, stat, res->state_code, res->state.c_str(), res->content.size(), res->header["Content-Length"].c_str());
			});
		printf("%s:%d\n", __func__, __LINE__);
	});
	
	///* 创建一个定时器，定时发送 hello world */
	//client.timer.create(&client, 1000, 3000, [&client]() {
	//	if (client.sock1)HsocketSend(client.sock1, "hello world 1", 13);
	//	if (client.sock2)HsocketSend(client.sock2, "hello world 2", 13);
	//});

	//TimeSleep(10);
	//printf("factory 优雅关闭\n");
	//factory.stop();
	
	//printf("factory 重启\n");
	//listen_port = 8001;
	//factory.listen("::", listen_port);
	//printf("正在监听%d端口……\n", listen_port);
	//TimeSleep(30);
	//printf("factory 关闭\n");
	//factory.stop();

	while (true){
		TimeSleep(10);
	}
}
