#ifndef VIDEO_H
#define VIDEO_H

#include <QByteArray>
#include <QImage>
#include <QSize>
#include <QString>
#include <QStringList>

#include <QtGlobal>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <qt_windows.h>
#  include <d3d11.h>
#  include <dxgi1_2.h>
#  include <wrl/client.h>      // Microsoft::WRL::ComPtr
#endif

class QScreen;

/*==============================================================================
 *  ScreenCapturer —— GPU 捕获（DXGI 桌面复制 Desktop Duplication）
 *
 *  参考 D:\cmake_open\qt_screen\screen\screen_data.cpp 的采集方式，替换掉原来的
 *  GDI BitBlt：
 *
 *    GDI 版  ：每次 BitBlt 让 GDI 把桌面在 CPU 侧重画一遍，再 GetDIBits 读回来
 *    DXGI 版 ：D3D11 设备 + IDXGIOutput1::DuplicateOutput 拿到"这块输出的画面"，
 *              GPU 直接 CopyResource 到一张 CPU 可读的 staging 纹理，
 *              Map 出来就是 BGRA 像素（CPU 只做缩放 + 编码，不再走 GDI 全屏拷贝）
 *
 *  另外两个好处：
 *    1. 画面没动时 AcquireNextFrame 直接返回 DXGI_ERROR_WAIT_TIMEOUT，
 *       这一轮可以什么都不做（见 CaptureWorker 的静止心跳补发）；
 *    2. 锁屏 / 改分辨率 / 切独占全屏会让复制对象永久失效（DXGI_ERROR_ACCESS_LOST），
 *       这里会置错让上层重建链路。
 *
 *  【线程约定】
 *    open() / grab() / close() 必须在同一个线程里调用 —— 也就是 CaptureWorker
 *    所在的采集线程（见 capture_worker.h）。COM/D3D11 对象都跟着那个线程。
 *    静态的 screenDescriptions() / defaultScreenIndex() / resolveScreen() 只读屏幕
 *    元数据，可以在 GUI 线程调用；encodeJpeg() 是纯 CPU，任意线程都可以。
 *
 *  【鼠标指针】
 *    桌面复制拿到的画面同样【不含】鼠标指针，还是要在 CPU 侧自己画：
 *      1. GetCursorInfo 拿到 HCURSOR、全局坐标、是否显示；
 *      2. 把画面里光标要盖住的那块背景拷进一块 32bpp DIB（就是画面自己的像素）；
 *      3. DrawIconEx(..., DI_NORMAL) 把光标画在这块背景上 —— alpha 混合和单色掩码
 *         交给 GDI，比手工画箭头保真（I 型、缩放箭头等形状都能出来）；
 *      4. 把这块画好光标的背景贴回画面。
 *    放大倍数见 setCursorScale()，放大时光标热点（箭头尖端）位置不变。
 *============================================================================*/

class ScreenCapturer
{
public:
    ScreenCapturer();
    ~ScreenCapturer();

    // 输出分辨率固定 800x480（下位机 LCD 的尺寸，也是零拷贝解码的前提）
    static QSize targetSize() { return QSize(800, 480); }

    /* ---------------- 参数 ----------------
     * 由采集线程读取。跨线程改参数请走 CaptureWorker 的槽（它内部会做线程切换）。 */
    void setScreenIndex(int index) { m_screenIndex = index; }
    int  screenIndex() const { return m_screenIndex; }

    void setDrawCursor(bool on) { m_drawCursor = on; }
    bool drawCursor() const { return m_drawCursor; }

    void   setCursorScale(double s) { m_cursorScale = (s < 1.0) ? 1.0 : s; }
    double cursorScale() const { return m_cursorScale; }

    /* ---------------- 采集链路（同一个线程里成对调用） ---------------- */

    /**
     * @brief 建/重建采集链路：D3D11 设备 + 目标输出 + 桌面复制 + staging 纹理
     *
     * 会按"选中的 QScreen 对应的那块输出"到所有显卡适配器里找，找到后在那块
     * 适配器上建设备（多显卡/独显直连的机器上，输出不一定挂在默认适配器上）。
     *
     * @param[out] err 失败原因（可为 NULL）
     */
    bool open(QString *err);

    /** 只释放复制对象和 staging；设备/适配器留着复用（和参考实现一致） */
    void close();

    /** 连 D3D11 设备一起放掉（采集线程退出、CoUninitialize 之前调用） */
    void releaseAll();

    /** 采集链路是否可用 */
    bool isOpen() const;

    /**
     * @brief 抓一帧：GPU 复制 → Map → 缩放成 800x480 →（可选）画鼠标 → RGB888
     *
     * @param[out] out 输出图像，800x480 Format_RGB888
     * @param[out] err 出错原因；*err 为空表示"这一轮没有新画面"（画面静止，不是错误）
     * @return true 拿到了新画面；false 没有新帧或出错（看 err 区分）
     */
    bool grab(QImage *out, QString *err);

    /** grab() + JPEG 编码，一步到位 */
    bool grabJpeg(QByteArray *jpeg, int quality, QString *err);

    /* ---------------- 屏幕列表（GUI 线程可调用，只读元数据） ---------------- */

    /** 按下标取 QScreen；下标越界返回主屏；一块屏都没有返回 nullptr */
    static QScreen *resolveScreen(int index);

    /** 默认选哪块：有第二块屏就选它（多屏镜像场景），否则主屏 */
    static int defaultScreenIndex();

    /** 每块屏一行，顺序和 QGuiApplication::screens() 一致，可直接当下拉框条目 */
    static QStringList screenDescriptions();

    /* ---------------- JPEG 编码（纯 CPU，任意线程） ---------------- */

    /** 质量 1~100，默认 80；失败返回空 QByteArray（并 qWarning） */
    static QByteArray encodeJpeg(const QImage &image, int quality = 80);

private:
#ifdef Q_OS_WIN
    /** 遍历所有适配器的所有输出，找 DesktopCoordinates 与目标矩形一致的那块 */
    bool openCaptureChain(QString *err);
    /** 建 CPU 可读的 staging 纹理 */
    bool createStaging(UINT width, UINT height, DXGI_FORMAT format, QString *err);
    /** 把鼠标画到已经缩放好的画面上（光标位置按输出矩形换算） */
    void overlayCursor(QImage *img);
#endif

    int    m_screenIndex = -1;      // -1 = 主屏
    bool   m_drawCursor  = true;    // 是否把鼠标指针画进去
    double m_cursorScale = 1.0;     // 指针放大倍数

#ifdef Q_OS_WIN
    Microsoft::WRL::ComPtr<ID3D11Device>           m_device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext>    m_context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_duplication;
    Microsoft::WRL::ComPtr<ID3D11Texture2D>        m_staging;
    LUID   m_deviceAdapterLuid = {};    // 设备建在哪块适配器上，换屏时用它判断要不要重建
    UINT   m_width  = 0;                // staging 纹理尺寸 = 输出分辨率
    UINT   m_height = 0;
    int    m_skipFrames = 0;            // 刚建好复制后要丢弃的帧数（第一帧常常是全黑）
    QRect  m_outputRect;                // 该输出在虚拟桌面里的物理像素矩形（算光标位置用）
#endif
};

#endif // SCREENCAP_H
