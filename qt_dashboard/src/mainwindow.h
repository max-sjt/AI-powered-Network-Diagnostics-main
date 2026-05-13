#pragma once

#include "models.h"
#include "project_log_parser.h"
#include "windows_network_probe.h"

#include <QMainWindow>
#include <QProcess>
#include <QThread>

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class QLabel;
class QTimer;

class ProbeWorker : public QObject {
    Q_OBJECT

public:
    explicit ProbeWorker(QObject* parent = nullptr);

public slots:
    void begin();
    void setTargetHost(const QString& targetHost);
    void refresh();

signals:
    void snapshotReady(const ProbeSnapshot& snapshot);

private:
    WindowsNetworkProbe probe_;
    QString targetHost_;
    QTimer* timer_ = nullptr;
};

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;

signals:
    void requestTargetChange(const QString& targetHost);
    void requestManualRefresh();

private slots:
    void handleSnapshot(const ProbeSnapshot& snapshot);
    void applyTargetHost();
    void loadSampleLog();
    void openLogFile();
    void parseCurrentLog();
    void updateLogSelection();
    void syncRagSourceFromLog();
    void runRagAnalysis();
    void handleRagFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void handleRagError(QProcess::ProcessError error);

private:
    struct MetricCardWidgets {
        QLabel* value = nullptr;
        QLabel* subtitle = nullptr;
    };

    void initializeUi();
    void configureRoleObjectNames();
    void applyTheme();
    void configureTables();
    void configureCharts();
    void wireUi();
    void updateProjectPaths();
    void setRagStatus(const QString& text, bool isError = false);
    void updateOverviewCards(const ProbeSnapshot& snapshot);
    void updateInterfaceTable(const ProbeSnapshot& snapshot);
    void updateLiveNarrative(const ProbeSnapshot& snapshot);
    void pushHistory(QVector<double>& history, double value);
    QString buildDiagnosisReport(const InterfaceSnapshot& iface, const QString& context) const;
    QList<InterfaceSnapshot> aggregateLogMetrics(const QList<MetricRecord>& timeRecords) const;
    void updateLogMetricsView(const QString& timestamp);
    void populateAiTimesFromText(const QString& text, const QString& preferredTime = QString());
    void fillKnowledgeBase();

    Ui::MainWindow* ui_ = nullptr;
    ProjectLogParser parser_;
    QThread* workerThread_ = nullptr;
    ProbeWorker* worker_ = nullptr;
    QProcess* ragProcess_ = nullptr;

    ProbeSnapshot latestSnapshot_;
    QList<MetricRecord> parsedRecords_;
    QVector<double> liveRttHistory_;
    QVector<double> liveTrafficHistory_;
    QString projectRoot_;
    QString ragScriptPath_;
    QString ragTempLogPath_;

    MetricCardWidgets overallCard_;
    MetricCardWidgets interfaceCard_;
    MetricCardWidgets latencyCard_;
    MetricCardWidgets throughputCard_;
};
