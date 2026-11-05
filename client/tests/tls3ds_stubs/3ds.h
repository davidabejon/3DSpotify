#ifndef TEST_3DS_H
#define TEST_3DS_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int32_t Result;
typedef struct { double start; double elapsed; } TickCounter;
typedef struct test_thread *Thread;
#define U64_MAX UINT64_MAX
#define R_FAILED(rc) ((rc) < 0)
void osTickCounterStart(TickCounter *t);
void osTickCounterUpdate(TickCounter *t);
double osTickCounterRead(TickCounter *t);
Thread threadCreate(void (*entry)(void *), void *arg, size_t stack,
                    int priority, int core, bool detached);
Result threadJoin(Thread thread, u64 timeout);
void threadFree(Thread thread);
void svcSleepThread(int64_t nanoseconds);
#endif
