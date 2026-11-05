// LogsTab.cpp
// Добавить в MediaStationServer.pro:
//   HEADERS += LogsTab.h
//   SOURCES += LogsTab.cpp

#include "LogsTab.h"
#include <QStandardPaths>
#include <QFileInfo>

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QSplitter>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFileInfo>
#include <QDateTime>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QCoreApplication>
#include <QHostAddress>
#include <QProcess>
#include <QDebug>

LogsTab::LogsTab(QWidget *parent)
    : QWidget(parent)
{
    QVBoxLayout *root = new QVBoxLayout(this);
    root->setSpacing(8);

    // ── Статус + прогресс ─────────────────────────────────────────────────────
    QHBoxLayout *topRow = new QHBoxLayout;
    m_statusLabel = new QLabel(QString("Ожидание подключения на порту %1...").arg(kPort));
    m_statusLabel->setStyleSheet("color: gray; font-size: 11px;");
    topRow->addWidget(m_statusLabel, 1);

    m_progressBar = new QProgressBar;
    m_progressBar->setRange(0, 100);
    m_progressBar->setVisible(false);
    m_progressBar->setMaximumWidth(200);
    topRow->addWidget(m_progressBar);
    root->addLayout(topRow);

    // ── Сплиттер: список сессий | список файлов ───────────────────────────────
    QSplitter *splitter = new QSplitter(Qt::Horizontal, this);

    QGroupBox *leftBox = new QGroupBox("Полученные логи");
    QVBoxLayout *leftLayout = new QVBoxLayout(leftBox);
    m_sessionList = new QListWidget;
    leftLayout->addWidget(m_sessionList);
    QHBoxLayout *leftBtns = new QHBoxLayout;
    m_deleteBtn = new QPushButton("🗑  Удалить");
    m_deleteBtn->setEnabled(false);
    leftBtns->addStretch();
    leftBtns->addWidget(m_deleteBtn);
    leftLayout->addLayout(leftBtns);
    splitter->addWidget(leftBox);

    QGroupBox *rightBox = new QGroupBox("Файлы");
    QVBoxLayout *rightLayout = new QVBoxLayout(rightBox);
    m_fileList = new QListWidget;
    rightLayout->addWidget(m_fileList);
    m_openBtn = new QPushButton("📂  Открыть папку");
    m_openBtn->setEnabled(false);
    rightLayout->addWidget(m_openBtn);
    splitter->addWidget(rightBox);

    splitter->setStretchFactor(0, 1);
    splitter->setStretchFactor(1, 1);
    root->addWidget(splitter, 1);

    QLabel *hint = new QLabel("Папка сохранения: " + logsRootDir());
    hint->setStyleSheet("color: gray; font-size: 10px;");
    hint->setWordWrap(true);
    root->addWidget(hint);

    // ── Сигналы UI ─────────────────────────────────────────────────────────────
    connect(m_sessionList, &QListWidget::itemClicked, this, &LogsTab::onSessionSelected);
    connect(m_openBtn,     &QPushButton::clicked,      this, &LogsTab::onOpenFolderClicked);
    connect(m_deleteBtn,   &QPushButton::clicked,      this, &LogsTab::onDeleteClicked);

    // ── TCP-сервер ─────────────────────────────────────────────────────────────
    m_server = new QTcpServer(this);
    connect(m_server, &QTcpServer::newConnection, this, &LogsTab::onNewConnection);
    if (m_server->listen(QHostAddress::Any, kPort)) {
        qDebug() << "[LogsTab] Listening on port" << kPort;
    } else {
        setStatus(QString("Ошибка: порт %1 занят — %2").arg(kPort).arg(m_server->errorString()));
    }

    // ── UDP-обнаружение на отдельном порту (12348) ────────────────────────────
    m_udpSocket = new QUdpSocket(this);
    if (m_udpSocket->bind(QHostAddress::Any, kLogsDiscoverPort, QUdpSocket::ShareAddress)) {
        connect(m_udpSocket, &QUdpSocket::readyRead, this, &LogsTab::processUdpDiscovery);
        qDebug() << "[LogsTab] UDP discovery on port" << kLogsDiscoverPort;
    } else {
        qWarning() << "[LogsTab] UDP bind failed on port" << kLogsDiscoverPort
                   << ":" << m_udpSocket->errorString();
    }

    // ── Загрузка уже существующих сессий ──────────────────────────────────────
    QDir rootDir(logsRootDir());
    if (rootDir.exists()) {
        const auto entries = rootDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot,
                                                   QDir::Time | QDir::Reversed);
        for (const QFileInfo &fi : entries) {
            // Имя папки: "2025-04-23_14-32-15_192.168.1.10"
            QStringList parts = fi.fileName().split('_');
            QString ip = parts.size() >= 3 ? parts.last() : "?";
            QDateTime dt = QDateTime::fromString(
                parts.size() >= 2 ? parts[0] + "_" + parts[1] : parts[0],
                "yyyy-MM-dd_HH-mm-ss");
            addSession(fi.absoluteFilePath(), ip, dt);
        }
    }
}

LogsTab::~LogsTab()
{
    for (QTcpSocket *s : m_clients.keys())
        cleanupClient(s);
}

// ─────────────────────────────────────────────────────────────────────────────
// Новое подключение
// ─────────────────────────────────────────────────────────────────────────────

void LogsTab::onNewConnection()
{
    while (m_server->hasPendingConnections()) {
        QTcpSocket *socket = m_server->nextPendingConnection();

        ClientState cs;
        cs.clientIp = socket->peerAddress().toString();
        if (cs.clientIp.startsWith("::ffff:"))
            cs.clientIp = cs.clientIp.mid(7);
        m_clients[socket] = cs;

        connect(socket, &QTcpSocket::readyRead,    this, &LogsTab::onReadyRead);
        connect(socket, &QTcpSocket::disconnected, this, &LogsTab::onSocketDisconnected);

        setStatus(QString("Подключился: %1").arg(cs.clientIp));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Приём данных
// ─────────────────────────────────────────────────────────────────────────────

void LogsTab::onReadyRead()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket || !m_clients.contains(socket)) return;
    ClientState &cs = m_clients[socket];

    // ── Хендшейк ──────────────────────────────────────────────────────────────
    if (cs.phase == ClientState::PhaseHandshake) {
        cs.buffer.append(socket->readAll());
        int nl = cs.buffer.indexOf('\n');
        if (nl < 0) return; // ждём полной строки

        QByteArray line = cs.buffer.left(nl).trimmed();
        cs.buffer = cs.buffer.mid(nl + 1);

        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (doc.isNull() || !doc.isObject()
            || doc.object().value("type").toString() != "logs") {
            qWarning() << "[LogsTab] Bad handshake from" << cs.clientIp;
            cleanupClient(socket);
            return;
        }

        cs.expectedSize = doc.object().value("archiveSize").toVariant().toLongLong();
        if (cs.expectedSize <= 0) {
            cleanupClient(socket);
            return;
        }

        // Готовим папку и файл для архива
        QDateTime now = QDateTime::currentDateTime();
        QString folderName = now.toString("yyyy-MM-dd_HH-mm-ss") + "_" + cs.clientIp;
        cs.extractPath = logsRootDir() + "/" + folderName;
        QDir().mkpath(cs.extractPath);

        QString archivePath = cs.extractPath + "/logs.7z";
        cs.archiveFile = new QFile(archivePath);
        if (!cs.archiveFile->open(QIODevice::WriteOnly)) {
            delete cs.archiveFile;
            cs.archiveFile = nullptr;
            cleanupClient(socket);
            return;
        }

        cs.phase = ClientState::PhaseReceiving;
        cs.receivedBytes = 0;
        m_progressBar->setVisible(true);
        m_progressBar->setValue(0);

        socket->write("READY\n");

        // Данные могли прийти вместе с хендшейком
        if (!cs.buffer.isEmpty()) {
            cs.archiveFile->write(cs.buffer);
            cs.receivedBytes += cs.buffer.size();
            cs.buffer.clear();
        }

        // ── Приём архива ──────────────────────────────────────────────────────────
    } else if (cs.phase == ClientState::PhaseReceiving) {
        QByteArray chunk = socket->readAll();
        if (cs.archiveFile) {
            cs.archiveFile->write(chunk);
            cs.receivedBytes += chunk.size();
        }

        if (cs.expectedSize > 0) {
            int pct = static_cast<int>(100.0 * cs.receivedBytes / cs.expectedSize);
            m_progressBar->setValue(pct);
            setStatus(QString("Приём от %1: %2 / %3 КБ")
                          .arg(cs.clientIp)
                          .arg(cs.receivedBytes / 1024)
                          .arg(cs.expectedSize / 1024));
        }

        if (cs.receivedBytes >= cs.expectedSize) {
            QString archivePath = cs.archiveFile->fileName();
            cs.archiveFile->close();
            delete cs.archiveFile;
            cs.archiveFile = nullptr;

            socket->write("OK\n");
            socket->flush();

            QString extractPath = cs.extractPath;
            QString clientIp    = cs.clientIp;
            QDateTime dt        = QDateTime::currentDateTime();
            cleanupClient(socket);

            extractArchive(archivePath, extractPath, clientIp, dt);
        }
    }
}

void LogsTab::onSocketDisconnected()
{
    QTcpSocket *socket = qobject_cast<QTcpSocket *>(sender());
    if (!socket) return;
    if (m_clients.contains(socket))
        cleanupClient(socket);
    socket->deleteLater();
}

// ─────────────────────────────────────────────────────────────────────────────
// Распаковка через 7zr
// ─────────────────────────────────────────────────────────────────────────────

void LogsTab::extractArchive(const QString &archivePath, const QString &extractPath,
                             const QString &clientIp, const QDateTime &dt)
{
    setStatus("Распаковка...");

    QString sevenZ = QCoreApplication::applicationDirPath() + "/tools/7zr.exe";
    if (!QFile::exists(sevenZ))
        sevenZ = QCoreApplication::applicationDirPath() + "/7zr.exe";
    if (!QFile::exists(sevenZ))
        sevenZ = "7zr";

    QProcess *proc = new QProcess(this);
    connect(proc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, proc, archivePath, extractPath, clientIp, dt]
            (int exitCode, QProcess::ExitStatus) {
                QFile::remove(archivePath);
                m_progressBar->setVisible(false);

                if (exitCode == 0) {
                    setStatus(QString("Логи от %1 сохранены.").arg(clientIp));
                    addSession(extractPath, clientIp, dt);
                } else {
                    setStatus(QString("Ошибка распаковки от %1 (код %2)")
                                  .arg(clientIp).arg(exitCode));
                    qWarning() << "[LogsTab] 7zr stderr:" << proc->readAllStandardError();
                }
                proc->deleteLater();
            });

    // Распаковываем файлы прямо в extractPath (команда e — без структуры папок)
    proc->start(sevenZ, {"e", archivePath, "-o" + extractPath, "-y"});
}

// ─────────────────────────────────────────────────────────────────────────────
// UI — выбор сессии
// ─────────────────────────────────────────────────────────────────────────────

void LogsTab::onSessionSelected(QListWidgetItem *item)
{
    if (!item) return;
    QString path = item->data(Qt::UserRole).toString();

    m_fileList->clear();
    m_openBtn->setEnabled(QDir(path).exists());
    m_deleteBtn->setEnabled(true);

    QDir dir(path);
    if (!dir.exists()) {
        m_fileList->addItem("(папка не найдена)");
        return;
    }

    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files, QDir::Name)) {
        qint64 s = fi.size();
        QString sz = s < 1024       ? QString("%1 Б").arg(s)
                     : s < 1024*1024  ? QString("%1 КБ").arg(s / 1024)
                                       : QString("%1 МБ").arg(s / 1024 / 1024);

        auto *i = new QListWidgetItem(QString("📄  %1   (%2)").arg(fi.fileName(), sz));
        i->setData(Qt::UserRole, fi.absoluteFilePath());
        m_fileList->addItem(i);
    }

    if (m_fileList->count() == 0)
        m_fileList->addItem("(нет файлов)");
}

void LogsTab::onOpenFolderClicked()
{
    QListWidgetItem *cur = m_sessionList->currentItem();
    if (cur)
        QDesktopServices::openUrl(
            QUrl::fromLocalFile(cur->data(Qt::UserRole).toString()));
}

void LogsTab::onDeleteClicked()
{
    QListWidgetItem *cur = m_sessionList->currentItem();
    if (!cur) return;

    if (QMessageBox::question(this, "Удалить",
                              "Удалить папку с логами?\n" + cur->text(),
                              QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
        return;

    QDir(cur->data(Qt::UserRole).toString()).removeRecursively();
    delete m_sessionList->takeItem(m_sessionList->row(cur));
    m_fileList->clear();
    m_openBtn->setEnabled(false);
    m_deleteBtn->setEnabled(false);
}

// ─────────────────────────────────────────────────────────────────────────────
// Вспомогательные
// ─────────────────────────────────────────────────────────────────────────────

QString LogsTab::logsRootDir() const
{
    // Рядом с программой — если туда можно писать (как раньше). Если программа
    // стоит в Program Files, писать туда нельзя — берём папку данных пользователя.
    static const QString dir = []() {
        const QString local = QCoreApplication::applicationDirPath() + "/received_logs";
        if (QDir().mkpath(local) && QFileInfo(local).isWritable())
            return local;
        return QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)
               + "/received_logs";
    }();
    return dir;
}

void LogsTab::cleanupClient(QTcpSocket *socket)
{
    if (!m_clients.contains(socket)) return;
    ClientState &cs = m_clients[socket];
    if (cs.archiveFile) {
        cs.archiveFile->close();
        delete cs.archiveFile;
        cs.archiveFile = nullptr;
    }
    m_clients.remove(socket);
    // disconnect() снимает и сигнал disconnected → onSocketDisconnected, где
    // вызывался deleteLater. Поэтому при плохом хендшейке сокеты не удалялись
    // никогда. Теперь удаление подключаем заново.
    socket->disconnect(this);
    if (socket->state() == QAbstractSocket::UnconnectedState) {
        socket->deleteLater();
    } else {
        connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
        socket->disconnectFromHost();
    }
}

void LogsTab::addSession(const QString &folderPath, const QString &clientIp, const QDateTime &dt)
{
    int fileCount = QDir(folderPath).entryList(QDir::Files).size();
    QString label = QString("[%1]  %2   (%3 файл(ов))")
                        .arg(dt.isValid() ? dt.toString("dd.MM.yyyy  HH:mm:ss") : "?")
                        .arg(clientIp)
                        .arg(fileCount);

    auto *item = new QListWidgetItem(label);
    item->setData(Qt::UserRole, folderPath);
    m_sessionList->insertItem(0, item); // новые — сверху
}

void LogsTab::setStatus(const QString &msg)
{
    m_statusLabel->setText(msg);
}

// ─── НОВЫЙ МЕТОД: UDP-обнаружение с пометкой "logs" ─────────────────────────
void LogsTab::processUdpDiscovery()
{
    while (m_udpSocket->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(m_udpSocket->pendingDatagramSize()));
        QHostAddress sender;
        quint16 senderPort = 0;
        m_udpSocket->readDatagram(datagram.data(), datagram.size(), &sender, &senderPort);

        if (datagram.trimmed() == "DISCOVER_MEDIACENTER") {
            // Отвечаем с пометкой, что это сервер логов
            m_udpSocket->writeDatagram("MEDIACENTER_RESPONSE logs", sender, senderPort);
        }
    }
}
