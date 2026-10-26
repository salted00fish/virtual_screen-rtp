#include "widget.h"

#include <QApplication>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

int main(int argc, char *argv[])
{
#ifdef Q_OS_WIN
    /* 必须声明 DPI 感知，否则 Windows 会对坐标做一次隐式缩放，
       副屏在 125%/150% 缩放下 SetCursorPos 的落点会偏。
       注意：本工程用的是 Qt 5.12.9 自带的 MinGW 7.3.0，它的 winuser.h 里
       只有 Vista 时代的 SetProcessDPIAware()，没有 Win10 的
       SetProcessDpiAwarenessContext()（shellscalingapi.h 也不存在），
       所以只能用前者。 */
    SetProcessDPIAware();
#endif

    QApplication a(argc, argv);
    Widget w;
    w.show();
    return a.exec();
}
