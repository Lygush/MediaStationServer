// UpdateTab.h
#pragma once

#include <QWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPushButton>
#include <QLabel>
#include <QProgressBar>
#include <QTextEdit>
#include <QTcpSocket>
#include <QUdpSocket>
#include <QProcess>
#include <QFile>
#include <QTimer>
#include <QElapsedTimer>
#include <QStringList>
#include <QCheckBox>

class UpdateTab : public QWidget
{
    Q_OBJECT
public:
    explicit UpdateTab(QWidget *parent = nullptr);
    ~UpdateTab();

private slots:
    void browseExe();
    void onPrepareClicked();
    void onSendClicked();
    void onCancelClicked();
    void discoverDevices();

    void startCopyStep();
    void startDeployStep();
    void deployNextExe();
    void deployFinished(int exitCode, QProcess::ExitStatus status);
    void deployOutput();

    void startPackStep();
    void packProgress();
    void packFinished(int exitCode, QProcess::ExitStatus status);

    void socketConnected();
    void socketDisconnected();
    void socketError(QAbstractSocket::SocketError err);
    void socketReadyRead();
    void socketBytesWritten(qint64 bytes);

private:
    // ── UI ────────────────────────────────────────────────────────────────────
    QLineEdit    *exeEdit;
    QLineEdit    *qmlDirEdit;
    QLineEdit    *windeployqtEdit;
    QLineEdit    *versionEdit;
    QTextEdit    *notesEdit;

    QComboBox    *ipCombo;
    QPushButton  *discoverBtn;
    QLabel       *discoverStatus;

    QProgressBar *progressPrep;
    QProgressBar *progressSend;
    QLabel       *statusLabel;
    QTextEdit    *logView;
    QPushButton  *prepareBtn;   // «Подготовить»
    QPushButton  *sendBtn;      // «Отправить» (активна только после подготовки)
    QPushButton  *cancelBtn;
    QCheckBox    *excludeToolsCheckBox;

    // ── Состояние ─────────────────────────────────────────────────────────────
    enum Step {
        StepIdle,
        StepCopy,
        StepDeploy,
        StepPack,
        StepConnecting,
        StepSending
    } m_step;

    bool        m_cancelled;
    bool        m_archiveReady;  // true после успешной подготовки
    QString     m_tempDir;
    QString     m_archivePath;
    QString     m_hash;
    qint64      m_archiveSize;
    qint64      m_bytesSent;

    QProcess    *m_deployProc;
    QProcess    *m_packProc;
    QStringList  m_deployExeQueue;

    QTcpSocket   *m_socket;
    QUdpSocket   *m_udpSocket;
    QFile        *m_archiveFile;
    QElapsedTimer m_speedTimer;
    qint64        m_lastBytes;
    qint64        m_lastMs;

    QByteArray m_incoming;

    // ── Вспомогательные методы ────────────────────────────────────────────────
    void log(const QString &msg);
    void setStatus(const QString &msg);
    void abort(const QString &reason, bool dialog = true);
    void cleanup();
    void cleanupArchive();      // удаляет только архив и temp, не трогает сокет
    void resetUI();
    void sendChunk();
    void saveSettings();
    void loadSettings();

    QString sevenZipPath() const;
    QString winDeployPath() const;
    QString computeHash(const QString &filePath) const;
    static bool copyDirRecursive(const QString &src, const QString &dst);
};
