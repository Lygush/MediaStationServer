// UpdateTab.cpp
// ─────────────────────────────────────────────────────────────────────────────
// Вкладка «Обновление MediaStation»
// Изменения:
//   - Раздельные кнопки «Подготовить» и «Отправить»
//   - Собственный поиск медиацентра (UDP discover) без зависимости от вкладки Игры
//   - Сохранение путей (exe, qml, windeployqt) в QSettings между сессиями
//   - Temp-папка создаётся в системном temp, мусор убирается всегда
//   - Копирование кастомных *.dll из папки сборки перед запуском windeployqt
// ─────────────────────────────────────────────────────────────────────────────

#include "UpdateTab.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QFormLayout>
#include <QGroupBox>
#include <QFileDialog>
#include <QMessageBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDebug>
#include <QRegularExpression>
#include <QThread>
#include <QSettings>
#include <QNetworkInterface>
#include <QtConcurrent/QtConcurrent>

#ifdef Q_OS_WIN
#  include <windows.h>
#endif

// Версия из ресурса VERSIONINFO exe-файла (MediaStation.pro пишет туда
// содержимое файла VERSION). Пустая строка — ресурса нет (старая сборка)
// или не Windows: тогда версию вводят руками, как раньше.
static QString readExeVersion(const QString &exePath)
{
#ifdef Q_OS_WIN
    const std::wstring path = QDir::toNativeSeparators(exePath).toStdWString();
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (size == 0) return {};
    QByteArray buf(int(size), Qt::Uninitialized);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data())) return {};
    VS_FIXEDFILEINFO *info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void **>(&info), &len)
        || !info || len < sizeof(VS_FIXEDFILEINFO))
        return {};
    const int major = HIWORD(info->dwFileVersionMS);
    const int minor = LOWORD(info->dwFileVersionMS);
    const int patch = HIWORD(info->dwFileVersionLS);
    if (major == 0 && minor == 0 && patch == 0) return {};
    return QStringLiteral("%1.%2.%3").arg(major).arg(minor).arg(patch);
#else
    Q_UNUSED(exePath);
    return {};
#endif
}

// ─────────────────────────────────────────────────────────────────────────────
// Конструктор / деструктор
// ─────────────────────────────────────────────────────────────────────────────

UpdateTab::UpdateTab(QWidget *parent)
    : QWidget(parent)
    , m_step(StepIdle)
    , m_cancelled(false)
    , m_archiveReady(false)
    , m_archiveSize(0)
    , m_bytesSent(0)
    , m_deployProc(nullptr)
    , m_packProc(nullptr)
    , m_socket(nullptr)
    , m_udpSocket(new QUdpSocket(this))
    , m_archiveFile(nullptr)
    , m_lastBytes(0)
    , m_lastMs(0)
{
    m_udpSocket->bind(QHostAddress::AnyIPv4, 0, QUdpSocket::ShareAddress);

    QVBoxLayout *root = new QVBoxLayout(this);
    root->setSpacing(8);

    // ── Источник обновления ───────────────────────────────────────────────────
    QGroupBox *srcBox = new QGroupBox("Источник обновления");
    QFormLayout *srcForm = new QFormLayout(srcBox);

    // MediaStation.exe
    QHBoxLayout *exeRow = new QHBoxLayout;
    exeEdit = new QLineEdit;
    exeEdit->setPlaceholderText("Путь к собранному MediaStation.exe...");
    QPushButton *exeBrowse = new QPushButton("Обзор...");
    connect(exeBrowse, &QPushButton::clicked, this, &UpdateTab::browseExe);
    exeRow->addWidget(exeEdit);
    exeRow->addWidget(exeBrowse);
    srcForm->addRow("MediaStation.exe:", exeRow);

    // QML папка
    QHBoxLayout *qmlRow = new QHBoxLayout;
    qmlDirEdit = new QLineEdit;
    qmlDirEdit->setPlaceholderText("Путь к папке с QML (обычно MediaStation/qml)");
    QPushButton *qmlBrowse = new QPushButton("Обзор...");
    connect(qmlBrowse, &QPushButton::clicked, this, [this]() {
        QString dir = QFileDialog::getExistingDirectory(this, "Выберите папку QML", qmlDirEdit->text());
        if (!dir.isEmpty()) { qmlDirEdit->setText(dir); saveSettings(); }
    });
    qmlRow->addWidget(qmlDirEdit);
    qmlRow->addWidget(qmlBrowse);
    srcForm->addRow("QML папка:", qmlRow);

    // windeployqt6.exe
    QHBoxLayout *deployRow = new QHBoxLayout;
    windeployqtEdit = new QLineEdit;
    windeployqtEdit->setPlaceholderText("Путь к windeployqt6.exe (MSVC)");
    QPushButton *deployBrowse = new QPushButton("Обзор...");
    connect(deployBrowse, &QPushButton::clicked, this, [this]() {
        QString f = QFileDialog::getOpenFileName(this, "Выберите windeployqt6.exe",
                                                 windeployqtEdit->text(), "Executable (*.exe)");
        if (!f.isEmpty()) { windeployqtEdit->setText(f); saveSettings(); }
    });
    deployRow->addWidget(windeployqtEdit);
    deployRow->addWidget(deployBrowse);
    srcForm->addRow("windeployqt6:", deployRow);

    // Версия
    versionEdit = new QLineEdit;
    versionEdit->setPlaceholderText("Берётся из MediaStation.exe");
    srcForm->addRow("Версия:", versionEdit);

    // Release Notes
    notesEdit = new QTextEdit;
    notesEdit->setPlaceholderText("Описание изменений (Release Notes)...");
    notesEdit->setMaximumHeight(80);
    srcForm->addRow("Что нового:", notesEdit);

    // Исключить tools
    excludeToolsCheckBox = new QCheckBox("Исключить папку tools из архива обновления");
    srcForm->addRow("", excludeToolsCheckBox);

    root->addWidget(srcBox);

    // ── Медиацентр (поиск) ────────────────────────────────────────────────────
    QGroupBox *netBox = new QGroupBox("Медиацентр");
    QFormLayout *netForm = new QFormLayout(netBox);

    QHBoxLayout *ipRow = new QHBoxLayout;
    ipCombo = new QComboBox;
    ipCombo->setEditable(true);
    ipCombo->setInsertPolicy(QComboBox::NoInsert);
    ipCombo->addItem("127.0.0.1");
    discoverBtn = new QPushButton("Найти");
    connect(discoverBtn, &QPushButton::clicked, this, &UpdateTab::discoverDevices);
    ipRow->addWidget(ipCombo);
    ipRow->addWidget(discoverBtn);
    netForm->addRow("IP медиацентра:", ipRow);

    discoverStatus = new QLabel;
    discoverStatus->setStyleSheet("color: gray; font-size: 10px;");
    netForm->addRow("", discoverStatus);

    root->addWidget(netBox);

    // ── Кнопки управления ─────────────────────────────────────────────────────
    QHBoxLayout *btnRow = new QHBoxLayout;
    prepareBtn = new QPushButton("⚙  Подготовить архив");
    prepareBtn->setStyleSheet("font-weight: bold;");
    sendBtn = new QPushButton("📤  Отправить на медиацентр");
    sendBtn->setEnabled(false);   // активна только после успешной подготовки
    cancelBtn = new QPushButton("Отмена");
    cancelBtn->setEnabled(false);
    btnRow->addWidget(prepareBtn);
    btnRow->addWidget(sendBtn);
    btnRow->addWidget(cancelBtn);
    connect(prepareBtn, &QPushButton::clicked, this, &UpdateTab::onPrepareClicked);
    connect(sendBtn,    &QPushButton::clicked, this, &UpdateTab::onSendClicked);
    connect(cancelBtn,  &QPushButton::clicked, this, &UpdateTab::onCancelClicked);
    root->addLayout(btnRow);

    statusLabel = new QLabel("Готов");
    root->addWidget(statusLabel);

    QFormLayout *progForm = new QFormLayout;
    progressPrep = new QProgressBar;
    progressPrep->setRange(0, 100);
    progForm->addRow("Подготовка:", progressPrep);
    progressSend = new QProgressBar;
    progressSend->setRange(0, 100);
    progForm->addRow("Отправка:", progressSend);
    root->addLayout(progForm);

    QGroupBox *logBox = new QGroupBox("Журнал");
    QVBoxLayout *logLayout = new QVBoxLayout(logBox);
    logView = new QTextEdit;
    logView->setReadOnly(true);
    logView->setMaximumHeight(160);
    logView->setFont(QFont("Consolas", 9));
    logLayout->addWidget(logView);
    root->addWidget(logBox);

    // Сохраняем пути при изменении
    connect(exeEdit,        &QLineEdit::editingFinished, this, &UpdateTab::saveSettings);
    connect(exeEdit,        &QLineEdit::editingFinished, this, &UpdateTab::refreshVersionFromExe);
    connect(qmlDirEdit,     &QLineEdit::editingFinished, this, &UpdateTab::saveSettings);
    connect(windeployqtEdit,&QLineEdit::editingFinished, this, &UpdateTab::saveSettings);

    loadSettings();
}

UpdateTab::~UpdateTab()
{
    m_cancelled = true;
    ++m_job;
    cleanup();
    cleanupArchive();   // при закрытии программы архив в %TEMP% не оставляем
}

// ─────────────────────────────────────────────────────────────────────────────
// Настройки
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::saveSettings()
{
    QSettings s("MediaStationServer", "UpdateTab");
    s.setValue("exePath",       exeEdit->text());
    s.setValue("qmlDir",        qmlDirEdit->text());
    s.setValue("windeployqt",   windeployqtEdit->text());
}

void UpdateTab::loadSettings()
{
    QSettings s("MediaStationServer", "UpdateTab");
    exeEdit->setText(        s.value("exePath").toString());
    qmlDirEdit->setText(     s.value("qmlDir").toString());
    windeployqtEdit->setText(s.value("windeployqt").toString());
    refreshVersionFromExe();
}

// Версия — из exe. Поле доступно для ввода, только если в exe её нет
// (сборка до появления файла VERSION).
bool UpdateTab::refreshVersionFromExe()
{
    const QString exePath = exeEdit->text().trimmed();
    const QString v = QFile::exists(exePath) ? readExeVersion(exePath) : QString();
    if (!v.isEmpty()) {
        versionEdit->setText(v);
        versionEdit->setReadOnly(true);
        versionEdit->setToolTip("Версия прочитана из MediaStation.exe (файл VERSION в репозитории)");
        return true;
    }
    versionEdit->setReadOnly(false);
    versionEdit->setToolTip("В exe нет версии — введите вручную");
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Поиск медиацентра (UDP)
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::discoverDevices()
{
    discoverStatus->setText("Поиск...");
    discoverBtn->setEnabled(false);
    ipCombo->clear();
    ipCombo->addItem("127.0.0.1");

    QByteArray datagram = "DISCOVER_MEDIACENTER";
    m_udpSocket->writeDatagram(datagram, QHostAddress::Broadcast, 12346);
    for (const QNetworkInterface &iface : QNetworkInterface::allInterfaces()) {
        if ((iface.flags() & QNetworkInterface::IsUp) &&
            !(iface.flags() & QNetworkInterface::IsLoopBack)) {
            for (const QNetworkAddressEntry &entry : iface.addressEntries()) {
                QHostAddress bc = entry.broadcast();
                if (!bc.isNull() && bc.protocol() == QAbstractSocket::IPv4Protocol)
                    m_udpSocket->writeDatagram(datagram, bc, 12346);
            }
        }
    }

    // Читаем ответы 3 секунды
    QTimer *timer = new QTimer(this);
    timer->setSingleShot(true);
    auto readConn = connect(m_udpSocket, &QUdpSocket::readyRead, this, [this]() {
        while (m_udpSocket->hasPendingDatagrams()) {
            QByteArray reply(m_udpSocket->pendingDatagramSize(), 0);
            QHostAddress sender;
            m_udpSocket->readDatagram(reply.data(), reply.size(), &sender);
            QString ip = sender.toString();
            if (ipCombo->findText(ip) == -1) ipCombo->addItem(ip);
        }
    });
    connect(timer, &QTimer::timeout, this, [this, timer, readConn]() {
        disconnect(readConn);
        timer->deleteLater();
        discoverBtn->setEnabled(true);
        discoverStatus->setText(ipCombo->count() > 1
                                    ? QString("Найдено: %1").arg(ipCombo->count() - 1)
                                    : "Медиацентры не найдены");
    });
    timer->start(3000);
}

// ─────────────────────────────────────────────────────────────────────────────
// Кнопка «Подготовить»
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::browseExe()
{
    QString f = QFileDialog::getOpenFileName(
        this, "Выберите собранный MediaStation.exe",
        exeEdit->text(), "Executable (*.exe)");
    if (!f.isEmpty()) { exeEdit->setText(f); saveSettings(); refreshVersionFromExe(); }
}

void UpdateTab::onPrepareClicked()
{
    const QString exePath = exeEdit->text().trimmed();
    refreshVersionFromExe();   // exe могли пересобрать после выбора
    const QString version = versionEdit->text().trimmed();

    if (exePath.isEmpty() || !QFile::exists(exePath)) {
        QMessageBox::warning(this, "Ошибка", "Укажите корректный путь к MediaStation.exe");
        return;
    }
    if (version.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Укажите версию (например, 1.2.0)");
        return;
    }

    m_cancelled    = false;
    // Прошлый готовый архив больше не нужен — раньше он оставался в %TEMP%
    if (m_archiveReady) {
        m_archiveReady = false;
        cleanupArchive();
    }
    sendBtn->setEnabled(false);
    prepareBtn->setEnabled(false);
    cancelBtn->setEnabled(true);
    progressPrep->setValue(0);
    progressSend->setValue(0);
    logView->clear();
    m_step = StepCopy;

    startCopyStep();
}

// ─────────────────────────────────────────────────────────────────────────────
// Кнопка «Отправить»
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::onSendClicked()
{
    if (!m_archiveReady || m_archivePath.isEmpty() || !QFile::exists(m_archivePath)) {
        QMessageBox::warning(this, "Ошибка", "Сначала подготовьте архив кнопкой «Подготовить».");
        return;
    }
    const QString targetIp = ipCombo->currentText().trimmed();
    if (targetIp.isEmpty()) {
        QMessageBox::warning(this, "Ошибка", "Укажите IP медиацентра.");
        return;
    }

    m_cancelled = false;
    sendBtn->setEnabled(false);
    prepareBtn->setEnabled(false);
    cancelBtn->setEnabled(true);
    progressSend->setValue(0);
    m_step = StepConnecting;
    setStatus("Подключение к медиацентру...");
    log("── Отправка на медиацентр ──");

    if (m_socket) { m_socket->disconnect(); m_socket->deleteLater(); }
    m_socket = new QTcpSocket(this);
    connect(m_socket, &QTcpSocket::connected,     this, &UpdateTab::socketConnected);
    connect(m_socket, &QTcpSocket::disconnected,  this, &UpdateTab::socketDisconnected);
    connect(m_socket, &QTcpSocket::errorOccurred, this, &UpdateTab::socketError);
    connect(m_socket, &QTcpSocket::readyRead,     this, &UpdateTab::socketReadyRead);
    connect(m_socket, &QTcpSocket::bytesWritten,  this, &UpdateTab::socketBytesWritten);
    m_socket->connectToHost(targetIp, 12345);
}

void UpdateTab::onCancelClicked()
{
    m_cancelled = true;
    abort(QString(), false);
}

// ─────────────────────────────────────────────────────────────────────────────
// ШАГ 1: Копирование файлов (асинхронно)
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::startCopyStep()
{
    setStatus("Копирование файлов...");
    log("── Шаг 1: Копирование exe + Updater + tools + qml + dll ──");

    const QString exePath  = exeEdit->text().trimmed();
    const QFileInfo exeInfo(exePath);
    const QString buildDir = exeInfo.absolutePath();

    const QString updaterSrc = QDir(buildDir).filePath("Updater/Updater.exe");
    const bool hasUpdater    = QFile::exists(updaterSrc);

    const QString qmlSrc = qmlDirEdit->text().trimmed();
    const bool hasQml    = !qmlSrc.isEmpty() && QDir(qmlSrc).exists();

    // Обе папки — в системном temp
    const QString ts  = QString::number(QDateTime::currentMSecsSinceEpoch());
    m_tempDir     = QDir::temp().absoluteFilePath("ms_staging_" + ts);
    m_archivePath = QDir::temp().absoluteFilePath("ms_update_"  + ts + ".7z");

    if (!QDir().mkpath(m_tempDir)) {
        abort("Не удалось создать временную папку " + m_tempDir);
        return;
    }

    const QString dstExe     = m_tempDir + "/" + exeInfo.fileName();
    const QString dstUpdater = m_tempDir + "/Updater/Updater.exe";
    const QString srcTools   = buildDir + "/tools";
    const QString dstTools   = m_tempDir + "/tools";
    const QString dstQml     = m_tempDir + "/qml";
    const bool excludeTools  = excludeToolsCheckBox->isChecked();

    const int job = m_job;
    const QString tempDir = m_tempDir;
    m_copyRunning = true;
    QFuture<bool> future = QtConcurrent::run([=]() -> bool {
        if (!QFile::copy(exePath, dstExe))
            return false;

        if (hasUpdater) {
            QDir().mkpath(QFileInfo(dstUpdater).absolutePath());
            if (!QFile::copy(updaterSrc, dstUpdater))
                return false;
        }

        if (!excludeTools && QDir(srcTools).exists())
            copyDirRecursive(srcTools, dstTools); // не фатально

        if (hasQml && !copyDirRecursive(qmlSrc, dstQml))
            return false;

        // ── Кастомные DLL из папки сборки ──────────────────────────────────
        const QDir buildDirQ(buildDir);
        for (const QFileInfo &dllInfo : buildDirQ.entryInfoList({"*.dll"}, QDir::Files)) {
            const QString dstDll = tempDir + "/" + dllInfo.fileName();
            if (!QFile::exists(dstDll))   // не перебиваем уже скопированное
                QFile::copy(dllInfo.absoluteFilePath(), dstDll);
        }

        return true;
    });

    QFutureWatcher<bool> *watcher = new QFutureWatcher<bool>(this);
    connect(watcher, &QFutureWatcher<bool>::finished, this,
            [this, watcher, exeInfo, hasUpdater, hasQml, buildDir, job, tempDir]()
            {
                const bool ok = watcher->result();
                watcher->deleteLater();
                m_copyRunning = false;
                // Отменили, пока шло копирование: папку тогда удалить было
                // нельзя (её читал фоновый поток) — удаляем сейчас
                if (job != m_job || m_cancelled) {
                    QDir(tempDir).removeRecursively();
                    return;
                }
                if (!ok) { abort("Ошибка копирования файлов"); return; }

                log("  + " + exeInfo.fileName());
                log(hasUpdater ? "  + Updater/Updater.exe" : "  ! Updater.exe не найден — пропущен");
                log(hasQml     ? "  + qml/" : "  ! QML не указана");
                if (excludeToolsCheckBox->isChecked())
                    log("  ! tools/ исключены");
                else
                    log(QDir(m_tempDir + "/tools").exists() ? "  + tools/" : "  ! tools/ не найдены");

                // Посчитаем, сколько кастомных dll скопировалось
                const int dllCount = QDir(m_tempDir).entryList({"*.dll"}, QDir::Files).count();
                log(dllCount > 0
                        ? QString("  + %1 dll из папки сборки").arg(dllCount)
                        : "  ! dll в папке сборки не найдены");

                progressPrep->setValue(15);
                startDeployStep();
            });
    watcher->setFuture(future);
}

// ─────────────────────────────────────────────────────────────────────────────
// ШАГ 2: windeployqt6
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::startDeployStep()
{
    setStatus("Запуск windeployqt6...");
    log("── Шаг 2: windeployqt6 ──");

    const QString winDeploy = winDeployPath();
    if (winDeploy.isEmpty()) {
        log("  ! windeployqt6 не найден — шаг пропущен");
        progressPrep->setValue(40);
        startPackStep();
        return;
    }
    log("  windeployqt6: " + winDeploy);

    m_deployExeQueue.clear();
    m_deployExeQueue << m_tempDir + "/MediaStation.exe";
    if (QFile::exists(m_tempDir + "/Updater/Updater.exe"))
        m_deployExeQueue << m_tempDir + "/Updater/Updater.exe";

    deployNextExe();
}

void UpdateTab::deployNextExe()
{
    if (m_deployExeQueue.isEmpty()) {
        log("  windeployqt6 завершён");
        progressPrep->setValue(40);
        startPackStep();
        return;
    }

    const QString exePath = m_deployExeQueue.takeFirst();
    const QString exeName = QFileInfo(exePath).fileName();
    log(QString("  → %1").arg(exeName));

    const QString deployDir =
        exeName.compare("Updater.exe", Qt::CaseInsensitive) == 0 ? "Updater" : ".";

    if (m_deployProc) { m_deployProc->deleteLater(); m_deployProc = nullptr; }

    m_deployProc = new QProcess(this);
    connect(m_deployProc, &QProcess::readyReadStandardOutput, this, &UpdateTab::deployOutput);
    connect(m_deployProc, &QProcess::readyReadStandardError,  this, &UpdateTab::deployOutput);
    connect(m_deployProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdateTab::deployFinished);

    QStringList args;
    args << "--qmldir" << "qml" << "--compiler-runtime"
         << "--no-translations" << "--dir" << deployDir << exePath;

    m_deployProc->setWorkingDirectory(m_tempDir);
    m_deployProc->start(winDeployPath(), args);
}

void UpdateTab::deployOutput()
{
    if (!m_deployProc) return;
    const QString out = QString::fromLocal8Bit(
                            m_deployProc->readAllStandardOutput() +
                            m_deployProc->readAllStandardError()).trimmed();
    if (!out.isEmpty()) log("  " + out);
}

void UpdateTab::deployFinished(int exitCode, QProcess::ExitStatus)
{
    if (!m_deployProc) return;
    const QStringList args = m_deployProc->arguments();
    const QString exeName  = args.isEmpty() ? "?" : QFileInfo(args.last()).fileName();
    log(exitCode == 0 ? QString("  √ %1").arg(exeName)
                      : QString("  ! код %1 для %2").arg(exitCode).arg(exeName));
    m_deployProc->deleteLater();
    m_deployProc = nullptr;
    if (!m_cancelled) deployNextExe();
}

// ─────────────────────────────────────────────────────────────────────────────
// ШАГ 3: Упаковка в 7z
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::startPackStep()
{
    if (m_cancelled) return;
    setStatus("Упаковка архива...");
    log("── Шаг 3: Упаковка в 7z ──");
    log("  7zr: " + sevenZipPath());

    m_packProc = new QProcess(this);
    m_packProc->setWorkingDirectory(m_tempDir);
    connect(m_packProc, &QProcess::readyReadStandardOutput, this, &UpdateTab::packProgress);
    connect(m_packProc, &QProcess::readyReadStandardError,  this, &UpdateTab::packProgress);
    connect(m_packProc, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, &UpdateTab::packFinished);

    m_packProc->start(sevenZipPath(),
                      {"a", "-t7z", "-mx=5", "-mmt=on", "-bsp1", m_archivePath, "."});
}

void UpdateTab::packProgress()
{
    if (!m_packProc) return;
    QByteArray out = m_packProc->readAllStandardOutput() + m_packProc->readAllStandardError();
    static QRegularExpression re("\\s*(\\d+)%");
    int last = -1;
    for (const QString &line : QString::fromLocal8Bit(out).split('\n', Qt::SkipEmptyParts)) {
        QRegularExpressionMatch m = re.match(line);
        if (m.hasMatch()) last = m.captured(1).toInt();
    }
    if (last >= 0) progressPrep->setValue(40 + last * 50 / 100);
}

void UpdateTab::packFinished(int exitCode, QProcess::ExitStatus status)
{
    if (m_packProc) { m_packProc->deleteLater(); m_packProc = nullptr; }
    if (m_cancelled) return;
    if (exitCode != 0 || status != QProcess::NormalExit) {
        abort(QString("Ошибка упаковки (код %1)").arg(exitCode)); return;
    }

    m_archiveSize = QFileInfo(m_archivePath).size();
    if (m_archiveSize == 0) { abort("Архив пуст"); return; }
    log(QString("  Архив: %1 МБ").arg(m_archiveSize / 1024.0 / 1024.0, 0, 'f', 1));

    setStatus("Вычисление контрольной суммы...");
    auto *watcher = new QFutureWatcher<QString>(this);
    const int job = m_job;
    const QString archivePath = m_archivePath;
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, watcher, job, archivePath]() {
        const QString hash = watcher->result();
        watcher->deleteLater();
        // Отменили во время подсчёта: архив был открыт и мог не удалиться
        if (job != m_job) { QFile::remove(archivePath); return; }
        m_hash = hash;
        if (m_hash.isEmpty()) { abort("Ошибка вычисления хэша"); return; }

        log("  SHA-256: " + m_hash);
        progressPrep->setValue(100);

        // Temp-папка больше не нужна — архив готов
        if (!m_tempDir.isEmpty()) {
            QDir(m_tempDir).removeRecursively();
            m_tempDir.clear();
        }

        m_archiveReady = true;
        m_step = StepIdle;

        // Обновляем UI: подготовка завершена, можно отправлять
        prepareBtn->setEnabled(true);
        sendBtn->setEnabled(true);
        cancelBtn->setEnabled(false);
        setStatus(QString("Архив готов (%1 МБ). Нажмите «Отправить».")
                      .arg(m_archiveSize / 1024.0 / 1024.0, 0, 'f', 1));
        log("── Подготовка завершена. Нажмите «Отправить». ──");
    });
    watcher->setFuture(QtConcurrent::run([this, archivePath]() { return computeHash(archivePath); }));
}

// ─────────────────────────────────────────────────────────────────────────────
// Сетевая отправка
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::socketConnected()
{
    m_socket->setSocketOption(QAbstractSocket::SendBufferSizeSocketOption,    512 * 1024);
    m_socket->setSocketOption(QAbstractSocket::ReceiveBufferSizeSocketOption, 512 * 1024);
    m_socket->setSocketOption(QAbstractSocket::LowDelayOption, 1);

    m_step = StepSending;
    m_bytesSent = m_lastBytes = m_lastMs = 0;
    m_speedTimer.restart();
    m_incoming.clear();

    QJsonObject meta;
    meta["kind"]         = QString("mediastation_update");
    meta["version"]      = versionEdit->text().trimmed();
    meta["archiveSize"]  = m_archiveSize;
    meta["expectedHash"] = m_hash;
    meta["releaseNotes"] = notesEdit->toPlainText().trimmed();

    QByteArray data = QJsonDocument(meta).toJson(QJsonDocument::Compact);
    data.append('\n');
    m_socket->write(data);

    setStatus("Ожидание готовности медиацентра...");
    log("  Метаданные отправлены, ожидаем READY...");
}

void UpdateTab::socketReadyRead()
{
    if (!m_socket) return;
    m_incoming.append(m_socket->readAll());

    while (m_incoming.contains('\n')) {
        int idx = m_incoming.indexOf('\n');
        QByteArray line = m_incoming.left(idx).trimmed();
        m_incoming = m_incoming.mid(idx + 1);

        if (line == "CANCEL") { abort("Медиацентр отменил обновление", false); return; }

        if (line == "READY") {
            setStatus("Отправка архива...");
            log("  Медиацентр готов. Начинаем отправку...");
            sendChunk();
        } else if (line.startsWith("OK")) {
            m_step = StepIdle;
            log("  ✓ Медиацентр подтвердил получение.");
            m_socket->disconnect();
            m_socket->disconnectFromHost();
            // Архив больше не нужен
            cleanupArchive();
            m_archiveReady = false;
            sendBtn->setEnabled(false);
            resetUI();
            QTimer::singleShot(0, this, [this]() {
                QMessageBox::information(this, "Готово",
                                         "Обновление успешно доставлено!\n\n"
                                         "На медиацентре появится диалог подтверждения.");
            });
        } else if (line.startsWith("FAIL")) {
            abort("Ошибка на стороне медиацентра:\n" + QString::fromUtf8(line));
        }
    }
}

void UpdateTab::sendChunk()
{
    if (!m_archiveFile) {
        m_archiveFile = new QFile(m_archivePath, this);
        if (!m_archiveFile->open(QIODevice::ReadOnly)) {
            abort("Не удалось открыть архив для отправки"); return;
        }
    }
    // Дописываем, только пока в буфере сокета меньше 8 МБ (иначе на каждый
    // bytesWritten добавлялось ещё 2 МБ и весь архив оседал в памяти)
    constexpr qint64 kMaxBuffered = 8 * 1024 * 1024;
    while (m_socket && m_archiveFile && m_socket->bytesToWrite() < kMaxBuffered) {
        QByteArray chunk = m_archiveFile->read(2 * 1024 * 1024);
        if (chunk.isEmpty()) {
            m_archiveFile->close();
            m_archiveFile->deleteLater();
            m_archiveFile = nullptr;
            return;
        }
        const qint64 written = m_socket->write(chunk);
        if (written < 0) { abort("Ошибка записи в сокет"); return; }
        if (written < chunk.size()) {
            m_archiveFile->seek(m_archiveFile->pos() - (chunk.size() - written));
            return;
        }
    }
}

void UpdateTab::socketBytesWritten(qint64 bytes)
{
    if (!m_socket || m_step != StepSending) return;
    m_bytesSent += bytes;

    if (m_archiveSize > 0)
        progressSend->setValue(static_cast<int>(100.0 * m_bytesSent / m_archiveSize));

    const qint64 elapsed = m_speedTimer.elapsed();
    if (elapsed - m_lastMs >= 1000) {
        const double bps = (m_bytesSent - m_lastBytes) * 1000.0 / qMax(elapsed - m_lastMs, qint64(1));
        const double mbps = bps * 8 / 1'000'000;
        setStatus(QString("Отправка: %1% (%2)")
                      .arg(progressSend->value())
                      .arg(mbps >= 1.0
                               ? QString("%1 Мбит/с").arg(mbps, 0, 'f', 1)
                               : QString("%1 КБ/с").arg(bps / 1024, 0, 'f', 1)));
        m_lastBytes = m_bytesSent;
        m_lastMs    = elapsed;
    }

    if (m_archiveFile)
        sendChunk();
    if (m_bytesSent >= m_archiveSize) {
        if (m_archiveFile) {
            m_archiveFile->close();
            m_archiveFile->deleteLater();
            m_archiveFile = nullptr;
        }
        setStatus("Архив отправлен. Ожидание подтверждения...");
    }
}

void UpdateTab::socketDisconnected()
{
    if (m_step != StepIdle) resetUI();
}

void UpdateTab::socketError(QAbstractSocket::SocketError)
{
    if (m_cancelled) return;
    abort("Сетевая ошибка: " + (m_socket ? m_socket->errorString() : "неизвестно"));
}

// ─────────────────────────────────────────────────────────────────────────────
// Вспомогательные методы
// ─────────────────────────────────────────────────────────────────────────────

void UpdateTab::log(const QString &msg)
{
    logView->append(msg);
    qDebug() << "[UpdateTab]" << msg;
}

void UpdateTab::setStatus(const QString &msg) { statusLabel->setText(msg); }

void UpdateTab::cleanupArchive()
{
    if (!m_archivePath.isEmpty()) {
        QFile::remove(m_archivePath);
        m_archivePath.clear();
    }
    if (!m_tempDir.isEmpty()) {
        // Пока фоновый поток копирует в папку, удалять её нельзя — это сделает
        // обработчик завершения копирования
        if (!m_copyRunning)
            QDir(m_tempDir).removeRecursively();
        m_tempDir.clear();
    }
}

void UpdateTab::cleanup()
{
    if (m_deployProc) {
        m_deployProc->disconnect();
        m_deployProc->kill();
        m_deployProc->deleteLater();
        m_deployProc = nullptr;
    }
    if (m_packProc) {
        m_packProc->disconnect();
        m_packProc->kill();
        m_packProc->deleteLater();
        m_packProc = nullptr;
    }
    if (m_archiveFile) {
        m_archiveFile->close();
        m_archiveFile->deleteLater();
        m_archiveFile = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect();
        if (m_socket->state() == QAbstractSocket::ConnectedState) {
            m_socket->write("CANCEL\n");
            m_socket->flush();
            m_socket->disconnectFromHost();
        }
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    // Готовый архив не удаляем: после сетевой ошибки или отказа на ТВ его
    // можно отправить ещё раз без повторной подготовки (это минуты).
    // Удаляем только недоделанный (отмена во время подготовки).
    if (!m_archiveReady)
        cleanupArchive();
}

void UpdateTab::abort(const QString &reason, bool dialog)
{
    ++m_job;              // результаты уже запущенных фоновых задач больше не нужны
    m_step = StepIdle;
    cleanup();
    resetUI();
    if (dialog && !reason.isEmpty())
        QMessageBox::critical(this, "Ошибка обновления", reason);
    else if (!reason.isEmpty())
        setStatus(reason);
}

void UpdateTab::resetUI()
{
    m_step      = StepIdle;
    m_cancelled = false;
    m_bytesSent = 0;
    m_incoming.clear();
    prepareBtn->setEnabled(true);
    sendBtn->setEnabled(m_archiveReady);
    cancelBtn->setEnabled(false);
    progressPrep->setValue(0);
    progressSend->setValue(0);
    if (!m_archiveReady) setStatus("Готов");
}

QString UpdateTab::sevenZipPath() const
{
    const QString local = QCoreApplication::applicationDirPath() + "/tools/7zr.exe";
    if (QFile::exists(local)) return local;
    const QString local2 = QCoreApplication::applicationDirPath() + "/7zr.exe";
    if (QFile::exists(local2)) return local2;
    return "7zr";
}

QString UpdateTab::winDeployPath() const
{
    QString custom = windeployqtEdit->text().trimmed();
    if (!custom.isEmpty() && QFile::exists(custom)) return custom;

    QProcess p;
    p.start("where.exe", {"windeployqt6.exe"});
    if (p.waitForFinished(3000)) {
        for (const QString &line :
             QString::fromLocal8Bit(p.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts)) {
            QString path = line.trimmed();
            if (QFile::exists(path)) return path;
        }
    }

    const QString qtDir = qEnvironmentVariable("QTDIR");
    if (!qtDir.isEmpty()) {
        const QString path = QDir(qtDir).filePath("bin/windeployqt6.exe");
        if (QFile::exists(path)) return path;
    }
    return QString();
}

QString UpdateTab::computeHash(const QString &filePath) const
{
    QFile f(filePath);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    while (!f.atEnd()) h.addData(f.read(4 * 1024 * 1024));
    return QString::fromLatin1(h.result().toHex());
}

bool UpdateTab::copyDirRecursive(const QString &src, const QString &dst)
{
    QDir srcDir(src);
    if (!srcDir.exists()) return false;
    QDir().mkpath(dst);
    for (const QFileInfo &info : srcDir.entryInfoList(QDir::NoDotAndDotDot | QDir::AllEntries)) {
        const QString dstPath = dst + "/" + info.fileName();
        if (info.isDir()) {
            if (!copyDirRecursive(info.absoluteFilePath(), dstPath)) return false;
        } else {
            if (QFile::exists(dstPath)) QFile::remove(dstPath);
            if (!QFile::copy(info.absoluteFilePath(), dstPath)) return false;
        }
    }
    return true;
}
