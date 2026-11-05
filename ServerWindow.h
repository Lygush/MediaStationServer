// ServerWindow.h
#pragma once

#include <QMainWindow>
#include <QTabWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QProcess>
#include <QFile>
#include <QTimer>
#include <QListWidget>
#include <QGroupBox>
#include <QCheckBox>
#include <QElapsedTimer>
#include "LogsTab.h"   // содержит объявление LogsTab и нужные ей Qt-заголовки

class UpdateTab;


// ─────────────────────────────────────────────────────────────────────────────
// ServerWindow
// ─────────────────────────────────────────────────────────────────────────────
class ServerWindow : public QMainWindow
{
    Q_OBJECT
public:
    explicit ServerWindow(QWidget *parent = nullptr);
    ~ServerWindow();

private slots:
    void browseFolder();
    void onPrepareClicked();
    void onSendClicked();
    void onCancelClicked();
    void onFolderChanged(const QString &path);
    void discoverDevices();

    void socketConnected();
    void socketDisconnected();
    void socketError(QAbstractSocket::SocketError error);
    void bytesWritten(qint64 bytes);
    void readyRead();

    void archiveFinished(int exitCode, QProcess::ExitStatus status);
    void updatePackProgress();
    void negotiationTimeout();

private:
    QTabWidget *tabWidget;
    UpdateTab  *updateTab;
    LogsTab    *logsTab;      // ← новая вкладка

    // ── UI вкладки «Игры» ────────────────────────────────────────────────────
    QLineEdit    *folderEdit;
    QLineEdit    *nameEdit;
    QComboBox    *typeCombo;
    QComboBox    *ipCombo;
    QPushButton  *prepareButton;
    QPushButton  *sendButton;
    QPushButton  *cancelButton;
    QPushButton  *discoverButton;
    QLabel       *statusLabel;
    QProgressBar *progressPack;
    QProgressBar *progressSend;

    QGroupBox    *exeGroupBox;
    QListWidget  *exeListWidget;
    QLineEdit    *exeCustomEdit;
    QPushButton  *exeBrowseBtn;

    QGroupBox    *iconGroupBox;
    QListWidget  *iconListWidget;
    QCheckBox    *noIconCheckBox;

    // ── Сеть ─────────────────────────────────────────────────────────────────
    QTcpSocket *socket;
    QUdpSocket *udpSocket;
    QByteArray  incomingData;
    qint64      bytesSent;
    qint64      metadataSize;

    QProcess   *sevenZipProcess;
    QFile      *archiveFile;
    QString     tempArchivePath;
    qint64      archiveSize;

    QTimer *negotiationTimer;

    QElapsedTimer speedTimer;
    qint64        lastBytesSent;
    qint64        lastTimeMs;

    enum State {
        Idle, Packing, ReadyToSend, Connecting, WaitingForApproval, SendingArchive
    } state;

    bool cancelledByUser;
    bool archiveReady;

    void connectToClient();
    void sendNegotiationPacket();
    void startPacking();
    void sendPackProgressToClient(int percent);
    void startSendingArchive();
    void sendArchiveChunk();
    void cleanupResources();
    void abortAll(const QString &reason, bool showDialog = true);
    void resetUI();

    QString selectedExeRelativePath() const;
    void updateIconList(const QString &path);
    QWidget *buildGamesTab();
};
