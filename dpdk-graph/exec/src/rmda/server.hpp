#include <stdio.h> 
#include <rpc/rpc.h> /* always needed */ 

#include "rmdacomm.h"
//extern pthread_mutex_t mt_mutex;
static volatile int recv_job_num, send_job_num;
char *data;
static char **inbuf;
static volatile int inlength[1024];

static void produce_data(data_req *req,data_arg *retval)
{
/*
	retval->data.data_len = req->len;
	retval->data.data_val= malloc(req->len);
	memset(retval->data.data_val, req->c, req->len);
*/
//        printf("req->len:%d\n",req->len);
}

int * block_write_1_svc(data_arg *data, struct svc_req *svc_req)
{	
	int *ret = (int *)malloc(sizeof(int));

	*ret = 0;
	//pthread_mutex_lock(&mt_mutex);
	
	std::cout<<"RDMA:..................."<<std::endl;
	printf("server: data len is %d       from: %d\n",data->index, data->data.data_len);
	if(inlength[data->index] != data->data.data_len){
		std::cout<<"inlength[data->index] != data->data.data_len: "<< inlength[data->index]<<" != "<< data->data.data_len<<std::endl;
		assert(inlength[data->index] == data->data.data_len);
	}
	memcpy(inbuf[data->index],data->data.data_val, data->data.data_len);
	//std::cout<<"data value is :" <<(char*)data->data.data_val<<std::endl;
	//free(data->data.data_val);
	
	//usleep(100);
	//pthread_mutex_unlock(&mt_mutex);
	__sync_fetch_and_sub(&recv_job_num,1);	
	return ret;
}

data_arg * block_read_1_svc(data_req *req, struct svc_req *svc_req)
{	
	data_arg *retval;
/*
	retval =(data_arg *)malloc(sizeof(data_arg));
	pthread_mutex_lock(&mt_mutex);
	produce_data(req,retval);
	//usleep(100);
	pthread_mutex_unlock(&mt_mutex);
*/	
	return retval;
}
