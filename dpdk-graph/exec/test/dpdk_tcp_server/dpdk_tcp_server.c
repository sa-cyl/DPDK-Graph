/*
* This program is used to test netdp user space tcp stack
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

#include "netdpsock_intf.h"
#include "netdp_errno.h"


#define BUFFER_SIZE 5000
#define MAX_EVENTS 10  

char *buf;
int offset = 0;

int dpdk_handle_event(struct epoll_event ev)
{
    char recv_buf[BUFFER_SIZE];       
    int len; 
    int send_len = 0;
    char send_buf[BUFFER_SIZE];       

		//printf("step.............................3\n");
    if (ev.events&EPOLLIN)
    {

        while(1)
        {
            //printf("1...===========I have received your message : %d", offset);
            len = netdpsock_recvfrom(ev.data.fd, buf+offset, 1024*3, 0, NULL, NULL);
            if(len > 0)  
            {  
/*
                sprintf(send_buf, "I have received your message.");
                send_len = netdpsock_send(ev.data.fd, send_buf, 2500, 0);  
                if(send_len < 0)
                {
                    printf("send data failed, send_len %d \n", send_len);
                }

                printf("receive from client(%d) , data len:%d \n", ev.data.fd, len);  
*/
		offset += len;	
                //printf("2...I have received your message : %d  Len: %d\n", offset,len);
    		if(offset == 1024*1024*100)
    		{
                	sprintf(send_buf, "ACK: a bock has been recieved successful!");
                	send_len = netdpsock_send(ev.data.fd, send_buf, 41, 0);  
			printf("A block has been received, length : %d\n", offset);
			break;
    		}
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
    
    if (ev.events&EPOLLERR || ev.events&EPOLLHUP) 
    {
        printf("remote close the socket, event %x \n", ev.events);
        netdpsock_close(ev.data.fd);
    }
    

    return 0;
}

int main(int argc, char * argv[])     
{ 
    int ret;
    int server_sockfd;   
    int client_sockfd;     
    struct sockaddr_in my_addr;      
    struct sockaddr_in remote_addr;     
    int sin_size;   
    u_short  port;


    buf = malloc(1024*1024*100+1);
    memset(buf, 0, 1024*1024*100);

    ret = netdpsock_init(NULL);
    if(ret != 0)
        printf("init sock failed \n");
    

    printf("input a port number:\n");
    scanf("%u", &port);
    memset(&my_addr,0,sizeof(my_addr)); 
    my_addr.sin_family=AF_INET; 
    my_addr.sin_addr.s_addr=INADDR_ANY;   
    my_addr.sin_port=htons(port);    
  
    if((server_sockfd=netdpsock_socket(PF_INET,SOCK_STREAM, 0)) < 0)     
    {       
        printf("socket error \n");     
        return 1;     
    }     

    if (netdpsock_bind(server_sockfd,(struct sockaddr *)&my_addr,sizeof(struct sockaddr)) < 0)     
    {     
        printf("bind error \n");     
        return 1;     
    }     

    if (netdpsock_listen(server_sockfd, 5) < 0)  
    {     
        printf("listen error \n");     
        return 1;     
    }   
    
    sin_size=sizeof(struct sockaddr_in);   

    int epoll_fd;  
    epoll_fd=netdpsock_epoll_create(MAX_EVENTS);  
    if(epoll_fd==-1)  
    {  
        printf("epoll_create failed \n"); 
        netdpsock_close(server_sockfd);
        return 1;     
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
        return 1;     
    }  
    
    int nfds;

    printf("dpdk tcp server is running \n");
    
    while(1)  
    {  
		//printf("step.............................7\n");
        nfds=netdpsock_epoll_wait(epoll_fd, events, MAX_EVENTS, -1);  
        if(nfds==-1)  
        {  
            printf("start epoll_wait failed \n");  
            netdpsock_close(server_sockfd);
            netdpsock_close(epoll_fd);
            return 1;     
        }  
        else if(nfds == 0)
        {
            printf("epoll timeout \n");
            continue;
        }
        
		//printf("step.............................8\n");
        int i;  
        for(i = 0; i < nfds; i++)  
        {  
		//printf("step.............................9\n");
            if(events[i].data.fd==server_sockfd)  
            {  
                if((client_sockfd = netdpsock_accept(server_sockfd, (struct sockaddr *)&remote_addr,&sin_size)) < 0)  
                {     
                    printf("accept client_sockfd failed \n");     
                    netdpsock_close(server_sockfd);
                    netdpsock_close(epoll_fd);
                    return 1;     
                }  
                
                ev.events=EPOLLIN;  
                ev.data.fd=client_sockfd;  
                if(netdpsock_epoll_ctl(epoll_fd, EPOLL_CTL_ADD,client_sockfd,&ev)==-1)  
                {  
                    printf("epoll_ctl:client_sockfd register failed \n");  
                    netdpsock_close(server_sockfd);
                    netdpsock_close(epoll_fd);
                    return 1;     
                }  
                
               
                printf("accept client %s,  family: %d, port %d \n",inet_ntoa(remote_addr.sin_addr), remote_addr.sin_family, remote_addr.sin_port);  
                
                //netdpsock_send(client_sockfd, "I have received your message.", 20, 0);  

            }  
            else  
            {  
		//printf("step.............................1\n");
                ret = dpdk_handle_event(events[i]);
		//printf("step.............................2\n");
            }  
        }  
    }  
    return 0;     
}    
