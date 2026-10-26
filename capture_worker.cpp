#include "capture_worker.h"

#include <QDebug>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <qt_windows.h>
#endif

namespace {

// 画面静止时多久补发一次上一帧（毫秒）；0 = 不补发
const int KEEPALIVE_MS = 1000;
// 采集失效后多久重试一次
const int RECOVER_INTERVAL_MS = 1000;
// 恢复日志节流：每 N 次重试提示一次（≈N 秒）
const int RECOVER_LOG_EVERY = 30;

#ifdef Q_OS_WIN
// 锁屏 / UAC 弹窗时输入桌面切到 Winlogon 安全桌面，这时候 DXGI 桌面复制拿不到东西，
// 重试也没用（会立刻再 ACCESS_LOST），所以只查状态、等解锁。
bool sessionIsLocked()
{
    HDESK desk = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS | DESKTOP_SWITCHDESKTOP);
    if (desk == nullptr) {
        return true;
    }

    wchar_t name[128] = { 0 };
    DWORD needed = 0;
    bool locked = true;
    if (GetUserObjectInformationW(desk, UOI_NAME, name, sizeof(name), &needed)) {
        locked = (_wcsicmp(name, L"Default") != 0);   // 正常桌面叫 Default
    }
    CloseDesktop(desk);
    return locked;
}
#endif

} // namespace

CaptureWorker::CaptureWorker(QObject *parent) : QObject(parent)
{
    m_tick = new QTimer(this);
    m_tick->setTimerType(Qt::PreciseTimer);
    connect(m_tick, &QTimer::timeout, this, &CaptureWorker::onTick);

    m_recover = new QTimer(this);
    m_recover->setInterval(RECOVER_INTERVAL_MS);
    connect(m_recover, &QTimer::timeout, this, &CaptureWorker::onRecoverTick);
}

CaptureWorker::~CaptureWorker()
{
    // 正常路径下 shutdown() 已经在 stop 线程前调用过了，这里只做兜底
    releaseAll();
}

void CaptureWorker::start(int fps)
{
    if (!m_comInited) {
        // 桌面复制/DXGI 是 COM 组件，采集线程里初始化一次，线程退出时配对反初始化
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        m_comInited = SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
        if (!m_comInited) {
            emit message(QStringLiteral("CoInitializeEx 失败(0x%1)，GPU 捕获可能不可用")
                             .arg(quint32(hr), 8, 16, QLatin1Char('0')));
        }
    }

    m_wantRunning = true;
    m_stat.restart();

    setFps(fps > 0 ? fps : 10);        // 内部会启动/重启定时器

    if (!ensureOpen()) {
        return;                        // markBroken 已经挂上恢复定时器了
    }

    emit message(QStringLiteral("GPU 捕获（DXGI 桌面复制）已启动"));
    emit captureStateChanged(true);

    // 立刻抓一帧，别让第一帧干等一个周期
    QMetaObject::invokeMethod(this, "onTick", Qt::QueuedConnection);
}

void CaptureWorker::stop()
{
    m_wantRunning = false;
    if (m_tick) {
        m_tick->stop();
    }
    if (m_recover) {
        m_recover->stop();
    }
    m_broken = false;
}

void CaptureWorker::shutdown()
{
    stop();
    releaseAll();
}

void CaptureWorker::releaseAll()
{
    m_capturer.releaseAll();
    m_lastJpeg.clear();

#ifdef Q_OS_WIN
    if (m_comInited) {
        CoUninitialize();
        m_comInited = false;
    }
#endif
}

void CaptureWorker::setFps(int fps)
{
    // 只是"节拍上限"：QTimer 到这个间隔就叫一次 onTick，抓不到新帧就空转跳过。
    // 实际能跑多快取决于 grab+编码 的耗时（本机实测 7~11 ms/帧）和后面 Wi-Fi/ESP32 那一环。
    m_fps = qBound(1, fps, 240);
    if (m_tick) {
        m_tick->start(qMax(1, 1000 / m_fps));
    }
}

void CaptureWorker::setQuality(int quality)
{
    m_quality = qBound(1, quality, 100);
}

void CaptureWorker::setScreenIndex(int index)
{
    if (m_capturer.screenIndex() == index) {
        return;
    }

    m_capturer.setScreenIndex(index);
    if (m_wantRunning) {
        // 换屏了：旧输出的复制对象作废，重建
        m_capturer.close();
        m_lastJpeg.clear();
        ensureOpen();
    }
}

void CaptureWorker::setDrawCursor(bool on)
{
    m_capturer.setDrawCursor(on);
}

void CaptureWorker::setCursorScale(int scale)
{
    m_capturer.setCursorScale(scale);
}

void CaptureWorker::rebuild()
{
    m_capturer.close();
    m_lastJpeg.clear();
    if (m_wantRunning && !m_broken) {
        ensureOpen();
    }
}

bool CaptureWorker::ensureOpen()
{
    if (m_capturer.isOpen()) {
        return true;
    }

    QString err;
    if (!m_capturer.open(&err)) {
        markBroken(err.isEmpty() ? QStringLiteral("采集初始化失败") : err);
        return false;
    }

    m_broken = false;
    m_recoverTick = 0;
    emit captureStateChanged(true);
    return true;
}

void CaptureWorker::markBroken(const QString &why)
{
    if (!m_broken) {
        qWarning().noquote() << "[采集] 失效：" << why << "→ 释放链路，每秒重试";
        emit message(QStringLiteral("采集失效：%1（正在重试）").arg(why));
        emit captureStateChanged(false);
        m_broken = true;
        m_recoverTick = 0;
    }

    m_capturer.close();
    m_lastJpeg.clear();

    if (m_wantRunning && m_recover) {
        m_recover->start();
    }
}

void CaptureWorker::onRecoverTick()
{
    if (!m_wantRunning) {
        m_recover->stop();
        return;
    }

#ifdef Q_OS_WIN
    if (sessionIsLocked()) {
        if (++m_recoverTick % RECOVER_LOG_EVERY == 0) {
            qDebug() << "[采集] 仍是锁屏状态，等解锁后再重建";
        }
        return;
    }
#endif

    QString err;
    if (m_capturer.open(&err)) {
        m_broken = false;
        m_recoverTick = 0;
        m_recover->stop();
        qInfo().noquote() << "[采集] 已恢复";
        emit message(QStringLiteral("GPU 捕获已恢复"));
        emit captureStateChanged(true);
        QMetaObject::invokeMethod(this, "onTick", Qt::QueuedConnection);
        return;
    }

    if (++m_recoverTick % RECOVER_LOG_EVERY == 0) {
        qWarning().noquote() << "[采集] 重建失败：" << err << "→ 继续每秒重试";
    }
}

void CaptureWorker::onTick()
{
    if (!m_wantRunning || m_broken) {
        return;
    }
    if (!ensureOpen()) {
        return;
    }

    QImage frame;
    QString err;
    if (!m_capturer.grab(&frame, &err)) {
        if (!err.isEmpty()) {
            markBroken(err);           // 真错误（ACCESS_LOST / 分辨率变化…）
            return;
        }

        // 画面没动：按心跳周期补发上一帧，别让链路看着像断了
        if (KEEPALIVE_MS > 0 && !m_lastJpeg.isEmpty() &&
            m_sinceSend.isValid() && m_sinceSend.elapsed() >= KEEPALIVE_MS) {
            m_sinceSend.restart();
            ++m_keepaliveSends;
            emit frameReady(m_lastJpeg);
        }
        return;
    }

    QByteArray jpeg = ScreenCapturer::encodeJpeg(frame, m_quality);
    if (jpeg.isEmpty()) {
        qWarning() << "[采集] JPEG 编码失败，本帧跳过";
        return;
    }

    m_lastJpeg = jpeg;
    m_sinceSend.restart();
    ++m_frames;

    if (m_stat.isValid() && m_stat.elapsed() >= 1000) {
        qDebug().noquote() << QStringLiteral("[采集] %1 fps（含静止补发 %2 次），最近一帧 %3 字节，"
                                            "线程 0x%4")
                                  .arg(m_frames)
                                  .arg(m_keepaliveSends)
                                  .arg(jpeg.size())
                                  .arg(reinterpret_cast<quintptr>(QThread::currentThreadId()), 0, 16);
        m_frames = 0;
        m_keepaliveSends = 0;
        m_stat.restart();
    }

    emit frameReady(jpeg);
}
