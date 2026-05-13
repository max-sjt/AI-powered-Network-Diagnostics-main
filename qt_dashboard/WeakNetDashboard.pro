QT += widgets network

TEMPLATE = app
TARGET = WeakNetDashboard

CONFIG += c++17
CONFIG += release

DESTDIR = $$OUT_PWD/bin
OBJECTS_DIR = $$OUT_PWD/obj
MOC_DIR = $$OUT_PWD/moc
RCC_DIR = $$OUT_PWD/rcc
UI_DIR = $$OUT_PWD/ui

SOURCES += \
    src/main.cpp \
    src/mainwindow.cpp \
    src/project_log_parser.cpp \
    src/trend_chart_widget.cpp \
    src/windows_network_probe.cpp

HEADERS += \
    src/mainwindow.h \
    src/models.h \
    src/project_log_parser.h \
    src/trend_chart_widget.h \
    src/windows_network_probe.h

FORMS += \
    src/mainwindow.ui

win32: LIBS += -lIphlpapi -lWs2_32 -lWlanapi -lOle32
win32-msvc*: QMAKE_CXXFLAGS += /utf-8
