#ifndef WIDGET_H
#define WIDGET_H

#include <QElapsedTimer>
#include <QHostAddress>
#include <QSet>
#include <QString>
#include <QWidget>
#include <atomic>
#include "audio.h"
#include "ikcp.h"
#include "touch.h"
#include <atomic>


QT_BEGIN_NAMESPACE
namespace Ui { class Widget; }
QT_END_NAMESPACE

class QThread;
class QUdpSocket;
class QLabel;
class QLineEdit;
class QSpinBox;
class QPushButton;
class QComboBox;
class QCheckBox;
class QScreen;

class rtp;
class CaptureWorker;

class Widget : public QWidget
{
    Q_OBJECT

public:
    Widget(QWidget *parent = nullptr);
    ~Widget();

    touch *esp_touch = nullptr;

signals:
    void signal_touch(QByteArray);
    void signal_kcp_data(QByteArray data);
    void signal_kcp_keep(void);
    /*kcp*/
public:
    QUdpSocket  *audio_udpSocket   = nullptr;
    QHostAddress audio_targetAddr;
    quint16      audio_targetPort = 5005;

    ikcpcb *audio_kcp = nullptr;
    QTimer *kcpTimer = nullptr;
    std::atomic<quint16> kcp_keep_cnt{0};

    void audio_kcp_create(void);
    void audio_kcp_destroy(void);
    void audio_kcp_timer_create(void);
    void audio_kcp_timer_destroy(void);
    void audio_kcp_input(void);
    void kcp_data(QByteArray data);
    void kcp_keep(void);

private slots:
    void onStartStopClicked();                  // “开始推流 / 停止推流”按钮
    void refreshScreenList();                   // 重新枚举显示器，填“捕获屏幕”下拉框

    /* 采集线程送过来的成品帧：GUI 线程只做 RTP 分片发送和统计 */
    void onFrameReady(const QByteArray &jpeg);
    /* 采集线程的状态变化（就绪 / 失效 / 恢复） */
    void onCaptureMessage(const QString &text);
    void audio_info(const QString &msg);

private:
    void buildUi();                             // 用代码创建所有控件（不改 widget.ui）
    void watchScreens();                        // 订阅显示器变化 → 通知采集线程重建
    void requestRebuild(const QString &why);    // 显示模式变化
    void updateStatus(bool streaming, int frameBytes = -1, int packets = -1);

    audio *audio_capture;

    Ui::Widget *ui;

    /* ---- 网络线程（原有结构：QUdpSocket 挪到线程里，invokeMethod 投递） ---- */
    QThread     *netThread   = nullptr;
    QUdpSocket  *udpSocket   = nullptr;

    QThread     *audio_netThread   = nullptr;


    rtp         *rtpq        = nullptr;

    /* ---- 采集线程（GPU 抓帧 + 缩放 + JPEG 编码都在里面） ---- */
    QThread       *captureThread = nullptr;
    CaptureWorker *captureWorker = nullptr;
    bool           m_captureOk   = false;       // 采集链路当前是否正常
    QString        m_captureMsg;                // 采集侧最近一条状态/错误

    // UI 控件（代码创建）
    QLineEdit   *ipEdit        = nullptr;
    QSpinBox    *portSpin      = nullptr;
    QSpinBox    *audio_portSpin      = nullptr;
    QComboBox   *screenCombo   = nullptr;       // 捕获哪块显示器
    QPushButton *refreshScreensBtn = nullptr;
    QCheckBox   *cursorCheck   = nullptr;       // 是否把鼠标指针抓进去
    QSpinBox    *cursorScaleSpin = nullptr;     // 指针放大倍数
    QLabel *audio_hz_Label = nullptr;
    QLabel *audioChLabel = nullptr;
    QSpinBox    *qualitySpin   = nullptr;
    QSpinBox    *fpsSpin       = nullptr;
    QSpinBox    *payloadSpin   = nullptr;
    QPushButton *startStopBtn  = nullptr;
    QCheckBox   *videoCheck    = nullptr;       // 允许发送视频 RTP 包
    QCheckBox   *audioCheck    = nullptr;       // 允许发送音频 RTP 包
    QLabel      *statusLabel   = nullptr;

    // 目标地址/端口的“值拷贝”：只在 GUI 线程写入，供投递到网络线程的 lambda 捕获
    QHostAddress m_targetAddr;
    quint16      m_targetPort = 5004;



    // 统计
    QElapsedTimer m_elapsed;
    qint64 m_frameIndex   = 0;      // 已发送帧序号
    qint64 m_lastFrameBytes = 0;    // 最近一帧 JPEG 字节数
    int    m_lastPackets    = 0;    // 最近一帧拆成的 RTP 包数
    bool   m_streaming      = false;
    QString m_lastError;

    QSet<QScreen *> m_watchedScreens;   // 已经订阅过信号的屏幕，避免重复连接

    /* GUI 线程写、网络线程读：必须是原子量，否则是数据竞争 */
    std::atomic<bool> audio_enable{false};
    std::atomic<bool> video_enable{false};

};
#endif // WIDGET_H
