/*
 * This File was created by YongLi Cheng 2014/3/16
 */
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <assert.h>
#include <arpa/inet.h>

#define SERVPORT 9001
#define MAXDATASIZE 6
#define SERVER_IP "192.168.3.65"

#include <iostream>

class lxclient{
	public:
	void client(){
    		int sockfd, recvbytes;
    		char buf[MAXDATASIZE];
    		struct hostent *host;
    		struct sockaddr_in serv_addr;

    		if (( sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
        		std::cout<<"socket error!"<<std::endl;
    		}
		assert(sockfd!=-1);

    		bzero(&serv_addr,sizeof(serv_addr));
    		serv_addr.sin_family    = AF_INET;
    		serv_addr.sin_port      = htons(SERVPORT);
    		serv_addr.sin_addr.s_addr= inet_addr(SERVER_IP);

    		if (connect(sockfd, (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
        		std::cout<<"connect error!"<<std::endl;
        		assert(false);
    		}

		memcpy(buf,"XGREQT",6);
		//int *li = (int *)(buf+6);
		//for(int i=0;i<1024*1024;i++) li[i]=i;
		
		int wret=write(sockfd,buf, MAXDATASIZE);
		std::cout<<wret<<std::endl;
		/*while(wret<MAXDATASIZE){
			std::cout<<wret<<std::endl;
			wret=wret+write(sockfd,buf+wret, MAXDATASIZE-wret);
			std::cout<<wret<<std::endl;
		}*/
   		if ((recvbytes = recv(sockfd, buf, MAXDATASIZE,0)) == -1) {
        		std::cout<<"recv error!"<<std::endl;
    		}
		assert(recvbytes!=-1);

    		buf[recvbytes] = '\0';
    		std::cout<<"Received: "<<buf<<std::endl;
    		close(sockfd);
	}

};
int main(void){
	lxclient lx;
	lx.client();
	std::cout<<"kjkljljlk"<<std::endl;
}
