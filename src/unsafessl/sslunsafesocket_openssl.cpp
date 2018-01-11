
#define SslUnsafeSocket_DEBUG
#define QT_DECRYPT_SSL_TRAFFIC

//#include "qssl_p.h"
#include "sslunsafeerror.h"
#include "sslunsafesocket_openssl_p.h"
#include "sslunsafesocket_openssl_symbols_p.h"
#include "sslunsafesocket.h"
#include "sslunsafecertificate_p.h"
#include "sslunsafecipher_p.h"
#include "sslunsafekey_p.h"
#include "sslunsafeellipticcurve.h"
#include "sslunsafepresharedkeyauthenticator.h"
#include "sslunsafepresharedkeyauthenticator_p.h"

#include <QtCore/qdatetime.h>
#include <QtCore/qdebug.h>
#include <QtCore/qdir.h>
#include <QtCore/qdiriterator.h>
#include <QtCore/qelapsedtimer.h>
#include <QtCore/qfile.h>
#include <QtCore/qfileinfo.h>
#include <QtCore/qmutex.h>
#include <QtCore/qthread.h>
#include <QtCore/qurl.h>
#include <QtCore/qvarlengtharray.h>

#include <QHostInfo>

#include <string.h>

QT_BEGIN_NAMESPACE

bool SslUnsafeSocketPrivate::s_libraryLoaded = false;
bool SslUnsafeSocketPrivate::s_loadedCiphersAndCerts = false;
bool SslUnsafeSocketPrivate::s_loadRootCertsOnDemand = false;

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
int SslUnsafeSocketBackendPrivate::s_indexForSSLExtraData = -1;
#endif

/* \internal

    From OpenSSL's thread(3) manual page:

    OpenSSL can safely be used in multi-threaded applications provided that at
    least two callback functions are set.

    locking_function(int mode, int n, const char *file, int line) is needed to
    perform locking on shared data structures.  (Note that OpenSSL uses a
    number of global data structures that will be implicitly shared
    whenever multiple threads use OpenSSL.)  Multi-threaded
    applications will crash at random if it is not set.  ...
    ...
    id_function(void) is a function that returns a thread ID. It is not
    needed on Windows nor on platforms where getpid() returns a different
    ID for each thread (most notably Linux)
*/
class QOpenSslLocks
{
public:
    inline QOpenSslLocks()
        : initLocker(QMutex::Recursive),
          locksLocker(QMutex::Recursive)
    {
        QMutexLocker locker(&locksLocker);
        int numLocks = uq_CRYPTO_num_locks();
        locks = new QMutex *[numLocks];
        memset(locks, 0, numLocks * sizeof(QMutex *));
    }
    inline ~QOpenSslLocks()
    {
        QMutexLocker locker(&locksLocker);
        for (int i = 0; i < uq_CRYPTO_num_locks(); ++i)
            delete locks[i];
        delete [] locks;

        SslUnsafeSocketPrivate::deinitialize();
    }
    inline QMutex *lock(int num)
    {
        QMutexLocker locker(&locksLocker);
        QMutex *tmp = locks[num];
        if (!tmp)
            tmp = locks[num] = new QMutex(QMutex::Recursive);
        return tmp;
    }

    QMutex *globalLock()
    {
        return &locksLocker;
    }

    QMutex *initLock()
    {
        return &initLocker;
    }

private:
    QMutex initLocker;
    QMutex locksLocker;
    QMutex **locks;
};
Q_GLOBAL_STATIC(QOpenSslLocks, openssl_locks)

QString SslUnsafeSocketBackendPrivate::getErrorsFromOpenSsl()
{
    QString errorString;
    unsigned long errNum;
    while ((errNum = uq_ERR_get_error())) {
        if (! errorString.isEmpty())
            errorString.append(QLatin1String(", "));
        const char *error = uq_ERR_error_string(errNum, NULL);
        errorString.append(QString::fromLatin1(error)); // error is ascii according to man ERR_error_string
    }
    return errorString;
}

extern "C" {
static void locking_function(int mode, int lockNumber, const char *, int)
{
    QMutex *mutex = openssl_locks()->lock(lockNumber);

    // Lock or unlock it
    if (mode & CRYPTO_LOCK)
        mutex->lock();
    else
        mutex->unlock();
}
static unsigned long id_function()
{
    return (quintptr)QThread::currentThreadId();
}

#if OPENSSL_VERSION_NUMBER >= 0x10001000L && !defined(OPENSSL_NO_PSK)
static unsigned int q_ssl_psk_client_callback(SSL *ssl,
                                              const char *hint,
                                              char *identity, unsigned int max_identity_len,
                                              unsigned char *psk, unsigned int max_psk_len)
{
    SslUnsafeSocketBackendPrivate *d = reinterpret_cast<SslUnsafeSocketBackendPrivate *>(uq_SSL_get_ex_data(ssl, SslUnsafeSocketBackendPrivate::s_indexForSSLExtraData));
    Q_ASSERT(d);
    return d->tlsPskClientCallback(hint, identity, max_identity_len, psk, max_psk_len);
}

static unsigned int q_ssl_psk_server_callback(SSL *ssl,
                                              const char *identity,
                                              unsigned char *psk, unsigned int max_psk_len)
{
    SslUnsafeSocketBackendPrivate *d = reinterpret_cast<SslUnsafeSocketBackendPrivate *>(uq_SSL_get_ex_data(ssl, SslUnsafeSocketBackendPrivate::s_indexForSSLExtraData));
    Q_ASSERT(d);
    return d->tlsPskServerCallback(identity, psk, max_psk_len);
}
#endif
} // extern "C"

SslUnsafeSocketBackendPrivate::SslUnsafeSocketBackendPrivate()
    : ssl(0),
      readBio(0),
      writeBio(0),
      session(0)
{
    // Calls SSL_library_init().
    ensureInitialized();
}

SslUnsafeSocketBackendPrivate::~SslUnsafeSocketBackendPrivate()
{
    destroySslContext();
}

SslUnsafeCipher SslUnsafeSocketBackendPrivate::SslUnsafeCipher_from_SSL_CIPHER(SSL_CIPHER *cipher)
{
    SslUnsafeCipher ciph;

    char buf [256];
    QString descriptionOneLine = QString::fromLatin1(uq_SSL_CIPHER_description(cipher, buf, sizeof(buf)));

    const auto descriptionList = descriptionOneLine.splitRef(QLatin1Char(' '), QString::SkipEmptyParts);
    if (descriptionList.size() > 5) {
        // ### crude code.
        ciph.d->isNull = false;
        ciph.d->name = descriptionList.at(0).toString();

        QString protoString = descriptionList.at(1).toString();
        ciph.d->protocolString = protoString;
        ciph.d->protocol = QSsl::UnknownProtocol;
        if (protoString == QLatin1String("SSLv3"))
            ciph.d->protocol = QSsl::SslV3;
        else if (protoString == QLatin1String("SSLv2"))
            ciph.d->protocol = QSsl::SslV2;
        else if (protoString == QLatin1String("TLSv1"))
            ciph.d->protocol = QSsl::TlsV1_0;
        else if (protoString == QLatin1String("TLSv1.1"))
            ciph.d->protocol = QSsl::TlsV1_1;
        else if (protoString == QLatin1String("TLSv1.2"))
            ciph.d->protocol = QSsl::TlsV1_2;

        if (descriptionList.at(2).startsWith(QLatin1String("Kx=")))
            ciph.d->keyExchangeMethod = descriptionList.at(2).mid(3).toString();
        if (descriptionList.at(3).startsWith(QLatin1String("Au=")))
            ciph.d->authenticationMethod = descriptionList.at(3).mid(3).toString();
        if (descriptionList.at(4).startsWith(QLatin1String("Enc=")))
            ciph.d->encryptionMethod = descriptionList.at(4).mid(4).toString();
        ciph.d->exportable = (descriptionList.size() > 6 && descriptionList.at(6) == QLatin1String("export"));

        ciph.d->bits = uq_SSL_CIPHER_get_bits(cipher, &ciph.d->supportedBits);
    }
    return ciph;
}

// static
inline SslUnsafeErrorEntry SslUnsafeErrorEntry::fromStoreContext(X509_STORE_CTX *ctx) {
    SslUnsafeErrorEntry result = {
        uq_X509_STORE_CTX_get_error(ctx),
        uq_X509_STORE_CTX_get_error_depth(ctx)
    };
    return result;
}

// ### This list is shared between all threads, and protected by a
// mutex. Investigate using thread local storage instead.
struct SslUnsafeErrorList
{
    QMutex mutex;
    QVector<SslUnsafeErrorEntry> errors;
};
Q_GLOBAL_STATIC(SslUnsafeErrorList, _q_sslErrorList)

int uq_X509Callback(int ok, X509_STORE_CTX *ctx)
{
    if (!ok) {
        // Store the error and at which depth the error was detected.
        _q_sslErrorList()->errors << SslUnsafeErrorEntry::fromStoreContext(ctx);
#ifdef SslUnsafeSocket_DEBUG
        qDebug() << "verification error: dumping bad certificate";
        qDebug() << SslUnsafeCertificatePrivate::SslUnsafeCertificate_from_X509(uq_X509_STORE_CTX_get_current_cert(ctx)).toPem();
        qDebug() << "dumping chain";
        const auto certs = SslUnsafeSocketBackendPrivate::STACKOFX509_to_SslUnsafeCertificates(uq_X509_STORE_CTX_get_chain(ctx));
        for (const SslUnsafeCertificate &cert : certs) {
            qDebug() << "Issuer:" << "O=" << cert.issuerInfo(SslUnsafeCertificate::Organization)
                << "CN=" << cert.issuerInfo(SslUnsafeCertificate::CommonName)
                << "L=" << cert.issuerInfo(SslUnsafeCertificate::LocalityName)
                << "OU=" << cert.issuerInfo(SslUnsafeCertificate::OrganizationalUnitName)
                << "C=" << cert.issuerInfo(SslUnsafeCertificate::CountryName)
                << "ST=" << cert.issuerInfo(SslUnsafeCertificate::StateOrProvinceName);
            qDebug() << "Subject:" << "O=" << cert.subjectInfo(SslUnsafeCertificate::Organization)
                << "CN=" << cert.subjectInfo(SslUnsafeCertificate::CommonName)
                << "L=" << cert.subjectInfo(SslUnsafeCertificate::LocalityName)
                << "OU=" << cert.subjectInfo(SslUnsafeCertificate::OrganizationalUnitName)
                << "C=" << cert.subjectInfo(SslUnsafeCertificate::CountryName)
                << "ST=" << cert.subjectInfo(SslUnsafeCertificate::StateOrProvinceName);
            qDebug() << "Valid:" << cert.effectiveDate() << '-' << cert.expiryDate();
        }
#endif
    }
    // Always return OK to allow verification to continue. We're handle the
    // errors gracefully after collecting all errors, after verification has
    // completed.
    return 1;
}

long SslUnsafeSocketBackendPrivate::setupOpenSslOptions(QSsl::SslProtocol protocol, QSsl::SslOptions sslOptions)
{
    long options;
    if (protocol == QSsl::TlsV1SslV3)
        options = SSL_OP_ALL|SSL_OP_NO_SSLv2;
    else if (protocol == QSsl::SecureProtocols)
        options = SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3;
    else if (protocol == QSsl::TlsV1_0OrLater)
        options = SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3;
#if OPENSSL_VERSION_NUMBER >= 0x10001000L
    // Choosing Tlsv1_1OrLater or TlsV1_2OrLater on OpenSSL < 1.0.1
    // will cause an error in SslUnsafeContext::fromConfiguration, meaning
    // we will never get here.
    else if (protocol == QSsl::TlsV1_1OrLater)
        options = SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3|SSL_OP_NO_TLSv1;
    else if (protocol == QSsl::TlsV1_2OrLater)
        options = SSL_OP_ALL|SSL_OP_NO_SSLv2|SSL_OP_NO_SSLv3|SSL_OP_NO_TLSv1|SSL_OP_NO_TLSv1_1;
#endif
    else
        options = SSL_OP_ALL;

    // This option is disabled by default, so we need to be able to clear it
    if (sslOptions & QSsl::SslOptionDisableEmptyFragments)
        options |= SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;
    else
        options &= ~SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS;

#ifdef SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION
    // This option is disabled by default, so we need to be able to clear it
    if (sslOptions & QSsl::SslOptionDisableLegacyRenegotiation)
        options &= ~SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
    else
        options |= SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION;
#endif

#ifdef SSL_OP_NO_TICKET
    if (sslOptions & QSsl::SslOptionDisableSessionTickets)
        options |= SSL_OP_NO_TICKET;
#endif
#ifdef SSL_OP_NO_COMPRESSION
    if (sslOptions & QSsl::SslOptionDisableCompression)
        options |= SSL_OP_NO_COMPRESSION;
#endif

    if (!(sslOptions & QSsl::SslOptionDisableServerCipherPreference))
        options |= SSL_OP_CIPHER_SERVER_PREFERENCE;

    return options;
}

bool SslUnsafeSocketBackendPrivate::initSslContext()
{
    Q_Q(SslUnsafeSocket);

    // If no external context was set (e.g. bei QHttpNetworkConnection) we will create a default context
    if (!sslContextPointer) {
        // create a deep copy of our configuration
        SslUnsafeConfigurationPrivate *configurationCopy = new SslUnsafeConfigurationPrivate(configuration);
        configurationCopy->ref.store(0);              // the SslUnsafeConfiguration constructor refs up
        sslContextPointer = SslUnsafeContext::sharedFromConfiguration(mode, configurationCopy, allowRootCertOnDemandLoading);
    }

    if (sslContextPointer->error() != SslUnsafeError::NoError) {
        setErrorAndEmit(QAbstractSocket::SslInvalidUserDataError, sslContextPointer->errorString());
        sslContextPointer.clear(); // deletes the SslUnsafeContext
        return false;
    }

    // Create and initialize SSL session
    if (!(ssl = sslContextPointer->createSsl())) {
        // ### Bad error code
        setErrorAndEmit(QAbstractSocket::SslInternalError,
                        SslUnsafeSocket::tr("Error creating SSL session, %1").arg(getErrorsFromOpenSsl()));
        return false;
    }

    if (configuration.protocol != QSsl::SslV2 &&
        configuration.protocol != QSsl::SslV3 &&
        configuration.protocol != QSsl::UnknownProtocol &&
        mode == SslUnsafeSocket::SslClientMode && uq_SSLeay() >= 0x00090806fL) {
        // Set server hostname on TLS extension. RFC4366 section 3.1 requires it in ACE format.
        QString tlsHostName = verificationPeerName.isEmpty() ? q->peerName() : verificationPeerName;
        if (tlsHostName.isEmpty())
            tlsHostName = QHostInfo::localHostName();
        QByteArray ace = QUrl::toAce(tlsHostName);
        // only send the SNI header if the URL is valid and not an IP
        if (!ace.isEmpty()
            && !QHostAddress().setAddress(tlsHostName)
            && !(configuration.sslOptions & QSsl::SslOptionDisableServerNameIndication)) {
            // We don't send the trailing dot from the host header if present see
            // https://tools.ietf.org/html/rfc6066#section-3
            if (ace.endsWith('.'))
                ace.chop(1);
            if (!uq_SSL_ctrl(ssl, SSL_CTRL_SET_TLSEXT_HOSTNAME, TLSEXT_NAMETYPE_host_name, ace.data()))
                qWarning() << "could not set SSL_CTRL_SET_TLSEXT_HOSTNAME, Server Name Indication disabled";
        }
    }

    // Clear the session.
    errorList.clear();

    // Initialize memory BIOs for encryption and decryption.
    readBio = uq_BIO_new(uq_BIO_s_mem());
    writeBio = uq_BIO_new(uq_BIO_s_mem());
    if (!readBio || !writeBio) {
        setErrorAndEmit(QAbstractSocket::SslInternalError,
                        SslUnsafeSocket::tr("Error creating SSL session: %1").arg(getErrorsFromOpenSsl()));
        return false;
    }

    // Assign the bios.
    uq_SSL_set_bio(ssl, readBio, writeBio);

    if (mode == SslUnsafeSocket::SslClientMode)
        uq_SSL_set_connect_state(ssl);
    else
        uq_SSL_set_accept_state(ssl);

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
    // Save a pointer to this object into the SSL structure.
    if (uq_SSLeay() >= 0x10001000L)
        uq_SSL_set_ex_data(ssl, s_indexForSSLExtraData, this);
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10001000L && !defined(OPENSSL_NO_PSK)
    // Set the client callback for PSK
    if (uq_SSLeay() >= 0x10001000L) {
        if (mode == SslUnsafeSocket::SslClientMode)
            uq_SSL_set_psk_client_callback(ssl, &q_ssl_psk_client_callback);
        else if (mode == SslUnsafeSocket::SslServerMode)
            uq_SSL_set_psk_server_callback(ssl, &q_ssl_psk_server_callback);
    }
#endif

    return true;
}

void SslUnsafeSocketBackendPrivate::destroySslContext()
{
    if (ssl) {
        uq_SSL_free(ssl);
        ssl = 0;
    }
    sslContextPointer.clear();
}

/*!
    \internal
*/
void SslUnsafeSocketPrivate::deinitialize()
{
    uq_CRYPTO_set_id_callback(0);
    uq_CRYPTO_set_locking_callback(0);
    uq_ERR_free_strings();
}

/*!
    \internal

    Does the minimum amount of initialization to determine whether SSL
    is supported or not.
*/

bool SslUnsafeSocketPrivate::supportsSsl()
{
    return ensureLibraryLoaded();
}

bool SslUnsafeSocketPrivate::ensureLibraryLoaded()
{
    if (!uq_resolveOpenSslSymbols())
        return false;

    // Check if the library itself needs to be initialized.
    QMutexLocker locker(openssl_locks()->initLock());

    if (!s_libraryLoaded) {
        s_libraryLoaded = true;

        // Initialize OpenSSL.
        uq_CRYPTO_set_id_callback(id_function);
        uq_CRYPTO_set_locking_callback(locking_function);
        if (uq_SSL_library_init() != 1)
            return false;
        uq_SSL_load_error_strings();
        uq_OpenSSL_add_all_algorithms();

#if OPENSSL_VERSION_NUMBER >= 0x10001000L
        if (uq_SSLeay() >= 0x10001000L)
            SslUnsafeSocketBackendPrivate::s_indexForSSLExtraData = uq_SSL_get_ex_new_index(0L, NULL, NULL, NULL, NULL);
#endif

        // Initialize OpenSSL's random seed.
        if (!uq_RAND_status()) {
            qWarning("Random number generator not seeded, disabling SSL support");
            return false;
        }
    }
    return true;
}

void SslUnsafeSocketPrivate::ensureCiphersAndCertsLoaded()
{
    QMutexLocker locker(openssl_locks()->initLock());
    if (s_loadedCiphersAndCerts)
        return;
    s_loadedCiphersAndCerts = true;

    resetDefaultCiphers();
    resetDefaultEllipticCurves();

#if QT_CONFIG(library)
    //load symbols needed to receive certificates from system store
#if defined(Q_OS_WIN)
    HINSTANCE hLib = LoadLibraryW(L"Crypt32");
    if (hLib) {
        ptrCertOpenSystemStoreW = (PtrCertOpenSystemStoreW)GetProcAddress(hLib, "CertOpenSystemStoreW");
        ptrCertFindCertificateInStore = (PtrCertFindCertificateInStore)GetProcAddress(hLib, "CertFindCertificateInStore");
        ptrCertCloseStore = (PtrCertCloseStore)GetProcAddress(hLib, "CertCloseStore");
        if (!ptrCertOpenSystemStoreW || !ptrCertFindCertificateInStore || !ptrCertCloseStore)
            qWarning(lcSsl, "could not resolve symbols in crypt32 library"); // should never happen
    } else {
        qWarning(lcSsl, "could not load crypt32 library"); // should never happen
    }
#elif defined(Q_OS_QNX)
    s_loadRootCertsOnDemand = true;
#elif defined(Q_OS_UNIX) && !defined(Q_OS_MAC)
    // check whether we can enable on-demand root-cert loading (i.e. check whether the sym links are there)
    QList<QByteArray> dirs = unixRootCertDirectories();
    QStringList symLinkFilter;
    symLinkFilter << QLatin1String("[0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f].[0-9]");
    for (int a = 0; a < dirs.count(); ++a) {
        QDirIterator iterator(QLatin1String(dirs.at(a)), symLinkFilter, QDir::Files);
        if (iterator.hasNext()) {
            s_loadRootCertsOnDemand = true;
            break;
        }
    }
#endif
#endif // QT_CONFIG(library)
    // if on-demand loading was not enabled, load the certs now
    if (!s_loadRootCertsOnDemand)
        setDefaultCaCertificates(systemCaCertificates());
#ifdef Q_OS_WIN
    //Enabled for fetching additional root certs from windows update on windows 6+
    //This flag is set false by setDefaultCaCertificates() indicating the app uses
    //its own cert bundle rather than the system one.
    //Same logic that disables the unix on demand cert loading.
    //Unlike unix, we do preload the certificates from the cert store.
    if ((QSysInfo::windowsVersion() & QSysInfo::WV_NT_based) >= QSysInfo::WV_6_0)
        s_loadRootCertsOnDemand = true;
#endif
}

/*!
    \internal

    Declared static in SslUnsafeSocketPrivate, makes sure the SSL libraries have
    been initialized.
*/

void SslUnsafeSocketPrivate::ensureInitialized()
{
    if (!supportsSsl())
        return;

    ensureCiphersAndCertsLoaded();
}

long SslUnsafeSocketPrivate::sslLibraryVersionNumber()
{
    if (!supportsSsl())
        return 0;

    return uq_SSLeay();
}

QString SslUnsafeSocketPrivate::sslLibraryVersionString()
{
    if (!supportsSsl())
        return QString();

    const char *versionString = uq_SSLeay_version(SSLEAY_VERSION);
    if (!versionString)
        return QString();

    return QString::fromLatin1(versionString);
}

long SslUnsafeSocketPrivate::sslLibraryBuildVersionNumber()
{
    return OPENSSL_VERSION_NUMBER;
}

QString SslUnsafeSocketPrivate::sslLibraryBuildVersionString()
{
    // Using QStringLiteral to store the version string as unicode and
    // avoid false positives from Google searching the playstore for old
    // SSL versions. See QTBUG-46265
    return QStringLiteral(OPENSSL_VERSION_TEXT);
}

/*!
    \internal

    Declared static in SslUnsafeSocketPrivate, backend-dependent loading of
    application-wide global ciphers.
*/
void SslUnsafeSocketPrivate::resetDefaultCiphers()
{
    SSL_CTX *myCtx = uq_SSL_CTX_new(uq_SSLv23_client_method());
    SSL *mySsl = uq_SSL_new(myCtx);

    QList<SslUnsafeCipher> ciphers;
    QList<SslUnsafeCipher> defaultCiphers;

    STACK_OF(SSL_CIPHER) *supportedCiphers = uq_SSL_get_ciphers(mySsl);
    for (int i = 0; i < uq_sk_SSL_CIPHER_num(supportedCiphers); ++i) {
        if (SSL_CIPHER *cipher = uq_sk_SSL_CIPHER_value(supportedCiphers, i)) {
            SslUnsafeCipher ciph = SslUnsafeSocketBackendPrivate::SslUnsafeCipher_from_SSL_CIPHER(cipher);
            if (!ciph.isNull()) {
                // Unconditionally exclude ADH and AECDH ciphers since they offer no MITM protection
                if (!ciph.name().toLower().startsWith(QLatin1String("adh")) &&
                    !ciph.name().toLower().startsWith(QLatin1String("exp-adh")) &&
                    !ciph.name().toLower().startsWith(QLatin1String("aecdh"))) {
                    ciphers << ciph;

                    if (ciph.usedBits() >= 128)
                        defaultCiphers << ciph;
                }
            }
        }
    }

    uq_SSL_CTX_free(myCtx);
    uq_SSL_free(mySsl);

    setDefaultSupportedCiphers(ciphers);
    setDefaultCiphers(defaultCiphers);
}

void SslUnsafeSocketPrivate::resetDefaultEllipticCurves()
{
    QVector<SslUnsafeEllipticCurve> curves;

#ifndef OPENSSL_NO_EC
    const size_t curveCount = uq_EC_get_builtin_curves(NULL, 0);

    QVarLengthArray<EC_builtin_curve> builtinCurves(static_cast<int>(curveCount));

    if (uq_EC_get_builtin_curves(builtinCurves.data(), curveCount) == curveCount) {
        curves.reserve(int(curveCount));
        for (size_t i = 0; i < curveCount; ++i) {
            SslUnsafeEllipticCurve curve;
            curve.id = builtinCurves[int(i)].nid;
            curves.append(curve);
        }
    }
#endif // OPENSSL_NO_EC

    // set the list of supported ECs, but not the list
    // of *default* ECs. OpenSSL doesn't like forcing an EC for the wrong
    // ciphersuite, so don't try it -- leave the empty list to mean
    // "the implementation will choose the most suitable one".
    setDefaultSupportedEllipticCurves(curves);
}

#ifndef Q_OS_DARWIN // Apple implementation in SslUnsafeSocket_mac_shared.cpp
QList<SslUnsafeCertificate> SslUnsafeSocketPrivate::systemCaCertificates()
{
    ensureInitialized();
#ifdef SslUnsafeSocket_DEBUG
    QElapsedTimer timer;
    timer.start();
#endif
    QList<SslUnsafeCertificate> systemCerts;
#if defined(Q_OS_WIN)
    if (ptrCertOpenSystemStoreW && ptrCertFindCertificateInStore && ptrCertCloseStore) {
        HCERTSTORE hSystemStore;
        hSystemStore = ptrCertOpenSystemStoreW(0, L"ROOT");
        if(hSystemStore) {
            PCCERT_CONTEXT pc = NULL;
            while(1) {
                pc = ptrCertFindCertificateInStore( hSystemStore, X509_ASN_ENCODING, 0, CERT_FIND_ANY, NULL, pc);
                if(!pc)
                    break;
                QByteArray der((const char *)(pc->pbCertEncoded), static_cast<int>(pc->cbCertEncoded));
                SslUnsafeCertificate cert(der, QSsl::Der);
                systemCerts.append(cert);
            }
            ptrCertCloseStore(hSystemStore, 0);
        }
    }
#elif defined(Q_OS_UNIX)
    QSet<QString> certFiles;
    QDir currentDir;
    QStringList nameFilters;
    QList<QByteArray> directories;
    QSsl::EncodingFormat platformEncodingFormat;
# ifndef Q_OS_ANDROID
    directories = unixRootCertDirectories();
    nameFilters << QLatin1String("*.pem") << QLatin1String("*.crt");
    platformEncodingFormat = QSsl::Pem;
# else
    // Q_OS_ANDROID
    QByteArray ministroPath = qgetenv("MINISTRO_SSL_CERTS_PATH"); // Set by Ministro
    directories << ministroPath;
    nameFilters << QLatin1String("*.der");
    platformEncodingFormat = QSsl::Der;
    if (ministroPath.isEmpty()) {
        QList<QByteArray> certificateData = fetchSslCertificateData();
        for (int i = 0; i < certificateData.size(); ++i) {
            systemCerts.append(SslUnsafeCertificate::fromData(certificateData.at(i), QSsl::Der));
        }
    } else
# endif //Q_OS_ANDROID
    {
        currentDir.setNameFilters(nameFilters);
        for (int a = 0; a < directories.count(); a++) {
            currentDir.setPath(QLatin1String(directories.at(a)));
            QDirIterator it(currentDir);
            while (it.hasNext()) {
                it.next();
                // use canonical path here to not load the same certificate twice if symlinked
                certFiles.insert(it.fileInfo().canonicalFilePath());
            }
        }
        for (const QString& file : qAsConst(certFiles))
            systemCerts.append(SslUnsafeCertificate::fromPath(file, platformEncodingFormat));
# ifndef Q_OS_ANDROID
        systemCerts.append(SslUnsafeCertificate::fromPath(QLatin1String("/etc/pki/tls/certs/ca-bundle.crt"), QSsl::Pem)); // Fedora, Mandriva
        systemCerts.append(SslUnsafeCertificate::fromPath(QLatin1String("/usr/local/share/certs/ca-root-nss.crt"), QSsl::Pem)); // FreeBSD's ca_root_nss
# endif
    }
#endif
#ifdef SslUnsafeSocket_DEBUG
    qDebug() << "systemCaCertificates retrieval time " << timer.elapsed() << "ms";
    qDebug() << "imported " << systemCerts.count() << " certificates";
#endif

    return systemCerts;
}
#endif // Q_OS_DARWIN

void SslUnsafeSocketBackendPrivate::startClientEncryption()
{
    if (!initSslContext()) {
        setErrorAndEmit(QAbstractSocket::SslInternalError,
                        SslUnsafeSocket::tr("Unable to init SSL Context: %1").arg(getErrorsFromOpenSsl()));
        return;
    }

    // Start connecting. This will place outgoing data in the BIO, so we
    // follow up with calling transmit().
    startHandshake();
    transmit();
}

void SslUnsafeSocketBackendPrivate::startServerEncryption()
{
    if (!initSslContext()) {
        setErrorAndEmit(QAbstractSocket::SslInternalError,
                        SslUnsafeSocket::tr("Unable to init SSL Context: %1").arg(getErrorsFromOpenSsl()));
        return;
    }

    // Start connecting. This will place outgoing data in the BIO, so we
    // follow up with calling transmit().
    startHandshake();
    transmit();
}

/*!
    \internal

    Transmits encrypted data between the BIOs and the socket.
*/
void SslUnsafeSocketBackendPrivate::transmit()
{
    Q_Q(SslUnsafeSocket);

    // If we don't have any SSL context, don't bother transmitting.
    if (!ssl)
        return;

    bool transmitting;
    do {
        transmitting = false;

        // If the connection is secure, we can transfer data from the write
        // buffer (in plain text) to the write BIO through SSL_write.
        if (connectionEncrypted && !writeBuffer.isEmpty()) {
            qint64 totalBytesWritten = 0;
            int nextDataBlockSize;
            while ((nextDataBlockSize = writeBuffer.nextDataBlockSize()) > 0) {
                int writtenBytes = uq_SSL_write(ssl, writeBuffer.readPointer(), nextDataBlockSize);
                if (writtenBytes <= 0) {
                    int error = uq_SSL_get_error(ssl, writtenBytes);
                    //write can result in a want_write_error - not an error - continue transmitting
                    if (error == SSL_ERROR_WANT_WRITE) {
                        transmitting = true;
                        break;
                    } else if (error == SSL_ERROR_WANT_READ) {
                        //write can result in a want_read error, possibly due to renegotiation - not an error - stop transmitting
                        transmitting = false;
                        break;
                    } else {
                        // ### Better error handling.
                        setErrorAndEmit(QAbstractSocket::SslInternalError,
                                        SslUnsafeSocket::tr("Unable to write data: %1").arg(
                                            getErrorsFromOpenSsl()));
                        return;
                    }
                }
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: encrypted" << writtenBytes << "bytes";
#endif
                writeBuffer.free(writtenBytes);
                totalBytesWritten += writtenBytes;

                if (writtenBytes < nextDataBlockSize) {
                    // break out of the writing loop and try again after we had read
                    transmitting = true;
                    break;
                }
            }

            if (totalBytesWritten > 0) {
                // Don't emit bytesWritten() recursively.
                if (!emittedBytesWritten) {
                    emittedBytesWritten = true;
                    emit q->bytesWritten(totalBytesWritten);
                    emittedBytesWritten = false;
                }
                emit q->channelBytesWritten(0, totalBytesWritten);
            }
        }

        // Check if we've got any data to be written to the socket.
        QVarLengthArray<char, 4096> data;
        int pendingBytes;
        while (plainSocket->isValid() && (pendingBytes = uq_BIO_pending(writeBio)) > 0) {
            // Read encrypted data from the write BIO into a buffer.
            data.resize(pendingBytes);
            int encryptedBytesRead = uq_BIO_read(writeBio, data.data(), pendingBytes);

            // Write encrypted data from the buffer to the socket.
            qint64 actualWritten = plainSocket->write(data.constData(), encryptedBytesRead);
#ifdef SslUnsafeSocket_DEBUG
            qDebug() << "SslUnsafeSocketBackendPrivate::transmit: wrote" << encryptedBytesRead << "encrypted bytes to the socket" << actualWritten << "actual.";
#endif
            if (actualWritten < 0) {
                //plain socket write fails if it was in the pending close state.
                setErrorAndEmit(plainSocket->error(), plainSocket->errorString());
                return;
            }
            transmitting = true;
        }

        // Check if we've got any data to be read from the socket.
        if (!connectionEncrypted || !readBufferMaxSize || buffer.size() < readBufferMaxSize)
            while ((pendingBytes = plainSocket->bytesAvailable()) > 0) {
                // Read encrypted data from the socket into a buffer.
                data.resize(pendingBytes);
                // just peek() here because q_BIO_write could write less data than expected
                int encryptedBytesRead = plainSocket->peek(data.data(), pendingBytes);

#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: read" << encryptedBytesRead << "encrypted bytes from the socket";
#endif
                // Write encrypted data from the buffer into the read BIO.
                int writtenToBio = uq_BIO_write(readBio, data.constData(), encryptedBytesRead);

                // do the actual read() here and throw away the results.
                if (writtenToBio > 0) {
                    // ### TODO: make this cheaper by not making it memcpy. E.g. make it work with data=0x0 or make it work with seek
                    plainSocket->read(data.data(), writtenToBio);
                } else {
                    // ### Better error handling.
                    setErrorAndEmit(QAbstractSocket::SslInternalError,
                                    SslUnsafeSocket::tr("Unable to decrypt data: %1").arg(
                                        getErrorsFromOpenSsl()));
                    return;
                }

                transmitting = true;
            }

        // If the connection isn't secured yet, this is the time to retry the
        // connect / accept.
        if (!connectionEncrypted) {
#ifdef SslUnsafeSocket_DEBUG
            qDebug() << "SslUnsafeSocketBackendPrivate::transmit: testing encryption";
#endif
            if (startHandshake()) {
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: encryption established";
#endif
                connectionEncrypted = true;
                transmitting = true;
            } else if (plainSocket->state() != QAbstractSocket::ConnectedState) {
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: connection lost";
#endif
                break;
            } else if (paused) {
                // just wait until the user continues
                return;
            } else {
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: encryption not done yet";
#endif
            }
        }

        // If the request is small and the remote host closes the transmission
        // after sending, there's a chance that startHandshake() will already
        // have triggered a shutdown.
        if (!ssl)
            continue;

        // We always read everything from the SSL decryption buffers, even if
        // we have a readBufferMaxSize. There's no point in leaving data there
        // just so that readBuffer.size() == readBufferMaxSize.
        int readBytes = 0;
        data.resize(4096);
        ::memset(data.data(), 0, data.size());
        do {
            // Don't use SSL_pending(). It's very unreliable.
            if ((readBytes = uq_SSL_read(ssl, data.data(), data.size())) > 0) {
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: decrypted" << readBytes << "bytes";
#endif
                buffer.append(data.constData(), readBytes);

                if (readyReadEmittedPointer)
                    *readyReadEmittedPointer = true;
                emit q->readyRead();
                emit q->channelReadyRead(0);
                transmitting = true;
                continue;
            }

            // Error.
            switch (uq_SSL_get_error(ssl, readBytes)) {
            case SSL_ERROR_WANT_READ:
            case SSL_ERROR_WANT_WRITE:
                // Out of data.
                break;
            case SSL_ERROR_ZERO_RETURN:
                // The remote host closed the connection.
#ifdef SslUnsafeSocket_DEBUG
                qDebug() << "SslUnsafeSocketBackendPrivate::transmit: remote disconnect";
#endif
                shutdown = true; // the other side shut down, make sure we do not send shutdown ourselves
                setErrorAndEmit(QAbstractSocket::RemoteHostClosedError,
                                SslUnsafeSocket::tr("The TLS/SSL connection has been closed"));
                return;
            case SSL_ERROR_SYSCALL: // some IO error
            case SSL_ERROR_SSL: // error in the SSL library
                // we do not know exactly what the error is, nor whether we can recover from it,
                // so just return to prevent an endless loop in the outer "while" statement
                setErrorAndEmit(QAbstractSocket::SslInternalError,
                                SslUnsafeSocket::tr("Error while reading: %1").arg(getErrorsFromOpenSsl()));
                return;
            default:
                // SSL_ERROR_WANT_CONNECT, SSL_ERROR_WANT_ACCEPT: can only happen with a
                // BIO_s_connect() or BIO_s_accept(), which we do not call.
                // SSL_ERROR_WANT_X509_LOOKUP: can only happen with a
                // SSL_CTX_set_client_cert_cb(), which we do not call.
                // So this default case should never be triggered.
                setErrorAndEmit(QAbstractSocket::SslInternalError,
                                SslUnsafeSocket::tr("Error while reading: %1").arg(getErrorsFromOpenSsl()));
                break;
            }
        } while (ssl && readBytes > 0);
    } while (ssl && transmitting);
}

static SslUnsafeError _q_OpenSSL_to_SslUnsafeError(int errorCode, const SslUnsafeCertificate &cert)
{
    SslUnsafeError error;
    switch (errorCode) {
    case X509_V_OK:
        // X509_V_OK is also reported if the peer had no certificate.
        break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        error = SslUnsafeError(SslUnsafeError::UnableToGetIssuerCertificate, cert); break;
    case X509_V_ERR_UNABLE_TO_DECRYPT_CERT_SIGNATURE:
        error = SslUnsafeError(SslUnsafeError::UnableToDecryptCertificateSignature, cert); break;
    case X509_V_ERR_UNABLE_TO_DECODE_ISSUER_PUBLIC_KEY:
        error = SslUnsafeError(SslUnsafeError::UnableToDecodeIssuerPublicKey, cert); break;
    case X509_V_ERR_CERT_SIGNATURE_FAILURE:
        error = SslUnsafeError(SslUnsafeError::CertificateSignatureFailed, cert); break;
    case X509_V_ERR_CERT_NOT_YET_VALID:
        error = SslUnsafeError(SslUnsafeError::CertificateNotYetValid, cert); break;
    case X509_V_ERR_CERT_HAS_EXPIRED:
        error = SslUnsafeError(SslUnsafeError::CertificateExpired, cert); break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_BEFORE_FIELD:
        error = SslUnsafeError(SslUnsafeError::InvalidNotBeforeField, cert); break;
    case X509_V_ERR_ERROR_IN_CERT_NOT_AFTER_FIELD:
        error = SslUnsafeError(SslUnsafeError::InvalidNotAfterField, cert); break;
    case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
        error = SslUnsafeError(SslUnsafeError::SelfSignedCertificate, cert); break;
    case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        error = SslUnsafeError(SslUnsafeError::SelfSignedCertificateInChain, cert); break;
    case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        error = SslUnsafeError(SslUnsafeError::UnableToGetLocalIssuerCertificate, cert); break;
    case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        error = SslUnsafeError(SslUnsafeError::UnableToVerifyFirstCertificate, cert); break;
    case X509_V_ERR_CERT_REVOKED:
        error = SslUnsafeError(SslUnsafeError::CertificateRevoked, cert); break;
    case X509_V_ERR_INVALID_CA:
        error = SslUnsafeError(SslUnsafeError::InvalidCaCertificate, cert); break;
    case X509_V_ERR_PATH_LENGTH_EXCEEDED:
        error = SslUnsafeError(SslUnsafeError::PathLengthExceeded, cert); break;
    case X509_V_ERR_INVALID_PURPOSE:
        error = SslUnsafeError(SslUnsafeError::InvalidPurpose, cert); break;
    case X509_V_ERR_CERT_UNTRUSTED:
        error = SslUnsafeError(SslUnsafeError::CertificateUntrusted, cert); break;
    case X509_V_ERR_CERT_REJECTED:
        error = SslUnsafeError(SslUnsafeError::CertificateRejected, cert); break;
    default:
        error = SslUnsafeError(SslUnsafeError::UnspecifiedError, cert); break;
    }
    return error;
}

bool SslUnsafeSocketBackendPrivate::startHandshake()
{
    Q_Q(SslUnsafeSocket);

    // Check if the connection has been established. Get all errors from the
    // verification stage.
    QMutexLocker locker(&_q_sslErrorList()->mutex);
    _q_sslErrorList()->errors.clear();
    int result = (mode == SslUnsafeSocket::SslClientMode) ? uq_SSL_connect(ssl) : uq_SSL_accept(ssl);

    const auto &lastErrors = _q_sslErrorList()->errors;
    if (!lastErrors.isEmpty())
        storePeerCertificates();
    for (const auto &currentError : lastErrors) {
        emit q->peerVerifyError(_q_OpenSSL_to_SslUnsafeError(currentError.code,
                                configuration.peerCertificateChain.value(currentError.depth)));
        if (q->state() != QAbstractSocket::ConnectedState)
            break;
    }

    errorList << lastErrors;
    locker.unlock();

    // Connection aborted during handshake phase.
    if (q->state() != QAbstractSocket::ConnectedState)
        return false;

    // Check if we're encrypted or not.
    if (result <= 0) {
        switch (uq_SSL_get_error(ssl, result)) {
        case SSL_ERROR_WANT_READ:
        case SSL_ERROR_WANT_WRITE:
            // The handshake is not yet complete.
            break;
        default:
            QString errorString
                    = SslUnsafeSocket::tr("Error during SSL handshake: %1").arg(getErrorsFromOpenSsl());
#ifdef SslUnsafeSocket_DEBUG
            qDebug() << "SslUnsafeSocketBackendPrivate::startHandshake: error!" << errorString;
#endif
            setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, errorString);
            q->abort();
        }
        return false;
    }

    // store peer certificate chain
    storePeerCertificates();

    // Start translating errors.
    QList<SslUnsafeError> errors;

#if 0
    // check the whole chain for blacklisting (including root, as we check for subjectInfo and issuer)
    for (const SslUnsafeCertificate &cert : qAsConst(configuration.peerCertificateChain)) {
        if (SslUnsafeCertificatePrivate::isBlacklisted(cert)) {
            SslUnsafeError error(SslUnsafeError::CertificateBlacklisted, cert);
            errors << error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
    }
#endif

    bool doVerifyPeer = configuration.peerVerifyMode == SslUnsafeSocket::VerifyPeer
                        || (configuration.peerVerifyMode == SslUnsafeSocket::AutoVerifyPeer
                            && mode == SslUnsafeSocket::SslClientMode);

    // Check the peer certificate itself. First try the subject's common name
    // (CN) as a wildcard, then try all alternate subject name DNS entries the
    // same way.
    if (!configuration.peerCertificate.isNull()) {
        // but only if we're a client connecting to a server
        // if we're the server, don't check CN
        if (mode == SslUnsafeSocket::SslClientMode) {
            QString peerName = (verificationPeerName.isEmpty () ? q->peerName() : verificationPeerName);

            if (!isMatchingHostname(configuration.peerCertificate, peerName)) {
                // No matches in common names or alternate names.
                SslUnsafeError error(SslUnsafeError::HostNameMismatch, configuration.peerCertificate);
                errors << error;
                emit q->peerVerifyError(error);
                if (q->state() != QAbstractSocket::ConnectedState)
                    return false;
            }
        }
    } else {
        // No peer certificate presented. Report as error if the socket
        // expected one.
        if (doVerifyPeer) {
            SslUnsafeError error(SslUnsafeError::NoPeerCertificate);
            errors << error;
            emit q->peerVerifyError(error);
            if (q->state() != QAbstractSocket::ConnectedState)
                return false;
        }
    }

    // Translate errors from the error list into SslUnsafeErrors.
    errors.reserve(errors.size() + errorList.size());
    for (const auto &error : qAsConst(errorList))
        errors << _q_OpenSSL_to_SslUnsafeError(error.code, configuration.peerCertificateChain.value(error.depth));

    if (!errors.isEmpty()) {
        sslErrors = errors;

#ifdef Q_OS_WIN
        //Skip this if not using system CAs, or if the SSL errors are configured in advance to be ignorable
        if (doVerifyPeer
            && s_loadRootCertsOnDemand
            && allowRootCertOnDemandLoading
            && !verifyErrorsHaveBeenIgnored()) {
            //Windows desktop versions starting from vista ship with minimal set of roots
            //and download on demand from the windows update server CA roots that are
            //trusted by MS.
            //However, this is only transparent if using WinINET - we have to trigger it
            //ourselves.
            SslUnsafeCertificate certToFetch;
            bool fetchCertificate = true;
            for (int i=0; i< sslErrors.count(); i++) {
                switch (sslErrors.at(i).error()) {
                case SslUnsafeError::UnableToGetLocalIssuerCertificate: // site presented intermediate cert, but root is unknown
                case SslUnsafeError::SelfSignedCertificateInChain: // site presented a complete chain, but root is unknown
                    certToFetch = sslErrors.at(i).certificate();
                    break;
                case SslUnsafeError::SelfSignedCertificate:
                case SslUnsafeError::CertificateBlacklisted:
                    //With these errors, we know it will be untrusted so save time by not asking windows
                    fetchCertificate = false;
                    break;
                default:
#ifdef SslUnsafeSocket_DEBUG
                    qDebug() << sslErrors.at(i).errorString();
#endif
                    break;
                }
            }
            if (fetchCertificate && !certToFetch.isNull()) {
                fetchCaRootForCert(certToFetch);
                return false;
            }
        }
#endif
        if (!checkSslErrors())
            return false;
        // A slot, attached to sslErrors signal can call
        // abort/close/disconnetFromHost/etc; no need to
        // continue handshake then.
        if (q->state() != QAbstractSocket::ConnectedState)
            return false;
    } else {
        sslErrors.clear();
    }

    continueHandshake();
    return true;
}

void SslUnsafeSocketBackendPrivate::storePeerCertificates()
{
    // Store the peer certificate and chain. For clients, the peer certificate
    // chain includes the peer certificate; for servers, it doesn't. Both the
    // peer certificate and the chain may be empty if the peer didn't present
    // any certificate.
    X509 *x509 = uq_SSL_get_peer_certificate(ssl);
    configuration.peerCertificate = SslUnsafeCertificatePrivate::SslUnsafeCertificate_from_X509(x509);
    uq_X509_free(x509);
    if (configuration.peerCertificateChain.isEmpty()) {
        configuration.peerCertificateChain = STACKOFX509_to_SslUnsafeCertificates(uq_SSL_get_peer_cert_chain(ssl));
        if (!configuration.peerCertificate.isNull() && mode == SslUnsafeSocket::SslServerMode)
            configuration.peerCertificateChain.prepend(configuration.peerCertificate);
    }
}

bool SslUnsafeSocketBackendPrivate::checkSslErrors()
{
    Q_Q(SslUnsafeSocket);
    if (sslErrors.isEmpty())
        return true;

    emit q->sslErrors(sslErrors);

    bool doVerifyPeer = configuration.peerVerifyMode == SslUnsafeSocket::VerifyPeer
                        || (configuration.peerVerifyMode == SslUnsafeSocket::AutoVerifyPeer
                            && mode == SslUnsafeSocket::SslClientMode);
    bool doEmitSslError = !verifyErrorsHaveBeenIgnored();
    // check whether we need to emit an SSL handshake error
    if (doVerifyPeer && doEmitSslError) {
        if (q->pauseMode() & QAbstractSocket::PauseOnSslErrors) {
            pauseSocketNotifiers(q);
            paused = true;
        } else {
            setErrorAndEmit(QAbstractSocket::SslHandshakeFailedError, sslErrors.constFirst().errorString());
            plainSocket->disconnectFromHost();
        }
        return false;
    }
    return true;
}

unsigned int SslUnsafeSocketBackendPrivate::tlsPskClientCallback(const char *hint,
                                                            char *identity, unsigned int max_identity_len,
                                                            unsigned char *psk, unsigned int max_psk_len)
{
    SslUnsafePreSharedKeyAuthenticator authenticator;

    // Fill in some read-only fields (for the user)
    if (hint)
        authenticator.d->identityHint = QByteArray::fromRawData(hint, int(::strlen(hint))); // it's NUL terminated, but do not include the NUL

    authenticator.d->maximumIdentityLength = int(max_identity_len) - 1; // needs to be NUL terminated
    authenticator.d->maximumPreSharedKeyLength = int(max_psk_len);

    // Let the client provide the remaining bits...
    Q_Q(SslUnsafeSocket);
    emit q->preSharedKeyAuthenticationRequired(&authenticator);

    // No PSK set? Return now to make the handshake fail
    if (authenticator.preSharedKey().isEmpty())
        return 0;

    // Copy data back into OpenSSL
    const int identityLength = qMin(authenticator.identity().length(), authenticator.maximumIdentityLength());
    ::memcpy(identity, authenticator.identity().constData(), identityLength);
    identity[identityLength] = 0;

    const int pskLength = qMin(authenticator.preSharedKey().length(), authenticator.maximumPreSharedKeyLength());
    ::memcpy(psk, authenticator.preSharedKey().constData(), pskLength);
    return pskLength;
}

unsigned int SslUnsafeSocketBackendPrivate::tlsPskServerCallback(const char *identity,
                                                            unsigned char *psk, unsigned int max_psk_len)
{
    SslUnsafePreSharedKeyAuthenticator authenticator;

    // Fill in some read-only fields (for the user)
    authenticator.d->identityHint = configuration.preSharedKeyIdentityHint;
    authenticator.d->identity = identity;
    authenticator.d->maximumIdentityLength = 0; // user cannot set an identity
    authenticator.d->maximumPreSharedKeyLength = int(max_psk_len);

    // Let the client provide the remaining bits...
    Q_Q(SslUnsafeSocket);
    emit q->preSharedKeyAuthenticationRequired(&authenticator);

    // No PSK set? Return now to make the handshake fail
    if (authenticator.preSharedKey().isEmpty())
        return 0;

    // Copy data back into OpenSSL
    const int pskLength = qMin(authenticator.preSharedKey().length(), authenticator.maximumPreSharedKeyLength());
    ::memcpy(psk, authenticator.preSharedKey().constData(), pskLength);
    return pskLength;
}

#ifdef Q_OS_WIN

void SslUnsafeSocketBackendPrivate::fetchCaRootForCert(const SslUnsafeCertificate &cert)
{
    Q_Q(SslUnsafeSocket);
    //The root certificate is downloaded from windows update, which blocks for 15 seconds in the worst case
    //so the request is done in a worker thread.
    QWindowsCaRootFetcher *fetcher = new QWindowsCaRootFetcher(cert, mode);
    QObject::connect(fetcher, SIGNAL(finished(SslUnsafeCertificate,SslUnsafeCertificate)), q, SLOT(_q_caRootLoaded(SslUnsafeCertificate,SslUnsafeCertificate)), Qt::QueuedConnection);
    QMetaObject::invokeMethod(fetcher, "start", Qt::QueuedConnection);
    pauseSocketNotifiers(q);
    paused = true;
}

//This is the callback from QWindowsCaRootFetcher, trustedRoot will be invalid (default constructed) if it failed.
void SslUnsafeSocketBackendPrivate::_q_caRootLoaded(SslUnsafeCertificate cert, SslUnsafeCertificate trustedRoot)
{
    Q_Q(SslUnsafeSocket);
    if (!trustedRoot.isNull() && !trustedRoot.isBlacklisted()) {
        if (s_loadRootCertsOnDemand) {
            //Add the new root cert to default cert list for use by future sockets
            SslUnsafeSocket::addDefaultCaCertificate(trustedRoot);
        }
        //Add the new root cert to this socket for future connections
        q->addCaCertificate(trustedRoot);
        //Remove the broken chain ssl errors (as chain is verified by windows)
        for (int i=sslErrors.count() - 1; i >= 0; --i) {
            if (sslErrors.at(i).certificate() == cert) {
                switch (sslErrors.at(i).error()) {
                case SslUnsafeError::UnableToGetLocalIssuerCertificate:
                case SslUnsafeError::CertificateUntrusted:
                case SslUnsafeError::UnableToVerifyFirstCertificate:
                case SslUnsafeError::SelfSignedCertificateInChain:
                    // error can be ignored if OS says the chain is trusted
                    sslErrors.removeAt(i);
                    break;
                default:
                    // error cannot be ignored
                    break;
                }
            }
        }
    }
    // Continue with remaining errors
    if (plainSocket)
        plainSocket->resume();
    paused = false;
    if (checkSslErrors() && ssl) {
        bool willClose = (autoStartHandshake && pendingClose);
        continueHandshake();
        if (!willClose)
            transmit();
    }
}

class QWindowsCaRootFetcherThread : public QThread
{
public:
    QWindowsCaRootFetcherThread()
    {
        qRegisterMetaType<SslUnsafeCertificate>();
        setObjectName(QStringLiteral("QWindowsCaRootFetcher"));
        start();
    }
    ~QWindowsCaRootFetcherThread()
    {
        quit();
        wait(15500); // worst case, a running request can block for 15 seconds
    }
};

Q_GLOBAL_STATIC(QWindowsCaRootFetcherThread, windowsCaRootFetcherThread);

QWindowsCaRootFetcher::QWindowsCaRootFetcher(const SslUnsafeCertificate &certificate, SslUnsafeSocket::SslMode sslMode)
    : cert(certificate), mode(sslMode)
{
    moveToThread(windowsCaRootFetcherThread());
}

QWindowsCaRootFetcher::~QWindowsCaRootFetcher()
{
}

void QWindowsCaRootFetcher::start()
{
    QByteArray der = cert.toDer();
    PCCERT_CONTEXT wincert = CertCreateCertificateContext(X509_ASN_ENCODING, (const BYTE *)der.constData(), der.length());
    if (!wincert) {
#ifdef SslUnsafeSocket_DEBUG
        qDebug(lcSsl, "QWindowsCaRootFetcher failed to convert certificate to windows form");
#endif
        emit finished(cert, SslUnsafeCertificate());
        deleteLater();
        return;
    }

    CERT_CHAIN_PARA parameters;
    memset(&parameters, 0, sizeof(parameters));
    parameters.cbSize = sizeof(parameters);
    // set key usage constraint
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    LPSTR oid = (LPSTR)(mode == SslUnsafeSocket::SslClientMode ? szOID_PKIX_KP_SERVER_AUTH : szOID_PKIX_KP_CLIENT_AUTH);
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier = &oid;

#ifdef SslUnsafeSocket_DEBUG
    QElapsedTimer stopwatch;
    stopwatch.start();
#endif
    PCCERT_CHAIN_CONTEXT chain;
    BOOL result = CertGetCertificateChain(
        0, //default engine
        wincert,
        0, //current date/time
        0, //default store
        &parameters,
        0, //default dwFlags
        0, //reserved
        &chain);
#ifdef SslUnsafeSocket_DEBUG
    qDebug() << "QWindowsCaRootFetcher" << stopwatch.elapsed() << "ms to get chain";
#endif

    SslUnsafeCertificate trustedRoot;
    if (result) {
#ifdef SslUnsafeSocket_DEBUG
        qDebug() << "QWindowsCaRootFetcher - examining windows chains";
        if (chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR)
            qDebug() << " - TRUSTED";
        else
            qDebug() << " - NOT TRUSTED" << chain->TrustStatus.dwErrorStatus;
        if (chain->TrustStatus.dwInfoStatus & CERT_TRUST_IS_SELF_SIGNED)
            qDebug() << " - SELF SIGNED";
        qDebug() << "SslUnsafeSocketBackendPrivate::fetchCaRootForCert - dumping simple chains";
        for (unsigned int i = 0; i < chain->cChain; i++) {
            if (chain->rgpChain[i]->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR)
                qDebug() << " - TRUSTED SIMPLE CHAIN" << i;
            else
                qDebug() << " - UNTRUSTED SIMPLE CHAIN" << i << "reason:" << chain->rgpChain[i]->TrustStatus.dwErrorStatus;
            for (unsigned int j = 0; j < chain->rgpChain[i]->cElement; j++) {
                SslUnsafeCertificate foundCert(QByteArray((const char *)chain->rgpChain[i]->rgpElement[j]->pCertContext->pbCertEncoded
                    , chain->rgpChain[i]->rgpElement[j]->pCertContext->cbCertEncoded), QSsl::Der);
                qDebug() << "   - " << foundCert;
            }
        }
        qDebug() << " - and" << chain->cLowerQualityChainContext << "low quality chains"; //expect 0, we haven't asked for them
#endif

        //based on http://msdn.microsoft.com/en-us/library/windows/desktop/aa377182%28v=vs.85%29.aspx
        //about the final chain rgpChain[cChain-1] which must begin with a trusted root to be valid
        if (chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR
            && chain->cChain > 0) {
            const PCERT_SIMPLE_CHAIN finalChain = chain->rgpChain[chain->cChain - 1];
            // http://msdn.microsoft.com/en-us/library/windows/desktop/aa377544%28v=vs.85%29.aspx
            // rgpElement[0] is the end certificate chain element. rgpElement[cElement-1] is the self-signed "root" certificate element.
            if (finalChain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR
                && finalChain->cElement > 0) {
                    trustedRoot = SslUnsafeCertificate(QByteArray((const char *)finalChain->rgpElement[finalChain->cElement - 1]->pCertContext->pbCertEncoded
                        , finalChain->rgpElement[finalChain->cElement - 1]->pCertContext->cbCertEncoded), QSsl::Der);
            }
        }
        CertFreeCertificateChain(chain);
    }
    CertFreeCertificateContext(wincert);

    emit finished(cert, trustedRoot);
    deleteLater();
}
#endif

void SslUnsafeSocketBackendPrivate::disconnectFromHost()
{
    if (ssl) {
        if (!shutdown) {
            uq_SSL_shutdown(ssl);
            shutdown = true;
            transmit();
        }
    }
    plainSocket->disconnectFromHost();
}

void SslUnsafeSocketBackendPrivate::disconnected()
{
    if (plainSocket->bytesAvailable() <= 0)
        destroySslContext();
    else {
        // Move all bytes into the plain buffer
        qint64 tmpReadBufferMaxSize = readBufferMaxSize;
        readBufferMaxSize = 0; // reset temporarily so the plain socket buffer is completely drained
        transmit();
        readBufferMaxSize = tmpReadBufferMaxSize;
    }
    //if there is still buffered data in the plain socket, don't destroy the ssl context yet.
    //it will be destroyed when the socket is deleted.
}

SslUnsafeCipher SslUnsafeSocketBackendPrivate::sessionCipher() const
{
    if (!ssl)
        return SslUnsafeCipher();
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
    // FIXME This is fairly evil, but needed to keep source level compatibility
    // with the OpenSSL 0.9.x implementation at maximum -- some other functions
    // don't take a const SSL_CIPHER* when they should
    SSL_CIPHER *sessionCipher = const_cast<SSL_CIPHER *>(uq_SSL_get_current_cipher(ssl));
#else
    SSL_CIPHER *sessionCipher = uq_SSL_get_current_cipher(ssl);
#endif
    return sessionCipher ? SslUnsafeCipher_from_SSL_CIPHER(sessionCipher) : SslUnsafeCipher();
}

QSsl::SslProtocol SslUnsafeSocketBackendPrivate::sessionProtocol() const
{
    if (!ssl)
        return QSsl::UnknownProtocol;
    int ver = uq_SSL_version(ssl);

    switch (ver) {
    case 0x2:
        return QSsl::SslV2;
    case 0x300:
        return QSsl::SslV3;
    case 0x301:
        return QSsl::TlsV1_0;
    case 0x302:
        return QSsl::TlsV1_1;
    case 0x303:
        return QSsl::TlsV1_2;
    }

    return QSsl::UnknownProtocol;
}

void SslUnsafeSocketBackendPrivate::continueHandshake()
{
    Q_Q(SslUnsafeSocket);
    // if we have a max read buffer size, reset the plain socket's to match
    if (readBufferMaxSize)
        plainSocket->setReadBufferSize(readBufferMaxSize);

    if (uq_SSL_ctrl((ssl), SSL_CTRL_GET_SESSION_REUSED, 0, NULL))
        configuration.peerSessionShared = true;

#ifdef QT_DECRYPT_SSL_TRAFFIC
    if (ssl->session && ssl->s3) {
        const char *mk = reinterpret_cast<const char *>(ssl->session->master_key);
        QByteArray masterKey(mk, ssl->session->master_key_length);
        const char *random = reinterpret_cast<const char *>(ssl->s3->client_random);
        QByteArray clientRandom(random, SSL3_RANDOM_SIZE);

        // different format, needed for e.g. older Wireshark versions:
//        const char *sid = reinterpret_cast<const char *>(ssl->session->session_id);
//        QByteArray sessionID(sid, ssl->session->session_id_length);
//        QByteArray debugLineRSA("RSA Session-ID:");
//        debugLineRSA.append(sessionID.toHex().toUpper());
//        debugLineRSA.append(" Master-Key:");
//        debugLineRSA.append(masterKey.toHex().toUpper());
//        debugLineRSA.append("\n");

        QByteArray debugLineClientRandom("CLIENT_RANDOM ");
        debugLineClientRandom.append(clientRandom.toHex().toUpper());
        debugLineClientRandom.append(" ");
        debugLineClientRandom.append(masterKey.toHex().toUpper());
        debugLineClientRandom.append("\n");

        QString sslKeyFile = QDir::tempPath() + QLatin1String("/qt-ssl-keys");
        QFile file(sslKeyFile);
        if (!file.open(QIODevice::Append))
            qWarning() << "could not open file" << sslKeyFile << "for appending";
        if (!file.write(debugLineClientRandom))
            qWarning() << "could not write to file" << sslKeyFile;
        file.close();
    } else {
        qWarning() << "could not decrypt SSL traffic";
    }
#endif

    // Cache this SSL session inside the SslUnsafeContext
    if (!(configuration.sslOptions & QSsl::SslOptionDisableSessionSharing)) {
        if (!sslContextPointer->cacheSession(ssl)) {
            sslContextPointer.clear(); // we could not cache the session
        } else {
            // Cache the session for permanent usage as well
            if (!(configuration.sslOptions & QSsl::SslOptionDisableSessionPersistence)) {
                if (!sslContextPointer->sessionASN1().isEmpty())
                    configuration.sslSession = sslContextPointer->sessionASN1();
                configuration.sslSessionTicketLifeTimeHint = sslContextPointer->sessionTicketLifeTimeHint();
            }
        }
    }

#if OPENSSL_VERSION_NUMBER >= 0x1000100fL && !defined(OPENSSL_NO_NEXTPROTONEG)

    configuration.nextProtocolNegotiationStatus = sslContextPointer->npnContext().status;
    if (sslContextPointer->npnContext().status == SslUnsafeConfiguration::NextProtocolNegotiationUnsupported) {
        // we could not agree -> be conservative and use HTTP/1.1
        configuration.nextNegotiatedProtocol = QByteArrayLiteral("http/1.1");
    } else {
        const unsigned char *proto = 0;
        unsigned int proto_len = 0;
#if OPENSSL_VERSION_NUMBER >= 0x10002000L
        if (uq_SSLeay() >= 0x10002000L) {
            uq_SSL_get0_alpn_selected(ssl, &proto, &proto_len);
            if (proto_len && mode == SslUnsafeSocket::SslClientMode) {
                // Client does not have a callback that sets it ...
                configuration.nextProtocolNegotiationStatus = SslUnsafeConfiguration::NextProtocolNegotiationNegotiated;
            }
        }

        if (!proto_len) { // Test if NPN was more lucky ...
#else
        {
#endif
            uq_SSL_get0_next_proto_negotiated(ssl, &proto, &proto_len);
        }

        if (proto_len)
            configuration.nextNegotiatedProtocol = QByteArray(reinterpret_cast<const char *>(proto), proto_len);
        else
            configuration.nextNegotiatedProtocol.clear();
    }
#endif // OPENSSL_VERSION_NUMBER >= 0x1000100fL ...

#if OPENSSL_VERSION_NUMBER >= 0x10002000L
    if (uq_SSLeay() >= 0x10002000L && mode == SslUnsafeSocket::SslClientMode) {
        EVP_PKEY *key;
        if (q_SSL_get_server_tmp_key(ssl, &key))
            configuration.ephemeralServerKey = SslUnsafeKey(key, QSsl::PublicKey);
    }
#endif // OPENSSL_VERSION_NUMBER >= 0x10002000L ...

    connectionEncrypted = true;
    emit q->encrypted();
    if (autoStartHandshake && pendingClose) {
        pendingClose = false;
        q->disconnectFromHost();
    }
}

QList<SslUnsafeCertificate> SslUnsafeSocketBackendPrivate::STACKOFX509_to_SslUnsafeCertificates(STACK_OF(X509) *x509)
{
    ensureInitialized();
    QList<SslUnsafeCertificate> certificates;
    for (int i = 0; i < uq_sk_X509_num(x509); ++i) {
        if (X509 *entry = uq_sk_X509_value(x509, i))
            certificates << SslUnsafeCertificatePrivate::SslUnsafeCertificate_from_X509(entry);
    }
    return certificates;
}

QList<SslUnsafeError> SslUnsafeSocketBackendPrivate::verify(const QList<SslUnsafeCertificate> &certificateChain, const QString &hostName)
{
    QList<SslUnsafeError> errors;
    if (certificateChain.count() <= 0) {
        errors << SslUnsafeError(SslUnsafeError::UnspecifiedError);
        return errors;
    }

    // Setup the store with the default CA certificates
    X509_STORE *certStore = uq_X509_STORE_new();
    if (!certStore) {
        qWarning() << "Unable to create certificate store";
        errors << SslUnsafeError(SslUnsafeError::UnspecifiedError);
        return errors;
    }

    if (s_loadRootCertsOnDemand) {
        setDefaultCaCertificates(defaultCaCertificates() + systemCaCertificates());
    }

    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto caCertificates = SslUnsafeConfiguration::defaultConfiguration().caCertificates();
    for (const SslUnsafeCertificate &caCertificate : caCertificates) {
        // From https://www.openssl.org/docs/ssl/SSL_CTX_load_verify_locations.html:
        //
        // If several CA certificates matching the name, key identifier, and
        // serial number condition are available, only the first one will be
        // examined. This may lead to unexpected results if the same CA
        // certificate is available with different expiration dates. If a
        // ``certificate expired'' verification error occurs, no other
        // certificate will be searched. Make sure to not have expired
        // certificates mixed with valid ones.
        //
        // See also: SslUnsafeContext::fromConfiguration()
        if (caCertificate.expiryDate() >= now) {
            uq_X509_STORE_add_cert(certStore, reinterpret_cast<X509 *>(caCertificate.handle()));
        }
    }

    QMutexLocker sslErrorListMutexLocker(&_q_sslErrorList()->mutex);

    // Register a custom callback to get all verification errors.
    X509_STORE_set_verify_cb_func(certStore, uq_X509Callback);

    // Build the chain of intermediate certificates
    STACK_OF(X509) *intermediates = 0;
    if (certificateChain.length() > 1) {
        intermediates = (STACK_OF(X509) *) uq_sk_new_null();

        if (!intermediates) {
            uq_X509_STORE_free(certStore);
            errors << SslUnsafeError(SslUnsafeError::UnspecifiedError);
            return errors;
        }

        bool first = true;
        for (const SslUnsafeCertificate &cert : certificateChain) {
            if (first) {
                first = false;
                continue;
            }
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
            uq_sk_push( (_STACK *)intermediates, reinterpret_cast<X509 *>(cert.handle()));
#else
            uq_sk_push( (STACK *)intermediates, reinterpret_cast<char *>(cert.handle()));
#endif
        }
    }

    X509_STORE_CTX *storeContext = uq_X509_STORE_CTX_new();
    if (!storeContext) {
        uq_X509_STORE_free(certStore);
        errors << SslUnsafeError(SslUnsafeError::UnspecifiedError);
        return errors;
    }

    if (!uq_X509_STORE_CTX_init(storeContext, certStore, reinterpret_cast<X509 *>(certificateChain[0].handle()), intermediates)) {
        uq_X509_STORE_CTX_free(storeContext);
        uq_X509_STORE_free(certStore);
        errors << SslUnsafeError(SslUnsafeError::UnspecifiedError);
        return errors;
    }

    // Now we can actually perform the verification of the chain we have built.
    // We ignore the result of this function since we process errors via the
    // callback.
    (void) uq_X509_verify_cert(storeContext);

    uq_X509_STORE_CTX_free(storeContext);
#if OPENSSL_VERSION_NUMBER >= 0x10000000L
    uq_sk_free( (_STACK *) intermediates);
#else
    uq_sk_free( (STACK *) intermediates);
#endif

    // Now process the errors
    const auto errorList = std::move(_q_sslErrorList()->errors);
    _q_sslErrorList()->errors.clear();

    sslErrorListMutexLocker.unlock();

#if 0
    // Translate the errors
    if (SslUnsafeCertificatePrivate::isBlacklisted(certificateChain[0])) {
        SslUnsafeError error(SslUnsafeError::CertificateBlacklisted, certificateChain[0]);
        errors << error;
    }
#endif

    // Check the certificate name against the hostname if one was specified
    if ((!hostName.isEmpty()) && (!isMatchingHostname(certificateChain[0], hostName))) {
        // No matches in common names or alternate names.
        SslUnsafeError error(SslUnsafeError::HostNameMismatch, certificateChain[0]);
        errors << error;
    }

    // Translate errors from the error list into SslUnsafeErrors.
    errors.reserve(errors.size() + errorList.size());
    for (const auto &error : qAsConst(errorList))
        errors << _q_OpenSSL_to_SslUnsafeError(error.code, certificateChain.value(error.depth));

    uq_X509_STORE_free(certStore);

    return errors;
}

bool SslUnsafeSocketBackendPrivate::importPkcs12(QIODevice *device,
                                            SslUnsafeKey *key, SslUnsafeCertificate *cert,
                                            QList<SslUnsafeCertificate> *caCertificates,
                                            const QByteArray &passPhrase)
{
    if (!supportsSsl())
        return false;

    // These are required
    Q_ASSERT(device);
    Q_ASSERT(key);
    Q_ASSERT(cert);

    // Read the file into a BIO
    QByteArray pkcs12data = device->readAll();
    if (pkcs12data.size() == 0)
        return false;

    BIO *bio = uq_BIO_new_mem_buf(const_cast<char *>(pkcs12data.constData()), pkcs12data.size());

    // Create the PKCS#12 object
    PKCS12 *p12 = uq_d2i_PKCS12_bio(bio, 0);
    if (!p12) {
        qWarning() << "Unable to read PKCS#12 structure, " << uq_ERR_error_string(uq_ERR_get_error(), 0);
        uq_BIO_free(bio);
        return false;
    }

    // Extract the data
    EVP_PKEY *pkey = nullptr;
    X509 *x509;
    STACK_OF(X509) *ca = 0;

    if (!uq_PKCS12_parse(p12, passPhrase.constData(), &pkey, &x509, &ca)) {
        qWarning() << "Unable to parse PKCS#12 structure, %s", uq_ERR_error_string(uq_ERR_get_error(), 0);
        uq_PKCS12_free(p12);
        uq_BIO_free(bio);
        return false;
    }

    // Convert to Qt types
    if (!key->d->fromEVP_PKEY(pkey)) {
        qWarning() << "Unable to convert private key";
        uq_sk_pop_free(reinterpret_cast<STACK *>(ca), reinterpret_cast<void(*)(void*)>(uq_sk_free));
        uq_X509_free(x509);
        uq_EVP_PKEY_free(pkey);
        uq_PKCS12_free(p12);
        uq_BIO_free(bio);

        return false;
    }

    *cert = SslUnsafeCertificatePrivate::SslUnsafeCertificate_from_X509(x509);

    if (caCertificates)
        *caCertificates = SslUnsafeSocketBackendPrivate::STACKOFX509_to_SslUnsafeCertificates(ca);

    // Clean up
    uq_sk_pop_free(reinterpret_cast<STACK *>(ca), reinterpret_cast<void(*)(void*)>(uq_sk_free));
    uq_X509_free(x509);
    uq_EVP_PKEY_free(pkey);
    uq_PKCS12_free(p12);
    uq_BIO_free(bio);

    return true;
}