# Unit and integration tests for secure-desktop-stream microphone passthrough (client side).
# Build out of tree: qmake6 <wt>/tests/mic/mic.pro && make && ./moonlight-mic-tests
# Never touches the real microphone: capture sources are fakes and the platform permission
# backend is the stub from miccapture_none.cpp.

QT += testlib
QT -= gui
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = moonlight-mic-tests

APP_DIR = $$clean_path($$PWD/../../app)
COMMONC_DIR = $$clean_path($$PWD/../../moonlight-common-c/moonlight-common-c)

INCLUDEPATH += $$APP_DIR $$COMMONC_DIR/src $$COMMONC_DIR/enet/include
DEFINES += HAS_SOCKLEN_T
# moonlight-common-c builds with -Wno-unused-parameter
QMAKE_CFLAGS += -Wno-unused-parameter

CONFIG += link_pkgconfig
PKGCONFIG += openssl opus
packagesExist(sdl2) {
    PKGCONFIG += sdl2
} else {
    # Build containers without SDL2: logging-only shim
    INCLUDEPATH += $$PWD/shim
}
unix: LIBS += -lpthread

HEADERS += \
    testutil.h \
    $$APP_DIR/streaming/audio/miccapture.h \
    $$APP_DIR/streaming/audio/micsession.h \
    $$APP_DIR/streaming/audio/micstreamer.h

SOURCES += \
    main.cpp \
    testutil.cpp \
    tst_micencoder.cpp \
    tst_micstreamer.cpp \
    tst_micsession.cpp \
    tst_micwire.cpp \
    commonc_globals.c \
    $$APP_DIR/streaming/audio/miccapture_none.cpp \
    $$APP_DIR/streaming/audio/micsession.cpp \
    $$APP_DIR/streaming/audio/micstreamer.cpp \
    $$COMMONC_DIR/src/Platform.c \
    $$COMMONC_DIR/src/PlatformCrypto.c \
    $$COMMONC_DIR/src/PlatformSockets.c \
    $$COMMONC_DIR/src/SdsMicPacket.c \
    $$COMMONC_DIR/src/SdsMicStream.c

# Settings plumbing needs QtQml (StreamingPreferences is a QML singleton)
qtHaveModule(qml) {
    QT += qml
    DEFINES += MIC_TEST_PREFERENCES
    HEADERS += $$APP_DIR/settings/streamingpreferences.h
    SOURCES += \
        tst_micpreferences.cpp \
        wmutils_stub.cpp \
        $$APP_DIR/settings/streamingpreferences.cpp
}

macx {
    HEADERS += tst_miccapture_mac.h $$APP_DIR/streaming/audio/miccapture_format_mac.h
    SOURCES += tst_miccapture_mac.mm
    LIBS += -framework AVFoundation -framework Foundation
}
