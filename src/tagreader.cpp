#include "tagreader.h"
#include <tag.h>
#include <taglib/fileref.h>
#include <QTextCodec>

QString TagReader::decodeCp1251IfNeeded(const QString &str)
{
    if (str.isEmpty())
        return str;

    int highCount = 0;
    int letterCount = 0;
    int curConsecutiveHigh = 0;
    int maxConsecutiveHigh = 0;

    for (const QChar &ch : str)
    {
        ushort u = ch.unicode();
        if (u > 0x00FF)
        {
            // Already contains non-Latin1 Unicode (e.g., proper UTF-8/UTF-16 Cyrillic)
            return str;
        }
        bool isCp1251Cyrillic = (u >= 0x00C0 && u <= 0x00FF) || u == 0x00A8 || u == 0x00B8;
        if (isCp1251Cyrillic)
        {
            highCount++;
            letterCount++;
            curConsecutiveHigh++;
            if (curConsecutiveHigh > maxConsecutiveHigh)
                maxConsecutiveHigh = curConsecutiveHigh;
        }
        else
        {
            curConsecutiveHigh = 0;
            if ((u >= 'A' && u <= 'Z') || (u >= 'a' && u <= 'z') || (u >= 0x0080 && u <= 0x00BF))
                letterCount++;
        }
    }

    if (highCount > 0 && (maxConsecutiveHigh >= 2 || highCount * 3 >= letterCount))
    {
        QTextCodec *codec = QTextCodec::codecForName("Windows-1251");
        if (codec)
        {
            return codec->toUnicode(str.toLatin1());
        }
    }
    return str;
}

static QString tagLibStringToQString(const TagLib::String &tstr)
{
    if (tstr.isEmpty())
        return QString();
    if (tstr.isLatin1())
    {
        QString raw = QString::fromLatin1(tstr.toCString(false));
        return TagReader::decodeCp1251IfNeeded(raw);
    }
    return TagReader::decodeCp1251IfNeeded(QString::fromUtf8(tstr.toCString(true)));
}

TagReader::TagReader(QObject *parent) : QObject(parent)
{
    m_logger = spdlog::get("logger");
    m_duration = 0;
    discoverer = gst_discoverer_new(2 * GST_SECOND, nullptr);
}

TagReader::~TagReader()
{
    gst_object_unref(discoverer);
}

QString TagReader::getArtist()
{
    return m_artist;
}

QString TagReader::getTitle()
{
    return m_title;
}

QString TagReader::getAlbum()
{
    return m_album;
}

QString TagReader::getTrack()
{
    return m_track;
}

unsigned int TagReader::getDuration() const
{
    return m_duration;
}

void TagReader::setMedia(const QString& path)
{
    m_logger->info("{} Getting tags for: {}", m_loggingPrefix, path);
    if ((path.endsWith(".mp3", Qt::CaseInsensitive)) || (path.endsWith(".ogg", Qt::CaseInsensitive)) || path.endsWith(".mp4", Qt::CaseInsensitive) || path.endsWith(".m4v", Qt::CaseInsensitive))
    {
        m_logger->info("{} Using taglib to get tags", m_loggingPrefix);
        taglibTags(path);
        return;
    }
    m_logger->info("{} Using GStreamer to get tags", m_loggingPrefix);
    GError *err = nullptr;
    gchar *gstUri = gst_filename_to_uri(path.toUtf8().constData(), nullptr);
    QByteArray fallbackUri;
    const char *uriToUse = gstUri;
    if (!uriToUse)
    {
#ifdef Q_OS_WIN
        QString uri = "file:///" + path;
#else
        QString uri = "file://" + path;
#endif
        fallbackUri = uri.toUtf8().toPercentEncoding("!$&'()*+,;=:/?[]@");
        uriToUse = fallbackUri.constData();
    }
    GstDiscovererInfo *discovererInfo = gst_discoverer_discover_uri(discoverer, uriToUse, &err);
    if (gstUri)
        g_free(gstUri);
    if (err)
    {
        g_printerr("Got error: %s\n", err->message);
        g_error_free(err);
    }
    if (GST_IS_DISCOVERER_INFO(discovererInfo))
    {
        auto duration = gst_discoverer_info_get_duration(discovererInfo);
        m_duration = duration / 1000000;
        m_logger->debug("{} Got duration: {}", m_loggingPrefix, m_duration);
        const GstTagList *tags = gst_discoverer_info_get_tags(discovererInfo);
        if (GST_IS_TAG_LIST(tags))
        {
            gchar *tagVal = nullptr;
            if (gst_tag_list_get_string(tags,"artist",&tagVal))
            {
                m_artist = decodeCp1251IfNeeded(QString::fromUtf8(tagVal));
                g_free(tagVal);
                m_logger->info("{} Got artist tag: {}", m_loggingPrefix, m_artist);
            }
            if (gst_tag_list_get_string(tags,"title",&tagVal))
            {
                m_title = decodeCp1251IfNeeded(QString::fromUtf8(tagVal));
                g_free(tagVal);
                m_logger->info("{} Got title tag: {}", m_loggingPrefix, m_title);
            }
        }
        else
            m_logger->warn("{} Invalid or missing metadata tags", m_loggingPrefix);
        gst_discoverer_info_unref(discovererInfo);
    }
    else
        m_logger->error("{} Error retrieving discovererInfo", m_loggingPrefix);
    m_logger->info("{} Done getting tags for: {}", m_loggingPrefix, path);
}

void TagReader::taglibTags(const QString& path)
{
#ifdef Q_OS_WIN
    TagLib::FileRef f(reinterpret_cast<const wchar_t *>(path.utf16()));
#else
    TagLib::FileRef f(path.toUtf8().constData());
#endif
    if (!f.isNull() && f.tag() && f.audioProperties())
    {
        m_artist = tagLibStringToQString(f.tag()->artist());
        m_title = tagLibStringToQString(f.tag()->title());
        m_duration = f.audioProperties()->length() * 1000;
        m_album = tagLibStringToQString(f.tag()->album());
        auto track = f.tag()->track();
        if (track == 0)
            m_track = QString();
        else if (track < 10)
            m_track = "0" + QString::number(track);
        else
            m_track = QString::number(track);
        m_logger->info("{} Taglib result: Artist: {} - Title: {} - Album: {} - Track: {} - Duration: {}", m_loggingPrefix, m_artist, m_title, m_album, m_track, m_duration);
    }
    else
    {
        m_logger->error("{} Taglib was unable to process the specified file", m_loggingPrefix);
        m_artist = QString();
        m_title = QString();
        m_album = QString();
        m_duration = 0;
    }
}
