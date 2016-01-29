#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <iostream>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string>
#include <time.h>

ssize_t reada(int f, char * tbuf, size_t nbytes) {
    size_t nread = 0;
    char * buf = (char*)tbuf;
    while(nread<nbytes) {
        ssize_t a = read(f, buf, nbytes - nread);
        if (a == (-1)) {
            std::cout << "Error, could not read: " << strerror(errno) << "; file-desc: " << f << std::endl;
            std::cout << "Pread arguments: " << f << " tbuf: " << tbuf << " nbytes: " << nbytes << std::endl;
            assert(a != (-1));
        }
        assert(a>=0);
        buf += a;
        nread += a;
		if( a == 0 )
		{
			break;
		}
    }
    assert(nread <= nbytes);
	return nread;
}

int main(int argc , char ** argv)
{
	if(argc < 3 || argc > 4)
	{
		printf("Usage:%s infilename outfilename [ad]\n" , argv[0]);
		return 0;
	}
	long ignore_edges=0;
	bool dynamic = (argc == 4);
	bool append=false;
	if(dynamic)
	{
		if(argv[3][0] == 'a')
		{
			append=true;
		}
		srand(time(NULL));
	}
	if(append == false)
	{
		int fd = open(argv[1] , O_RDONLY);
		if(fd < 0)
		{
			printf("Couldn't open file %s\n",argv[1]);
			return -1;
		}
		int digit , values;
		char lineBuff[128];
		off_t sz = lseek(fd , 0 , SEEK_END);
		lseek(fd , 0 , SEEK_SET);
		printf("binary mode , File size is %ld\n" , sz);
		if( sz % (sizeof(int64_t)*2) != 0 )
		{
			printf("File size is not correct!!!\n");
			return -2;
		}
		FILE * fp = fopen(argv[2] , "w");
		if(NULL == fp)
		{
			printf("Couldn't open file %s\n",argv[2]);
			return -3;
		}
		const int edges_per_read = 20000000;
		char * buff = (char*)malloc(sizeof(int64_t)*edges_per_read);
		long nread = 0;
		int64_t from , to;
		float tempvalue;
		while(nread < sz)
		{
			long temp = reada(fd , buff , sizeof(int64_t)*edges_per_read);
			nread+=temp;
			int64_t * bufptr = (int64_t*)buff;
			printf("%ld/%ld\n" , nread , sz);
			for(int iter = 0 ; iter != temp ; iter+=2*sizeof(int64_t))
			{
				from = *bufptr++;
				to = *bufptr++;
				if(from < 0 || to < 0)
				{
					printf("Error:from:%ld to:%ld,ignore\n" , from , to);
					ignore_edges++;
					continue;
				}
				if(from == to)
				{
					ignore_edges++;
				}
				if(from != to)
				{
					if(!dynamic)
					{
						fprintf(fp , "%ld %ld\n",from , to);
					}
					else
					{
						digit = rand()%1000;
						if(digit < 40)
						{
							values = 5;
						}
						else if(digit < 103)
						{
							values = 4;
						}
						else if(digit < 214)
						{
							values = 3;
						}
						else if(digit < 464)
						{
							values = 2;
						}
						else
						{
							values = 1;
						}
						int ptr=0;
						for(int index = 0 ; index != values ; ++index)
						{
							tempvalue = rand()%1000 + (rand()%1000)/1000.0;
							ptr += sprintf(lineBuff+ptr , "%f:" , tempvalue);
						}
						lineBuff[ptr-1] = '\0';
						fprintf(fp , "%ld %ld %s\n" , from , to , lineBuff);
					}
				}
			}
		}
		assert(nread == sz);
		free(buff);
		close(fd);
		fclose(fp);
	}
	else
	{
		FILE * rfp = fopen(argv[1] , "r");
		FILE * wfp = fopen(argv[2] , "w");
		if(rfp == NULL)
		{
			fprintf(stderr , "Couldn't open file %s\n" , argv[1]);
			return -1;
		}
		if(wfp == NULL)
		{
			fprintf(stderr , "Couldn't open file %s\n" , argv[2]);
			return -1;
		}
		char buff[2048];
		char lineBuff[128];
		float tempvalue;
		int digit , values , len;
		while(fgets((char*)buff , 2048 , rfp))
		{
			len = strlen((char*)buff);
			if((len != 0) && (buff[len-1] == '\n'))
			{
				if((len != 1) && (buff[len-2] == '\r'))
				{
					buff[len-2] = '\0';
				}
				buff[len-1] = '\0';
			}
			if(buff[0] == '\0')
			{
				break;
			}
			if(buff[0] == '#')
			{
				fprintf(wfp , "%s\n" , buff);
				continue;
			}
			digit = rand()%1000;
			if(digit < 40)
			{
				values = 5;
			}
			else if(digit < 103)
			{
				values = 4;
			}
			else if(digit < 214)
			{
				values = 3;
			}
			else if(digit < 464)
			{
				values = 2;
			}
			else
			{
				values = 1;
			}
			int ptr=0;
			for(int index = 0 ; index != values ; ++index)
			{
				tempvalue = rand()%1000 + (rand()%1000)/1000.0;
				ptr += sprintf(lineBuff+ptr , "%f:" , tempvalue);
			}
			lineBuff[ptr-1] = '\0';
			fprintf(wfp , "%s %s\n" , buff , lineBuff);
		}
		fclose(rfp);
		fclose(wfp);
	}
	printf("Ignore edges:%ld\n" , ignore_edges);
	return 0;
}
