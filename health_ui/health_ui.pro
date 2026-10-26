# ============================================================
# health_ui.pro — 板端 Qt 界面
# ============================================================
#
# 用 QLocalSocket（AF_UNIX）连守护进程，只读展示 + 发命令。
# 界面布局全部在 mainwindow.ui 里，用 Qt Designer 改；
# 本工程不含任何手写 widget 布局代码。
#
# 构建：
#   qmake health_ui.pro && make
#
# 板端部署（i.MX6ULL / armhf）：
#   /path/to/Qt/arm/bin/qmake health_ui.pro && make
#   产物是动态链接的，运行时需要把 Qt 库一起带到板子上，
#   或者用 usr/local/Qt 的 sysroot 部署脚本。
# ============================================================

QT += core gui widgets network charts

TARGET   = health_ui
TEMPLATE = app

CONFIG += c++11

# 让 qmake 调用 uic 生成 ui_mainwindow.h。
# 生成位置在两套构建体系下不一样：
#   qmake + make         → 输出到构建目录（out-of-source 时）
#   Qt Creator 影子构建   → 输出到 shadow build 目录
# 两种情况下都不该把 ui_mainwindow.h 提交到版本库，
# C++ 里统一写 #include "ui_mainwindow.h"，由编译器 -I 找到。
FORMS += mainwindow.ui

HEADERS += \
    mainwindow.h \
    localclient.h

SOURCES += \
    main.cpp \
    mainwindow.cpp \
    localclient.cpp

# 关闭废弃 API，避免误用 Qt4 风格的 QLocalSocket::error()
DEFINES += QT_DEPRECATED_WARNINGS

# Qt6 下 QLocalSocket::errorOccurred 已统一，不再需要 QOverload 包裹。
