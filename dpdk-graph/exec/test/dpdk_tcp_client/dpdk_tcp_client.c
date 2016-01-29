#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <errno.h>
#include <netinet/in.h>
#include <termios.h>
#include <sys/epoll.h>
#include <sys/time.h>

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


//int fd = -1;

#define TCP_CLIENT_SEND_LEN 1024*64
#define BUFFER_SIZE 1024*1024*100

struct epoll_event events[20];


long gettime()  
{     	struct timeval tv;     
	gettimeofday(&tv,NULL);     
	return tv.tv_sec*1000  + tv.tv_usec / 1000;  
}  

long time_start, time_end;
void tcp_send_thread(void *client_fd)
{  
    int data_num = 0;
    int data_len = 0;
    char send_data[5000];
    memset(send_data, 0, sizeof(send_data));
    int send_len = 0;
    int left, send_size, bs;

    char *send_buf;
    int offset=0;
    send_buf = malloc(BUFFER_SIZE+1);
    memset(send_buf, 0, BUFFER_SIZE);

    int fd = *(int *)client_fd;
    left = 1024*1024*100;
    bs =   1024*1024*100;
    while(1)
    {
        if(fd > 0)
        {
            data_num++;
            //sprintf(send_data, "Hello, linux tcp server, num:%d !", data_num);
            send_len = 0;
            
            //printf("1...send len %d, data len:%d \n", send_len, offset);
	    if(left > 1024*3) send_size= 1024*3;
 	    else send_size = left;
            send_len = netdpsock_send(fd, send_buf+offset, send_size, 0);
            //data_len += send_len;

	    if(send_len>0) 
	    {
		offset += send_len;
	    	left -= send_len;
	    }
            //printf("2...send len %d, data len:%d \n", send_len, offset);
            //printf("3...buff_size %d, offset:%d \n", BUFFER_SIZE, offset);
	    if(offset == bs)
	    {
		printf("A block has been sent, length: %d\n", offset);
		break;
	    }
        }
        //usleep(20000);
    }
    
}


int main(void)
{
    int ret[50];
    int i = 0 ;
    int epfd;
    int data_num =0;
    struct sockaddr_in addr_in;  
    struct sockaddr_in remote_addr[50];  
    struct epoll_event event;
    char recv_buf[5000];
    int recv_len; 
    pthread_t id[50];  
    int fd[50];
    int servers;
    int j,t;

    printf("Input a number of servers:");
    scanf("%d", &servers);
    ret[0] = netdpsock_init(NULL);
    if(ret[0] != 0)
        printf("init sock ring failed \n");

    /* create epoll socket */
    epfd = netdpsock_epoll_create(0);
    if(epfd < 0)
    {
        printf("create epoll socket failed \n");
        return -1;
    }

for(j=0; j<servers; j++)
{
printf("step....................................1 \n");
    fd[j] = netdpsock_socket(AF_INET, SOCK_STREAM, 0);	
    if(fd[j] < 0)
    {
        printf("create socket failed \n");
        netdpsock_close(epfd);
        return -1;
    }
printf("step....................................2 \n");

    memset(&remote_addr[j], 0, sizeof(remote_addr));      
    remote_addr[j].sin_family = AF_INET;  
    remote_addr[j].sin_port   = htons(7001+(u_short)j);  
    remote_addr[j].sin_addr.s_addr = inet_addr("192.168.2.193"); 
//    remote_addr.sin_addr.s_addr = htonl(0x03030303); 

printf("step....................................3 \n");
    if(netdpsock_connect(fd[j], (struct sockaddr *)&remote_addr[j], sizeof(struct sockaddr)) < 0)     
    {     
        printf("connect to server failed \n");
        netdpsock_close(fd[j]);
        netdpsock_close(epfd);
        return -1;  
    } 
    
printf("step....................................4 \n");
    event.data.fd = fd[j];  
    event.events = EPOLLIN | EPOLLET;  

    ret[j] = netdpsock_epoll_ctl(epfd, EPOLL_CTL_ADD, fd[j], &event);
    if(ret[j] != 0)
    {
        printf("epoll ctl failed \n");
        netdpsock_close(fd[j]);
        netdpsock_close(epfd);
        return -1;
    }
}
printf("step....................................5 \n");
    printf("start dpdk tcp client application \n");

for(t=0; t<servers; t++)
{
    ret[t]=pthread_create(&id[t], NULL, (void *) tcp_send_thread, &fd[t]);  
    if(ret[t]!=0)  
    {  
        printf ("Create pthread error!\n");  
        return 0;  
    }  
}
printf("step....................................6 \n");
    time_start = gettime();
    int event_num = 0;
    
    while(1)
    {
        event_num = netdpsock_epoll_wait (epfd, events, 20, -1);
        if(event_num <= 0)
        {
            printf("epoll_wait failed \n");
            continue;
        }
            
        for(i = 0; i < event_num; i++)
        {
            if ((events[i].events & EPOLLERR) || (events[i].events & EPOLLHUP) || (!(events[i].events & EPOLLIN)))  
            {  
                printf("dpdk socket(%d) error\n", events[i].data.fd);
                netdpsock_close (events[i].data.fd);  
                //fd = -1;
                continue;  
            }   

            if (events[i].events & EPOLLIN)
            {
                while(1)
                {
                    recv_len = netdpsock_recvfrom(events[i].data.fd, recv_buf, 5000, 0, NULL, NULL);
                    if((recv_len < 0) && (errno == NETDP_EAGAIN))
                    {
                       // printf("no data in socket \n");

                        break;
                    }
                    else if(recv_len < 0)
                    {
                         // socket error
                         //netdpsock_close(fd);
                         break;

                    }

		    time_end = gettime();
                    printf("%s .elapsing time: %ld ms\n", recv_buf, time_end-time_start);

                }
            
            }
            else
            {
                printf("unknow event %x, fd:%d \n", events[i].events, events[i].data.fd);
            }
            
        }
    
    }



    //netdpsock_close(fd);
    netdpsock_close(epfd);

    return 0;
}
