#include <stdio.h> 
#include <rpc/rpc.h> /* always needed */ 
#include "rmdacomm.h"

int main(int argc, char **argv)
{
	CLIENT *c1;
	int i,*ret;
	char* s="GoldenGlobalView";
	char *server = malloc(20);

	data_arg *data_argp = malloc(sizeof(data_arg));
	data_argp->data.data_len = 10;
	data_argp->data.data_val = s;
	
	if (argc != 2) { 
		fprintf(stderr, "usage: %s host ip address\n", argv[0]); 
		exit(1); 
	} 

	server = argv[1]; 
	printf("server addr is %s\n",server);

	c1 = clnt_create(server, OSDPROG, OSDVERS, "tcp");
	if (c1 == (CLIENT *)NULL) {
		fprintf(stderr, "connect fail \n");
		exit(1);
	}

	ret = block_write_1(data_argp, c1);
	if(ret==NULL){
		printf("res ==NULL\n");
	}
	
	
}
