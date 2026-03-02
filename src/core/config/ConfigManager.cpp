#include "ConfigManager.h"

#include <QDir>
#include <QFileInfo>
#include <QSettings>

static const QString KEY_SHARED_DB = QStringLiteral("shared_database");
static const QString KEY_ADMIN = QStringLiteral("admin");
static const QString KEY_RDP_GFX = QStringLiteral("rdp/gfx_pipeline");
static const QString KEY_RDP_H264 = QStringLiteral("rdp/h264");
static const QString KEY_RDP_RFX = QStringLiteral("rdp/remotefx");
static const QString KEY_RDP_FONT_SMOOTHING = QStringLiteral("rdp/font_smoothing");
static const QString KEY_RDP_VERBOSE_LOG = QStringLiteral("rdp/verbose_log");

ConfigManager::ConfigManager() = default;

ConfigManager::~ConfigManager()
{
    delete m_settings;
}

bool ConfigManager::load(const QString &configDir)
{
    m_configDir = configDir;
    QString iniPath = configDir + QStringLiteral("/config.ini");

    delete m_settings;
    m_settings = new QSettings(iniPath, QSettings::IniFormat);
    return true;
}

bool ConfigManager::save() const
{
    if (!m_settings)
        return false;
    m_settings->sync();
    return m_settings->status() == QSettings::NoError;
}

bool ConfigManager::exists() const
{
    if (m_configDir.isEmpty())
        return false;
    return QFileInfo::exists(m_configDir + QStringLiteral("/config.ini"));
}

QString ConfigManager::sharedDatabasePath() const
{
    if (!m_sharedDbOverride.isEmpty())
        return m_sharedDbOverride;
    if (m_settings)
        return m_settings->value(KEY_SHARED_DB).toString();
    return {};
}

void ConfigManager::setSharedDatabasePath(const QString &path)
{
    if (m_settings)
        m_settings->setValue(KEY_SHARED_DB, path);
}

bool ConfigManager::isAdmin() const
{
    if (m_hasAdminOverride)
        return m_adminOverride;
    if (m_settings)
        return m_settings->value(KEY_ADMIN, false).toBool();
    return false;
}

void ConfigManager::setAdmin(bool admin)
{
    if (m_settings)
        m_settings->setValue(KEY_ADMIN, admin);
}

void ConfigManager::setSharedDbOverride(const QString &path)
{
    m_sharedDbOverride = path;
}

void ConfigManager::setAdminOverride(bool admin)
{
    m_adminOverride = admin;
    m_hasAdminOverride = true;
}

bool ConfigManager::rdpGfxPipeline() const
{
    if (m_settings)
        return m_settings->value(KEY_RDP_GFX, false).toBool();
    return false;
}

void ConfigManager::setRdpGfxPipeline(bool enabled)
{
    if (m_settings)
        m_settings->setValue(KEY_RDP_GFX, enabled);
}

bool ConfigManager::rdpH264() const
{
    if (m_settings)
        return m_settings->value(KEY_RDP_H264, false).toBool();
    return false;
}

void ConfigManager::setRdpH264(bool enabled)
{
    if (m_settings)
        m_settings->setValue(KEY_RDP_H264, enabled);
}

bool ConfigManager::rdpRemoteFx() const
{
    if (m_settings)
        return m_settings->value(KEY_RDP_RFX, true).toBool();
    return true;
}

void ConfigManager::setRdpRemoteFx(bool enabled)
{
    if (m_settings)
        m_settings->setValue(KEY_RDP_RFX, enabled);
}

bool ConfigManager::rdpFontSmoothing() const
{
    if (m_settings)
        return m_settings->value(KEY_RDP_FONT_SMOOTHING, true).toBool();
    return true;
}

void ConfigManager::setRdpFontSmoothing(bool enabled)
{
    if (m_settings)
        m_settings->setValue(KEY_RDP_FONT_SMOOTHING, enabled);
}

bool ConfigManager::rdpVerboseLog() const
{
    if (m_settings)
        return m_settings->value(KEY_RDP_VERBOSE_LOG, false).toBool();
    return false;
}

void ConfigManager::setRdpVerboseLog(bool enabled)
{
    if (m_settings)
        m_settings->setValue(KEY_RDP_VERBOSE_LOG, enabled);
}
