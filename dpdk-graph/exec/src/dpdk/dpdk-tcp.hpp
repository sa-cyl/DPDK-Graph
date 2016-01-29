/*
 * This program is used to test netdp user space tcp stack
 * Create by ChengYongLi 2015/11/05
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <termios.h>
#include <sys/epoll.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>

#ifndef __linux__
  #ifdef __FreeBSD__
    #include <sys/socket.h>
  #else
    #include <net/socket.h>
  #endif
#endif

#include <sys/time.h>

extern "C"
     {
	#include "netdpsock_intf.h"
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

extern "C"
     {
	#include "netdp_errno.h"
	#define NETDP_EAGAIN            35
     }


#define BUFFER_SIZE 5000  
#define MAX_EVENTS 10  


int dpdk_write_all(int fd, void *buf, int n);
int dpdk_read_all(int fd, void *buf, int n);
int m_send_to_worker(int sockfd,CLIENT *cl, int p,char * obuf, int olength,int j);

int dpdk_handle_event(struct epoll_event ev)
{
    char recv_buf[BUFFER_SIZE];       
    int len, from, olen;     
    char send_buf[BUFFER_SIZE];       
std::cout<<"..............................................................................."<<std::endl;
    if (ev.events&EPOLLIN)
    {

        while(1)
        {
            len = netdpsock_recvfrom(ev.data.fd, recv_buf, 2*sizeof(int), 0, NULL, NULL);
std::cout<<"================================================================================"<<std::endl;
            if(len > 0)  
            {  
               // sprintf(send_buf, "I have received your message.");
                
                //netdpsock_send(ev.data.fd, send_buf, 2500, 0);  
		memcpy(&from, recv_buf, sizeof(int));
		memcpy(&olen, recv_buf+sizeof(int), sizeof(int));
                printf("receive from client(%d) , data len:%d from: %d     olength[%d]: %d\n", ev.data.fd, len, from, from, olen);  
		if(olen ==inlength[from])
		{
			std::cout<<"Incoming out-edge data block is valid. olength["<<from<<"]:"<<olen<<".... inlength["<<from<<"]:"<<inlength[from]<<std::endl;
			in_infos[from].from = from;
			in_infos[from].len = olen;
			in_infos[from].fd = ev.data.fd;
			in_infos[from].offset = 0;
			recv_fd ++;
		}
		else
		{
			std::cout<<"Incoming out-edge data block is invalid. olength["<<from<<"]:"<<olen<<".... inlength["<<from<<"]:"<<inlength[from]<<std::endl;
			assert(false);
		}
		break;
            } 
            else if(len < 0)
            {
                if (errno == NETDP_EAGAIN)   
                {
                    break;
                }
                else
                {
                    printf("remote close the socket, errno %d \n", errno);
                    netdpsock_close(ev.data.fd);
                    break;
                }
            }
            else
            {
                printf("remote close the socket, len %d \n", len);
                netdpsock_close(ev.data.fd);
                break;
            }

        }
    }
    else if (ev.events&EPOLLERR || ev.events&EPOLLHUP) 
    {
        printf("remote close the socket, event %x \n", ev.events);
        netdpsock_close(ev.data.fd);
    }
    
    return 0;
}

int dpdk_block_event(struct epoll_event ev)
{
    char recv_buf[BUFFER_SIZE];
    int len, olen, offset, from, ptr,p;
    char send_buf[BUFFER_SIZE];

    offset = -1;
    ptr =0;

    int ts1;
    for(int i=0;i<M;i++){
	//std::cout<<in_infos[i].fd<<"     ev fd"<< ev.data.fd<<" olen: "<< in_infos[i].len<<"  from: "<< in_infos[i].from<<std::endl;
	if(i==exec_interval) continue;
	if(in_infos[i].fd == ev.data.fd){
		from=in_infos[i].from;	
		offset=in_infos[i].offset;
		olen=in_infos[i].len;
		p = i;
		break;
	}
    }
    if(offset == -1){
	std::cout<<"Recv out-edge block fail: can not find in-edge block information!"<<std::endl;
	assert(false);
    }

    if (ev.events&EPOLLIN)
    {

        while(1)
        {
            len = netdpsock_recvfrom(ev.data.fd, inbuf[from]+offset+ptr, 1024*64, 0, NULL, NULL);
            if(len > 0)
            {
memcpy(&ts1, inbuf[from]+offset+ptr, sizeof(int));
		ptr += len;
recv_counter += len;
    //std::cout<<"recv bytes: "<<len<<" total "<<recv_counter<< " offset +ptr  :"<<offset+ptr<<"          first number: "<< ts1<<std::endl;
            }
            else if(len < 0)
            {
                if (errno == NETDP_EAGAIN)
                {
                    break;
                }
                else
                {
                    printf("remote close the socket, errno %d \n", errno);
                    netdpsock_close(ev.data.fd);
		    assert(false);
                    //break;
                }
            }
            else
            {
                printf("remote close the socket, len %d \n", len);
                netdpsock_close(ev.data.fd);
		assert(false);
                //break;
            }

        }
    }
    else if (ev.events&EPOLLERR || ev.events&EPOLLHUP)
    {
        printf("remote close the socket, event %x \n", ev.events);
        netdpsock_close(ev.data.fd);
	assert(false);
    }

    in_infos[p].offset = offset +ptr;
    //recv_counter += ptr;
    if(in_infos[p].offset==in_infos[p].len){
	std::cout<<" out-edge data block "<< in_infos[p].from<<"   have been recvied."<<std::endl;
	__sync_fetch_and_sub(&recv_job_num,1);
	in_infos[p].offset = 0;
    }
    return 0;
}
void *dpdk_server(void *info)     
{
    int ret;
    int server_sockfd;
    int client_sockfd;
    struct sockaddr_in my_addr;
    struct sockaddr_in remote_addr;
    socklen_t sin_size;

    ret = netdpsock_init(NULL);
    if(ret != 0)
        printf("init sock failed \n");
   
    memset(&my_addr,0,sizeof(my_addr));
    my_addr.sin_family=AF_INET;
    my_addr.sin_addr.s_addr=INADDR_ANY;
    my_addr.sin_port=htons(8109);
 
    if((server_sockfd=netdpsock_socket(PF_INET,SOCK_STREAM, 0)) < 0)
    {
        printf("socket error \n");
	assert(false);
        //return 1;
    }

    if (netdpsock_bind(server_sockfd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr)) < 0)
    {
        printf("bind error \n");
	assert(false);
        //return 1;
    }   

    if (netdpsock_listen(server_sockfd, 5) < 0)
    {   
        printf("listen error \n");
	assert(false);
        //return 1;
    }  

    sin_size=sizeof(struct sockaddr_in);

    int epoll_fd;
    epoll_fd=netdpsock_epoll_create(MAX_EVENTS);
    if(epoll_fd==-1)
    { 
        printf("epoll_create failed \n");
        netdpsock_close(server_sockfd);
	assert(false);
        //return 1;
    } 
    struct epoll_event ev;
    struct epoll_event events[MAX_EVENTS];
    ev.events=EPOLLIN;
    ev.data.fd=server_sockfd;

    if(netdpsock_epoll_ctl(epoll_fd,EPOLL_CTL_ADD,server_sockfd,&ev)==-1)
    { 
        printf("epll_ctl:server_sockfd register failed");
        netdpsock_close(server_sockfd);
        netdpsock_close(epoll_fd);
	assert(false);
        //return 1;
    } 

    int nfds;

    printf("dpdk tcp server is running \n");

    while(1)
    { 
        nfds=netdpsock_epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if(nfds==-1)
        {
            printf("start epoll_wait failed \n");
            netdpsock_close(server_sockfd);
            netdpsock_close(epoll_fd);
	    assert(false);
            //return 1;
        }
        else if(nfds == 0)
        {
            printf("epoll timeout \n");
            continue;
        }

        int i;
        for(i = 0; i < nfds; i++)
        {
            if(events[i].data.fd==server_sockfd)
            {
                if((client_sockfd = netdpsock_accept(server_sockfd, (struct sockaddr *)&remote_addr,&sin_size)) < 0)
                {
                    printf("accept client_sockfd failed \n");
                    netdpsock_close(server_sockfd);
                    netdpsock_close(epoll_fd);
		    assert(false);
                    //return 1;
                }

                ev.events=EPOLLIN;
                ev.data.fd=client_sockfd;
                if(netdpsock_epoll_ctl(epoll_fd, EPOLL_CTL_ADD,client_sockfd,&ev)==-1)
                {
                    printf("epoll_ctl:client_sockfd register failed \n");
                    netdpsock_close(server_sockfd);
                    netdpsock_close(epoll_fd);
		    assert(false);
                    //return 1;
                }


                printf("dpdk: accept client %s \n",inet_ntoa(remote_addr.sin_addr));
		//recv_fd ++;

                //netdpsock_send(client_sockfd, "I have received your message.", 20, 0);

            }
            else
            {
		ccc1++;
		if(recv_fd > -1){
			ccc2++;
                	ret = dpdk_handle_event(events[i]);
		} else
		{
			ccc3++;
                	ret = dpdk_block_event(events[i]);
		}
            }
        }
    } 
    //return 0;
}

int dpdk_read_all(int fd, void *buf, int n) {
        int nleft;
        int nbytes;
        char *ptr;
        ptr = (char*)buf;
        nleft = n;
std::cout<<"dpdk_recv..................................1.........:n="<<n<<std::endl;
        for(; nleft > 0;){
                //nbytes = read(fd, ptr, nleft); //for linux tcp
                //nbytes = netdpsock_recv(fd, ptr, nleft, 0); //for dpdk tcp
std::cout<<"dpdk_recv..................................2.........:n="<<n<<std::endl;
	        nbytes = netdpsock_recvfrom(fd, ptr, nleft, 0, NULL, NULL);
std::cout<<"dpdk_recv..................................3.........:nbytes="<<nbytes<<std::endl;
                if(nbytes < 0){
std::cout<<"dpdk_recv..................................4.........:n="<<n<<std::endl;
                        if(errno == NETDP_EAGAIN) 
			{
std::cout<<"dpdk_recv..................................5.........:n="<<n<<std::endl;
				nbytes = 0;
			}
                        else 
			{
std::cout<<"dpdk_recv..................................6.........:n="<<n<<std::endl;
				return(-1);
			}
                } else if(nbytes == 0) 
		{
std::cout<<"dpdk_recv..................................7.........:nbytes="<<nbytes<<std::endl;
			continue;
		} 
std::cout<<"dpdk_recv..................................8.........:nbytes="<<nbytes<<std::endl;
                nleft -= nbytes;
                ptr += nbytes;
std::cout<<"dpdk_recv..................................9.........:nbytes="<<nbytes<<std::endl;
        }
std::cout<<"dpdk_recv..................................10.........:nbytes="<<nbytes<<std::endl;
        return(n - nleft);
}

int dpdk_write_all(int fd, void *buf, int n) {
        int nleft, nbytes, c=0;
        char *ptr;
        nleft = n;
        ptr = (char*)buf;
	int from, send_len;
/*
for a test
int ty;
int ofs;
if(n>1000)
{
for(ty=0;ty<=(n/1460);ty++){
ofs = ty*1460;
	memcpy(ptr+ofs, &ty, sizeof(int));
}
}
*/
	//memcpy(&from, buf, sizeof(int));
//std::cout<<"dpdk_send..................................1.........:"<<std::endl;
        for(; nleft > 0;){
//std::cout<<"dpdk_send..................................2.........:"<<std::endl;
		if(nleft>1024*64) send_len = 1024*64;
		else send_len = nleft;
                nbytes = netdpsock_send(fd, ptr, send_len, 0);
		//usleep(20);
//std::cout<<"dpdk_send..................................3.........:  len:"<<nbytes<<std::endl;
                if(nbytes <= 0){
//std::cout<<"dpdk_send..................................4.........:"<<std::endl;
                        if(errno == NETDP_EAGAIN) 
			{
				nbytes = 0;
//std::cout<<"dpdk_send..................................5.........:"<<std::endl;
			}
                        else 
			{
				printf("write error: errno = %d, strerror = %s \n" , errno, strerror(errno));
				assert(false);
			}
                }
                nleft -= nbytes;
                ptr += nbytes;
		c += nbytes;
		//std::cout<<"send bytes: "<<nbytes<<"  total: "<<c<<std::endl;
        }
        return(n);
}


int m_send_to_worker(int sockfd,CLIENT *cl, int p,char * obuf, int olength,int j){
        int ret = 0 ;
        char msg[1024];
if(rdma == 0){
        //memcpy(msg,(void*)&p,sizeof(int));
        //memcpy(msg+sizeof(int),(void*)&olength,sizeof(int));
        //ret =  dpdk_write_all(sockfd,(void*)msg,sizeof(int)*2); //for linux tcp;
        //ret = netdpsock_send(sockfd, (void*)msg, sizeof(int)*2, 0);
        //if(ret == -1){
        //      std::cout<<"inital data  send from "<<p<<" "<<olength<<" fail!"<<std::endl;
        //      assert(false);
        //}
        //else{
        //      std::cout<<"Send: p is:  "<<p<<" olength: " << olength<<"sockfd: "<<sockfd<<std::endl;
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
