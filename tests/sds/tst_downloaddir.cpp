// S9 (never overwrite, O_EXCL) and S14 (quarantine attribute, no execute bits) for the client
// download folder.

#include "streaming/sds/filesanitize.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

#include <cerrno>
#include <fcntl.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#ifdef Q_OS_DARWIN
#include <sys/xattr.h>
#endif

using namespace Sds;

namespace {

QByteArray readFile(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        return QByteArray();
    }
    return f.readAll();
}

bool writeFile(const QString& path, const QByteArray& data)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

QByteArray quarantineOf(const QString& path)
{
#ifdef Q_OS_DARWIN
    char buf[512];
    ssize_t n = getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", buf, sizeof(buf), 0, XATTR_NOFOLLOW);
    return n > 0 ? QByteArray(buf, static_cast<int>(n)) : QByteArray();
#else
    Q_UNUSED(path);
    return QByteArray();
#endif
}

} // namespace

class TestDownloadDir : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    void opensAndCreatesPrivateFolder();
    void refusesSymlinkedFolder();
    void refusesFileAsFolder();
    void refusesWritableSharedFolder();
#ifdef Q_OS_DARWIN
    void refusesAclWritableFolder();
    void refusesInheritedWritablePart();
    void refusesWritablePartAclAtPublication();
    void acceptsReadOnlyAclAndCurrentOwnerMutationAcl();
#endif
    void refusesReplacedPartFile();
    void partFileIsExclusiveAndPrivate();
    void finalizeSetsQuarantineAndNoExecBits();
    void collisionNeverOverwrites();
    void symlinkAtTargetIsNotFollowed();
    void renameNoReplaceIsExclusive();
    void refusesUnsanitizedName();
    void discardRemovesPart();
    void removesStalePartFiles();
    void quarantineValueFormat();

private:
    QTemporaryDir* m_Tmp = nullptr;
    QString m_Folder;
};

void TestDownloadDir::init()
{
    m_Tmp = new QTemporaryDir();
    QVERIFY(m_Tmp->isValid());
    m_Folder = m_Tmp->filePath(QStringLiteral("Downloads/Moonlight"));
}

void TestDownloadDir::cleanup()
{
    delete m_Tmp;
    m_Tmp = nullptr;
}

void TestDownloadDir::opensAndCreatesPrivateFolder()
{
    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY2(dir.open(&error), qPrintable(error));
    QVERIFY(dir.isOpen());

    struct stat st;
    QCOMPARE(::lstat(QFile::encodeName(m_Folder).constData(), &st), 0);
    QVERIFY(S_ISDIR(st.st_mode));
    QCOMPARE(st.st_mode & 0777, mode_t(0700));

    // Opening an existing folder works too
    DownloadDir again(m_Folder);
    QVERIFY2(again.open(&error), qPrintable(error));
}

void TestDownloadDir::refusesSymlinkedFolder()
{
    QString real = m_Tmp->filePath(QStringLiteral("elsewhere"));
    QVERIFY(QDir().mkpath(real));
    QVERIFY(QDir().mkpath(m_Tmp->filePath(QStringLiteral("Downloads"))));
    QCOMPARE(::symlink(QFile::encodeName(real).constData(), QFile::encodeName(m_Folder).constData()), 0);

    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY(!dir.open(&error));
    QVERIFY(error.contains(QStringLiteral("not a real directory")));
}

void TestDownloadDir::refusesFileAsFolder()
{
    QVERIFY(QDir().mkpath(m_Tmp->filePath(QStringLiteral("Downloads"))));
    QVERIFY(writeFile(m_Folder, "not a folder"));

    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY(!dir.open(&error));
}

void TestDownloadDir::refusesWritableSharedFolder()
{
    QVERIFY(QDir().mkpath(m_Folder));
    for (mode_t mode : {mode_t(0770), mode_t(0702)}) {
        QCOMPARE(::chmod(QFile::encodeName(m_Folder).constData(), mode), 0);
        DownloadDir dir(m_Folder);
        QString error;
        QVERIFY2(!dir.open(&error), "a different user must not be able to replace received files");
        QVERIFY(!dir.isOpen());
    }
    QCOMPARE(::chmod(QFile::encodeName(m_Folder).constData(), 0700), 0);
}

void TestDownloadDir::refusesReplacedPartFile()
{
    DownloadDir dir(m_Folder);
    QString error, part, finalName;
    QVERIFY(dir.open(&error));
    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    QCOMPARE(::write(fd, "verified", 8), ssize_t(8));
    const QString source = QDir(m_Folder).filePath(part);
    const QString outside = m_Tmp->filePath(QStringLiteral("outside"));
    QVERIFY(writeFile(outside, "unverified"));
    QCOMPARE(::unlink(QFile::encodeName(source).constData()), 0);
    QCOMPARE(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(source).constData()), 0);
    QVERIFY2(!dir.finalizePartFile(fd, part, QStringLiteral("received.txt"), &finalName, &error),
             "publication must refer to the same file that was written and quarantined");
    QVERIFY(!QFile::exists(QDir(m_Folder).filePath(QStringLiteral("received.txt"))));
    QCOMPARE(readFile(outside), QByteArray("unverified"));
}

#ifdef Q_OS_DARWIN
void TestDownloadDir::refusesAclWritableFolder()
{
    QVERIFY(QDir().mkpath(m_Folder));
    QCOMPARE(::chmod(QFile::encodeName(m_Folder).constData(), 0700), 0);
    QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "user:nobody allow add_file,delete_child", m_Folder}), 0);
    struct stat st;
    QCOMPARE(::stat(QFile::encodeName(m_Folder).constData(), &st), 0);
    QCOMPARE(st.st_mode & 0777, mode_t(0700));
    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY2(!dir.open(&error), "mode 0700 does not exclude another user granted access by an ACL");
}

void TestDownloadDir::refusesInheritedWritablePart()
{
    DownloadDir dir(m_Folder);
    QString error, part;
    QVERIFY(dir.open(&error));
    QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "user:nobody allow write,file_inherit", m_Folder}), 0);
    int fd = dir.createPartFile(&part, &error);
    if (fd >= 0) dir.discardPartFile(fd, part);
    QVERIFY2(fd < 0, "new part files must not inherit write access for a different user");
}

void TestDownloadDir::refusesWritablePartAclAtPublication()
{
    DownloadDir dir(m_Folder);
    QString error, part, finalName;
    QVERIFY(dir.open(&error));
    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    QCOMPARE(::write(fd, "verified", 8), ssize_t(8));
    const QString path = QDir(m_Folder).filePath(part);
    QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "user:nobody allow write", path}), 0);
    QVERIFY2(!dir.finalizePartFile(fd, part, "received.txt", &finalName, &error),
             "an extended ACL must not let another user change a verified download");
    QVERIFY(!QFile::exists(QDir(m_Folder).filePath("received.txt")));
}

void TestDownloadDir::acceptsReadOnlyAclAndCurrentOwnerMutationAcl()
{
    QVERIFY(QDir().mkpath(m_Folder));
    QCOMPARE(::chmod(QFile::encodeName(m_Folder).constData(), 0700), 0);
    QCOMPARE(QProcess::execute("/bin/chmod", {"+a", "user:nobody allow read", m_Folder}), 0);
    const auto* owner = ::getpwuid(::geteuid());
    QVERIFY(owner != nullptr);
    const QString grant = QStringLiteral("user:%1 allow write").arg(QString::fromLocal8Bit(owner->pw_name));
    QCOMPARE(QProcess::execute("/bin/chmod", {"+a", grant, m_Folder}), 0);
    DownloadDir dir(m_Folder);
    QString error, part;
    QVERIFY2(dir.open(&error), qPrintable(error));
    int fd = dir.createPartFile(&part, &error);
    QVERIFY2(fd >= 0, qPrintable(error));
    dir.discardPartFile(fd, part);
}
#endif

void TestDownloadDir::partFileIsExclusiveAndPrivate()
{
    DownloadDir dir(m_Folder);
    QString error, part;
    QVERIFY(dir.open(&error));

    int fd = dir.createPartFile(&part, &error);
    QVERIFY2(fd >= 0, qPrintable(error));
    QVERIFY(QRegularExpression(QStringLiteral("^\\.[0-9a-f]{32}\\.part$")).match(part).hasMatch());

    struct stat st;
    QCOMPARE(::fstat(fd, &st), 0);
    QVERIFY(S_ISREG(st.st_mode));
    QCOMPARE(st.st_mode & 0777, mode_t(0600));

    // The same name cannot be created twice (O_EXCL)
    int dirFd = ::open(QFile::encodeName(m_Folder).constData(), O_RDONLY | O_DIRECTORY);
    QVERIFY(dirFd >= 0);
    int again = ::openat(dirFd, QFile::encodeName(part).constData(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    QCOMPARE(again, -1);
    QCOMPARE(errno, EEXIST);
    ::close(dirFd);

    dir.discardPartFile(fd, part);
}

void TestDownloadDir::finalizeSetsQuarantineAndNoExecBits()
{
    DownloadDir dir(m_Folder);
    QString error, part, finalName;
    QVERIFY(dir.open(&error));

    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    // Even if the data looks like a script it must not become executable
    QByteArray content("#!/bin/sh\necho owned\n");
    QCOMPARE(::write(fd, content.constData(), content.size()), ssize_t(content.size()));
    ::fchmod(fd, 0755); // simulate a hostile mode change before finalize

    QVERIFY2(dir.finalizePartFile(fd, part, QStringLiteral("run.sh"), &finalName, &error), qPrintable(error));
    QCOMPARE(finalName, QStringLiteral("run.sh"));

    QString path = QDir(m_Folder).filePath(finalName);
    QCOMPARE(readFile(path), content);
    QVERIFY(!QFile::exists(QDir(m_Folder).filePath(part)));

    struct stat st;
    QCOMPARE(::lstat(QFile::encodeName(path).constData(), &st), 0);
    QVERIFY(S_ISREG(st.st_mode));
    QCOMPARE(st.st_mode & 0111, mode_t(0)); // S14: no execute bits
    QCOMPARE(st.st_mode & 0777, mode_t(0600));

#ifdef Q_OS_DARWIN
    QByteArray q = quarantineOf(path);
    QVERIFY2(QRegularExpression(QStringLiteral("^0081;[0-9a-f]{8};Moonlight;[0-9A-F-]{36}$"))
             .match(QString::fromLatin1(q)).hasMatch(), q.constData());
#endif
}

void TestDownloadDir::collisionNeverOverwrites()
{
    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY(dir.open(&error));

    QString existing = QDir(m_Folder).filePath(QStringLiteral("report.pdf"));
    QVERIFY(writeFile(existing, "original"));

    for (int i = 1; i <= 3; i++) {
        QString part, finalName;
        int fd = dir.createPartFile(&part, &error);
        QVERIFY(fd >= 0);
        QByteArray content = "download " + QByteArray::number(i);
        QCOMPARE(::write(fd, content.constData(), content.size()), ssize_t(content.size()));
        QVERIFY2(dir.finalizePartFile(fd, part, QStringLiteral("report.pdf"), &finalName, &error), qPrintable(error));
        QCOMPARE(finalName, QStringLiteral("report (%1).pdf").arg(i));
        QCOMPARE(readFile(QDir(m_Folder).filePath(finalName)), content);
    }

    QCOMPARE(readFile(existing), QByteArray("original"));
}

void TestDownloadDir::symlinkAtTargetIsNotFollowed()
{
    DownloadDir dir(m_Folder);
    QString error, part, finalName;
    QVERIFY(dir.open(&error));

    QString outside = m_Tmp->filePath(QStringLiteral("outside-target"));
    QString link = QDir(m_Folder).filePath(QStringLiteral("a.txt"));
    QCOMPARE(::symlink(QFile::encodeName(outside).constData(), QFile::encodeName(link).constData()), 0);

    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    QCOMPARE(::write(fd, "data", 4), ssize_t(4));
    QVERIFY(dir.finalizePartFile(fd, part, QStringLiteral("a.txt"), &finalName, &error));

    QCOMPARE(finalName, QStringLiteral("a (1).txt"));
    QVERIFY(!QFile::exists(outside)); // the dangling symlink's target was never created
    QVERIFY(QFileInfo(link).isSymLink());
}

void TestDownloadDir::renameNoReplaceIsExclusive()
{
    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY(dir.open(&error));

    QVERIFY(writeFile(QDir(m_Folder).filePath(QStringLiteral("src")), "new"));
    QVERIFY(writeFile(QDir(m_Folder).filePath(QStringLiteral("dst")), "old"));

    QCOMPARE(dir.renameNoReplace(QStringLiteral("src"), QStringLiteral("dst")), EEXIST);
    QCOMPARE(readFile(QDir(m_Folder).filePath(QStringLiteral("dst"))), QByteArray("old"));
    QCOMPARE(dir.renameNoReplace(QStringLiteral("src"), QStringLiteral("../escape")), EINVAL);
    QCOMPARE(dir.renameNoReplace(QStringLiteral("src"), QStringLiteral("dst2")), 0);
    QCOMPARE(readFile(QDir(m_Folder).filePath(QStringLiteral("dst2"))), QByteArray("new"));
}

void TestDownloadDir::refusesUnsanitizedName()
{
    DownloadDir dir(m_Folder);
    QString error, part, finalName;
    QVERIFY(dir.open(&error));

    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    QVERIFY(!dir.finalizePartFile(fd, part, QStringLiteral("../escape.txt"), &finalName, &error));
    QVERIFY(!QFile::exists(QDir(m_Folder).filePath(part)));
    QVERIFY(!QFile::exists(m_Tmp->filePath(QStringLiteral("Downloads/escape.txt"))));
}

void TestDownloadDir::discardRemovesPart()
{
    DownloadDir dir(m_Folder);
    QString error, part;
    QVERIFY(dir.open(&error));
    int fd = dir.createPartFile(&part, &error);
    QVERIFY(fd >= 0);
    dir.discardPartFile(fd, part);
    QVERIFY(!QFile::exists(QDir(m_Folder).filePath(part)));
}

void TestDownloadDir::removesStalePartFiles()
{
    QVERIFY(QDir().mkpath(m_Folder));
    QString stale = QDir(m_Folder).filePath(QStringLiteral(".0123456789abcdef0123456789abcdef.part"));
    QString fresh = QDir(m_Folder).filePath(QStringLiteral(".fedcba9876543210fedcba9876543210.part"));
    QString other = QDir(m_Folder).filePath(QStringLiteral(".unrelated.part"));
    QVERIFY(writeFile(stale, "x"));
    QVERIFY(writeFile(fresh, "x"));
    QVERIFY(writeFile(other, "x"));

    struct timeval times[2];
    gettimeofday(&times[0], nullptr);
    times[0].tv_sec -= 3 * 24 * 3600;
    times[1] = times[0];
    QCOMPARE(::utimes(QFile::encodeName(stale).constData(), times), 0);
    QCOMPARE(::utimes(QFile::encodeName(other).constData(), times), 0);

    DownloadDir dir(m_Folder);
    QString error;
    QVERIFY(dir.open(&error));

    QVERIFY(!QFile::exists(stale));
    QVERIFY(QFile::exists(fresh));
    QVERIFY(QFile::exists(other));
}

void TestDownloadDir::quarantineValueFormat()
{
    QUuid id = QUuid::fromString(QStringLiteral("{12345678-9abc-def0-1234-56789abcdef0}"));
    QCOMPARE(quarantineValue(0x691f2b3c, id), QByteArray("0081;691f2b3c;Moonlight;12345678-9ABC-DEF0-1234-56789ABCDEF0"));
    QCOMPARE(quarantineValue(0x10, id).left(14), QByteArray("0081;00000010;"));
}

int runDownloadDirTests(int argc, char** argv)
{
    TestDownloadDir t;
    return QTest::qExec(&t, argc, argv);
}

#include "tst_downloaddir.moc"
