#pragma once

#include <QtGlobal>

// Each test file provides one entry point that runs its QTest classes
int runCertPinningTests(int argc, char** argv);
int runKeyMappingTests(int argc, char** argv);
int runLanPolicyTests(int argc, char** argv);
int runSourcePolicyTests(int argc, char** argv);
int runNvHttpTests(int argc, char** argv);
int runBoxArtTests(int argc, char** argv);
int runPreferencesTests(int argc, char** argv);
int runPacked444Tests(int argc, char** argv);
int runAdaptiveRefinementTests(int argc, char** argv);
#ifdef Q_OS_DARWIN
int runKeychainTests(int argc, char** argv);
int runMetalYCoCgRTests(int argc, char** argv);
int runMetalRefinementTests(int argc, char** argv);
#endif
