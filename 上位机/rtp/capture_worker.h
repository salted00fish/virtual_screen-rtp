#ifndef CAPTURE_WORKER_H
#define CAPTURE_WORKER_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QString>

#include "video.h"

class QTimer;

/*==============================================================================
 *  CaptureWorker —— 采集线程里干活的对象
 *
 *  之前抓屏是在 GUI 线程用 QTimer 驱动的：抓一帧 40ms 左右，界面会跟着卡顿。
 *  现在把「GPU 抓帧 + 缩放 + JPEG 编码」整条链路搬到独立线程：
 *
 *      GUI 线程                     采集线程                    网络线程
 *   控件/状态栏  ──invokeMethod──►  CaptureWorker  ──frameReady──►  (GUI 线程转发)
 *                                    ScreenCapturer                 rtp 分片
 *                                    QTimer(fps)                    QUdpSocket
 *
 *  frameReady 里的 JPEG 是**已编码好的整帧**，GUI 线程收到后只管交给 rtp 分片，
 *  不碰任何屏幕/GPU 资源，所以界面始终是流畅的。
 *
 *  【跨线程约定】
 *    所有 public slots 都是给别的线程调的（信号槽自动排队）；
 *    ScreenCapturer 的 open/grab/close 只在本线程里调用。
 *
 *  【静止画面 / 失效恢复】
 *    - 画面没动时 DXGI 不给新帧，这里按 KEEPALIVE 周期补发上一帧，
 *      免得下位机那边看着像"断流"，也免得统计里帧率变成 0；
 *    - 锁屏、改分辨率、切独占全屏会让桌面复制永久失效（ACCESS_LOST），
 *      此时释放链路并每秒重试（锁屏期间只查状态不碰 DXGI）。
 *============================================================================*/

class CaptureWorker : public QObject
{
    Q_OBJECT
public:
    explicit CaptureWorker(QObject *parent = nullptr);
    ~CaptureWorker() override;

public slots:
    /** 开始采集（会顺带建好 GPU 采集链路） */
    void start(int fps);
    /** 暂停采集（链路保留，下次 start 直接复用） */
    void stop();
    /** 线程退出前调用：停表 + 释放 GPU 资源 + CoUninitialize */
    void shutdown();

    void setFps(int fps);
    void setQuality(int quality);
    void setScreenIndex(int index);     // 换屏 → 重建采集链路
    void setDrawCursor(bool on);
    void setCursorScale(int scale);
    /** 显示模式变化等外部原因导致的重建 */
    void rebuild();

signals:
    /** 一整帧 JPEG（已经编好码），由 GUI 线程接手做 RTP 分片发送 */
    void frameReady(const QByteArray &jpeg);
    /** 状态变化（就绪 / 失效 / 恢复），给状态栏用；不是每帧都发 */
    void message(const QString &text);
    /** 采集链路是否正常 */
    void captureStateChanged(bool ok);

private slots:
    void onTick();
    void onRecoverTick();

private:
    bool ensureOpen();
    void markBroken(const QString &why);
    void releaseAll();

    ScreenCapturer m_capturer;

    QTimer *m_tick    = nullptr;        // 采集节拍（按 fps 走）
    QTimer *m_recover = nullptr;        // 失效后的重试

    bool m_comInited   = false;
    bool m_broken      = false;
    bool m_wantRunning = false;

    int  m_fps     = 10;
    int  m_quality = 80;

    QByteArray     m_lastJpeg;          // 静止画面时补发用
    QElapsedTimer  m_sinceSend;
    QElapsedTimer  m_stat;
    quint32        m_frames = 0;
    quint32        m_keepaliveSends = 0;
    int            m_recoverTick = 0;
};

#endif // CAPTURE_WORKER_H
