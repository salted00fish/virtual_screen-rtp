#include "video.h"

#include <QBuffer>
#include <QDebug>
#include <QGuiApplication>
#include <QImageWriter>
#include <QScreen>

#ifdef Q_OS_WIN
using namespace Microsoft::WRL;

namespace {

QString hrText(HRESULT hr)
{
    return QStringLiteral("0x%1").arg(quint32(hr), 8, 16, QLatin1Char('0'));
}

// 光标位图的实际尺寸：彩色光标看 hbmColor，单色光标看 hbmMask（掩码图是上下两半）
void cursorBitmapSize(const ICONINFO &ii, int *w, int *h)
{
    BITMAP bm;
    memset(&bm, 0, sizeof(bm));

    if (ii.hbmColor != nullptr && GetObject(ii.hbmColor, sizeof(bm), &bm) == sizeof(bm)) {
        *w = bm.bmWidth;
        *h = bm.bmHeight;
        return;
    }
    if (ii.hbmMask != nullptr && GetObject(ii.hbmMask, sizeof(bm), &bm) == sizeof(bm)) {
        *w = bm.bmWidth;
        *h = bm.bmHeight / 2;      // 单色光标：掩码图是 AND 在上、XOR 在下
        return;
    }
    *w = GetSystemMetrics(SM_CXCURSOR);
    *h = GetSystemMetrics(SM_CYCURSOR);
}

} // namespace
#endif // Q_OS_WIN

ScreenCapturer::ScreenCapturer()
{
}

ScreenCapturer::~ScreenCapturer()
{
    close();
}

/*==============================================================================
 *  屏幕列表（GUI 线程用，只读元数据）
 *============================================================================*/
QScreen *ScreenCapturer::resolveScreen(int index)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    if (screens.isEmpty()) {
        return nullptr;
    }

    QScreen *primary  = QGuiApplication::primaryScreen();
    QScreen *fallback = primary ? primary : screens.first();

    if (index < 0 || index >= screens.size()) {
        return fallback;               // -1（用主屏）或下标失效（拔屏/换主屏）
    }

    QScreen *picked = screens.at(index);
    return picked ? picked : fallback;
}

int ScreenCapturer::defaultScreenIndex()
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    QScreen *primary = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        if (screens.at(i) != primary) {
            return i;                  // 有第二块屏就选它
        }
    }

    const int pi = screens.indexOf(primary);
    return (pi >= 0) ? pi : 0;
}

QStringList ScreenCapturer::screenDescriptions()
{
    QStringList list;
    const QList<QScreen *> screens = QGuiApplication::screens();
    QScreen *primary = QGuiApplication::primaryScreen();

    for (int i = 0; i < screens.size(); ++i) {
        QScreen *s = screens.at(i);
        const QRect g = s->geometry();
        const qreal dpr = s->devicePixelRatio();

        QString desc = QStringLiteral("屏幕%1: %2  %3x%4 @ (%5,%6)")
                           .arg(i + 1)
                           .arg(s->name())
                           .arg(g.width()).arg(g.height())
                           .arg(g.x()).arg(g.y());

        if (dpr != 1.0) {
            desc += QStringLiteral("  DPR=%1(物理 %2x%3)")
                        .arg(dpr)
                        .arg(qRound(g.width() * dpr))
                        .arg(qRound(g.height() * dpr));
        }
        if (s == primary) {
            desc += QStringLiteral("  [主屏]");
        }
        list << desc;
    }

    if (list.isEmpty()) {
        list << QStringLiteral("(没有检测到屏幕)");
    }
    return list;
}

/*==============================================================================
 *  采集链路（DXGI 桌面复制）
 *============================================================================*/
#ifdef Q_OS_WIN

bool ScreenCapturer::openCaptureChain(QString *err)
{
    QScreen *screen = resolveScreen(m_screenIndex);
    if (screen == nullptr) {
        if (err) *err = QStringLiteral("没有可用屏幕");
        return false;
    }

    // 目标输出在虚拟桌面里的"物理像素"矩形：DXGI 的 DesktopCoordinates 是物理像素，
    // QScreen::geometry() 在 150% 缩放时是逻辑像素，所以要乘 dpr
    const QRect g   = screen->geometry();
    const qreal dpr = screen->devicePixelRatio();
    const QRect phys(qRound(g.x() * dpr), qRound(g.y() * dpr),
                     qRound(g.width() * dpr), qRound(g.height() * dpr));

    ComPtr<IDXGIFactory1> factory;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                    reinterpret_cast<void **>(factory.GetAddressOf()));
    if (FAILED(hr)) {
        if (err) *err = QStringLiteral("CreateDXGIFactory1 失败(%1)").arg(hrText(hr));
        return false;
    }

    // ---- 1. 到所有适配器里找这块屏对应的输出 ----
    //      多显卡机器上输出不一定挂在默认适配器，EnumOutputs 的顺序也不等于 Qt 的屏幕下标，
    //      所以按 DesktopCoordinates 的原点匹配最可靠
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput>   output;
    RECT foundRect = { 0, 0, 0, 0 };

    for (UINT ai = 0; factory->EnumAdapters1(ai, &adapter) != DXGI_ERROR_NOT_FOUND; ++ai) {
        for (UINT oi = 0; ; ++oi) {
            ComPtr<IDXGIOutput> out;
            if (adapter->EnumOutputs(oi, &out) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_OUTPUT_DESC od;
            memset(&od, 0, sizeof(od));
            if (SUCCEEDED(out->GetDesc(&od)) &&
                od.DesktopCoordinates.left == phys.x() && od.DesktopCoordinates.top == phys.y()) {
                output = out;
                foundRect = od.DesktopCoordinates;
                break;
            }
        }
        if (output) {
            break;                      // adapter 就是命中的那块
        }
    }

    // ---- 2. 兜底：坐标没匹配上（高 DPI / 奇怪拓扑）就在默认适配器上按下标取 ----
    if (!output) {
        qWarning() << "ScreenCapturer: 没找到坐标匹配的输出" << phys
                   << "，回退到默认适配器按下标取";

        ComPtr<IDXGIDevice>  tmpDev;
        ComPtr<IDXGIAdapter> tmpAdapter;
        m_device.Reset();
        m_context.Reset();

        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0,
                                     D3D11_SDK_VERSION, &m_device, nullptr, &m_context)) ||
            FAILED(m_device.As(&tmpDev)) ||
            FAILED(tmpDev->GetAdapter(&tmpAdapter))) {
            if (err) *err = QStringLiteral("创建默认 D3D11 设备/适配器失败");
            return false;
        }

        const int idx = (m_screenIndex < 0) ? 0 : m_screenIndex;
        if (FAILED(tmpAdapter->EnumOutputs(UINT(idx), &output)) || !output) {
            if (err) *err = QStringLiteral("EnumOutputs(%1) 失败：这块屏可能不支持桌面复制").arg(idx);
            return false;
        }

        DXGI_ADAPTER_DESC ad;
        memset(&ad, 0, sizeof(ad));
        tmpAdapter->GetDesc(&ad);
        m_deviceAdapterLuid = ad.AdapterLuid;
        adapter.Reset();

        DXGI_OUTPUT_DESC od;
        memset(&od, 0, sizeof(od));
        output->GetDesc(&od);
        foundRect = od.DesktopCoordinates;
    } else {
        // ---- 2b. 在命中输出的那块适配器上准备 D3D11 设备（已经是它了就复用）----
        DXGI_ADAPTER_DESC ad;
        memset(&ad, 0, sizeof(ad));
        adapter->GetDesc(&ad);

        const bool sameAdapter = m_device &&
                                 m_deviceAdapterLuid.LowPart  == ad.AdapterLuid.LowPart &&
                                 m_deviceAdapterLuid.HighPart == ad.AdapterLuid.HighPart;
        if (!sameAdapter) {
            m_device.Reset();
            m_context.Reset();
            m_duplication.Reset();
            m_staging.Reset();

            hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &m_device, nullptr, &m_context);
            if (FAILED(hr)) {
                if (err) *err = QStringLiteral("在适配器上创建 D3D11 设备失败(%1)").arg(hrText(hr));
                return false;
            }
            m_deviceAdapterLuid = ad.AdapterLuid;
            qInfo().noquote() << QStringLiteral("ScreenCapturer: D3D11 设备建在 [%1]")
                                     .arg(QString::fromWCharArray(ad.Description));
        }
    }

    // ---- 3. 建桌面复制 ----
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output.As(&output1))) {
        if (err) *err = QStringLiteral("IDXGIOutput1 转换失败（系统太老？）");
        return false;
    }

    hr = output1->DuplicateOutput(m_device.Get(), &m_duplication);
    if (FAILED(hr)) {
        if (err) {
            *err = QStringLiteral("DuplicateOutput 失败(%1)：可能被别的捕获程序独占、"
                                  "当前是锁屏/安全桌面，或该输出属于另一块显卡").arg(hrText(hr));
        }
        return false;
    }

    // 刚建好桌面复制后的第一帧经常是全黑的（DXGI 的已知行为），丢掉它别送给下位机
    m_skipFrames = 1;

    // ---- 4. 按输出分辨率建 staging 纹理 ----
    DXGI_OUTDUPL_DESC dd;
    memset(&dd, 0, sizeof(dd));
    m_duplication->GetDesc(&dd);
    if (dd.ModeDesc.Width == 0 || dd.ModeDesc.Height == 0) {
        if (err) *err = QStringLiteral("桌面复制的输出分辨率为 0");
        close();
        return false;
    }
    if (!createStaging(dd.ModeDesc.Width, dd.ModeDesc.Height, dd.ModeDesc.Format, err)) {
        close();
        return false;
    }

    m_outputRect = QRect(int(foundRect.left), int(foundRect.top),
                         int(foundRect.right - foundRect.left),
                         int(foundRect.bottom - foundRect.top));

    qInfo().noquote() << QStringLiteral("ScreenCapturer: GPU 捕获已就绪 —— %1，输出 %2x%3，"
                                        "虚拟桌面位置 (%4,%5)")
                             .arg(screen->name())
                             .arg(m_width).arg(m_height)
                             .arg(m_outputRect.x()).arg(m_outputRect.y());
    return true;
}

bool ScreenCapturer::createStaging(UINT width, UINT height, DXGI_FORMAT format, QString *err)
{
    D3D11_TEXTURE2D_DESC desc;
    memset(&desc, 0, sizeof(desc));
    desc.Width            = width;
    desc.Height           = height;
    desc.Format           = format;          // 通常 DXGI_FORMAT_B8G8R8A8_UNORM
    desc.MipLevels        = 1;
    desc.ArraySize        = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage            = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags   = D3D11_CPU_ACCESS_READ;
    desc.BindFlags        = 0;

    m_staging.Reset();
    const HRESULT hr = m_device->CreateTexture2D(&desc, nullptr, &m_staging);
    if (FAILED(hr)) {
        if (err) *err = QStringLiteral("创建 staging 纹理失败(%1)").arg(hrText(hr));
        return false;
    }

    m_width  = width;
    m_height = height;
    return true;
}

bool ScreenCapturer::open(QString *err)
{
    close();                             // 先清掉旧的复制对象（设备按需复用）
    return openCaptureChain(err);
}

void ScreenCapturer::close()
{
    m_staging.Reset();
    m_duplication.Reset();
}

void ScreenCapturer::releaseAll()
{
    close();
#ifdef Q_OS_WIN
    m_device.Reset();
    m_context.Reset();
    m_deviceAdapterLuid = LUID();
    m_width  = 0;
    m_height = 0;
    m_skipFrames = 0;
    m_outputRect = QRect();
#endif
}

bool ScreenCapturer::isOpen() const
{
    return m_duplication != nullptr && m_staging != nullptr;
}

bool ScreenCapturer::grab(QImage *out, QString *err)
{
    if (err) {
        err->clear();
    }
    if (out == nullptr) {
        return false;
    }
    if (!isOpen()) {
        if (err) *err = QStringLiteral("采集链路未初始化");
        return false;
    }

    DXGI_OUTDUPL_FRAME_INFO info;
    memset(&info, 0, sizeof(info));
    ComPtr<IDXGIResource> resource;

    // 超时 0：没有新画面就立刻返回，不阻塞采集线程
    HRESULT hr = m_duplication->AcquireNextFrame(0, &info, &resource);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        return false;                    // 画面没动（err 为空，不是错误）
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        // 锁屏 / 改分辨率 / 改刷新率 / 独占全屏 / 切换用户：复制对象永久失效，必须重建
        if (err) *err = QStringLiteral("桌面复制失效(DXGI_ERROR_ACCESS_LOST)");
        close();
        return false;
    }
    if (FAILED(hr)) {
        if (err) *err = QStringLiteral("AcquireNextFrame 失败(%1)").arg(hrText(hr));
        close();
        return false;
    }

    // 刚建好/刚重建完复制对象时，头一帧往往是空白的，直接丢掉（err 留空 = 当"没有新帧"）
    if (m_skipFrames > 0) {
        --m_skipFrames;
        m_duplication->ReleaseFrame();
        return false;
    }

    // 输出分辨率变了：staging 尺寸对不上时 CopyResource 会静默失败，所以自己判
    DXGI_OUTDUPL_DESC dd;
    memset(&dd, 0, sizeof(dd));
    m_duplication->GetDesc(&dd);
    if (dd.ModeDesc.Width != m_width || dd.ModeDesc.Height != m_height) {
        if (err) {
            *err = QStringLiteral("输出分辨率变化 %1x%2 → %3x%4，需要重建采集链路")
                       .arg(m_width).arg(m_height)
                       .arg(dd.ModeDesc.Width).arg(dd.ModeDesc.Height);
        }
        m_duplication->ReleaseFrame();
        close();
        return false;
    }

    ComPtr<ID3D11Texture2D> gpuTexture;
    resource.As(&gpuTexture);
    if (!gpuTexture) {
        m_duplication->ReleaseFrame();
        if (err) *err = QStringLiteral("取 ID3D11Texture2D 失败");
        return false;
    }

    // 尺寸/格式再核一遍（CopyResource 不返回错误，判不到就会画面卡住还没提示）
    D3D11_TEXTURE2D_DESC srcDesc;
    D3D11_TEXTURE2D_DESC stgDesc;
    memset(&srcDesc, 0, sizeof(srcDesc));
    memset(&stgDesc, 0, sizeof(stgDesc));
    gpuTexture->GetDesc(&srcDesc);
    m_staging->GetDesc(&stgDesc);
    if (srcDesc.Width != stgDesc.Width || srcDesc.Height != stgDesc.Height ||
        srcDesc.Format != stgDesc.Format) {
        m_duplication->ReleaseFrame();
        if (err) *err = QStringLiteral("帧尺寸/格式变化，需要重建采集链路");
        close();
        return false;
    }

    // GPU → staging（CPU 可读）
    m_context->CopyResource(m_staging.Get(), gpuTexture.Get());
    m_duplication->ReleaseFrame();

    D3D11_MAPPED_SUBRESOURCE mapped;
    memset(&mapped, 0, sizeof(mapped));
    hr = m_context->Map(m_staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(hr)) {
        if (err) *err = QStringLiteral("Map staging 纹理失败(%1)").arg(hrText(hr));
        return false;
    }

    // 零拷贝包一层 QImage：DXGI 默认 BGRA，正好是 Format_RGB32 的内存布局
    const QImage wrapped(reinterpret_cast<const uchar *>(mapped.pData),
                         int(m_width), int(m_height), int(mapped.RowPitch),
                         QImage::Format_RGB32);

    // 缩放成 800x480（平滑插值，文字在 LCD 上还能看）；scaled() 会拷一份，之后就能解映射
    QImage scaled = wrapped.scaled(targetSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);

    m_context->Unmap(m_staging.Get(), 0);

    if (scaled.isNull()) {
        if (err) *err = QStringLiteral("缩放到 800x480 失败");
        return false;
    }

    if (m_drawCursor) {
        overlayCursor(&scaled);
    }

    *out = scaled.convertToFormat(QImage::Format_RGB888);
    return !out->isNull();
}

void ScreenCapturer::overlayCursor(QImage *img)
{
    if (img == nullptr || img->isNull() || m_outputRect.isEmpty()) {
        return;
    }

    CURSORINFO ci;
    memset(&ci, 0, sizeof(ci));
    ci.cbSize = sizeof(ci);
    if (!GetCursorInfo(&ci) || (ci.flags & CURSOR_SHOWING) == 0 || ci.hCursor == nullptr) {
        return;                          // 光标没显示
    }

    ICONINFO ii;
    memset(&ii, 0, sizeof(ii));
    if (!GetIconInfo(ci.hCursor, &ii)) {
        return;
    }

    int curW = 0, curH = 0;
    cursorBitmapSize(ii, &curW, &curH);

    // 物理像素 → 画面像素 的比例（1920 → 800 就是 0.4167）
    const double k = double(img->width()) / double(m_outputRect.width());
    const double s = (m_cursorScale < 1.0) ? 1.0 : m_cursorScale;

    // 光标热点（箭头尖端）在画面里的位置；放大时光标位图左上角要往回退 (s-1) 个热点
    const int tipX = qRound((ci.ptScreenPos.x - m_outputRect.x()) * k);
    const int tipY = qRound((ci.ptScreenPos.y - m_outputRect.y()) * k);
    const int hotX = qRound(ii.xHotspot * k);
    const int hotY = qRound(ii.yHotspot * k);

    const int drawW = qMax(1, qRound(curW * k * s));
    const int drawH = qMax(1, qRound(curH * k * s));
    const int drawX = tipX - hotX - qRound(hotX * (s - 1.0));
    const int drawY = tipY - hotY - qRound(hotY * (s - 1.0));

    const QRect dst(drawX, drawY, drawW, drawH);
    const QRect clipped = dst.intersected(img->rect());
    if (clipped.isEmpty()) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;                          // 光标不在这块屏上
    }

    // 画面转成 32bpp 才好和 DIB 互拷（每行正好 width*4 字节）。
    // 注意：QImage 是隐式共享的，*img 本来就是 RGB32 时 convertToFormat() 只是浅拷贝，
    //       后面 scanLine() 写第一笔时会自动 detach，所以画完必须把 canvas 赋回 *img。
    QImage canvas = img->convertToFormat(QImage::Format_RGB32);
    if (canvas.isNull()) {
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;
    }

    // 1) 把光标要盖住的那块背景拷进一块 32bpp DIB
    BITMAPINFO bmi;
    memset(&bmi, 0, sizeof(bmi));
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = clipped.width();
    bmi.bmiHeader.biHeight      = -clipped.height();   // 负数 = 自上而下
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    void *bits = nullptr;
    HDC dc = CreateCompatibleDC(nullptr);
    HBITMAP dib = dc ? CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0) : nullptr;
    if (dc == nullptr || dib == nullptr || bits == nullptr) {
        if (dib) DeleteObject(dib);
        if (dc)  DeleteDC(dc);
        if (ii.hbmColor) DeleteObject(ii.hbmColor);
        if (ii.hbmMask)  DeleteObject(ii.hbmMask);
        return;
    }

    HGDIOBJ oldBmp = SelectObject(dc, dib);
    const int rowBytes = clipped.width() * 4;
    for (int row = 0; row < clipped.height(); ++row) {
        memcpy(static_cast<uchar *>(bits) + row * rowBytes,
               canvas.constScanLine(clipped.y() + row) + clipped.x() * 4,
               size_t(rowBytes));
    }

    // 2) 把光标画在这块背景上（alpha 混合 / 单色掩码交给 GDI）
    DrawIconEx(dc, dst.x() - clipped.x(), dst.y() - clipped.y(),
               ci.hCursor, drawW, drawH, 0, nullptr, DI_NORMAL);

    // 3) 贴回画面
    for (int row = 0; row < clipped.height(); ++row) {
        memcpy(canvas.scanLine(clipped.y() + row) + clipped.x() * 4,
               static_cast<uchar *>(bits) + row * rowBytes,
               size_t(rowBytes));
    }

    // 必须赋回：canvas 有可能是 detach 出来的独立副本（*img 本来就是 RGB32 时也是），
    // 不赋回就等于"光标画在了副本上然后扔掉"，画面里根本看不到鼠标。
    *img = canvas;

    SelectObject(dc, oldBmp);
    DeleteObject(dib);
    DeleteDC(dc);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
}

#else  /* !Q_OS_WIN */

bool ScreenCapturer::open(QString *err)
{
    if (err) *err = QStringLiteral("GPU 捕获（DXGI 桌面复制）只在 Windows 上实现");
    return false;
}

void ScreenCapturer::close() {}
void ScreenCapturer::releaseAll() {}
bool ScreenCapturer::isOpen() const { return false; }

bool ScreenCapturer::grab(QImage *out, QString *err)
{
    Q_UNUSED(out);
    if (err) *err = QStringLiteral("GPU 捕获（DXGI 桌面复制）只在 Windows 上实现");
    return false;
}

#endif /* Q_OS_WIN */

bool ScreenCapturer::grabJpeg(QByteArray *jpeg, int quality, QString *err)
{
    if (jpeg == nullptr) {
        return false;
    }

    QImage frame;
    if (!grab(&frame, err)) {
        return false;
    }

    *jpeg = encodeJpeg(frame, quality);
    if (jpeg->isEmpty()) {
        if (err) *err = QStringLiteral("JPEG 编码失败");
        return false;
    }
    return true;
}

QByteArray ScreenCapturer::encodeJpeg(const QImage &image, int quality)
{
    if (image.isNull()) {
        qWarning("ScreenCapturer: 待编码图像为空");
        return QByteArray();
    }

    if (quality < 1) {
        quality = 1;
    } else if (quality > 100) {
        quality = 100;
    }

    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    if (!buffer.open(QIODevice::WriteOnly)) {
        qWarning("ScreenCapturer: QBuffer 打开失败，无法编码 JPEG");
        return QByteArray();
    }

    QImageWriter writer(&buffer, "jpeg");
    writer.setQuality(quality);
    if (!writer.write(image)) {
        qWarning() << "ScreenCapturer: QImageWriter 编码 JPEG 失败:" << writer.errorString()
                   << "，改用 QImage::save 重试";
        buffer.close();
        jpeg.clear();
        if (!buffer.open(QIODevice::WriteOnly)) {
            return QByteArray();
        }
        if (!image.save(&buffer, "JPEG", quality)) {
            qWarning("ScreenCapturer: QImage::save 编码 JPEG 也失败，"
                     "请确认 imageformats/qjpeg.dll 插件存在");
            return QByteArray();
        }
    }
    buffer.close();

    return jpeg;
}
