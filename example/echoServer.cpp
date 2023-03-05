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
	Socket sock1;
	Socket sock2;
	Timer timer;
	void ConnectionMade(Socket sock, PROTOCOL protocol) {
		if (sock == this->sock1) {
			printf("sock1 连接成功\n");
		}
		if (sock == this->sock2) {
			printf("sock2 连接成功\n");
		}
		/* 创建一个定时器，定时发送 hello world */
		timer.create(this, 1000, 3000, [this, sock]() {
			if (sock == this->sock1) {
				sock.send("hello world 1", 13);
			}
			if (sock == this->sock2) {
				sock.send("hello world 2", 13);
			}
		});
	};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {
		timer.close();
	};
	void ConnectionRecved(Socket sock, const char* data, int len) {
		printf("client 接收: [%.*s]\n\n", len, data);
		sock.popbuf(len);
	};
};

class EchoServer: public BaseWorker		//继承BaseWorker
{
public:
	Socket server_sock;
	void ConnectionMade(Socket sock, PROTOCOL protocol) {
		server_sock = sock;
	};
	void ConnectionFailed(Socket sock, int err) {};
	void ConnectionClosed(Socket sock, int err) {};
	void ConnectionRecved(Socket sock, const char* data, int len) {
		printf("server 接收: [%.*s]\n", len, data);
		sock.send(data, len);
		sock.popbuf(len);
		sock.close();	//从容关闭，后续触发ConnectionClosed事件
		//sock.closed();	//立即关闭，不触发事件
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
	client->task([client, listen_port]() {  //向client所在的线程(非当前线程)安排个任务，用于实现线程安全，
		//这一段在woker工作的线程执行
		client->sock1.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL); 
		client->sock2.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);
	});
	/*当前线程执行，非线程安全*/
	//client->sock1.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);
	//client->sock2.connect(client, "127.0.0.1", listen_port, TCP_PROTOCOL);

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
