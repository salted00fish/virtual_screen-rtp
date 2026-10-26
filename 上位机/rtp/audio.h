#ifndef AUDIO_H
#define AUDIO_H

#include <QObject>
#include <QWidget>
#include <QThread>
#include "opus.h"

#define AV_AUDIO_RATE       48000
#define AV_AUDIO_CHANNELS   2
#define AV_AUDIO_PKT_FRAMES 480              /* 10ms */

class audio_capture_thread : public QThread
{
    Q_OBJECT
public:
    explicit audio_capture_thread(QWidget *parent = nullptr);
    ~audio_capture_thread() override;

    void stop(void);

protected:
    void run(void) override;

private:
    bool audio_capturing;
    bool audio_captur_stop;
    int latencyMs = 0;

    OpusEncoder *audio_opus = nullptr;
    int opus_err;
    QByteArray opus_out;
signals:
    void opus_packet_ready(QByteArray pcm, quint32 ptsMs);
    void capture_status(const QString &msg);
};

class audio : public QWidget
{
    Q_OBJECT
public:
    explicit audio(QWidget *parent = nullptr);

    void start(void);
    void stop(void);

private:
    audio_capture_thread *audio_cap = nullptr;
signals:
    void audio_packet(QByteArray pcm, quint32 ptsMs);
    void signal_audio_capture_status(const QString &msg);
};

#endif // AUDIO_H
