#include <unistd.h>

int main(int argc , char ** argv)
{
	int ret;
	if(argc == 1)
	{
		ret = execl("./bin/sharder_basic_normal",NULL); 
	}
	else
	{
		ret = execl("./bin/sharder_basic_dynamic",NULL);
	}
	return ret;
}
