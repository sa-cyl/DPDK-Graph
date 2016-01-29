/* 
 * This file was created by YongLI Cheng 2014/3/16 \
 */ 
#include <iostream>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <assert.h>
#include <pthread.h>
#include <schedule/task.hpp>
#include <comm/xgclient.hpp>
#define BACKLOG 10
#define PORT 3333
#define MAXSIZE 1024*1024*sizeof(int)+6

static void *reply(void *wc){
	int rval;
	int *li;
	int fd=(*(workercontrol*)wc).fd_master_client;
	int fd_server_master=(*(workercontrol*)wc).fd_master_server;
	char buf[MAXSIZE];
	char Phead[6];
	char Pack_t[1];
	int offset=0;
	int nbyte;

	while(1){
		while((rval = read(fd, buf, MAXSIZE)) <= 0) {
			//std::cout<<"reading stream error!"<<std::endl;
			//close(fd);
			//pthread_cancel(pthread_self());
		}
		nbyte=rval;
		li=(int*)(buf+6);
		memcpy(Phead,buf,5);
		if(strcmp(Phead,"XGREQ")!=0){
			std::cout<<"Bad Package Formationt|"<<Phead<<std::endl;
			continue;
		}

		Pack_t[0]=buf[5];
		if(Pack_t[0]=='T'){
			std::cout<<"Aaaaa"<<Pack_t<<"|"<<Phead<<"|"<<li[0]<<"|"<<Pack_t[0]<<li[1]<<std::endl;

			char* msg = (char*)"XGACKT";
			if (send(fd, msg, strlen(msg), 0) == -1)
				std::cout<<"send error!"<<std::endl;
		}else if(Pack_t[0]=='B'){
			std::cout<<"Aaaaa"<<Pack_t<<"|"<<Phead<<"|"<<li[0]<<"|"<<Pack_t[0]<<li[1]<<std::endl;

			char* msg = (char*)"XGACKB";
			if (send(fd, msg, strlen(msg), 0) == -1)
				std::cout<<"send error!"<<std::endl;
		}else if(Pack_t[0]=='X'){
			int i;
			while(nbyte<MAXSIZE){
				offset+=rval;
				rval=read(fd, buf+offset, MAXSIZE);
				nbyte+=rval;
				std::cout<<rval<<";;;;;;;;;;;;;;;;;;;;"<<std::endl;			
			}
			for(i=0;i<1024*1024;i++) {
				std::cout<<li[i]<<"|"<<std::endl;
			}
			//std::cout<<"Aaaaa"<<Pack_t<<"|"<<Phead<<"|"<<li[1048570]<<"|"<<i-1<<std::endl;
			char* msg = (char*)"Hello,Mr hqlong, you are connected!\n";
			if (send(fd, msg, strlen(msg), 0) == -1)
				std::cout<<"send error!"<<std::endl;
		}else if(Pack_t[0]=='S'){
			int c;
			int isFirst;
			task *T;

			memcpy(&c, buf+6, sizeof(int));
			T = (task*)malloc(sizeof(task) * c);
                	for(int j = 0; j < c; j++){ 
				T[j]= *(task*)(buf+6+sizeof(int)+j*sizeof(task));
				std::cout<<T[j].machine<<"==== " <<T[j].iter<<" "<<T[j].interval<<std::endl;
			}		
			char* msg = (char*)"XGACKS";
			if (send(fd, msg, strlen(msg), 0) == -1)
				std::cout<<"send back XGACKS fail!"<<std::endl;
			isFirst = T[0].interval;
			for(int i = 0; i < c; i ++) {
				sleep(14);
				if(isFirst != 0){ 
					read(fd, buf, 6);
					memcpy(Phead,buf,6);
				}
				sleep(2);
				isFirst = 1;
				send(fd, "XGREQU", 6, 0);//this interval have been updated
				std::cout<<"machine  "<<T[i].machine<<"inter  "<<T[i].iter<<"   interval:"<<T[i].interval<<std::endl;
			}
			
			send(fd, "XGREQF", 6, 0); // Tell xgMaster this machine have finished.
			std::cout<<"Finished..."<<std::endl;
			continue;
		}else{
			std::cout<<"Unkown Package Formation"<<std::endl;
			continue;
		}
	}
}
	static void *server(void *info) {
		int sockfd, client_fd, fd_master_server;
		workercontrol wc;
		struct sockaddr_in my_addr;
		struct sockaddr_in remote_addr;

		if ((sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			std::cout<<"socket create failed!"<<std::endl;
		}
		assert(sockfd!=-1);

		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(PORT);
		my_addr.sin_addr.s_addr = INADDR_ANY;
		bzero(&(my_addr.sin_zero), 8);
		if (bind(sockfd, (struct sockaddr*) &my_addr, sizeof(struct sockaddr))==-1){
			std::cout<<"bind error!"<<std::endl;
			assert(false);
		}

		if (listen(sockfd, BACKLOG) == -1) {
			std::cout<<"listen error!"<<std::endl;
			assert(false);
		}

		wc.fd_master_server = *((int*)info);
		while (1) {
			socklen_t sin_size = sizeof(struct sockaddr_in);
			if ((client_fd = accept(sockfd, (struct sockaddr*) &remote_addr, &sin_size)) == -1) {
				std::cout<<"accept error!"<<std::endl;
				continue;
			}
			wc.fd_master_client = client_fd;
			std::cout<<"Received a connection from"<<inet_ntoa(remote_addr.sin_addr)<<std::endl;

                	pthread_t t;
                	int ret = pthread_create(&t, NULL, &reply, &wc);
			assert(ret>=0);
		}
	}

int main()
{
       	pthread_t ts;
	int fd_master;

	int done = 0;



        /* Start a server for xgMaster and other worker */
        int ret = pthread_create(&ts, NULL, &server, &fd_master);
	assert(ret>=0);

	/* Waiting for done */
	while(!done){ };

	/* do for end */
	close(fd_master);
}
