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

class xgclient{
	public:
	int master_fd;
	
	int connect_master(){
    		int sockfd;
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
    		}
	 	master_fd = sockfd;
	}
};
