#include "mainwindow.h"

#include "trend_chart_widget.h"
#include "ui_mainwindow.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QTextCursor>
#include <QTextEdit>
#include <QTimer>

namespace {

const InterfaceSnapshot* selectPrimaryInterface(const QList<InterfaceSnapshot>& interfaces) {
    for (const InterfaceSnapshot& iface : interfaces) {
        if (iface.usingNow) {
            return &iface;
        }
    }
    for (const InterfaceSnapshot& iface : interfaces) {
        if (iface.isDefaultRoute) {
            return &iface;
        }
    }
    for (const InterfaceSnapshot& iface : interfaces) {
        if (iface.isUp) {
            return &iface;
        }
    }
    return interfaces.isEmpty() ? nullptr : &interfaces.first();
}

QString formatRtt(int rttMs) {
    return rttMs >= 0 ? QStringLiteral("%1 ms").arg(rttMs) : QStringLiteral("N/A");
}

QString formatLoss(double lossRate) {
    return lossRate >= 0.0 ? QStringLiteral("%1%").arg(QString::number(lossRate, 'f', 1))
                           : QStringLiteral("N/A");
}

QString formatRssi(int rssiDbm) {
    return rssiDbm > -1000 ? QStringLiteral("%1 dBm").arg(rssiDbm) : QStringLiteral("N/A");
}

NetQualityLevel scoreToLevel(double score) {
    if (score >= 90.0) {
        return NetQualityLevel::Excellent;
    }
    if (score >= 75.0) {
        return NetQualityLevel::Good;
    }
    if (score >= 50.0) {
        return NetQualityLevel::Fair;
    }
    return NetQualityLevel::Poor;
}

QString findProjectRoot(const QString& startPath) {
    QDir dir(startPath);
    for (int i = 0; i < 8; ++i) {
        if (dir.exists(QStringLiteral("AI-assisted analysis")) &&
            dir.exists(QStringLiteral("qt_dashboard"))) {
            return dir.absolutePath();
        }
        if (!dir.cdUp()) {
            break;
        }
    }
    return {};
}

void assignObjectName(QWidget* widget, const QString& name) {
    if (widget) {
        widget->setObjectName(name);
    }
}

} // namespace

ProbeWorker::ProbeWorker(QObject* parent)
    : QObject(parent), targetHost_(WindowsNetworkProbe::defaultTargetHost()) {
}

void ProbeWorker::begin() {
    if (!timer_) {
        timer_ = new QTimer(this);
        timer_->setInterval(4000);
        connect(timer_, &QTimer::timeout, this, &ProbeWorker::refresh);
        timer_->start();
    }
    refresh();
}

void ProbeWorker::setTargetHost(const QString& targetHost) {
    const QString trimmed = targetHost.trimmed();
    targetHost_ = trimmed.isEmpty() ? WindowsNetworkProbe::defaultTargetHost() : trimmed;
}

void ProbeWorker::refresh() {
    emit snapshotReady(probe_.collect(targetHost_));
}

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui_(new Ui::MainWindow) {
    ui_->setupUi(this);
    initializeUi();

    ragProcess_ = new QProcess(this);
    connect(ragProcess_,
            qOverload<int, QProcess::ExitStatus>(&QProcess::finished),
            this,
            &MainWindow::handleRagFinished);
    connect(ragProcess_, &QProcess::errorOccurred, this, &MainWindow::handleRagError);

    workerThread_ = new QThread(this);
    worker_ = new ProbeWorker;
    worker_->moveToThread(workerThread_);
    connect(workerThread_, &QThread::started, worker_, &ProbeWorker::begin);
    connect(workerThread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(this, &MainWindow::requestTargetChange, worker_, &ProbeWorker::setTargetHost);
    connect(this, &MainWindow::requestManualRefresh, worker_, &ProbeWorker::refresh);
    connect(worker_, &ProbeWorker::snapshotReady, this, &MainWindow::handleSnapshot);
    workerThread_->start();

    loadSampleLog();
    statusBar()->showMessage(QStringLiteral("Qt Creator 友好的 .ui 版本已启动，正在采集本机网络数据..."));
}

MainWindow::~MainWindow() {
    if (ragProcess_ && ragProcess_->state() != QProcess::NotRunning) {
        ragProcess_->kill();
        ragProcess_->waitForFinished(1500);
    }

    if (workerThread_) {
        workerThread_->quit();
        workerThread_->wait(3000);
    }

    delete ui_;
}

void MainWindow::initializeUi() {
    setWindowTitle(QStringLiteral("WeakNet Visual Dashboard"));
    resize(1500, 960);

    ui_->targetEdit->setText(WindowsNetworkProbe::defaultTargetHost());
    ui_->liveNarrativeEdit->setReadOnly(true);
    ui_->logNarrativeEdit->setReadOnly(true);
    ui_->aiResultEdit->setReadOnly(true);
    ui_->compatibilityEdit->setReadOnly(true);

    overallCard_ = { ui_->overallValueLabel, ui_->overallSubtitleLabel };
    interfaceCard_ = { ui_->interfaceCardValueLabel, ui_->interfaceCardSubtitleLabel };
    latencyCard_ = { ui_->latencyValueLabel, ui_->latencySubtitleLabel };
    throughputCard_ = { ui_->throughputValueLabel, ui_->throughputSubtitleLabel };

    configureRoleObjectNames();
    applyTheme();
    configureTables();
    configureCharts();
    wireUi();
    fillKnowledgeBase();
    updateProjectPaths();
}

void MainWindow::configureRoleObjectNames() {
    assignObjectName(ui_->centralwidget, QStringLiteral("Root"));
    assignObjectName(ui_->heroFrame, QStringLiteral("Hero"));

    for (QWidget* frame : { ui_->overallCardFrame, ui_->interfaceCardFrame, ui_->latencyCardFrame, ui_->throughputCardFrame }) {
        assignObjectName(frame, QStringLiteral("MetricCard"));
    }

    for (QLabel* label : { ui_->overallTitleLabel, ui_->interfaceCardTitleLabel, ui_->latencyTitleLabel, ui_->throughputTitleLabel }) {
        assignObjectName(label, QStringLiteral("MetricTitle"));
    }
    for (QLabel* label : { ui_->overallValueLabel, ui_->interfaceCardValueLabel, ui_->latencyValueLabel, ui_->throughputValueLabel }) {
        assignObjectName(label, QStringLiteral("MetricValue"));
    }
    for (QLabel* label : { ui_->overallSubtitleLabel, ui_->interfaceCardSubtitleLabel, ui_->latencySubtitleLabel, ui_->throughputSubtitleLabel }) {
        assignObjectName(label, QStringLiteral("MetricSubtitle"));
    }

    for (QWidget* frame : {
             ui_->interfaceSectionFrame,
             ui_->eventSectionFrame,
             ui_->liveNarrativeFrame,
             ui_->logInputSectionFrame,
             ui_->logMetricsSectionFrame,
             ui_->logNarrativeSectionFrame,
             ui_->ragControlFrame,
             ui_->aiLogPreviewFrame,
             ui_->aiResultFrame,
             ui_->knowledgeSectionFrame,
             ui_->compatibilitySectionFrame }) {
        assignObjectName(frame, QStringLiteral("SectionCard"));
    }

    for (QLabel* label : {
             ui_->interfaceSectionTitleLabel,
             ui_->eventSectionTitleLabel,
             ui_->liveNarrativeTitleLabel,
             ui_->logInputSectionTitleLabel,
             ui_->logMetricsSectionTitleLabel,
             ui_->logNarrativeSectionTitleLabel,
             ui_->ragControlTitleLabel,
             ui_->aiLogPreviewTitleLabel,
             ui_->aiResultTitleLabel,
             ui_->knowledgeSectionTitleLabel,
             ui_->compatibilitySectionTitleLabel }) {
        assignObjectName(label, QStringLiteral("SectionTitle"));
    }

    assignObjectName(ui_->heroTitleLabel, QStringLiteral("HeroTitle"));
    assignObjectName(ui_->heroSubtitleLabel, QStringLiteral("HeroSubtitle"));
    assignObjectName(ui_->ragStatusLabel, QStringLiteral("StatusNote"));
}

void MainWindow::applyTheme() {
    setStyleSheet(QString::fromUtf8(R"(
QWidget#Root {
    background: #0b1220;
    color: #edf3ff;
}
QFrame#Hero {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:1, stop:0 #16223d, stop:0.45 #0f7a76, stop:1 #0f1728);
    border: 1px solid #25456c;
    border-radius: 24px;
}
QFrame#MetricCard, QFrame#SectionCard {
    background: #111a2b;
    border: 1px solid #223352;
    border-radius: 18px;
}
QLabel#HeroTitle {
    font-size: 28px;
    font-weight: 700;
}
QLabel#HeroSubtitle, QLabel#MetricSubtitle, QLabel#StatusNote {
    color: #9bb0d0;
}
QLabel#MetricTitle {
    color: #8ea3c4;
    font-size: 12px;
    letter-spacing: 1px;
}
QLabel#MetricValue {
    font-size: 28px;
    font-weight: 700;
}
QLabel#SectionTitle {
    font-size: 16px;
    font-weight: 700;
}
QLineEdit, QComboBox, QPlainTextEdit, QTextEdit, QListWidget, QTableWidget {
    background: #0f1727;
    border: 1px solid #223352;
    border-radius: 12px;
    padding: 6px;
    color: #edf3ff;
}
QPushButton {
    background: #1f6feb;
    color: white;
    border: none;
    border-radius: 12px;
    padding: 10px 16px;
    font-weight: 600;
}
QPushButton:hover {
    background: #388bfd;
}
QPushButton:disabled {
    background: #35527a;
    color: #adc0dc;
}
QHeaderView::section {
    background: #152138;
    color: #cfe0ff;
    border: none;
    padding: 8px;
    font-weight: 600;
}
QTableWidget {
    gridline-color: #223352;
    selection-background-color: #204477;
}
QTabWidget::pane {
    border: none;
}
QTabBar::tab {
    background: #10192a;
    color: #9fb5d7;
    padding: 10px 18px;
    margin-right: 6px;
    border-top-left-radius: 12px;
    border-top-right-radius: 12px;
}
QTabBar::tab:selected {
    background: #16213b;
    color: #ffffff;
}
QScrollBar:vertical {
    background: transparent;
    width: 10px;
}
QScrollBar::handle:vertical {
    background: #34507c;
    border-radius: 5px;
}
)"));

    ui_->overallValueLabel->setStyleSheet(QStringLiteral("color:#30c48d;"));
    ui_->interfaceCardValueLabel->setStyleSheet(QStringLiteral("color:#57b5ff;"));
    ui_->latencyValueLabel->setStyleSheet(QStringLiteral("color:#ffb84d;"));
    ui_->throughputValueLabel->setStyleSheet(QStringLiteral("color:#8b8dff;"));
}

void MainWindow::configureTables() {
    auto configureTable = [](QTableWidget* table, const QStringList& headers) {
        table->setColumnCount(headers.size());
        table->setHorizontalHeaderLabels(headers);
        table->horizontalHeader()->setStretchLastSection(true);
        table->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
        table->verticalHeader()->setVisible(false);
        table->setEditTriggers(QAbstractItemView::NoEditTriggers);
        table->setSelectionMode(QAbstractItemView::SingleSelection);
        table->setSelectionBehavior(QAbstractItemView::SelectRows);
        table->setAlternatingRowColors(true);
    };

    configureTable(ui_->interfaceTable, {
        QStringLiteral("接口"),
        QStringLiteral("IPv4"),
        QStringLiteral("状态"),
        QStringLiteral("默认路由"),
        QStringLiteral("RTT"),
        QStringLiteral("丢包"),
        QStringLiteral("RSSI"),
        QStringLiteral("吞吐(MB/s)"),
        QStringLiteral("PPS"),
        QStringLiteral("连接"),
        QStringLiteral("质量"),
        QStringLiteral("关键问题")
    });

    configureTable(ui_->logMetricsTable, {
        QStringLiteral("接口"),
        QStringLiteral("RTT"),
        QStringLiteral("丢包"),
        QStringLiteral("RSSI"),
        QStringLiteral("流量"),
        QStringLiteral("连接"),
        QStringLiteral("质量分"),
        QStringLiteral("诊断重点")
    });

    configureTable(ui_->knowledgeTable, {
        QStringLiteral("能力模块"),
        QStringLiteral("项目来源"),
        QStringLiteral("健康阈值"),
        QStringLiteral("界面呈现"),
        QStringLiteral("建议方向")
    });

    ui_->overviewContentRowLayout->setStretch(0, 3);
    ui_->overviewContentRowLayout->setStretch(1, 4);
    ui_->overviewBottomRowLayout->setStretch(0, 2);
    ui_->overviewBottomRowLayout->setStretch(1, 3);
    ui_->logContentRowLayout->setStretch(0, 3);
    ui_->logContentRowLayout->setStretch(1, 4);
    ui_->ragMainRowLayout->setStretch(0, 3);
    ui_->ragMainRowLayout->setStretch(1, 4);
    ui_->knowledgePageLayout->setStretch(0, 4);
    ui_->knowledgePageLayout->setStretch(1, 3);
}

void MainWindow::configureCharts() {
    ui_->rttChart->setPresentation(QStringLiteral("默认接口 RTT 走势"), QColor(QStringLiteral("#57b5ff")));
    ui_->trafficChart->setPresentation(QStringLiteral("默认接口吞吐走势"), QColor(QStringLiteral("#30c48d")));
    ui_->logRttChart->setPresentation(QStringLiteral("日志 RTT 轨迹"), QColor(QStringLiteral("#57b5ff")));
    ui_->logTrafficChart->setPresentation(QStringLiteral("日志流量轨迹"), QColor(QStringLiteral("#ffb84d")));

    ui_->rttChart->setEmptyText(QStringLiteral("等待实时 RTT 样本..."));
    ui_->trafficChart->setEmptyText(QStringLiteral("等待实时吞吐样本..."));
    ui_->logRttChart->setEmptyText(QStringLiteral("解析日志后显示 RTT 轨迹"));
    ui_->logTrafficChart->setEmptyText(QStringLiteral("解析日志后显示流量轨迹"));
}

void MainWindow::wireUi() {
    connect(ui_->applyTargetButton, &QPushButton::clicked, this, &MainWindow::applyTargetHost);
    connect(ui_->refreshTargetButton, &QPushButton::clicked, this, [this]() { emit requestManualRefresh(); });
    connect(ui_->targetEdit, &QLineEdit::returnPressed, this, &MainWindow::applyTargetHost);

    connect(ui_->sampleLogButton, &QPushButton::clicked, this, &MainWindow::loadSampleLog);
    connect(ui_->openLogButton, &QPushButton::clicked, this, &MainWindow::openLogFile);
    connect(ui_->parseLogButton, &QPushButton::clicked, this, &MainWindow::parseCurrentLog);
    connect(ui_->logTimeCombo, &QComboBox::currentTextChanged, this, &MainWindow::updateLogSelection);

    connect(ui_->syncRagSourceButton, &QPushButton::clicked, this, &MainWindow::syncRagSourceFromLog);
    connect(ui_->runRagButton, &QPushButton::clicked, this, &MainWindow::runRagAnalysis);
}

void MainWindow::updateProjectPaths() {
    projectRoot_ = findProjectRoot(QCoreApplication::applicationDirPath());
    if (projectRoot_.isEmpty()) {
        projectRoot_ = findProjectRoot(QDir::currentPath());
    }

    if (!projectRoot_.isEmpty()) {
        ragScriptPath_ = QDir(projectRoot_).filePath(QStringLiteral("AI-assisted analysis/qt_rag_bridge.py"));
    } else {
        ragScriptPath_.clear();
    }

    if (QFileInfo::exists(ragScriptPath_)) {
        ui_->ragScriptPathEdit->setText(ragScriptPath_);
    } else {
        ui_->ragScriptPathEdit->setText(QStringLiteral("未找到 qt_rag_bridge.py，请确认仓库结构完整"));
    }
}

void MainWindow::setRagStatus(const QString& text, bool isError) {
    ui_->ragStatusLabel->setText(text);
    ui_->ragStatusLabel->setStyleSheet(isError
                                           ? QStringLiteral("color:#ff8e8e;")
                                           : QStringLiteral("color:#9bb0d0;"));
}

void MainWindow::handleSnapshot(const ProbeSnapshot& snapshot) {
    latestSnapshot_ = snapshot;
    updateOverviewCards(snapshot);
    updateInterfaceTable(snapshot);
    updateLiveNarrative(snapshot);

    const InterfaceSnapshot* active = selectPrimaryInterface(snapshot.interfaces);
    if (active) {
        if (active->rttMs >= 0) {
            pushHistory(liveRttHistory_, active->rttMs);
            ui_->rttChart->setSeries(
                liveRttHistory_,
                QStringLiteral("ms"),
                QStringLiteral("当前 %1").arg(formatRtt(active->rttMs)));
        }
        if (active->trafficMBps > 0.0 || !liveTrafficHistory_.isEmpty()) {
            pushHistory(liveTrafficHistory_, active->trafficMBps);
            ui_->trafficChart->setSeries(
                liveTrafficHistory_,
                QStringLiteral("MB/s"),
                QStringLiteral("当前 %1 MB/s").arg(QString::number(active->trafficMBps, 'f', 2)));
        }
    }

    for (const QString& line : snapshot.timeline) {
        ui_->eventList->insertItem(0, line);
    }
    while (ui_->eventList->count() > 40) {
        delete ui_->eventList->takeItem(ui_->eventList->count() - 1);
    }

    statusBar()->showMessage(QStringLiteral("%1 | %2")
                                 .arg(snapshot.capturedAt.toString(QStringLiteral("HH:mm:ss")))
                                 .arg(snapshot.summary));
}

void MainWindow::applyTargetHost() {
    emit requestTargetChange(ui_->targetEdit->text());
    emit requestManualRefresh();
}

void MainWindow::loadSampleLog() {
    ui_->logInputEdit->setPlainText(parser_.sampleLog());
    parseCurrentLog();
    syncRagSourceFromLog();
}

void MainWindow::openLogFile() {
    const QString path = QFileDialog::getOpenFileName(
        this,
        QStringLiteral("选择项目日志"),
        QString(),
        QStringLiteral("Text Files (*.log *.txt);;All Files (*.*)"));
    if (path.isEmpty()) {
        return;
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        statusBar()->showMessage(QStringLiteral("日志打开失败: %1").arg(path));
        return;
    }

    ui_->logInputEdit->setPlainText(QString::fromUtf8(file.readAll()));
    parseCurrentLog();
}

void MainWindow::parseCurrentLog() {
    parsedRecords_ = parser_.parse(ui_->logInputEdit->toPlainText());
    const QStringList times = parser_.availableTimes(parsedRecords_);

    ui_->logTimeCombo->blockSignals(true);
    ui_->logTimeCombo->clear();
    ui_->logTimeCombo->addItems(times);
    ui_->logTimeCombo->blockSignals(false);

    if (!times.isEmpty()) {
        ui_->logTimeCombo->setCurrentIndex(times.size() - 1);
        updateLogSelection();
        statusBar()->showMessage(QStringLiteral("日志解析完成，共识别 %1 条指标").arg(parsedRecords_.size()));
    } else {
        ui_->logMetricsTable->setRowCount(0);
        ui_->logNarrativeEdit->setPlainText(QStringLiteral("未解析出项目日志格式，请粘贴 log_capture.py 的输出文本。"));
        ui_->logRttChart->setSeries({}, QStringLiteral("ms"));
        ui_->logTrafficChart->setSeries({}, QStringLiteral("MB/s"));
    }
}

void MainWindow::updateLogSelection() {
    const QString selectedTime = ui_->logTimeCombo->currentText();
    if (!selectedTime.isEmpty()) {
        const int aiIndex = ui_->ragTimeCombo->findText(selectedTime);
        if (aiIndex >= 0) {
            ui_->ragTimeCombo->setCurrentIndex(aiIndex);
        }
    }
    updateLogMetricsView(selectedTime);
}

void MainWindow::syncRagSourceFromLog() {
    ui_->aiLogPreviewEdit->setPlainText(ui_->logInputEdit->toPlainText());
    populateAiTimesFromText(ui_->aiLogPreviewEdit->toPlainText(), ui_->logTimeCombo->currentText());
    setRagStatus(QStringLiteral("已从“项目日志回放”页同步日志和时间点"));
    ui_->mainTabWidget->setCurrentWidget(ui_->aiPage);
}

void MainWindow::runRagAnalysis() {
    if (ragProcess_->state() != QProcess::NotRunning) {
        setRagStatus(QStringLiteral("上一轮 RAG 分析还在运行中，请稍候..."));
        return;
    }

    updateProjectPaths();
    if (!QFileInfo::exists(ragScriptPath_)) {
        setRagStatus(QStringLiteral("未找到 Python RAG 桥接脚本，请确认仓库中存在 AI-assisted analysis/qt_rag_bridge.py"), true);
        ui_->aiResultEdit->setPlainText(QStringLiteral("脚本入口不存在，无法启动 Python RAG 分析。"));
        return;
    }

    const QString logText = ui_->aiLogPreviewEdit->toPlainText().trimmed();
    if (logText.isEmpty()) {
        setRagStatus(QStringLiteral("AI 页没有日志内容，先点“从日志页同步”或直接粘贴文本"), true);
        return;
    }

    populateAiTimesFromText(logText, ui_->ragTimeCombo->currentText());
    const QString timePoint = ui_->ragTimeCombo->currentText().trimmed();
    if (timePoint.isEmpty()) {
        setRagStatus(QStringLiteral("当前日志中没有可用时间点，无法执行 AI RAG 分析"), true);
        return;
    }

    const QString tempDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    ragTempLogPath_ = QDir(tempDir).filePath(QStringLiteral("weaknet_qt_rag_input.log"));

    QFile tempFile(ragTempLogPath_);
    if (!tempFile.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        setRagStatus(QStringLiteral("无法创建临时日志文件，AI RAG 分析已取消"), true);
        return;
    }
    tempFile.write(logText.toUtf8());
    tempFile.close();

    const QString program = ui_->pythonCommandEdit->text().trimmed().isEmpty()
        ? QStringLiteral("python")
        : ui_->pythonCommandEdit->text().trimmed();

    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert(QStringLiteral("PYTHONIOENCODING"), QStringLiteral("utf-8"));
    const QString apiKey = ui_->apiKeyEdit->text().trimmed();
    if (!apiKey.isEmpty()) {
        env.insert(QStringLiteral("DASHSCOPE_API_KEY"), apiKey);
    }

    ragProcess_->setProcessEnvironment(env);
    ragProcess_->setWorkingDirectory(QFileInfo(ragScriptPath_).absolutePath());
    ragProcess_->setProgram(program);
    ragProcess_->setArguments({
        ragScriptPath_,
        QStringLiteral("analyze"),
        QStringLiteral("--log-file"),
        ragTempLogPath_,
        QStringLiteral("--time-point"),
        timePoint
    });

    ui_->runRagButton->setEnabled(false);
    ui_->syncRagSourceButton->setEnabled(false);
    ui_->aiResultEdit->setPlainText(QStringLiteral("正在调用 Python RAG 分析脚本，请稍候..."));
    setRagStatus(QStringLiteral("正在用 %1 分析 %2").arg(program, timePoint));
    ragProcess_->start();
}

void MainWindow::handleRagFinished(int exitCode, QProcess::ExitStatus exitStatus) {
    ui_->runRagButton->setEnabled(true);
    ui_->syncRagSourceButton->setEnabled(true);

    const QString stdOut = QString::fromUtf8(ragProcess_->readAllStandardOutput()).trimmed();
    const QString stdErr = QString::fromUtf8(ragProcess_->readAllStandardError()).trimmed();

    QString resultText = stdOut;
    if (!stdErr.isEmpty()) {
        if (!resultText.isEmpty()) {
            resultText += QStringLiteral("\n\n");
        }
        resultText += QStringLiteral("[Python 运行信息]\n") + stdErr;
    }

    if (exitStatus == QProcess::NormalExit && exitCode == 0) {
        if (resultText.isEmpty()) {
            resultText = QStringLiteral("Python RAG 已执行，但没有返回内容。");
        }
        ui_->aiResultEdit->setPlainText(resultText);
        setRagStatus(QStringLiteral("AI RAG 分析完成"));
    } else {
        if (resultText.isEmpty()) {
            resultText = QStringLiteral("Python RAG 分析失败，未返回可读输出。");
        }
        ui_->aiResultEdit->setPlainText(resultText);
        setRagStatus(QStringLiteral("AI RAG 分析失败，退出码 %1").arg(exitCode), true);
    }

    ui_->aiResultEdit->moveCursor(QTextCursor::Start);
}

void MainWindow::handleRagError(QProcess::ProcessError error) {
    Q_UNUSED(error);
    ui_->runRagButton->setEnabled(true);
    ui_->syncRagSourceButton->setEnabled(true);

    const QString stderrText = QString::fromUtf8(ragProcess_->readAllStandardError()).trimmed();
    ui_->aiResultEdit->setPlainText(stderrText.isEmpty()
                                        ? QStringLiteral("Python 进程启动失败，请确认 Python 命令和依赖是否可用。")
                                        : stderrText);
    setRagStatus(QStringLiteral("Python 进程启动失败，请检查 Python 命令"), true);
}

void MainWindow::updateOverviewCards(const ProbeSnapshot& snapshot) {
    const InterfaceSnapshot* active = selectPrimaryInterface(snapshot.interfaces);

    overallCard_.value->setText(QStringLiteral("%1  %2")
                                    .arg(qualityLevelLabel(snapshot.overallQuality.level),
                                         QString::number(snapshot.overallQuality.score, 'f', 1)));
    overallCard_.value->setStyleSheet(
        QStringLiteral("color:%1;").arg(qualityLevelColor(snapshot.overallQuality.level).name()));
    overallCard_.subtitle->setText(snapshot.summary);

    interfaceCard_.value->setText(active ? active->name : QStringLiteral("无"));
    interfaceCard_.subtitle->setText(active
                                         ? QStringLiteral("%1 | %2")
                                               .arg(active->ipv4.isEmpty() ? QStringLiteral("无 IPv4") : active->ipv4,
                                                    active->isWifi && !active->wifiSsid.isEmpty()
                                                        ? QStringLiteral("Wi-Fi / %1").arg(active->wifiSsid)
                                                        : (active->isWifi ? QStringLiteral("Wi-Fi") : QStringLiteral("以太网")))
                                         : QStringLiteral("未找到可用接口"));

    latencyCard_.value->setText(active
                                    ? QStringLiteral("%1 / %2")
                                          .arg(formatRtt(active->rttMs), formatLoss(active->packetLossRate))
                                    : QStringLiteral("N/A"));
    latencyCard_.subtitle->setText(active
                                       ? QStringLiteral("目标 %1，RSSI %2")
                                             .arg(snapshot.targetHost, formatRssi(active->rssiDbm))
                                       : QStringLiteral("未获取到 RTT 和丢包数据"));

    throughputCard_.value->setText(active
                                       ? QStringLiteral("%1 MB/s")
                                             .arg(QString::number(active->trafficMBps, 'f', 2))
                                       : QStringLiteral("0.00 MB/s"));
    throughputCard_.subtitle->setText(active
                                          ? QStringLiteral("%1 pps | %2 connections")
                                                .arg(active->packetsPerSecond)
                                                .arg(active->activeConnections)
                                          : QStringLiteral("等待流量样本"));
}

void MainWindow::updateInterfaceTable(const ProbeSnapshot& snapshot) {
    ui_->interfaceTable->setRowCount(snapshot.interfaces.size());

    for (int row = 0; row < snapshot.interfaces.size(); ++row) {
        const InterfaceSnapshot& iface = snapshot.interfaces[row];
        auto setText = [this, row](int column, const QString& text, const QColor& color = QColor()) {
            auto* item = new QTableWidgetItem(text);
            if (color.isValid()) {
                item->setForeground(color);
            }
            ui_->interfaceTable->setItem(row, column, item);
        };

        setText(0, iface.name);
        setText(1, iface.ipv4.isEmpty() ? QStringLiteral("-") : iface.ipv4);
        setText(2, iface.isUp ? QStringLiteral("UP") : QStringLiteral("DOWN"),
                iface.isUp ? QColor(QStringLiteral("#30c48d")) : QColor(QStringLiteral("#ff6b6b")));
        setText(3, iface.isDefaultRoute ? QStringLiteral("YES") : QStringLiteral("NO"));
        setText(4, formatRtt(iface.rttMs));
        setText(5, formatLoss(iface.packetLossRate));
        setText(6, formatRssi(iface.rssiDbm));
        setText(7, QString::number(iface.trafficMBps, 'f', 2));
        setText(8, QString::number(iface.packetsPerSecond));
        setText(9, QString::number(iface.activeConnections));
        setText(10,
                QStringLiteral("%1 (%2)")
                    .arg(qualityLevelLabel(iface.quality.level))
                    .arg(QString::number(iface.quality.score, 'f', 1)),
                qualityLevelColor(iface.quality.level));
        setText(11, iface.quality.issues.join(QStringLiteral("；")));
    }
}

void MainWindow::updateLiveNarrative(const ProbeSnapshot& snapshot) {
    const InterfaceSnapshot* active = selectPrimaryInterface(snapshot.interfaces);
    if (!active) {
        ui_->liveNarrativeEdit->setPlainText(QStringLiteral("当前没有可用于诊断的网络接口。"));
        return;
    }

    ui_->liveNarrativeEdit->setPlainText(buildDiagnosisReport(*active, QStringLiteral("实时诊断")));
}

void MainWindow::pushHistory(QVector<double>& history, double value) {
    history.push_back(value);
    while (history.size() > 40) {
        history.removeFirst();
    }
}

QString MainWindow::buildDiagnosisReport(const InterfaceSnapshot& iface, const QString& context) const {
    QString report;
    report += QStringLiteral("%1\n").arg(context);
    report += QStringLiteral("接口: %1\n").arg(iface.name);
    report += QStringLiteral("综合评分: %1 (%2)\n")
                  .arg(QString::number(iface.quality.score, 'f', 1), qualityLevelLabel(iface.quality.level));
    report += QStringLiteral("RTT: %1，TCP 丢包: %2，RSSI: %3，吞吐: %4 MB/s\n\n")
                  .arg(formatRtt(iface.rttMs),
                       formatLoss(iface.packetLossRate),
                       formatRssi(iface.rssiDbm),
                       QString::number(iface.trafficMBps, 'f', 2));

    report += QStringLiteral("观察:\n");
    for (const QString& issue : iface.quality.issues) {
        report += QStringLiteral("- %1\n").arg(issue);
    }

    report += QStringLiteral("\n建议:\n");
    if (iface.rttMs > 200) {
        report += QStringLiteral("- RTT 已进入高延迟区间，优先检查链路拥塞、VPN 或 DNS 解析路径。\n");
    }
    if (iface.packetLossRate > 2.0) {
        report += QStringLiteral("- 丢包偏高，建议排查网卡驱动、交换设备端口和上游链路质量。\n");
    }
    if (iface.isWifi && iface.rssiDbm < -70) {
        report += QStringLiteral("- Wi-Fi 信号较弱，建议缩短距离、切换频段或排查干扰源。\n");
    }
    if (iface.trafficMBps <= 0.0 && iface.activeConnections > 0) {
        report += QStringLiteral("- 连接存在但吞吐趋近于 0，建议抓包或检查代理/防火墙策略。\n");
    }
    if (iface.quality.level == NetQualityLevel::Excellent || iface.quality.level == NetQualityLevel::Good) {
        report += QStringLiteral("- 当前主要指标稳定，可以继续观察趋势而不必立即干预。\n");
    }

    report += QStringLiteral("\n诊断依据:\n");
    report += QStringLiteral("- 评分权重继承项目 server/network_quality_assessor.cpp 的 RTT、丢包、RSSI、流量四因子思路。\n");
    report += QStringLiteral("- 项目日志回放兼容 AI-assisted analysis/log_capture.py 的输出格式。\n");
    report += QStringLiteral("- AI 页通过 Python 脚本联动 AI-assisted analysis/local_vector_rag_analyzer.py。\n");
    report += QStringLiteral("- Windows 实时采样使用本机接口信息、ICMP 和 WLAN API 完成。");
    return report;
}

QList<InterfaceSnapshot> MainWindow::aggregateLogMetrics(const QList<MetricRecord>& timeRecords) const {
    QMap<QString, QList<MetricRecord>> grouped;
    for (const MetricRecord& record : timeRecords) {
        grouped[record.interfaceName].push_back(record);
    }

    QList<InterfaceSnapshot> result;
    for (auto it = grouped.begin(); it != grouped.end(); ++it) {
        InterfaceSnapshot iface;
        iface.name = it.key();
        iface.isUp = true;

        double rttTotal = 0.0;
        int rttCount = 0;
        double lossTotal = 0.0;
        int lossCount = 0;
        double trafficTotal = 0.0;
        int trafficCount = 0;
        int rssiTotal = 0;
        int rssiCount = 0;
        double scoreTotal = 0.0;
        int scoreCount = 0;

        for (const MetricRecord& record : it.value()) {
            iface.usingNow = iface.usingNow || record.usingFlag;
            if (record.rttMs >= 0.0) {
                rttTotal += record.rttMs;
                ++rttCount;
            }
            if (record.tcpLossRate >= 0.0) {
                lossTotal += record.tcpLossRate;
                ++lossCount;
            }
            if (record.trafficMBps >= 0.0) {
                trafficTotal += record.trafficMBps;
                ++trafficCount;
            }
            if (record.rssiDbm > -1000) {
                rssiTotal += record.rssiDbm;
                ++rssiCount;
                iface.isWifi = true;
            }
            if (record.qualityScore >= 0.0) {
                scoreTotal += record.qualityScore;
                ++scoreCount;
            }
            if (record.flows >= 0) {
                iface.activeConnections = qMax(iface.activeConnections, static_cast<quint32>(record.flows));
            }
            if (record.pps >= 0) {
                iface.packetsPerSecond = qMax<quint64>(iface.packetsPerSecond, static_cast<quint64>(record.pps));
            }
        }

        iface.rttMs = rttCount > 0 ? qRound(rttTotal / rttCount) : -1;
        iface.packetLossRate = lossCount > 0 ? lossTotal / lossCount : -1.0;
        iface.trafficMBps = trafficCount > 0 ? trafficTotal / trafficCount : 0.0;
        iface.rssiDbm = rssiCount > 0 ? qRound(rssiTotal / static_cast<double>(rssiCount)) : -1000;
        iface.quality = assessQualityMetrics(iface.rttMs,
                                             iface.packetLossRate,
                                             iface.rssiDbm,
                                             iface.trafficMBps,
                                             iface.activeConnections,
                                             true);
        if (scoreCount > 0) {
            iface.quality.score = scoreTotal / scoreCount;
            iface.quality.level = scoreToLevel(iface.quality.score);
            iface.quality.levelName = qualityLevelName(iface.quality.level);
        }
        result.push_back(iface);
    }

    return result;
}

void MainWindow::updateLogMetricsView(const QString& timestamp) {
    if (timestamp.isEmpty()) {
        return;
    }

    const QList<MetricRecord> timeRecords = parser_.recordsForTime(parsedRecords_, timestamp);
    const QList<InterfaceSnapshot> interfaces = aggregateLogMetrics(timeRecords);

    ui_->logMetricsTable->setRowCount(interfaces.size());
    for (int row = 0; row < interfaces.size(); ++row) {
        const InterfaceSnapshot& iface = interfaces[row];
        auto setText = [this, row](int column, const QString& text, const QColor& color = QColor()) {
            auto* item = new QTableWidgetItem(text);
            if (color.isValid()) {
                item->setForeground(color);
            }
            ui_->logMetricsTable->setItem(row, column, item);
        };

        setText(0, iface.name);
        setText(1, formatRtt(iface.rttMs));
        setText(2, formatLoss(iface.packetLossRate));
        setText(3, formatRssi(iface.rssiDbm));
        setText(4, QString::number(iface.trafficMBps, 'f', 2) + QStringLiteral(" MB/s"));
        setText(5, QString::number(iface.activeConnections));
        setText(6,
                QStringLiteral("%1 (%2)")
                    .arg(qualityLevelLabel(iface.quality.level))
                    .arg(QString::number(iface.quality.score, 'f', 1)),
                qualityLevelColor(iface.quality.level));
        setText(7, iface.quality.issues.join(QStringLiteral("；")));
    }

    const InterfaceSnapshot* primary = selectPrimaryInterface(interfaces);
    if (primary) {
        ui_->logNarrativeEdit->setPlainText(buildDiagnosisReport(*primary, QStringLiteral("项目日志回放")));
        QVector<double> rttSeries;
        QVector<double> trafficSeries;
        for (const MetricRecord& record : parsedRecords_) {
            if (record.interfaceName != primary->name) {
                continue;
            }
            if (record.rttMs >= 0.0) {
                rttSeries.push_back(record.rttMs);
            }
            if (record.trafficMBps >= 0.0) {
                trafficSeries.push_back(record.trafficMBps);
            }
        }
        ui_->logRttChart->setSeries(
            rttSeries,
            QStringLiteral("ms"),
            QStringLiteral("时间点 %1").arg(timestamp));
        ui_->logTrafficChart->setSeries(
            trafficSeries,
            QStringLiteral("MB/s"),
            QStringLiteral("接口 %1").arg(primary->name));
    } else {
        ui_->logNarrativeEdit->setPlainText(QStringLiteral("该时间点没有可用的指标。"));
        ui_->logRttChart->setSeries({}, QStringLiteral("ms"));
        ui_->logTrafficChart->setSeries({}, QStringLiteral("MB/s"));
    }
}

void MainWindow::populateAiTimesFromText(const QString& text, const QString& preferredTime) {
    const QList<MetricRecord> aiRecords = parser_.parse(text);
    const QStringList times = parser_.availableTimes(aiRecords);

    const QString selectedTime = preferredTime.isEmpty() ? ui_->ragTimeCombo->currentText() : preferredTime;
    ui_->ragTimeCombo->blockSignals(true);
    ui_->ragTimeCombo->clear();
    ui_->ragTimeCombo->addItems(times);
    const int index = ui_->ragTimeCombo->findText(selectedTime);
    if (index >= 0) {
        ui_->ragTimeCombo->setCurrentIndex(index);
    } else if (!times.isEmpty()) {
        ui_->ragTimeCombo->setCurrentIndex(times.size() - 1);
    }
    ui_->ragTimeCombo->blockSignals(false);
}

void MainWindow::fillKnowledgeBase() {
    struct Row {
        QString capability;
        QString source;
        QString threshold;
        QString presentation;
        QString recommendation;
    };

    const QList<Row> rows = {
        { QStringLiteral("RTT 延迟"),
          QStringLiteral("server/network_quality_assessor.cpp"),
          QStringLiteral("优秀 <= 50ms；一般 <= 200ms"),
          QStringLiteral("实时卡片 + 曲线图 + 日志回放"),
          QStringLiteral("检查拥塞、路由、DNS、VPN") },
        { QStringLiteral("TCP 丢包"),
          QStringLiteral("client/weaknet_client.h + AI 日志解析"),
          QStringLiteral("优秀 <= 0.1%；一般 <= 2%"),
          QStringLiteral("实时卡片 + 表格 + 日志诊断"),
          QStringLiteral("检查链路质量、交换机端口、驱动") },
        { QStringLiteral("RSSI / Wi-Fi"),
          QStringLiteral("server/include/net_info.hpp"),
          QStringLiteral("优秀 >= -50dBm；较差 < -70dBm"),
          QStringLiteral("无线接口识别 + 质量加权"),
          QStringLiteral("优化位置、频段、干扰源") },
        { QStringLiteral("流量 / PPS"),
          QStringLiteral("server/src/server.cpp 的 ACTIVE 汇总"),
          QStringLiteral("连续非零吞吐更利于质量评分"),
          QStringLiteral("实时吞吐曲线 + 表格指标"),
          QStringLiteral("检查带宽利用、代理、阻塞") },
        { QStringLiteral("综合质量"),
          QStringLiteral("network_quality_assessor 评分思路"),
          QStringLiteral(">=90 优秀；>=75 良好；>=50 一般"),
          QStringLiteral("首页大卡片 + 每接口评分"),
          QStringLiteral("结合 RTT/丢包/RSSI/流量综合决策") },
        { QStringLiteral("AI RAG 诊断"),
          QStringLiteral("AI-assisted analysis/qt_rag_bridge.py + local_vector_rag_analyzer.py"),
          QStringLiteral("按时间点进行知识库增强诊断"),
          QStringLiteral(".ui 页面内直接调用 Python 脚本"),
          QStringLiteral("定位某一时刻问题、输出解决建议") }
    };

    ui_->knowledgeTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        ui_->knowledgeTable->setItem(row, 0, new QTableWidgetItem(rows[row].capability));
        ui_->knowledgeTable->setItem(row, 1, new QTableWidgetItem(rows[row].source));
        ui_->knowledgeTable->setItem(row, 2, new QTableWidgetItem(rows[row].threshold));
        ui_->knowledgeTable->setItem(row, 3, new QTableWidgetItem(rows[row].presentation));
        ui_->knowledgeTable->setItem(row, 4, new QTableWidgetItem(rows[row].recommendation));
    }

    ui_->compatibilityEdit->setPlainText(QStringLiteral(
        "这套界面现在已经改成 Qt Creator 更友好的 .ui 版本：\n\n"
        "1. 主界面结构\n"
        "主窗口布局、Tab 页、输入框、按钮、表格和文本区域都在 mainwindow.ui 里，可以直接用 Qt Creator 打开并拖拽调整。\n\n"
        "2. 自定义控件\n"
        "趋势图 TrendChartWidget 仍然是代码控件，但已经支持 Qt Designer / Promote 用法，所以不会影响你继续拖拽布局。\n\n"
        "3. AI 诊断联动\n"
        "AI RAG 诊断页通过 QProcess 调用 AI-assisted analysis/qt_rag_bridge.py，再桥接到 local_vector_rag_analyzer.py。你可以直接在界面里运行 Python 诊断，而不是手动切终端。\n\n"
        "4. 为什么还是保留代码逻辑\n"
        "实时采样、质量评分、日志聚合和 Python 进程控制更适合放在 C++ 逻辑层；.ui 负责界面结构，代码负责动态数据和行为绑定，这样后续维护会更稳。\n\n"
        "5. Windows 兼容策略\n"
        "Linux 侧原本依赖 DBus 和 eBPF，Windows 端继续使用本机接口采样 + 项目日志回放 + Python RAG 联动三条链路，所以它能在 Win 环境真正跑起来。"));
}
