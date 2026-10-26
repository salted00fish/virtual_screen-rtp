#include "touch.h"

#include <QGuiApplication>
#include <QScreen>
#include <QRect>
#include <QList>
#include <QDebug>
#include <QThread>

#define CLICK_TYPE               0x01
#define LONG_CLICK_TYPE          0x02
#define DOUBLE_CLICK_TYPE        0x03
#define MOVE_TYPE                0x04
#define MOVEING_TYPE             0x05
#define SLIDE_END_TYPE           0x06

touch::touch(QWidget *parent) : QWidget(parent)
{

}

void touch::touch_slit(QByteArray data)
{
    if (data.size() < int(sizeof(touch_data_t)))
        return;

    const int kWheelDelta = 120;
    const int kScrollStepPx = 30;   /* ★ 手指移动 30 单位 = 滚一格（可调） */

    touch_data_t touch_data;
    memcpy(&touch_data, data.constData(), sizeof(touch_data));

#ifdef Q_OS_WIN
    if (touch_data.x > 799) touch_data.x = 799;
    if (touch_data.y > 479) touch_data.y = 479;

    const QRect scr = touch_screenRect.isEmpty() ? secondScreenRect() : touch_screenRect;
    if (scr.isEmpty())
    {
        qWarning() << "touch_slit: 没有可用的屏幕矩形";
        return;
    }

    const int px = scr.left() + int(qint64(touch_data.x) * scr.width()  / 800);
    const int py = scr.top()  + int(qint64(touch_data.y) * scr.height() / 480);



    switch (touch_data.type)
    {

    case CLICK_TYPE:
    {
        SetCursorPos(px, py);
        sendMouseClick(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
    }break;

    case LONG_CLICK_TYPE:
    {
        SetCursorPos(px, py);
        sendMouseClick(MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_RIGHTUP);
    }break;

    case DOUBLE_CLICK_TYPE:
    {
        SetCursorPos(px, py);
        sendMouseClick(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
        QThread::msleep(40);
        sendMouseClick(MOUSEEVENTF_LEFTDOWN, MOUSEEVENTF_LEFTUP);
    }break;

    case MOVE_TYPE:
    {
        m_sliding = true;
        m_lastX   = int(touch_data.x);
        m_lastY   = int(touch_data.y);
        m_accumX  = 0;
        m_accumY  = 0;
        m_lockDir = 0;                  /* 方向未定，等第一段位移判定 */
        SetCursorPos(px, py);
        break;
    }

    case MOVEING_TYPE:
   {
       if (!m_sliding) break;

       const int x = int(touch_data.x);
       const int y = int(touch_data.y);

       m_accumX += (m_lastX - x);
       m_accumY += (y - m_lastY);
       m_lastX   = x;
       m_lastY   = y;

       /* 首次超过 12 单位就锁死方向，避免斜滑时纵横乱跳 */
       if (m_lockDir == 0) {
           if      (qAbs(m_accumX) > 12) m_lockDir = 1;   /* 横滑 */
           else if (qAbs(m_accumY) > 12) m_lockDir = 2;   /* 竖滑 */
           else break;                                     /* 还不到阈值 */
       }

       if (m_lockDir == 1)
       {
           int steps = m_accumX / kScrollStepPx;
           if (steps != 0) {
               const int n = qAbs(steps);
               for (int i = 0; i < n; ++i) {
                   sendHWheel(steps > 0 ? kWheelDelta : -kWheelDelta);
                   if (i + 1 < n) QThread::msleep(12);
               }
               m_accumX -= steps * kScrollStepPx;
           }
       }
       else
       {
           int steps = m_accumY / kScrollStepPx;
           if (steps != 0) {
               const int n = qAbs(steps);
               for (int i = 0; i < n; ++i) {
                   sendWheel(steps > 0 ? kWheelDelta : -kWheelDelta);
                   if (i + 1 < n) QThread::msleep(12);
               }
               m_accumY -= steps * kScrollStepPx;
           }
       }
       break;
   }

    case SLIDE_END_TYPE:
    {
        m_sliding = false;
        m_accumX  = 0;
        m_accumY  = 0;
        m_lockDir = 0;
        break;
    }

    default:break;
    }

    qDebug()<<"type"<<touch_data.type;
    qDebug() << "touch click 下位机(" << touch_data.x << "," << touch_data.y << ") -> 屏幕(" << px << "," << py << ")";
#else
    Q_UNUSED(x);
    Q_UNUSED(y);
#endif
}

void touch::setScreenRect(const QRect &rect)
{
    touch_screenRect = rect;
    qDebug() << "touch: 坐标映射屏幕 ->" << touch_screenRect;
}

// ---------------------------------------------------------------------------
// 取“非主屏”的第一块屏幕的物理像素矩形（虚拟桌面坐标）；找不到返回空 QRect。
// ---------------------------------------------------------------------------
QRect touch::secondScreenRect()
{
    QRect rect;

#ifdef Q_OS_WIN
    struct Ctx
    {
        QRect rect;
        bool  found = false;
    } ctx;

    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR mon, HDC, LPRECT, LPARAM lp) -> BOOL
    {
        auto *c = reinterpret_cast<Ctx *>(lp);

        MONITORINFO mi = {};
        mi.cbSize = sizeof(MONITORINFO);
        if (!GetMonitorInfoW(mon, &mi))
            return TRUE;                       /* 继续找下一块 */

        if (mi.dwFlags & MONITORINFOF_PRIMARY)
            return TRUE;                       /* 主屏跳过 */

        c->rect  = QRect(mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right  - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top);
        c->found = true;
        return FALSE;                          /* 找到了，停止枚举 */
    }, reinterpret_cast<LPARAM>(&ctx));

    if (ctx.found)
        rect = ctx.rect;
#endif

    /* 兜底：枚举失败就用 Qt 的 screens()（逻辑坐标，普通 DPI 下与物理一致） */
    if (rect.isEmpty())
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (screens.size() >= 2)
            rect = screens.at(1)->geometry();
    }

    return rect;
}

void touch::sendMouseClick(DWORD downFlag, DWORD upFlag)
{
    INPUT in[2] = {};
    in[0].type       = INPUT_MOUSE;
    in[0].mi.dwFlags = downFlag;
    in[1].type       = INPUT_MOUSE;
    in[1].mi.dwFlags = upFlag;
    SendInput(2, in, sizeof(INPUT));
}

void touch::sendWheel(int delta)
{
    INPUT in = {};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = DWORD(delta);
    SendInput(1, &in, sizeof(INPUT));
}

void touch::sendHWheel(int delta)
{
    INPUT in = {};
    in.type         = INPUT_MOUSE;
    in.mi.dwFlags   = MOUSEEVENTF_HWHEEL;
    in.mi.mouseData = DWORD(delta);
    SendInput(1, &in, sizeof(INPUT));
}
