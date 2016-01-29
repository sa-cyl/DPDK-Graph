/*
 * @file
 * @author  YongLi Cheng <sa_cyl@163.com>
 * @version 1.0
 *
 * Copyright [2014] [YongLI Cheng / HuaZhong University]
 */

#include <iostream>
#include <comm/machine.hpp>
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
#include <omp.h>
#include<queue>
#include "CE_Graph_basic_includes.hpp"
using namespace xGraph;
using namespace CE_Graph;

typedef float EdgeDataType;
struct topvalue{
    unsigned int vid;
    float value;
};
struct topvalue_int{
    unsigned int vid;
    int value;
};

int main(int argc, const char **argv) {
	int M, ret;
	task *T,t1;
	char *buf;

	metrics m("Application");

	std::vector<std::pair<int, std::string> > hosts;
	std::vector<task> Tc;
	std::cout<<"FL: Today,begin...,2014/12/20"<<std::endl;

	CE_Graph_init(argc, argv);
	/* determine the number of workers */
    	int machines = get_option_int("machine", 100); //get_option_int
	M = determine_machines("conf/hosts.slave", machines, &hosts);
	if(M < 1){
		std::cout<<"There is not a worker. "<<std::endl;
		assert(false);
	}
	for(unsigned int i=0;i<=hosts.size()-1;i++)
            std::cout<<"worker"<<i<<": "<<hosts[i].second.c_str()<<std::endl;

    	/* Process input file - if not already preprocessed */
    	std::string filename = get_option_string("file"); // Base filename  src/util/cmdopts.hpp
    	int niters = get_option_int("niters", 0); //get_option_int
    	int crucial_option = get_option_int("crucial_option", -1);
        if(niters == 0){
            std::cout<<"Please input niters. For example: niters 10." <<std::endl;
            assert(false);
        }

        int ntop = get_option_int("top", 20);
        int asyn = get_option_int("asyn", 0); // 0: synchronization, 1: asynchronization
        int rdma = get_option_int("rdma", 0); // 0: tcp/ip, 1: rdma 
	int reorder = get_option_int("reorder", 0);

    	int nshards = get_option_int("nshards", -1);
        if(nshards == -1){
            std::cout<<"Please input nshards. For example: nshards 20." <<std::endl;
            assert(nshards != -1);
        }

	if(nshards > M){
		std::cout<<"The number of worker machines("<<M<<") must be greater than "<<nshards<<","<<std::endl;
		assert(nshards <= M);
	}

	/* Assign tasks for workers*/
	T = (task*)malloc(sizeof(task) * nshards * niters);
	int k = 0;
	for(int i = 0; i < niters; i++) {
		for(int j = 0; j < nshards; j++) {
			if((k % M) == 0) k = 0;
			t1.machine = k;
			t1.iter = i;
			t1.interval = nshards * i + j;
			T[i * nshards + j] = t1;
			k ++;
		}
	}

	M = nshards;
	buf=NULL;
	buf = (char*)malloc(1024*1024);
	for(int i = 0; i < M; i++) {
		Tc.clear();
		for(int j = 0; j < niters * nshards; j ++) {
			if(T[j].machine == i) Tc.push_back(T[j]);
		}
		int len = 6 + 7*sizeof(int) + sizeof(task)*Tc.size()+15*M;
		//if(buf != NULL) free(buf);
		//buf = (char*)malloc(len);
		memcpy(buf,"XGREQS",6); //package 'S'=schedule
		

		int c;
		c = Tc.size();
		crucial_option = i;
		memcpy(buf + 6,(void*)&c,sizeof(int)); //package 'S'=schedule
		memcpy(buf + 6 + sizeof(int),(void*)&M,sizeof(int)); 
		memcpy(buf + 6 + 2*sizeof(int),(void*)&niters,sizeof(int));
		memcpy(buf + 6 + 3*sizeof(int),(void*)&nshards,sizeof(int));
		memcpy(buf + 6 + 4*sizeof(int),(void*)&crucial_option,sizeof(int));
		memcpy(buf + 6 + 5*sizeof(int),(void*)&ntop,sizeof(int));
		memcpy(buf + 6 + 6*sizeof(int),(void*)&asyn,sizeof(int));
		memcpy(buf + 6 + 7*sizeof(int),(void*)&rdma,sizeof(int));
		
		char ip[15];
		for(int k = 0; k < M; k++) {
			memset(ip, 0, 15);
			memcpy(ip, hosts[k].second.c_str(), 15);	
			memcpy((void*)(buf+6+8*sizeof(int)+k*15),ip,15);	
		}
		for(unsigned int j = 0; j < Tc.size(); j++) memcpy((void*)(buf+6+8*sizeof(int)+15*M+j*sizeof(task)),(void*)&(Tc[j]),sizeof(task));
		for(unsigned int j = 0; j < Tc.size(); j++) std::cout<<((task*)(buf+6+8*sizeof(int)+15*M+j*sizeof(task)))->machine<<" " \
                                                        <<((task*)(buf+6+8*sizeof(int)+15*M+j*sizeof(task)))->iter<<" " \
							<<((task*)(buf+6+8*sizeof(int)+15*M+j*sizeof(task)))->interval<<" " \
							<<(((task*)(buf+6+8*sizeof(int)+15*M+j*sizeof(task)))->interval)%nshards<<std::endl;
		int wret=write(hosts[i].first ,buf, len);
		if(wret != len){
			std::cout<<"Send Schedule Message to "<<hosts[i].second<< "Fail!"<<std::endl;
		}
		int recvbytes;
                if ((recvbytes = recv(hosts[i].first, buf, 1024,0)) == -1) {
                        std::cout<<"recv error!"<<std::endl;
                }
		assert(recvbytes != -1);
		char Phead[7];
		memset(Phead, '\0', 7);
		memcpy(Phead, buf, 6);
        	if(strcmp(Phead,"XGACKS")!=0){
                	std::cout<<"Recv Package S Fail:"<<hosts[i].second<<std::endl;
			std::cout<<Phead<<"=============="<<"XGACKS==========="<<Phead<<std::endl;
			assert(false);
        	}
		std::cout<<*((int*)(buf+6))<<Phead<<std::endl;
	}

	/* Receive init finished messages from worker nodes. */
   	volatile int cc = 0;
	char re[1024][1024];
	omp_set_num_threads(M);
       	#pragma omp parallel for
       	for(int y = 0; y < M; y++){
        	ret = read(hosts[y].first, (char*)re[y], 1024);
             	if(ret > 0){
			std::cout<<"Worker node" << y << " have finished Init." <<(char*)re[y]<<std::endl;
             		__sync_add_and_fetch(&cc, 1);
		}
        } 
        while(cc < M) {};
	/*
	 * Section: Begin to Schedule.
     	 */
	m.start_time("Iters");
	int j=0;
	for(int i = 0; i < niters; i ++){
  		/* Note worker nodes to begin a new iteration. */	
        	metrics_entry me1 = m.start_time();
        	std::stringstream ss1;
        	ss1<<"Iteration"<<j<<" runtime";
		j++;

		std::cout<<"niters:  "<<i<<"/"<<niters<<std::endl;
		for(int i = 0; i < nshards; i++){
		while((ret = send(hosts[i].first ,(char*)"tbegin", 6, 0)) < 6){
				std::cout<< " Sending signal to worker node " << i << "." <<std::endl;
				if(ret < 0){
					std::cout<<"Send TOBEGIN fail, exit..."<<std::endl;
					exit(0);
				}
			}

		}	


		/* Receive finished messages from worker nodes. */
   		volatile int cc = 0;
		omp_set_num_threads(M);
        	#pragma omp parallel for
        	for(int y = 0; y < M; y++){
             		ret = read(hosts[y].first, (char*)re[y], 1024);
             		if(ret > 0){
				std::cout<<"Worker node" << y << " have finished." <<(char*)re[y]<<std::endl;
             			__sync_add_and_fetch(&cc, 1);
			}
        	} 
        	while(cc < M) {};


		
		/* Check whether job have finished. */
		m.stop_time(me1,ss1.str(),false);
	}

	/* Merge Results */
    std::queue<topvalue> q[50];
    std::queue<topvalue_int> q1[50];
    int vt;
	volatile int done = 0;
	    omp_set_num_threads(M);
        #pragma omp parallel for
		for(int i = 0; i < M; i ++) {
            char *topbuf;
            topbuf = (char*)malloc(1024*1024*10+1);
			recv(hosts[i].first, topbuf,1024*1024*10, 0);
            topvalue tv;
            topvalue_int tv_int;
            memcpy(&vt,topbuf,sizeof(int));
            for(int j = 0; j < ntop; j++){
                if(vt==0){
                    memcpy(&tv.vid, topbuf+(sizeof(int)+sizeof(float))*j+sizeof(int), sizeof(int));
                    memcpy(&tv.value, topbuf+(sizeof(int)+sizeof(float))*j+2*sizeof(int), sizeof(float));
                    q[i].push(tv);
                }else if(vt==1){
                    memcpy(&tv_int.vid, topbuf+(sizeof(int)+sizeof(int))*j+sizeof(int), sizeof(int));
                    memcpy(&tv_int.value, topbuf+(sizeof(int)+sizeof(int))*j+2*sizeof(int), sizeof(int));
                    //std::cout<<"vid: "<<tv_int.vid<<"  value:  "<<tv_int.value<<std::endl;
                    q1[i].push(tv_int);
                }
            }
            __sync_add_and_fetch(&done, 1); 
		}
	while(done < M){}	
    /*
    for(int v=0;v<M;v++){
        std::cout<<"============================== M:"<<v<<std::endl;
        for(int w=0;w<ntop;w++){
            topvalue_int tt;
            tt=q1[v].front();
            std::cout<<" vid: " <<tt.vid<<" value: "<<tt.value<<std::endl;
            q1[v].pop();
        } 
    }
    */
    int pos,f;
    unsigned int oldvid;
    float value;
    int   val;
    std::stringstream fname;

    std::cout << "Print top " << ntop << " vertices: "<< std::endl;
if(reorder == 1){
    fname << filename <<".map";
    f = open(((std::string)fname.str()).c_str(), O_RDONLY);
}
    if(f < 0 && reorder == 1){
        std::cout<<"Can not open the file : "<<fname.str()<<"   toplist fail! "<< strerror(errno)<<std::endl; 
    }else{
        if(vt==0){
            for(int t=0; t<ntop; t++){
                pos=0;
                value = (float)0.0;
                for(int p=0; p<M; p++){
                    if(q[p].front().value>value){
                        value = q[p].front().value;
                        pos = p;
                    } 
                }
if(reorder == 1){
                if(pread(f, &oldvid, sizeof(int),q[pos].front().vid*sizeof(int))<0){
                    std::cout<<"read oldvid fail!  " << strerror(errno)<<std::endl;;
                    break;
                }
                std::cout<<t+1<<".  "<<oldvid<<"  "<<q[pos].front().value<<std::endl;
}
                std::cout<<t+1<<".    "<<q[pos].front().vid<<"  "<<q[pos].front().value<<std::endl;
                q[pos].pop();
            }
        }else if(vt==1){
            for(int t=0; t<ntop; t++){
                pos=0;
                val = 0;
                for(int p=0; p<M; p++){
                    if(q1[p].front().value>val){
                        val = q1[p].front().value;
                        pos = p;
                    } 
                }
if(reorder == 1){
                if(pread(f, &oldvid, sizeof(int),q1[pos].front().vid*sizeof(int))<0){
                    std::cout<<"read oldvid fail!  " << strerror(errno)<<std::endl;;
                    break;
                }
                std::cout<<t+1<<".  "<<oldvid<<"  "<<q1[pos].front().value<<std::endl;
}
                std::cout<<t+1<<".   "<<q1[pos].front().value<<std::endl;
                q1[pos].pop();
            }
        }
        close(f);
    }
	std::cout<<"Finished"<<std::endl;
	m.stop_time("Iters");

	/* inform workers to finish. */
       for(int i = 0; i < M; i ++) {
            send(hosts[i].first, (char*)"XGREQF", 6, 0);
        }

	/* close clients connect.*/
        for(unsigned int i=0;i<=hosts.size()-1;i++)
            close(hosts[i].first);

	/* Report Result */
	metrics_report(m); 
	return(0);
}
