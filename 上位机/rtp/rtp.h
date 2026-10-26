#ifndef RTP_H
#define RTP_H

#include <QWidget>
#include <QByteArray>
#include <QList>
#include <functional>

/*==============================================================================
 *
 *  一个 UDP 包 == 一个 RTP 包 == 12 字节固定头 + JPEG 载荷（无扩展、无 padding）。
 *
 *   0                   1                   2                   3
 *   0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1 2 3 4 5 6 7 8 9 0 1
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |V=2|P|X|  CC   |M|     PT=26   |       sequence number         |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                           timestamp                           |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                             SSRC                              |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *  |                        JPEG payload ...                       |
 *  +-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+-+
 *
 *  字段说明（全部为大端 / 网络字节序）：
 *    V  = 2     版本号，固定 2
 *    P  = 0     无 padding
 *    X  = 0     无扩展头
 *    CC = 0     无 CSRC
 *    M  = 0/1   标记位：同一帧的最后一个分片为 1，其余分片为 0（下位机靠它判定一帧结束）
 *    PT = 26    载荷类型，固定 26（JPEG）
 *    sequence number  每发出一个 RTP 包 +1，uint16 自然回绕（0xFFFF -> 0x0000）
 *    timestamp        同一帧的所有分片取值相同；每开始新的一帧 timestamp 递增一次
 *    SSRC             固定常量 0x12345678
 *============================================================================*/

#define VIDEO_SSRC  (0x12345678u)
#define AUDIO_SSRC  (0x12345789u)

#pragma pack(1)
typedef struct
{
    uint8_t  VPXCC;           // V(2) + P(1) + X(1) + CC(4)
    uint8_t  MPT;             // M(1) + PT(7)
    uint16_t sequence_number;
    uint32_t timestamp;
    uint32_t ssrc;
}rtp_head_t;
#pragma pack()


class rtp : public QWidget
{
    Q_OBJECT
public:
    explicit rtp(QWidget *parent = nullptr);


    void rtp_video_send_init(std::function<void(const QList<QByteArray> &) > fn) { rtp_video_send = std::move(fn); }
    void rtp_audio_send_init(std::function<void(const QList<QByteArray> &) > fn) { rtp_audio_send = std::move(fn); }

    int send_jpeg_frame(const QByteArray &jpeg,int maxPayload = 1400);
    int send_audio_frame(const QByteArray &jpeg,quint32 ptsMs);

    void send_rtp_Packet(const QByteArray &payloadData, bool isMarker, uint16_t seqNum, uint32_t timestamp, uint32_t ssrc);

private:
    QByteArray audio_fec_code(const QList<QByteArray> pack);
    QList<QByteArray> fec_group;

private:
    std::function<void(const QList<QByteArray> &)> rtp_video_send;
    std::function<void(const QList<QByteArray> &)> rtp_audio_send;

    QList<QByteArray> video_batch;
    QList<QByteArray> audio_batch;

    uint16_t video_seqNum   = 0;
    uint32_t video_timestamp = 0;

    uint16_t audio_seqNum   = 0;
    uint32_t audio_timestamp = 0;
};

#endif // RTP_H
