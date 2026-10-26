#ifndef TOUC_H
#define TOUC_H

#include <QWidget>
#include <QRect>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

typedef struct
{
    uint16_t x;
    uint16_t y;
    uint32_t type;
}touch_data_t;

class touch : public QWidget
{
    Q_OBJECT
public:
    explicit touch(QWidget *parent = nullptr);
    void touch_slit(QByteArray data);
    void setScreenRect(const QRect &rect);

private:
    //滑动的变量
    bool m_sliding   = false;
    int  m_lastX     = 0;
    int  m_lastY     = 0;
    int  m_accumX    = 0;
    int  m_accumY    = 0;
    int  m_lockDir   = 0;

private:
    /* 第二块屏幕（非主屏）的物理像素矩形；找不到返回空 QRect */
    QRect secondScreenRect();
    void sendMouseClick(DWORD downFlag, DWORD upFlag);
    void sendWheel(int delta);
    void sendHWheel(int delta);
    QRect touch_screenRect;
signals:

};

#endif // TOUC_H
