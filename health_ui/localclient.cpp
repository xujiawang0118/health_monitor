#include "localclient.h"

#include <QTimer>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QJsonObject>
#include <QLocalSocket>

namespace {
constexpr int kReconnectIntervalMs = 2000;
}

LocalClient::LocalClient(QObject *parent)
    : QObject(parent)
    , m_socket(new QLocalSocket(this))
    , m_reconnectTimer(new QTimer(this))
    , m_wantConnected(false)
    , m_nextSeq(1)
{
    m_reconnectTimer->setSingleShot(true);
    m_reconnectTimer->setInterval(kReconnectIntervalMs);
    connect(m_reconnectTimer, &QTimer::timeout, this, [this]() {
        if (m_wantConnected && m_socket->state() == QLocalSocket::UnconnectedState)
            m_socket->connectToServer(m_path);
    });

    connect(m_socket, &QLocalSocket::connected,    this, &LocalClient::onConnected);
    connect(m_socket, &QLocalSocket::disconnected, this, &LocalClient::onDisconnected);
    connect(m_socket, &QLocalSocket::readyRead,    this, &LocalClient::onReadyRead);

    /*
     * errorOccurred 是 Qt 5.15 才有的信号；更老的 5.x 里
     * error(LocalSocketError) 和 error() 重载过，必须用 QOverload 选中
     * 带参数的那个，否则取函数地址会编译失败。
     */
#if QT_VERSION >= QT_VERSION_CHECK(5, 15, 0)
    connect(m_socket, &QLocalSocket::errorOccurred, this, &LocalClient::onSocketError);
#else
    connect(m_socket, QOverload<QLocalSocket::LocalSocketError>::of(&QLocalSocket::error),
            this, &LocalClient::onSocketError);
#endif
}

void LocalClient::connectToDaemon(const QString &path)
{
    m_path = path;
    m_wantConnected = true;

    /*
     * abort() 而不是 disconnectFromServer()：
     * abort 会立刻关闭 socket 并清空内部读写缓冲，
     * 避免残留半帧数据被下一次连接读到（协议错位比断线更难查）。
     */
    m_socket->abort();
    m_socket->connectToServer(m_path);
}

void LocalClient::disconnectFromDaemon()
{
    m_wantConnected = false;
    m_reconnectTimer->stop();
    m_socket->abort();
}

bool LocalClient::isConnected() const
{
    return m_socket->state() == QLocalSocket::ConnectedState;
}

int LocalClient::sendCommand(const QString &cmd)
{
    if (m_socket->state() != QLocalSocket::ConnectedState)
        return 0;

    const int seq = m_nextSeq++;

    QJsonObject root;
    root.insert(QStringLiteral("seq"), seq);
    root.insert(QStringLiteral("cmd"), cmd);

    QByteArray frame = QJsonDocument(root).toJson(QJsonDocument::Compact);
    frame.append('\n');   // 服务端按 '\n' 切帧，少这个换行会被一直憋在缓冲里

    if (m_socket->write(frame) != frame.size())
        return 0;
    m_socket->flush();

    return seq;
}

void LocalClient::onConnected()
{
    m_reconnectTimer->stop();
    emit connected();
}

void LocalClient::onDisconnected()
{
    emit disconnected();
    scheduleReconnect();
}

void LocalClient::onSocketError(QLocalSocket::LocalSocketError err)
{
    /*
     * 连接失败时 errorOccurred 先到，disconnected 不一定跟着来；
     * 而断开时两个信号可能都到位。scheduleReconnect 内部有
     * isActive() 判断，重复调用不会堆出多个重连。
     */
    Q_UNUSED(err);
    emit errorText(m_socket->errorString());
    scheduleReconnect();
}

void LocalClient::scheduleReconnect()
{
    if (!m_wantConnected)
        return;
    if (m_reconnectTimer->isActive())
        return;
    m_reconnectTimer->start();
}

void LocalClient::onReadyRead()
{
    /*
     * 走到这里不代表能一次读完整帧：>=2KB 或跨 TCP/Unix socket 边界时，
     * 一帧可能被拆成两次 readyRead。canReadLine() 只在缓冲里确实有 '\n'
     * 时才返回 true，正好把半个包留到下次，不用自己维护偏移量。
     */
    while (m_socket->canReadLine())
    {
        const QByteArray line = m_socket->readLine();
        if (line.trimmed().isEmpty())
            continue;

        QJsonParseError perr;
        const QJsonDocument doc = QJsonDocument::fromJson(line, &perr);
        if (perr.error != QJsonParseError::NoError || !doc.isObject())
        {
            // 单帧坏掉不该拖垮整个连接：扔掉这一行继续读下一行
            emit errorText(QStringLiteral("非法 JSON 帧: %1").arg(perr.errorString()));
            continue;
        }

        const QJsonObject obj = doc.object();

        /*
         * 两类上行帧靠字段区分：
         *   响应 {"seq":n,"ok":true|false[,"err":"..."]}
         *   快照 {"t":毫秒,"mpu":{...},"ppg":{...}}
         * 服务端目前只有这两类，用"有没有 seq+ok"来分流足够；
         * 以后加新类型建议补一个 "type" 字段，别再靠猜。
         */
        if (obj.contains(QStringLiteral("seq")) && obj.contains(QStringLiteral("ok")))
        {
            emit responseReceived(obj.value(QStringLiteral("seq")).toInt(),
                                  obj.value(QStringLiteral("ok")).toBool(),
                                  obj.value(QStringLiteral("err")).toString());
        }
        else if (obj.contains(QStringLiteral("t")))
        {
            emit snapshotReceived(obj);
        }
    }
}
