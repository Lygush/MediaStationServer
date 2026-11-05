// ServerWindow.cpp
#include "ServerWindow.h"
#include "UpdateTab.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QFormLayout>
#include <QFileDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDir>
#include <QRegularExpression>
#include <QDirIterator>
#include <QPainter>
#include <QCheckBox>
#include <QDebug>
#include <QNetworkInterface>
#include <QHostInfo>

ServerWindow::ServerWindow(QWidget *parent)
    : QMainWindow(parent)
    , socket(nullptr)
    , udpSocket(new QUdpSocket(this))
    , bytesSent(0)
    , metadataSize(0)
    , sevenZipProcess(nullptr)
    , archiveFile(nullptr)
    , archiveSize(0)
    , negotiationTimer(nullptr)
    , state(Idle)
    , lastBytesSent(0)
    , lastTimeMs(0)
    , cancelledByUser(false)
    , archiveReady(false)
{
    setWindowTitle("MediaStation Server");
    resize(700, 760);

    if (!udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress))
        qWarning() << "ServerWindow: UDP bind failed:" << udpSocket->errorString();

    QWidget *central = new QWidget(this);
    setCentralWidget(central);
    QVBoxLayout *rootLayout = new QVBoxLayout(central);
    rootLayout->setContentsMargins(0, 0, 0, 0);

    tabWidget = new QTabWidget(this);

    QWidget *gamesTab = buildGamesTab(); // инициализирует ipCombo
    updateTab = new UpdateTab(this);     // собственный ipCombo внутри

    logsTab   = new LogsTab(this);          // ← добавить

    tabWidget->addTab(gamesTab,  "🎮  Отправка игры");
    tabWidget->addTab(updateTab, "🔄  Обновление MediaStation");
    tabWidget->addTab(logsTab,   "📋  Логи");  // ← добавить

    rootLayout->addWidget(tabWidget);

    tempArchivePath = QDir::temp().absoluteFilePath("mediacenter_send.7z");
}

ServerWindow::~ServerWindow()
{
    cancelledByUser = true;
    cleanupResources();
    QFile::remove(tempArchivePath);
}

// ─────────────────────────────────────────────────────────────────────────────
// Построение вкладки «Игры»
// ─────────────────────────────────────────────────────────────────────────────

QWidget *ServerWindow::buildGamesTab()
{
    QWidget *tab = new QWidget;
    QVBoxLayout *mainLayout = new QVBoxLayout(tab);
    mainLayout->setSpacing(10);
    mainLayout->setContentsMargins(10, 10, 10, 10);

    QFormLayout *form = new QFormLayout;

    // Папка с игрой
    folderEdit = new QLineEdit;
    QPushButton *browseBtn = new QPushButton("Обзор...");
    QHBoxLayout *folderLayout = new QHBoxLayout;
    folderLayout->addWidget(folderEdit);
    folderLayout->addWidget(browseBtn);
    connect(browseBtn,  &QPushButton::clicked,   this, &ServerWindow::browseFolder);
    connect(folderEdit, &QLineEdit::textChanged, this, &ServerWindow::onFolderChanged);
    form->addRow("Папка с игрой:", folderLayout);

    nameEdit = new QLineEdit;
    form->addRow("Название:", nameEdit);

    typeCombo = new QComboBox;
    typeCombo->addItem("Приложение (app)", "app");
    typeCombo->addItem("Веб (web)", "web");
    typeCombo->addItem("PS1", "ps1");
    form->addRow("Тип:", typeCombo);

    // IP + Найти
    QHBoxLayout *ipLayout = new QHBoxLayout;
    ipCombo = new QComboBox;
    ipCombo->setEditable(true);
    ipCombo->setInsertPolicy(QComboBox::NoInsert);
    ipCombo->addItem("127.0.0.1");
    discoverButton = new QPushButton("Найти");
    connect(discoverButton, &QPushButton::clicked, this, &ServerWindow::discoverDevices);
    ipLayout->addWidget(ipCombo);
    ipLayout->addWidget(discoverButton);
    form->addRow("IP медиацентра:", ipLayout);

    mainLayout->addLayout(form);

    // Выбор exe
    exeGroupBox = new QGroupBox("Запускаемый .exe файл");
    QVBoxLayout *exeLayout = new QVBoxLayout(exeGroupBox);
    QLabel *exeHint = new QLabel("Выберите .exe который будет запускать игру.");
    exeHint->setStyleSheet("color: gray; font-size: 11px;");
    exeLayout->addWidget(exeHint);

    exeListWidget = new QListWidget;
    exeListWidget->setMaximumHeight(100);
    connect(exeListWidget, &QListWidget::currentItemChanged,
            this, [this](QListWidgetItem *cur, QListWidgetItem *) {
                if (cur) exeCustomEdit->setText(cur->data(Qt::UserRole).toString());
            });
    exeLayout->addWidget(exeListWidget);

    QHBoxLayout *exeCustomLayout = new QHBoxLayout;
    exeCustomEdit = new QLineEdit;
    exeCustomEdit->setPlaceholderText("Путь к exe (относительный от папки игры)...");
    exeBrowseBtn = new QPushButton("Обзор...");
    connect(exeBrowseBtn, &QPushButton::clicked, this, [this]() {
        QString startDir = folderEdit->text().isEmpty() ? QDir::homePath() : folderEdit->text();
        QString f = QFileDialog::getOpenFileName(this, "Выберите exe", startDir, "Executables (*.exe)");
        if (!f.isEmpty()) {
            QString gameFolder = QDir::cleanPath(folderEdit->text());
            QString cleanF    = QDir::cleanPath(f);
            if (!gameFolder.isEmpty() && cleanF.startsWith(gameFolder, Qt::CaseInsensitive))
                exeCustomEdit->setText(QDir(gameFolder).relativeFilePath(cleanF));
            else
                exeCustomEdit->setText(f);
        }
    });
    exeCustomLayout->addWidget(exeCustomEdit);
    exeCustomLayout->addWidget(exeBrowseBtn);
    exeLayout->addLayout(exeCustomLayout);
    mainLayout->addWidget(exeGroupBox);

    // Выбор иконки
    iconGroupBox = new QGroupBox("Иконка игры (необязательно)");
    QVBoxLayout *iconLayout = new QVBoxLayout(iconGroupBox);
    QLabel *iconHint = new QLabel("Выберите иконку для отображения в медиацентре. Рекомендуемый размер 256x256.");
    iconHint->setStyleSheet("color: gray; font-size: 11px;");
    iconHint->setWordWrap(true);
    iconLayout->addWidget(iconHint);

    iconListWidget = new QListWidget;
    iconListWidget->setViewMode(QListWidget::IconMode);
    iconListWidget->setIconSize(QSize(64, 64));
    iconListWidget->setGridSize(QSize(80, 80));
    iconListWidget->setResizeMode(QListWidget::Adjust);
    iconListWidget->setSelectionMode(QAbstractItemView::SingleSelection);
    iconLayout->addWidget(iconListWidget);

    noIconCheckBox = new QCheckBox("Не использовать иконку (стандартная иконка медиацентра)");
    connect(noIconCheckBox, &QCheckBox::toggled, iconListWidget, &QListWidget::setDisabled);
    iconLayout->addWidget(noIconCheckBox);
    mainLayout->addWidget(iconGroupBox);

    // ── Кнопки: «Подготовить» + «Отправить» + «Отмена» ──────────────────────
    QHBoxLayout *btnLayout = new QHBoxLayout;
    prepareButton = new QPushButton("⚙  Подготовить архив");
    prepareButton->setStyleSheet("font-weight: bold;");
    sendButton = new QPushButton("📤  Отправить игру");
    sendButton->setEnabled(false);  // активна только после подготовки
    cancelButton = new QPushButton("Отмена");
    cancelButton->setEnabled(false);
    btnLayout->addWidget(prepareButton);
    btnLayout->addWidget(sendButton);
    btnLayout->addWidget(cancelButton);
    connect(prepareButton, &QPushButton::clicked, this, &ServerWindow::onPrepareClicked);
    connect(sendButton,    &QPushButton::clicked, this, &ServerWindow::onSendClicked);
    connect(cancelButton,  &QPushButton::clicked, this, &ServerWindow::onCancelClicked);
    mainLayout->addLayout(btnLayout);

    statusLabel = new QLabel("Готов");
    mainLayout->addWidget(statusLabel);

    QFormLayout *progForm = new QFormLayout;
    progressPack = new QProgressBar;
    progressPack->setRange(0, 100);
    progressPack->setValue(0);
    progForm->addRow("Упаковка:", progressPack);
    progressSend = new QProgressBar;
    progressSend->setRange(0, 100);
    progressSend->setValue(0);
    progForm->addRow("Отправка:", progressSend);
    mainLayout->addLayout(progForm);

    return tab;
}

// ─────────────────────────────────────────────────────────────────────────────
// Вкладка игр — обработка событий
// ─────────────────────────────────────────────────────────────────────────────

void ServerWindow::onFolderChanged(const QString &path)
{
    exeListWidget->clear();
    exeCustomEdit->clear();
    if (path.isEmpty() || !QDir(path).exists()) return;

    QDir dir(path);
    QStringList found;
    QDirIterator it(path, QStringList() << "*.exe", QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) found.append(it.next());

    std::sort(found.begin(), found.end(), [&](const QString &a, const QString &b) {
        int da = dir.relativeFilePath(a).count('/');
        int db = dir.relativeFilePath(b).count('/');
        return da != db ? da < db
                        : dir.relativeFilePath(a).toLower() < dir.relativeFilePath(b).toLower();
    });

    for (const QString &exePath : found) {
        QString rel = dir.relativeFilePath(exePath);
        QListWidgetItem *item = new QListWidgetItem(rel);
        item->setData(Qt::UserRole, rel);
        exeListWidget->addItem(item);
    }
    if (exeListWidget->count() > 0) exeListWidget->setCurrentRow(0);
    updateIconList(path);
}

void ServerWindow::updateIconList(const QString &path)
{
    iconListWidget->clear();
    if (path.isEmpty() || !QDir(path).exists()) return;
    QDir dir(path);
    QDirIterator it(path, {"*.ico","*.png","*.jpg","*.jpeg","*.bmp"},
                    QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString filePath = it.next();
        QPixmap pixmap(filePath);
        if (pixmap.isNull()) continue;
        QPixmap scaled = pixmap.scaled(64, 64, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        QPixmap square(64, 64);
        square.fill(Qt::transparent);
        QPainter painter(&square);
        painter.drawPixmap((64 - scaled.width()) / 2, (64 - scaled.height()) / 2, scaled);
        painter.end();
        QString rel = dir.relativeFilePath(filePath);
        QListWidgetItem *item = new QListWidgetItem(QIcon(square), rel);
        item->setData(Qt::UserRole, rel);
        item->setToolTip(rel);
        iconListWidget->addItem(item);
    }
    if (iconListWidget->count() > 0) iconListWidget->setCurrentRow(0);
}

QString ServerWindow::selectedExeRelativePath() const
{
    QString custom = exeCustomEdit->text().trimmed();
    if (!custom.isEmpty()) return custom;
    QListWidgetItem *cur = exeListWidget->currentItem();
    return cur ? cur->data(Qt::UserRole).toString() : QString();
}

void ServerWindow::browseFolder()
{
    QString dir = QFileDialog::getExistingDirectory(this, "Выберите папку с игрой",
                                                    folderEdit->text(), QFileDialog::ShowDirsOnly);
    if (!dir.isEmpty()) {
        folderEdit->setText(dir);
        if (nameEdit->text().isEmpty())
            nameEdit->setText(QDir(dir).dirName());
    }
}

void ServerWindow::discoverDevices()
{
    statusLabel->setText("Поиск медиацентров...");
    discoverButton->setEnabled(false);
    ipCombo->clear();
    ipCombo->addItem("127.0.0.1");

    QByteArray datagram = "DISCOVER_MEDIACENTER";
    udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, 12346);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::IsUp) &&
            !(iface.flags() & QNetworkInterface::IsLoopBack)) {
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                QHostAddress bc = entry.broadcast();
                if (!bc.isNull() && bc.protocol() == QAbstractSocket::IPv4Protocol)
                    udpSocket->writeDatagram(datagram, bc, 12346);
            }
        }
    }

    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    auto readConn = connect(udpSocket, &QUdpSocket::readyRead, this, [this]() {
        while (udpSocket->hasPendingDatagrams()) {
            QByteArray reply(udpSocket->pendingDatagramSize(), 0);
            QHostAddress sender;
            udpSocket->readDatagram(reply.data(), reply.size(), &sender);
            QString ip = sender.toString();
            if (ipCombo->findText(ip) == -1) ipCombo->addItem(ip);
        }
    });
    connect(timer, &QTimer::timeout, this, [this, timer, readConn]() {
        disconnect(readConn);
        timer->deleteLater();
        discoverButton->setEnabled(true);
        statusLabel->setText(ipCombo->count() > 1 ? "Поиск завершён" : "Медиацентры не найдены");
    });
    timer->start(3000);
}

// ─────────────────────────────────────────────────────────────────────────────
// «Подготовить» — упаковываем архив локально, без отправки
// ─────────────────────────────────────────────────────────────────────────────

void ServerWindow::onPrepareClicked()
{
    if (folderEdit->text().isEmpty() || !QDir(folderEdit->text()).exists()) {
        QMessageBox::warning(this, "Ошибка", "Укажите корректную папку с игрой"); return;
    }
    if (nameEdit->text().isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Введите название игры"); return;
    }
    QString exeRel = selectedExeRelativePath();
    QString kind   = typeCombo->currentData().toString();
    if (kind == "app" && exeRel.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Не выбран .exe файл"); return;
    }

    archiveReady    = false;
    cancelledByUser = false;
    prepareButton->setEnabled(false);
    sendButton->setEnabled(false);
    cancelButton->setEnabled(true);
    progressPack->setValue(0);
    progressSend->setValue(0);
    state = Packing;
    statusLabel->setText("Упаковка архива...");
    QFile::remove(tempArchivePath);
    startPacking();
}

// ─────────────────────────────────────────────────────────────────────────────
// «Отправить» — подключаемся и отправляем уже готовый архив
// ─────────────────────────────────────────────────────────────────────────────

void ServerWindow::onSendClicked()
{
    if (!archiveReady || !QFile::exists(tempArchivePath)) {
        QMessageBox::warning(this, "Ошибка", "Сначала подготовьте архив."); return;
    }
    cancelledByUser = false;
    sendButton->setEnabled(false);
    prepareButton->setEnabled(false);
    cancelButton->setEnabled(true);
    progressSend->setValue(0);
    state = Connecting;
    statusLabel->setText("Подключение к медиацентру...");
    connectToClient();
}

void ServerWindow::onCancelClicked()
{
    cancelledByUser = true;
    abortAll(QString(), false);
}

void ServerWindow::connectToClient()
{
    if (socket) { socket->disconnect(); socket->deleteLater(); socket = nullptr; }
    socket = new QTcpSocket(this);
    connect(socket, &QTcpSocket::connected,     this, &ServerWindow::socketConnected);
    connect(socket, &QTcpSocket::disconnected,  this, &ServerWindow::socketDisconnected);
    connect(socket, &QTcpSocket::errorOccurred, this, &ServerWindow::socketError);
    connect(socket, &QTcpSocket::bytesWritten,  this, &ServerWindow::bytesWritten);
    connect(socket, &QTcpSocket::readyRead,     this, &ServerWindow::readyRead);
    QString targetIp = ipCombo->currentText().trimmed();
    if (targetIp.isEmpty()) targetIp = "127.0.0.1";
    socket->connectToHost(targetIp, 12345);
}

void ServerWindow::socketConnected()
{
    statusLabel->setText("Ожидание подтверждения пользователя...");
    state = WaitingForApproval;
    bytesSent = metadataSize = 0;
    incomingData.clear();
    socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,    512 * 1024);
    socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);
    socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);
    speedTimer.start();
    lastBytesSent = lastTimeMs = 0;
    sendNegotiationPacket();
}

void ServerWindow::sendNegotiationPacket()
{
    if (negotiationTimer) {
        negotiationTimer->stop();
        negotiationTimer->deleteLater();
        negotiationTimer = nullptr;
    }
    QJsonObject meta;
    meta["title"] = nameEdit->text();
    meta["kind"]  = typeCombo->currentData().toString();
    QString exeRel = selectedExeRelativePath();
    if (!exeRel.isEmpty()) meta["exeRelativePath"] = exeRel;
    if (!noIconCheckBox->isChecked()) {
        QListWidgetItem *curIcon = iconListWidget->currentItem();
        if (curIcon) meta["iconRelativePath"] = curIcon->data(Qt::UserRole).toString();
    }
    QByteArray data = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    data.append('\n');
    metadataSize = data.size();
    socket->write(data);
    negotiationTimer = new QTimer(this);
    negotiationTimer->setSingleShot(true);
    connect(negotiationTimer, &QTimer::timeout, this, &ServerWindow::negotiationTimeout);
    negotiationTimer->start(120000);
}

void ServerWindow::negotiationTimeout()
{
    abortAll("Медиацентр не ответил (таймаут 2 мин)");
}

void ServerWindow::startPacking()
{
    QString sevenZPath = QCoreApplication::applicationDirPath() + "/tools/7zr.exe";
    if (!QFile::exists(sevenZPath)) sevenZPath = "7zr";
    sevenZipProcess = new QProcess(this);
    sevenZipProcess->setWorkingDirectory(folderEdit->text());
    QStringList args;
    args << "a" << "-mx1" << "-bsp1" << tempArchivePath << ".";
    connect(sevenZipProcess, &QProcess::readyReadStandardOutput, this, &ServerWindow::updatePackProgress);
    connect(sevenZipProcess, &QProcess::readyReadStandardError,  this, &ServerWindow::updatePackProgress);
    connect(sevenZipProcess,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &ServerWindow::archiveFinished);
    sevenZipProcess->start(sevenZPath, args);
}

void ServerWindow::updatePackProgress()
{
    if (!sevenZipProcess) return;
    QByteArray out = sevenZipProcess->readAllStandardOutput()
                   + sevenZipProcess->readAllStandardError();
    static QRegularExpression re("\\s*(\\d+)%");
    int lastPercent = -1;
    for (const QString &line : QString::fromLocal8Bit(out).split('\n', Qt::SkipEmptyParts)) {
        QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch()) lastPercent = m.captured(1).toInt();
    }
    if (lastPercent >= 0) {
        progressPack->setValue(lastPercent);
        sendPackProgressToClient(lastPercent);
    }
}

void ServerWindow::sendPackProgressToClient(int percent)
{
    if (socket && socket->state() == QAbstractSocket::ConnectedState)
        socket->write(("PACK:" + QString::number(percent) + "\n").toUtf8());
}

void ServerWindow::archiveFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (sevenZipProcess) {
        sevenZipProcess->disconnect();
        sevenZipProcess->deleteLater();
        sevenZipProcess = nullptr;
    }
    if (cancelledByUser || state != Packing) return;
    if (exitCode != 0 || exitStatus != QProcess::NormalExit) {
        abortAll("Не удалось создать архив (код " + QString::number(exitCode) + ")"); return;
    }
    archiveSize = QFileInfo(tempArchivePath).size();
    if (archiveSize == 0) { abortAll("Архив пуст"); return; }

    progressPack->setValue(100);
    archiveReady = true;
    state = ReadyToSend;

    // Разблокируем кнопки
    prepareButton->setEnabled(true);
    sendButton->setEnabled(true);
    cancelButton->setEnabled(false);
    statusLabel->setText(QString("Архив готов (%1 МБ). Нажмите «Отправить игру».")
                             .arg(archiveSize / 1024.0 / 1024.0, 0, 'f', 1));
}

void ServerWindow::startSendingArchive()
{
    QJsonObject meta;
    meta["archiveSize"] = archiveSize;
    QByteArray data = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    data.append('\n');
    socket->write(data);
    statusLabel->setText("Ожидание готовности клиента...");
    state = SendingArchive;
}

void ServerWindow::sendArchiveChunk()
{
    if (!archiveFile) {
        archiveFile = new QFile(tempArchivePath, this);
        if (!archiveFile->open(QIODevice::ReadOnly)) {
            abortAll("Не удалось открыть архив для отправки"); return;
        }
    }
    const qint64 chunkSize = 2 * 1024 * 1024;
    QByteArray chunk = archiveFile->read(chunkSize);
    if (chunk.isEmpty()) {
        archiveFile->close(); archiveFile->deleteLater(); archiveFile = nullptr;
        statusLabel->setText("Архив отправлен. Ожидание подтверждения..."); return;
    }
    qint64 written = socket->write(chunk);
    if (written < 0)
        abortAll("Ошибка записи в сокет");
    else if (written < chunk.size())
        archiveFile->seek(archiveFile->pos() - (chunk.size() - written));
}

void ServerWindow::bytesWritten(qint64 bytes)
{
    if (state != SendingArchive || !archiveFile) return;
    bytesSent += bytes;
    if (archiveSize > 0)
        progressSend->setValue(static_cast<int>(100.0 * bytesSent / archiveSize));
    qint64 elapsed = speedTimer.elapsed();
    if (elapsed - lastTimeMs >= 1000) {
        qint64 deltaMs = elapsed - lastTimeMs;
        if (deltaMs > 0) {
            double speedBps  = (bytesSent - lastBytesSent) * 1000.0 / deltaMs;
            double speedMbps = speedBps * 8 / 1'000'000;
            QString speedText = speedMbps >= 1.0
                                    ? QString("%1 Мбит/с").arg(speedMbps, 0, 'f', 1)
                                    : QString("%1 КБ/с").arg(speedBps / 1024, 0, 'f', 1);
            statusLabel->setText(QString("Отправка: %1% (%2)").arg(progressSend->value()).arg(speedText));
        }
        lastBytesSent = bytesSent;
        lastTimeMs    = elapsed;
    }
    if (bytesSent < archiveSize)
        sendArchiveChunk();
    else {
        archiveFile->close(); archiveFile->deleteLater(); archiveFile = nullptr;
    }
}

void ServerWindow::readyRead()
{
    incomingData.append(socket->readAll());
    while (incomingData.contains('\n')) {
        int idx = incomingData.indexOf('\n');
        QByteArray line = incomingData.left(idx).trimmed();
        incomingData = incomingData.mid(idx + 1);
        if (line == "CANCEL") {
            cancelledByUser = false;
            abortAll("Установка отменена пользователем на медиацентре", false); return;
        }
        if (state == WaitingForApproval) {
            if (line == "OK") {
                if (negotiationTimer) {
                    negotiationTimer->stop();
                    negotiationTimer->deleteLater();
                    negotiationTimer = nullptr;
                }
                statusLabel->setText("Одобрено. Отправка архива...");
                startSendingArchive();
            } else if (line == "REJECTED") {
                if (negotiationTimer) {
                    negotiationTimer->stop();
                    negotiationTimer->deleteLater();
                    negotiationTimer = nullptr;
                }
                abortAll("Установка отклонена пользователем", false);
            }
        } else if (state == SendingArchive) {
            if (line == "READY") {
                bytesSent = lastBytesSent = lastTimeMs = 0;
                speedTimer.restart();
                statusLabel->setText("Отправка архива...");
                sendArchiveChunk();
            } else if (line.startsWith("OK")) {
                state = Idle;
                cancelledByUser = false;
                archiveReady = false;
                if (socket) socket->disconnectFromHost();
                QFile::remove(tempArchivePath);
                resetUI();
                QTimer::singleShot(0, this, [this]() {
                    QMessageBox::information(this, "Успех",
                        "Игра успешно отправлена!\nМедиацентр начал установку.");
                });
            } else if (line.startsWith("FAIL")) {
                abortAll("Ошибка на стороне медиацентра:\n" + QString::fromUtf8(line));
            }
        }
    }
}

void ServerWindow::socketDisconnected()
{
    if (state != Idle && state != ReadyToSend) resetUI();
}

void ServerWindow::socketError(QAbstractSocket::SocketError err)
{
    if (err == QAbstractSocket::RemoteHostClosedError && state == Idle) return;
    if (cancelledByUser) return;
    abortAll("Сетевая ошибка: " + (socket ? socket->errorString() : "неизвестно"));
}

void ServerWindow::cleanupResources()
{
    if (negotiationTimer) {
        negotiationTimer->stop();
        negotiationTimer->deleteLater();
        negotiationTimer = nullptr;
    }
    if (sevenZipProcess) {
        sevenZipProcess->disconnect();
        sevenZipProcess->kill();
        sevenZipProcess->deleteLater();
        sevenZipProcess = nullptr;
    }
    if (archiveFile) {
        archiveFile->close();
        archiveFile->deleteLater();
        archiveFile = nullptr;
    }
    if (socket) {
        socket->disconnect();
        if (socket->state() == QAbstractSocket::ConnectedState) {
            socket->write("CANCEL\n");
            socket->flush();
            socket->disconnectFromHost();
        }
        socket->deleteLater();
        socket = nullptr;
    }
}

void ServerWindow::abortAll(const QString &reason, bool showDialog)
{
    state = Idle;
    cleanupResources();
    resetUI();
    if (showDialog && !reason.isEmpty())
        QMessageBox::critical(this, "Ошибка", reason);
    else if (!reason.isEmpty())
        statusLabel->setText(reason);
}

void ServerWindow::resetUI()
{
    state = Idle;
    cancelledByUser = false;
    incomingData.clear();
    bytesSent = archiveSize = metadataSize = lastBytesSent = lastTimeMs = 0;
    prepareButton->setEnabled(true);
    sendButton->setEnabled(archiveReady);
    cancelButton->setEnabled(false);
    progressPack->setValue(0);
    progressSend->setValue(0);
    if (!archiveReady)
        statusLabel->setText("Готов");
}
