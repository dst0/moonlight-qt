#include "nvcomputer.h"
#include "certpinning.h"
#include "lanpolicy.h"
#include <Limelight.h>

#include <QDebug>
#include <QUuid>
#include <QtNetwork/QNetworkReply>
#include <QEventLoop>
#include <QTimer>
#include <QXmlStreamReader>
#include <QSslKey>
#include <QImageReader>
#include <QtEndian>
#include <QNetworkProxy>
#include <QRegularExpression>

namespace {

QString sanitizeReplyErrorString(QString errorText, const QUrl& requestUrl, const QUrl& replyUrl)
{
    for (const QUrl& url : {requestUrl, replyUrl}) {
        if (!url.isValid()) {
            continue;
        }
        QUrl cleanUrl(url);
        cleanUrl.setQuery(QString());
        const QString cleanStr = cleanUrl.toString();
        errorText.replace(url.toString(), cleanStr);
        errorText.replace(QString::fromUtf8(url.toEncoded()), cleanStr);
    }

    // Strip any remaining URL query component (including &rikey=... or &uniqueid=...)
    static const QRegularExpression kQueryRe(QStringLiteral("\\?[^\\s]*"));
    errorText.remove(kQueryRe);

    static const QRegularExpression kSecretParamRe(
        QStringLiteral("(?:&|\\b)(?:rikey|uniqueid|uuid|clientchallenge|serverchallenge|challenge_response|serverchallengeresp|clientpairingsecret|salt|signature|clientcert)=[^\\s&]*"),
        QRegularExpression::CaseInsensitiveOption);
    errorText.remove(kSecretParamRe);

    return errorText.trimmed();
}

} // namespace

#define FAST_FAIL_TIMEOUT_MS 2000
#define REQUEST_TIMEOUT_MS 5000
#define LAUNCH_TIMEOUT_MS 120000
#define RESUME_TIMEOUT_MS 30000
#define QUIT_TIMEOUT_MS 30000

#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
#define XML_NAME_EQUALS(x, y) ((x) == (y))
#else
#define XML_NAME_EQUALS(x, y) ((x) == (u##y))
#endif

NvHTTP::NvHTTP(NvAddress address, uint16_t httpsPort, QSslCertificate serverCert, bool useTrueUid, QNetworkAccessManager* nam) :
    m_Nam(nam ? nam : new QNetworkAccessManager(this)),
    m_ServerCert(serverCert),
    m_UseTrueUid(useTrueUid)
{
    m_BaseUrlHttp.setScheme("http");
    m_BaseUrlHttps.setScheme("https");

    setAddress(address);
    setHttpsPort(httpsPort);

    // Never use a proxy server
    QNetworkProxy noProxy(QNetworkProxy::NoProxy);
    m_Nam->setProxy(noProxy);
}

NvHTTP::NvHTTP(NvComputer* computer, QNetworkAccessManager* nam) :
    NvHTTP(computer->activeAddress, computer->activeHttpsPort, computer->serverCert, !computer->isNvidiaServerSoftware, nam)
{
}

void NvHTTP::setServerCert(QSslCertificate serverCert)
{
    m_ServerCert = serverCert;
}

void NvHTTP::setAddress(NvAddress address)
{
    Q_ASSERT(!address.isNull());

    m_Address = address;

    m_BaseUrlHttp.setHost(address.address());
    m_BaseUrlHttps.setHost(address.address());

    m_BaseUrlHttp.setPort(address.port());
}

void NvHTTP::setHttpsPort(uint16_t port)
{
    m_BaseUrlHttps.setPort(port);
}

void NvHTTP::setTrueUid(bool useTrueUid)
{
    m_UseTrueUid = useTrueUid;
}

NvAddress NvHTTP::address()
{
    return m_Address;
}

QSslCertificate NvHTTP::serverCert()
{
    return m_ServerCert;
}

uint16_t NvHTTP::httpPort()
{
    return m_BaseUrlHttp.port();
}

uint16_t NvHTTP::httpsPort()
{
    return m_BaseUrlHttps.port();
}

QVector<int>
NvHTTP::parseQuad(QString quad)
{
    QVector<int> ret;

    // Return an empty vector for old GFE versions
    // that were missing GfeVersion.
    if (quad.isEmpty()) {
        return ret;
    }

    QStringList parts = quad.split(".");
    ret.reserve(parts.length());
    for (int i = 0; i < parts.length(); i++)
    {
        ret.append(parts.at(i).toInt());
    }

    return ret;
}

int
NvHTTP::getCurrentGame(QString serverInfo)
{
    // GFE 2.8 started keeping currentgame set to the last game played. As a result, it no longer
    // has the semantics that its name would indicate. To contain the effects of this change as much
    // as possible, we'll force the current game to zero if the server isn't in a streaming session.
    QString serverState = getXmlString(serverInfo, "state");
    if (serverState.endsWith("_SERVER_BUSY"))
    {
        return getXmlString(serverInfo, "currentgame").toInt();
    }
    else
    {
        return 0;
    }
}

QString
NvHTTP::getServerInfo(NvLogLevel logLevel, bool fastFail)
{
    QString serverInfo;

    // If we have a pinned cert for this host, never query plaintext HTTP or fall back to HTTP.
    if (!m_ServerCert.isNull())
    {
        if (httpsPort() == 0) {
            setHttpsPort(DEFAULT_HTTPS_PORT);
        }

        serverInfo = openConnectionToString(m_BaseUrlHttps,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        verifyResponseStatus(serverInfo);
    }
    else
    {
        // Only use HTTP prior to pairing (no pinned cert yet)
        serverInfo = openConnectionToString(m_BaseUrlHttp,
                                            "serverinfo",
                                            nullptr,
                                            fastFail ? FAST_FAIL_TIMEOUT_MS : REQUEST_TIMEOUT_MS,
                                            logLevel);
        verifyResponseStatus(serverInfo);

        // Populate the HTTPS port
        uint16_t port = getXmlString(serverInfo, "HttpsPort").toUShort();
        if (port == 0) {
            port = DEFAULT_HTTPS_PORT;
        }
        setHttpsPort(port);
    }

    return serverInfo;
}

void
NvHTTP::startApp(QString verb,
                 bool isGfe,
                 int appId,
                 PSTREAM_CONFIGURATION streamConfig,
                 bool sops,
                 bool localAudio,
                 int gamepadMask,
                 bool persistGameControllersOnDisconnect,
                 QString& rtspSessionUrl,
                 const QString& sdsChromaMode,
                 QString extraQueryParams,
                 bool sdsTileRefinement,
                 bool sdsLosslessMode,
                 const QString& sdsLosslessHash,
                 const QString& sdsLosslessEncoder)
{
    int riKeyId;

    memcpy(&riKeyId, streamConfig->remoteInputAesIv, sizeof(riKeyId));
    riKeyId = qFromBigEndian(riKeyId);

    const bool sendHdrMode = (streamConfig->supportedVideoFormats & VIDEO_FORMAT_MASK_10BIT) &&
                             sdsChromaMode.isEmpty() &&
                             !sdsTileRefinement &&
                             !sdsLosslessMode;

    QString losslessParams;
    if (sdsLosslessMode) {
        losslessParams = QStringLiteral("&sdsLosslessMode=1");
        if (!sdsLosslessHash.isEmpty()) {
            losslessParams += QStringLiteral("&sdsLosslessHash=") + QString::fromUtf8(QUrl::toPercentEncoding(sdsLosslessHash));
        }
        if (!sdsLosslessEncoder.isEmpty()) {
            losslessParams += QStringLiteral("&sdsLosslessEncoder=") + QString::fromUtf8(QUrl::toPercentEncoding(sdsLosslessEncoder));
        }
    }

    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   verb,
                                   "appid="+QString::number(appId)+
                                   "&mode="+QString::number(streamConfig->width)+"x"+
                                   QString::number(streamConfig->height)+"x"+
                                   // Using an FPS value over 60 causes SOPS to default to 720p60,
                                   // so force it to 0 to ensure the correct resolution is set. We
                                   // used to use 60 here but that locked the frame rate to 60 FPS
                                   // on GFE 3.20.3. We don't need this hack for Sunshine.
                                   QString::number((streamConfig->fps > 60 && isGfe) ? 0 : streamConfig->fps)+
                                   "&additionalStates=1&sops="+QString::number(sops ? 1 : 0)+
                                   "&rikey="+QByteArray(streamConfig->remoteInputAesKey, sizeof(streamConfig->remoteInputAesKey)).toHex()+
                                   "&rikeyid="+QString::number(riKeyId)+
                                   (sendHdrMode ?
                                       "&hdrMode=1&clientHdrCapVersion=0&clientHdrCapSupportedFlagsInUint32=0&clientHdrCapMetaDataId=NV_STATIC_METADATA_TYPE_1&clientHdrCapDisplayData=0x0x0x0x0x0x0x0x0x0x0" :
                                        "")+
                                   (!sdsChromaMode.isEmpty() && !sdsLosslessMode ?
                                       "&sdsChromaMode=" + QString::fromUtf8(QUrl::toPercentEncoding(sdsChromaMode)) :
                                        "")+
                                   (sdsTileRefinement && !sdsLosslessMode ? "&sdsTileRefinement=1" : "")+
                                   losslessParams+
                                   "&localAudioPlayMode="+QString::number(localAudio ? 1 : 0)+
                                   "&surroundAudioInfo="+QString::number(SURROUNDAUDIOINFO_FROM_AUDIO_CONFIGURATION(streamConfig->audioConfiguration))+
                                   "&remoteControllersBitmap="+QString::number(gamepadMask)+
                                   "&gcmap="+QString::number(gamepadMask)+
                                   "&gcpersist="+QString::number(persistGameControllersOnDisconnect ? 1 : 0)+
                                   LiGetLaunchUrlQueryParameters()+
                                   extraQueryParams,
                                   LAUNCH_TIMEOUT_MS);

    // Throws if the request failed
    verifyResponseStatus(response);

    rtspSessionUrl = getXmlString(response, "sessionUrl0");
}

void
NvHTTP::quitApp()
{
    QString response =
            openConnectionToString(m_BaseUrlHttps,
                                   "cancel",
                                   nullptr,
                                   QUIT_TIMEOUT_MS);

    // Throws if the request failed
    verifyResponseStatus(response);

    // Newer GFE versions will just return success even if quitting fails
    // if we're not the original requester.
    if (getCurrentGame(getServerInfo(NvHTTP::NVLL_ERROR)) != 0) {
        // Generate a synthetic GfeResponseException letting the caller know
        // that they can't kill someone else's stream.
        throw GfeHttpResponseException(599, "");
    }
}

QVector<NvDisplayMode>
NvHTTP::getDisplayModeList(QString serverInfo)
{
    QXmlStreamReader xmlReader(serverInfo);
    QVector<NvDisplayMode> modes;

    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "DisplayMode")) {
                modes.append(NvDisplayMode());
            }
            else if (!modes.isEmpty()) {
                if (XML_NAME_EQUALS(name, "Width")) {
                    modes.last().width = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "Height")) {
                    modes.last().height = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "RefreshRate")) {
                    modes.last().refreshRate = xmlReader.readElementText().toInt();
                }
            }
        }
    }

    return modes;
}

QVector<NvApp>
NvHTTP::getAppList()
{
    QString appxml = openConnectionToString(m_BaseUrlHttps,
                                            "applist",
                                            nullptr,
                                            REQUEST_TIMEOUT_MS,
                                            NvLogLevel::NVLL_ERROR);
    verifyResponseStatus(appxml);

    QXmlStreamReader xmlReader(appxml);
    QVector<NvApp> apps;
    while (!xmlReader.atEnd()) {
        while (xmlReader.readNextStartElement()) {
            auto name = xmlReader.name();
            if (XML_NAME_EQUALS(name, "App")) {
                // We must have a valid app before advancing to the next one
                if (!apps.isEmpty() && !apps.last().isInitialized()) {
                    qWarning() << "Invalid applist XML";
                    throw std::runtime_error("Invalid applist XML");
                }
                apps.append(NvApp());
            }
            else if (!apps.isEmpty()) {
                if (XML_NAME_EQUALS(name, "AppTitle")) {
                    // If an app has no name, Sunshine may send us <AppTitle/>,
                    // which readElementText() returns as a null QString.
                    // We want to treat this as an empty QString instead, so we
                    // will explicitly convert it. An empty string will satisfy
                    // NvApp's isInitialized() check.
                    QString name = xmlReader.readElementText();
                    if (name.isNull()) {
                        name = "";
                    }
                    apps.last().name = name;
                }
                else if (XML_NAME_EQUALS(name, "ID")) {
                    apps.last().id = xmlReader.readElementText().toInt();
                }
                else if (XML_NAME_EQUALS(name, "IsHdrSupported")) {
                    apps.last().hdrSupported = xmlReader.readElementText() == "1";
                }
                else if (XML_NAME_EQUALS(name, "IsAppCollectorGame")) {
                    apps.last().isAppCollectorGame = xmlReader.readElementText() == "1";
                }
            }
        }
    }

    return apps;
}

void
NvHTTP::verifyResponseStatus(QString xml)
{
    QXmlStreamReader xmlReader(xml);

    while (xmlReader.readNextStartElement())
    {
        if (XML_NAME_EQUALS(xmlReader.name(), "root"))
        {
            // Status code can be 0xFFFFFFFF in some rare cases on GFE 3.20.3, and
            // QString::toInt() will fail in that case, so use QString::toUInt()
            // and cast the result to an int instead.
            int statusCode = (int)xmlReader.attributes().value("status_code").toUInt();
            if (statusCode == 200)
            {
                // Successful
                return;
            }
            else
            {
                QString statusMessage = xmlReader.attributes().value("status_message").toString();
                if (statusCode != 401) {
                    // 401 is expected for unpaired PCs when we fetch serverinfo over HTTPS
                    qWarning() << "Request failed:" << statusCode;
                }
                if (statusCode == -1 && statusMessage == "Invalid") {
                    // Special case handling an audio capture error which GFE doesn't
                    // provide any useful status message for.
                    statusCode = 418;
                    statusMessage = tr("Missing audio capture device. Reinstalling GeForce Experience should resolve this error.");
                }
                throw GfeHttpResponseException(statusCode, statusMessage);
            }
        }
    }

    throw GfeHttpResponseException(-1, "Malformed XML (missing root element)");
}

QImage
NvHTTP::getBoxArt(int appId)
{
    QNetworkReply* reply = openConnection(m_BaseUrlHttps,
                                          "appasset",
                                          "appid="+QString::number(appId)+
                                          "&AssetType=2&AssetIdx=0",
                                          REQUEST_TIMEOUT_MS,
                                          NvLogLevel::NVLL_VERBOSE);
    QImage image = QImageReader(reply).read();
    delete reply;

    return image;
}

QByteArray
NvHTTP::getXmlStringFromHex(QString xml,
                            QString tagName)
{
    return QByteArray::fromHex(getXmlString(xml, tagName).toUtf8());
}

QString
NvHTTP::getXmlString(QString xml,
                     QString tagName)
{
    QXmlStreamReader xmlReader(xml);

    while (!xmlReader.atEnd())
    {
        if (xmlReader.readNext() != QXmlStreamReader::StartElement)
        {
            continue;
        }

        if (xmlReader.name() == tagName)
        {
            return xmlReader.readElementText();
        }
    }

    return QString();
}

QString
NvHTTP::openConnectionToString(QUrl baseUrl,
                               QString command,
                               QString arguments,
                               int timeoutMs,
                               NvLogLevel logLevel)
{
    QNetworkReply* reply = openConnection(baseUrl, command, arguments, timeoutMs, logLevel);
    QString ret;

    QTextStream stream(reply);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    stream.setEncoding(QStringConverter::Utf8);
#else
    stream.setCodec("UTF-8");
#endif

    ret = stream.readAll();
    delete reply;

    return ret;
}

QNetworkReply*
NvHTTP::openConnection(QUrl baseUrl,
                       QString command,
                       QString arguments,
                       int timeoutMs,
                       NvLogLevel logLevel)
{
    // Port must be set
    Q_ASSERT(baseUrl.port(0) != 0);

    if (!LanPolicy::isPermittedConnectionTarget(QHostAddress(baseUrl.host()))) {
        throw GfeHttpResponseException(403, "Use a private LAN IP address or an SSH loopback tunnel");
    }

    // Build a URL for the request
    QUrl url(baseUrl);
    url.setPath("/" + command);

    // Use a placeholder UID for GFE allow them to quit games for each other.
    url.setQuery("uniqueid=" + (m_UseTrueUid ? IdentityManager::get()->getUniqueId() : "0123456789ABCDEF") +
                 "&uuid=" + QUuid::createUuid().toRfc4122().toHex() +
                 ((arguments != nullptr) ? ("&" + arguments) : ""));

    QNetworkRequest request(url);
    // Redirects could leave the permitted LAN and bypass the original address check.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);

    // Add our client certificate
    QSslConfiguration sslConfig = IdentityManager::get()->getSslConfig();

    // Every HTTPS request strictly pins the host certificate learned during pairing
    bool pinServerCert = url.scheme() == QStringLiteral("https");
    if (pinServerCert) {
        if (m_ServerCert.isNull()) {
            // We should never make an HTTPS request without a pinned cert
            if (logLevel >= NvLogLevel::NVLL_ERROR) {
                qWarning() << "Refusing HTTPS request without a pinned server certificate:" << command;
            }
            throw GfeHttpResponseException(401, "No pinned server certificate");
        }
        sslConfig = CertPinning::restrictToPinned(sslConfig, m_ServerCert);
    }
    request.setSslConfiguration(sslConfig);

#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    // Disable HTTP/2 (GFE 3.22 doesn't like it) and Qt 6 enables it by default
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
#endif


    // Use fine-grained idle timeouts to avoid calling QNetworkAccessManager::clearAccessCache(),
    // which tears down the NAM's global thread each time. We must not keep persistent connections
    // or GFE will puke.
#if QT_VERSION >= QT_VERSION_CHECK(6, 12, 0)
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, -1);
#elif QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    request.setAttribute(QNetworkRequest::ConnectionCacheExpiryTimeoutSecondsAttribute, 0);
#endif

    QNetworkReply* reply = m_Nam->get(request);
    if (pinServerCert) {
        CertPinning::enforce(reply, m_ServerCert);
    }
    QEventLoop loop;

    // Connect the quit signals
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, &loop, &QEventLoop::quit);

    // Enforce a transfer timeout if requested
    if (timeoutMs) {
        QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // Time the request to distinguish a slow response from an unreachable host
    auto* connectTimer = new QTimer(reply);
    connectTimer->setSingleShot(true);
    connect(connectTimer, &QTimer::timeout, reply, [reply, logLevel, command]() {
        if (!reply->isFinished()) {
            if (logLevel >= NvLogLevel::NVLL_ERROR) {
                qWarning() << "Aborting timed out connection for" << command;
            }

            reply->setProperty("timeout_reason", "connection_timeout");
            reply->abort();
        }
    });
    connect(reply, &QNetworkReply::requestSent, connectTimer, &QTimer::stop);
    connectTimer->start(REQUEST_TIMEOUT_MS);
#endif

    if (logLevel >= NvLogLevel::NVLL_VERBOSE) {
        qInfo() << "Executing request:" << command;
    }
    loop.exec(QEventLoop::ExcludeUserInputEvents);

    // Abort the request if it timed out
    if (!reply->isFinished())
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << "Aborting timed out request for" << command;
        }
        reply->abort();
    }

#if QT_VERSION < QT_VERSION_CHECK(6, 3, 0)
    // If we couldn't use fine-grained connection idle timeouts, kill them all now
    m_Nam->clearAccessCache();
#endif

    // Handle error
    if (reply->error() != QNetworkReply::NoError)
    {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "request failed with error:" << reply->error();
        }

        if (reply->error() == QNetworkReply::SslHandshakeFailedError ||
                (pinServerCert && CertPinning::wasRejected(reply))) {
            GfeHttpResponseException exception(495, "Server certificate mismatch");
            delete reply;
            throw exception;
        }
        else if (reply->error() == QNetworkReply::OperationCanceledError) {
            QString reason = reply->property("timeout_reason").toString();
            QtNetworkReplyException exception(QNetworkReply::TimeoutError,
                                              reason == "connection_timeout" ?
                                                  "Connection timed out" :
                                                  "Request timed out");
            delete reply;
            throw exception;
        }
        else {
            QtNetworkReplyException exception(
                reply->error(),
                sanitizeReplyErrorString(reply->errorString(), url, reply->url()));
            delete reply;
            throw exception;
        }
    }

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (status >= 300 && status < 400) {
        delete reply;
        throw GfeHttpResponseException(status, "HTTP redirects are not permitted");
    }

    // Defense in depth: never hand out data from an HTTPS peer that is not the pinned host
    if (pinServerCert && !CertPinning::matchesPinned(m_ServerCert, reply->sslConfiguration().peerCertificate())) {
        if (logLevel >= NvLogLevel::NVLL_ERROR) {
            qWarning() << command << "response did not come from the pinned server certificate";
        }
        GfeHttpResponseException exception(495, "Server certificate mismatch");
        delete reply;
        throw exception;
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    // The requestSent signal must have stopped the timer for any successful request,
    // otherwise we could falsely time out a slow response when we connected fine.
    Q_ASSERT(!connectTimer->isActive());
#endif

    return reply;
}
