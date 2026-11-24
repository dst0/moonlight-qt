// Test helpers that act like "another application" on a private pasteboard.

#include <QByteArray>
#include <QString>
#include <QStringList>

#import <AppKit/AppKit.h>

bool sdsTestWriteTiffOnly(const QString& pasteboardName, const QByteArray& png)
{
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:pasteboardName.toNSString()];
        NSData* data = [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
        NSBitmapImageRep* rep = [NSBitmapImageRep imageRepWithData:data];
        NSData* tiff = rep != nil ? [rep TIFFRepresentation] : nil;
        if (tiff == nil) {
            return false;
        }
        [pb clearContents];
        return [pb setData:tiff forType:NSPasteboardTypeTIFF];
    }
}

QStringList sdsTestPasteboardTypes(const QString& pasteboardName)
{
    QStringList result;
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:pasteboardName.toNSString()];
        for (NSString* type in [pb types]) {
            result.append(QString::fromNSString(type));
        }
    }
    return result;
}

QStringList sdsTestPasteboardFiles(const QString& name)
{
    QStringList paths;
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:name.toNSString()];
        NSArray* urls = [pb readObjectsForClasses:@[[NSURL class]] options:@{NSPasteboardURLReadingFileURLsOnlyKey:@YES}];
        for (NSURL* url in urls) paths.append(QString::fromNSString([url path]));
    }
    return paths;
}

bool sdsTestWriteFileUrls(const QString& name, const QStringList& paths)
{
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:name.toNSString()];
        NSMutableArray* urls = [NSMutableArray array];
        for (const auto& path : paths) [urls addObject:[NSURL fileURLWithPath:path.toNSString()]];
        [pb clearContents];
        return [pb writeObjects:urls];
    }
}

bool sdsTestWritePng(const QString& name, const QByteArray& png)
{
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:name.toNSString()];
        [pb clearContents];
        NSData* data = [NSData dataWithBytes:png.constData() length:static_cast<NSUInteger>(png.size())];
        return [pb setData:data forType:NSPasteboardTypePNG];
    }
}

QByteArray sdsTestReadPng(const QString& name)
{
    @autoreleasepool {
        NSPasteboard* pb = [NSPasteboard pasteboardWithName:name.toNSString()];
        NSData* data = [pb dataForType:NSPasteboardTypePNG];
        if (data == nil || [data length] == 0) {
            return {};
        }
        return QByteArray(static_cast<const char*>([data bytes]), static_cast<qsizetype>([data length]));
    }
}
