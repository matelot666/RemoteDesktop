#include "SessionInfoDialog.h"
#include "core/rdp/RdpSession.h"

#include <QVBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QLabel>
#include <QDialogButtonBox>

static QString boolStr(bool v) { return v ? QStringLiteral("Yes") : QStringLiteral("No"); }

static QGroupBox *makeSection(const QString &title,
                              const QVector<QPair<QString, QString>> &rows)
{
    auto *box = new QGroupBox(title);
    auto *form = new QFormLayout(box);
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    for (const auto &row : rows) {
        auto *label = new QLabel(row.second);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(row.first + QStringLiteral(":"), label);
    }
    return box;
}

SessionInfoDialog::SessionInfoDialog(RdpSession *session, QWidget *parent)
    : QDialog(parent)
{
    setWindowTitle(QStringLiteral("RDP Session Info"));
    setMinimumWidth(420);

    auto info = session->sessionInfo();

    auto *layout = new QVBoxLayout(this);

    // Security string
    QStringList secParts;
    if (info.nlaSecurity) secParts << QStringLiteral("NLA");
    if (info.tlsSecurity) secParts << QStringLiteral("TLS");
    if (info.rdpSecurity) secParts << QStringLiteral("RDP");
    QString secStr = secParts.isEmpty() ? QStringLiteral("Unknown") : secParts.join(QStringLiteral(" + "));

    // Connection
    layout->addWidget(makeSection(QStringLiteral("Connection"), {
        {QStringLiteral("Server"), QStringLiteral("%1:%2").arg(info.hostname).arg(info.port)},
        {QStringLiteral("Username"), info.username.isEmpty() ? QStringLiteral("(none)") : info.username},
        {QStringLiteral("Security"), secStr},
    }));

    // Display
    layout->addWidget(makeSection(QStringLiteral("Display"), {
        {QStringLiteral("Resolution"), QStringLiteral("%1 x %2").arg(info.width).arg(info.height)},
        {QStringLiteral("Color Depth"), QStringLiteral("%1-bit").arg(info.colorDepth)},
        {QStringLiteral("Pixel Format"), QStringLiteral("BGRA32")},
    }));

    // Codecs
    layout->addWidget(makeSection(QStringLiteral("Codecs"), {
        {QStringLiteral("RemoteFX"), boolStr(info.remoteFx)},
        {QStringLiteral("NSCodec"), boolStr(info.nsCodec)},
        {QStringLiteral("GFX Pipeline"), boolStr(info.gfxPipeline)},
        {QStringLiteral("GFX Progressive"), boolStr(info.gfxProgressive)},
        {QStringLiteral("H.264/AVC 444"), boolStr(info.gfxAvc444)},
        {QStringLiteral("H.264/AVC"), boolStr(info.gfxH264)},
    }));

    // Performance
    layout->addWidget(makeSection(QStringLiteral("Performance"), {
        {QStringLiteral("Compression"), boolStr(info.compression)},
        {QStringLiteral("Fast Path Input"), boolStr(info.fastPathInput)},
        {QStringLiteral("Fast Path Output"), boolStr(info.fastPathOutput)},
        {QStringLiteral("Frame Markers"), boolStr(info.frameMarkers)},
        {QStringLiteral("Network Auto-Detect"), boolStr(info.networkAutoDetect)},
        {QStringLiteral("Bitmap Cache"), boolStr(info.bitmapCache)},
        {QStringLiteral("Font Smoothing"), boolStr(info.fontSmoothing)},
        {QStringLiteral("Desktop Composition"), boolStr(info.desktopComposition)},
    }));

    // Channels
    layout->addWidget(makeSection(QStringLiteral("Channels"), {
        {QStringLiteral("Clipboard (cliprdr)"), boolStr(info.clipboardActive)},
        {QStringLiteral("Graphics Pipeline (rdpgfx)"), boolStr(info.gfxChannelActive)},
        {QStringLiteral("Dynamic Virtual Channels (drdynvc)"), boolStr(info.drdynvcActive)},
    }));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
