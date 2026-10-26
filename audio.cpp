#include "audio.h"
#include "avclock.h"
#include "rtp.h"
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601          /* WASAPI 需要 Vista+ */
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <QDebug>
#include <opus.h>

const CLSID kCLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
const IID kIID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
const IID kIID_IAudioClient =
    {0x1CB9AD4C, 0xDBFA, 0x4C32, {0xB1, 0x78, 0xC2, 0xF5, 0x68, 0xA7, 0x03, 0xB2}};
const IID kIID_IAudioCaptureClient =
    {0xC8ADBD64, 0xE71E, 0x48A0, {0xA4, 0xDE, 0x18, 0x5C, 0x39, 0x5C, 0xD3, 0x17}};
const GUID kSubFormatPcm =
    {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};
const GUID kSubFormatFloat =
    {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71}};


audio_capture_thread::audio_capture_thread(QWidget *parent) : QThread(parent)
{
    opus_err = OPUS_OK;
    audio_opus = opus_encoder_create(AV_AUDIO_RATE,AV_AUDIO_CHANNELS,OPUS_APPLICATION_AUDIO,&opus_err);
    if(opus_err != OPUS_OK || audio_opus == NULL)
    {
        qWarning("opus_encoder_create error");
    }

    if(audio_opus)
    {
        opus_encoder_ctl(audio_opus,OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(audio_opus,OPUS_SET_COMPLEXITY(10));
    }
}

audio_capture_thread::~audio_capture_thread()
{
    stop();
}

void audio_capture_thread::stop(void)
{
    audio_captur_stop = true;
    if (isRunning())
    {
        wait(600);
    }

    if(audio_opus)
    {
        opus_encoder_destroy(audio_opus);
        audio_opus = nullptr;
    }
}

static inline int16_t clamp16(int32_t v)
{
    if (v > 32767)  return 32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static inline void read_channel(const BYTE *data, size_t idx, int ch, int bits,
                                bool isFloat, int validBits, int16_t &out)
{
    if(isFloat)
    {
        const float *f = reinterpret_cast<const float *>(data) + idx * ch;
        double v = (double)f[0];
        if (v > 1.0)  v = 1.0;
        if (v < -1.0) v = -1.0;
        out = (int16_t)(v * 32767.0);
        return;
    }
    switch (bits)
    {
    case 8: {
        const uint8_t *u = data + idx * ch;                       /* 8bit 是无符号 */
        out = (int16_t)(((int32_t)u[0] - 128) << 8);
        break;
    }
    case 16: {
        const int16_t *s = reinterpret_cast<const int16_t *>(data) + idx * ch;
        out = s[0];
        break;
    }
    case 24: {
        const uint8_t *u = data + idx * ch * 3;
        int32_t v = (int32_t)((uint32_t)u[0] | ((uint32_t)u[1] << 8) | ((uint32_t)u[2] << 16));
        if (v & 0x800000) v |= (int32_t)0xFF000000;               /* 符号扩展 */
        out = clamp16(v >> 8);
        break;
    }
    case 32: {
        const int32_t *s = reinterpret_cast<const int32_t *>(data) + idx * ch;
        /* 32bit 容器里有效位是左对齐（MSB 对齐）的：
         *   有效32 -> 取高 16 位
         *   有效24 -> 取高 16 位（低 8 位本来是 0 填充）
         * 所以恒为 16。原来的 bits - validBits + 8 在 validBits==32 时只移 8 位，
         * 增益×256，全被 clamp 成方波。 */
        (void)validBits;
        int shift = 16;
        if (shift < 0 || shift > 31) shift = 16;      /* 驱动少报/乱报时保持原行为 */
        out = clamp16(s[0] >> shift);
        break;
    }
    default: {
        const int16_t *s = reinterpret_cast<const int16_t *>(data) + idx * ch;
        out = s[0];
        break;
    }
    }
}

void audio_capture_thread::run()
{
    QString errMsg;

    opus_err = OPUS_OK;
    audio_opus = opus_encoder_create(AV_AUDIO_RATE,AV_AUDIO_CHANNELS,OPUS_APPLICATION_AUDIO,&opus_err);
    if(opus_err != OPUS_OK || audio_opus == NULL)
    {
        errMsg = QStringLiteral("opus创建失败");
        emit capture_status(errMsg);
        qWarning("opus_encoder_create error");
        return ;
    }

    if(audio_opus)
    {
        opus_encoder_ctl(audio_opus,OPUS_SET_BITRATE(96000));
        opus_encoder_ctl(audio_opus,OPUS_SET_COMPLEXITY(10));
    }

    audio_capturing = false;
    audio_captur_stop = false;

    HRESULT err = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comOk = SUCCEEDED(err);

    if (!comOk && err != RPC_E_CHANGED_MODE)
    {
        errMsg = QStringLiteral("COM 初始化失败");
        emit capture_status(errMsg);
        qWarning("AudioCapture: COM 初始化失败: 0x%08X", (quint32)err);
        return;
    }

    IMMDeviceEnumerator  *enumerator = nullptr;
    IMMDevice            *device     = nullptr;
    IAudioClient         *client     = nullptr;
    IAudioCaptureClient  *capture    = nullptr;
    WAVEFORMATEX         *mix        = nullptr;

    const int PKT_BYTES = AV_AUDIO_PKT_FRAMES * AV_AUDIO_CHANNELS * 2;

    do
    {
        err = CoCreateInstance(kCLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                              kIID_IMMDeviceEnumerator, (void **)&enumerator);

        if(FAILED(err))
        {
            errMsg = QStringLiteral("创建设备枚举器失败");
            qWarning("创建设备枚举器失败: 0x%08X", (quint32)err);
            break;
        }

        err = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if(FAILED(err))
        {
            errMsg = QStringLiteral("没有默认播放设备");
            qWarning("没有默认播放设备(回环采集需要有声卡)");
            break;
        }

        err = device->Activate(kIID_IAudioClient, CLSCTX_ALL, nullptr, (void **)&client);
        if(FAILED(err))
        {
            errMsg = QStringLiteral("激活 IAudioClient 失败");
            qWarning("激活 IAudioClient 失败: 0x%08X", (quint32)err);
            break;
        }

        err = client->GetMixFormat(&mix);
        if (FAILED(err) || mix == nullptr)
        {
            errMsg = QStringLiteral("GetMixFormat 失败");
            qWarning("GetMixFormat 失败");
            break;
        }

        REFERENCE_TIME streamLatency = 0;
        client->GetStreamLatency(&streamLatency);          /* 100ns */
        latencyMs = (int)(streamLatency / 10000LL);

        err = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                200 * 10000LL, 0, mix, nullptr);
        if (FAILED(err))
        {
            errMsg = QStringLiteral("初始化回环采集失败");
            qWarning("初始化回环采集失败: 0x%08X（换独占模式的应用会占住声卡）", (quint32)err);
            break;
        }

        err = client->GetService(kIID_IAudioCaptureClient, (void **)&capture);
        if (FAILED(err))
        {
            errMsg = QStringLiteral("取 IAudioCaptureClient 失败");
            qWarning("取 IAudioCaptureClient 失败");
            break;
        }

        /* ---- 源格式 ---- */
        const int   srcCh   = (int)mix->nChannels;
        const int   srcBits = (int)mix->wBitsPerSample;   /* 容器位数 */
        int         srcValidBits = srcBits;               /* 有效位数，默认等于容器 */
        const DWORD srcRate = mix->nSamplesPerSec;
        bool isFloat = (mix->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);

        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        {
            const WAVEFORMATEXTENSIBLE *ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix);
            if (memcmp(&ext->SubFormat, &kSubFormatFloat, sizeof(GUID)) == 0)
                isFloat = true;
            else if (memcmp(&ext->SubFormat, &kSubFormatPcm, sizeof(GUID)) == 0)
                isFloat = false;

            /* 有效位数可能小于容器位数（32 位容器装 24 位有效是最常见的一种）。
             * 驱动偶尔填 0，这时按"等于容器"处理。 */
            srcValidBits = (int)ext->Samples.wValidBitsPerSample;
            if (srcValidBits <= 0 || srcValidBits > srcBits)
                srcValidBits = srcBits;
        }

        if (isFloat && srcBits != 32)
        {
            errMsg = QStringLiteral("不支持的浮点位深");
            qWarning("不支持的浮点位深: %d",srcBits);
            break;
        }

        const qint64 ptsBiasUs =(qint64)(latencyMs / 2) * 1000LL;

        err = client->Start();
        if(FAILED(err))
        {
            errMsg = QStringLiteral("启动采集失败");
            qWarning("启动采集失败: 0x%u",(quint32)err);
            break;
        }

        audio_capturing = true;

        qWarning().noquote() << QStringLiteral("回环采集已启动: %1Hz %2声道 %3bit(有效%4) %5 → 48kHz 立体声 16bit，"
                                               "端点延迟 %6ms，PTS 补偿 %7ms")
                                .arg(srcRate).arg(srcCh)
                                .arg(srcBits)
                                .arg(srcValidBits)
                                .arg(isFloat ? QStringLiteral("float") : QStringLiteral("int"))
                                .arg(latencyMs)
                                .arg(ptsBiasUs / 1000);

        emit capture_status(QStringLiteral("%1Hz %2声道 %3bit %4 → 48kHz 立体声 16bit")
                                      .arg(srcRate).arg(srcCh).arg(srcBits)
                                      .arg(isFloat ? QStringLiteral("float") : QStringLiteral("int")));

        /* ================= 采集循环 ================= */
        const double ratio = (double)srcRate / (double)AV_AUDIO_RATE;
        double  pos = 0.0;                  /* 源位置（浮点，跨缓冲保留） */
        QByteArray acc;                     /* 输出累积（交错 int16） */
        qint64  accPtsUs = 0;
        bool    accPtsValid = false;

        while(!audio_captur_stop)
        {
            UINT32 packetFrames = 0;
            err = capture->GetNextPacketSize(&packetFrames);

            if(FAILED(err))
            {
                errMsg = QStringLiteral("采集流中断");
                qWarning().noquote() << QStringLiteral("采集流中断: 0x%1").arg((quint32)err, 8, 16, QChar('0'));
                break;
            }

            if (packetFrames == 0)
            {
                msleep(2);
                continue;
            }

            BYTE  *data = nullptr;
            UINT32 frames = 0;
            DWORD  bufFlags = 0;
            UINT64 devPos = 0, qpcPos = 0;

            err = capture->GetBuffer(&data, &frames, &bufFlags, &devPos, &qpcPos);
            if(FAILED(err))
            {
                msleep(2);
                continue;
            }

            if(frames == 0)
            {
                capture->ReleaseBuffer(0);
                continue;
            }

            const bool silent = (bufFlags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            const qint64 baseUs = (qpcPos != 0) ? avclock::qpc_to_us((qint64)qpcPos)
                                                : avclock::now_us();

            const double firstPos = pos;
            int outFrames = 0;

            for (double p = pos; p < (double)frames; p += ratio)
                outFrames++;

            if(outFrames > 0)
            {
                const bool accWasEmpty = acc.isEmpty();
                const size_t base = (size_t)acc.size();
                acc.resize((int)(base + (size_t)outFrames * AV_AUDIO_CHANNELS * 2));
                int16_t *dst = reinterpret_cast<int16_t *>(acc.data() + base);

                int idx = 0;

                for (double p = pos; p < (double)frames; p += ratio)
                {
                    size_t si = (size_t)(p + 0.5);
                    if (si >= frames)
                        si = frames - 1;

                    int16_t l = 0, r = 0;

                    if(!silent)
                    {
                        read_channel(data, si, srcCh, srcBits, isFloat, srcValidBits, l);

                        if (srcCh >= 2)
                        {
                            const BYTE *right = data + (size_t)srcBits / 8;   /* 第二声道 */
                            read_channel(right, si, srcCh, srcBits, isFloat, srcValidBits, r);
                        } else
                        {
                            r = l;
                        }

                        dst[idx * 2]     = l;
                        dst[idx * 2 + 1] = r;
                        idx++;
                    }
                    else
                    {
                        /* 静音包：l/r 已经是 0，也必须写进去并推进 idx。
                         * 原来这一段整个跳过，resize 出来的未初始化字节会被当成
                         * 音频编码发出去 —— 系统静音/放歌间隙时就是满幅噪声。 */
                        dst[idx * 2]     = 0;
                        dst[idx * 2 + 1] = 0;
                        idx++;
                    }
                }

                    pos = pos + (double)outFrames * ratio - (double)frames;   /* 进位保留 */

                    if(accWasEmpty || !accPtsValid)
                    {
                        /* 累加缓冲空了：直接以 QPC 的采集时刻为准（比按采样数累加更准） */
                        accPtsUs = baseUs + (qint64)((firstPos + 0.5) * 1000000.0 / (double)srcRate);
                        accPtsValid = true;
                    }
                }else
                {
                    pos -= (double)frames;   /* 这一批源数据一个输出点都没凑满 */
                }

                    capture->ReleaseBuffer(frames);

                    while(acc.size() >= PKT_BYTES)
                    {
                          const QByteArray pkt = acc.left(PKT_BYTES);
                          acc.remove(0, PKT_BYTES);

                          const quint32 ptsMs = (quint32)((accPtsUs + ptsBiasUs) / 1000);

                          opus_out.resize(1500);
                          const int n = opus_encode(audio_opus,
                                                    reinterpret_cast<const opus_int16 *>(pkt.constData()),
                                                    AV_AUDIO_PKT_FRAMES,          // 480 = 10ms
                                                    reinterpret_cast<unsigned char *>(opus_out.data()),
                                                    opus_out.size());
                          if (n > 0)
                          {
                             emit opus_packet_ready(opus_out.left(n),ptsMs);
                          }
                          else
                          {
                              qWarning("opus_encode error");
                          }

                          accPtsUs += (qint64)AV_AUDIO_PKT_FRAMES * 1000000 / AV_AUDIO_RATE;
                    }
        }

      if(client)client->Stop();
    }while(0);

    audio_captur_stop = false;
    if (capture)    capture->Release();
    if (client)     client->Release();
    if (mix)        CoTaskMemFree(mix);
    if (device)     device->Release();
    if (enumerator) enumerator->Release();
    if (comOk)      CoUninitialize();

    if(!errMsg.isEmpty())
     {
         emit capture_status(errMsg);
         return;
     }
}




audio::audio(QWidget *parent) : QWidget(parent)
{

}

void audio::start()
{
    if(audio_cap != NULL) return;
    audio_cap = new audio_capture_thread(this);
    connect(audio_cap,&audio_capture_thread::opus_packet_ready,this, &audio::audio_packet);
    connect(audio_cap,&audio_capture_thread::capture_status,this, &audio::signal_audio_capture_status);
    audio_cap->start();
}

void audio::stop()
{
    if(audio_cap)
    {
        audio_cap->stop();
        delete audio_cap;
        audio_cap = nullptr;
    }
}
