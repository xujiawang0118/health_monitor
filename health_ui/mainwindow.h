#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QColor>
#include <QDateTime>
#include <QHash>
#include <QJsonObject>

/*
 * Qt Charts 在 Qt5 里放在 QtCharts 命名空间内，
 * Qt6 里取消了命名空间。这里用版本宏把差异吃掉，
 * 头文件只做前向声明，避免把图表头文件传播给每个包含者。
 */
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#include <QtCharts/QChartGlobal>
QT_CHARTS_BEGIN_NAMESPACE
class QChart;
class QLineSeries;
class QDateTimeAxis;
class QValueAxis;
QT_CHARTS_END_NAMESPACE
QT_CHARTS_USE_NAMESPACE
#else
class QChart;
class QLineSeries;
class QDateTimeAxis;
class QValueAxis;
#endif

class QTimer;
class LocalClient;

namespace Ui {
class MainWindow;
}

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(const QString &socketPath, QWidget *parent = nullptr);
    ~MainWindow() override;

private slots:
    /*
     * 名字必须严格写成 on_<objectName>_<signal>()，
     * 这是 ui->setupUi() 内部调用 QMetaObject::connectSlotsByName()
     * 时的匹配规则。objectName 一旦在 Designer 里改掉，
     * 这个槽就静默失联 —— 编译不报错，点了没反应，非常难查。
     */
    void on_pingButton_clicked();

    void onClientConnected();
    void onClientDisconnected();
    void onSnapshot(const QJsonObject &obj);
    void onResponse(int seq, bool ok, const QString &err);
    void onClientError(const QString &msg);
    void onStaleTimeout();
    void onCommandTimeout();

private:
    void setupChart();
    void appendPoint(qint64 tMs, double hr);
    void updateAxes();
    void setStateText(const QString &text, const QColor &color);
    void clearSeries();

    Ui::MainWindow *ui;
    LocalClient    *m_client;

    QChart         *m_chart;
    QLineSeries    *m_hrSeries;
    QDateTimeAxis  *m_axisX;
    QValueAxis     *m_axisY;

    QTimer *m_staleTimer;    // 数据停滞检测：socket 活着不代表数据在流
    QTimer *m_cmdTimer;      // 命令超时兜底

    QHash<int, QDateTime> m_pending;  // seq → 发出时刻
    qint64 m_lastT;                   // 上一帧的 t，用于检测时间回退
};

#endif // MAINWINDOW_H
