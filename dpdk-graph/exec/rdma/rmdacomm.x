struct data_arg
{
	/*uint64_t inlen;
	uint8_t *indata;*/
	opaque data<>;
};

struct data_req
{
	/*uint64_t inlen;
	uint8_t *indata;*/
	char c;
	unsigned int len;
};

typedef struct data_arg data_arg;
typedef struct data_req data_req;

program OSDPROG {			/* name of remote program (not used)			*/
    version OSDVERS {		/* declaration of version (see below)*/
		int BLOCK_WRITE(data_arg) = 1;
		data_arg BLOCK_READ(data_req) = 2;
    } = 1;					/* definition of the program version	*/
} = 0x30090941;				/* remote program number (must be  unique)*/
