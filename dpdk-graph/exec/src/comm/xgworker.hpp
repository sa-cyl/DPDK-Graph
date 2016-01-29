/*
@file
 * @author  YongLi Cheng <ChengYongLi@hust.edu.cn>
 * @version 1.0
 *
 * @section LICENSE
 *
 * Copyright [2014] [Yongli Cheng , Xiuneng Wang / Huazhong University of Science and Technology]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "io/stripedio.hpp"
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
#include"threadpool.hpp"
#include "rmda/rmdacomm_svc.hpp"
#include "rmda/rmdacomm_clnt.hpp"
#include <rpc/rpc.h>


extern "C"
     {
        #include "netdpsock_intf.h"
	ssize_t netdpsock_send(int sockfd, const void *buf, size_t len, int flags);
	ssize_t netdpsock_recv(int sockfd, void *buf, size_t len, int flags);
        int netdpsock_close(int fd);
        ssize_t netdpsock_recvfrom(int sockfd, void *buf, size_t len, int flags, struct sockaddr *src_addr, socklen_t *addrlen);
        ssize_t netdpsock_sendto(int sockfd, const void *buf, size_t len, int flags, const struct sockaddr *dest_addr, socklen_t addrlen);
        ssize_t netdpsock_send(int sockfd, const void *buf, size_t len, int flags);
        int netdpsock_bind(int sockfd, const struct sockaddr *addr, socklen_t addrlen);
        int netdpsock_epoll_ctl(int epfd, int op, int fd, struct epoll_event *event);
        int netdpsock_epoll_wait(int epfd, struct epoll_event *events, int maxevents, int timeout);
        int netdpsock_accept(int sockfd, struct sockaddr *addr, socklen_t *addrlen);
        int netdpsock_epoll_create(int size);
        int netdpsock_init(char *file_prefix);
        int netdpsock_listen(int sockfd, int backlog);
        int netdpsock_socket(int domain, int type, int protocol);
     }

using namespace CE_Graph;

#define BACKLOG 10
#define PORT1 3333
#define PORT2 7777
#define MAXSIZE 1024 * 1024 * 50
#define COMMTHREADS 20

static int exec_interval;
static int interval;
static size_t nedges;
static int ll_main_to_main;
static pthread_mutex_t tlock,olock;
static volatile int fd_master = -1;
static volatile int inbuf_loaded = 0;
static volatile int obuf_loaded = 0;
static int M, P, N,CO, asyn, rdma, sockfd1,sockfd2;
static int sockfds[1024],sockfdr[1024];
static CLIENT *cli[1024];;
static data_arg data_argn[1024];
static volatile int interval_dirt_disk[1024];
static volatile int interval_dirt_memory[1024];
static volatile int interval_dirt_main[1024];
static volatile int exec_interval_buf = -1;
static char  **obuf;
static volatile int olength[1024];
static volatile int recv_fd=0;
static volatile int recv_counter=0,ccc1=0,ccc2=0,ccc3=0;
//static volatile int recv_job_num, send_job_num;
//static char **inbuf;
//static volatile int inlength[1024];
static std::string bname;
static char *in_to_out_buf = NULL;
static char *out_to_in_buf = NULL;
static char *main_to_main_buf = NULL;
static int cout_g=0;
static dense_bitset *prev_bitset = NULL;
static vid_t sel_v,interval_st,interval_en;

typedef struct{
	int fd;
	int len;
	int from;	
	int offset;
}in_info;
static volatile in_info in_infos[1024];
/* for test */
static int cc_prev = 0;

char *wbuf;
int read_all(int fd, void *buf, int n);
int write_all(int fd, void *buf, int n);
//int write_to_worker(char *ip, int port,  void *buf, int from , int to, int flage, int n);

void *reply(void *client_fd){ //because this function is static ,its vars is static
	
	int fd,rval;
	char msg[1024];
	int Bid, offset, length;
	
	fd = *(int*)client_fd;
	rval = read_all(fd, msg, sizeof(int)*3);
	if(rval == -1)  std::cout<<"Recv Initial data...fail..."<<std::endl;
	memcpy(&Bid, msg, sizeof(int));
	memcpy(&offset, msg + sizeof(int), sizeof(int));
	memcpy(&length, msg + 2*sizeof(int), sizeof(int));
	//std::cout<<"Bid: "<<Bid<<"=================== offset: "<<offset<<" length: "<<length<<" fd: "<<fd<<std::endl;

        rval = read_all(fd, wbuf + offset, length);
        send(fd,(char*)"XGACKT", 6, 0);
        close(fd);
}
	void *server_rdma(void *info){
        	register SVCXPRT *transp;

        	pmap_unset (OSDPROG, OSDVERS);

/*
        	transp = svcudp_create(RPC_ANYSOCK);
        	if (transp == NULL) { 
                	fprintf (stderr, "%s", "cannot create udp service.");
                	exit(1);
        	}       
        	if (!svc_register(transp, OSDPROG, OSDVERS, osdprog_1, IPPROTO_UDP)) {
                	fprintf (stderr, "%s", "unable to register (OSDPROG, OSDVERS, udp).");
                	exit(1);
        	}       

        	transp = svctcp_create(RPC_ANYSOCK, 0, 0);
        	if (transp == NULL) { 
                	fprintf (stderr, "%s", "cannot create tcp service.");
                	exit(1);
        	}       
        	if (!svc_register(transp, OSDPROG, OSDVERS, osdprog_1, IPPROTO_TCP)) {
                	fprintf (stderr, "%s", "unable to register (OSDPROG, OSDVERS, tcp).");
                	exit(1);
        	}       

        	svc_run ();*/
/* disable rdma
	transp = svcrdma_create();
        if (transp == NULL) {
                fprintf (stderr, "%s", "cannot create rdma service.");
                exit(1);
        }   
        printf("create rdma transp success~\n");

        if (!svc_register(transp, OSDPROG, OSDVERS, osdprog_1, 37549)) {
                fprintf (stderr, "%s", "unable to register (OSDPROG, OSDVERS, rdma).");
                exit(1);
        }   
        printf("reg rdma transp success~\n");

        printf("server is running\n");
        svc_run_rdma (); 
        	fprintf (stderr, "%s", "svc_run returned");
*/
	}
	 void *server1(void *info) {
		int ret, len;
		char msg[1024];
		char buf[1024 + 1];
		struct sockaddr_in my_addr;
		struct sockaddr_in remote_addr;
		int from, to, flag, length;
        	char *tb = NULL;

		if ((sockfd1 = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			std::cout<<"socket create failed!"<<std::endl;
		}
		assert(sockfd1!=-1);

		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(PORT1);
		my_addr.sin_addr.s_addr = INADDR_ANY;
		bzero(&(my_addr.sin_zero), 8);
		if (bind(sockfd1, (struct sockaddr*) &my_addr, sizeof(struct sockaddr))==-1){
			std::cout<<"bind error!"<<std::endl;
			assert(false);
		}

		if (listen(sockfd1, BACKLOG) == -1) {
			std::cout<<"listen error!"<<std::endl;
			assert(false);
		}
		int isFirst = 1;
		int cc =0;
		int client_fd;
		
		while (1) {
			cc++;
			socklen_t sin_size = sizeof(struct sockaddr_in);
			if ((client_fd = accept(sockfd1, (struct sockaddr*) &remote_addr, &sin_size)) < 0) {
				std::cout<<"accept error!"<<std::endl;
				continue;
			}
			if(isFirst == 1) {//for xgMaster.
				isFirst = 0;
				//std::cout<<"M:Received a connection from"<<inet_ntoa(remote_addr.sin_addr)<<std::endl;
				read(client_fd, buf, 1024);
            			char* msg1 = (char*)"XGACKT";
            			if (send(client_fd, msg1, strlen(msg1), 0) == -1)
                    			std::cout<<"send back XGACKS fail!"<<std::endl;
				fd_master = client_fd;
				//std::cout<<"Master sockID: "<<client_fd<<std::endl;
				continue;
			}

			std::cout<<"W:Received a connection from"<<inet_ntoa(remote_addr.sin_addr)<<std::endl;
			//std::cout<<"client sockID: "<<client_fd<<std::endl;
			/* todo for otherworker */
			int  ret = read_all(client_fd, msg, sizeof(int));
			if(ret == -1)  {
				std::cout<<"Recv Initial data...fail..."<<std::endl;
				assert(false);
			}
			memcpy(&from, msg, sizeof(int));
			std::cout<<"From worker: "<< from<<"fd: "<<client_fd<<std::endl;
			sockfds[from] = client_fd;
			recv_fd ++;
			if(recv_fd == M){ 
				//close(sockfd1);
				//break;
			}
		}
	}

	void *server2(void *info) {
		int sockfd;
		int ret, len;
		char msg[1024];
		char buf[1024 + 1];
		char *tb;
        	char *tient_sockfd = NULL;
		struct sockaddr_in my_addr;
		struct sockaddr_in remote_addr;
		int from, to, flag, length;

		if ((sockfd2 = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
			std::cout<<"socket create failed!"<<std::endl;
		}
		assert(sockfd2!=-1);

		my_addr.sin_family = AF_INET;
		my_addr.sin_port = htons(PORT2);
		my_addr.sin_addr.s_addr = INADDR_ANY;
		bzero(&(my_addr.sin_zero), 8);
		if (bind(sockfd2, (struct sockaddr*) &my_addr, sizeof(struct sockaddr))==-1){
			std::cout<<"bind error!"<<std::endl;
			assert(false);
		}

		if (listen(sockfd2, BACKLOG) == -1) {
			std::cout<<"listen error!"<<std::endl;
			assert(false);
		}
		int client_fd;
		
		while (1) {
			 socklen_t sin_size = sizeof(struct sockaddr_in);
			 if ((client_fd = accept(sockfd2, (struct sockaddr*) &remote_addr, &sin_size)) == -1) {
				std::cout<<"accept error!"<<std::endl;
				continue;
			 }


            /* for single_thread */
            ret = read_all(client_fd, msg, sizeof(int)*4);
            if(ret == -1)  std::cout<<"Recv Initial data...fail..."<<std::endl;
            memcpy(&from, msg, sizeof(int));
            memcpy(&to, msg + sizeof(int), sizeof(int));
            memcpy(&flag, msg + 2*sizeof(int), sizeof(int));
            memcpy(&length, msg + 3*sizeof(int), sizeof(int));

            int f = from % P;
            int t = to % P;
            std::string block_filename;
            if(tb != NULL) free(tb);
            tb = (char*)malloc(length+1);
            ret = read_all(client_fd, tb, length);
			send(client_fd,(char*)"XGACKT", 6, 0);
			if(client_fd > 0) close(client_fd);

            while(exec_interval_buf == -1){} //for first interval
			if(to - from < M && t == exec_interval_buf){
                if(flag == 0){
				    if(to > P * N -1){ 
                        /*
                        block_filename = filename_block_edata<float>(bname, to%P, from%P, P, 0);
                        if (file_exists(block_filename)){                                                                                                                                 
                             size_t fsize = get_filesize(block_filename);
                             if(fsize != length){
                                std::cout<<"edata is bad!"<<std::endl;
                             }    
                             assert(fsize==length);
                         }else{
                             std::cout<<"can not find file: " << block_filename <<std::endl;
                             assert(false);
                        }
                        */
                        __sync_add_and_fetch(&cout_g, 1);
                        writeedatablock(bname, tb, t, f , P, 0,length);
                    }else{
                        __sync_add_and_fetch(&cout_g, 1);
                        pthread_mutex_lock(&tlock);
                        if(inlength[f]==-1){
                            if(inbuf[f] != NULL) free(inbuf[f]);
                            inbuf[f] = (char*)malloc(length+1);
                            memcpy(inbuf[f], tb, length);
                            inlength[f] = length;
                        }else if(inlength[f]>0){
                            assert(inlength[f]==length);
                            memcpy(inbuf[f], tb, length);
                        }
                        pthread_mutex_unlock(&tlock);
                    }
                }else if(flag == 1){
				    if(to > P * N -1){
                        /*
                        block_filename = filename_block_edata<float>(bname, to%P, from%P, P, 1);
                        if (file_exists(block_filename)){                                                                                                                                 
                             size_t fsize = get_filesize(block_filename);
                             if(fsize != length){
                                std::cout<<"edata is bad!"<<std::endl;
                             }    
                             assert(fsize==length);
                         }else{
                             std::cout<<"can not find file: " << block_filename <<std::endl;
                             assert(false);
                        }
                        */
                        __sync_add_and_fetch(&cout_g, 1);
                        writeedatablock(bname, tb, t, f , P, 1,length);
                    }else{
                        __sync_add_and_fetch(&cout_g, 1);
                        pthread_mutex_lock(&olock);
                        if(olength[f]==-1){
                            if(obuf[f] != NULL) free(obuf[f]);
                            obuf[f] = (char*)malloc(length+1);
                            memcpy(obuf[f],tb,length);
                            olength[f] = length;
                        }else if(olength[f]>0){
                            assert(olength[f]==length);
                            memcpy(obuf[f], tb, length);
                        }
                        pthread_mutex_unlock(&olock);
                    }
                }
				//interval_dirt_memory[to % P] --;
				__sync_add_and_fetch(&interval_dirt_memory[to % P], -1);
			} else if(to - from > M || t != exec_interval_buf){
                  if(flag == 0){
                        //check data length
                        /*
                        block_filename = filename_block_edata<float>(bname, to%P, from%P, P, 0);
                        if (file_exists(block_filename)){                                                                                                                                 
                             size_t fsize = get_filesize(block_filename);
                             if(fsize != length){
                                std::cout<<"edata is bad!"<<std::endl;
                             }    
                             assert(fsize==length);
                         }else{
                             std::cout<<"can not find file: " << block_filename <<std::endl;
                             assert(false);
                        }
                        */
                        __sync_add_and_fetch(&cout_g, 1);
                        writeedatablock(bname, tb, t, f , P, 0,length);
                  }else if(flag == 1){
                        //check data length
                        /*
                        block_filename = filename_block_edata<float>(bname, to%P, from%P, P, 1);
                        if (file_exists(block_filename)){                                                                                                                                 
                             size_t fsize = get_filesize(block_filename);
                             if(fsize != length){
                                std::cout<<"edata is bad!"<<std::endl;
                             }    
                             assert(fsize==length);
                         }else{
                             std::cout<<"can not find file: " << block_filename <<std::endl;
                             assert(false);
                        }
                        */
                        __sync_add_and_fetch(&cout_g, 1);
                        writeedatablock(bname, tb, t, f , P, 1,length);
                  }
                   __sync_add_and_fetch(&interval_dirt_disk[to % P], -1);
             }
		}
	}
int connect_worker(char *ip, int i) {
        struct sockaddr_in serv_addr;
	
	int sockfd[COMMTHREADS];
	int c = 0;
        while (( sockfd[i] = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
               std::cout<<"socket error!"<<std::endl;
               return(-1);
        }

        bzero(&serv_addr,sizeof(serv_addr));
        serv_addr.sin_family    = AF_INET;
        serv_addr.sin_port      = htons(PORT1);
        serv_addr.sin_addr.s_addr= inet_addr(ip);

        if(connect(sockfd[i], (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
              std::cout<<"connect error!"<<errno<<std::endl;
              return(-1);
        }
	return(sockfd[i]);
}

int read_all(int fd, void *buf, int n) {
	int nleft;
	int nbytes;
	char *ptr;
	ptr = (char*)buf;
	nleft = n;
	for(; nleft > 0;){
		nbytes = read(fd, ptr, nleft);
		if(nbytes < 0){
			if(errno == EINTR) nbytes = 0;
			else return(-1);
		} else if(nbytes == 0) break;
		nleft -= nbytes;
		ptr += nbytes;
	}
	return(n - nleft);
}

int write_all(int fd, void *buf, int n) {
	int nleft, nbytes;
	char *ptr;
	nleft = n;
	ptr = (char*)buf;
	for(; nleft > 0;){
		nbytes = write(fd, ptr, nleft);
		if(nbytes <= 0){
			if(errno == EINTR) nbytes = 0;
			else return(-1);
		}
		nleft -= nbytes;
		ptr += nbytes;
  	}
	return(n);
}


int write_to_worker_multi(char *ip, int port, void *buf, int n) {
	int fd[COMMTHREADS],cc;
	char msg[COMMTHREADS][1024];
	int ret[COMMTHREADS];
        struct sockaddr_in serv_addr;

	int Bid[COMMTHREADS];   	//the id of sliding block
	int offset[COMMTHREADS]; 	//offset of the sliding block
	int length[COMMTHREADS]; 	//sent length chars

        timeval start_time,end;
        double lasttime;
        gettimeofday(&start_time, NULL);

	cc = n / COMMTHREADS;
        volatile int done = 0;
	omp_set_num_threads(COMMTHREADS);
	#pragma omp parallel for
	for(int i = 0; i < COMMTHREADS; i ++) {
		Bid[i] = 0;
		offset[i] = cc * i;
		length[i] = cc;
		if((i == COMMTHREADS -1) && ((n % COMMTHREADS)>0)) length[i] = n % COMMTHREADS;

	//std::cout<<Bid[i]<<"||===New=======||"<<offset[i]<<"||"<<length[i]<<"   threadid : "<<pthread_self()<<std::endl;
      	/*fd[i] = connect_worker(ip, i);

	if(fd[i] == -1){
           	std::cout<<"Connect to " << ip << " Fail!"<<std::endl;
		//if(fd[i] > 0) close(fd[i]);
           	assert(false);
      	}*/

	/* start:connect to worker. */

        if((fd[i] = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
               std::cout<<"socket error!"<<std::endl;
	       assert(false);
        }

        bzero(&serv_addr,sizeof(serv_addr));
        serv_addr.sin_family    = AF_INET;
        serv_addr.sin_port      = htons(PORT1);
        serv_addr.sin_addr.s_addr= inet_addr(ip);

        if(connect(fd[i], (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
              std::cout<<"connect error!"<<errno<<std::endl;
	      assert(false);
        }
	/* end:connect to worker. */

	//std::cout<<"|"<<fd[i]<<"|"<<std::endl;
	//std::cout<<"i : "<<i<<"|||"<<Bid[i]<<"||||"<<offset[i]<<"||"<<length[i]<<std::endl;
	memcpy(msg[i], (void*)&Bid[i], sizeof(int));
	memcpy(msg[i] + sizeof(int) , (void*)&offset[i], sizeof(int));
	memcpy(msg[i] + 2*sizeof(int), (void*)&length[i], sizeof(int));
        memcpy(&Bid[i], msg[i], sizeof(int));
        memcpy(&offset[i], msg[i] + sizeof(int), sizeof(int));
        memcpy(&length[i], msg[i] + 2*sizeof(int), sizeof(int));
	//std::cout<<"i : "<<i<<"|||"<<Bid[i]<<"||||"<<offset[i]<<"||"<<length[i]<<std::endl;
	ret[i] = write_all(fd[i], msg[i], sizeof(int)*3);
	if(ret[i] == -1) {
                 std::cout<<"send Intial data fail!"<<std::endl;
                 if(fd[i] > 0) close(fd[i]);
                 assert(false);
	}

        /*if (send(fd[i], msg[i], sizeof(int)*3,0) == -1){
                 std::cout<<"send Intial data fail!"<<std::endl;
		 if(fd[i] > 0) close(fd[i]);
		 assert(false);
	}*/

	ret[i] = write_all(fd[i], (void*)((char*)buf + offset[i]), length[i]);
	if(ret[i] == -1){
		std::cout<<"send to "<<ip<<" fail!"<<std::endl;
                if(fd[i] > 0) close(fd[i]);
		assert(false);
	} else {
		recv(fd[i], msg[i], 1024,0);
        	gettimeofday(&end, NULL);
                lasttime = end.tv_sec - start_time.tv_sec + ((double)(end.tv_usec - start_time.tv_usec)) / 1.0E6;
                //std::cout<<"send time:  "<<lasttime<<std::endl;
		__sync_add_and_fetch(&done, 1);
		close(fd[i]);
	}

	}
	while(done < COMMTHREADS){}
	return(0);
}


int write_to_worker(char *ip, int port, void *buf, int from, int to, int flag, int n) { //flag =0 inedge, flag=1 outedge
	int fd;
	char msg[1024];
	int ret;
        struct sockaddr_in serv_addr;

	int Bid;   	//the id of sliding block
	int offset; 	//offset of the sliding block
	int length; 	//sent length chars

        timeval start_time,end;
        double lasttime;
        gettimeofday(&start_time, NULL);

	Bid = 0;
	offset = 0;
	length = n;


	/* start:connect to worker. */

        if((fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
               std::cout<<"socket error!"<<std::endl;
	       assert(false);
        }

        bzero(&serv_addr,sizeof(serv_addr));
        serv_addr.sin_family    = AF_INET;
        serv_addr.sin_port      = htons(port);
        serv_addr.sin_addr.s_addr= inet_addr(ip);

        if(connect(fd, (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
              std::cout<<"connect error!"<<errno<<std::endl;
	      assert(false);
        }
	/* end:connect to worker. */

	//std::cout<<"|"<<fd<<"|"<<std::endl;
	//std::cout<<"i : "<<i<<"|||"<<Bid[i]<<"||||"<<offset[i]<<"||"<<length[i]<<std::endl;
	memcpy(msg, (void*)&from, sizeof(int));
	memcpy(msg + sizeof(int) , (void*)&to, sizeof(int));
	memcpy(msg + 2*sizeof(int) , (void*)&flag, sizeof(int));
	memcpy(msg + 3*sizeof(int), (void*)&length, sizeof(int));
        //memcpy(&Bid, msg, sizeof(int));
        //memcpy(&offset, msg + sizeof(int), sizeof(int));
        //memcpy(&length, msg + 2*sizeof(int), sizeof(int));
	//std::cout<<"i : "<<i<<"|||"<<Bid[i]<<"||||"<<offset[i]<<"||"<<length[i]<<std::endl;
	ret = write_all(fd, msg, sizeof(int)*4);
	if(ret == -1) {
                 std::cout<<"send Intial data fail!"<<std::endl;
                 if(fd > 0) close(fd);
                 assert(false);
	}

	ret = write_all(fd, (void*)((char*)buf), length);
	if(ret == -1){
		std::cout<<"send to "<<ip<<" fail!"<<std::endl;
                if(fd > 0) close(fd);
		assert(false);
	} else {
		recv(fd, msg, 1024,0);
        	gettimeofday(&end, NULL);
                lasttime = end.tv_sec - start_time.tv_sec + ((double)(end.tv_usec - start_time.tv_usec)) / 1.0E6;
                //std::cout<<"send time:  "<<lasttime<<std::endl;
		close(fd);
	}

	return(0);
}

/*
int m_send_to_worker(int sockfd,CLIENT *cl, int p,char * obuf, int olength,int j){
	int ret = 0 ;
	char msg[1024];
if(rdma == 0){
	//memcpy(msg,(void*)&p,sizeof(int));
	//memcpy(msg+sizeof(int),(void*)&olength,sizeof(int));
	//ret =  dpdk_write_all(sockfd,(void*)msg,sizeof(int)*2); //for linux tcp;
	//ret = netdpsock_send(sockfd, (void*)msg, sizeof(int)*2, 0);
	//if(ret == -1){
	//	std::cout<<"inital data  send from "<<p<<" "<<olength<<" fail!"<<std::endl;
	//	assert(false);
	//} 
	//else{
	//	std::cout<<"Send: p is:  "<<p<<" olength: " << olength<<"sockfd: "<<sockfd<<std::endl;
	//}

	ret = dpdk_write_all(sockfd,(void*)(obuf),olength);
	if(ret == -1){
		std::cout<<"send from "<<p<<" fail!"<<std::endl;
		assert(false);
	}else{
		std::cout<<"Send out-edge block successful. length: "<< ret <<std::endl;
	} 
	__sync_fetch_and_sub(&send_job_num,1);
}

//char* s="GoldenGlobalView";
int *ret1;
if(rdma ==1){
	data_argn[j].data.data_len = olength;
	data_argn[j].data.data_val = obuf;	
	data_argn[j].index = p;	
        ret1 = block_write_1(&data_argn[j], cl);
	std::cout<<"Send: p is:  "<<p<<" olength: " << olength<<" j:   "<<j<<std::endl;
        if(ret1==NULL){
                printf("res1 ==NULL\n");
        } 
	__sync_fetch_and_sub(&send_job_num,1);
}

}
*/
int m_recv_to_worker(int sockfd,int * clientfds ,int index,fd_set * pallset){
//	timeval start_time,end;
  //  double lasttime;
//	  gettimeofday(&start_time, NULL);
	char msg[1024];
	int ret = 0 ;
	int p;
	int olength;
	ret = read_all(sockfd,(void*)msg, 2*sizeof(int));	 //for linux tcp
        //ret = netdpsock_recv(sockfd, (void*)msg, 2*sizeof(int), 0);
	if(ret == -1){
		perror("recv initial data error");
		assert(false);
	}
	else{
		memcpy(&p,msg,sizeof(int));
		memcpy(&olength,msg+sizeof(int),sizeof(int));
		//std::cout <<"Recv from :  " <<p <<" olength: "<<olength<<std::endl;
	}
	
	if(inlength[p] != olength){
		std::cout<<"Recv a bad blcok: olength["<<p <<"]="<<olength<<", inlength[" << p << "]=" <<inlength[p]<<std::endl;
		assert(olength == inlength[p]);
	}

	//std::cout<<"Recv: olength["<<p <<"]="<<olength<<", inlength[" << p << "]=" <<inlength[p]<<std::endl;
	ret =read_all(sockfd,inbuf[p],olength); 
	//ret = netdpsock_recv(sockfd, inbuf[p],olength, 0);
	if(ret == -1){
		perror("recv data error");
		assert(false);
	}
//	else{
//		std::cout<<" data  send from "<<p<<" success!"<<std::endl;
//	}
	//clientfds[index] = sockfd;
	//FD_SET(sockfd,pallset);
	//std::cout << "sockfd "<<sockfd<<"ret to FDSET"<<std::endl;
	__sync_fetch_and_sub(&recv_job_num,1);
//	std::cout <<"recv job"<<recv_job_num<<std::endl;
//	gettimeofday(&end, NULL);
//	lasttime = end.tv_sec - start_time.tv_sec + ((double)(end.tv_usec - start_time.tv_usec)) / 1.0E6;
//	std::cout<<"recv time:  "<<lasttime<<std::endl;
	return 1;
}


