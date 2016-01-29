#include <stdio.h> 
#include "stub.h" 
#include <time.h>

#define CLNT_THREAD_NUMS 8
#define BLOCK_LEN ((0x01<<22)*1)
#define FILE_LEN BLOCK_LEN*(0x01<<8)
#define LOOP_NUM 1000

char *data;

pthread_mutex_t mt_mutex;

struct arg_tirpc{
	u_int len;
	char *data;
	CLIENT *clnt;
};


static void fill_buffer(char ch)
{
	data = malloc(BLOCK_LEN);
	memset(data, ch, BLOCK_LEN);
}

static void fill_char(char ch)
{
	data = malloc(sizeof(char));
	memset(data, ch, sizeof(char));
}

static void multiple_thread(int n, void* pfun, struct arg_tirpc *parg)
{
	pthread_t *t; //[CLIENT_THREAD_NUMS] ; 
	int i;
	
	t = (pthread_t *)malloc( n * sizeof(pthread_t));
	if(NULL == t){
		printf("Malloc pthread_t ERROR! return!\n");
		return;
	}
	for(i=0;i<n;i++){
		pthread_create(&t[i],NULL,pfun,&parg[i]);	
	}
	printf("Thread number is %d\n",i);
	
	for(i=0;i<n;i++){		 
		pthread_join(t[i],NULL); 
	}
}

static void issue_request(char *server, void *pfun, char * funname){

	struct arg_tirpc arg[CLNT_THREAD_NUMS];
	int i;
	struct timeval tim1, tim2;
	double used_time = 0.0;
	CLIENT *clnt;
	
	for(i = 0;i < CLNT_THREAD_NUMS; i ++){
		arg[i].len= BLOCK_LEN;
		arg[i].data = data;	
		
		clnt = clnt_create(server, OSDPROG, OSDVERS, "tcp"); 
		if (clnt == (CLIENT *)NULL) { 
			clnt_pcreateerror(server); 
			exit(1); 
		}
		arg[i].clnt = clnt;
	}
	gettimeofday(&tim1,NULL);
	multiple_thread(CLNT_THREAD_NUMS, pfun, arg);
	gettimeofday(&tim2,NULL);
	
	for(i = 0;i < CLNT_THREAD_NUMS; i ++){
		clnt_destroy(arg[i].clnt);
	}
	used_time += (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec); 

	time_t timep;
	time(&timep);

	printf("\n\n%s performace test : %s", funname, ctime(&timep));
	printf("\n======================================\n");
	printf("*********%s data %ld MB(%ld B)\n", funname, 
							((long)FILE_LEN>>20)*CLNT_THREAD_NUMS, 
							(long)FILE_LEN*CLNT_THREAD_NUMS);  
	printf("*********Thread number is %d\n~~~~~~~~~~The average throughput is %f KBps(%f MBps)\n",
						CLNT_THREAD_NUMS, 
						((long)FILE_LEN*CLNT_THREAD_NUMS*1000000.0)/(used_time*1024), 
						((long)FILE_LEN*CLNT_THREAD_NUMS*1000000.0)/(used_time*1024*1024));
	printf("======================================\n\n");
	
}

static void issue_request_rdma(char *server, void *pfun, char * funname){

	struct arg_tirpc arg[CLNT_THREAD_NUMS];
	int i;
	struct timeval tim1, tim2;
	double used_time = 0.0;
	CLIENT *clnt;
	
	for(i = 0;i < CLNT_THREAD_NUMS; i ++){
		arg[i].len= BLOCK_LEN;
		arg[i].data = data;	
		
		clnt = clnt_create(server, OSDPROG, OSDVERS, "rdma"); 
		if (clnt == (CLIENT *)NULL) { 
			clnt_pcreateerror(server); 
			exit(1); 
		}
		arg[i].clnt = clnt;
	}
	gettimeofday(&tim1,NULL);
	multiple_thread(CLNT_THREAD_NUMS, pfun, arg);
	gettimeofday(&tim2,NULL);
	for(i = 0;i < CLNT_THREAD_NUMS; i ++){
		clnt_destroy(arg[i].clnt);
	}
	used_time += (tim2.tv_sec-tim1.tv_sec)*1000000.0 + (tim2.tv_usec-tim1.tv_usec); 

	time_t timep;
	time(&timep);

	printf("\n\n%s performace test : %s", funname, ctime(&timep));
	printf("\n======================================\n");
	printf("*********%s data %d MB(%d B)\n", funname, 
							((long)FILE_LEN>>20)*CLNT_THREAD_NUMS, 
							(long)FILE_LEN*CLNT_THREAD_NUMS);  
	printf("*********Thread number is %d\n~~~~~~~~~~The average throughput is %f KBps(%f MBps)\n",
						CLNT_THREAD_NUMS, 
						((long)FILE_LEN*CLNT_THREAD_NUMS*1000000.0)/(used_time*1024), 
						((long)FILE_LEN*CLNT_THREAD_NUMS*1000000.0)/(used_time*1024*1024));
	printf("======================================\n\n");
	
}


static int test_write(struct arg_tirpc *argp)
{
	int i,*ret;
	data_arg *data_argp = malloc(sizeof(data_arg));
	CLIENT *clnt;
	
	data_argp->data.data_len = argp->len;
	data_argp->data.data_val = argp->data;
	clnt = argp->clnt;

	long count = (long)FILE_LEN / BLOCK_LEN;

	for(i = 0; i < count; i++){
		ret = date_write_1(data_argp, clnt);
		if(ret==NULL){
			printf("res ==NULL\n");
		}
	}

	free(data_argp);

	//return *ret;
	return 0;
}


static int test_read(struct arg_tirpc *argp)
{
	int i,*ret;
	data_req *read_req = malloc(sizeof(data_req));
	data_arg *data_result = NULL;
	CLIENT *clnt;
	
	read_req->c = *(argp->data);
	read_req->len = argp->len;
	clnt = argp->clnt;

	long count = (long)FILE_LEN / BLOCK_LEN;

	for(i = 0; i < count; i++){
		data_result = date_read_1(read_req, clnt);
		if(data_result==NULL){
			printf("data_argp ==NULL\n");
		}else{
			printf("recv form server data len is %d\n",data_result->data.data_len);
			printf("recv form server data value is %.10s\n",data_result->data.data_val);
		}		
		free(data_result);
	}
	free(read_req);

	//return *ret;
	return 0;
}

int main(int argc, char **argv) 
{ 
	
	int ret = 0,choice;
	CLIENT *clnt; 
	char *server = malloc(20);//="192.168.2.101"; 
	
	if (argc != 2) { 
		fprintf(stderr, "usage: %s host ip address\n", argv[0]); 
		exit(1); 
	} 

	server = argv[1]; 
	
	printf("server addr is %s\n",server);
	pthread_mutex_init( &mt_mutex, NULL );
	//clnt = clnt_create(server, OSDPROG, OSDVERS, "tcp"); 
	//clnt = clnt_create(server, OSDPROG, OSDVERS, "rdma"); 
	//if (clnt == (CLIENT *)NULL) { 
	//	clnt_pcreateerror(server); 
	//	exit(1); 
	//} 
	//printf("create rdma transp success~\n");
	printf("1:write test\n2:read test\n");
	scanf("%d",&choice);
	if(choice==1){
		fill_buffer('A');
		issue_request_rdma(server, test_write, "test_write");
		free(data);
	}else if(choice == 2){
		fill_char('B');
		issue_request_rdma(server, test_read, "test_read");
	}
	
	//while(1){};
	return ret;
}


