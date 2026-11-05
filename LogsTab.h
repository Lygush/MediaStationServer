// LogsTab.h
#pragma once

#include <QWidget>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUdpSocket>          // <-- добавлено для UDP
#include <QFile>
#include <QMap>
#include <QListWidget>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QProcess>
#include <QDateTime>

class LogsTab : public QWidget
{
    Q_OBJECT
public:
    explicit LogsTab(QWidget *parent = nullptr);
    ~LogsTab();

private slots:
    void onNewConnection();
    void onReadyRead();
    void onSocketDisconnected();
    void onSessionSelected(QListWidgetItem *item);
    void onOpenFolderClicked();
    void onDeleteClicked();
    void processUdpDiscovery();   // <-- новый слот

private:
    static constexpr quint16 kPort = 12347;              // TCP для приёма архивов
    static constexpr quint16 kLogsDiscoverPort = 12348;  // UDP для обнаружения лог-сервера

    QTcpServer *m_server = nullptr;
    QUdpSocket *m_udpSocket = nullptr;        // <-- добавлено

    struct ClientState {
        enum Phase { PhaseHandshake, PhaseReceiving } phase = PhaseHandshake;
        QByteArray buffer;
        qint64     expectedSize  = 0;
        qint64     receivedBytes = 0;
        QString    extractPath;
        QFile     *archiveFile = nullptr;
        QString    clientIp;
    };
    QMap<QTcpSocket *, ClientState> m_clients;

    QListWidget  *m_sessionList  = nullptr;
    QListWidget  *m_fileList     = nullptr;
    QLabel       *m_statusLabel  = nullptr;
    QProgressBar *m_progressBar  = nullptr;
    QPushButton  *m_openBtn      = nullptr;
    QPushButton  *m_deleteBtn    = nullptr;

    QString logsRootDir() const;
    void    cleanupClient(QTcpSocket *socket);
    void    extractArchive(const QString &archivePath, const QString &extractPath,
                        const QString &clientIp, const QDateTime &dt);
    void    addSession(const QString &folderPath, const QString &clientIp, const QDateTime &dt);
    void    setStatus(const QString &msg);
};
