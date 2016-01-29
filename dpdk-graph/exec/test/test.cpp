#include <iostream>    
#include <stdio.h>
#include <omp.h> // OpenMP编程需要包含的头文件    
#include <iostream>   
#include <omp.h> // OpenMP编程需要包含的头文件   
   
#include <iostream>   
#include <omp.h> // OpenMP编程需要包含的头文件   
   
int main()   
{   
#pragma omp parallel sections //声明该并行区域分为若干个section,section之间的运行顺序为并行的关系   
    {   
#pragma omp section //第一个section,由某个线程单独完成   
#pragma omp parallel for schedule(dynamic)
        for (int i = 0; i < 5; ++i)    
        {   
            std::cout << i << "+" << std::endl;   
	std::cout << omp_get_thread_num() << "+" << std::endl;   
        std::cout << omp_get_thread_num() << "-" << std::endl;
        std::cout << omp_get_num_threads() << "-" << std::endl;
        std::cout << omp_get_num_threads() << "-" << std::endl;
        }   
   
#pragma omp section //第一个section,由某个线程单独完成   
        for (int j = 0; j < 5; ++j)    
        {   
         std::cout << j << "-" << std::endl;   
        }   
    }   
   
    return 0;   
}   
