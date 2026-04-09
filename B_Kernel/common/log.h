//To Do: I need some changes here

#include <stdio.h>
#include <stdlib.h>

//Log each output
#define LOG_INFO(msg) \
    printf("\033[32m[INFO]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_INFO_FMT(fmt, ...) \
    printf("\033[32m[INFO]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//WARN which potentially could give misleading answer but doesn't break program
#define LOG_WARN(msg) \
    printf("\033[33m[WARN]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_WARN_FMT(fmt, ...) \
    printf("\033[33m[WARN]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//ERROR which determine deterministic error in some computation    
#define LOG_ERROR(msg) \
    printf("\033[31m[ERROR]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_ERROR_FMT(fmt, ...) \
    printf("\033[31m[ERROR]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//Absolute Invariant violation
#define PANIC(msg)\
    do{ \
        printf("\n\x1b[31m[PANIC]\x1b[0m[%s:%d] %s\n", __FILE__, __LINE__, msg); \
        exit(1); \
    } while(0)

#define KASSERT(cond, msg) \
    do{ \
        if(!(cond)){ \
            PANIC(msg); \
        } \
    } while(0)
