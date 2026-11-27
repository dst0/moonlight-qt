# Unit tests for the secure-desktop-stream client hardening.
# Built and run by tools/run-client-tests.sh in the secure-desktop-stream repository.

QT += testlib network gui qml
CONFIG += console c++17 testcase
CONFIG -= app_bundle
TEMPLATE = app
TARGET = moonlight-unit-tests

APP_DIR = $$clean_path($$PWD/../../app)
SOURCE_ROOT = $$clean_path($$PWD/../..)

INCLUDEPATH += $$APP_DIR
INCLUDEPATH += $$SOURCE_ROOT/moonlight-common-c/moonlight-common-c/src
INCLUDEPATH += $$SOURCE_ROOT/qmdnsengine/qmdnsengine/src/include $$SOURCE_ROOT/qmdnsengine

CONFIG += link_pkgconfig
PKGCONFIG += openssl sdl2

HEADERS += \
    tests.h \
    testcerts.h

SOURCES += \
    main.cpp \
    testcerts.cpp \
    tst_boxart.cpp \
    tst_certpinning.cpp \
    tst_keymapping.cpp \
    tst_lanpolicy.cpp \
    tst_sourcepolicy.cpp \
    tst_nvhttp.cpp \
    tst_preferences.cpp \
    test_adaptiverefinement.cpp \
    test_packed444.cpp \
    $$APP_DIR/backend/boxartmanager.cpp \
    $$APP_DIR/backend/nvcomputer.cpp \
    $$APP_DIR/backend/nvapp.cpp \
    $$APP_DIR/backend/nvhttp.cpp \
    $$APP_DIR/backend/nvaddress.cpp \
    $$APP_DIR/backend/certpinning.cpp \
    $$APP_DIR/backend/lanpolicy.cpp \
    $$APP_DIR/backend/identitymanager.cpp \
    $$APP_DIR/settings/streamingpreferences.cpp \
    $$APP_DIR/path.cpp \
    $$APP_DIR/streaming/input/keymapping.cpp \
    $$APP_DIR/streaming/bandwidth.cpp \
    $$APP_DIR/streaming/video/adaptiverefinement.cpp \
    $$APP_DIR/streaming/video/adaptiverefinement_codec.cpp \
    $$APP_DIR/streaming/video/packed444.cpp

HEADERS += \
    $$APP_DIR/backend/boxartmanager.h \
    $$APP_DIR/backend/nvcomputer.h \
    $$APP_DIR/backend/nvhttp.h \
    $$APP_DIR/settings/streamingpreferences.h \
    $$APP_DIR/streaming/bandwidth.h \
    $$APP_DIR/streaming/video/adaptiverefinement.h \
    $$APP_DIR/streaming/video/adaptiverefinement_codec.h \
    $$APP_DIR/streaming/video/packed444.h

macx {
    HEADERS += \
        test_metal_refinement.h \
        test_metal_ycocgr.h
    SOURCES += \
        tst_keychain.cpp \
        test_metal_refinement.mm \
        test_metal_ycocgr.mm \
        $$APP_DIR/backend/keychain_mac.cpp
    LIBS += -framework Security -framework CoreFoundation -framework Metal -framework Foundation
}
