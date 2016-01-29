#include <omp.h>  
#include <stdio.h>
int main(int argc, char * argv[])    
{  
    omp_set_nested(10);     // none zero value is OK!  
#pragma omp parallel num_threads(2)  
    {  
        printf("ID: %d, Max threads: %d, Num threads: %d \n",omp_get_thread_num(), omp_get_max_threads(), omp_get_num_threads());  
#pragma omp parallel num_threads(5)  
        printf("Nested, ID: %d, Max threads: %d, Num threads: %d \n",omp_get_thread_num(), omp_get_max_threads(), omp_get_num_threads());  
  }  
	return 0;    
}  
