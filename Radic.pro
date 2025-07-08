TARGET = Radic
QT += core gui widgets
CONFIG += c++17

INCLUDEPATH += /usr/include/freerdp3
INCLUDEPATH += /usr/include/winpr3

LIBS += -lfreerdp3 -lfreerdp-client3 -lwinpr3

gcc:QMAKE_CXXFLAGS += -Wall -Wextra -Werror=return-type -Werror=trigraphs -Wno-switch -Wno-reorder -Wno-unused-parameter

SOURCES += \
    CommandForm.cpp \
    ConnectionDialog.cpp \
    Global.cpp \
    MySettings.cpp \
    MyView.cpp \
    VerifyCertificateDialog.cpp \
    joinpath.cpp \
    main.cpp \
    MainWindow.cpp \
    rdpcert.cpp

HEADERS += \
    CommandForm.h \
    ConnectionDialog.h \
    Global.h \
    MainWindow.h \
    MySettings.h \
    MyView.h \
    VerifyCertificateDialog.h \
    joinpath.h \
    rdpcert.h

FORMS += \
    CommandForm.ui \
    ConnectionDialog.ui \
    MainWindow.ui \
    VerifyCertificateDialog.ui

