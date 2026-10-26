#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "localclient.h"

#include <QTimer>
#include <QDateTime>
#include <QJsonObject>
#include <QJsonValue>
#include <QVBoxLayout>
#include <QPainter>

#include <QtCharts/QChart>
#include <QtCharts/QLegend>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QDateTimeAxis>
#include <QtCharts/QValueAxis>

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
QT_CHARTS_USE_NAMESPACE
#endif

namespace {
constexpr int kMaxPoints        = 300;  // 2Hz 下约 150 秒
constexpr int kStaleTimeoutMs   = 3000; // 3 秒没有新快照就算数据中断
constexpr int kCmdTimeoutMs     = 2000; // 命令 2 秒没响应就算超时
constexpr int kCmdPollMs        = 200;  // 命令超时检查周期
}

MainWindow::MainWindow(const QString &socketPath, QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
    , m_client(nullptr)
    , m_chart(nullptr)
    , m_hrSeries(nullptr)
    , m_axisX(nullptr)
    , m_axisY(nullptr)
    , m_staleTimer(nullptr)
    , m_cmdTimer(nullptr)
    , m_lastT(0)
{
    /*
     * setupUi 做两件我们依赖的事：
     *   1. 按 .ui 里的对象树把 ui->xxx 成员指针创建出来
     *   2. 调用 QMetaObject::connectSlotsByName(this)，
     *      把 on_pingButton_clicked 这类槽自动连到对应信号
     */
    ui->setupUi(this);

    setupChart();

    m_client = new LocalClient(this);

    m_staleTimer = new QTimer(this);
    m_staleTimer->setSingleShot(true);
    m_staleTimer->setInterval(kStaleTimeoutMs);
    connect(m_staleTimer, &QTimer::timeout, this, &MainWindow::onStaleTimeout);

    m_cmdTimer = new QTimer(this);
    m_cmdTimer->setInterval(kCmdPollMs);
    connect(m_cmdTimer, &QTimer::timeout, this, &MainWindow::onCommandTimeout);

    connect(m_client, &LocalClient::connected,    this, &MainWindow::onClientConnected);
    connect(m_client, &LocalClient::disconnected, this, &MainWindow::onClientDisconnected);
    connect(m_client, &LocalClient::snapshotReceived, this, &MainWindow::onSnapshot);
    connect(m_client, &LocalClient::responseReceived, this, &MainWindow::onResponse);
    connect(m_client, &LocalClient::errorText,    this, &MainWindow::onClientError);

    setStateText(QStringLiteral("未连接"), Qt::red);
    ui->statusbar->showMessage(QStringLiteral("连接 %1").arg(socketPath));

    m_client->connectToDaemon(socketPath);
}

MainWindow::~MainWindow()
{
    // 先断开，避免析构过程中 socket 回调打到半析构的窗口上
    if (m_client)
        m_client->disconnectFromDaemon();
    delete ui;
}

void MainWindow::setupChart()
{
    m_chart = new QChart();
    m_chart->legend()->hide();
    // 2Hz 刷新 + 开动画会导致 X 轴一直在追赶，曲线抖得看不清
    m_chart->setAnimationOptions(QChart::NoAnimation);

    m_hrSeries = new QLineSeries();
    m_hrSeries->setName(QStringLiteral("心率"));
    m_chart->addSeries(m_hrSeries);

    m_axisX = new QDateTimeAxis();
    m_axisX->setFormat(QStringLiteral("hh:mm:ss"));
    m_axisX->setTickCount(5);
    m_axisX->setTitleText(QStringLiteral("时间"));

    m_axisY = new QValueAxis();
    m_axisY->setRange(60, 120);
    m_axisY->setLabelFormat(QStringLiteral("%.0f"));
    m_axisY->setTitleText(QStringLiteral("bpm"));

    m_chart->addAxis(m_axisX, Qt::AlignBottom);
    m_chart->addAxis(m_axisY, Qt::AlignLeft);
    m_hrSeries->attachAxis(m_axisX);
    m_hrSeries->attachAxis(m_axisY);

    /*
     * 为什么 .ui 里放的是普通 QWidget 而不是提升成 QChartView：
     * "提升的类"要求 uic 生成对应头文件 include，而 QtCharts 的命名空间
     * 和头文件名在 Qt5 / Qt6 之间变过（Qt5 是 QtCharts::QChartView，
     * Qt6 取消了命名空间）。提升写死其中一种，换版本就编译不过。
     * 在代码里 new QChartView 则被版本宏一次覆盖，.ui 保持版本无关。
     */
    QChartView *view = new QChartView(m_chart, ui->chartContainer);
    view->setRenderHint(QPainter::Antialiasing);

    QVBoxLayout *layout = new QVBoxLayout(ui->chartContainer);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(view);
}

void MainWindow::onClientConnected()
{
    setStateText(QStringLiteral("已连接，等待数据…"), QColor(0x1a, 0x7f, 0x37));
    ui->statusbar->showMessage(QStringLiteral("已连接守护进程"));
    // 连接建立后服务端会立刻推一帧快照，用这个计时器统一兜底
    m_staleTimer->start();
}

void MainWindow::onClientDisconnected()
{
    setStateText(QStringLiteral("连接断开，重连中…"), Qt::red);
    ui->statusbar->showMessage(QStringLiteral("连接断开"));
    m_staleTimer->stop();
    ui->pingButton->setEnabled(true);
    m_pending.clear();
    m_cmdTimer->stop();
}

void MainWindow::onClientError(const QString &msg)
{
    ui->statusbar->showMessage(msg, 3000);
}

void MainWindow::onStaleTimeout()
{
    /*
     * socket 还连着，但服务端不再推数据。常见原因：
     *   守护进程被看门狗复位（UDS 会跟着断，这条多半是断连先报）
     *   传感器线程卡在 I2C 里，快照线程还在跑但数据不更新（这条能捕获）
     * 界面上必须区分"连接正常"和"数据新鲜"，否则谁也不知道板子还活没活。
     */
    setStateText(QStringLiteral("数据中断"), Qt::darkYellow);
}

void MainWindow::onSnapshot(const QJsonObject &obj)
{
    m_staleTimer->start();

    const qint64 t = qint64(obj.value(QStringLiteral("t")).toDouble());
    const QJsonObject mpu = obj.value(QStringLiteral("mpu")).toObject();
    const QJsonObject ppg = obj.value(QStringLiteral("ppg")).toObject();

    ui->accelValueLabel->setText(
        QStringLiteral("ax %1    ay %2    az %3")
            .arg(mpu.value(QStringLiteral("ax")).toDouble(), 0, 'f', 2)
            .arg(mpu.value(QStringLiteral("ay")).toDouble(), 0, 'f', 2)
            .arg(mpu.value(QStringLiteral("az")).toDouble(), 0, 'f', 2));

    /*
     * 服务端 cJSON 用 AddNumberToObject 写 bool，落地是数字 1/0；
     * 这里同时兼容 true/false 的写法，免得以后改了服务端界面就失灵。
     */
    const QJsonValue validVal = ppg.value(QStringLiteral("valid"));
    const bool valid = validVal.isBool() ? validVal.toBool() : (validVal.toInt() != 0);

    if (valid)
    {
        const int hr   = ppg.value(QStringLiteral("hr")).toInt();
        const int spo2 = ppg.value(QStringLiteral("spo2")).toInt();

        ui->hrValueLabel->setText(QStringLiteral("%1 bpm").arg(hr));
        ui->spo2ValueLabel->setText(QStringLiteral("%1 %").arg(spo2));

        /*
         * t 是 CLOCK_REALTIME 毫秒，板子开 NTP 对时可能往回跳。
         * 时间回退会让 QDateTimeAxis 的区间翻转、曲线画不出来，
         * 所以检测到明显回退就把整条曲线清掉重新开始。
         * 1 秒的容差是为了忽略服务端两次取时的正常抖动。
         */
        if (m_lastT > 0 && t + 1000 < m_lastT)
            clearSeries();

        if (t > 0)
            appendPoint(t, hr);

        ui->hrValueLabel->setStyleSheet(QStringLiteral("color: #1a7f37;"));
    }
    else
    {
        // 手指没放上去或者还在预热，保持占位而不是显示上一次的陈旧数值
        ui->hrValueLabel->setText(QStringLiteral("-- bpm"));
        ui->spo2ValueLabel->setText(QStringLiteral("-- %"));
        ui->hrValueLabel->setStyleSheet(QStringLiteral("color: gray;"));
    }

    m_lastT = t;

    if (m_client->isConnected())
        setStateText(QStringLiteral("已连接"), QColor(0x1a, 0x7f, 0x37));
}

void MainWindow::appendPoint(qint64 tMs, double hr)
{
    if (m_hrSeries->count() >= kMaxPoints)
        m_hrSeries->remove(0);
    m_hrSeries->append(qreal(tMs), hr);
    updateAxes();
}

void MainWindow::updateAxes()
{
    const int n = m_hrSeries->count();
    if (n == 0)
        return;

    /*
     * X 轴用真实时间戳而不是点数索引，这样断连一段时间后曲线会自己留白，
     * 一眼能看出数据缺口在哪。
     */
    const qint64 t0 = qint64(m_hrSeries->at(0).x());
    qint64 t1 = qint64(m_hrSeries->at(n - 1).x());
    if (t1 - t0 < 5000)          // 单点时区间宽度为 0，轴会退化
        t1 = t0 + 5000;
    m_axisX->setRange(QDateTime::fromMSecsSinceEpoch(t0),
                      QDateTime::fromMSecsSinceEpoch(t1));

    double lo = 1e9;
    double hi = -1e9;
    for (int i = 0; i < n; ++i)
    {
        const double y = m_hrSeries->at(i).y();
        lo = qMin(lo, y);
        hi = qMax(hi, y);
    }

    // 留上下边距，同时卡在生理范围内，避免个别毛刺把整条曲线压扁
    lo = qMax(lo - 5.0, 20.0);
    hi = qMin(hi + 5.0, 220.0);
    if (hi - lo < 20.0)
        hi = lo + 20.0;

    m_axisY->setRange(lo, hi);
}

void MainWindow::clearSeries()
{
    m_hrSeries->clear();
    m_axisY->setRange(60, 120);
}

void MainWindow::setStateText(const QString &text, const QColor &color)
{
    ui->stateLabel->setText(text);
    ui->stateLabel->setStyleSheet(QStringLiteral("color: %1;").arg(color.name()));
}

void MainWindow::on_pingButton_clicked()
{
    if (!m_client->isConnected())
    {
        ui->statusbar->showMessage(QStringLiteral("未连接，无法发送"), 2000);
        return;
    }

    const int seq = m_client->sendCommand(QStringLiteral("ping"));
    if (seq <= 0)
    {
        ui->statusbar->showMessage(QStringLiteral("发送失败"), 2000);
        return;
    }

    /*
     * 命令是异步的：seq 先记下来，响应对不上就靠超时兜底。
     * 按钮在响应或超时之前保持禁用，防止连点堆出一串 pending。
     */
    m_pending.insert(seq, QDateTime::currentDateTime());
    ui->pingButton->setEnabled(false);
    if (!m_cmdTimer->isActive())
        m_cmdTimer->start();
    ui->statusbar->showMessage(QStringLiteral("已发送 ping (seq=%1)").arg(seq), 2000);
}

void MainWindow::onResponse(int seq, bool ok, const QString &err)
{
    if (m_pending.remove(seq) <= 0)
        return;   // 超时之后才到的迟到响应，忽略

    ui->pingButton->setEnabled(true);

    if (ok)
        ui->statusbar->showMessage(QStringLiteral("ping 成功 (seq=%1)").arg(seq), 3000);
    else
        ui->statusbar->showMessage(
            QStringLiteral("命令失败 (seq=%1): %2").arg(seq).arg(err), 5000);
}

void MainWindow::onCommandTimeout()
{
    const QDateTime now = QDateTime::currentDateTime();

    // QHash 遍历时不能删元素，先把过期的 key 收集起来
    QList<int> expired;
    for (auto it = m_pending.constBegin(); it != m_pending.constEnd(); ++it)
    {
        if (it.value().msecsTo(now) > kCmdTimeoutMs)
            expired.append(it.key());
    }

    for (int seq : expired)
    {
        m_pending.remove(seq);
        ui->pingButton->setEnabled(true);
        ui->statusbar->showMessage(
            QStringLiteral("命令超时，无响应 (seq=%1，守护进程可能卡住)").arg(seq), 5000);
    }

    if (m_pending.isEmpty())
        m_cmdTimer->stop();
}
