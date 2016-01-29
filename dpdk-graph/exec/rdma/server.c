#include <stdio.h> 
#include <rpc/rpc.h> /* always needed */ 

#include "rmdacomm.h"
//extern pthread_mutex_t mt_mutex;

char *data;

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
	
	printf("data len is %d\n",data->data.data_len);
	printf("data value is %.10s\n",data->data.data_val);
	//free(data->data.data_val);
	
	//usleep(100);
	//pthread_mutex_unlock(&mt_mutex);
	
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
