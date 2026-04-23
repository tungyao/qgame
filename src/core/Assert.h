#pragma once
#include <cstdlib>
#include <cstdio>

#ifdef NDEBUG
    #define ASSERT(expr) ((void)0)
    #define ASSERT_MSG(expr, msg) ((void)0)
#else
    #define ASSERT(expr) \
        do { if (!(expr)) { \
            ::fprintf(stderr, "ASSERT failed: %s\n  at %s:%d\n", #expr, __FILE__, __LINE__); \
            ::abort(); \
        } } while(0)

    #define ASSERT_MSG(expr, msg) \
        do { if (!(expr)) { \
            ::fprintf(stderr, "ASSERT failed: %s\n  msg: %s\n  at %s:%d\n", #expr, msg, __FILE__, __LINE__); \
            ::abort(); \
        } } while(0)
#endif
