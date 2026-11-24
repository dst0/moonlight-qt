// Globals that moonlight-common-c's Connection.c normally defines, so the mic stream
// (SdsMicStream.c) can be linked into the tests without the whole connection machinery.
#include "Limelight-internal.h"

char* RemoteAddrString;
struct sockaddr_storage RemoteAddr;
struct sockaddr_storage LocalAddr;
SOCKADDR_LEN AddrLen;
STREAM_CONFIGURATION StreamConfig;
CONNECTION_LISTENER_CALLBACKS ListenerCallbacks;

// Platform.c's initializePlatform() references ENet, which the tests do not use
int enet_initialize(void) { return 0; }
void enet_deinitialize(void) {}

static void quietLog(const char* format, ...) { (void)format; }

void micTestInitCommonC(void)
{
    ListenerCallbacks.logMessage = quietLog;
    PltTicksInit();
}

#include "SdsMic.h"

// Sets up the negotiated microphone stream towards 127.0.0.1:port as the RTSP handshake would
int micTestStartStream(uint16_t port, const char pingPayload[16], const unsigned char key[16])
{
    struct sockaddr_in* addr = (struct sockaddr_in*)&RemoteAddr;

    memset(&RemoteAddr, 0, sizeof(RemoteAddr));
    addr->sin_family = AF_INET;
    addr->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    memset(&LocalAddr, 0, sizeof(LocalAddr));
    AddrLen = sizeof(struct sockaddr_in);
    memcpy(StreamConfig.remoteInputAesKey, key, 16);

    initializeMicStream();
    MicPortNumber = port;
    memcpy(MicPingPayload.payload, pingPayload, sizeof(MicPingPayload.payload));
    MicStreamNegotiated = true;
    return startMicStream();
}

void micTestStopStream(void)
{
    stopMicStream();
}
