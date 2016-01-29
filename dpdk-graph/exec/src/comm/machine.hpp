/*
 * @file
 * @author  YongLi Cheng <sa_cyl@163.com>
 * @version 1.0
 *
 * Copyright [2014] [YongLI Cheng / HuaZhong University]
 */

#include <unistd.h>
#include <iostream>
#include <fstream>
#include <memory.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#include <stdlib.h>
#include <errno.h>
#include <vector>
#include <assert.h>
#include <sys/time.h>
#include <stdio.h>

#define CLIPORT 3333
#define ISSPACE(x) ((x)==' '||(x)=='\r'||(x)=='\n'||(x)=='\f'||(x)=='\b'||(x)=='\t')
#define MAXDATASIZE 30
namespace xGraph {
	char *Trim(char *String)
	{
        	char *Tail, *Head;
        	for(Tail = String + strlen( String ) - 1; Tail >= String; Tail-- )
                	if (!ISSPACE(*Tail)) break;
        	Tail[1] = 0;
        	for(Head = String; Head <= Tail; Head ++ )
                	if (!ISSPACE(*Head )) break;
        	if(Head != String )
                	memcpy( String, Head, (Tail - Head + 2)*sizeof(char));
        	return String;
	}

        int testhost(char *ip){
                int sockfd, recvbytes;
                char buf[MAXDATASIZE];
                struct sockaddr_in serv_addr;

                if (( sockfd = socket(AF_INET, SOCK_STREAM, 0)) == -1) {
                        std::cout<<"socket error!"<<std::endl;
			return(-1);
                }

                bzero(&serv_addr,sizeof(serv_addr));
                serv_addr.sin_family    = AF_INET;
                serv_addr.sin_port      = htons(CLIPORT);
                serv_addr.sin_addr.s_addr= inet_addr(ip);

		//timeval start_time,end;
      		//double lasttime;
		//gettimeofday(&start_time, NULL);
                if (connect(sockfd, (struct sockaddr *)&serv_addr,sizeof(struct sockaddr)) == -1) {
                        //std::cout<<"connect error!"<<std::endl;
        		//gettimeofday(&end, NULL);
        		//lasttime = end.tv_sec - start_time.tv_sec + ((double)(end.tv_usec - start_time.tv_usec)) / 1.0E6;     
			//std::cout<<lasttime<<std::endl;
			return(-1);
                }
        		//gettimeofday(&end, NULL);
        		//lasttime = end.tv_sec - start_time.tv_sec + ((double)(end.tv_usec - start_time.tv_usec)) / 1.0E6;     
			//std::cout<<ip<<"  "<<lasttime<<std::endl;


                memcpy(buf,"XGREQT",6);

                int wret=write(sockfd,buf,6);
                while(wret<6){
                        std::cout<<wret<<std::endl;
                	close(sockfd);
			return(-1);
                }
                if ((recvbytes = recv(sockfd, buf, MAXDATASIZE,0)) == -1) {
                        std::cout<<"recv error!"<<std::endl;
                	close(sockfd);
			return(-1);
                }

                buf[recvbytes] = '\0';
		if(buf[5] == 'T'){ 
                	//std::cout<<"Received: "<<buf<<std::endl;
			return(sockfd);
		}
		return(-1);
        }


	int determine_machines(std::string machineconffile, int m, std::vector<std::pair<int, std::string> > *hosts) {
		int maxlen = 1000;
	 	char * s = (char*) malloc(maxlen);

        	FILE * inf = fopen(machineconffile.c_str(), "r");
        	if (inf == NULL) {
            		std::cout << "Could open :" << machineconffile << " error: " << strerror(errno) << std::endl;
        	}
        	assert(inf != NULL);

		int i = 0;
		int sockfd;
		while(fgets(s, maxlen, inf) != NULL) {
			Trim(s);
			if((sockfd=testhost(s)) >= 0) {
				(*hosts).push_back(std::pair<int,std::string>(sockfd, s));
				i++;
			}
			//std::cout<<s<<std::endl;		
		}

		free(s);
        	fclose(inf);
		if(i > m) return(m);
		return(i);	
	}
};
