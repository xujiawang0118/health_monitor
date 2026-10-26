#ifndef LOCALCLIENT_H
#define LOCALCLIENT_H

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QLocalSocket>

class QTimer;

/*
 * LocalClient — QLocalSocket 的薄封装
 *
 * 职责只有三件事：
 *   1. 连接 / 断线重连（守护进程可能比界面晚启动，也可能被看门狗复位）
 *   2. 按行切帧，把 NDJSON 解析成 QJsonObject 往上抛
 *   3. 发命令并分配 seq，让上层能把响应和请求对上
 *
 * 缺点先说清楚：AF_UNIX 只能本机通信，PC 上的 Qt 连不到板子。
 * PC 端开发时要么在同一台 PC 上跑 mock 守护进程，
 * 要么把协议换成 QTcpSocket 走现有的 :6666。
 *
 * 为什么用 QLocalSocket 而不是自己 open/connect AF_UNIX：
 *   QLocalSocket 继承 QIODevice，自带读缓冲，
 *   canReadLine() / readLine() 直接就拿到了按行切好的帧，
 *   省掉 service 端那套手写 inbuf + memmove 的粘包处理。
 *   代价是只能在 Qt 程序里用（C 侧仍然要自己切）。
 */
class LocalClient : public QObject
{
    Q_OBJECT

public:
    explicit LocalClient(QObject *parent = nullptr);

    /* 开始连接；连接失败会自动每 2 秒重试，直到 disconnectFromDaemon() */
    void connectToDaemon(const QString &path);

    /* 主动断开，并停止自动重连（析构或退出时用） */
    void disconnectFromDaemon();

    bool isConnected() const;

    /*
     * 发送一条命令，自动带上递增的 seq。
     * 返回 seq（> 0）；未连接或写失败返回 0。
     * 响应通过 responseReceived() 回来，调用方自己用超时兜底。
     */
    int sendCommand(const QString &cmd);

signals:
    void connected();
    void disconnected();
    void snapshotReceived(const QJsonObject &obj);
    void responseReceived(int seq, bool ok, const QString &err);
    void errorText(const QString &msg);

private slots:
    void onConnected();
    void onDisconnected();
    void onReadyRead();
    void onSocketError(QLocalSocket::LocalSocketError err);

private:
    void scheduleReconnect();

    QLocalSocket *m_socket;
    QTimer       *m_reconnectTimer;
    QString       m_path;
    bool          m_wantConnected;
    int           m_nextSeq;
};

#endif // LOCALCLIENT_H
