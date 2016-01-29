#include <stdio.h>

#if 0
#define PRINTF_ERR printf
#define PRINTF_INFO printf
#else
#define PRINTF_ERR printf
#define PRINTF_INFO 
#define PRINTF_DEBUG
#endif