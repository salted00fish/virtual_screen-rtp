#include "rtp.h"
#include <QtEndian>
#include "avclock.h"
#include <QThread>
#include <QDebug>

rtp::rtp(QWidget *parent) : QWidget(parent)
{
    avclock::av_clock_init();
}

void rtp::send_rtp_Packet(const QByteArray &payloadData,bool isMarker,uint16_t seqNum,uint32_t timestamp,uint32_t ssrc)
{
    uint8_t payloadType = 0;
    QByteArray rtpPacket = {};
    rtp_head_t rtp_head = {};
    rtpPacket.resize(sizeof(rtp_head_t) + payloadData.size());

    uint8_t version = 2; // V=2 (占2位)
    uint8_t padding = 0; // P=0 (占1位)
    uint8_t ext     = 0; // X=0 (占1位)
    uint8_t cc      = 0; // CC=0 (占4位)

    if(ssrc == VIDEO_SSRC)
         payloadType = 26;
    else if(ssrc == AUDIO_SSRC)
         payloadType = 96;

    rtp_head.VPXCC = (version << 6) | (padding << 5) | (ext << 4) | (cc & 0x0F);
    rtp_head.MPT = (isMarker ? 0x80 : 0x00) | (payloadType & 0x7F);
    rtp_head.sequence_number = qToBigEndian<uint16_t>(seqNum);
    rtp_head.timestamp       = qToBigEndian<uint32_t>(timestamp);
    rtp_head.ssrc            = qToBigEndian<uint32_t>(ssrc);

    char *dest = rtpPacket.data();
    memcpy(dest, &rtp_head, sizeof(rtp_head_t));
    memcpy(dest + sizeof(rtp_head_t), payloadData.constData(), payloadData.size());


    if (ssrc == VIDEO_SSRC)
        video_batch.append(rtpPacket);
    else if (ssrc == AUDIO_SSRC)
        audio_batch.append(rtpPacket);
}



int rtp::send_jpeg_frame(const QByteArray &jpeg, int maxPayload)
{
    if (jpeg.isEmpty())
    {
        qWarning("rtp::send_jpeg_frame: JPEG 数据为空，本帧不发送");
        return 0;
    }

    if (maxPayload <= 0)
        maxPayload = 1400;

    video_timestamp = avclock::now_ms();
    uint32_t ssrc = VIDEO_SSRC;

    const int total = jpeg.size();
    const char *rawJpeg = jpeg.constData();
    int offset = 0;
    int packets = 0;

    while (offset < total)
    {
        const int len = qMin(maxPayload, total - offset);
        const bool isMarker = ((offset + len )>= total)? 1 : 0;

        send_rtp_Packet(QByteArray::fromRawData(rawJpeg + offset, len),
                        isMarker, video_seqNum, video_timestamp, ssrc);

        video_seqNum = static_cast<uint16_t>(video_seqNum + 1);
        offset += len;
        packets++;
    }

    /* 一帧的包一次性投递（整帧只跨线程一次），网络线程里再逐个 writeDatagram */
    if (!video_batch.isEmpty() && rtp_video_send)
    {
        QList<QByteArray> batch;
        batch.swap(video_batch);
        rtp_video_send(batch);
    }

    return packets;
}

int rtp::send_audio_frame(const QByteArray &opus_buf, quint32 ptsMs)
{
    static uint8_t fec_code_cnt = 0;
    static uint16_t fec_seq = 0;

    if (opus_buf.isEmpty())
    {
        qWarning("opus_buf 数据为空，本帧不发送");
        return 0;
    }

    audio_timestamp = ptsMs;
    const uint32_t ssrc = AUDIO_SSRC;

    const int total = opus_buf.size();
    const char *rawJpeg = opus_buf.constData();
    int offset = 0;
    int packets = 0;

    while (offset < total)
    {
        const int len = qMin(1400, total - offset);
        const bool isMarker = ((offset + len )>= total)? 1 : 0;

        send_rtp_Packet(QByteArray::fromRawData(rawJpeg+offset,len),
                        isMarker,audio_seqNum,audio_timestamp,ssrc);

        if(fec_code_cnt == 0)
        {
            fec_seq = audio_seqNum;
        }

        audio_seqNum = static_cast<uint16_t>(audio_seqNum + 1);
        offset += len;
        packets++;
    }

    if (!audio_batch.isEmpty() && rtp_audio_send)
    {
        fec_group.append(audio_batch);

        QList<QByteArray> batch;
        batch.swap(audio_batch);
        rtp_audio_send(batch);
        fec_code_cnt++;
    }

    if(fec_code_cnt == 3)
    {

        QByteArray len_tab;
        for (int i = 0; i < fec_group.length(); i++)
        {
            uint16_t len = static_cast<uint16_t>(fec_group[i].length());
            len_tab.append(char((len >> 8)& 0xFF));
            len_tab.append(char(len & 0xFF));
        }

        QByteArray fec_data;

        fec_data.append("FEC", 3);
        fec_data.append(char((fec_seq >> 8) & 0xFF));
        fec_data.append(char(fec_seq & 0xFF));
        fec_data.append(len_tab);

        fec_data.append(audio_fec_code(fec_group));

        QList<QByteArray> fec_batch;
        fec_batch.append(fec_data);
        rtp_audio_send(fec_batch);

        fec_code_cnt = 0;
        fec_batch.clear();
        fec_group.clear();
    }

    return packets;
}

QByteArray rtp::audio_fec_code(const QList<QByteArray> pack)
{
    int maxlen = 0;
    for(int i = 0;i < pack.length();i++)
    {
        if(pack[i].length() > maxlen)
        {
            maxlen = pack[i].length();
        }
    }

    QByteArray result(maxlen, 0);

    for(int i=0;i<pack.length();i++)
    {
        for (int j = 0; j < pack[i].length(); j++) // 遍历这个包的每个字节
        {
            result[j] = result[j] ^ pack[i][j];
        }
    }

    return result;
}
