#pragma once

#include <QString>

class QSettings;

class ConfigManager {
public:
    ConfigManager();
    ~ConfigManager();

    bool load(const QString &configDir);
    bool save() const;
    bool exists() const;

    QString sharedDatabasePath() const;
    void setSharedDatabasePath(const QString &path);

    bool isAdmin() const;
    void setAdmin(bool admin);

    // CLI overrides (take precedence over INI values)
    void setSharedDbOverride(const QString &path);
    void setAdminOverride(bool admin);

    // RDP performance options (persisted to config.ini)
    bool rdpGfxPipeline() const;
    void setRdpGfxPipeline(bool enabled);

    bool rdpH264() const;
    void setRdpH264(bool enabled);

    bool rdpRemoteFx() const;
    void setRdpRemoteFx(bool enabled);

    bool rdpFontSmoothing() const;
    void setRdpFontSmoothing(bool enabled);

    bool rdpVerboseLog() const;
    void setRdpVerboseLog(bool enabled);

private:
    QSettings *m_settings = nullptr;
    QString m_configDir;
    QString m_sharedDbOverride;
    bool m_adminOverride = false;
    bool m_hasAdminOverride = false;
};
