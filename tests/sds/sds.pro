# secure-desktop-stream client tests (Qt Test). Kept separate from tests/unit.
#
#   qmake6 tests/sds/sds.pro && make && ./sds_tests
#
# The integration suite starts tests/sds/mock_sds_host.py with `python3 -I`.

QT += core network testlib
QT -= gui
CONFIG += console c++17 testcase
CONFIG -= app_bundle debug_and_release
TEMPLATE = app
TARGET = sds_tests

APP_DIR = $$PWD/../../app
INCLUDEPATH += $$APP_DIR

DEFINES += QT_DEPRECATED_WARNINGS

SOURCES += \
    main.cpp \
    testutil.cpp \
    tst_sanitizer.cpp \
    tst_downloaddir.cpp \
    tst_clipboard.cpp \
    tst_fileclipboard.cpp \
    tst_controller.cpp \
    tst_sdsclient.cpp \
    $$APP_DIR/backend/sdsclient.cpp \
    $$APP_DIR/backend/lanpolicy.cpp \
    $$APP_DIR/backend/certpinning.cpp \
    $$APP_DIR/streaming/sds/clipboardsync.cpp \
    $$APP_DIR/streaming/sds/fileclipboard.cpp \
    $$APP_DIR/streaming/sds/filesanitize.cpp \
    $$APP_DIR/streaming/sds/sdscontroller.cpp \
    $$APP_DIR/streaming/video/adaptiverefinement_codec.cpp

HEADERS += \
    testutil.h \
    $$APP_DIR/backend/sdsclient.h \
    $$APP_DIR/streaming/sds/clipboard_platform.h \
    $$APP_DIR/streaming/sds/clipboardsync.h \
    $$APP_DIR/streaming/sds/fileclipboard.h \
    $$APP_DIR/streaming/sds/filesanitize.h \
    $$APP_DIR/streaming/sds/sdscontroller.h \
    $$APP_DIR/streaming/sds/sdstypes.h \
    $$APP_DIR/streaming/video/adaptiverefinement_codec.h

macx {
    SOURCES += $$APP_DIR/streaming/sds/clipboard_mac.mm pasteboard_helper.mm
    LIBS += -framework AppKit -lobjc
} else {
    SOURCES += $$APP_DIR/streaming/sds/clipboard_null.cpp
}

CONFIG += link_pkgconfig
PKGCONFIG += openssl
LIBS += -lz

QMAKE_CFLAGS += $$(CFLAGS)
QMAKE_CXXFLAGS += $$(CXXFLAGS)
QMAKE_LFLAGS += $$(LDFLAGS)
