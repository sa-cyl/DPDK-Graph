/**
 * @file
 * @author  Xiuneng Wang <xiunengwang@hust.edu.cn>
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
 
 *
 * @section DESCRIPTION
 *
 * Sharder converts a graph into shards which the CE_Graph engine
 * can process.
 */

/**
 * @section TODO
 * Change all C-style IO to Unix-style IO.
 */


#ifndef CE_Graph_SHARDER_DEF
#define CE_Graph_SHARDER_DEF


#include <iostream>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include <vector>
#include <omp.h>
#include <errno.h>
#include <sstream>
#include <string>

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "api/chifilenames.hpp"
#include "api/CE_Graph_context.hpp"
#include "CE_Graph_types.hpp"
#include "io/stripedio.hpp"
#include "logger/logger.hpp"
#include "engine/auxdata/degree_data.hpp"
#include "metrics/metrics.hpp"
#include "metrics/reps/basic_reporter.hpp"
#include "shards/memoryshard.hpp"
#include "shards/slidingshard.hpp"
#include "output/output.hpp"
#include "util/ioutil.hpp"
#include "util/radixSort.hpp"
#include "util/kwaymerge.hpp"
#include "util/qsort.hpp"

extern int opti_times;

namespace CE_Graph {
    template <typename VT, typename ET> class sharded_graph_output;
    
    
#define SHARDER_BUFSIZE (64 * 1024 * 1024)
#define BUFFER_SIZE 1024*1024*4
#define INFO_PER_BLOCKS 32
#define PORT 6789
#define MAX_SEND_THREADS 4
#define MAX_RECV_THREADS 4
    
    enum ProcPhase  { COMPUTE_INTERVALS=1, SHOVEL=2 };
    
	/**
	 * modified by Wang
	 * Data:2014-04-17
	 */
	 std::string edge_type;
	 
	 typedef void (* recv_handle)(char * data , size_t datasize , std::string command , void * auxdata);
	 
	 void test_handle(char * data , size_t datasize , std::string command , void * auxdata)
	{
		std::cout << "Handle receives the command :" << command << std::endl;
		std::cout << "Size of its data:" << datasize << std::endl;
		if(data != NULL)
		{
			std::cout << "This is its data:\n" << data << std::endl;
		}
	}
	 
	void sharder_handle(char * data , size_t datasize , std::string command , void * auxdata);
	 
	int CheckDirs(std::string filename)
	{
		std::string dirname;
		size_t found=0;
		found = filename.find_first_of('/');
		while(found != std::string::npos)
		{
			dirname = filename.substr(0,found+1);
			if( access(dirname.c_str() , F_OK) != 0 )
			{
				logstream(LOG_INFO) << "Create dir:" << dirname << std::endl;
				mkdir(dirname.c_str() , 0777);
			}
			found = filename.find_first_of('/' , found+1);
		}
		return 0;
	}

struct length16
{
	char buff[16];
};

struct length24
{
	char buff[24];
};

struct length32
{
	char buff[32];
};

struct length64
{
	char buff[64];
};
	
struct BufferSenderExec
{
	std::string headtip;
	std::string to;
	char * buffer;
	int * doneptr;
	size_t buffsize;
	
	BufferSenderExec(char * buff , size_t size , std::string header , std::string t , int * done):buffer(buff),buffsize(size),headtip(header),to(t),doneptr(done) {}

	int send()
	{
		if(buffer == NULL)
		{
			assert(buffsize == 0);
		}
		
		int sockfd = socket(AF_INET , SOCK_STREAM , 0);
		if(sockfd < 0)
		{
			logstream(LOG_ERROR) << "Could not create socket fd for transferring command " << headtip << std::endl;
			return -1;
		}
		sockaddr_in servaddr;
		memset(&servaddr , 0 , sizeof(servaddr));
		servaddr.sin_family = AF_INET;
		servaddr.sin_port = htons(PORT);
		inet_pton(AF_INET , to.c_str() , &servaddr.sin_addr);
		if ( connect(sockfd , (sockaddr*)&servaddr , sizeof(servaddr) ) < 0 )
		{
			logstream(LOG_ERROR) << "Could not connect to " << to << " for transferring command " << headtip << std::endl;
			close(sockfd);
			return -2;
		}
		
		int head_size = ((headtip.length() + 2 + sizeof(int) + sizeof(long))>128)?(headtip.length()+2+sizeof(int)+sizeof(long)):128;
		long sz = buffsize;
		char * temp_buff = (char*)malloc(head_size);
		
		assert(temp_buff != NULL);
		
		temp_buff[sizeof(int)] = 'C';
		
		strcpy(temp_buff + sizeof(int) + 1, headtip.c_str());
		
		int totallength = strlen(temp_buff+sizeof(int))+1;
		
		*(int *)temp_buff = totallength;
		
		*(long *)(temp_buff + sizeof(int) + totallength) = sz;
		
		logstream(LOG_DEBUG) << "Head tip:" << temp_buff + sizeof(int) << std::endl;
		
		writea(sockfd , temp_buff , totallength + sizeof(int) + sizeof(long) );
		
		if( (buffer != NULL) && (buffsize > 0)  )
		{
			writea(sockfd , buffer , buffsize);
		}
		
		logstream(LOG_DEBUG) << "Sent command " << headtip << " Length:" << buffsize << std::endl;
		
		shutdown(sockfd , 1);
		
		int result = reada(sockfd , temp_buff , 7);
		
		close(sockfd);
		
		if(result < 0)
		{
			logstream(LOG_ERROR) << "Received nothing from " << to << " after transferred command " << headtip << std::endl;
			free(temp_buff);
			return -3;
		}
		
		logstream(LOG_DEBUG) << "Received:" << temp_buff << " from " << to << std::endl;
		
		free(temp_buff);
		
		return 0;
	}
};

struct FileSenderExec
{
	std::string filename;
	std::string to;
	
	FileSenderExec(std::string name , std::string t):filename(name) , to(t) {}
	
	int send()
	{
		int times=0;
		char * buff;
		buff = (char*)malloc(BUFFER_SIZE);
		while(buff == NULL && times < 3)
		{
			usleep(100);
			buff = (char *)malloc(BUFFER_SIZE);
			times++;
		}
		if(NULL == buff)
		{
			logstream(LOG_ERROR) << "No enough buffer for transferring file " << filename << std::endl;
			return -1;
		}
		int f = open(filename.c_str() , O_RDONLY , S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
		if( f < 0)
		{
			logstream(LOG_ERROR) << "Could not open file " << filename << std::endl;
			free(buff);
			return -2;
		}
		off_t sz = lseek(f, 0, SEEK_END);
		lseek(f,0,SEEK_SET);
		
		logstream(LOG_DEBUG) <<"filename:" << filename << " filesize:" << sz << std::endl;
		
		int sockfd = socket(AF_INET , SOCK_STREAM , 0);
		if(sockfd < 0)
		{
			logstream(LOG_ERROR) << "Could not create socket fd for transferring file " << filename << std::endl;
			free(buff);
			close(f);
			return -3;
		}
		sockaddr_in servaddr;
		memset(&servaddr , 0 , sizeof(servaddr));
		servaddr.sin_family = AF_INET;
		servaddr.sin_port = htons(PORT);
		inet_pton(AF_INET , to.c_str() , &servaddr.sin_addr);
		if ( connect(sockfd , (sockaddr*)&servaddr , sizeof(servaddr) ) < 0 )
		{
			logstream(LOG_ERROR) << "Could not connect to " << to << " for transferring file " << filename << std::endl;
			logstream(LOG_ERROR) << strerror(errno) << std::endl;
			free(buff);
			close(f);
			close(sockfd);
			return -4;
		}
		
		char size_buff[20];
		sprintf(size_buff , "%ld" , sz);
		sprintf(buff+sizeof(int) , "Ffilename:%s filesize:%s" , filename.c_str() , size_buff);
		int totallength = strlen(buff+sizeof(int))+1;
		
		*(int *)buff = totallength;
		
		*(long *)(buff + sizeof(int) + totallength) = sz;
		
		logstream(LOG_DEBUG) << "Head tip:" << buff + sizeof(int) << std::endl;
		
		writea(sockfd , buff , totallength + sizeof(int) + sizeof(long) );
		
		long nsent = 0;
		int temp , blocks=0;
		while(nsent < sz)
		{
			if(blocks == INFO_PER_BLOCKS)
			{
				std::cout << "Sent " << (nsent >> 20) << " Mbytes of file " << filename << std::endl;  
				blocks=-1;
			}
			blocks++;
			temp = reada(f , buff , BUFFER_SIZE);
			nsent += temp;
			writea(sockfd , buff , temp);
		};
		
		assert(nsent == sz);
		
		logstream(LOG_DEBUG) << "Sent " << nsent << " bytes of file " << filename << std::endl;
		
		shutdown(sockfd , 1);
		
		int result = reada(sockfd , buff , 7);
		
		close(sockfd);
		close(f);
		
		if(result < 0)
		{
			logstream(LOG_ERROR) << "Received nothing from " << to << " after transferred file " << filename << std::endl;
			return -6;
		}
		
		logstream(LOG_DEBUG) << "Received:" << buff << " from " << to << std::endl;
		free(buff);
		
		return 0;
	}
};


struct ReceiveExec
{
	int connfd;
	recv_handle handle;
	void * auxdata;
	
	ReceiveExec(int fd , recv_handle hle , void * aux = NULL):connfd(fd),handle(hle),auxdata(aux) {}
	
	int recv()
	{
		char * headtip = (char*)malloc(256);
		int headbuffsize = 256;
		long nread = reada(connfd , headtip , headbuffsize);
		
		int realheadsize = *(int*)(headtip);
		realheadsize += sizeof(int) + sizeof(long);
		
		logstream(LOG_DEBUG) << "Receiving:" << "Head size:" << realheadsize - sizeof(int) - sizeof(long) << std::endl;
		
		if( realheadsize > headbuffsize )
		{
			headtip = (char*) realloc(headtip , realheadsize );
			assert(headtip != NULL);
			logstream(LOG_DEBUG) << "Realloc for head buff" << std::endl;
			reada(connfd , headtip+headbuffsize , realheadsize - headbuffsize);
			nread=headbuffsize=realheadsize;
		}
		
		long sz = *(long*)(headtip+realheadsize-sizeof(long));
		
		logstream(LOG_DEBUG) << "Receiving:" << "Data size:" << sz << std::endl;
		
		nread -= realheadsize;
		
		std::string head(headtip+sizeof(int)+1);
		
		assert(head.length() == (realheadsize-sizeof(int)-sizeof(long)-2) );
		
		if(headtip[sizeof(int)] == 'C')
		{
			char * buff=NULL;
			
			if(sz > 0)
			{
				buff = (char*)malloc(sz);
				int times=0;
				while(buff == NULL && times < 3)
				{
					usleep(100);
					buff = (char *)malloc(BUFFER_SIZE);
					times++;
				}
				if(NULL == buff)
				{
					logstream(LOG_ERROR) << "No enough buffer for receiving data of Command " << head << std::endl;
					free(headtip);
					return -1;
				}
				if(nread > 0)
				{
					memcpy(buff , headtip+realheadsize , nread);
				}
				if(nread < sz)
				{
					nread+=reada(connfd , buff+nread , sz-nread);
				}
			}
			
			logstream(LOG_DEBUG) << "Excepted " << sz << " bytes , Received " << nread << " bytes of command " << head << std::endl;
			
			assert(nread == sz);
			
			if(handle == NULL)
			{
				logstream(LOG_WARNING) << "Recv_Handle is not reset , data of command " << head << " is discarded" << std::endl;
			}
			else
			{
				handle(buff , sz , head , auxdata);
			}
			free(buff);
			logstream(LOG_DEBUG) << "Finished Receiving Command " << head << std::endl;
		}
		else if(headtip[sizeof(int)] == 'F')
		{
			int times=0;
			char * buff;
			buff = (char*)malloc(BUFFER_SIZE);
			while(buff == NULL && times < 3)
			{
				usleep(100);
				buff = (char *)malloc(BUFFER_SIZE);
				times++;
			}
			if(NULL == buff)
			{
				logstream(LOG_ERROR) << "No enough buffer for receiving file " << std::endl;
				free(headtip);
				return -1;
			}
			
			
			std::string filename = head.substr(head.find_first_of(':')+1 , head.find_first_of(' ')-head.find_first_of(':')-1 );
			std::string filesize = head.substr(head.find_last_of(':')+1);
			long datalength = atol(filesize.c_str());
			
			assert(sz == datalength);
			
			CheckDirs(filename);
			
			logstream(LOG_INFO) << "Start receiving file: " << filename << "\nfilesize:" << filesize << std::endl;
			
			int f = open(filename.c_str() , O_WRONLY | O_CREAT , S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
			if( f < 0 )
			{
				logstream(LOG_ERROR) << " Could not open file " << filename << " for writing!" << std::endl;
				free(buff);
				free(headtip);
				return -2;
			}
			
			if( nread > 0 )
			{
				writea(f , headtip+realheadsize , nread);
			}
			
			int temp , blocks=0;
			
			while(nread < sz)
			{
				if(blocks == INFO_PER_BLOCKS)
				{
					logstream(LOG_INFO) << "Received " << (nread >> 20) << " Mbytes of file " << filename << std::endl;  
					blocks=-1;
				}
				blocks++;
				temp = reada(connfd , buff , BUFFER_SIZE);
				nread += temp;
				writea(f , buff , temp);
			};
			close(f);
			free(buff);
			logstream(LOG_INFO) << "Finished Receiving file " << filename << std::endl;
		}
		logstream(LOG_DEBUG) << "Received " << nread << " data bytes in total" << std::endl; 
		
		assert(nread == sz);
		
		sprintf(headtip, "200 OK"); 
		
		writea(connfd , headtip , strlen(headtip)+1 );
		
		free(headtip);
		
		return 0;
	}
};

void * buffsend_run(void * _info)
{
	BufferSenderExec * bse = (BufferSenderExec*) _info;
	bse->send();
	if(bse->doneptr != NULL)
	{
		*(bse->doneptr) = 1;
	}
	else
	{
		if(bse->buffer != NULL)
		{
			free(bse->buffer);
		}
	}
	delete bse;
	return NULL;
}

void * filesend_run(void * _info) 
{
	FileSenderExec * fse = (FileSenderExec*) _info;
	fse->send();
	delete fse;
	return NULL;
}

void * recv_run(void * _info)
{
	ReceiveExec * re = (ReceiveExec*) _info;
	re->recv();
	close(re->connfd);
	delete re;
	return NULL;
}

struct WaiterExec
{
	int listenfd;
	std::vector<pthread_t> * receivers;
	recv_handle handle;
	void * auxdata;
	
	WaiterExec(int fd , std::vector<pthread_t> * reces , recv_handle hle , void * aux=NULL):listenfd(fd),receivers(reces),handle(hle),auxdata(aux) {}
	
	int wait()
	{
		int connfd;
		socklen_t clilen;
		sockaddr_in cliaddr;
		pthread_setcancelstate(PTHREAD_CANCEL_ENABLE , NULL);
		pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED , NULL);
		while(1)
			{
				//logstream(LOG_DEBUG) << "OK , Now Waiting " << std::endl;
				clilen = sizeof(cliaddr);
				pthread_testcancel();
				connfd = accept(listenfd , (sockaddr*) &cliaddr , &clilen);
				pthread_testcancel();
				logstream(LOG_DEBUG) << "Accepted" << std::endl;
				if( connfd < 0 )
				{
					logstream(LOG_ERROR) << "Listen Error" << std::endl;
					close(listenfd);
					return -4;
				}
				ReceiveExec * re = new ReceiveExec(connfd , handle , auxdata);
				if( receivers->size() > MAX_RECV_THREADS )
				{
					for(int iter = 0 ; iter != receivers->size() ; ++iter)
					{
						pthread_join( (*receivers)[iter] , NULL);
					}
					receivers->clear();
				}
				pthread_t t;
				int ret = pthread_create(&t , NULL , recv_run , (void*)re);
				assert(ret>=0);
				receivers->push_back(t);
			}
		return 0;
	}
};

void * wait_run(void * _info)
{
	WaiterExec * we = (WaiterExec*) _info;
	we->wait();
	return NULL;
}

class DataTransfer
{
	public:
		DataTransfer()
		{
			senders.clear();
			receivers.clear();
			listenfd = -1;
			waiter = (pthread_t)-1;
			handle=NULL;
			auxdata=NULL;
		}
		
		~DataTransfer()
		{
			if(waiter != (pthread_t)-1)
			{
				CancelWaiting();
			}
			if(!senders.empty())
			{
				for(int iter = 0 ; iter != senders.size() ; ++iter)
				{
					pthread_join(senders[iter] , NULL);
				}
			}
			if(!receivers.empty())
			{
				for(int iter = 0 ; iter != receivers.size() ; ++iter)
				{
					pthread_join(receivers[iter] , NULL);
				}
			}
		}
		
		recv_handle SetRecvHandle(recv_handle hle)
		{
			recv_handle old = handle;
			handle = hle;
			if(waiter != (pthread_t)-1)
			{
				CancelWaiting();
				WaitForDatas();
			}
			return old;
		}
		
		void * SetAuxData(void * aux)
		{
			void * old = auxdata;
			auxdata = aux;
			if(waiter != (pthread_t)-1)
			{
				CancelWaiting();
				WaitForDatas();
			}
			return old;
		}
		
		int AddTask(std::string filename , std::string to)
		{
			logstream(LOG_DEBUG) << "New Task:" << filename << " to the " << to << std::endl;
			std::string::reverse_iterator ri = filename.rbegin();
			if(*ri == '/')
			{
				logstream(LOG_DEBUG) << filename << " is a director , will send all files in it" << std::endl;
				DIR * dirptr = NULL;
				dirent * entry = NULL;
				if( (dirptr = opendir(filename.c_str())) == NULL )
				{
					logstream(LOG_ERROR) << "Failed to open director " << filename << std::endl;
					return -1;
				}
				while(entry = readdir(dirptr))
				{
					if( (0 == strcmp(".",entry->d_name)) || (0 == strcmp("..",entry->d_name)) )
					{
						continue;
					}
					std::string subfilename = filename + entry->d_name;
					if(entry->d_type == DT_DIR)
					{
						subfilename+='/';
					}
					AddTask(subfilename , to);
				}
				closedir(dirptr);
				return 0;
			}
			FileSenderExec * fse = new FileSenderExec(filename , to);
			if( senders.size() > MAX_SEND_THREADS )
			{
				for(int iter = 0 ; iter != senders.size() ; ++iter)
				{
					pthread_join(senders[iter] , NULL);
				}
				senders.clear();
			}
			pthread_t t;
			int ret = pthread_create(&t , NULL , filesend_run , (void*)fse);
			assert(ret>=0);
			senders.push_back(t);
			return 0;
		}
		
		int AddTask(char * buffer , size_t buffsize , std::string headtip , std::string to , int * doneptr) 
		{
			logstream(LOG_DEBUG) << "New Task:Command " << headtip << " to the " << to << std::endl;
			BufferSenderExec * bse = new BufferSenderExec(buffer , buffsize, headtip , to , doneptr);
			if( senders.size() > MAX_SEND_THREADS )
			{
				for(int iter = 0 ; iter != senders.size() ; ++iter)
				{
					pthread_join(senders[iter] , NULL);
				}
				senders.clear();
			}
			pthread_t t;
			int ret = pthread_create(&t , NULL , buffsend_run , (void*)bse);
			assert(ret>=0);
			senders.push_back(t);
			return 0;
		}
		
		int WaitForDatas()
		{
			if(handle == NULL)
			{
				logstream(LOG_WARNING) << "RECV_HANDLE IS NOT SET , COMMAND MODE IS NOT SUPPORTED" << std::endl;
			}
			if(listenfd != -1)
			{
				assert(false);
			}
			sockaddr_in servaddr;
			
			listenfd = socket(AF_INET , SOCK_STREAM , 0);
			if(listenfd < 0)
			{
				logstream(LOG_ERROR) << "Could not create socket fd for listening!" << std::endl;
				return -1;
			}
			memset(&servaddr , 0 , sizeof(servaddr));
			servaddr.sin_family = AF_INET;
			servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
			servaddr.sin_port = htons(PORT);
			
			if ( bind(listenfd , (sockaddr*) &servaddr , sizeof(servaddr) ) < 0 )
			{
				logstream(LOG_ERROR) << "Bind fail!" << std::endl;
				close(listenfd);
				return -2;
			}
			
			if ( listen(listenfd , 5) < 0 )
			{
				logstream(LOG_ERROR) << "Listen fail!" << std::endl;
				close(listenfd);
				return -3;
			}
			
			
			WaiterExec * we = new WaiterExec(listenfd , &receivers , handle , auxdata);
			int ret = pthread_create(&waiter , NULL , wait_run , (void*)we);
			assert(ret>=0);
			
			return 0;
		}
		
		int CancelWaiting()
		{
			if(waiter != (pthread_t)-1 )
			{
				pthread_cancel(waiter);
				pthread_join(waiter , NULL);
				close(listenfd);
				listenfd = -1;
				waiter = (pthread_t)-1;
			}
			logstream(LOG_DEBUG) << "Stop waiting" << std::endl;
			return 0;
		}
	private:
		std::vector<pthread_t> senders;
		std::vector<pthread_t> receivers;
		pthread_t waiter;
		int listenfd;
		recv_handle handle;
		void * auxdata;
	
};
	 
	/**
	 * End modification
	 */
	
	typedef uint8_t shard_t;
	
    template <typename EdgeDataType>
    class DuplicateEdgeFilter {
    public:
        virtual bool acceptFirst(EdgeDataType& first, EdgeDataType& second) = 0;
		virtual ~DuplicateEdgeFilter() {}
    };
    
    
    template <typename EdgeDataType>
    struct edge_with_value {
        vid_t src;
        vid_t dst;
        EdgeDataType value;
        
#ifdef DYNAMICEDATA
        // For dynamic edge data, we need to know if the value needs to be added
        // to the vector, or are we storing an empty vector.
        bool is_chivec_value;
        uint16_t valindex;
#endif
        edge_with_value() {}
        
        edge_with_value(vid_t src, vid_t dst, EdgeDataType value) : src(src), dst(dst), value(value) {
#ifdef DYNAMICEDATA
            is_chivec_value = false;
            valindex = 0;
#endif
        }
        
        // Order primarily by dst, then by src
        bool operator< (edge_with_value<EdgeDataType> &x2) {
            return (dst < x2.dst);
        }
        
        
        bool stopper() { return src == 0 && dst == 0; }
    };
    
    template <typename EdgeDataType>
    bool edge_t_src_less(const edge_with_value<EdgeDataType> &a, const edge_with_value<EdgeDataType> &b) {
        if (a.src == b.src) {
#ifdef DYNAMICEDATA
            if (a.dst == b.dst) {
                return a.valindex < b.valindex;
            }
#endif
            return a.dst < b.dst;
        }
        return a.src < b.src;
    }
    
    template <typename EdgeDataType>
    bool edge_t_dst_less(const edge_with_value<EdgeDataType> &a, const edge_with_value<EdgeDataType> &b) {
        return a.dst < b.dst;
    }
    
    template <class EdgeDataType>
    struct dstF {inline vid_t operator() (edge_with_value<EdgeDataType> a) {return a.dst;} };
    
    template <class EdgeDataType>
    struct srcF {inline vid_t operator() (edge_with_value<EdgeDataType> a) {return a.src;} };
    
  
    
    template <typename EdgeDataType>
    struct shard_flushinfo {
        std::string shovelname;
        size_t numedges;
        edge_with_value<EdgeDataType> * buffer;
        vid_t max_vertex;
        
        shard_flushinfo(std::string shovelname, vid_t max_vertex, size_t numedges, edge_with_value<EdgeDataType> * buffer) :
        shovelname(shovelname), numedges(numedges), buffer(buffer), max_vertex(max_vertex) {}
        
        void flush() {
            /* Sort */
            // TODO: remove duplicates here!
            logstream(LOG_INFO) << "Sorting shovel: " << shovelname << ", max:" << max_vertex << std::endl;
            iSort(buffer, (intT)numedges, (intT)max_vertex, dstF<EdgeDataType>());
            logstream(LOG_INFO) << "Sort done." << shovelname << std::endl;
            int f = open(shovelname.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            writea(f, buffer, numedges * sizeof(edge_with_value<EdgeDataType>));
            close(f);
            free(buffer);
        }
    };
    
    // Run in a thread
    template <typename EdgeDataType>
    static void * shard_flush_run(void * _info) {
        shard_flushinfo<EdgeDataType> * task = (shard_flushinfo<EdgeDataType>*)_info;
        task->flush();
        return NULL;
    }
    
    
    template <typename EdgeDataType>
    struct shovel_merge_source : public merge_source<edge_with_value<EdgeDataType> > {
        
        size_t bufsize_bytes;
        size_t bufsize_edges;
        std::string shovelfile;
        size_t idx;
        size_t bufidx;
        edge_with_value<EdgeDataType> * buffer;
        int f;
        size_t numedges;
        
        shovel_merge_source(size_t bufsize_bytes, std::string shovelfile) : bufsize_bytes(bufsize_bytes), 
        shovelfile(shovelfile), idx(0), bufidx(0) {
            assert(bufsize_bytes % sizeof(edge_with_value<EdgeDataType>) == 0);
            f = open(shovelfile.c_str(), O_RDONLY);
            
            if (f < 0) {
                logstream(LOG_ERROR) << "Could not open shovel file: " << shovelfile << std::endl;
                printf("Error: %d, %s\n", errno, strerror(errno));
            }
            
            assert(f>=0);
            
            buffer = (edge_with_value<EdgeDataType> *) malloc(bufsize_bytes);
            numedges =   (get_filesize(shovelfile) / sizeof(edge_with_value<EdgeDataType> ));
            bufsize_edges =   (bufsize_bytes / sizeof(edge_with_value<EdgeDataType>));
            load_next();
        }
        
        virtual ~shovel_merge_source() {
            if (buffer != NULL) free(buffer);
            buffer = NULL;
        }
        
        void finish() {
            close(f);
            //remove(shovelfile.c_str());

            free(buffer);
            buffer = NULL;
        }
        
        void load_next() {
            size_t len = std::min(bufsize_bytes,  ((numedges - idx) * sizeof(edge_with_value<EdgeDataType>)));
            preada(f, buffer, len, idx * sizeof(edge_with_value<EdgeDataType>));
            bufidx = 0;
        }
        
        bool has_more() {
            return idx < numedges;
        }
        
        edge_with_value<EdgeDataType> next() {
            if (bufidx == bufsize_edges) {
                load_next();
            }
            idx++;
            if (idx == numedges) {
                edge_with_value<EdgeDataType> x = buffer[bufidx++];
                finish();
                return x;
            }
            return buffer[bufidx++];
        }
    };
    
	
    template <typename EdgeDataType, typename FinalEdgeDataType=EdgeDataType>
    class sharder : public merge_sink<edge_with_value<EdgeDataType> > {
        
        typedef edge_with_value<EdgeDataType> edge_t;
        
    protected:
        std::string basefilename;
        
        vid_t max_vertex_id;
        
        /* Sharding */
        int nshards;
        std::vector< std::pair<vid_t, vid_t> > intervals;
        
        int phase;
        
        int vertexchunk;
        size_t nedges;
        std::string prefix;
        
        int compressed_block_size;
        
        int * bufptrs;
        size_t bufsize;
        size_t edgedatasize;
        size_t ebuffer_size;
        size_t edges_per_block;
        
        vid_t filter_max_vertex;
        
        DuplicateEdgeFilter<EdgeDataType> * duplicate_edge_filter;
        
        bool no_edgevalues;
#ifdef DYNAMICEDATA
        edge_t last_added_edge;
		size_t realedges;
#endif
        
        metrics m;
        
		DataTransfer dt;
		
        size_t curshovel_idx;
        size_t shovelsize;
        int numshovels;
        size_t shoveled_edges;
        bool shovel_sorted;
        edge_with_value<EdgeDataType> * curshovel_buffer;
        std::vector<std::string> refiles;
		std::vector<pthread_t> shovelthreads;
		shard_t * vertex_to_bucks;
		vid_t * vertexs_map;
		long * vertexs_in_bucks;
		long * edges_in_bucks;
		int sub_done;
        
		std::vector<std::string> IPs;
    public:
        
        sharder(std::string basefilename) : basefilename(basefilename), m("sharder") {          
            
            edgedatasize = sizeof(FinalEdgeDataType);
            no_edgevalues = false;
            compressed_block_size = 4096 * 1024;
            filter_max_vertex = 0;
            curshovel_buffer = NULL;
            while (compressed_block_size % sizeof(FinalEdgeDataType) != 0) compressed_block_size++;
            edges_per_block = compressed_block_size / sizeof(FinalEdgeDataType);
            duplicate_edge_filter = NULL;
			vertex_to_bucks = NULL;
			vertexs_map = NULL;
			vertexs_in_bucks = NULL;
			edges_in_bucks = NULL;
			degrees = NULL;
#ifdef DYNAMICEDATA
			realedges=0;
#endif
        }
        
        
        virtual ~sharder() {
            if (curshovel_buffer != NULL) free(curshovel_buffer);
        }
		
        void set_duplicate_filter(DuplicateEdgeFilter<EdgeDataType> * filter) {
            this->duplicate_edge_filter = filter;
        }
        
        void set_max_vertex_id(vid_t maxid) {
            filter_max_vertex = maxid;
        }
        
        void set_no_edgevalues() {
            no_edgevalues = true;
        }
        
		void set_skip_shovel(int num , long maxid , long edges)
		{
			numshovels = num;
			max_vertex_id = maxid;
			shoveled_edges = edges;
		}
		
        /**
         * Call to start a preprocessing session.
         */
        void start_preprocessing() {
            m.start_time("preprocessing");
            numshovels = 0;
            shovelsize = (1024l * 1024l * size_t(get_option_int("membudget_mb", 1024)) / 4l / sizeof(edge_with_value<EdgeDataType>));
            curshovel_idx = 0;
            
            logstream(LOG_INFO) << "Starting preprocessing, shovel size: " << shovelsize << std::endl;
            
            curshovel_buffer = (edge_with_value<EdgeDataType> *) calloc(shovelsize, sizeof(edge_with_value<EdgeDataType>));
            
            assert(curshovel_buffer != NULL);
            
            shovelthreads.clear();
            
            /* Write the maximum vertex id place holder - to be filled later */
            max_vertex_id = 0;
            shoveled_edges = 0;
        }
        
        /**
         * Call to finish the preprocessing session.
         */
        void end_preprocessing() {
            m.stop_time("preprocessing");
            flush_shovel(false);
			
			char infofile[128];
			sprintf(infofile , "%s%d.shoveled" , basefilename.c_str() , sizeof(EdgeDataType));
			FILE * fp = fopen(infofile , "w");
			if( NULL == fp )
			{
				logstream(LOG_WARNING) << "Couldn't open file " << infofile << ", which will lead reading the basefile again next time." << std::endl;
			}
			else
			{
				fprintf(fp , "%d\n%d\n%ld" , numshovels , max_vertex_id , shoveled_edges);
				fclose(fp);
			}
			
        }
        
        void flush_shovel(bool async=true) {
            /* Flush in separate thread unless the last one */
            shard_flushinfo<EdgeDataType> * flushinfo = new shard_flushinfo<EdgeDataType>(shovel_filename(numshovels), max_vertex_id, curshovel_idx, curshovel_buffer);
            
            if (!async) {
                curshovel_buffer = NULL;
                flushinfo->flush();
                
                /* Wait for threads to finish */
                logstream(LOG_INFO) << "Waiting shoveling threads..." << std::endl;
                for(int i=0; i < (int)shovelthreads.size(); i++) {
                    pthread_join(shovelthreads[i], NULL);
                }
            } else {
                if (shovelthreads.size() > 2) {
                    logstream(LOG_INFO) << "Too many outstanding shoveling threads..." << std::endl;

                    for(int i=0; i < (int)shovelthreads.size(); i++) {
                        pthread_join(shovelthreads[i], NULL);
                    }
                    shovelthreads.clear();
                }
                curshovel_buffer = (edge_with_value<EdgeDataType> *) calloc(shovelsize, sizeof(edge_with_value<EdgeDataType>));
                pthread_t t;
                int ret = pthread_create(&t, NULL, shard_flush_run<EdgeDataType>, (void*)flushinfo);
                shovelthreads.push_back(t);
                assert(ret>=0);
            }
            numshovels++;
            curshovel_idx=0;
        }
        
        /**
         * Add edge to be preprocessed with a value.
         */
        void preprocessing_add_edge(vid_t from, vid_t to, EdgeDataType val, bool input_value=false) {
            if (from == to) {
                // Do not allow self-edges
                return;
            }  
            edge_with_value<EdgeDataType> e(from, to, val);
#ifdef DYNAMICEDATA
            e.is_chivec_value = input_value;
            if (e.src == last_added_edge.src && e.dst == last_added_edge.dst) {
                e.valindex = last_added_edge.valindex + 1;
            }
            last_added_edge = e;
#endif
            curshovel_buffer[curshovel_idx++] = e;
            if (curshovel_idx == shovelsize) {
                flush_shovel();
            }
            
            max_vertex_id = std::max(std::max(from, to), max_vertex_id);
            shoveled_edges++;
        }
        
#ifdef DYNAMICEDATA
        void preprocessing_add_edge_multival(vid_t from, vid_t to, std::vector<EdgeDataType> & vals) {
            typename std::vector<EdgeDataType>::iterator iter;
            for(iter=vals.begin(); iter != vals.end(); ++iter) {
                preprocessing_add_edge(from, to, *iter, true);
            }
            max_vertex_id = std::max(std::max(from, to), max_vertex_id);
        }
        
#endif
        
        /**
         * Add edge without value to be preprocessed
         */
        void preprocessing_add_edge(vid_t from, vid_t to) {
            preprocessing_add_edge(from, to, EdgeDataType());
        }
        
        /** Buffered write function */
        /* template <typename T>
        void bwrite(int f, char * buf, char * &bufptr, T val) {
            if (bufptr + sizeof(T) - buf >= SHARDER_BUFSIZE) {
                writea(f, buf, bufptr - buf);
                bufptr = buf;
            }
            *((T*)bufptr) = val;
            bufptr += sizeof(T);
        } */
        
		template <typename T>
        void bwrite(int f, int f_s , char * buf, char * & buf_s , char * &bufptr, T val) {
            if (bufptr + sizeof(T) - buf >= SHARDER_BUFSIZE) {
                writea(f, buf, bufptr - buf);
				if( f_s != -1 )
				{
					writea(f_s , buf_s , bufptr - buf_s);
					buf_s = buf;
				}
                bufptr = buf;
            }
            *((T*)bufptr) = val;
            bufptr += sizeof(T);
        }
		
        int blockid;
        
        /* template <typename T>
        void edata_flush(char * buf, char * bufptr, std::string & shard_filename, size_t totbytes) {
            int len = (int) (bufptr - buf);
            
            m.start_time("edata_flush");
            
            std::string block_filename = filename_shard_edata_block(shard_filename, blockid, compressed_block_size);
            int f = open(block_filename.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            write_compressed(f, buf, len);
            close(f);
            
            m.stop_time("edata_flush");
            
            
#ifdef DYNAMICEDATA
            // Write block's uncompressed size
            write_block_uncompressed_size(block_filename, len);
            
#endif
            
            blockid++;
        }
        
        template <typename T>
        void bwrite_edata(char * &buf, char * &bufptr, T val, size_t & totbytes, std::string & shard_filename,
                          size_t & edgecounter) {
            if (no_edgevalues) return;
            
            if (edgecounter == edges_per_block) {
                edata_flush<T>(buf, bufptr, shard_filename, totbytes);
                bufptr = buf;
                edgecounter = 0;
            }
            
            // Check if buffer is big enough
            if (bufptr - buf + sizeof(T) > ebuffer_size) {
                ebuffer_size *= 2;
                logstream(LOG_DEBUG) << "Increased buffer size to: " << ebuffer_size << std::endl;
                size_t ptroff = bufptr - buf; // Remember the offset
                buf = (char *) realloc(buf, ebuffer_size);
                bufptr = buf + ptroff;
            }
            
            totbytes += sizeof(T);
            *((T*)bufptr) = val;
            bufptr += sizeof(T);
        } */
        
        
        /**
         * Executes sharding.
         * @param nshards_string the number of shards as a number, or "auto" for automatic determination
         */
        int execute_sharding(std::string nshards_string) {
            char IPbuff[16];
			int machines = get_config_option_int("machines");
			for(int iter = 0 ; iter != machines ; ++iter)
			{
				sprintf(IPbuff , "IP%d" , iter+1);
				IPs.push_back(get_config_option_string(IPbuff));
			}
			
			m.start_time("execute_sharding");
            
            determine_number_of_shards(nshards_string);
            write_shards();
            
            m.stop_time("execute_sharding");
            
            /* Print metrics */
            basic_reporter basicrep;
            m.report(basicrep);
            
            return nshards;
        }
        
        /**
         * Sharding. This code might be hard to read - modify very carefully!
         */
    protected:
        
		/**
		 * modified by Wang
		 * Data:2014-04-17
		 */
		
        void determine_number_of_shards(std::string nshards_string) {
            if (nshards_string.find("auto") != std::string::npos || nshards_string == "0") {
                logstream(LOG_INFO) << "Determining number of shards automatically." << std::endl;
                
                int membudget_mb = get_option_int("membudget_mb", 1024);
                logstream(LOG_INFO) << "Assuming available memory is " << membudget_mb << " megabytes. " << std::endl;
                logstream(LOG_INFO) << " (This can be defined with configuration parameter 'membudget_mb')" << std::endl;
                
                size_t numedges = shoveled_edges; 
                
                double max_shardsize = membudget_mb * 1024. * 1024. / 8;
                logstream(LOG_INFO) << "Determining maximum shard size: " << (max_shardsize / 1024. / 1024.) << " MB." << std::endl;
                
				nshards = (int) ( 1 + (numedges * sizeof(FinalEdgeDataType) / max_shardsize)*1.1 + 0.5);
				
				//nshards = 16;
				
				
				
                /*
				//modified here
				//So dirty!
				nshards = (int) ( 1 + (numedges * sizeof(FinalEdgeDataType) / max_shardsize)*2 + 0.5);
				
				double totalsize = numedges*sizeof(edge_with_value<EdgeDataType>);
				
				logstream(LOG_INFO) << "Determining total shard size: " << (totalsize / 1024. / 1024.) << " MB." << std::endl;
				
				size_t singleshovelsize = get_filesize(shovel_filename(0));
				
				logstream(LOG_INFO) << "Single file size: " << (singleshovelsize / 1024. / 1024.) << " MB." << std::endl;
				
				int separt = max_vertex_id / nshards;
				
				
				for(int iter = 0 ; iter != nshards - 1 ; ++iter)
				{
					intervals.push_back(std::pair<vid_t,vid_t>(iter * separt , iter*separt + separt - 1));
					logstream(LOG_INFO) << "Shard " << iter << ": " << intervals[iter].first << "\t" << intervals[iter].second << std::endl;
				}
				
				intervals.push_back(std::pair<vid_t,vid_t>((nshards - 1) * separt , max_vertex_id));
				logstream(LOG_INFO) << "Shard " << nshards-1 << ": " << intervals[nshards-1].first << "\t" << intervals[nshards-1].second << std::endl;
				
				//modified ends
				
				*/
            } else {
                nshards = atoi(nshards_string.c_str());
            }
			int machines = IPs.size() + 1;
				
			while(nshards%machines != 0) nshards++;
			
			if(nshards > 255)
			{
				logstream(LOG_FATAL) << "NSHARDS OVERFLOW , TRY TO DEFINE TYPE SHARD_T WITH LARGER TYPE AND REBUILD" << std::endl;
				assert(false);
			}
			
			logstream(LOG_INFO) << "Shards:" << nshards << "   Machines:" << machines << std::endl;
            assert(nshards > 0);
            logstream(LOG_INFO) << "Number of shards to be created: " << nshards << std::endl;
			
			edges_in_bucks = (long*)calloc(nshards , sizeof(long));
			
        }
        
		/**
		 * End modification
		 */
        
    protected:
        
        void one_shard_intervals() {
            assert(nshards == 1);
            std::string fname = filename_intervals(basefilename, nshards);
            FILE * f = fopen(fname.c_str(), "w");
            intervals.push_back(std::pair<vid_t,vid_t>(0, max_vertex_id));
            fprintf(f, "%u\n", max_vertex_id);
            fclose(f);
            
            /* Write meta-file with the number of vertices */
            std::string numv_filename = basefilename + ".numvertices";
            f = fopen(numv_filename.c_str(), "w");
            fprintf(f, "%u\n", 1 + max_vertex_id);
            fclose(f);
            
            assert(nshards == (int)intervals.size());
        }
        
        
        std::string shovel_filename(int idx) {
            std::stringstream ss;
			std::string filename=basefilename;
			unsigned int pos;
			if( (pos = basefilename.find_last_of('/')) != std::string::npos)
			{
				filename=basefilename.substr(pos+1);
			}
			std::string defaultdirname = "./";
			if( pos != std::string::npos )
			{
				defaultdirname = basefilename.substr(0,pos+1);
			}
			std::string dirname = get_option_string("tmpfiledir" , defaultdirname);
            ss << dirname << filename << sizeof(EdgeDataType) << "." << idx << ".shovel";
            return ss.str();
        }
        
        
        int lastpart;
        
        
        
        
        degree * degrees;
        
		/**
		 * modified by Wang
		 * Data:2014-04-17
		 */
		
        virtual void finish_shard(int shard, edge_t * shovelbuf, size_t shovelsize) {
            m.start_time("shard_final");
            blockid = 0;
#ifdef DYNAMICEDATA
			int written_edges=0;
			int jumpover = 0;
            size_t edgecounter = 0;
			vid_t lastdst = 0xffffffff;
            size_t num_uniq_edges = 0;
            size_t last_edge_count = 0;
#endif

			int current_sliding_shard = 0;
			
            logstream(LOG_INFO) << "Starting final processing for shard: " << shard << std::endl;
            
			//std::string sddirname = dirname_shard(basefilename , shard , nshards);
			
            std::string fname = filename_shard_adj(basefilename, shard, nshards);
            /* std::string edfname = filename_shard_edata<FinalEdgeDataType>(basefilename, shard, nshards);
            std::string edblockdirname = dirname_shard_edata_block(edfname, compressed_block_size);
            
			unsigned int pos;
			if( (pos = fname.find_last_of('/')) != std::string::npos)
			{
				fname = fname.substr(pos+1);
			}
			if( (pos = edfname.find_last_of('/')) != std::string::npos)
			{
				edfname = edfname.substr(pos+1);
			}
			if( (pos = edblockdirname.find_last_of('/')) != std::string::npos)
			{
				edblockdirname = edblockdirname.substr(pos+1);
			} 
			
			std::string save  = fname;
			
			fname = sddirname + '/' + save;
			edfname = sddirname + '/' + edfname;
			edblockdirname = sddirname + '/' + edblockdirname;
			
			*/
            /* Make the block directory */
            /*
			if (!no_edgevalues)
                mkdir(edblockdirname.c_str(), 0777);
            */
			size_t numedges = shovelsize / sizeof(edge_t);
			
            edges_in_bucks[shard] = numedges;
			
            logstream(LOG_DEBUG) << "Shovel size:" << shovelsize << " edges: " << numedges << std::endl;
            
            logstream(LOG_DEBUG) << "Now Sorting for the shard " << shard << std::endl;
			m.start_time("finish_shard.sort");
			
#ifndef DYNAMICEDATA
            iSort(shovelbuf, (int)numedges, max_vertex_id, srcF<EdgeDataType>());
#else
            quickSort(shovelbuf, (int)numedges, edge_t_src_less<EdgeDataType>);
#endif
			
            m.stop_time("finish_shard.sort");
			logstream(LOG_DEBUG) << "Sorting done" << std::endl;
			
            // Remove duplicates
            if (duplicate_edge_filter != NULL && numedges > 0) {
                edge_t * tmpbuf = (edge_t*) calloc(numedges, sizeof(edge_t));
                size_t i = 1;
                tmpbuf[0] = shovelbuf[0];
                for(size_t j=1; j<numedges; j++) {
                    edge_t prev = tmpbuf[i - 1];
                    edge_t cur = shovelbuf[j];
                    
                    if (prev.src == cur.src && prev.dst == cur.dst) {
                        if (duplicate_edge_filter->acceptFirst(cur.value, prev.value)) {
                            // Replace the edge with the newer one
                            tmpbuf[i - 1] = cur;
                        }
                    } else {
                        tmpbuf[i++] = cur;
                    }
                }
                numedges = i;
                logstream(LOG_DEBUG) << "After duplicate elimination: " << numedges << " edges" << std::endl;
                free(shovelbuf);
                shovelbuf = tmpbuf; tmpbuf = NULL;
            }
            
            // Create the final file
            int f = open(fname.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (f < 0) {
                logstream(LOG_ERROR) << "Could not open " << fname << " error: " << strerror(errno) << std::endl;
            }
            assert(f >= 0);
            int trerr = ftruncate(f, 0);
            assert(trerr == 0);
            
			//modified here , for creating the sliding_shard files
			int f_sliding = -1;
			char * buf_sliding = NULL;
			
            char * buf = (char*) malloc(SHARDER_BUFSIZE);
            char * bufptr = buf;
			
			/* char * buf_sliding = buf;
			
            char * ebuf = (char*) malloc(compressed_block_size);
            ebuffer_size = compressed_block_size;
            char * ebufptr = ebuf; */
            
			//modified here
			//char * lastbuf
			
            vid_t curvid=0;

            size_t istart = 0;
            //size_t tot_edatabytes = 0; 
            for(size_t i=0; i <= numedges; i++) {
                if ( (i & 0xFFFFF) == 0) logstream(LOG_DEBUG) << i << " / " << numedges << std::endl;
#ifdef DYNAMICEDATA
                i += jumpover;  // With dynamic values, there might be several values for one edge, and thus the edge repeated in the data.
                jumpover = 0;
#endif //DYNAMICEDATA
                edge_t edge = (i < numedges ? shovelbuf[i] : edge_t(0, 0, EdgeDataType())); // Last "element" is a stopper
				
#ifdef DYNAMICEDATA
                
                if (lastdst == edge.dst && edge.src == curvid) {
                    // Currently not supported
                    logstream(LOG_ERROR) << "Duplicate edge in the stream - aborting" << std::endl;
					logstream(LOG_ERROR) << "I:" << i << " edge.valindex:" << edge.valindex << " nextshovel.valindex:" << shovelbuf[i+1].valindex << shovelbuf[i+2].valindex << std::endl;
                    logstream(LOG_ERROR) << "Left:" << shovelbuf[i-1].src << " " << shovelbuf[i-1].dst << " Right:" << edge.src << " " << edge.dst << std::endl;
					assert(false);
                }
                lastdst = edge.dst;
#endif
                
/*                if (!edge.stopper()) {
#ifndef DYNAMICEDATA
                    bwrite_edata<FinalEdgeDataType>(ebuf, ebufptr, FinalEdgeDataType(edge.value), tot_edatabytes, edfname, edgecounter);
#else
                    /* If we have dynamic edge data, we need to write the header of chivector - if there are edge values */
                    /*if (edge.is_chivec_value) {
                        // Need to check how many values for this edge
                        int count = 1;
                        while(shovelbuf[i + count].valindex == count) { count++; }
                        
                        assert(count < 32768);
                        
                        typename chivector<EdgeDataType>::sizeword_t szw;
                        ((uint16_t *) &szw)[0] = (uint16_t)count;  // Sizeword with length and capacity = count
                        ((uint16_t *) &szw)[1] = (uint16_t)count;
                        bwrite_edata<typename chivector<EdgeDataType>::sizeword_t>(ebuf, ebufptr, szw, tot_edatabytes, edfname, edgecounter);
                        for(int j=0; j < count; j++) {
                            bwrite_edata<EdgeDataType>(ebuf, ebufptr, EdgeDataType(shovelbuf[i + j].value), tot_edatabytes, edfname, edgecounter);
                        }
                        jumpover = count - 1; // Jump over
                    } else {
                        // Just write size word with zero
                        bwrite_edata<int>(ebuf, ebufptr, 0, tot_edatabytes, edfname, edgecounter);
                    }
                    num_uniq_edges++;
                    
#endif
                    edgecounter++; // Increment edge counter here --- notice that dynamic edata case makes two or more calls to bwrite_edata before incrementing
                }
				*/
#ifdef DYNAMICEDATA
				if (edge.is_chivec_value) {
					// Need to check how many values for this edge
					int count = 1;
					while(shovelbuf[i + count].valindex == count) { count++; }
					
					assert(count < 32768);
					jumpover = count - 1; // Jump over
				}
				if( !edge.stopper() )
				{
					num_uniq_edges++;
				}
#endif
                if (degrees != NULL && edge.src != edge.dst) {
                    degrees[edge.src].outdegree++;
                    degrees[edge.dst].indegree++;
                } 
                
                if ((edge.src != curvid || edge.stopper()) ) {
                    // New vertex
#ifndef DYNAMICEDATA
                    size_t count = i - istart;
#else
                    size_t count = num_uniq_edges - 1 - last_edge_count;
                    last_edge_count = num_uniq_edges - 1;
                    if (edge.stopper()) count++;  
#endif
                    //modified here for sliding_shard files
					/* if( curvid > intervals[current_sliding_shard].second )
					{
						if ( f_sliding != -1 )
						{
							writea(f_sliding , buf_sliding , bufptr-buf_sliding);
							close(f_sliding);
							f_sliding = -1;
						}
						buf_sliding = bufptr;
						if( current_sliding_shard != shard )
						{
							logstream(LOG_INFO) << "Finished the " << current_sliding_shard << " sliding adj files" << std::endl;
						}
						current_sliding_shard++;
					}
					if( f_sliding == -1 && current_sliding_shard != shard )
					{
						std::string sliding_dirname = dirname_shard(basefilename , current_sliding_shard , nshards);
						std::string sliding_filename = sliding_dirname + '/' + save;
						
						f_sliding = open(sliding_filename.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
						if (f_sliding < 0) {
							logstream(LOG_ERROR) << "Could not open " << save << " error: " << strerror(errno) << std::endl;
						}
						assert(f_sliding >= 0);
						int trerr = ftruncate(f_sliding, 0);
						assert(trerr == 0);
					} */
                    if (count>0) {
                        if (count < 255) {
                            uint8_t x = (uint8_t)count;
                            bwrite<uint8_t>(f, f_sliding , buf, buf_sliding , bufptr, x);
                        } else {
                            bwrite<uint8_t>(f, f_sliding , buf, buf_sliding , bufptr, 0xff);
                            bwrite<uint32_t>(f, f_sliding , buf, buf_sliding , bufptr, (uint32_t)count);
                        }
                    }
					
					
                    
#ifndef DYNAMICEDATA
                    for(size_t j=istart; j < i; j++) {
                        bwrite(f, f_sliding , buf, buf_sliding , bufptr,  shovelbuf[j].dst);
                    }
#else
                    // Special dealing with dynamic edata because some edges can be present multiple
                    // times in the shovel.
                    for(size_t j=istart; j < i; j++) {
                        if (j == istart || shovelbuf[j - 1].dst != shovelbuf[j].dst) {
                            bwrite(f, f_sliding , buf, buf_sliding , bufptr,  shovelbuf[j].dst);
							written_edges++;
                        }
                    }
#endif 
                    istart = i;
#ifdef DYNAMICEDATA
                    istart += jumpover;
#endif 
                    
                    // Handle zeros
                    if (!edge.stopper()) {
                        if (edge.src - curvid > 1 || (i == 0 && edge.src>0)) {
                            int nz = edge.src - curvid - 1;
                            if (i == 0 && edge.src > 0) nz = edge.src; // border case with the first one
                            do {
                                bwrite<uint8_t>(f, f_sliding , buf, buf_sliding , bufptr, 0);
                                nz--;
                                int tnz = std::min(254, nz);
                                bwrite<uint8_t>(f, f_sliding , buf, buf_sliding , bufptr, (uint8_t) tnz);
                                nz -= tnz;
                            } while (nz>0);
                        }
                    }
                    curvid = edge.src;
                }
            }
            
            /* Flush buffers and free memory */
            writea(f, buf, bufptr - buf);
			if(f_sliding != -1)
			{
				writea(f_sliding , buf_sliding , bufptr - buf_sliding);
				close(f_sliding);
				f_sliding = -1;
				logstream(LOG_INFO) << "Finished the " << current_sliding_shard << " sliding adj files" << std::endl;
			}
            free(buf);
            free(shovelbuf);
            close(f);
            
#ifdef DYNAMICEDATA
			realedges += num_uniq_edges;
			logstream(LOG_DEBUG) << "Num_uniq:" << num_uniq_edges << " Written:" << written_edges << std::endl;
#endif
	
            /* Write edata size file */
            /* if (!no_edgevalues) {
                edata_flush<FinalEdgeDataType>(ebuf, ebufptr, edfname, tot_edatabytes);
                
                std::string sizefilename = edfname + ".size";
                std::ofstream ofs(sizefilename.c_str());
#ifndef DYNAMICEDATA
                ofs << tot_edatabytes;
#else
                ofs << num_uniq_edges * sizeof(int); // For dynamic edge data, write the number of edges.
#endif
                
                ofs.close();
            }
            free(ebuf); */
            
            m.stop_time("shard_final");
        }
        
        /* Begin: Kway -merge sink interface */
        
        size_t edges_per_shard;
        size_t cur_shard_counter;
        size_t shard_capacity;
        size_t sharded_edges;
        int shardnum;
        edge_with_value<EdgeDataType> * sinkbuffer;
        vid_t prevvid;
        vid_t this_interval_start;
        
		/*
		 * End modification
		 */
		
        virtual void add(edge_with_value<EdgeDataType> val) {
			//modified here
			//if (intervals[shardnum].second<val.dst)
            if (cur_shard_counter >= edges_per_shard && val.dst != prevvid) 
			{
                createnextshard();
            }
            
            if (cur_shard_counter == shard_capacity) {
                /* Really should have a way to limit shard sizes, but probably not needed in practice */
                logstream(LOG_WARNING) << "Shard " << shardnum << " overflowing! " << cur_shard_counter << " / " << shard_capacity << std::endl;
                shard_capacity = (size_t) (1.2 * shard_capacity);
                sinkbuffer = (edge_with_value<EdgeDataType>*) realloc(sinkbuffer, shard_capacity * sizeof(edge_with_value<EdgeDataType>));
            }
            
            sinkbuffer[cur_shard_counter++] = val;
            prevvid = val.dst;
            sharded_edges++;
        }
        
        void createnextshard() {
            assert(shardnum < nshards);
			//modified here
            intervals.push_back(std::pair<vid_t, vid_t>(this_interval_start, (shardnum == nshards - 1 ? max_vertex_id : prevvid)));
            this_interval_start = prevvid + 1;
            finish_shard(shardnum++, sinkbuffer, cur_shard_counter * sizeof(edge_with_value<EdgeDataType>));
            sinkbuffer = (edge_with_value<EdgeDataType> *) malloc(shard_capacity * sizeof(edge_with_value<EdgeDataType>));
            cur_shard_counter = 0;
            
            // Adjust edges per hard so that it takes into account how many edges have been spilled now
            logstream(LOG_INFO) << "Remaining edges: " << (shoveled_edges - sharded_edges) << " remaining shards:" << (nshards - shardnum)
                << " edges per shard=" << edges_per_shard << std::endl;
            if (shardnum < nshards) edges_per_shard = (shoveled_edges - sharded_edges) / (nshards - shardnum);
            logstream(LOG_INFO) << "Edges per shard: " << edges_per_shard << std::endl;
            
        }
        
        virtual void done() {
            createnextshard();
            if (shoveled_edges != sharded_edges) {
                logstream(LOG_INFO) << "Shoveled " << shoveled_edges << " but sharded " << sharded_edges << " edges" << std::endl;
            }
            assert(shoveled_edges == sharded_edges);
            
            logstream(LOG_INFO) << "Created " << shardnum << " shards, expected: " << nshards << std::endl;
            assert(shardnum <= nshards);
            free(sinkbuffer);
            sinkbuffer = NULL;
            
            /* Write intervals */
            std::string fname = filename_intervals(basefilename, nshards);
            FILE * f = fopen(fname.c_str(), "w");
            
            if (f == NULL) {
                logstream(LOG_ERROR) << "Could not open file: " << fname << " error: " <<
                strerror(errno) << std::endl;
            }
            assert(f != NULL);
            for(int i=0; i<(int)intervals.size(); i++) {
               fprintf(f, "%u\n", intervals[i].second);
            }
            fclose(f);
            
            /* Write meta-file with the number of vertices */
            std::string numv_filename = basefilename + ".numvertices";
            f = fopen(numv_filename.c_str(), "w");
            fprintf(f, "%u\n", 1 + max_vertex_id);
            fclose(f);
        }
        
        /* End: Kway -merge sink interface */
		
        
        /**
         * Write the shard by sorting the shovel file and compressing the
         * adjacency information.
         * To support different shard types, override this function!
         */
        void write_shards() {
            
            size_t membudget_mb = (size_t) get_option_int("membudget_mb", 1024);
			
            // Check if we have enough memory to keep track
            // of the vertex degrees in-memory (heuristic)
            
			/*Don't need to count degree before
			
			//modified here
			//bool count_degrees_inmem = membudget_mb * 1024 * 1024 / 3 > max_vertex_id * sizeof(degree); 
			bool count_degrees_inmem = true;
			
			degrees = NULL;
#ifdef DYNAMICEDATA
            if (!count_degrees_inmem) {
                /* Temporary: force in-memory count of degrees because the PSW-based computation
                 is not yet compatible with dynamic edge data.
                 */
				 /*
                logstream(LOG_WARNING) << "Dynamic edge data support only sharding when the vertex degrees can be computed in-memory." << std::endl;
                logstream(LOG_WARNING) << "If the program gets very slow (starts swapping), the data size is too big." << std::endl;
                count_degrees_inmem = true;
            }
#endif
            if (count_degrees_inmem) {
                degrees = (degree *) calloc(1 + max_vertex_id, sizeof(degree));
            } 
            
			//modified here
			mkdirs();
			Don't need to create all the dirs now
			*/
			bool count_degrees_inmem = true;
			if (count_degrees_inmem) {
                degrees = (degree *) calloc(1 + max_vertex_id, sizeof(degree));
            } 
            // KWAY MERGE
            sharded_edges = 0;
            edges_per_shard = shoveled_edges / nshards + 1;
            shard_capacity = edges_per_shard / 2 * 3;  // Shard can go 50% over
            shardnum = 0;
            this_interval_start = 0;
            sinkbuffer = (edge_with_value<EdgeDataType> *) calloc(shard_capacity, sizeof(edge_with_value<EdgeDataType>));
            logstream(LOG_INFO) << "Edges per shard: " << edges_per_shard << " nshards=" << nshards << " total: " << shoveled_edges << std::endl;
            cur_shard_counter = 0;
            
            /* Initialize kway merge sources */
            size_t B = membudget_mb * 1024 * 1024 / 2 / numshovels;
            while (B % sizeof(edge_with_value<EdgeDataType>) != 0) B++;
            logstream(LOG_INFO) << "Buffer size in merge phase: " << B << std::endl;
            prevvid = (-1);
            std::vector< merge_source<edge_with_value<EdgeDataType> > *> sources;
            for(int i=0; i < numshovels; i++) {
                sources.push_back(new shovel_merge_source<EdgeDataType>(B, shovel_filename(i)));
            }
            
            kway_merge<edge_with_value<EdgeDataType> > merger(sources, this);
            merger.merge();
            
            // Delete sources
            for(int i=0; i < (int)sources.size(); i++) {
                delete (shovel_merge_source<EdgeDataType> *)sources[i];
            }
            
            /* Do not need to create degree files here
            if (!count_degrees_inmem) {
#ifndef DYNAMICEDATA
                // Use memory-efficient (but slower) method to create degree-data
                create_degree_file();
#endif
                
            } else {
                std::string degreefname = filename_degree_data(basefilename);
                int degreeOutF = open(degreefname.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
                if (degreeOutF < 0) {
                    logstream(LOG_ERROR) << "Could not create: " << degreeOutF << std::endl;
                    assert(degreeOutF >= 0);
                }
                
                writea(degreeOutF, degrees, sizeof(degree) * (1 + max_vertex_id));
                free(degrees);
                close(degreeOutF);
            } */
            
			vid_t * sendbuff = (vid_t*)calloc(nshards , sizeof(vid_t));
			
			if(opti_times >= 1)
			{
				create_partition_files();
				
				intervals.clear();
				vid_t vertex_st = 0;
				
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					intervals.push_back(std::pair<vid_t , vid_t>(vertex_st , vertex_st + vertexs_in_bucks[iter]-1));
					sendbuff[iter] = vertex_st + vertexs_in_bucks[iter]-1;
					vertex_st += vertexs_in_bucks[iter];
				}
			}
			else if(opti_times == 0)
			{
				logstream(LOG_DEBUG) << "Will not Spilt the graph" << std::endl;
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					sendbuff[iter] = intervals[iter].second;
				}
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					std::string adjfilename = filename_shard_adj(basefilename, iter, nshards);
					remove(adjfilename.c_str());
				}
			}
			else if(opti_times == -1)
			{
				logstream(LOG_DEBUG) << "Will Use the average mode" << std::endl;
				
				vertex_to_bucks = (shard_t*)calloc(max_vertex_id + 1 , sizeof(shard_t));
				vertexs_in_bucks = (long *)calloc(nshards , sizeof(long));
				
				assert(vertex_to_bucks != NULL);
				assert(vertexs_in_bucks != NULL);
				assert(edges_in_bucks != NULL);
				
				memset(vertex_to_bucks , 0 , (max_vertex_id+1) * sizeof(shard_t));
				memset(vertexs_in_bucks , 0 , nshards*sizeof(long));
				memset(edges_in_bucks , 0 , nshards*sizeof(long));
				
				int to_buck = 0;
				
				for(int iter = 0 ; iter != max_vertex_id+1 ; ++iter)
				{
					vertex_to_bucks[iter] = to_buck;
					vertexs_in_bucks[to_buck]++;
					edges_in_bucks[to_buck] += (degrees[iter].indegree + degrees[iter].outdegree);
					
					to_buck++;
					if(to_buck == nshards)
					{
						to_buck = 0;
					}
				}
				
				intervals.clear();
				vid_t vertex_st = 0;
				
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					intervals.push_back(std::pair<vid_t , vid_t>(vertex_st , vertex_st + vertexs_in_bucks[iter]-1));
					sendbuff[iter] = vertex_st + vertexs_in_bucks[iter]-1;
					vertex_st += vertexs_in_bucks[iter];
				}
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					std::string adjfilename = filename_shard_adj(basefilename, iter, nshards);
					remove(adjfilename.c_str());
				}
			}
			
			size_t buffsize = (basefilename.length() + 1)*sizeof(char) + sizeof(shard_t) + nshards * sizeof(vid_t);
			char * buff = (char*) malloc( buffsize ) ;
			strcpy(buff , basefilename.c_str());
			*(shard_t*)(buff+basefilename.length()+1) = nshards;
			memcpy(buff + basefilename.length() + 1 + sizeof(shard_t) , sendbuff , nshards * sizeof(vid_t));
			
			free(sendbuff);
			
			int sent=0 , machines = IPs.size();
			for( int iter = 0 ; iter != machines ; ++iter)
			{
				sent=0;
				dt.AddTask(buff , buffsize ,std::string("PreInfo") ,  IPs[iter], &sent );
				
				shard_t * duty = (shard_t*)malloc(nshards/(1+machines) * (sizeof(shard_t) + sizeof(long)) );
				int index=0;
				for(int i=0 ; i != nshards ; ++i)
				{
					if(i%(1+machines) == iter)
					{
						duty[index++] = i;
						if(opti_times>=1)
						{
							*(long*)(duty+index) = edges_in_bucks[i]/1.5;
						}
						else
						{
							*(long*)(duty+index) = edges_in_bucks[i]*2.5;
						}
						index += sizeof(long);
					}
				}
				assert(index == nshards/(1+machines) * (sizeof(shard_t) + sizeof(long)) );
				
				dt.AddTask((char*)duty , index*sizeof(shard_t) , std::string("Duty") , IPs[iter] , NULL);
				
				while(sent == 0) usleep(1000);
			}
			free(buff);
			
			if(opti_times != 0)
			{
			
				vertexs_map = (vid_t*) calloc( max_vertex_id + 1 , sizeof(vid_t) );
				assert(vertexs_map != NULL);
				
				memset(vertexs_map , -1 , (max_vertex_id+1) * sizeof(vid_t) );
				
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					vertexs_in_bucks[iter] = intervals[iter].first;
				}
				
				logstream(LOG_INFO) << "Start to renumber the vertex..." << std::endl;
				for(int iter = 0 ; iter != max_vertex_id+1 ; ++iter)
				{
					vertexs_map[iter] = vertexs_in_bucks[vertex_to_bucks[iter]]++;;
				}
				logstream(LOG_INFO) << "Finished renumbering..." << std::endl;
			
			}
			
			memset(degrees , 0 , (max_vertex_id+1) * sizeof(degree));
			
			reshovel();
			
			std::string degreefilename = filename_degree_data(basefilename);
			
			int f_degree = open(degreefilename.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
			assert(f_degree >= 0);
			
			int trerr = ftruncate(f_degree , 0);
			assert(trerr == 0);
			
			writea(f_degree , degrees , (max_vertex_id + 1) * sizeof(degree));
			
			free(degrees);
			degrees = NULL;
			close(f_degree);
			
			for(int iter = 0 ; iter != IPs.size() ; ++iter)
			{
				this->dt.AddTask(degreefilename , IPs[iter]);
			}
			
			if(opti_times != 0)
			{
				vid_t * remap = (vid_t*)malloc( (max_vertex_id + 1) * sizeof(vid_t));
				
				for( int iter = 0 ; iter != max_vertex_id+1 ; ++iter )
				{
					remap[vertexs_map[iter]] = iter;
				}
				
				free(vertexs_map);
				vertexs_map = NULL;
				
				std::string mapfilename = basefilename + ".map";
				
				int f_map = open(mapfilename.c_str() ,  O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
				assert(f_map >= 0);
				
				writea(f_map , remap , (1+max_vertex_id) * sizeof(vid_t));
				free(remap);
				close(f_map);
			}
			
			for(int iter = 0 ; iter != refiles.size() ; ++iter)
			{
				logstream(LOG_INFO) << "Reshard for " << refiles[iter] << std::endl;
				size_t fsize = get_filesize(refiles[iter]);
				
				char * shovelbuf = (char*)malloc(fsize);
				
				int f = open(refiles[iter].c_str() , O_RDONLY);
				reada(f , shovelbuf , fsize);
				close(f);
				
				reshard(machines + iter * (machines+1) ,  (edge_t*)shovelbuf , fsize/sizeof(edge_t));
				remove(refiles[iter].c_str());
			}
			
			
			std::string f_inter = filename_intervals(basefilename , nshards);
			
			FILE * fp = fopen(f_inter.c_str() , "w");
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				fprintf(fp , "%d\n" , intervals[iter].second);
			}
			fclose(fp);
        }
        
        
        typedef char dummy_t;
        
        typedef sliding_shard<int, dummy_t> slidingshard_t;
        typedef memory_shard<int, dummy_t> memshard_t;
        
		vid_t determine_next_window(vid_t fromvid, vid_t maxvid, size_t membudget , size_t & edges)
		{
				size_t memreq = 0;
                int inc , outc;
				edges = 0;
                for(int i= fromvid; i <= maxvid; i++) {
                    inc = degrees[i].indegree;
                    outc = degrees[i].outdegree;
                    
                    // Raw data and object cost included
                    memreq += sizeof(CE_Graph_vertex<int, dummy_t>) + (sizeof(dummy_t) + sizeof(vid_t) + sizeof(CE_Graph_edge<dummy_t>))*(outc + inc);
                    if (memreq > membudget) {
                        logstream(LOG_DEBUG) << "Memory budget exceeded with " << memreq << " bytes." << std::endl;
                        return i - 1;  // Previous was enough
                    }
					edges +=(inc+outc);
                }
                return maxvid;
		}

        void create_partition_files() {
            // Initialize IO
            stripedio * iomgr = new stripedio(m);
            std::vector<slidingshard_t * > sliding_shards;
            
			int membudget_mb = get_option_int("membudget_mb", 1024);
			
			vertex_to_bucks = (shard_t*)calloc(max_vertex_id + 1 , sizeof(shard_t));
			vertexs_in_bucks = (long *)calloc(nshards , sizeof(long));
			int * cross_in_bucks = (int *)calloc(nshards , sizeof(int));
			
			assert(vertex_to_bucks != NULL);
			assert(vertexs_in_bucks != NULL);
			assert(cross_in_bucks != NULL);
			assert(edges_in_bucks != NULL);
			
			memset(vertex_to_bucks , 0 , (max_vertex_id+1) * sizeof(shard_t));
			memset(vertexs_in_bucks , 0 , nshards*sizeof(long));
			memset(edges_in_bucks , 0 , nshards*sizeof(long));
			
			
			double close_ratio;
#ifndef DYNAMICEDATA
			int capacity = (shoveled_edges) / nshards * 2.005;
#else
			int capacity = (realedges) / nshards * 2.005;
#endif	
			int vertex_limit = (max_vertex_id+1) * 2.005 / nshards;
			
			double ratio = get_option_int("edge_ratio" , 100) / 100.0;
			
			logstream(LOG_DEBUG) << "Determining the capacity of each bucks:" << capacity << std::endl;
			
			CE_Graph_edge<dummy_t> * tempbuffin , * tempbuffout;
			
            
            int loadthreads = 4;
            
            m.start_time("partition.runtime");
            
            /* Initialize streaming shards */
            int blocksize = compressed_block_size;
            
            for(int p=0; p < nshards; p++) {
                logstream(LOG_INFO) << "Initialize streaming shard: " << p << std::endl;
                sliding_shards.push_back(
                                         new slidingshard_t(iomgr, filename_shard_edata<dummy_t>(basefilename, p, nshards),
                                                            filename_shard_adj(basefilename, p, nshards), intervals[p].first,
                                                            intervals[p].second,
                                                            blocksize, m, true, true));
            }
            
            CE_Graph_context ginfo;
            ginfo.nvertices = 1 + intervals[nshards - 1].second;
            ginfo.scheduler = NULL;
            
			membudget_mb = get_option_int("membudget_mb", 1024);
			size_t membuffer = (membudget_mb * 1024 * 1024.  - (double)(max_vertex_id+1) * ( sizeof(degree) + sizeof(shard_t) )) * 0.8; 
			
			logstream(LOG_DEBUG) << "Assuming membudget is " << membuffer/1024/1024 << " MB" << std::endl; 
			
			
			
            /* std::string outputfname = filename_degree_data(basefilename);
            
            int degreeOutF = open(outputfname.c_str(), O_RDWR | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (degreeOutF < 0) {
                logstream(LOG_ERROR) << "Could not create: " << degreeOutF << std::endl;
            }
            assert(degreeOutF >= 0);
            int trerr = ftruncate(degreeOutF, ginfo.nvertices * sizeof(int) * 2);
            assert(trerr == 0);
            if (trerr != 0) {
                logstream(LOG_FATAL) << "Could not truncate!" << std::endl;
                exit(0);
            } */
            
            for(int window=0; window<nshards; window++) {
                metrics_entry mwi = m.start_time();
                
                vid_t interval_st = intervals[window].first;
                vid_t interval_en = intervals[window].second;
                
                /* Flush stream shard for the window */
                sliding_shards[window]->flush();
                
                /* Load shard[window] into memory */
                memshard_t memshard(iomgr, filename_shard_edata<FinalEdgeDataType>(basefilename, window, nshards), filename_shard_adj(basefilename, window, nshards),
                                    interval_st, interval_en, blocksize, m);
                memshard.only_adjacency = true;
                logstream(LOG_INFO) << "Interval: " << interval_st << " " << interval_en << std::endl;
                
				size_t edges=0;
				
                for(vid_t subinterval_st=interval_st; subinterval_st <= interval_en; ) {
                    //vid_t subinterval_en = std::min(interval_en, subinterval_st + subwindow);
                    vid_t subinterval_en = determine_next_window(subinterval_st, 
                                                                interval_en, 
                                                                membuffer , edges);
					
					logstream(LOG_INFO) << "(Partition proc.) Sub-window: [" << subinterval_st << " - " << subinterval_en << "]" << std::endl;
                    logstream(LOG_DEBUG) << edges << " edges this time" << std::endl;
					assert(subinterval_en >= subinterval_st && subinterval_en <= interval_en);
                    
                    /* Preallocate vertices */
                    metrics_entry men = m.start_time();
                    int nvertices = subinterval_en - subinterval_st + 1;
                    std::vector< CE_Graph_vertex<int, dummy_t> > vertices(nvertices, CE_Graph_vertex<int, dummy_t>()); // preallocate
                    
					size_t edges_before=0;
                    CE_Graph_edge<dummy_t> * edgebuff = (CE_Graph_edge<dummy_t> *)calloc(edges , sizeof(CE_Graph_edge<dummy_t>));
                    assert(edgebuff != NULL);
					for(int i=0; i < nvertices; i++) {
						
						tempbuffin = tempbuffout = NULL;
						if(degrees[subinterval_st+i].indegree != 0)
						{
							tempbuffin = edgebuff + edges_before;
							assert(tempbuffin != NULL);
						}
						edges_before += degrees[subinterval_st+i].indegree;
						if(degrees[subinterval_st+i].outdegree != 0)
						{
							tempbuffout = edgebuff + edges_before;
							assert(tempbuffout != NULL);
						}
						edges_before += degrees[subinterval_st+i].outdegree;
						vertices[i] = CE_Graph_vertex<int, dummy_t>(subinterval_st + i, tempbuffin, tempbuffout, 0, 0);
                        vertices[i].scheduled =  true;
                    }
                    
					//logstream(LOG_DEBUG) << "Excepted to have " << edges << " , Real read edges:" << edges_before << std::endl;
					
					assert(edges_before == edges);
					logstream(LOG_DEBUG) << "Preallocate finished" << std::endl;
					
                    metrics_entry me = m.start_time();
                    omp_set_num_threads(loadthreads);
#pragma omp parallel for
                    for(int p=-1; p < nshards; p++)  {
                        if (p == (-1)) {
                            // if first window, now need to load the memshard
                            if (memshard.loaded() == false) {
                                memshard.load();
                            }
                            
                            /* Load vertices from memshard (only inedges for now so can be done in parallel) */
                            memshard.load_vertices(subinterval_st, subinterval_en, vertices);
                        } else {
                            /* Stream forward other than the window partition */
                            if (p != window) {
                                sliding_shards[p]->read_next_vertices(nvertices, subinterval_st, vertices, false);
                            }
                        }
                    }
                    
                    m.stop_time(me, "stream_ahead", window);
                    
                    
					logstream(LOG_DEBUG) << "Load vertices finished" << std::endl;
					
                    metrics_entry mev = m.start_time();
                    // Read first current values
                    
                    /* int * vbuf = (int*) malloc(nvertices * sizeof(int) * 2);
                    
                    for(int i=0; i<nvertices; i++) {
                        vbuf[2 * i] = vertices[i].num_inedges();
                        vbuf[2 * i +1] = vertices[i].num_outedges();
                    }
                    pwritea(degreeOutF, vbuf, nvertices * sizeof(int) * 2, subinterval_st * sizeof(int) * 2);
                    
                    free(vbuf); */
                    
					shard_t to_buck;
					int inedges , outedges;
					vid_t curvetice , othervertice;
					double temp_ratio;
					double E_ratio = (double)shoveled_edges/(max_vertex_id+1)/600 * (-1);
					double base_r = 1.0/(max_vertex_id+1);
					for(int iter = 0 ; iter != nvertices ; ++iter)
					{
						memset(cross_in_bucks , 0 , nshards * sizeof(int));
						close_ratio= -1;
						to_buck = 0;
						
						curvetice = vertices[iter].id();
						assert(curvetice == subinterval_st + iter);
						inedges = vertices[iter].num_inedges();
						outedges = vertices[iter].num_outedges();
						
						for(int in=0; in != inedges ; ++in)
						{
							othervertice = vertices[iter].inedge(in)->vertex_id();
							if(othervertice > curvetice)
							{
								break;
							}
							else
							{
								cross_in_bucks[vertex_to_bucks[othervertice]]++;
							}
						}
						
						for(int out=0; out != outedges ; ++out)
						{
							othervertice = vertices[iter].outedge(out)->vertex_id();
							if(othervertice > curvetice)
							{
								break;
							}
							else
							{
								cross_in_bucks[vertex_to_bucks[othervertice]]++;
							}
						}
						
						for(int i = 0 ; i != nshards ; ++i)
						{
							temp_ratio = cross_in_bucks[i] * ( ( 1.0- (double)edges_in_bucks[i]/capacity ) * ratio + (1-ratio) * ( 1.0 - (double)vertexs_in_bucks[i]/vertex_limit )   );
							if( (temp_ratio - close_ratio > 0.000001) || ( (temp_ratio - close_ratio > E_ratio) && (vertexs_in_bucks[i] < vertexs_in_bucks[to_buck] * base_r * (curvetice)) ) )
							{
								close_ratio = temp_ratio;
								to_buck = i;
							}
						}
						
						vertex_to_bucks[curvetice] = to_buck;
						vertexs_in_bucks[to_buck]++;
						edges_in_bucks[to_buck] += (inedges + outedges);
					}
					
					free(edgebuff);
                    // Move window
                    subinterval_st = subinterval_en+1;
                }
				for(int iter = 0 ; iter != nshards ; ++iter)
				{
					logstream(LOG_DEBUG) << "There are " << vertexs_in_bucks[iter] << " vertexs and " << edges_in_bucks[iter] << " edges in the buck " << iter << std::endl;
				}
                /* Move the offset of the window-shard forward */
                sliding_shards[window]->set_offset(memshard.offset_for_stream_cont(), memshard.offset_vid_for_stream_cont(),
                                                   memshard.edata_ptr_for_stream_cont());
            }
			capacity *= 1.2;
			m.start_time("optimize.runtime");
			for(int optimization = 0 ; optimization != opti_times-1 ; ++optimization)
			{
				logstream(LOG_INFO) << "Now start to optimize the " << optimization+1 << " times" << std::endl;
				for(int window=0; window<nshards; window++) {
					metrics_entry mwi = m.start_time();
					
					vid_t interval_st = intervals[window].first;
					vid_t interval_en = intervals[window].second;
					
					/* Flush stream shard for the window */
					sliding_shards[window]->flush();
					
					/* Load shard[window] into memory */
					memshard_t memshard(iomgr, filename_shard_edata<FinalEdgeDataType>(basefilename, window, nshards), filename_shard_adj(basefilename, window, nshards),
										interval_st, interval_en, blocksize, m);
					memshard.only_adjacency = true;
					logstream(LOG_INFO) << "Interval: " << interval_st << " " << interval_en << std::endl;
					
					size_t edges=0;
					
					for(vid_t subinterval_st=interval_st; subinterval_st <= interval_en; ) {
						//vid_t subinterval_en = std::min(interval_en, subinterval_st + subwindow);
						vid_t subinterval_en = determine_next_window(subinterval_st, 
																	interval_en, 
																	membuffer , edges);
						
						logstream(LOG_INFO) << "(Partition proc.) Sub-window: [" << subinterval_st << " - " << subinterval_en << "]" << std::endl;
						logstream(LOG_DEBUG) << edges << " edges this time" << std::endl;
						assert(subinterval_en >= subinterval_st && subinterval_en <= interval_en);
						
						/* Preallocate vertices */
						metrics_entry men = m.start_time();
						int nvertices = subinterval_en - subinterval_st + 1;
						std::vector< CE_Graph_vertex<int, dummy_t> > vertices(nvertices, CE_Graph_vertex<int, dummy_t>()); // preallocate
						
						size_t edges_before=0;
						CE_Graph_edge<dummy_t> * edgebuff = (CE_Graph_edge<dummy_t> *)calloc(edges , sizeof(CE_Graph_edge<dummy_t>));
						assert(edgebuff != NULL);
						for(int i=0; i < nvertices; i++) {
							
							tempbuffin = tempbuffout = NULL;
							if(degrees[subinterval_st+i].indegree != 0)
							{
								tempbuffin = edgebuff + edges_before;
								assert(tempbuffin != NULL);
							}
							edges_before += degrees[subinterval_st+i].indegree;
							if(degrees[subinterval_st+i].outdegree != 0)
							{
								tempbuffout = edgebuff + edges_before;
								assert(tempbuffout != NULL);
							}
							edges_before += degrees[subinterval_st+i].outdegree;
							vertices[i] = CE_Graph_vertex<int, dummy_t>(subinterval_st + i, tempbuffin, tempbuffout, 0, 0);
							vertices[i].scheduled =  true;
						}
						
						//logstream(LOG_DEBUG) << "Excepted to have " << edges << " , Real read edges:" << edges_before << std::endl;
						
						assert(edges_before == edges);
						logstream(LOG_DEBUG) << "Preallocate finished" << std::endl;
						
						metrics_entry me = m.start_time();
						omp_set_num_threads(loadthreads);
	#pragma omp parallel for
						for(int p=-1; p < nshards; p++)  {
							if (p == (-1)) {
								// if first window, now need to load the memshard
								if (memshard.loaded() == false) {
									memshard.load();
								}
								
								/* Load vertices from memshard (only inedges for now so can be done in parallel) */
								memshard.load_vertices(subinterval_st, subinterval_en, vertices);
							} else {
								/* Stream forward other than the window partition */
								if (p != window) {
									sliding_shards[p]->read_next_vertices(nvertices, subinterval_st, vertices, false);
								}
							}
						}
						
						m.stop_time(me, "stream_ahead", window);
						
						
						logstream(LOG_DEBUG) << "Load vertices finished" << std::endl;
						
						metrics_entry mev = m.start_time();
						// Read first current values
						
						/* int * vbuf = (int*) malloc(nvertices * sizeof(int) * 2);
						
						for(int i=0; i<nvertices; i++) {
							vbuf[2 * i] = vertices[i].num_inedges();
							vbuf[2 * i +1] = vertices[i].num_outedges();
						}
						pwritea(degreeOutF, vbuf, nvertices * sizeof(int) * 2, subinterval_st * sizeof(int) * 2);
						
						free(vbuf); */
						
						shard_t to_buck , pre_buck;
						int inedges , outedges;
						vid_t curvetice , othervertice;
						double temp_ratio;
						for(int iter = 0 ; iter != nvertices ; ++iter)
						{
							memset(cross_in_bucks , 0 , nshards * sizeof(int));
							close_ratio= -1;
							to_buck = 0;
							
							curvetice = vertices[iter].id();
							assert(curvetice == subinterval_st + iter);
							inedges = vertices[iter].num_inedges();
							outedges = vertices[iter].num_outedges();
							
							for(int in=0; in != inedges ; ++in)
							{
								othervertice = vertices[iter].inedge(in)->vertex_id();
								cross_in_bucks[vertex_to_bucks[othervertice]]++;
							}
							
							for(int out=0; out != outedges ; ++out)
							{
								othervertice = vertices[iter].outedge(out)->vertex_id();
								cross_in_bucks[vertex_to_bucks[othervertice]]++;
							}
							
							for(int i = 0 ; i != nshards ; ++i)
							{
								temp_ratio = cross_in_bucks[i] * ( ( 1.0- (double)edges_in_bucks[i]/capacity ) * ratio + (1-ratio) * ( 1.0 - (double)vertexs_in_bucks[i]/vertex_limit )   );
								if( (temp_ratio - close_ratio > 0.000001) )
								{
									close_ratio = temp_ratio;
									to_buck = i;
								}
							}
							
							pre_buck = vertex_to_bucks[curvetice];
							if(to_buck != pre_buck)
							{
								vertex_to_bucks[curvetice] = to_buck;
								vertexs_in_bucks[pre_buck]--;
								vertexs_in_bucks[to_buck]++;
								edges_in_bucks[pre_buck] -= (inedges + outedges);
								edges_in_bucks[to_buck] += (inedges + outedges);
							}
						}
						
						free(edgebuff);
						// Move window
						subinterval_st = subinterval_en+1;
					}
					for(int iter = 0 ; iter != nshards ; ++iter)
					{
						logstream(LOG_DEBUG) << "There are " << vertexs_in_bucks[iter] << " vertexs and " << edges_in_bucks[iter] << " edges in the buck " << iter << std::endl;
					}
					/* Move the offset of the window-shard forward */
					sliding_shards[window]->set_offset(memshard.offset_for_stream_cont(), memshard.offset_vid_for_stream_cont(),
													   memshard.edata_ptr_for_stream_cont());
				}
			}
			m.stop_time("optimize.runtime");
            free(cross_in_bucks);
            m.stop_time("partition.runtime");
            delete iomgr;
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				std::string adjfilename = filename_shard_adj(basefilename, iter, nshards);
				remove(adjfilename.c_str());
			}
        }
		
		int * fds;
		long * offsets;
		int * packages;
		int machines;
		
		int start_reshovel(edge_with_value<EdgeDataType> ** buff , size_t * edges_in_buff)
		{
			fds = (int*) calloc(nshards , sizeof(int));
			offsets = (long*) calloc(nshards , sizeof(long));
			packages = (int*) calloc(nshards , sizeof(int));
			
			assert(fds != NULL);
			assert(offsets != NULL);
			assert(packages != NULL);
			
			memset(fds , 0 , sizeof(int)*nshards);
			memset(offsets , 0 , sizeof(long)*nshards);
			memset(packages , 0 , sizeof(int)*nshards);
			
			machines=IPs.size()+1;
			
			int trerr;
			
			for(int iter = machines-1 ; iter < nshards ; iter+=machines)
			{
				
				std::string filename = filename_shard_adj(basefilename , iter , nshards);
				filename += ".re";
				fds[iter] = open(filename.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
				trerr = ftruncate(fds[iter], 0);
				assert(trerr == 0);
				refiles.push_back(filename);
			}
			return 0;
		}
		
		int finish_reshovel_block(edge_with_value<EdgeDataType> * buff , int edges , int shardindex)
		{
			if(shardindex % machines == machines-1)
			{
				writea(fds[shardindex] , buff , edges * sizeof(edge_with_value<EdgeDataType>)); 
			}
			else
			{
				char buffer[128];
				sprintf(buffer , "Shovel:%d Offset:%ld" , shardindex , offsets[shardindex]);
				int sub_done=0;
				dt.AddTask((char*)buff , edges * sizeof(edge_with_value<EdgeDataType>) , buffer , IPs[shardindex%machines] , &sub_done);
				offsets[shardindex] += edges * sizeof(edge_with_value<EdgeDataType>);
				packages[shardindex]++;
				while(sub_done == 0) usleep(5000);
			}
			return 0;
		}
		
		int finish_reshovel(edge_with_value<EdgeDataType> ** buff , size_t * edges_in_buff)
		{
			int * sub_done = (int*)malloc(nshards*sizeof(int));
			memset(sub_done , 0 , sizeof(int)*nshards);
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				if(iter%machines != machines-1)
				{
					char buffer[128];
					sprintf(buffer , "Shovel:%d Offset:%ld" , iter , offsets[iter]);
					logstream(LOG_DEBUG) << "Will send " << edges_in_buff[iter] * sizeof(edge_with_value<EdgeDataType>) << " bytes" << std::endl;
					dt.AddTask((char*)buff[iter] , edges_in_buff[iter] * sizeof(edge_with_value<EdgeDataType>) , std::string(buffer) , IPs[iter%machines] , sub_done+iter);
					offsets[iter] += edges_in_buff[iter] * sizeof(edge_with_value<EdgeDataType>);
					packages[iter]++;
					while(sub_done[iter] == 0 ) usleep(5000);
				}
			}
			for(int iter = 0 ; iter != IPs.size() ; ++iter)
			{
				int * packbuff = (int*) calloc(nshards/machines , sizeof(int));
				int index=0;
				for(int i = iter ; i < nshards ; i+=machines)
				{
					packbuff[index++] = packages[i];
				}
				dt.AddTask((char*)packbuff , index*sizeof(int) , "Reshard" , IPs[iter] , NULL);
			}
			for(int iter = machines-1 ; iter < nshards ; iter += machines)
			{
				assert(fds[iter]>=0);
				writea(fds[iter] , buff[iter] , edges_in_buff[iter] * sizeof(edge_with_value<EdgeDataType>));
				sub_done[iter]=1;
			}
			free(offsets);
			free(sub_done);
			free(fds);
			free(packages);
			return 0;
		}

		void reshovel()
		{
			int times_of_shove = 2;
			
			size_t membudget_mb = get_option_int("membudget_mb", 1024);
			size_t membuffer = (membudget_mb * 1024 * 1024.  - (double)(max_vertex_id+1) * ( sizeof(vid_t) + sizeof(shard_t) + sizeof(degree) )) * 0.8; 
			
			if(opti_times == 0)
			{
				membuffer = (membudget_mb * 1024 * 1024. - (double)(max_vertex_id+1) * ( sizeof(degree))) * 0.8;
			}
			
			logstream(LOG_DEBUG) << "Assuming membudget is " << membuffer/1024/1024 << " MB" << std::endl; 
			
			size_t buffsize = membuffer / (nshards + times_of_shove);
			while(buffsize % sizeof(edge_with_value<EdgeDataType>) != 0) buffsize--;
			size_t edges_max_in_buffer = buffsize/sizeof(edge_with_value<EdgeDataType>);
			
			logstream(LOG_DEBUG) << "Buffersize is " << buffsize/1024/1024 << " MB" << std::endl; 
			logstream(LOG_DEBUG) << "Save " << edges_max_in_buffer << " edges per time" << std::endl;

			
			size_t buffsize_shovel = times_of_shove * buffsize;
			edge_with_value<EdgeDataType> * buff_shovel = (edge_with_value<EdgeDataType> *) malloc(buffsize_shovel);
			assert(buff_shovel != NULL);
			
			edge_with_value<EdgeDataType> ** buff = (edge_with_value<EdgeDataType> **) calloc(nshards , sizeof(edge_with_value<EdgeDataType>*));
			assert(buff != NULL);
			
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				buff[iter] = (edge_with_value<EdgeDataType> *) malloc(buffsize);
				assert(buff[iter] != NULL);
			}
			
			size_t * edges_in_buff = (size_t *) calloc( nshards , sizeof(size_t) );
			memset(edges_in_buff , 0 , nshards * sizeof(size_t));
			
			size_t edges_max_in_shovel_buffer = edges_max_in_buffer * times_of_shove;
			
			int src_index , dst_index , src_interval , dst_interval;
			
			logstream(LOG_INFO) << "Preallocate Finished , Now start to reshovel..." << std::endl;
			
			m.start_time("reshovel");
			
			m.start_time("Write Reshovel");
			start_reshovel(buff , edges_in_buff);
			m.stop_time("Write Reshovel");
			
			for(int iter = 0 ; iter != numshovels ; ++iter)
			{
				std::string filename = shovel_filename(iter);
				int f = open(filename.c_str() , O_RDONLY);
				off_t sz = lseek(f , 0 , SEEK_END);
				lseek(f , 0 , SEEK_SET);
				
				size_t edges_in_this_shovel = sz/sizeof(edge_with_value<EdgeDataType>);
				size_t edges_read = 0;
				size_t idx = 0;
				edge_with_value<EdgeDataType> temp;
				while(edges_read < edges_in_this_shovel)
				{
					if(idx == 0 || idx == edges_max_in_shovel_buffer )
					{
						m.start_time("Read Reshovel");
						reada(f , buff_shovel , buffsize_shovel);
						m.stop_time("Read Reshovel");
						idx = 0;
					}
					temp = buff_shovel[idx++];
					
					if(opti_times != 0)
					{
						src_index=vertexs_map[temp.src];
						dst_index=vertexs_map[temp.dst];
						
						src_interval = vertex_to_bucks[temp.src];
						dst_interval = vertex_to_bucks[temp.dst];
						
						temp.src = src_index;
						temp.dst = dst_index;
						
					}
					else
					{
						for(int index = 0 ; index != nshards ; ++index)
						{
							if( intervals[index].first <= temp.src && temp.src <= intervals[index].second )
							{
								src_interval = index;
								break;
							}
						}
						for(int index = 0 ; index != nshards ; ++index)
						{
							if( intervals[index].first <= temp.dst && temp.dst <= intervals[index].second )
							{
								dst_interval = index;
								break;
							}
						}
					}
					
					assert( (intervals[src_interval].first <= temp.src) && (temp.src <= intervals[src_interval].second) );
					assert( (intervals[dst_interval].first <= temp.dst) && (temp.dst <= intervals[dst_interval].second) );
					
#ifndef DYNAMICEDATA
					if(temp.src != temp.dst)
					{
						degrees[temp.src].outdegree++;
						degrees[temp.dst].indegree++;
					}
#else
					if( (temp.src != temp.dst) && (temp.valindex == 0) )
					{
						degrees[temp.src].outdegree++;
						degrees[temp.dst].indegree++;
					}
#endif

					buff[dst_interval][edges_in_buff[dst_interval]]=temp;
					if( edges_in_buff[dst_interval] == ( edges_max_in_buffer - 1 ) )
					{
						m.start_time("Write Reshovel");
						finish_reshovel_block(buff[dst_interval] , edges_max_in_buffer , dst_interval);
						m.stop_time("Write Reshovel");
						edges_in_buff[dst_interval] = 0;
					}
					else
					{
						edges_in_buff[dst_interval]++;
					}
					if(src_interval != dst_interval)
					{
						buff[src_interval][edges_in_buff[src_interval]]=temp;
						if(edges_in_buff[src_interval] == ( edges_max_in_buffer - 1 ) )
						{
							m.start_time("Write Reshovel");
							finish_reshovel_block(buff[src_interval] , edges_max_in_buffer , src_interval);
							m.stop_time("Write Reshovel");
							edges_in_buff[src_interval] = 0;
						}
						else
						{
							edges_in_buff[src_interval]++;
						}
					}
					edges_read++;
				}
				
				logstream(LOG_INFO) << "Reshovel shovel file " << iter << " finished" << std::endl;
				
				close(f);
			}
			
			m.start_time("Write Reshovel");
			finish_reshovel(buff , edges_in_buff);
			m.stop_time("Write Reshovel");
			
			m.stop_time("reshovel");
			
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				free(buff[iter]);
			}
			free(buff);
			free(edges_in_buff);
			free(buff_shovel);
			
		}
		
		struct write_sliding
		{
			int f_s , f_e[2];
			char * s_buff , * e_buff[2];
			vid_t curvid;
			char * s_buffed ,* e_buffed[2];
			bool vid_written;
#ifdef DYNAMICEDATA
			long int uniq_edges;
#endif
		};
		
		public:
		
		void reshard(int shard , edge_with_value<EdgeDataType> * shovelbuf , long edges )
		{
			m.start_time("reshard");
#ifdef DYNAMICEDATA
			int mem_edges=0;
			int written_edges=0;
			int jumpover = 0;
            size_t edgecounter = 0;
			vid_t lastdst=0xffffffff;
			size_t num_uniq_edges = 0;
            size_t last_edge_count = 0;
			long int * uniq_edges = (long int *)calloc(nshards , sizeof(long int));
			int current_writing_shard = -1;
#endif

			int current_sliding_shard = -1;
			int current_memory_shard = 0;
			long int edatasize = 0;
			
			vid_t zeros=0 , non_zeros=0;
			
            logstream(LOG_INFO) << "Starting final processing for reshard: " << shard << std::endl;
            
			std::string sddirname = dirname_shard(basefilename , shard , nshards);
			
            std::string fname = filename_shard_adj(basefilename, shard, nshards);
            std::string edfname = filename_shard_edata<FinalEdgeDataType>(basefilename, shard, nshards);
            //std::string edblockdirname = dirname_shard_edata_block(edfname, compressed_block_size);
            
			unsigned int pos;
			if( (pos = fname.find_last_of('/')) != std::string::npos)
			{
				fname = fname.substr(pos+1);
			}
			if( (pos = edfname.find_last_of('/')) != std::string::npos)
			{
				edfname = edfname.substr(pos+1);
			}
			/* if( (pos = edblockdirname.find_last_of('/')) != std::string::npos)
			{
				edblockdirname = edblockdirname.substr(pos+1);
			}  */
			
			std::string save;
			
			fname = sddirname + '/' + fname;
			edfname = sddirname + '/' + edfname;
			
			save = edfname;
			edfname += ".in";
			//edblockdirname = sddirname + '/' + edblockdirname;
			
			mkdir(sddirname.c_str(), 0777);
			
            /* Make the block directory */
            
			/* if (!no_edgevalues)
                mkdir(edblockdirname.c_str(), 0777); */
            
			size_t numedges = edges;
            
			
			int vertex_start = intervals[shard].first;
			int vertex_end = intervals[shard].second;
			
            logstream(LOG_DEBUG) << "Shovel size:" << edges*sizeof(edge_t) << " edges: " << edges << std::endl;
            
            logstream(LOG_DEBUG) << "Now Sorting for the reshard " << shard << std::endl;
			m.start_time("finish_reshard.sort");

			quickSort(shovelbuf, (int)numedges, edge_t_src_less<EdgeDataType>);
			
            m.stop_time("finish_reshard.sort");
			logstream(LOG_DEBUG) << "Sorting done" << std::endl;
            
            // Create the final file
            int f = open(fname.c_str(), O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (f < 0) {
                logstream(LOG_ERROR) << "Could not open " << fname << " error: " << strerror(errno) << std::endl;
            }
            assert(f >= 0);
            int trerr = ftruncate(f, 0);
            assert(trerr == 0);
			
			int f_e = open(edfname.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
            if (f_e < 0) {
                logstream(LOG_ERROR) << "Could not open " << edfname << " error: " << strerror(errno) << std::endl;
            }
            assert(f_e >= 0);
            trerr = ftruncate(f_e, 0);
            assert(trerr == 0);
            
			int f_sliding = -1;
			char * buf_sliding = NULL;
			
			//modified here , for creating the sliding_shard files
			/* int f_sliding = -1;
			int f_e = -1;
			char * buf_sliding = NULL;
			
            char * buf = (char*) malloc(SHARDER_BUFSIZE);
            char * bufptr = buf;
			
			/* char * buf_sliding = buf; */
			
            /* char * ebuf = (char*) malloc(compressed_block_size);
            ebuffer_size = compressed_block_size;
            char * ebufptr = ebuf;  */
            
			write_sliding * wsl = new write_sliding[nshards];
			
			char * buf = (char*) malloc(compressed_block_size);
            char * bufptr = buf;
			
			char * ebuf = (char*) malloc(compressed_block_size);
            ebuffer_size = compressed_block_size;
            char * ebufptr = ebuf;
			
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				if(iter != shard)
				{
					wsl[iter].s_buff = (char*)malloc(compressed_block_size);
					wsl[iter].e_buff[0] = (char*)malloc(compressed_block_size);
					wsl[iter].e_buff[1] = (char*)malloc(compressed_block_size);
					wsl[iter].s_buffed = wsl[iter].s_buff;
					wsl[iter].e_buffed[0] = wsl[iter].e_buff[0];
					wsl[iter].e_buffed[1] = wsl[iter].e_buff[1];
					wsl[iter].curvid = intervals[shard].first;
					wsl[iter].vid_written = false;
					std::string filename;
					filename = filename_shard_adj(basefilename , iter , nshards);
					if( (pos = filename.find_last_of('/')) != std::string::npos )
					{
						filename = filename.substr(pos+1);
					}
					filename = sddirname + "/" + filename;
					wsl[iter].f_s = open(filename.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
					trerr = ftruncate(wsl[iter].f_s, 0);
					assert(trerr == 0);
					filename = filename_shard_edata<FinalEdgeDataType>(basefilename , iter , nshards);
					if( (pos = filename.find_last_of('/')) != std::string::npos )
					{
						filename = filename.substr(pos+1);
					}
					filename = sddirname + "/" + filename;
					std::string tempname = filename+".in";
					wsl[iter].f_e[0] = open(tempname.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
					tempname = filename + ".out";
					wsl[iter].f_e[1] = open(tempname.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
					trerr = ftruncate(wsl[iter].f_e[0], 0);
					assert(trerr == 0);
					trerr = ftruncate(wsl[iter].f_e[1], 0);
					assert(trerr == 0);
#ifdef DYNAMICEDATA
					wsl[iter].uniq_edges=0;
#endif
				}
				else
				{
					wsl[iter].s_buff = buf;
					wsl[iter].e_buff[1] = ebuf;
					wsl[iter].s_buffed = buf ;
					wsl[iter].e_buffed[1] = ebuf;
					wsl[iter].curvid = 0;
					wsl[iter].f_s = f;
					wsl[iter].f_e[1] = f_e;
					wsl[iter].vid_written = false;
				}
			}
			
			//modified here
			//char * lastbuf
			
            vid_t curvid=0;
			bool willout=false;

            size_t istart = 0;
            //size_t tot_edatabytes = 0; 
			int index=0;
			
			int edge_values;
			
            for(size_t i=0; i <= numedges; i++) {
                if (  (i & 0xFFFFF) == 0) logstream(LOG_DEBUG) << i << " / " << numedges << std::endl;
#ifdef DYNAMICEDATA
                i += jumpover;  // With dynamic values, there might be several values for one edge, and thus the edge repeated in the data.
                jumpover = 0;
#endif //DYNAMICEDATA
                edge_t edge = (i < numedges ? shovelbuf[i] : edge_t(0, 0, EdgeDataType())); // Last "element" is a stopper
				
#ifdef DYNAMICEDATA
                
                if (lastdst == edge.dst && edge.src == curvid) {
                    // Currently not supported
                    logstream(LOG_ERROR) << "Duplicate edge in the stream - aborting" << std::endl;
                    assert(false);
                }
                lastdst = edge.dst;
#endif
				
				if(!edge.stopper())
				{
					while( (current_sliding_shard == -1) || (edge.src > intervals[current_sliding_shard].second) )
					{
						current_sliding_shard++;
						logstream(LOG_DEBUG) << "Shard move to " << current_sliding_shard << std::endl;
					}
#ifdef DYNAMICEDATA
					while( (current_writing_shard == -1) || (curvid > intervals[current_writing_shard].second) )
					{
						current_writing_shard++;
					}
					if(edge.is_chivec_value)
					{
						int count=1;
						 while(shovelbuf[i + count].valindex == count) { count++; }
                        
						edge_values=count;
						jumpover = count-1;
					}
					else
					{
						edge_values=0;
					}
					num_uniq_edges++;
#else
					edge_values=1;
#endif			
					if(current_sliding_shard != shard)
					{
#ifndef DYNAMICEDATA
						if( ( wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0] + sizeof(FinalEdgeDataType) ) > compressed_block_size)
						{
							edatasize += wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0];
							writea(wsl[current_sliding_shard].f_e[0] , wsl[current_sliding_shard].e_buff[0] , wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0]);
							wsl[current_sliding_shard].e_buffed[0] = wsl[current_sliding_shard].e_buff[0];
						}
						*(FinalEdgeDataType*) (wsl[current_sliding_shard].e_buffed[0]) = (FinalEdgeDataType)edge.value;
						wsl[current_sliding_shard].e_buffed[0] += sizeof(FinalEdgeDataType);
#else
						if( ( wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0] + sizeof(uint16_t)*2 + sizeof(FinalEdgeDataType)*edge_values ) > compressed_block_size)
						{
							edatasize += wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0];
							writea(wsl[current_sliding_shard].f_e[0] , wsl[current_sliding_shard].e_buff[0] , wsl[current_sliding_shard].e_buffed[0] - wsl[current_sliding_shard].e_buff[0]);
							wsl[current_sliding_shard].e_buffed[0] = wsl[current_sliding_shard].e_buff[0];
						}
						*(uint16_t*)wsl[current_sliding_shard].e_buffed[0] = (uint16_t)edge_values;
						wsl[current_sliding_shard].e_buffed[0] += sizeof(uint16_t);
						*(uint16_t*)wsl[current_sliding_shard].e_buffed[0] = (uint16_t)edge_values;
						wsl[current_sliding_shard].e_buffed[0] += sizeof(uint16_t);
						for(int value_index = 0 ; value_index != edge_values ; ++value_index)
						{
							*(FinalEdgeDataType*) (wsl[current_sliding_shard].e_buffed[0]) = (FinalEdgeDataType)shovelbuf[i+value_index].value;
							wsl[current_sliding_shard].e_buffed[0] += sizeof(FinalEdgeDataType);
						}
#endif
					}
					else
					{
						willout = true;
						for(int iter = 0 ; iter != nshards ; ++iter)
						{
							if(intervals[iter].first <= edge.dst && edge.dst <= intervals[iter].second)
							{
								index = iter;
								break;
							}
						}
#ifndef DYNAMICEDATA
						if( (wsl[index].e_buffed[1] - wsl[index].e_buff[1] + sizeof(FinalEdgeDataType)) > compressed_block_size)
						{
							if(index == shard)
							{
								edatasize += wsl[index].e_buffed[1] - wsl[index].e_buff[1];
							}
							writea(wsl[index].f_e[1] , wsl[index].e_buff[1] , wsl[index].e_buffed[1] - wsl[index].e_buff[1]);
							wsl[index].e_buffed[1] = wsl[index].e_buff[1];
						}
						*(FinalEdgeDataType*) (wsl[index].e_buffed[1]) = (FinalEdgeDataType)edge.value;
						wsl[index].e_buffed[1] += sizeof(FinalEdgeDataType);
#else
						if( ( wsl[index].e_buffed[1] - wsl[index].e_buff[1] + sizeof(uint16_t)*2 + sizeof(FinalEdgeDataType)*edge_values ) > compressed_block_size)
						{
							writea(wsl[index].f_e[1] , wsl[index].e_buff[1] , wsl[index].e_buffed[1] - wsl[index].e_buff[1]);
							wsl[index].e_buffed[1] = wsl[index].e_buff[1];
						}
						*(uint16_t*)wsl[index].e_buffed[1] = (uint16_t)edge_values;
						wsl[index].e_buffed[1] += sizeof(uint16_t);
						*(uint16_t*)wsl[index].e_buffed[1] = (uint16_t)edge_values;
						wsl[index].e_buffed[1] += sizeof(uint16_t);
						for(int value_index = 0 ; value_index != edge_values ; ++value_index)
						{
							*(FinalEdgeDataType*) (wsl[index].e_buffed[1]) = (FinalEdgeDataType)shovelbuf[i+value_index].value;
							wsl[index].e_buffed[1] += sizeof(FinalEdgeDataType);
						}
#endif
						while( (current_memory_shard != index) || (edge.src != wsl[current_memory_shard].curvid) )
						{
#ifndef DYNAMICEDATA
							size_t count = i - istart;
#else
							size_t count = num_uniq_edges - 1 - last_edge_count;
							last_edge_count = num_uniq_edges - 1;
							if (edge.stopper()) count++;
#endif
							if(count > 0)
							{
								if( (wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(uint8_t) + sizeof(uint32_t)) > compressed_block_size)
								{
									writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
									wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
								}
								if(count < 255)
								{
									*(uint8_t*)( wsl[current_memory_shard].s_buffed) = (uint8_t)count;
									wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
								}
								else
								{
									*(uint8_t*)(wsl[current_memory_shard].s_buffed) = (uint8_t)0xff;
									wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
									*(uint32_t*)(wsl[current_memory_shard].s_buffed) = (uint32_t)count;
									wsl[current_memory_shard].s_buffed += sizeof(uint32_t);
								}
								for(size_t j = istart ; j != i ; ++j)
								{
									if((wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(vid_t)) > compressed_block_size)
									{
										writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
										wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
									}
									assert((intervals[current_memory_shard].first <= shovelbuf[j].dst) && (shovelbuf[j].dst <= intervals[current_memory_shard].second));
#ifndef DYNAMICEDATA
									*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
									wsl[current_memory_shard].s_buffed += sizeof(vid_t);
#else
									if(j == istart || shovelbuf[j-1].dst != shovelbuf[j].dst)
									{
										*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
										wsl[current_memory_shard].s_buffed += sizeof(vid_t);
										written_edges++;
									}
#endif
								}
								if(current_memory_shard == shard)
								{
									non_zeros++;
#ifdef DYNAMICEDATA
									mem_edges+=count;
									uniq_edges[current_writing_shard]+=count;
#endif
								}
#ifdef DYNAMICEDATA
								else
								{
									wsl[current_memory_shard].uniq_edges += count;
								}
#endif
								wsl[current_memory_shard].vid_written = true;
							}
							
							if(edge.src > ( wsl[current_memory_shard].curvid + 1 - !(wsl[current_memory_shard].vid_written)) )
							{
								int nz = edge.src - wsl[current_memory_shard].curvid - 1 + !(wsl[current_memory_shard].vid_written);
								if(current_memory_shard == shard) zeros += nz;
								do {
									if((wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(uint8_t) * 2) > compressed_block_size)
									{
										writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
										wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
									}
									*(uint8_t*)(wsl[current_memory_shard].s_buffed) = (uint8_t)0;
									wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
									nz--;
									int tnz = std::min(254, nz);
									*(uint8_t*)(wsl[current_memory_shard].s_buffed) = (uint8_t)tnz;
									nz -= tnz;
									wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
								} while (nz>0);
							}
							
							if(wsl[current_memory_shard].curvid == edge.src)
							{
								wsl[current_memory_shard].vid_written = true;
							}
							else
							{
								wsl[current_memory_shard].vid_written = false;
							}
							
							if(i==0)
							{
								wsl[current_memory_shard].vid_written = false;
							}
							
							wsl[current_memory_shard].curvid = edge.src;
							istart = i;
#ifdef DYNAMICEDATA
							istart += jumpover;
#endif
							current_memory_shard = index;
						}
						
						curvid = edge.src;
						
					}
					
					if( ( current_sliding_shard == (shard+1) ) && (willout) )
					{
						willout = false;
#ifndef DYNAMICEDATA
						size_t count = i - istart;
#else
						size_t count = num_uniq_edges - 1 - last_edge_count;
						last_edge_count = num_uniq_edges - 1;
						if (edge.stopper()) count++;
#endif
						if(count > 0)
						{
							if( (wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(uint8_t) + sizeof(uint32_t)) > compressed_block_size)
							{
								writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
								wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
							}
							if(count < 255)
							{
								*(uint8_t*)( wsl[current_memory_shard].s_buffed) = (uint8_t)count;
								wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
							}
							else
							{
								*(uint8_t*)(wsl[current_memory_shard].s_buffed) = (uint8_t)0xff;
								wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
								*(uint32_t*)(wsl[current_memory_shard].s_buffed) = (uint32_t)count;
								wsl[current_memory_shard].s_buffed += sizeof(uint32_t);
							}
							for(size_t j = istart ; j != i ; ++j)
							{
								if((wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(vid_t)) > compressed_block_size)
								{
									writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
									wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
								}
#ifndef DYNAMICEDATA
								*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
								wsl[current_memory_shard].s_buffed += sizeof(vid_t);
#else
								if(j==istart || shovelbuf[j-1].dst != shovelbuf[j].dst)
								{
									*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
									wsl[current_memory_shard].s_buffed += sizeof(vid_t);
									written_edges++;
								}
#endif
							}
							wsl[current_memory_shard].vid_written = true;
						}
						
#ifdef DYNAMICEDATA
						if(current_memory_shard == shard)
						{
							mem_edges+=count;
							uniq_edges[current_writing_shard]+=count;
						}
						else
						{
							wsl[current_memory_shard].uniq_edges+=count;
						}
#endif
						
						istart = i;
#ifdef DYNAMICEDATA
						istart += jumpover;
#endif
						current_memory_shard = shard;
					}
					
					if( (edge.src != curvid) )
					{
#ifndef DYNAMICEDATA
						size_t count = i - istart;
#else
						size_t count = num_uniq_edges - 1 - last_edge_count;
						last_edge_count = num_uniq_edges - 1;
						if (edge.stopper()) count++;
#endif
						if(count>0)
						{
							if(wsl[shard].s_buffed - wsl[shard].s_buff + sizeof(uint8_t) + sizeof(uint32_t) > compressed_block_size)
							{
								writea(wsl[shard].f_s , wsl[shard].s_buff , wsl[shard].s_buffed - wsl[shard].s_buff);
								wsl[shard].s_buffed = wsl[shard].s_buff;
							}
							if(count < 255)
							{
								*(uint8_t*)(wsl[shard].s_buffed) = (uint8_t)count;
								wsl[shard].s_buffed += sizeof(uint8_t);
							}
							else
							{
								*(uint8_t*)(wsl[shard].s_buffed) = (uint8_t)0xff;
								wsl[shard].s_buffed += sizeof(uint8_t);
								*(uint32_t*)(wsl[shard].s_buffed) = (uint32_t)count;
								wsl[shard].s_buffed += sizeof(uint32_t);
							}
							for(size_t j = istart ; j != i ; ++j)
							{
								if(wsl[shard].s_buffed - wsl[shard].s_buff + sizeof(vid_t) > compressed_block_size)
								{
									writea(wsl[shard].f_s , wsl[shard].s_buff , wsl[shard].s_buffed - wsl[shard].s_buff);
									wsl[shard].s_buffed = wsl[shard].s_buff;
								}
#ifndef DYNAMICEDATA
								*(vid_t*)(wsl[shard].s_buffed) = shovelbuf[j].dst;
								wsl[shard].s_buffed += sizeof(vid_t);
#else
								if(j==istart || shovelbuf[j-1].dst != shovelbuf[j].dst)
								{
									*(vid_t*)(wsl[shard].s_buffed) = shovelbuf[j].dst;
									wsl[shard].s_buffed += sizeof(vid_t);
									written_edges++;
								}
#endif
							}
							non_zeros++;
#ifdef DYNAMICEDATA
							mem_edges+=count;
							uniq_edges[current_writing_shard]+=count;
#endif
							wsl[shard].vid_written = true;
						}
						
						istart = i;
#ifdef DYNAMICEDATA
						istart += jumpover;
#endif
						if(edge.src > (wsl[shard].curvid + 1 - !(wsl[shard].vid_written) ) )
						{
							int nz = edge.src - wsl[shard].curvid - 1 + !(wsl[shard].vid_written);
							zeros += nz;
							do {
								if(wsl[shard].s_buffed - wsl[shard].s_buff + sizeof(uint8_t) * 2 > compressed_block_size)
								{
									writea(wsl[shard].f_s , wsl[shard].s_buff , wsl[shard].s_buffed - wsl[shard].s_buff);
									wsl[shard].s_buffed = wsl[shard].s_buff;
								}
								*(uint8_t*)(wsl[shard].s_buffed) = (uint8_t)0;
								nz--;
								wsl[shard].s_buffed += sizeof(uint8_t);
								int tnz = std::min(254, nz);
								*(uint8_t*)(wsl[shard].s_buffed) = (uint8_t)tnz;
								nz -= tnz;
								wsl[shard].s_buffed += sizeof(uint8_t);
							} while (nz>0);
						}
						wsl[shard].curvid = edge.src;
						wsl[shard].vid_written = false;
						current_memory_shard = shard;
					
					}
			
					curvid = edge.src;
					
				}
				else
				{
#ifndef DYNAMICEDATA
					size_t count = i - istart;
#else
					size_t count = num_uniq_edges - 1 - last_edge_count;
					last_edge_count = num_uniq_edges - 1;
					if (edge.stopper()) count++; 
#endif
					if(count > 0)
					{
						if( (wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(uint8_t) + sizeof(uint32_t)) > compressed_block_size)
						{
							writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
							wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
						}
						if(count < 255)
						{
							*(uint8_t*)( wsl[current_memory_shard].s_buffed) = (uint8_t)count;
							wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
						}
						else
						{
							*(uint8_t*)(wsl[current_memory_shard].s_buffed) = (uint8_t)0xff;
							wsl[current_memory_shard].s_buffed += sizeof(uint8_t);
							*(uint32_t*)(wsl[current_memory_shard].s_buffed) = (uint32_t)count;
							wsl[current_memory_shard].s_buffed += sizeof(uint32_t);
						}
						for(size_t j = istart ; j != i ; ++j)
						{
							if((wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff + sizeof(vid_t)) > compressed_block_size)
							{
								writea(wsl[current_memory_shard].f_s , wsl[current_memory_shard].s_buff , wsl[current_memory_shard].s_buffed - wsl[current_memory_shard].s_buff);
								wsl[current_memory_shard].s_buffed = wsl[current_memory_shard].s_buff;
							}
#ifndef DYNAMICEDATA
							*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
							wsl[current_memory_shard].s_buffed += sizeof(vid_t);
#else
							if(j==istart || shovelbuf[j-1].dst != shovelbuf[j].dst)
							{
								*(vid_t*)(wsl[current_memory_shard].s_buffed) = shovelbuf[j].dst;
								wsl[current_memory_shard].s_buffed += sizeof(vid_t);
								written_edges++;
							}
#endif
						}
#ifdef DYNAMICEDATA
						if(current_memory_shard == shard)
						{
						  mem_edges += count;
						  uniq_edges[current_writing_shard]+=count;
						}
						else
						{
							wsl[current_memory_shard].uniq_edges+=count;
						}
#endif
						non_zeros++;
					}
				}
				
/* #ifdef DYNAMICEDATA
                
                if (lastdst == edge.dst && edge.src == curvid) {
                    // Currently not supported
                    logstream(LOG_ERROR) << "Duplicate edge in the stream - aborting" << std::endl;
                    assert(false);
                }
                lastdst = edge.dst;
#endif */
                
/*                if (!edge.stopper()) {
#ifndef DYNAMICEDATA
                    bwrite_edata<FinalEdgeDataType>(ebuf, ebufptr, FinalEdgeDataType(edge.value), tot_edatabytes, edfname, edgecounter);
#else
                    /* If we have dynamic edge data, we need to write the header of chivector - if there are edge values */
                    /*if (edge.is_chivec_value) {
                        // Need to check how many values for this edge
                        int count = 1;
                        while(shovelbuf[i + count].valindex == count) { count++; }
                        
                        assert(count < 32768);
                        
                        typename chivector<EdgeDataType>::sizeword_t szw;
                        ((uint16_t *) &szw)[0] = (uint16_t)count;  // Sizeword with length and capacity = count
                        ((uint16_t *) &szw)[1] = (uint16_t)count;
                        bwrite_edata<typename chivector<EdgeDataType>::sizeword_t>(ebuf, ebufptr, szw, tot_edatabytes, edfname, edgecounter);
                        for(int j=0; j < count; j++) {
                            bwrite_edata<EdgeDataType>(ebuf, ebufptr, EdgeDataType(shovelbuf[i + j].value), tot_edatabytes, edfname, edgecounter);
                        }
                        jumpover = count - 1; // Jump over
                    } else {
                        // Just write size word with zero
                        bwrite_edata<int>(ebuf, ebufptr, 0, tot_edatabytes, edfname, edgecounter);
                    }
                    num_uniq_edges++;
                    
#endif
                    edgecounter++; // Increment edge counter here --- notice that dynamic edata case makes two or more calls to bwrite_edata before incrementing
                }
				*/
                /* if (edge.src != edge.dst) {
					if( (vertex_start <= edge.src) && (edge.src <= vertex_end) )
					{
						degrees[edge.src - vertex_start].outdegree++;
                    }
					if( (vertex_start <= edge.dst) && (edge.dst <= vertex_end) )
					{
						degrees[edge.dst - vertex_start].indegree++;
					}
                } */
					
					
            }
            
            /* Flush buffers and free memory */
            writea(f, buf, wsl[shard].s_buffed - wsl[shard].s_buff);
            free(buf);
            free(shovelbuf);
            close(f);
            
			
			edatasize += wsl[shard].e_buffed[1] - wsl[shard].e_buff[1];
			writea(f_e , ebuf , wsl[shard].e_buffed[1]-wsl[shard].e_buff[1]);
			free(ebuf);
			close(f_e);
			
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				if(iter != shard)
				{
					edatasize += wsl[iter].e_buffed[0] - wsl[iter].e_buff[0];
					writea(wsl[iter].f_s , wsl[iter].s_buff , wsl[iter].s_buffed - wsl[iter].s_buff);
					writea(wsl[iter].f_e[0] , wsl[iter].e_buff[0] , wsl[iter].e_buffed[0] - wsl[iter].e_buff[0]);
					writea(wsl[iter].f_e[1] , wsl[iter].e_buff[1] , wsl[iter].e_buffed[1] - wsl[iter].e_buff[1]);
					free(wsl[iter].s_buff);
					free(wsl[iter].e_buff[0]);
					free(wsl[iter].e_buff[1]);
					close(wsl[iter].f_s);
					close(wsl[iter].f_e[0]);
					close(wsl[iter].f_e[1]);
#ifdef DYNAMICEDATA
					std::string filename_size = filename_shard_edata<FinalEdgeDataType>(basefilename , iter , nshards);
					if( (pos = filename_size.find_last_of('/')) != std::string::npos )
					{
						filename_size = filename_size.substr(pos+1);
					}
					filename_size = sddirname + "/" + filename_size + ".out.size";
					FILE * fp = fopen(filename_size.c_str() , "w");
					assert(fp != NULL);
					fprintf(fp , "%ld" , wsl[iter].uniq_edges*sizeof(int));
					fclose(fp);
#endif
				}
#ifdef DYNAMICEDATA
				std::string filename_size = filename_shard_edata<FinalEdgeDataType>(basefilename , iter , nshards);
				if( (pos = filename_size.find_last_of('/')) != std::string::npos )
				{
					filename_size = filename_size.substr(pos+1);
				}
				filename_size = sddirname + "/" + filename_size + ".in.size";
				FILE * fp = fopen(filename_size.c_str() , "w");
				assert(fp != NULL);
				fprintf(fp , "%ld" , uniq_edges[iter]*sizeof(int));
				fclose(fp);
#endif
			}
			delete [] wsl;
			
			std::string filename_edata_size = save + ".size";
			
			FILE * fp = fopen(filename_edata_size.c_str() , "w");
			
			assert(fp != NULL);
#ifndef DYNAMICEDATA
			fprintf(fp , "%ld" , edatasize);
#else
			fprintf(fp , "%ld" , mem_edges*sizeof(int));
			logstream(LOG_DEBUG) << "Num_uniq:" << num_uniq_edges << " Written:" << written_edges << std::endl;
			free(uniq_edges);
#endif
			fclose(fp);
			
			/* Write edata size file */
            /* if (!no_edgevalues) {
                edata_flush<FinalEdgeDataType>(ebuf, ebufptr, edfname, tot_edatabytes);
                
                std::string sizefilename = edfname + ".size";
                std::ofstream ofs(sizefilename.c_str());
#ifndef DYNAMICEDATA
                ofs << tot_edatabytes;
#else
                ofs << num_uniq_edges * sizeof(int); // For dynamic edge data, write the number of edges.
#endif
                
                ofs.close();
            }
            free(ebuf); */
            
            m.stop_time("reshard");
		}
		
		void sub_startup(std::string base , shard_t shardcount , std::vector< std::pair<vid_t , vid_t> > & invals)
		{
			basefilename = base;
			nshards = shardcount;
			intervals = invals;
			
			logstream(LOG_INFO) << "Ready for reshard..." << std::endl;
			logstream(LOG_INFO) << "Shards:" << nshards << std::endl;
			for(int iter = 0 ; iter != intervals.size() ; ++iter)
			{
				logstream(LOG_INFO) << "Interval " << iter << ":" << intervals[iter].first << "\t" << intervals[iter].second << std::endl;
			}
			
			max_vertex_id = intervals[nshards-1].second;
			
			logstream(LOG_INFO) << "MAX VERTEX ID: " << max_vertex_id <<std::endl;
		}
		
        template <typename A, typename B> friend class sharded_graph_output;
    }; // End class sharder
    
    
    /**
     * Outputs new edges into a shard - can be used from an update function
     */
	 
	class shard_sub_mode
	{
		std::string edge_type;
		std::string basefilename;
		shard_t nshards;
		std::vector< std::pair<vid_t , vid_t> > intervals;
		std::vector< std::pair<shard_t , long> > duties;
		int sub_done , interval_done;
		int edge_size;
		DataTransfer dt;
		bool shovel_in_mem;
		int * fds;
		int * packages;
		int * edges;
		char ** shovel_mem;
		pthread_mutex_t edge_mutex;
		
		public:
		/*
		**This are subfunctions, using when sharder runs with sub_mode
		*/
		shard_sub_mode(std::string edgetype)
		{
			edge_type = edgetype;
			duties.clear();
			shovel_in_mem=false;
			interval_done=0;
			shovel_mem=NULL;
			fds=NULL;
			edges=NULL;
			if(edge_check() != 0)
			{
				exit(-1);
			}
			pthread_mutex_init(&edge_mutex , NULL);
		}
		
		int edge_check()
		{
			if (edge_type == "float") {
				edge_size=sizeof(edge_with_value<float>);
			} else if (edge_type == "float-float") {
				edge_size=sizeof(edge_with_value< PairContainer<float> >);
			} else if (edge_type == "int") {
				edge_size=sizeof(edge_with_value<int>);
			} else if (edge_type == "uint") {
				edge_size=sizeof(edge_with_value<unsigned int>);
			} else if (edge_type == "int-int") {
				edge_size=sizeof(edge_with_value< PairContainer<int> >);
			} else if (edge_type == "short") {
				edge_size=sizeof(edge_with_value<short>);
			} else if (edge_type == "double") {
				edge_size=sizeof(edge_with_value<double>);
			} else if (edge_type == "char") {
				edge_size=sizeof(edge_with_value<char>);
			} else if (edge_type == "boolean") {
				edge_size=sizeof(edge_with_value<bool>);
			} else if (edge_type == "long") {
				edge_size=sizeof(edge_with_value<long>);
			} else if (edge_type == "none") {
				edge_size=sizeof(edge_with_value<char>);
			} else if (edge_type == "length16") {
				edge_size = sizeof(length16);
			} else if (edge_type == "length24") {
				edge_size = sizeof(length24);
			} else if (edge_type == "length32") {
				edge_size = sizeof(length32);
			} else if (edge_type == "length64") {
				edge_size = sizeof(length64);
			} else {
				logstream(LOG_ERROR) << "You need to specify edgedatatype. Currently supported: int, short, float, char, double, boolean, long.";
				return -1;    
			}
			return 0;
		}
		
		void set_basefilename(std::string filename)
		{
			basefilename = filename;
			logstream(LOG_INFO) << "Basefilename:" << basefilename << std::endl;
		}
		
		void set_nshards(shard_t num)
		{
			nshards = num;
			logstream(LOG_INFO) << "nshards:" << (int)nshards << std::endl;
		}
		
		void set_intervals(char * data , size_t buff_size)
		{
			intervals.clear();
			int nread = 0;
			vid_t subst = 0;
			vid_t to;
			while(nread < buff_size)
			{
				to = *(vid_t*)(data + nread);
				intervals.push_back(std::pair<vid_t,vid_t>(subst , to) );
				subst = to+1;
				nread += sizeof(vid_t);
			}
			interval_done=1;
			assert(nread == buff_size);
			assert(intervals.size() == nshards);
			for(int iter = 0 ; iter != intervals.size() ; ++iter)
			{
				logstream(LOG_INFO) << "Interval " << iter << ":" << intervals[iter].first << " " << intervals[iter].second << std::endl;
			}
		}
		
		void set_duties(char * data , size_t buff_size)
		{
			duties.clear();
			int nread = 0;
			shard_t duty;
			long edges;
			while(nread < buff_size)
			{
				duty = *(shard_t*)(data+nread);
				edges = *(long*)(data+sizeof(shard_t)+nread);
				duties.push_back(std::pair<shard_t , long>(duty , edges));
				nread += sizeof(shard_t)+sizeof(long);
			}
			assert(nread == buff_size);
			double memreq = 0.0;
			while(interval_done == 0) usleep(100);
			for(int iter = 0 ; iter != duties.size() ; ++iter)
			{
				logstream(LOG_INFO) << "Duty " << (int)duties[iter].first << ":" << duties[iter].second << std::endl;
				memreq += ((double)(edge_size) * (double)(duties[iter].second));
			}
			
			int membudget = get_option_int("membudget_mb" , 1024);
			
			if(membudget * 1024.0 * 1024 * 0.8 > memreq)
			{
				shovel_in_mem = true;
			}
			
			logstream(LOG_DEBUG) << "Require " << memreq/1024/1024.0 << " MB memory" << std::endl;
			logstream(LOG_DEBUG) << "Membudget is " << membudget << "MB" << std::endl;
			if(shovel_in_mem)
			{
				logstream(LOG_DEBUG) << "Save shovels in memory" << std::endl;
				shovel_mem = (char**) calloc( duties.size() , sizeof(char*) );
				
				for(int iter = 0 ; iter != duties.size() ; ++iter)
				{
					shovel_mem[iter] = (char*) calloc(duties[iter].second , edge_size);
					assert( shovel_mem[iter] != NULL );
				}
			}
			else
			{
				logstream(LOG_DEBUG) << "Save shovels in files" << std::endl;
				
				fds = (int *) calloc(duties.size() , sizeof(int));
				
				for(int iter = 0 ; iter != duties.size() ; ++iter)
				{
					std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
					filename += ".re";
					CheckDirs(filename);
					fds[iter] = open(filename.c_str() , O_WRONLY | O_CREAT, S_IROTH | S_IWOTH | S_IWUSR | S_IRUSR);
				}
			}
			packages = (int*) calloc(duties.size() , sizeof(int));
			this->edges = (int*) calloc(duties.size() , sizeof(int));
			memset(packages , 0 , duties.size() * sizeof(int));
			memset(this->edges , 0 , duties.size() * sizeof(int));
		}
		
		int write_shovel_data(char * data , size_t datasize , int shardindex , long offset)
		{
			int index=0;
			while(duties[index].first != shardindex) ++index;
			if(shovel_in_mem)
			{
				while(offset + datasize > duties[index].second * edge_size)
				{
					logstream(LOG_DEBUG) << "Overflow , realloc for shrad " << shardindex << std::endl;
					duties[index] = std::pair<shard_t , long>(duties[index].first , duties[index].second * 1.2);
					shovel_mem[index] = (char*) realloc(shovel_mem[index] , duties[index].second * edge_size);
					logstream(LOG_DEBUG) << "Shovel buffer size:" << duties[index].second * edge_size << std::endl;
					assert(shovel_mem[index] != NULL);
				}
				memcpy(shovel_mem[index] + offset , data , datasize);
			}
			else
			{
				assert(fds[index] >= 0);
				pwritea<char>(fds[index] , data , datasize , offset);
			}
			pthread_mutex_lock(&edge_mutex);
			if(edges[index] < (offset + datasize)/edge_size)
			{
				edges[index] = (offset + datasize)/edge_size;
			}
			pthread_mutex_unlock(&edge_mutex);
			__sync_add_and_fetch(&packages[index] , 1);
			return 0;
		}
		
		int finish_receive(char * data , size_t datasize)
		{
			for(int iter = 0 ; iter != duties.size() ; ++iter)
			{
				int package = *(int*)(data + iter * sizeof(int));
				while( package > packages[iter] )
				{	
					logstream(LOG_DEBUG) << "Shard:" << (int)duties[iter].first <<" Excepted to receive " << package << " packages , Now received " << packages[iter] << std::endl;
					usleep(100000);
				}
				logstream(LOG_DEBUG) << "Shard:" << (int)duties[iter].first << " Excepted to receive " << package << " packages , Now received " << packages[iter] << std::endl;
			}
			
			sub_done = 1;
		}
		
		int submode()
		{
			logstream(LOG_DEBUG) << "Run as submode , waiting for data..." << std::endl;
			logstream(LOG_DEBUG) << "EdgeType:" << edge_type << std::endl;
			sub_done = 0;
			dt.SetRecvHandle(sharder_handle);
			dt.SetAuxData(this);
			dt.WaitForDatas();
			while(sub_done != 1)
			{
				sleep(5);
			}
			for(int iter = 0 ; iter != duties.size() ; ++iter)
			{
				if(!shovel_in_mem)
				{
					close(fds[iter]);
				}
				if (edge_type == "float") {
					sharder<float , float> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<float>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<float>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "float-float") {
					sharder< PairContainer<float> , PairContainer<float> > sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<PairContainer<float> >*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<PairContainer<float> >*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "int") {
					sharder<int , int> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<int>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<int>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "uint") {
					sharder<unsigned int , unsigned int> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<unsigned int>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<unsigned int>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "int-int") {
					sharder<PairContainer<int> , PairContainer<int> > sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<PairContainer<int> >*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<PairContainer<int> >*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "short") {
					sharder<short , short> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<short>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<short>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "double") {
					sharder<double , double> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<double>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<double>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "char" || edge_type == "none") {
					sharder<char , char > sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<char>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<char>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "boolean") {
					sharder<bool , bool> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<bool>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<bool>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "long") {
					sharder<long , long> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<long>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<long>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "length16") {
					sharder<length16 , length16> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<length16>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<length16>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "length24") {
					sharder<length24 , length24> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<length24>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<length24>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "length32") {
					sharder<length32 , length32> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<length32>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<length32>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else if (edge_type == "length64") {
					sharder<length64 , length64> sharderobj(basefilename);
					sharderobj.sub_startup(basefilename , nshards , intervals);
					if(shovel_in_mem)
					{
						logstream(LOG_INFO) << "Reshard for the shard " << (int)duties[iter].first << std::endl;
						sharderobj.reshard(duties[iter].first , (edge_with_value<length64>*)shovel_mem[iter] , edges[iter]);
					}
					else
					{
						std::string filename = filename_shard_adj(basefilename , duties[iter].first , nshards);
						filename += ".re";
						logstream(LOG_INFO) << "Reshard for " << filename << std::endl;
						size_t fsize = get_filesize(filename);
						
						char * shovelbuf = (char*)malloc(fsize);
						
						int f = open(filename.c_str() , O_RDONLY);
						reada(f , shovelbuf , fsize);
						close(f);
						
						assert(fsize/edge_size == edges[iter]);
						
						sharderobj.reshard(duties[iter].first ,  (edge_with_value<length64>*)shovelbuf , edges[iter]);
						remove(filename.c_str());
					}
				} else {
					logstream(LOG_ERROR) << "You need to specify edgedatatype. Currently supported: int, short, float, char, double, boolean, long.";
					return -1;    
				}
			}
			std::string f_inter = filename_intervals(basefilename , nshards);
			
			FILE * fp = fopen(f_inter.c_str() , "w");
			for(int iter = 0 ; iter != nshards ; ++iter)
			{
				fprintf(fp , "%ld\n" , intervals[iter].second);
			}
			fclose(fp);
			
			std::string f_num = basefilename + ".numvertices";
			
			fp = fopen(f_num.c_str() , "w");
			fprintf(fp , "%d" , intervals[nshards-1].second + 1);
			fclose(fp);
			return 0;
		}
		/*
		**Subfunction ends
		*/
	};
	 
    template <typename VT, typename ET>
    class sharded_graph_output : public ioutput<VT, ET> {
        
        sharder<ET> * sharderobj;
        mutex lock;
        
    public:
        sharded_graph_output(std::string filename, DuplicateEdgeFilter<ET> * filter = NULL) {
            sharderobj = new sharder<ET>(filename);
            sharderobj->set_duplicate_filter(filter);
            sharderobj->start_preprocessing();
        }
        
        ~sharded_graph_output() {
            delete sharderobj;
            sharderobj = NULL;
        }
        
        
        
    public:
        void output_edge(vid_t from, vid_t to) {
            assert(false); // Need to use the custom method
        }
                
        
        virtual void output_edge(vid_t from, vid_t to, float value) {
            assert(false); // Need to use the custom method
        }
        
        virtual void output_edge(vid_t from, vid_t to, double value) {
            assert(false); // Need to use the custom method
        }
        
        
        virtual void output_edge(vid_t from, vid_t to, int value)  {
            assert(false); // Need to use the custom method
        }
        
        virtual void output_edge(vid_t from, vid_t to, size_t value)  {
            assert(false); // Need to use the custom method
        }
        
        void output_edgeval(vid_t from, vid_t to, ET value) {
            lock.lock();
            sharderobj->preprocessing_add_edge(from, to, value);
            lock.unlock();
        }
        
        void output_value(vid_t vid, VT value) {
            assert(false);  // Not used here
        }
        
        
        void close() {
        }
        
        size_t finish_sharding() {
            sharderobj->end_preprocessing();

            sharderobj->execute_sharding("auto");
            return sharderobj->nshards;
        }
        
    };
    
	void sharder_handle(char * data , size_t datasize , std::string command , void * auxdata)
	{
		shard_sub_mode * ssm = (shard_sub_mode *)auxdata;
		if(command == "PreInfo")
		{
			std::string bfn(data);
			ssm->set_basefilename(bfn);
			ssm->set_nshards(*(shard_t*)(data+bfn.length()+1));
			ssm->set_intervals(data+bfn.length()+1+sizeof(shard_t) , datasize-bfn.length()-1-sizeof(shard_t));
		}
		else if(command == "Duty")
		{
			ssm->set_duties(data,datasize);
		}
		else if(command.find("Shovel:") != std::string::npos)
		{
			int left = command.find_first_of(':');
			int mid = command.find_first_of(' ');
			int right = command.find_last_of(':');
			
			int shardindex;
			long offset;
			sscanf(command.substr(left+1 , mid-left).c_str() , "%d" , &shardindex);
			sscanf(command.substr(right+1).c_str() , "%ld" , &offset);
			
			logstream(LOG_DEBUG) << "Received: Shard " << shardindex << " Offset " << offset << std::endl; 
			
			ssm->write_shovel_data(data , datasize , shardindex,  offset);
		}
		else if(command == "Reshard")
		{
			ssm->finish_receive(data , datasize);
		}
	}
	
}; // namespace


#endif



