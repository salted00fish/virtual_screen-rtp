#include "avclock.h"
#include <QByteArray>
#include <cstdlib>
#include <windows.h>

namespace avclock
{

qint64 clock_freq;
qint64 clock_count;

static inline qint64 qpcRaw(void)
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<qint64>(c.QuadPart);
}

void av_clock_init()
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    clock_freq = static_cast<qint64>(f.QuadPart);
    if (clock_freq <= 0)
        clock_freq = 10000000LL;

    clock_count = qpcRaw();
}

qint64 qpc_to_us(qint64 qpc)
{
    if(clock_freq <= 0) return 0;

    return (qpc - clock_count) * 1000000ll / clock_freq;
}

qint64 now_us()
{
    return qpc_to_us(qpcRaw());
}

qint64 now_ms()
{
    return now_us() / 1000;
}

}

