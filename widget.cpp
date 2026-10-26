#include "widget.h"
#include "ui_widget.h"
#include "capture_worker.h"
#include "rtp.h"
#include "video.h"
#include "audio.h"
#include "avclock.h"

#include <QDebug>
#include <QGuiApplication>
#include <QHostAddress>
#include <QMetaObject>
#include <QScreen>
#include <QSignalBlocker>
#include <QThread>
#include <QUdpSocket>

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QTimer>



namespace {
// 默认参数（用户可在界面上改）
const char *kDefaultTargetIp = "192.168.1.11"; // 占位地址，改成下位机实际 IP
const quint16 kDefaultTargetPort = 5004;       // RTP 视频目标端口
const quint16 ADefaultTargetPort = 5005;       // RTP 音频目标端口

const int kDefaultQuality = 85;                // JPEG 质量
const int kDefaultFps     = 30;                // 帧率
const int kDefaultPayload = 1400;              // 每个 RTP 包最大载荷（1400+12+8+20=1440 < 1500 MTU）
}


static int kcp_output(const char *buf, int len, ikcpcb *kcp, void *user)
{
    Q_UNUSED(kcp);
    auto *self = static_cast<Widget*>(user);
    if (!self || !self->audio_udpSocket || len <= 0) return -1;

    qint64 n = self->audio_udpSocket->writeDatagram(buf, len, self->audio_targetAddr, self->audio_targetPort);
    return n == len ? len : -1;
}


Widget::Widget(QWidget *parent)
    : QWidget(parent)
    , ui(new Ui::Widget)
{

    ui->setupUi(this);


    connect(this,&Widget::signal_kcp_keep,this,&Widget::kcp_keep);

    esp_touch = new touch(this);
    connect(this,&Widget::signal_touch,esp_touch,&touch::touch_slit);
    connect(this,&Widget::signal_kcp_data,this,&Widget::kcp_data);

    audio_capture = new audio(this);
    connect(audio_capture,&audio::signal_audio_capture_status,this,&Widget::audio_info);

    // ---------------- 采集线程：GPU 抓帧 + 缩放 + JPEG 编码 ----------------
    // 抓一帧只要几毫秒（GPU 复制 + Map + 缩放），但 JPEG 编码和缩放加起来还是会把
    // GUI 线程顶住，所以整条链路都放这个线程里；GUI 线程只收编好的 JPEG。
    captureThread = new QThread();
    captureThread->setObjectName(QStringLiteral("capture"));

    captureWorker = new CaptureWorker();          // 无线程父对象，之后 moveToThread
    captureWorker->moveToThread(captureThread);

    // 顺序要紧：先 shutdown（在采集线程里释放 GPU 资源和 COM），再 deleteLater
    connect(captureThread, &QThread::finished, captureWorker, &CaptureWorker::shutdown);
    connect(captureThread, &QThread::finished, captureWorker, &QObject::deleteLater);

    connect(captureWorker, &CaptureWorker::frameReady, this, &Widget::onFrameReady);
    connect(captureWorker, &CaptureWorker::message, this, &Widget::onCaptureMessage);
    connect(captureWorker, &CaptureWorker::captureStateChanged, this, [this](bool ok)
    {
        m_captureOk = ok;
        updateStatus(m_streaming);
    });

    captureThread->start();


    // ---------------- 网络线程 + QUdpSocket（保持原有结构） ----------------
    netThread = new QThread(this);
    udpSocket = new QUdpSocket(); // 无线程父对象，之后 moveToThread

    audio_netThread = new QThread(this);
    audio_udpSocket = new QUdpSocket(); // 无线程父对象，之后 moveToThread

    QMetaObject::invokeMethod(udpSocket, [this]()
    {
        udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 1 << 20);

        if(udpSocket->bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0)))
        {
            qDebug() << "视频UDP已绑定端口" << udpSocket->localPort();
        }
        else
        {
            qWarning() << "视频UDP绑定端口失败:" << udpSocket->errorString();
        }
    }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(audio_udpSocket, [this]()
    {
        audio_udpSocket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption, 1 << 20);

        if(audio_udpSocket->bind(QHostAddress(QHostAddress::AnyIPv4),quint16(0)))
        {
            qDebug() << "音频 UDP已绑定端口" << audio_udpSocket->localPort();
            connect(audio_udpSocket,&QUdpSocket::readyRead,audio_udpSocket,[this](){audio_kcp_input();});
        }
        else
        {
            qWarning() << "音频UDP绑定端口失败:" << audio_udpSocket->errorString();
        }
    }, Qt::QueuedConnection);

    connect(netThread,&QThread::finished, udpSocket, &QObject::deleteLater);
    connect(audio_netThread, &QThread::finished, audio_udpSocket, &QObject::deleteLater);

    udpSocket->moveToThread(netThread);
    audio_udpSocket->moveToThread(audio_netThread);

    rtpq = new rtp(this);

    connect(audio_capture,&audio::audio_packet,rtpq,&rtp::send_audio_frame);

    rtpq->rtp_video_send_init([this](const QList<QByteArray> &batch)
    {
        const QHostAddress addr = m_targetAddr;
        const quint16      port = m_targetPort;

        QMetaObject::invokeMethod(udpSocket,[this,batch,addr,port]()
        {
            if(this->video_enable)
            {
                for (const QByteArray &data : batch)
                {
                    const qint64 n = udpSocket->writeDatagram(data,addr,port);

                    if (n != data.size())
                    {
                        qWarning() << "writeDatagram 发送不完整:" << n << "/" << data.size()<< "writeDatagram 失败:" << udpSocket->errorString()
                                   << "Error code:" << udpSocket->error();
                        break;
                    }
                }
            }
        },Qt::QueuedConnection);
    });


    rtpq->rtp_audio_send_init([this](const QList<QByteArray> &batch)
    {
        QMetaObject::invokeMethod(audio_udpSocket,[this, batch]()
        {
            if(this->audio_enable)
            {
                for (const QByteArray &data : batch)
                {
                    audio_udpSocket->writeDatagram(data,data.size(),audio_targetAddr,audio_targetPort);
                }
            }

        }, Qt::QueuedConnection);
    });



    netThread->start();
    audio_netThread->start();

    buildUi();

    // 显示器拓扑/分辨率/刷新率变化 → 让采集线程重建（GPU 复制对象会失效）
    watchScreens();

    updateStatus(false);
    setWindowTitle(QStringLiteral("RTP 屏幕推流发送端 (DXGI GPU 捕获 / 800x480 RGB888 JPEG)"));
}

Widget::~Widget()
{

    // 先让采集线程停手（它在跑的时候会一直用 GPU 资源）
    if (captureWorker && captureThread && captureThread->isRunning())
    {
        QMetaObject::invokeMethod(captureWorker, "stop", Qt::BlockingQueuedConnection);
    }
    if (captureThread && captureThread->isRunning())
    {
        captureThread->quit();
        captureThread->wait();
    }

    if (netThread && netThread->isRunning())
    {
        netThread->quit();
        netThread->wait();
    }

    if (audio_netThread && audio_netThread->isRunning())
    {
        QMetaObject::invokeMethod(audio_udpSocket, [this](){
            if (kcpTimer) { delete  kcpTimer; kcpTimer = nullptr; }
        }, Qt::BlockingQueuedConnection);

        QMetaObject::invokeMethod(audio_udpSocket, [this](){
            if (audio_kcp) { ikcp_release(audio_kcp); audio_kcp = nullptr; }
        }, Qt::BlockingQueuedConnection);
    }

    if (audio_netThread && audio_netThread->isRunning())
    {
        audio_netThread->quit();
        audio_netThread->wait();
    }

    delete ui;
}

void Widget::audio_kcp_create()
{
    if(audio_kcp)
    {
        ikcp_release(audio_kcp);
        audio_kcp = NULL;
    }

    audio_kcp = ikcp_create(0x11223344,this);
    ikcp_setoutput(audio_kcp,kcp_output);
    ikcp_wndsize(audio_kcp,16,64);
    ikcp_nodelay(audio_kcp,1,10,2,1);

}

void Widget::audio_kcp_destroy()
{
    if(!audio_kcp) return;

    ikcp_release(audio_kcp);
    audio_kcp = nullptr;
}

void Widget::audio_kcp_timer_create()
{
    if(kcpTimer)
    {
        delete kcpTimer;
        kcpTimer = nullptr;
    }

    kcpTimer = new QTimer(audio_udpSocket);
    kcpTimer->setInterval(10);
    kcpTimer->setTimerType(Qt::PreciseTimer);
    kcpTimer->start();

}

void Widget::audio_kcp_timer_destroy()
{
    if(!kcpTimer) return;

    kcpTimer->stop();
    delete kcpTimer;
    kcpTimer = nullptr;
}

void Widget::audio_kcp_input()
{
    while (audio_udpSocket->hasPendingDatagrams())
    {
        QByteArray d(int(audio_udpSocket->pendingDatagramSize()), Qt::Uninitialized);
        audio_udpSocket->readDatagram(d.data(), d.size());
        if (audio_kcp)
        {

            if(d.size() >= 4 && (d.at(0)=='P' && d.at(1)=='I' && d.at(2)=='N' && d.at(3)=='G'))
            {
                emit signal_kcp_keep();
            }else
            {
                ikcp_input(audio_kcp,d.constData(),long(d.size()));
                if(ikcp_peeksize(audio_kcp) > 0)
                {
                    emit signal_kcp_data(d);
                }
            }
        }

    }
}

void Widget::kcp_data(QByteArray data)
{
    QByteArray buf = data;

    int n;
    while ((n = ikcp_recv(audio_kcp, buf.data(), buf.size())) > 0)
    {

        if (n >= int(sizeof(touch_data_t)))
        {
            emit signal_touch(buf);
        }
    }
}

void Widget::kcp_keep()
{
    kcp_keep_cnt = 0;
}

// ---------------------------------------------------------------------------
// UI：全部用代码创建，不改 widget.ui 的 XML（ui 里仍是一个空 QWidget）
// ---------------------------------------------------------------------------
void Widget::buildUi()
{
    QVBoxLayout *mainLayout = new QVBoxLayout(this);

    QGroupBox *netBox = new QGroupBox(QStringLiteral("网络目标 (下位机 ESP32)"), this);
    QFormLayout *netForm = new QFormLayout(netBox);

    ipEdit = new QLineEdit(QString::fromLatin1(kDefaultTargetIp), netBox);
    ipEdit->setToolTip(QStringLiteral("下位机实际 IP，例如 192.168.1.11"));
    netForm->addRow(QStringLiteral("目标 IP:"), ipEdit);

    portSpin = new QSpinBox(netBox);
    portSpin->setRange(1, 65535);
    portSpin->setValue(kDefaultTargetPort);
    netForm->addRow(QStringLiteral("视频目标端口:"), portSpin);

    audio_portSpin = new QSpinBox(netBox);
    audio_portSpin->setRange(1, 65535);
    audio_portSpin->setValue(ADefaultTargetPort);
    netForm->addRow(QStringLiteral("音频目标端口:"), audio_portSpin);

    QLabel *localPortLabel = new QLabel(QStringLiteral("%1").arg(audio_udpSocket->localPort()), netBox);
    netForm->addRow(QStringLiteral("本地绑定端口:"), localPortLabel);

    mainLayout->addWidget(netBox);

    QGroupBox *encBox = new QGroupBox(QStringLiteral("采集 / 编码参数"), this);
    QFormLayout *encForm = new QFormLayout(encBox);

    // ---- 捕获哪块显示器（多屏时用）----
    QHBoxLayout *screenRow = new QHBoxLayout();
    screenCombo = new QComboBox(encBox);
    screenCombo->setToolTip(QStringLiteral("选择要抓取的显示器。推流过程中也可以直接切换，不用停。"));
    refreshScreensBtn = new QPushButton(QStringLiteral("刷新"), encBox);
    refreshScreensBtn->setFixedWidth(56);
    refreshScreensBtn->setToolTip(QStringLiteral("插拔显示器后点一下重新枚举"));
    screenRow->addWidget(screenCombo, 1);
    screenRow->addWidget(refreshScreensBtn, 0);
    encForm->addRow(QStringLiteral("捕获屏幕:"), screenRow);

    connect(refreshScreensBtn, &QPushButton::clicked, this, &Widget::refreshScreenList);
    connect(screenCombo, QOverload<int>::of(&QComboBox::currentIndexChanged),
            this, [this](int idx)
    {
        // 采参数在采集线程里用，必须投递过去改
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this, idx]()
            {
                captureWorker->setScreenIndex(idx);
            }, Qt::QueuedConnection);
        }
        updateStatus(m_streaming);

        //触摸屏幕的时候，选择操作那个屏幕
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (esp_touch && idx >= 0 && idx < screens.size())
            esp_touch->setScreenRect(screens.at(idx)->geometry());

    });

    // ---- 鼠标指针 ----
    cursorCheck = new QCheckBox(QStringLiteral("鼠标指针"), encBox);
    cursorCheck->setMaximumWidth(120);
    cursorCheck->setChecked(false);
    cursorCheck->setToolTip(QStringLiteral("桌面复制拿不到系统叠加的鼠标指针，勾上后额外用 GDI 画到画面里"));
    encForm->addRow(QString(), cursorCheck);

    cursorScaleSpin = new QSpinBox(encBox);
    cursorScaleSpin->setMaximumWidth(120);
    cursorScaleSpin->setRange(1, 4);
    cursorScaleSpin->setValue(1);
    cursorScaleSpin->setSuffix(QStringLiteral(" 倍"));
    cursorScaleSpin->setToolTip(QStringLiteral("缩到 800x480 后指针会很小，看不清可以放大；放大时箭头尖端位置不变"));

    // ---- 把"指针放大"和"音频"塞进同一行的字段位 ----
    QWidget *scaleRow = new QWidget(encBox);
    QHBoxLayout *scaleLay = new QHBoxLayout(scaleRow);
    scaleLay->setContentsMargins(0, 0, 0, 0);
    scaleLay->setSpacing(8);

    scaleLay->addWidget(cursorScaleSpin);

    scaleLay->addWidget(new QLabel(QStringLiteral("           音频频率:"), scaleRow));

    audio_hz_Label = new QLabel(QStringLiteral("48KHz"), scaleRow);
    audio_hz_Label->setFrameShape(QFrame::StyledPanel);
    audio_hz_Label->setAlignment(Qt::AlignCenter);
    audio_hz_Label->setMinimumWidth(70);
    scaleLay->addWidget(audio_hz_Label);


    scaleLay->addWidget(new QLabel(QStringLiteral("声道:"), scaleRow));

    audioChLabel = new QLabel(QStringLiteral("立体声"), scaleRow);
    audioChLabel->setFrameShape(QFrame::StyledPanel);
    audioChLabel->setAlignment(Qt::AlignCenter);
    audioChLabel->setMinimumWidth(70);
    scaleLay->addWidget(audioChLabel);


    scaleLay->addStretch(1);          // ← 必须在所有控件之后

    encForm->addRow(QStringLiteral("指针放大:"), scaleRow);



    connect(cursorCheck, &QCheckBox::toggled, this, [this](bool on)
    {
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this, on]()
            {
                captureWorker->setDrawCursor(on);
            }, Qt::QueuedConnection);
        }
        updateStatus(m_streaming);
    });
    connect(cursorScaleSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int v)
    {
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this, v]()
            {
                captureWorker->setCursorScale(v);
            }, Qt::QueuedConnection);
        }
    });

    qualitySpin = new QSpinBox(encBox);
    qualitySpin->setMaximumWidth(120);
    qualitySpin->setRange(1, 100);
    qualitySpin->setValue(kDefaultQuality);
    encForm->addRow(QStringLiteral("JPEG 质量 (1~100):"), qualitySpin);

    fpsSpin = new QSpinBox(encBox);
    fpsSpin->setMaximumWidth(120);
    fpsSpin->setRange(1, 240);
    fpsSpin->setValue(kDefaultFps);
    fpsSpin->setToolTip(QStringLiteral("这是采集上限，不是保证值：\n"
                                       "· 画面静止时 DXGI 不给新帧，只按 1 秒心跳补发\n"
                                       "· 高帧率下每帧 JPEG 越大越吃 Wi-Fi 带宽，"
                                       "下位机丢帧就先降帧率或降质量"));
    encForm->addRow(QStringLiteral("帧率 fps (1~240):"), fpsSpin);

    connect(qualitySpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int q)
    {
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this, q]()
            {
                captureWorker->setQuality(q);
            }, Qt::QueuedConnection);
        }
    });
    connect(fpsSpin, QOverload<int>::of(&QSpinBox::valueChanged), this, [this](int f)
    {
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this, f]()
            {
                captureWorker->setFps(f);
            }, Qt::QueuedConnection);
        }
    });

    payloadSpin = new QSpinBox(encBox);
    payloadSpin->setMaximumWidth(120);
    payloadSpin->setRange(200, 1400);
    payloadSpin->setSingleStep(100);
    payloadSpin->setValue(kDefaultPayload);
    payloadSpin->setToolTip(QStringLiteral("每个 RTP 包的最大载荷字节数，默认 1400（1440 < 1500 MTU）"));
    encForm->addRow(QStringLiteral("JPEG单包最大载荷:"), payloadSpin);

    QLabel *sizeLabel = new QLabel(QStringLiteral("采集: DXGI 桌面复制(GPU) -> 800x480 RGB888 -> JPEG (PT=26)"), encBox);
    encForm->addRow(QString(), sizeLabel);

    mainLayout->addWidget(encBox);

    QHBoxLayout *btnLayout = new QHBoxLayout();
    btnLayout->setContentsMargins(0, 0, 0, 0);

    /* 一个中文字宽 ≈ 字体像素高度的 1 倍，用它当间距，随系统字体自动变大变小 */
    const int kGap = fontMetrics().horizontalAdvance(QStringLiteral("中"));

    /* 左侧弹簧 + 右侧弹簧等长 → 按钮组整体居中 */
    btnLayout->addStretch(1);

    startStopBtn = new QPushButton(QStringLiteral("开始推流"), this);
    startStopBtn->setMinimumHeight(34);
    connect(startStopBtn, &QPushButton::clicked, this, &Widget::onStartStopClicked);
    btnLayout->addWidget(startStopBtn);

    btnLayout->addSpacing(kGap);

    /* 视频 / 音频 发送开关：取消勾选即停发，推流中可随时切换 */
    videoCheck = new QCheckBox(QStringLiteral("视频"), this);
    videoCheck->setChecked(true);
    videoCheck->setToolTip(QStringLiteral("取消勾选后不再发送视频 RTP 包（推流中可随时切换）"));
    btnLayout->addWidget(videoCheck);

    btnLayout->addSpacing(kGap);

    audioCheck = new QCheckBox(QStringLiteral("音频"), this);
    audioCheck->setChecked(true);
    audioCheck->setToolTip(QStringLiteral("取消勾选后不再发送音频 RTP 包（推流中可随时切换）"));
    btnLayout->addWidget(audioCheck);

    btnLayout->addStretch(1);

    connect(videoCheck, &QCheckBox::toggled, this, [this](bool on)
    {
        video_enable = on;
        qDebug() << "[发送] 视频" << (on ? "开启" : "关闭");
        updateStatus(m_streaming);
    });

    connect(audioCheck, &QCheckBox::toggled, this, [this](bool on)
    {
        audio_enable = on;
        qDebug() << "[发送] 音频" << (on ? "开启" : "关闭");
        updateStatus(m_streaming);
    });

    mainLayout->addLayout(btnLayout);

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    statusLabel->setMinimumHeight(48);
    statusLabel->setFrameShape(QFrame::StyledPanel);
    statusLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    statusLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    mainLayout->addWidget(statusLabel);

    mainLayout->addStretch(1);

    // 枚举显示器（有第二块屏就默认选它）
    refreshScreenList();
}

// ---------------------------------------------------------------------------
// 枚举显示器，填“捕获屏幕”下拉框
// ---------------------------------------------------------------------------
void Widget::refreshScreenList()
{
    const QStringList descs = ScreenCapturer::screenDescriptions();
    const int keep = screenCombo->currentIndex();   // 尽量保留用户当前的选择

    int idx = keep;
    {
        QSignalBlocker blocker(screenCombo);        // 填充过程别触发 currentIndexChanged
        screenCombo->clear();
        screenCombo->addItems(descs);

        if (idx < 0 || idx >= descs.size())
        {
            idx = ScreenCapturer::defaultScreenIndex();  // 有第二块屏就默认选它
        }
        if (idx < 0 || idx >= descs.size())
        {
            idx = 0;
        }
        screenCombo->setCurrentIndex(idx);
    }

    if (captureWorker)
    {
        QMetaObject::invokeMethod(captureWorker, [this, idx]()
        {
            captureWorker->setScreenIndex(idx);
        }, Qt::QueuedConnection);
    }

    if (esp_touch)
    {
        const QList<QScreen *> screens = QGuiApplication::screens();
        if (idx >= 0 && idx < screens.size())
            esp_touch->setScreenRect(screens.at(idx)->geometry());
    }

    qDebug() << "捕获屏幕:" << screenCombo->currentText();
}

// ---------------------------------------------------------------------------
// 显示器变化 → 通知采集线程重建（GPU 复制对象在改分辨率/换显示器后会失效）
// ---------------------------------------------------------------------------
void Widget::watchScreens()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    for (QScreen *s : screens)
    {
        if (s == nullptr || m_watchedScreens.contains(s))
        {
            continue;
        }
        m_watchedScreens.insert(s);

        connect(s, &QScreen::geometryChanged, this, [this](const QRect &)
        {
            requestRebuild(QStringLiteral("屏幕分辨率/位置变化"));
        });
        connect(s, &QScreen::physicalSizeChanged, this, [this](const QSizeF &)
        {
            requestRebuild(QStringLiteral("屏幕物理尺寸变化"));
        });
        connect(s, &QScreen::refreshRateChanged, this, [this](qreal)
        {
            requestRebuild(QStringLiteral("屏幕刷新率变化"));
        });
    }

    static bool appSignalsConnected = false;
    if (!appSignalsConnected)
    {
        appSignalsConnected = true;
        connect(qApp, &QGuiApplication::screenAdded, this, [this](QScreen *)
        {
            watchScreens();
            refreshScreenList();
            requestRebuild(QStringLiteral("显示器接入"));
        });
        connect(qApp, &QGuiApplication::screenRemoved, this, [this](QScreen *)
        {
            refreshScreenList();
            requestRebuild(QStringLiteral("显示器移除"));
        });
    }
}

void Widget::requestRebuild(const QString &why)
{
    qDebug().noquote() << "[采集] 显示模式变化：" << why << "→ 重建采集链路";
    if (captureWorker)
    {
        QMetaObject::invokeMethod(captureWorker, [this]()
        {
            captureWorker->rebuild();
        }, Qt::QueuedConnection);
    }
}

// ---------------------------------------------------------------------------
// 开始 / 停止
// ---------------------------------------------------------------------------
void Widget::onStartStopClicked()
{

    if (m_streaming)
    {
        if (captureWorker)
        {
            QMetaObject::invokeMethod(captureWorker, [this]()
            {
                captureWorker->stop();
            }, Qt::QueuedConnection);
        }
        m_streaming = false;
        startStopBtn->setText(QStringLiteral("开始推流"));
        ipEdit->setEnabled(true);
        portSpin->setEnabled(true);
        audio_portSpin->setEnabled(true);
        // 停止时保留最后一次统计，不刷新成“已停止”

        if (videoCheck) videoCheck->setChecked(false);
        if (audioCheck) audioCheck->setChecked(false);
        video_enable = false;
        audio_enable = false;

        QMetaObject::invokeMethod(audio_udpSocket, [this]() { audio_kcp_timer_destroy(); }, Qt::QueuedConnection);
        QMetaObject::invokeMethod(audio_udpSocket, [this]() { audio_kcp_destroy(); }, Qt::QueuedConnection);
        audio_capture->stop();
        return;
    }

    /* 开始推流：音视频发送强制打开，复选框同步回勾选态（放在 start() 之前，保证第一帧就有标志） */
    video_enable = true;
    audio_enable = true;
    if (videoCheck) videoCheck->setChecked(true);
    if (audioCheck) audioCheck->setChecked(true);

    QMetaObject::invokeMethod(audio_udpSocket, [this]() { audio_kcp_create(); }, Qt::QueuedConnection);
    QMetaObject::invokeMethod(audio_udpSocket, [this]() { audio_kcp_timer_create(); }, Qt::QueuedConnection);

    QMetaObject::invokeMethod(audio_udpSocket,[this]()
    {
        connect(kcpTimer,&QTimer::timeout,audio_udpSocket,[this]()
        {
            if(!audio_kcp) return;

            IUINT32 now_ms = (IUINT32)(avclock::now_us() / 1000);
            ikcp_update(audio_kcp,(IUINT32)(now_ms));

            static quint32 last_ping = 0;
            if((quint32)(now_ms - last_ping) >= 200)
            {
                last_ping = now_ms;
                const char ping[4] = {'P','I','N','G'};
                audio_udpSocket->writeDatagram(ping, 4, audio_targetAddr, audio_targetPort);

                kcp_keep_cnt++;

                if(kcp_keep_cnt == 5)
                {
                    kcp_keep_cnt = 6;
                    audio_kcp_create();
                }
            }

        });
    }, Qt::QueuedConnection);

    audio_capture->start();
    // 在 GUI 线程里把 UI 上的值取出来，转成值类型，供网络线程/采集线程的 lambda 使用
    const QString ipText = ipEdit->text().trimmed();
    QHostAddress addr;
    if (!addr.setAddress(ipText))
    {
        m_lastError = QStringLiteral("目标 IP 非法: %1").arg(ipText);
        statusLabel->setText(QStringLiteral("无法开始推流 —— %1").arg(m_lastError));
        qWarning() << m_lastError;
        return;
    }

    m_targetAddr = addr;
    audio_targetAddr = addr;

    m_targetPort = static_cast<quint16>(portSpin->value());
    audio_targetPort = static_cast<quint16>(audio_portSpin->value());

    m_frameIndex     = 0;
    m_lastFrameBytes = 0;
    m_lastPackets    = 0;
    m_lastError.clear();
    m_captureMsg.clear();

    m_elapsed.start();
    m_streaming = true;

    // 把当前界面参数一次性推给采集线程，然后开跑
    const int fps         = fpsSpin->value();
    const int quality     = qualitySpin->value();
    const int screenIdx   = screenCombo->currentIndex();
    const bool drawCursor = cursorCheck->isChecked();
    const int cursorScale = cursorScaleSpin->value();

    QMetaObject::invokeMethod(captureWorker,[this, fps, quality, screenIdx, drawCursor, cursorScale]()
    {
        captureWorker->setQuality(quality);
        captureWorker->setScreenIndex(screenIdx);
        captureWorker->setDrawCursor(drawCursor);
        captureWorker->setCursorScale(cursorScale);
        captureWorker->start(fps);
    },Qt::QueuedConnection);

    startStopBtn->setText(QStringLiteral("停止推流"));
    ipEdit->setEnabled(false);
    portSpin->setEnabled(false);
    audio_portSpin->setEnabled(false);
    // “捕获屏幕”等采集参数故意不禁用：推流中可以随时改，改了下一帧生效

    qDebug() << "开始推流 ->" << m_targetAddr.toString() << m_targetPort
             << "fps:" << fps << "quality:" << quality
             << "screen:" << screenCombo->currentText();

    updateStatus(true);
}

// ---------------------------------------------------------------------------
// 采集线程送来一整帧 JPEG：这里只做 RTP 分片发送 + 统计（GUI 线程）
// ---------------------------------------------------------------------------
void Widget::onFrameReady(const QByteArray &jpeg)
{
    if (!m_streaming || jpeg.isEmpty())
    {
        return;
    }

    const int packets = rtpq->send_jpeg_frame(jpeg, payloadSpin->value());
    if (packets <= 0)
    {
        m_lastError = QStringLiteral("rtp 分片发送失败，本帧未发出");
        updateStatus(true);
        return;
    }

    m_lastError.clear();
    ++m_frameIndex;
    m_lastFrameBytes = jpeg.size();
    m_lastPackets    = packets;

    updateStatus(true, static_cast<int>(m_lastFrameBytes), m_lastPackets);
}

void Widget::onCaptureMessage(const QString &text)
{
    m_captureMsg = text;
    qDebug().noquote() << "[采集]" << text;
    updateStatus(m_streaming);
}

void Widget::audio_info(const QString &msg)
{
    qDebug()<<msg;
}

void Widget::updateStatus(bool streaming, int frameBytes, int packets)
{
    const qint64 ms = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;

    double fps = 0.0;
    // 帧率按"开流以来的平均值"算：已发帧数 / 已用时间
    if (ms > 0 && m_frameIndex > 0)
        fps = m_frameIndex * 1000.0 / static_cast<double>(ms);

    const int shownBytes   = (frameBytes >= 0) ? frameBytes : static_cast<int>(m_lastFrameBytes);
    const int shownPackets = (packets >= 0) ? packets : m_lastPackets;

    QString text;
    if (streaming)
        text += QStringLiteral("状态: 推流中");
    else
        text += QStringLiteral("状态: 已停止");

    text += QStringLiteral("    视频: %1 音频: %2\n")
                .arg(video_enable ? QStringLiteral("开") : QStringLiteral("关"))
                .arg(audio_enable ? QStringLiteral("开") : QStringLiteral("关"));

    if (screenCombo)
    {
        text += QStringLiteral("捕获: GPU (DXGI 桌面复制)%1    屏幕: %2")
                    .arg(m_captureOk ? QString() : QStringLiteral("[未就绪]"))
                    .arg(screenCombo->currentText());
        text += cursorCheck && cursorCheck->isChecked()
                    ? QStringLiteral("    指针: 开")
                    : QStringLiteral("    指针: 关");
        text += QStringLiteral("\n");
    }

    text += QStringLiteral("已发送帧数: %1").arg(m_frameIndex);
    if (shownBytes > 0)
    {
        text += QStringLiteral("    当前帧: %1 字节 / %2 个 RTP 包")
                    .arg(shownBytes)
                    .arg(shownPackets);
    }
    text += QStringLiteral("\n估算帧率: %1 fps    已用时: %2 ms")
                .arg(fps, 0, 'f', 1)
                .arg(ms);

    if (!m_captureMsg.isEmpty())
        text += QStringLiteral("\n采集: %1").arg(m_captureMsg);

    if (!m_lastError.isEmpty())
        text += QStringLiteral("\n提示: %1").arg(m_lastError);

    statusLabel->setText(text);
}
