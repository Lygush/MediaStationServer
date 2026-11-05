QT       += core gui widgets network

greaterThan(QT_MAJOR_VERSION, 4): QT += widgets

CONFIG += c++17

SOURCES += \
    ServerWindow.cpp  \
    UpdateTab.cpp     \
    main.cpp          \
    LogsTab.cpp

HEADERS += \
    ServerWindow.h  \
    UpdateTab.h     \
    LogsTab.h

# Default rules for deployment.
qnx: target.path = /tmp/$${TARGET}/bin
else: unix:!android: target.path = /opt/$${TARGET}/bin
!isEmpty(target.path): INSTALLS += target
