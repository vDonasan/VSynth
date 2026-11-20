/* Pequenas diferenças entre sistemas operacionais. */
#ifndef PLATFORM_H
#define PLATFORM_H

#ifdef _WIN32
#include <windows.h>
static inline void sleep_ms(int ms) { Sleep((DWORD)ms); }
#else
#include <time.h>
static inline void sleep_ms(int ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}
#endif

#endif
