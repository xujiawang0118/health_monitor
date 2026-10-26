#include "mainwindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QCommandLineOption>
#include <QString>

namespace {
/*
 * 默认路径必须和板端 include/config.h 里的 LOCAL_SOCK_PATH 保持一致。
 * 改地址优先级从高到低：
 *   命令行 --socket /xxx.sock
 *   环境变量 HEALTH_MONITOR_SOCK
 *   上面这个编译期常量
 *
 * 守护进程侧用的是同一套规则（LOCAL_SOCK_ENV），所以两边要么都不设，
 * 要么设成同一个值 —— 最常见的故障就是一边设了环境变量另一边没设，
 * 现象是"连不上"但两端日志都正常。
 */
const char *kDefaultSockPath = "/home/debian/demo/health_monitor.sock";
const char *kSockEnvName     = "HEALTH_MONITOR_SOCK";
}

int main(int argc, char *argv[])
{
    /*
     * 这里刻意不开 AA_EnableHighDpiScaling。
     *
     * 板端走 linuxfb，缩放系数是 Qt 从 framebuffer 的物理尺寸算出来的
     * （fb_var_screeninfo 里的 width/height 毫米数）。这块屏上报的物理尺寸
     * 和实际不一致时，缩放系数会变成 1.5 之类，控件整体放大直接顶出屏幕。
     * 800×480 的固定屏用 1:1 像素最稳；Windows 上调布局时系统缩放由
     * 系统自己处理，也不该在程序里硬开。
     */
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("health_ui"));
    QApplication::setApplicationVersion(QStringLiteral("1.0"));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("健康监测终端 — 本地界面（QLocalSocket / AF_UNIX）"));
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption sockOpt(
        QStringList() << QStringLiteral("s") << QStringLiteral("socket"),
        QStringLiteral("守护进程 UDS 路径（默认取环境变量 %1，再退回内置路径）")
            .arg(QString::fromLatin1(kSockEnvName)),
        QStringLiteral("path"));
    parser.addOption(sockOpt);
    parser.process(app);

    /*
     * 命令行 > 环境变量 > 内置默认值。
     * qEnvironmentVariable 在变量不存在时返回空串，所以要判空再回退。
     */
    QString sockPath = parser.value(sockOpt);
    if (sockPath.isEmpty())
    {
        sockPath = qEnvironmentVariable(kSockEnvName);
        if (sockPath.isEmpty())
            sockPath = QString::fromLatin1(kDefaultSockPath);
    }

    MainWindow w(sockPath);
    w.show();

    return app.exec();
}
