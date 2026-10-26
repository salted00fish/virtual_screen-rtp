#ifndef AVCLOCK_H
#define AVCLOCK_H

#include <QWidget>

namespace avclock
{
    void av_clock_init(void);
    qint64 qpc_to_us(qint64 qpc);
    qint64 now_us(void);
    qint64 now_ms(void);
}


#endif // AVCLOCK_H
