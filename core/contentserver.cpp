/* Copyright (C) 2017 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <QDir>
#include <QFileInfo>
#include <QRegExp>
#include <QImage>
#include <QThread>
#include <QNetworkRequest>
#include <QEventLoop>
#include <QTextStream>
#include <QRegExp>
#include <QTimer>
#include <QAudioFormat>
#include <QAudioDeviceInfo>
#include <QDomDocument>
#include <QDomElement>
#include <QDomNode>
#include <QDomNodeList>
#include <QDomText>
#include <QSslConfiguration>
#include <QTextStream>

#include <iomanip>
#include <memory>

#include "contentserver.h"
#include "utils.h"
#include "settings.h"
#include "tracker.h"
#include "trackercursor.h"
#include "info.h"

// TagLib
#include "fileref.h"
#include "tag.h"
#include "tpropertymap.h"
#include "mpegfile.h"
#include "id3v2frame.h"
#include "id3v2tag.h"
#include "attachedpictureframe.h"

// Libav
#ifdef FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/mathematics.h>
}
#endif

#ifdef SAILFISH
#include <sailfishapp.h>
#include "iconprovider.h"
#endif

ContentServer* ContentServer::m_instance = nullptr;
ContentServerWorker* ContentServerWorker::m_instance = nullptr;

const QString ContentServer::queryTemplate =
        "SELECT ?item " \
        "nie:mimeType(?item) as mime " \
        "nie:title(?item) as title " \
        "nie:comment(?item) as comment " \
        "nfo:duration(?item) as duration " \
        "nie:title(nmm:musicAlbum(?item)) as album " \
        "nmm:artistName(nmm:performer(?item)) as artist " \
        "nfo:averageBitrate(?item) as bitrate " \
        "nfo:channels(?item) as channels " \
        "nfo:sampleRate(?item) as sampleRate " \
        "WHERE { ?item nie:url \"%1\". }";

const QHash<QString,QString> ContentServer::m_imgExtMap {
    {"jpg", "image/jpeg"},{"jpeg", "image/jpeg"},
    {"png", "image/png"},
    {"gif", "image/gif"}
};

const QHash<QString,QString> ContentServer::m_musicExtMap {
    {"mp3", "audio/mpeg"},
    {"m4a", "audio/mp4"}, {"m4b","audio/mp4"},
    {"aac", "audio/aac"},
    {"mpc", "audio/x-musepack"},
    {"flac", "audio/flac"},
    {"wav", "audio/vnd.wav"},
    {"ape", "audio/x-monkeys-audio"},
    {"ogg", "audio/ogg"}, {"oga", "audio/ogg"},
    {"wma", "audio/x-ms-wma"}
};

const QHash<QString,QString> ContentServer::m_videoExtMap {
    {"mkv", "video/x-matroska"},
    {"webm", "video/webm"},
    {"flv", "video/x-flv"},
    {"ogv", "video/ogg"},
    {"avi", "video/x-msvideo"},
    {"mov", "video/quicktime"}, {"qt", "video/quicktime"},
    {"wmv", "video/x-ms-wmv"},
    {"mp4", "video/mp4"}, {"m4v", "video/mp4"},
    {"mpg", "video/mpeg"}, {"mpeg", "video/mpeg"}, {"m2v", "video/mpeg"}
};

const QHash<QString,QString> ContentServer::m_playlistExtMap {
    {"m3u", "audio/x-mpegurl"},
    {"pls", "audio/x-scpls"},
    {"xspf", "application/xspf+xml"}
};

const QStringList ContentServer::m_m3u_mimes {
    "application/vnd.apple.mpegurl",
    "application/mpegurl",
    "application/x-mpegurl",
    "audio/mpegurl",
    "audio/x-mpegurl"
};

const QStringList ContentServer::m_pls_mimes {
    "audio/x-scpls"
};

const QStringList ContentServer::m_xspf_mimes {
    "application/xspf+xml"
};

const QString ContentServer::audioItemClass = "object.item.audioItem.musicTrack";
const QString ContentServer::videoItemClass = "object.item.videoItem.movie";
const QString ContentServer::imageItemClass = "object.item.imageItem.photo";
const QString ContentServer::playlistItemClass = "object.item.playlistItem";
const QString ContentServer::broadcastItemClass = "object.item.audioItem.audioBroadcast";
const QString ContentServer::defaultItemClass = "object.item";

const QString ContentServer::artCookie = "jupii_art";

const QByteArray ContentServer::userAgent = QString("%1 %2")
        .arg(Jupii::APP_NAME, Jupii::APP_VERSION).toLatin1();

/* DLNA.ORG_OP flags:
 * 00 - no seeking allowed
 * 01 - seek by byte
 * 10 - seek by time
 * 11 - seek by both*/
const QString ContentServer::dlnaOrgOpFlagsSeekBytes = "DLNA.ORG_OP=01";
const QString ContentServer::dlnaOrgOpFlagsNoSeek = "DLNA.ORG_OP=00";
const QString ContentServer::dlnaOrgCiFlags = "DLNA.ORG_CI=0";

ContentServerWorker* ContentServerWorker::instance(QObject *parent)
{
    if (ContentServerWorker::m_instance == nullptr) {
        ContentServerWorker::m_instance = new ContentServerWorker(parent);
    }

    return ContentServerWorker::m_instance;
}

ContentServerWorker::ContentServerWorker(QObject *parent) :
    QObject(parent),
    server(new QHttpServer(parent)),
    nam(new QNetworkAccessManager(parent))
{
    QObject::connect(server, &QHttpServer::newRequest,
                     this, &ContentServerWorker::requestHandler);

    if (!server->listen(static_cast<quint16>(Settings::instance()->getPort()))) {
        qWarning() << "Unable to start HTTP server!";
        //TODO: Handle: Unable to start HTTP server
    }
}

void ContentServerWorker::requestHandler(QHttpRequest *req, QHttpResponse *resp)
{
    qDebug() << ">>> requestHandler thread:" << QThread::currentThreadId();
    qDebug() << "  method:" << req->methodString();
    qDebug() << "  URL:" << req->url().path();
    qDebug() << "  headers:" << req->url().path();

    const auto& headers = req->headers();
    for (const auto& h : headers.keys()) {
        qDebug() << "    " << h << ":" << headers.value(h);
    }

    if (req->method() != QHttpRequest::HTTP_GET &&
        req->method() != QHttpRequest::HTTP_HEAD) {
        qWarning() << "Request method is unsupported";
        resp->setHeader("Allow", "HEAD, GET");
        sendEmptyResponse(resp, 405);
        return;
    }

    bool valid, isFile, isArt;
    auto id = ContentServer::idUrlFromUrl(req->url(), &valid, &isFile, &isArt);

    if (!valid) {
        qWarning() << "Unknown content requested!";
        sendEmptyResponse(resp, 404);
        return;
    }

    auto cs = ContentServer::instance();

    const ContentServer::ItemMeta *meta;

    if (isArt) {
        // Album Cover Art
        qWarning() << "Requested content is album cover!";
        meta = cs->makeMetaUsingExtension(id);
        requestForFileHandler(id, meta, req, resp);
        delete meta;
        return;
    } else {
        meta = cs->getMetaForId(id);
        if (!meta) {
            qWarning() << "No meta item found";
            sendEmptyResponse(resp, 404);
            return;
        }
    }

    if (isFile) {
        requestForFileHandler(id, meta, req, resp);
    } else {
        bool isMic = Utils::isUrlMic(id);
        bool isPulse = Utils::isUrlPulse(id);
        if (isMic) {
            requestForMicHandler(id, meta, req, resp);
        } else if (isPulse) {
            requestForPulseHandler(id, meta, req, resp);
        } else {
            requestForUrlHandler(id, meta, req, resp);
        }
    }
}

void ContentServerWorker::stopMic()
{
    if (micDev) {
        qDebug() << "Stopping mic";
        micDev->setActive(false);

        if (micItems.isEmpty()) {
            micDev->close();

            auto t = new QTimer(this);
            t->setSingleShot(true);
            connect(t, &QTimer::timeout, [this, t]{
                if (micItems.isEmpty() && micInput)
                    micInput.reset(nullptr);
                t->deleteLater();
            });

            t->start(100);
        }
    }
}

#ifdef PULSE
void ContentServerWorker::responseForPulseDone()
{
    qDebug() << "Pulse HTTP response done";
    auto resp = sender();
    for (int i = 0; i < pulseItems.size(); ++i) {
        if (resp == pulseItems[i].resp) {
            qDebug() << "Removing finished pulse item";
            auto id = pulseItems.at(i).id;
            pulseItems.removeAt(i);
            emit itemRemoved(id);
            break;
        }
    }
}

void ContentServerWorker::startPulse()
{
    if (PulseDevice::isInited()) {
        qDebug() << "Starting pulse device";
        if (!pulseDev)
            pulseDev = std::unique_ptr<PulseDevice>(new PulseDevice(this));
        PulseDevice::startTimer();
        PulseDevice::discoverStream();
    } else {
        qWarning() << "Pulse-audio is not inited";
    }
}

void ContentServerWorker::stopPulse()
{
    if (PulseDevice::isInited()) {
        qDebug() << "Stopping pulse device";
        if (pulseDev) {
            // TODO: fix "PulseDevice::m_instance"
            PulseDevice::stopRecordStream();
            PulseDevice::stopTimer();
            pulseDev.reset(nullptr);
        } else {
            qDebug() << "Pulse device doesn't exist";
        }
    } else {
        qWarning() << "Pulse-audio is not inited";
    }
}
#endif

void ContentServerWorker::startMic()
{
    qDebug() << "Starting mic";

    QAudioFormat format;
    format.setSampleRate(ContentServer::micSampleRate);
    format.setChannelCount(ContentServer::micChannelCount);
    format.setSampleSize(ContentServer::micSampleSize);
    format.setCodec("audio/pcm");
    //format.setByteOrder(QAudioFormat::LittleEndian);
    format.setByteOrder(QAudioFormat::BigEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    auto dev = QAudioDeviceInfo::defaultInputDevice();

    /*auto devName = QAudioDeviceInfo::defaultOutputDevice().deviceName() + ".monitor";
    auto devName = QString("sink.deep_buffer.monitor");
    auto devs = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    for (auto& d : devs) {
        qDebug() << "input dev:" << d.deviceName();
        if (d.deviceName() == devName) {
            dev = d;
            qDebug() << "Got dev:" << dev.deviceName();
        }
    }*/

    qDebug() << "Available input devs:";
    auto idevs = QAudioDeviceInfo::availableDevices(QAudio::AudioInput);
    for (auto& d : idevs) {
        qDebug() << "  " << d.deviceName();
    }
    qDebug() << "Available output devs:";
    auto odevs = QAudioDeviceInfo::availableDevices(QAudio::AudioOutput);
    for (auto& d : odevs) {
        qDebug() << "  " << d.deviceName();
    }

    if (!dev.isFormatSupported(format)) {
        qWarning() << "Default audio format not supported, trying to use the nearest.";
        format = dev.nearestFormat(format);
        qDebug() << "Nerest format:";
        qDebug() << " codec:" << format.codec();
        qDebug() << " sampleSize:" << format.sampleSize();
        qDebug() << " sampleRate:" << format.sampleRate();
        qDebug() << " channelCount:" << format.channelCount();
    }

    if (!micDev)
        micDev = std::unique_ptr<MicDevice>(new MicDevice(this));
    micInput = std::unique_ptr<QAudioInput>(new QAudioInput(dev, format, this));
    micDev->setActive(true);
    micInput->start(micDev.get());
}

void ContentServerWorker::requestForFileHandler(const QUrl &id,
                                                const ContentServer::ItemMeta* meta,
                                                QHttpRequest *req, QHttpResponse *resp)
{
    auto type = static_cast<ContentServer::Type>(Utils::typeFromId(id));

    if (meta->type == ContentServer::TypeVideo &&
        type == ContentServer::TypeMusic) {
#ifdef FFMPEG
        qDebug() << "Video content and type is audio => extracting audio stream";

        ContentServer::AvData data;
        if (!ContentServer::extractAudio(meta->path, data)) {
            qWarning() << "Unable to extract audio stream";
            sendEmptyResponse(resp, 404);
            return;
        }

        streamFile(data.path, data.mime, req, resp);
#else
        qWarning() << "Video content and type is audio => can't extract audio "
                      "because ffmpeg is disabled";
#endif
    } else {
        streamFile(meta->path, meta->mime, req, resp);
    }

    qDebug() << "requestForFileHandler done";
}

void ContentServerWorker::requestForUrlHandler(const QUrl &id,
                                                const ContentServer::ItemMeta *meta,
                                                QHttpRequest *req, QHttpResponse *resp)
{
    auto url = Utils::urlFromId(id);

    if (Settings::instance()->getRemoteContentMode() == 1) {
        // Redirection mode
        qDebug() << "Redirection mode enabled => sending HTTP redirection";
        sendRedirection(resp, url.toString());
    } else {
        // Proxy mode
        qDebug() << "Proxy mode enabled => creating proxy";
        QNetworkRequest request;
        request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);
        request.setUrl(url);

        // Add headers
        const auto& headers = req->headers();
        if (headers.contains("range"))
            request.setRawHeader("Range", headers.value("range").toLatin1());
        request.setRawHeader("Icy-MetaData", "1");
        request.setRawHeader("Connection", "close");
        request.setRawHeader("User-Agent", ContentServer::userAgent);

        QNetworkReply *reply;
        bool isHead = req->method() == QHttpRequest::HTTP_HEAD;
        if (isHead) {
            qDebug() << "HEAD request for url:" << url;
            reply = nam->head(request);
        } else {
            qDebug() << "GET request for url:" << url;
            reply = nam->get(request);
        }

        ProxyItem &item = proxyItems[reply];
        item.req = req;
        item.resp = resp;
        item.reply = reply;
        item.id = id;
        item.meta = headers.contains("icy-metadata");
        item.seek = meta->seekSupported;

        responseToReplyMap.insert(resp, reply);

        connect(reply, &QNetworkReply::metaDataChanged,
                this, &ContentServerWorker::proxyMetaDataChanged);
        connect(reply, &QNetworkReply::redirected,
                this, &ContentServerWorker::proxyRedirected);
        connect(reply, &QNetworkReply::finished,
                this, &ContentServerWorker::proxyFinished);
        connect(reply, &QNetworkReply::readyRead,
                this, &ContentServerWorker::proxyReadyRead);
        connect(resp, &QHttpResponse::done,
                this, &ContentServerWorker::responseForUrlDone);

        emit itemAdded(item.id);
    }
}

void ContentServerWorker::requestForMicHandler(const QUrl &id,
                                                const ContentServer::ItemMeta *meta,
                                                QHttpRequest *req, QHttpResponse *resp)
{
    bool isHead = req->method() == QHttpRequest::HTTP_HEAD;

    resp->setHeader("Content-Type", meta->mime);
    resp->setHeader("Connection", "close");
    resp->setHeader("transferMode.dlna.org", "Streaming");
    resp->setHeader("contentFeatures.dlna.org",
                    ContentServer::dlnaContentFeaturesHeader(meta->mime));

    if (isHead) {
        qDebug() << "Sending 200 response without content";
        sendResponse(resp, 200, "");
    } else {
        qDebug() << "Sending 200 response and starting streaming";
        resp->writeHead(200);

        if (!micDev || !micDev->isOpen()) {
            startMic();
        }

        SimpleProxyItem item;
        item.id = id;
        item.req = req;
        item.resp = resp;
        micItems.append(item);

        connect(resp, &QHttpResponse::done, this, &ContentServerWorker::responseForMicDone);
    }
}

void ContentServerWorker::requestForPulseHandler(const QUrl &id,
                                                const ContentServer::ItemMeta *meta,
                                                QHttpRequest *req, QHttpResponse *resp)
{
    qDebug() << "Pulse request handler";
#ifdef PULSE
    bool isHead = req->method() == QHttpRequest::HTTP_HEAD;

    resp->setHeader("Content-Type", meta->mime);
    resp->setHeader("Connection", "close");
    resp->setHeader("transferMode.dlna.org", "Streaming");
    resp->setHeader("contentFeatures.dlna.org",
                    ContentServer::dlnaContentFeaturesHeader(meta->mime));

    if (isHead) {
        qDebug() << "Sending 200 response without content";
        sendResponse(resp, 200, "");
    } else {
        qDebug() << "Sending 200 response and starting streaming";
        resp->writeHead(200);

        SimpleProxyItem item;
        item.id = id;
        item.req = req;
        item.resp = resp;
        pulseItems.append(item);
        emit itemAdded(item.id);
        connect(resp, &QHttpResponse::done, this, &ContentServerWorker::responseForPulseDone);
        startPulse();
    }
#else
    Q_UNUSED(meta)
    Q_UNUSED(req)
    qWarning() << "Pulse URL requested but pulse-audio is disabled";
    sendEmptyResponse(resp, 404);
#endif
}


bool ContentServerWorker::seqWriteData(QFile& file, qint64 size, QHttpResponse *resp)
{
    qint64 rlen = size;

    qDebug() << "Start of writting" << rlen << "of data";

    do {
        if (resp->isFinished()) {
            qWarning() << "Connection closed by server";
            return false;
        }

        const qint64 len = rlen < ContentServer::qlen ? rlen : ContentServer::qlen;
        QByteArray data; data.resize(static_cast<int>(len));
        auto cdata = data.data();
        const int count = static_cast<int>(file.read(cdata, len));
        rlen = rlen - len;

        if (count > 0) {
            resp->write(data);
            //QThread::currentThread()->msleep(ContentServer::threadWait);
        } else {
            break;
        }
    } while (rlen > 0);

    qDebug() << "End of writting all data";

    return true;
}

void ContentServerWorker::sendEmptyResponse(QHttpResponse *resp, int code)
{
    resp->setHeader("Content-Length", "0");
    resp->writeHead(code);
    resp->end();
}

void ContentServerWorker::sendResponse(QHttpResponse *resp, int code, const QByteArray &data)
{
    resp->writeHead(code);
    resp->end(data);
}

void ContentServerWorker::sendRedirection(QHttpResponse *resp, const QString &location)
{
    resp->setHeader("Location", location);
    resp->setHeader("Content-Length", "0");
    resp->setHeader("Connection", "close");
    resp->writeHead(302);
    resp->end();
}

void ContentServerWorker::responseForMicDone()
{
    qDebug() << "Mic HTTP response done";
    auto resp = sender();
    for (int i = 0; i < micItems.size(); ++i) {
        if (resp == micItems[i].resp) {
            qDebug() << "Removing finished mic item";
            micItems.removeAt(i);
            break;
        }
    }
}

void ContentServerWorker::responseForUrlDone()
{
    qDebug() << "Response done";
    auto resp = dynamic_cast<QHttpResponse*>(sender());
    if (responseToReplyMap.contains(resp)) {
        auto reply = responseToReplyMap.value(resp);
        if (reply->isFinished()) {
            qDebug() << "Reply already finished";
        } else {
            qDebug() << "Aborting reply";
            reply->abort();
        }
    } else {
        qWarning() << "Unknown response done";
    }
}

void ContentServerWorker::proxyMetaDataChanged()
{
    qDebug() << "Request meta data received";
    auto reply = dynamic_cast<QNetworkReply*>(sender());

    if (!proxyItems.contains(reply)) {
        qWarning() << "Proxy meta data: Cannot find proxy item";
        return;
    }

    auto &item = proxyItems[reply];

    auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    auto mime = reply->header(QNetworkRequest::ContentTypeHeader).toString();
    auto error = reply->error();

    // -- debug --
    qDebug() << "Request:" << (item.req->method() == QHttpRequest::HTTP_GET ? "GET" : "HEAD")
             << item.id;
    qDebug() << "Reply status:" << code << reason;
    qDebug() << "Error code:" << error;
    qDebug() << "Headers:";
    for (const auto &p : reply->rawHeaderPairs()) {
        qDebug() << p.first << p.second;
    }
    // -- debug --

    if (error != QNetworkReply::NoError || code > 299) {
        qWarning() << "Error response from network server";
        if (code < 400)
            code = 404;
        qDebug() << "Ending request with code:" << code;
        sendEmptyResponse(item.resp, code);
    } else if (mime.isEmpty()) {
        qWarning() << "No content type header receive from network server";
        qDebug() << "Ending request with code:" << 404;
        sendEmptyResponse(item.resp, 404);
    } else if (item.state == 0) {
        item.resp->setHeader("transferMode.dlna.org", "Streaming");
        item.resp->setHeader("contentFeatures.dlna.org",
                             ContentServer::dlnaContentFeaturesHeader(mime, item.seek));
        item.resp->setHeader("Content-Type", mime);
        item.resp->setHeader("Connection", "close");
        if (reply->header(QNetworkRequest::ContentLengthHeader).isValid())
            item.resp->setHeader("Content-Length",
                                 reply->header(QNetworkRequest::ContentLengthHeader).toString());
        if (reply->hasRawHeader("Accept-Ranges"))
            item.resp->setHeader("Accept-Ranges", reply->rawHeader("Accept-Ranges"));
        if (reply->hasRawHeader("Content-Range"))
            item.resp->setHeader("Content-Range", reply->rawHeader("Content-Range"));

        if (reply->hasRawHeader("icy-metaint")) {
            item.metaint = reply->rawHeader("icy-metaint").toInt();
            qDebug() << "Shoutcast stream has metadata. Interval is"
                     << item.metaint;
        }

        // copying icy-* headers
        const auto &headers = reply->rawHeaderPairs();
        for (const auto& h : headers) {
            if (h.first.toLower().startsWith("icy-"))
                item.resp->setHeader(h.first, h.second);
        }

        item.state = 1;

        qDebug() << "Sending head for request with code:" << code;
        item.resp->writeHead(code);
        return;
    }

    emit itemRemoved(item.id);
    proxyItems.remove(reply);
    responseToReplyMap.remove(item.resp);
    reply->abort();
}

void ContentServerWorker::proxyRedirected(const QUrl &url)
{
    qDebug() << "Request redirected to:" << url;
}

void ContentServerWorker::proxyFinished()
{
    qDebug() << "Request finished";
    auto reply = dynamic_cast<QNetworkReply*>(sender());

    if (!proxyItems.contains(reply)) {
        //qWarning() << "Cannot find proxy item";
        reply->deleteLater();
        return;
    }

    auto &item = proxyItems[reply];

    auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    auto error = reply->error();
    qDebug() << "Request:" << (item.req->method() == QHttpRequest::HTTP_GET ? "GET" : "HEAD")
             << item.id;
    qDebug() << "Error code:" << error;

    if (item.state == 0) {
        if (code < 200)
            code = 404;
        qDebug() << "Ending request with code:" << code;
        sendEmptyResponse(item.resp, code);
    } else {
        qDebug() << "Ending request";
        // TODO: Do not end if resp doesn't exists!
        item.resp->end();
    }

    emit itemRemoved(item.id);
    proxyItems.remove(reply);
    responseToReplyMap.remove(item.resp);
    reply->deleteLater();
}

void ContentServerWorker::processShoutcastMetadata(QByteArray &data,
                                                   ProxyItem &item)
{
    auto count = data.length();
    int bytes = item.metacounter + count;

    /*qDebug() << "========== processShoutcastMetadata ==========";
    qDebug() << "metacounter:" << item.metacounter;
    qDebug() << "metaint:" << item.metaint;
    qDebug() << "count:" << count;
    qDebug() << "bytes:" << bytes;
    qDebug() << "data size:" << data.size();*/

    if (bytes > item.metaint) {
        Q_ASSERT(item.metaint >= item.metacounter);

        int nmeta = bytes / item.metaint;
        int totalsize = 0;
        QList<QPair<int,int>> rpoints; // (start,size) to remove from data

        for (int i = 0; i < nmeta; ++i) {
            int offset = i * item.metaint + totalsize + i;
            int start = item.metaint - item.metacounter;
            int size = 16 * static_cast<uchar>(data.at(start + offset));
            int maxsize = count - (start + offset);

            /*qDebug() << "------- Soutcast metadata detected ---------";
            qDebug() << "totalsize:" << totalsize;
            qDebug() << "offset:" << offset;
            qDebug() << "start:" << start;
            qDebug() << "start+offset:" << start + offset;
            qDebug() << "metadata size:" << size;
            qDebug() << "metadata max size:" << maxsize;
            qDebug() << "data size:" << data.size();
            qDebug() << "metacounter:" << item.metacounter;
            qDebug() << "metaint:" << item.metaint;*/

            if (size > maxsize) {
                // partial metadata received
                qDebug() << "Partial metadata received";
                auto metadata = data.mid(start + offset, maxsize);
                data.remove(start + offset, maxsize);
                item.data = metadata;
                item.metacounter = bytes - metadata.size();
                return;
            } else {
                // full metadata received
                if (size > 0) {
                    auto metadata = data.mid(start + offset + 1, size);
                    emit shoutcastMetadataUpdated(item.id, metadata);
                    totalsize += size;
                }

                if (!item.meta) {
                    // Shoutcast meta data wasn't requested by client, so marking to remove
                    rpoints.append({start + offset, size +1});
                }
            }
        }

        item.metacounter = bytes - nmeta * (item.metaint + 1) - totalsize;

        if (!item.meta && !rpoints.isEmpty()) {
            // Removing metadata from stream
            int offset = 0;
            for (auto& p : rpoints) {
                data.remove(offset + p.first, p.second);
                offset = p.second;
            }
        }

    } else {
        item.metacounter = bytes;
    }
}

void ContentServerWorker::updatePulseStreamName(const QString &name)
{
    for (const auto& item : pulseItems) {
        qDebug() << "pulseStreamUpdated:" << item.id << name;
        emit pulseStreamUpdated(item.id, name);
    }
}

void ContentServerWorker::proxyReadyRead()
{
    auto reply = dynamic_cast<QNetworkReply*>(sender());

    if (!proxyItems.contains(reply)) {
        qWarning() << "Proxy ready read: Cannot find proxy item";
        return;
    }

    if (reply->isFinished()) {
        qWarning() << "Proxy ready read: Reply is finished";
    }

    auto &item = proxyItems[reply];

    if (item.resp->isFinished()) {
        qWarning() << "Server request already finished, so ending client side";
        emit itemRemoved(item.id);
        proxyItems.remove(reply);
        responseToReplyMap.remove(item.resp);
        reply->abort();
        reply->deleteLater();
        return;
    }

    if (item.state == 1) {
        if (!item.resp->isHeaderWritten()) {
            qWarning() << "Head not written but state=1 => this should not happen";
            emit itemRemoved(item.id);
            proxyItems.remove(reply);
            responseToReplyMap.remove(item.resp);
            reply->abort();
            reply->deleteLater();
            item.resp->end();
            return;
        }

        auto data = reply->readAll();
        if (!item.data.isEmpty()) {
            // adding cached data from previous packet
            data.prepend(item.data);
            item.data.clear();
        }

        if (data.length() > 0) {
            if (item.metaint > 0)
                processShoutcastMetadata(data, item);

            item.resp->write(data);
        }
    }
}

void ContentServerWorker::streamFile(const QString& path, const QString& mime,
                           QHttpRequest *req, QHttpResponse *resp)
{
    QFile file(path);

    if (!file.open(QFile::ReadOnly)) {
        qWarning() << "Unable to open file" << file.fileName() << "to read!";
        sendEmptyResponse(resp, 500);
        return;
    }

    const auto& headers = req->headers();
    qint64 length = file.bytesAvailable();
    bool isRange = headers.contains("range");
    bool isHead = req->method() == QHttpRequest::HTTP_HEAD;

    qDebug() << "Content file name:" << file.fileName();
    qDebug() << "Content size:" << length;
    qDebug() << "Content type:" << mime;
    qDebug() << "Content request contains Range header:" << isRange;
    qDebug() << "Content request is HEAD:" << isHead;

    resp->setHeader("Content-Type", mime);
    resp->setHeader("Accept-Ranges", "bytes");
    resp->setHeader("Connection", "close");
    resp->setHeader("transferMode.dlna.org", "Streaming");
    resp->setHeader("contentFeatures.dlna.org", ContentServer::dlnaContentFeaturesHeader(mime));

    if (isRange) {
        QRegExp rx("bytes[\\s]*=[\\s]*([\\d]+)-([\\d]*)");
        if (rx.indexIn(headers.value("range")) >= 0) {
            qint64 startByte = rx.cap(1).toInt();
            qint64 endByte = (rx.cap(3) == "" ? length-1 : rx.cap(3).toInt());
            qint64 rangeLength = endByte-startByte+1;

            /*qDebug() << "Range start:" << startByte;
            qDebug() << "Range end:" << endByte;
            qDebug() << "Range length:" << rangeLength;*/

            if (endByte > length-1) {
                qWarning() << "Range end byte is higher than content lenght";
                sendEmptyResponse(resp, 416);
                file.close();
                return;
            }

            resp->setHeader("Content-Length", QString::number(rangeLength));
            resp->setHeader("Content-Range", "bytes " +
                            QString::number(startByte) + "-" +
                            QString::number(endByte) + "/" +
                            QString::number(length-1));

            qDebug() << "Sending 206 response";
            if (isHead) {
                sendResponse(resp, 206, "");
                file.close();
                return;
            }
            resp->writeHead(206);

            // Sending data
            file.seek(startByte);
            if (!seqWriteData(file, rangeLength, resp)) {
                file.close();
                return;
            }

            resp->end();
            file.close();
            return;
        }

        qWarning() << "Unable to read Range header - regexp doesn't match.";
        sendEmptyResponse(resp, 416);
        file.close();
        return;
    }

    qDebug() << "Reqest doesn't contain Range header";

    resp->setHeader("Content-Length", QString::number(length));

    if (isHead) {
        qDebug() << "Sending 200 response without content";
        sendResponse(resp, 200, "");
        file.close();
        return;
    }

    qDebug() << "Sending 200 response";

    resp->writeHead(200);

    if (!seqWriteData(file, length, resp)) {
        file.close();
        return;
    }

    resp->end();
    file.close();
}

ContentServer::ContentServer(QObject *parent) :
    QThread(parent)
{
    qDebug() << "Creating Content Server in thread:" << QThread::currentThreadId();
#ifdef FFMPEG
    // Libav stuff
    av_log_set_level(AV_LOG_DEBUG);
    av_register_all();
    avcodec_register_all();
#endif

    // starting worker
    start(QThread::NormalPriority);
}

ContentServer* ContentServer::instance(QObject *parent)
{
    if (ContentServer::m_instance == nullptr) {
        ContentServer::m_instance = new ContentServer(parent);
    }

    return ContentServer::m_instance;
}

QString ContentServer::dlnaOrgFlagsForFile()
{
    char flags[448];
    sprintf(flags, "%s=%.8x%.24x", "DLNA.ORG_FLAGS",
            DLNA_ORG_FLAG_BYTE_BASED_SEEK |
            DLNA_ORG_FLAG_INTERACTIVE_TRANSFERT_MODE |
            DLNA_ORG_FLAG_BACKGROUND_TRANSFER_MODE, 0);
    QString f(flags);
    qDebug() << f;
    return f;
}

QString ContentServer::dlnaOrgFlagsForStreaming()
{
    char flags[448];
    sprintf(flags, "%s=%.8x%.24x", "DLNA.ORG_FLAGS",
            DLNA_ORG_FLAG_S0_INCREASE |
            DLNA_ORG_FLAG_SN_INCREASE |
            DLNA_ORG_FLAG_CONNECTION_STALL |
            DLNA_ORG_FLAG_STREAMING_TRANSFER_MODE, 0);
    QString f(flags);
    qDebug() << f;
    return f;
}

QString ContentServer::dlnaOrgPnFlags(const QString &mime)
{
    if (mime.contains("video/x-msvideo", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=AVI";
    /*if (mime.contains(image/jpeg"))
        return "DLNA.ORG_PN=JPEG_LRG";*/
    if (mime.contains("audio/aac", Qt::CaseInsensitive) ||
            mime.contains("audio/aacp", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=AAC";
    if (mime.contains("audio/mpeg", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=MP3";
    if (mime.contains("audio/vnd.wav", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=LPCM";
    if (mime.contains("audio/L16", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=LPCM";
    if (mime.contains("video/x-matroska", Qt::CaseInsensitive))
        return "DLNA.ORG_PN=MKV";
    return QString();
}

QString ContentServer::dlnaContentFeaturesHeader(const QString& mime, bool seek, bool flags)
{
    QString pnFlags = dlnaOrgPnFlags(mime);
    if (pnFlags.isEmpty()) {
        if (flags)
            return QString("%1;%2;%3").arg(
                        seek ? dlnaOrgOpFlagsSeekBytes : dlnaOrgOpFlagsNoSeek,
                        dlnaOrgCiFlags,
                        seek ? dlnaOrgFlagsForFile() : dlnaOrgFlagsForStreaming());
        else
            return QString("%1;%2").arg(seek ?
                                            dlnaOrgOpFlagsSeekBytes : dlnaOrgOpFlagsNoSeek,
                                            dlnaOrgCiFlags);
    } else {
        if (flags)
            return QString("%1;%2;%3;%4").arg(
                        pnFlags, seek ? dlnaOrgOpFlagsSeekBytes : dlnaOrgOpFlagsNoSeek,
                        dlnaOrgCiFlags,
                        seek ? dlnaOrgFlagsForFile() : dlnaOrgFlagsForStreaming());
        else
            return QString("%1;%2;%3").arg(
                        pnFlags, seek ? dlnaOrgOpFlagsSeekBytes : dlnaOrgOpFlagsNoSeek,
                        dlnaOrgCiFlags);
    }
}

ContentServer::Type ContentServer::getContentTypeByExtension(const QString &path)
{
    auto ext = path.split(".").last();
    ext = ext.toLower();

    if (m_imgExtMap.contains(ext)) {
        return ContentServer::TypeImage;
    } else if (m_musicExtMap.contains(ext)) {
        return ContentServer::TypeMusic;
    } else if (m_videoExtMap.contains(ext)) {
        return ContentServer::TypeVideo;
    } else if (m_playlistExtMap.contains(ext)) {
        return ContentServer::TypePlaylist;
    }

    // Default type
    return ContentServer::TypeUnknown;
}

ContentServer::Type ContentServer::getContentTypeByExtension(const QUrl &url)
{
    return getContentTypeByExtension(url.fileName());
}

ContentServer::Type ContentServer::getContentType(const QString &path)
{
    return getContentType(QUrl::fromLocalFile(path));
}

ContentServer::Type ContentServer::getContentType(const QUrl &url)
{
    const auto meta = getMeta(url);
    if (!meta) {
        qWarning() << "No cache item found, so guessing based on file extension";
        return getContentTypeByExtension(url);
    }

    return typeFromMime(meta->mime);
}

ContentServer::PlaylistType ContentServer::playlistTypeFromMime(const QString &mime)
{
    if (m_pls_mimes.contains(mime))
        return PlaylistPLS;
    if (m_m3u_mimes.contains(mime))
        return PlaylistM3U;
    if (m_xspf_mimes.contains(mime))
        return PlaylistXSPF;
    return PlaylistUnknown;
}

ContentServer::PlaylistType ContentServer::playlistTypeFromExtension(const QString &path)
{
    auto ext = path.split(".").last();
    ext = ext.toLower();
    return playlistTypeFromMime(m_playlistExtMap.value(ext));
}

ContentServer::Type ContentServer::typeFromMime(const QString &mime)
{
    // check for playlist first
    if (m_pls_mimes.contains(mime) ||
        m_xspf_mimes.contains(mime) ||
        m_m3u_mimes.contains(mime))
        return ContentServer::TypePlaylist;

    // hack for application/ogg
    if (mime.contains("/ogg", Qt::CaseInsensitive))
        return ContentServer::TypeMusic;
    if (mime.contains("/ogv", Qt::CaseInsensitive))
        return ContentServer::TypeVideo;

    auto name = mime.split("/").first().toLower();
    if (name == "audio")
        return ContentServer::TypeMusic;
    if (name == "video")
        return ContentServer::TypeVideo;
    if (name == "image")
        return ContentServer::TypeImage;

    // Default type
    return ContentServer::TypeUnknown;
}

QStringList ContentServer::getExtensions(int type) const
{
    QStringList exts;

    if (type & TypeImage)
        exts << m_imgExtMap.keys();
    if (type & TypeMusic)
        exts << m_musicExtMap.keys();
    if (type & TypeVideo)
        exts << m_videoExtMap.keys();
    if (type & TypePlaylist)
        exts << m_playlistExtMap.keys();

    for (auto& ext : exts) {
        ext.prepend("*.");
    }

    return exts;
}

QString ContentServer::getContentMimeByExtension(const QString &path)
{
    auto ext = path.split(".").last();
    ext = ext.toLower();

    if (m_imgExtMap.contains(ext)) {
        return m_imgExtMap.value(ext);
    } else if (m_musicExtMap.contains(ext)) {
        return m_musicExtMap.value(ext);
    } else if (m_videoExtMap.contains(ext)) {
        return m_videoExtMap.value(ext);
    } else if (m_playlistExtMap.contains(ext)) {
        return m_playlistExtMap.value(ext);
    }

    // Default mime
    return "application/octet-stream";
}

QString ContentServer::getContentMimeByExtension(const QUrl &url)
{
    return getContentMimeByExtension(url.path());
}

QString ContentServer::getContentMime(const QString &path)
{
    return getContentMime(QUrl::fromLocalFile(path));
}

QString ContentServer::getContentMime(const QUrl &url)
{
    const auto meta = getMeta(url);
    if (!meta) {
        qWarning() << "No cache item found, so guessing based on file extension";
        return getContentMimeByExtension(url);
    }

    return meta->mime;
}

void ContentServer::fillCoverArt(ItemMeta& item)
{
    item.albumArt = QString("%1/art-%2.jpg")
            .arg(Settings::instance()->getCacheDir(),
                 Utils::instance()->hash(item.path));

    if (QFileInfo::exists(item.albumArt)) {
        qDebug() << "Cover Art exists";
        return; // OK
    }

    // TODO: Support for extracting image from other than mp3 file formats
    TagLib::MPEG::File af(item.path.toUtf8().constData());

    if (af.isOpen()) {
        TagLib::ID3v2::Tag *tag = af.ID3v2Tag(true);
        TagLib::ID3v2::FrameList fl = tag->frameList("APIC");

        if(!fl.isEmpty()) {
            TagLib::ID3v2::AttachedPictureFrame *frame =
                    static_cast<TagLib::ID3v2::AttachedPictureFrame*>(fl.front());

            QImage img;
            img.loadFromData(reinterpret_cast<const uchar*>(frame->picture().data()),
                             static_cast<int>(frame->picture().size()));
            if (img.save(item.albumArt))
                return; // OK
            else
                qWarning() << "Unable to write album art image:" << item.albumArt;
        } else {
            qDebug() << "No cover art in" << item.path;
        }
    } else {
        qWarning() << "Cannot open file" << item.path << "with TagLib";
    }

    item.albumArt.clear();
}

bool ContentServer::getContentMeta(const QString &id, const QUrl &url, QString &meta)
{
    QString path, name, desc, author; int t = 0; QUrl icon;
    if (!Utils::pathTypeNameCookieIconFromId(id, &path, &t, &name, nullptr,
                                             &icon, &desc, &author))
        return false;

    bool audioType = static_cast<Type>(t) == TypeMusic; // extract audio stream from video
    QUrl urlFromId = Utils::urlFromId(id);

    const auto item = getMeta(urlFromId);
    if (!item) {
        qWarning() << "No meta item found";
        return false;
    }

    AvData data;
    if (audioType && item->local) {
#ifdef FFMPEG
        if (!extractAudio(path, data)) {
            qWarning() << "Cannot extract audio stream";
            return false;
        }
        qDebug() << "Audio stream extracted to" << data.path;
#else
        qWarning() << "Audio stream cannot be extracted because ffmpeg is disabled";
        return false;
#endif
    }

    auto u = Utils::instance();
    QString hash = u->hash(id);
    QString hash_dir = u->hash(id+"/parent");
    QTextStream m(&meta);

    m << "<?xml version=\"1.0\" encoding=\"utf-8\"?>" << endl;
    m << "<DIDL-Lite xmlns=\"urn:schemas-upnp-org:metadata-1-0/DIDL-Lite/\" ";
    m << "xmlns:dc=\"http://purl.org/dc/elements/1.1/\" ";
    m << "xmlns:upnp=\"urn:schemas-upnp-org:metadata-1-0/upnp/\" ";
    m << "xmlns:dlna=\"urn:schemas-dlna-org:metadata-1-0/\">";
    m << "<item id=\"" << hash << "\" parentID=\"" << hash_dir << "\" restricted=\"true\">";

    switch (item->type) {
    case TypeImage:
        m << "<upnp:albumArtURI>" << url.toString() << "</upnp:albumArtURI>";
        m << "<upnp:class>" << imageItemClass << "</upnp:class>";
        break;
    case TypeMusic:
        m << "<upnp:class>" << audioItemClass << "</upnp:class>";
        if (!icon.isEmpty() || !item->albumArt.isEmpty()) {
            auto id = Utils::idFromUrl(icon.isEmpty() ?
                                           QUrl::fromLocalFile(item->albumArt) :
                                           icon,
                                       artCookie);
            QUrl artUrl;
            if (makeUrl(id, artUrl))
                m << "<upnp:albumArtURI>" << artUrl.toString() << "</upnp:albumArtURI>";
            else
                qWarning() << "Cannot make Url form art path";
        }
        break;
    case TypeVideo:
        if (audioType)
            m << "<upnp:class>" << audioItemClass << "</upnp:class>";
        else
            m << "<upnp:class>" << videoItemClass << "</upnp:class>";
        break;
    case TypePlaylist:
        m << "<upnp:class>" << playlistItemClass << "</upnp:class>";
        break;
    default:
        m << "<upnp:class>" << defaultItemClass << "</upnp:class>";
    }

    if (name.isEmpty()) {
        if (item->title.isEmpty())
            m << "<dc:title>" << ContentServer::bestName(*item).toHtmlEscaped() << "</dc:title>";
        else
            m << "<dc:title>" << item->title.toHtmlEscaped() << "</dc:title>";
        if (!item->artist.isEmpty())
            m << "<upnp:artist>" << item->artist.toHtmlEscaped() << "</upnp:artist>";
        if (!item->album.isEmpty())
            m << "<upnp:album>" << item->album.toHtmlEscaped() << "</upnp:album>";
    } else {
        m << "<dc:title>" << name.toHtmlEscaped() << "</dc:title>";
        if (!author.isEmpty())
            m << "<upnp:artist>" << author.toHtmlEscaped() << "</upnp:artist>";
    }

    if (desc.isEmpty()) {
        if (!item->comment.isEmpty())
            m << "<upnp:longDescription>" << item->comment.toHtmlEscaped() << "</upnp:longDescription>";
    } else {
        m << "<upnp:longDescription>" << desc << "</upnp:longDescription>";
    }

    m << "<res ";

    if (audioType) {
        // puting audio stream info instead video file
        if (data.size > 0)
            m << "size=\"" << QString::number(data.size) << "\" ";
        m << "protocolInfo=\"http-get:*:" << data.mime << ":*\" ";
    } else {
        if (item->size > 0)
            m << "size=\"" << QString::number(item->size) << "\" ";
        //m << "protocolInfo=\"http-get:*:" << item->mime << ":*\" ";
        m << "protocolInfo=\"http-get:*:" << item->mime << ":"
          << dlnaContentFeaturesHeader(item->mime, item->seekSupported, false)
          << "\" ";
    }

    if (item->duration > 0) {
        int seconds = item->duration % 60;
        int minutes = ((item->duration - seconds) / 60) % 60;
        int hours = (item->duration - (minutes * 60) - seconds) / 3600;
        QString duration = QString::number(hours) + ":" + (minutes < 10 ? "0" : "") +
                           QString::number(minutes) + ":" + (seconds < 10 ? "0" : "") +
                           QString::number(seconds) + ".000";
        m << "duration=\"" << duration << "\" ";
    }

    if (audioType) {
        if (item->bitrate > 0)
            m << "bitrate=\"" << QString::number(data.bitrate) << "\" ";
        if (item->sampleRate > 0)
            m << "sampleFrequency=\"" << QString::number(item->sampleRate) << "\" ";
        if (item->channels > 0)
            m << "nrAudioChannels=\"" << QString::number(item->channels) << "\" ";
    } else {
        if (item->bitrate > 0)
            m << "bitrate=\"" << QString::number(item->bitrate, 'f', 0) << "\" ";
        if (item->sampleRate > 0)
            m << "sampleFrequency=\"" << QString::number(item->sampleRate, 'f', 0) << "\" ";
        if (item->channels > 0)
            m << "nrAudioChannels=\"" << QString::number(item->channels) << "\" ";
    }

    m << ">" << url.toString() << "</res>";
    m << "</item>\n";
    m << "</DIDL-Lite>";

    qDebug() << "DIDL:" << meta;

    return true;
}

bool ContentServer::getContentUrl(const QString &id, QUrl &url, QString &meta,
                                  QString cUrl)
{
    if (!Utils::isIdValid(id)) {
        return false;
    }

    if (!makeUrl(id, url)) {
        qWarning() << "Cannot make Url form id";
        return false;
    }

    if (!cUrl.isEmpty() && cUrl == url.toString()) {
        // Optimization: Url is the same as current -> skipping getContentMeta
        return true;
    }

    if (!getContentMeta(id, url, meta)) {
        qWarning() << "Cannot get content meta data";
        return false;
    }

    return true;
}

QString ContentServer::bestName(const ContentServer::ItemMeta &meta)
{
    QString name;

    if (!meta.title.isEmpty())
        name = meta.title;
    else if (!meta.filename.isEmpty() && meta.filename.length() > 1)
        name = meta.filename;
    else if (!meta.url.isEmpty())
        name = meta.url.toString();
    else
        name = tr("Unknown");

    return name;
}

bool ContentServer::makeUrl(const QString& id, QUrl& url)
{
    QString hash = QString::fromUtf8(encrypt(id.toUtf8()));

    QString ifname, addr;
    if (!Utils::instance()->getNetworkIf(ifname, addr)) {
        qWarning() << "Cannot find valid network interface";
        return false;
    }

    url.setScheme("http");
    url.setHost(addr);
    url.setPort(Settings::instance()->getPort());
    url.setPath("/" + hash);

    return true;
}

QByteArray ContentServer::encrypt(const QByteArray &data)
{
    QByteArray _data(data);

    QByteArray key = Settings::instance()->getKey();
    QByteArray tmp(key);
    while (key.size() < _data.size())
        key += tmp;

    for (int i = 0; i < _data.size(); ++i)
        _data[i] = _data.at(i) ^ key.at(i);

    _data = _data.toBase64(QByteArray::Base64UrlEncoding |
                         QByteArray::OmitTrailingEquals);
    return _data;
}

QByteArray ContentServer::decrypt(const QByteArray& data)
{
    QByteArray _data = QByteArray::fromBase64(data, QByteArray::Base64UrlEncoding |
                                                   QByteArray::OmitTrailingEquals);
    QByteArray key = Settings::instance()->getKey();
    QByteArray tmp(key);
    while (key.size() < _data.size())
        key += tmp;

    for (int i = 0; i < _data.size(); ++i)
        _data[i] = _data.at(i) ^ key.at(i);

    return _data;
}

QString ContentServer::pathFromUrl(const QUrl &url) const
{
    bool valid, isFile;
    auto id = idUrlFromUrl(url, &valid, &isFile);

    if (valid && isFile)
        return id.toLocalFile();

    //qWarning() << "Cannot get path from URL:" << url.toString() << id.toString();
    return QString();
}

QUrl ContentServer::idUrlFromUrl(const QUrl &url, bool* ok, bool* isFile, bool* isArt)
{
    QString hash = url.path();
    hash = hash.right(hash.length()-1);

    auto id = QUrl(QString::fromUtf8(decrypt(hash.toUtf8())));

    if (!id.isValid()) {
        //qWarning() << "Id is invalid" << id.toString();
        if (ok)
            *ok = false;
        return QUrl();
    }

    QUrlQuery q(id);
    if (!q.hasQueryItem(Utils::cookieKey) ||
            q.queryItemValue(Utils::cookieKey).isEmpty()) {
        qWarning() << "Id has no cookie";
        if (ok)
            *ok = false;
        return QUrl();
    } else {
        if (isArt)
            *isArt = q.queryItemValue(Utils::cookieKey) == artCookie;
    }

    if (id.isLocalFile()) {
        QString path = id.toLocalFile();
        QFileInfo file(path);
        if (!file.exists() || !file.isFile()) {
            qWarning() << "Content path doesn't exist";
            if (ok)
                *ok = false;
            if (isFile)
                *isFile = true;
            return QUrl();
        }

        if (ok)
            *ok = true;
        if (isFile)
            *isFile = true;
        return id.toString();
    }

    if (ok)
        *ok = true;
    if (isFile)
        *isFile = false;
    return id;
}

QString ContentServer::idFromUrl(const QUrl &url) const
{
    bool valid, isFile;
    auto id = idUrlFromUrl(url, &valid, &isFile);

    if (valid)
        return id.toString();

    //qWarning() << "Cannot get id from URL:" << url.toString();
    return QString();
}

QString ContentServer::urlFromUrl(const QUrl &url) const
{
    bool valid;
    auto id = idUrlFromUrl(url, &valid);

    if (valid)
        return Utils::urlFromId(id).toString();

    return QString();
}

#ifdef FFMPEG
bool ContentServer::fillAvDataFromCodec(const AVCodecParameters *codec,
                                        const QString& videoPath,
                                        AvData& data) {
    switch (codec->codec_id) {
    case AV_CODEC_ID_MP2:
    case AV_CODEC_ID_MP3:
        data.mime = "audio/mpeg";
        data.type = "mp3";
        data.extension = "mp3";
        break;
    case AV_CODEC_ID_VORBIS:
        data.mime = "audio/ogg";
        data.type = "oga";
        data.extension = "oga";
        break;
    default:
        data.mime = "audio/mp4";
        data.type = "mp4";
        data.extension = "m4a";
    }

    data.path = videoPath + ".audio-extracted." + data.extension;
    data.bitrate = codec->bit_rate;
    data.channels = codec->channels;

    return true;
}

bool ContentServer::extractAudio(const QString& path,
                                 ContentServer::AvData& data)
{
    auto f = path.toUtf8();
    const char* file = f.data();

    qDebug() << "Extracting audio from file:" << file;

    AVFormatContext *ic = NULL;
    if (avformat_open_input(&ic, file, NULL, NULL) < 0) {
        qWarning() << "avformat_open_input error";
        return false;
    }

    if ((avformat_find_stream_info(ic, NULL)) < 0) {
        qWarning() << "Could not find stream info";
        avformat_close_input(&ic);
        return false;
    }

    qDebug() << "nb_streams:" << ic->nb_streams;

    int aidx = av_find_best_stream(ic, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
    qDebug() << "audio stream index is:" << aidx;

    if (aidx < 0) {
        qWarning() << "No audio stream found";
        avformat_close_input(&ic);
        return false;
    }

    // Debug: audio stream side data
    qDebug() << "audio stream nb_side_data:" << ic->streams[aidx]->nb_side_data;
    for (int i = 0; i < ic->streams[aidx]->nb_side_data; ++i) {
        qDebug() << "-- audio stream side data --";
        qDebug() << "type:" << ic->streams[aidx]->side_data[i].type;
        qDebug() << "size:" << ic->streams[aidx]->side_data[i].size;
        QByteArray data(reinterpret_cast<const char*>(ic->streams[aidx]->side_data[i].data),
                        ic->streams[aidx]->side_data[i].size);
        qDebug() << "data:" << data;
    }
    // --

    qDebug() << "Audio codec:";
    qDebug() << "codec_id:" << ic->streams[aidx]->codecpar->codec_id;
    qDebug() << "codec_channels:" << ic->streams[aidx]->codecpar->channels;
    qDebug() << "codec_tag:" << ic->streams[aidx]->codecpar->codec_tag;
    // --

    if (!fillAvDataFromCodec(ic->streams[aidx]->codecpar, path, data)) {
        qWarning() << "Unable to find correct mime for the codec:"
                   << ic->streams[aidx]->codecpar->codec_id;
        avformat_close_input(&ic);
        return false;
    }

    qDebug() << "Audio stream content type" << data.mime;
    qDebug() << "Audio stream bitrate" << data.bitrate;
    qDebug() << "Audio stream channels" << data.channels;

    qDebug() << "av_guess_format";
    AVOutputFormat *of = NULL;
    auto t = data.type.toLatin1();
    of = av_guess_format(t.data(), NULL, NULL);
    if (!of) {
        qWarning() << "av_guess_format error";
        avformat_close_input(&ic);
        return false;
    }

    qDebug() << "avformat_alloc_context";
    AVFormatContext *oc = NULL;
    oc = avformat_alloc_context();
    if (!oc) {
        qWarning() << "avformat_alloc_context error";
        avformat_close_input(&ic);
        return false;
    }

    if (ic->metadata) {
        // Debug: metadata
        AVDictionaryEntry *tag = NULL;
        while ((tag = av_dict_get(ic->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
            qDebug() << tag->key << "=" << tag->value;

        if (!av_dict_copy(&oc->metadata, ic->metadata, 0) < 0) {
            qWarning() << "oc->metadata av_dict_copy error";
            avformat_close_input(&ic);
            avformat_free_context(oc);
            return false;
        }
    } else {
        qDebug() << "No metadata found";
    }

    oc->oformat = of;

    qDebug() << "avformat_new_stream";
    AVStream* ast = NULL;
    ast = avformat_new_stream(oc, ic->streams[aidx]->codec->codec);
    if (!ast) {
        qWarning() << "avformat_new_stream error";
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return false;
    }

    ast->id = 0;

    if (ic->streams[aidx]->metadata) {
        // Debug: audio stream metadata, codec
        AVDictionaryEntry *tag = NULL;
        while ((tag = av_dict_get(ic->streams[aidx]->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)))
            qDebug() << tag->key << "=" << tag->value;

        if (!av_dict_copy(&ast->metadata, ic->streams[aidx]->metadata, 0) < 0) {
            qWarning() << "av_dict_copy error";
            avformat_close_input(&ic);
            avformat_free_context(oc);
            return false;
        }
    } else {
        qDebug() << "No metadata in audio stream";
    }

    // Copy codec params
    AVCodecParameters* t_cpara = avcodec_parameters_alloc();
    if (avcodec_parameters_from_context(t_cpara, ic->streams[aidx]->codec) < 0) {
        qWarning() << "avcodec_parameters_from_context error";
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return false;
    }
    if (avcodec_parameters_copy(ast->codecpar, t_cpara ) < 0) {
        qWarning() << "avcodec_parameters_copy error";
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return false;
    }
    if (avcodec_parameters_to_context(ast->codec, t_cpara) < 0) {
        qWarning() << "avcodec_parameters_to_context error";
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return false;
    }
    avcodec_parameters_free(&t_cpara);

    ast->codecpar->codec_tag =
            av_codec_get_tag(oc->oformat->codec_tag,
                             ic->streams[aidx]->codecpar->codec_id);

    if (oc->oformat->flags & AVFMT_GLOBALHEADER)
        ast->codec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    qDebug() << "ast->codec->sample_fmt:" << ast->codec->sample_fmt;
    qDebug() << "ast->codec->bit_rate:" << ast->codec->bit_rate;
    qDebug() << "ast->codec->sample_rate:" << ast->codec->sample_rate;
    qDebug() << "ast->codec->channels:" << ast->codec->channels;
    qDebug() << "ast->codecpar->codec_tag:" << ast->codecpar->codec_tag;
    qDebug() << "ic->streams[aidx]->codecpar->codec_id:" << ic->streams[aidx]->codecpar->codec_id;
    qDebug() << "ast->codecpar->codec_id:" << ast->codecpar->codec_id;

    qDebug() << "Extracted audio file will be:" << data.path;

    QFile audioFile(data.path);
    if (audioFile.exists()) {
        qDebug() << "Extracted audio stream exists";
        data.size = QFileInfo(data.path).size();
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return true;
    }

    qDebug() << "avio_open";
    auto bapath = data.path.toUtf8();
    if (avio_open(&oc->pb, bapath.data(), AVIO_FLAG_WRITE) < 0) {
        qWarning() << "avio_open error";
        avformat_close_input(&ic);
        avformat_free_context(oc);
        return false;
    }

    qDebug() << "avformat_write_header";
    if (avformat_write_header(oc, NULL) < 0) {
        qWarning() << "avformat_write_header error";
        avformat_close_input(&ic);
        avio_close(oc->pb);
        avformat_free_context(oc);
        audioFile.remove();
        return false;
    }

    AVPacket pkt = { 0 };
    av_init_packet(&pkt);

    while (!av_read_frame(ic, &pkt)) {
        // Only processing audio stream packets
        if (pkt.stream_index == aidx) {
            // Debug: audio stream packet side data
            for (int i = 0; i < pkt.side_data_elems; ++i) {
                qDebug() << "Audio stream packet side data:";
                qDebug() << "type:" << pkt.side_data[i].type;
                qDebug() << "size:" << pkt.side_data[i].size;
                QByteArray data(reinterpret_cast<const char*>(pkt.side_data[i].data),
                                pkt.side_data[i].size);
                qDebug() << "data:" << data;
            }

            /*qDebug() << "------ orig -----";
            qDebug() << "duration:" << pkt.duration;
            qDebug() << "dts:" << pkt.dts;
            qDebug() << "pts:" << pkt.pts;
            qDebug() << "pos:" << pkt.pos;

            qDebug() << "------ time base -----";
            qDebug() << "ast->codec->time_base:" << ast->codec->time_base.num << ast->codec->time_base.den;
            qDebug() << "ast->time_base:" << ast->time_base.num << ast->time_base.den;
            qDebug() << "ic->streams[aidx]->codec->time_base:" << ic->streams[aidx]->codec->time_base.num << ic->streams[aidx]->codec->time_base.den;
            qDebug() << "ic->streams[aidx]->time_base:" << ic->streams[aidx]->time_base.num << ic->streams[aidx]->time_base.den;*/

            av_packet_rescale_ts(&pkt, ic->streams[aidx]->time_base, ast->time_base);

            /*qDebug() << "------ after rescale -----";
            qDebug() << "duration:" << pkt.duration;
            qDebug() << "dts:" << pkt.dts;
            qDebug() << "pts:" << pkt.pts;
            qDebug() << "pos:" << pkt.pos;*/

            pkt.stream_index = ast->index;

            if (av_write_frame(oc, &pkt) != 0) {
                qWarning() << "Error while writing audio frame";
                av_packet_unref(&pkt);
                avformat_close_input(&ic);
                avio_close(oc->pb);
                avformat_free_context(oc);
                audioFile.remove();
                return false;
            }
        }

        av_packet_unref(&pkt);
    }

    qDebug() << "av_write_trailer";
    if (av_write_trailer(oc) < 0) {
        qWarning() << "av_write_trailer error";
        avformat_close_input(&ic);
        avio_close(oc->pb);
        avformat_free_context(oc);
        audioFile.remove();
        return false;
    }

    qDebug() << "avformat_close_input";
    avformat_close_input(&ic);

    qDebug() << "avio_close";
    if (avio_close(oc->pb) < 0) {
        qDebug() << "avio_close error";
    }

    qDebug() << "avformat_free_context";
    avformat_free_context(oc);

    data.size = QFileInfo(data.path).size();

    return true;
}
#endif

const ContentServer::ItemMeta* ContentServer::getMeta(const QUrl &url, bool createNew)
{
    metaCacheMutex.lock();
    auto it = getMetaCacheIterator(url, createNew);
    auto meta = it == metaCache.end() ? nullptr : &it.value();
    metaCacheMutex.unlock();

    return meta;
}

const ContentServer::ItemMeta* ContentServer::getMetaForId(const QUrl &id, bool createNew)
{
    auto url = Utils::urlFromId(id);
    return getMeta(url, createNew);
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::getMetaCacheIterator(const QUrl &url, bool createNew)
{    
    const auto i = metaCache.find(url);
    if (i == metaCache.end()) {
        qDebug() << "Meta data for" << url << "not cached";
        if (createNew)
            return makeItemMeta(url);
        else
            return metaCache.end();
    }

    qDebug() << "Meta data for" << url << "found in cache";
    return i;
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::getMetaCacheIteratorForId(const QUrl &id, bool createNew)
{
    auto url = Utils::urlFromId(id);
    return getMetaCacheIterator(url, createNew);
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::metaCacheIteratorEnd()
{
    return metaCache.end();
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeItemMetaUsingTracker(const QUrl &url)
{
    const QString fileUrl = url.toString(QUrl::EncodeUnicode|QUrl::EncodeSpaces);
    const QString path = url.toLocalFile();
    const QString query = queryTemplate.arg(fileUrl);

    auto tracker = Tracker::instance();
    if (!tracker->query(query, false)) {
        qWarning() << "Cannot get tracker data for url:" << fileUrl;
        return metaCache.end();
    }

    auto res = tracker->getResult();
    TrackerCursor cursor(res.first, res.second);

    int n = cursor.columnCount();

    if (n == 10) {
        while(cursor.next()) {
            /*for (int i = 0; i < n; ++i) {
                auto name = cursor.name(i);
                auto type = cursor.type(i);
                auto value = cursor.value(i);
                qDebug() << "column:" << i;
                qDebug() << " name:" << name;
                qDebug() << " type:" << type;
                qDebug() << " value:" << value;
            }*/

            QFileInfo file(path);

            auto& meta = metaCache[url];
            meta.valid = true;
            meta.trackerId = cursor.value(0).toString();
            meta.url = url;
            meta.mime = cursor.value(1).toString();
            meta.title = cursor.value(2).toString();
            meta.comment = cursor.value(3).toString();
            meta.duration = cursor.value(4).toInt();
            meta.album = cursor.value(5).toString();
            meta.artist = cursor.value(6).toString();
            meta.bitrate = cursor.value(7).toDouble();
            meta.channels = cursor.value(8).toInt();
            meta.sampleRate = cursor.value(9).toDouble();
            meta.path = path;
            meta.filename = file.fileName();
            meta.albumArt = tracker->genAlbumArtFile(meta.album, meta.artist);
            meta.type = typeFromMime(meta.mime);
            meta.size = file.size();
            meta.local = true;
            meta.seekSupported = true;

            // defauls
            /*if (meta.title.isEmpty())
                meta.title = file.fileName();
            if (meta.artist.isEmpty())
                meta.artist = tr("Unknown");
            if (meta.album.isEmpty())
                meta.album = tr("Unknown");*/

            return metaCache.find(url);
        }
    }

    return metaCache.end();
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeItemMetaUsingTaglib(const QUrl &url)
{
    QString path = url.toLocalFile();
    QFileInfo file(path);

    ContentServer::ItemMeta meta;
    meta.valid = true;
    meta.url = url;
    meta.path = path;
    meta.mime = getContentMimeByExtension(path);
    meta.type = getContentTypeByExtension(path);
    meta.size = file.size();
    meta.filename = file.fileName();
    meta.local = true;
    meta.seekSupported = true;

    TagLib::FileRef f(path.toUtf8().constData());
    if(f.isNull()) {
        qWarning() << "Cannot extract meta data with TagLib";
    } else {
        if(f.tag()) {
            TagLib::Tag *tag = f.tag();
            meta.title = QString::fromWCharArray(tag->title().toCWString());
            meta.artist = QString::fromWCharArray(tag->artist().toCWString());
            meta.album = QString::fromWCharArray(tag->album().toCWString());
        }

        if(f.audioProperties()) {
            TagLib::AudioProperties *properties = f.audioProperties();
            meta.duration = properties->length();
            meta.bitrate = properties->bitrate();
            meta.sampleRate = properties->sampleRate();
            meta.channels = properties->channels();
        }
    }

    if (meta.mime == "audio/mpeg")
        fillCoverArt(meta);

    // defauls
    /*if (meta.title.isEmpty())
        meta.title = file.fileName();
    if (meta.artist.isEmpty())
        meta.artist = tr("Unknown");
    if (meta.album.isEmpty())
        meta.album = tr("Unknown");*/

    return metaCache.insert(url, meta);
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeMicItemMeta(const QUrl &url)
{
    ContentServer::ItemMeta meta;
    meta.valid = true;
    meta.url = url;
    meta.channels = ContentServer::micChannelCount;
    meta.sampleRate = ContentServer::micSampleRate;
    meta.mime = QString("audio/L%1;rate=%2;channels=%3")
            .arg(ContentServer::micSampleSize)
            .arg(meta.sampleRate)
            .arg(meta.channels);
    meta.bitrate = meta.sampleRate * ContentServer::micSampleSize * meta.channels;
    meta.type = ContentServer::TypeMusic;
    meta.size = 0;
    meta.local = true;
    meta.seekSupported = false;

    meta.title = tr("Microphone");
    //meta.artist = "Jupii";

#ifdef SAILFISH
    meta.albumArt = IconProvider::pathToId("icon-l-mic-cover");
#endif

    return metaCache.insert(url, meta);
}

#ifdef PULSE
const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makePulseItemMeta(const QUrl &url)
{
    auto mode = Settings::instance()->getPulseMode();

    // modes:
    // 0 - 44100 stereo
    // 1 - 44100 mono
    // 2 - 22050 stereo
    // 3 - 22050 mono
    PulseDevice::sampleSpec = {
        PA_SAMPLE_S16BE,
        mode == 0 || mode == 1 ? 44100 : 22050,
        mode == 0 || mode == 2 ? 2 : 1
    };

    ContentServer::ItemMeta meta;
    meta.valid = true;
    meta.url = url;
    meta.channels = PulseDevice::sampleSpec.channels;
    meta.sampleRate = PulseDevice::sampleSpec.rate;
    meta.mime = QString("audio/L%1;rate=%2;channels=%3")
            .arg(ContentServer::pulseSampleSize)
            .arg(meta.sampleRate)
            .arg(meta.channels);
    meta.bitrate = meta.sampleRate * ContentServer::pulseSampleSize * meta.channels;
    meta.type = ContentServer::TypeMusic;
    meta.size = 0;
    meta.local = true;
    meta.seekSupported = false;

    meta.title = tr("Audio output");

#ifdef SAILFISH
    meta.albumArt = IconProvider::pathToId("icon-l-pulse-cover");
#endif

    return metaCache.insert(url, meta);
}
#endif

QString ContentServer::mimeFromDisposition(const QString &disposition)
{
    QString mime;

    if (disposition.contains("attachment")) {
        qDebug() << "Content as a attachment detected";
        QRegExp rx("filename=\"?([^\";]*)\"?", Qt::CaseInsensitive);
        int pos = 0;
        while ((pos = rx.indexIn(disposition, pos)) != -1) {
            QString filename = rx.cap(1);
            if (!filename.isEmpty()) {
                qDebug() << "filename:" << filename;
                mime = getContentMimeByExtension(filename);
                break;
            }
            pos += rx.matchedLength();
        }
    }

    return mime;
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeItemMetaUsingHTTPRequest(const QUrl &url,
                                            std::shared_ptr<QNetworkAccessManager> nam,
                                            int counter)
{
    qDebug() << ">> makeItemMetaUsingHTTPRequest in thread:" << QThread::currentThreadId();
    if (counter >= maxRedirections) {
        qWarning() << "Max redirections reached";
        return metaCache.end();
    }

    qDebug() << "Sending HTTP request for url:" << url;
    QNetworkRequest request;
    request.setUrl(url);
    request.setRawHeader("User-Agent", userAgent);
    request.setAttribute(QNetworkRequest::FollowRedirectsAttribute, true);

    if (!nam) {
        nam = std::shared_ptr<QNetworkAccessManager>(new QNetworkAccessManager());
        /*connect(nam.get(), &QNetworkAccessManager::sslErrors,
                [](QNetworkReply *reply, const QList<QSslError> &errors){
            Q_UNUSED(reply)
            for (auto& err : errors) {
                qWarning() << "SSL error:" << err.error() << err.errorString();
            }
        });*/
    }

    auto reply = nam->get(request);

    QEventLoop loop;
    connect(reply, &QNetworkReply::metaDataChanged, [reply]{
        qDebug() << ">> metaDataChanged in thread:" << QThread::currentThreadId();
        qDebug() << "Received meta data of HTTP reply for url:" << reply->url();

        // Bug in Qt? "Content-Disposition" cannot be retrived with QNetworkRequest::ContentDispositionHeader
        //auto disposition = reply->header(QNetworkRequest::ContentDispositionHeader).toString().toLower();
        auto disposition = QString(reply->rawHeader("Content-Disposition")).toLower();
        auto mime = mimeFromDisposition(disposition);
        if (mime.isEmpty())
            mime = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
        auto type = typeFromMime(mime);

        if (type == ContentServer::TypePlaylist) {
            qWarning() << "Content is a playlist";
            // Content is needed, so not aborting
        } else {
            // Content is no needed, so aborting
            if (!reply->isFinished())
                reply->abort();
        }
    });
    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(httpTimeout, &loop, &QEventLoop::quit); // timeout
    loop.exec(); // waiting for HTTP reply...

    if (!reply->isFinished()) {
        qWarning() << "Timeout occured";
        reply->abort();
        reply->deleteLater();
        return metaCache.end();
    }

    qDebug() << "Received HTTP reply for url:" << url;

    qDebug() << "Headers:";
    for (const auto &p : reply->rawHeaderPairs()) {
        qDebug() << p.first << p.second;
    }

    auto code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    auto reason = reply->attribute(QNetworkRequest::HttpReasonPhraseAttribute).toString();
    auto error = reply->error();
    qDebug() << "Response code:" << code << reason;

    if (error != QNetworkReply::NoError &&
        error != QNetworkReply::OperationCanceledError) {
        qWarning() << "Error:" << error;
        reply->deleteLater();
        return metaCache.end();
    }

    if (code > 299 && code < 399) {
        qWarning() << "Redirection received:" << reply->error() << code << reason;
        QUrl newUrl = reply->header(QNetworkRequest::LocationHeader).toUrl();
        if (newUrl.isRelative())
            newUrl = url.resolved(newUrl);
        reply->deleteLater();
        if (newUrl.isValid())
            return makeItemMetaUsingHTTPRequest(newUrl, nam, counter + 1);
        else
            return metaCache.end();
    }

    if (code > 299) {
        qWarning() << "Unsupported response code:" << reply->error() << code << reason;
        reply->deleteLater();
        return metaCache.end();
    }

    // Bug in Qt? "Content-Disposition" cannot be retrived with QNetworkRequest::ContentDispositionHeader
    //auto disposition = reply->header(QNetworkRequest::ContentDispositionHeader).toString().toLower();
    auto disposition = QString(reply->rawHeader("Content-Disposition")).toLower();
    auto mime = mimeFromDisposition(disposition);
    if (mime.isEmpty())
        mime = reply->header(QNetworkRequest::ContentTypeHeader).toString().toLower();
    auto type = typeFromMime(mime);

    if (type == ContentServer::TypePlaylist) {
        qDebug() << "Content is a playlist";
        auto size = reply->bytesAvailable();
        if (size > 0) {
            auto ptype = playlistTypeFromMime(mime);
            auto items = ptype == PlaylistPLS ?
                         parsePls(reply->readAll()) :
                            ptype == PlaylistXSPF ?
                            parseXspf(reply->readAll()) :
                                parseM3u(reply->readAll());

            if (!items.isEmpty()) {
                QUrl url = items.first().url;
                qDebug() << "Trying get meta data for first item in the playlist:" << url;
                reply->deleteLater();
                return makeItemMetaUsingHTTPRequest(url, nam, counter + 1);
            }
        }

        qWarning() << "Playlist content is empty";
        reply->deleteLater();
        return metaCache.end();
    }

    if (type != TypeMusic && type != TypeVideo && type != TypeImage) {
        qWarning() << "Unsupported type";
        reply->deleteLater();
        return metaCache.end();
    }

    auto ranges = QString(reply->rawHeader("Accept-Ranges")).toLower().contains("bytes");
    int size = reply->header(QNetworkRequest::ContentLengthHeader).toInt();

    const QByteArray icy_name_h = "icy-name";
    const QByteArray icy_br_h = "icy-br";
    const QByteArray icy_sr_h = "icy-sr";

    ContentServer::ItemMeta meta;
    meta.valid = true;
    meta.url = url;
    meta.mime = mime;
    meta.type = type;
    meta.size = ranges ? size : 0;
    meta.filename = url.fileName();
    meta.local = false;
    meta.seekSupported = size > 0 ? ranges : false;

    if (reply->hasRawHeader(icy_name_h))
        meta.title = QString(reply->rawHeader(icy_name_h));
    else
        meta.title = url.fileName();
    /*if (reply->hasRawHeader(icy_br_h))
        meta.bitrate = reply->rawHeader(icy_br_h).toDouble();
    if (reply->hasRawHeader(icy_sr_h))
        meta.sampleRate = reply->rawHeader(icy_sr_h).toDouble();*/

    reply->deleteLater();
    return metaCache.insert(url, meta);
}

/*const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeItemMetaUsingExtension(const QUrl &url)
{
    return metaCache.insert(url, makeItemMetaUsingExtension2(url));
}*/

ContentServer::ItemMeta*
ContentServer::makeMetaUsingExtension(const QUrl &url)
{
    bool isFile = url.isLocalFile();
    auto item = new ContentServer::ItemMeta;
    item->valid = true;
    item->path = isFile ? url.toLocalFile() : "";
    item->url = url;
    item->mime = getContentMimeByExtension(url);
    item->type = getContentTypeByExtension(url);
    item->size = 0;
    item->filename = url.fileName();
    item->local = isFile;
    item->seekSupported = isFile;
    return item;
}

const QHash<QUrl, ContentServer::ItemMeta>::const_iterator
ContentServer::makeItemMeta(const QUrl &url)
{
    QHash<QUrl, ContentServer::ItemMeta>::const_iterator it;
    if (url.isLocalFile()) {
        if (QFile::exists(url.toLocalFile())) {
            it = makeItemMetaUsingTracker(url);
            if (it == metaCache.end()) {
                qWarning() << "Cannot get meta using Tacker, so fallbacking to Taglib";
                it = makeItemMetaUsingTaglib(url);
            }
        } else {
            // File doesn't exist so no need to try Taglib
            qWarning() << "File doesn't exist, cannot create meta item";
            it = metaCache.end();
        }
    } else if (Utils::isUrlMic(url)) {
        qDebug() << "Mic url detected";
        it = makeMicItemMeta(url);
#ifdef PULSE
    } else if (Utils::isUrlPulse(url)) {
        qDebug() << "Pulse url detected";
        it = makePulseItemMeta(url);
#endif
    } else {
        qDebug() << "Geting meta using HTTP request";
        it = makeItemMetaUsingHTTPRequest(url);
    }

    /*if (it == metaCache.end()) {
        qWarning() << "Fallbacking to extension";
        it = makeItemMetaUsingExtension(url);
    }*/

    return it;
}

/*QString ContentServer::makePlaylistForUrl(const QUrl &url)
{
    return QString("[playlist]\nnumberofentries=1\nFile1=%1\nTitle1=Test\nLength1=-1")
            .arg(url.toString());
}*/

void ContentServer::run()
{
    qDebug() << "Creating content server worker in thread:"
             << QThread::currentThreadId();

    auto worker = ContentServerWorker::instance();
    connect(worker, &ContentServerWorker::shoutcastMetadataUpdated, this, &ContentServer::shoutcastMetadataHandler);
    connect(worker, &ContentServerWorker::pulseStreamUpdated, this, &ContentServer::pulseStreamNameHandler);
    connect(worker, &ContentServerWorker::itemAdded, this, &ContentServer::itemAddedHandler);
    connect(worker, &ContentServerWorker::itemRemoved, this, &ContentServer::itemRemovedHandler);

#ifdef PULSE
    if (Settings::instance()->getPulseSupported()) {
        // Pulse audio loop
        qDebug() << "Starting pulse-audio module";
        if (PulseDevice::setupContext()) {
            QEventLoop qtLoop;
            // TODO: Loop exit
            int ret = 0;
            while (!ret) {
                while (pa_mainloop_iterate(PulseDevice::ml, 0, &ret) > 0)
                    continue;
                qtLoop.processEvents();
            }

            qWarning() << "Disconnecting pulse-audio";
            pa_context_disconnect(PulseDevice::ctx);
            pa_context_unref(PulseDevice::ctx);
            pa_mainloop_free(PulseDevice::ml);
            return;
        } else {
            qWarning() << "Cannot start pulse-audio module";
        }
    }
#endif

    // TODO: Loop exit
    QThread::exec();
    qDebug() << "Content server worker event loop exit in thread:"
             << QThread::currentThreadId();
}

QString ContentServer::streamTitle(const QUrl &id) const
{
    if (streams.contains(id)) {
        return streams.value(id).title;
    }

    return QString();
}

void ContentServer::itemAddedHandler(const QUrl &id)
{
    qDebug() << "New item for id:" << id;
    auto &stream = streams[id];
    stream.count++;
    stream.id = id;
}

void ContentServer::itemRemovedHandler(const QUrl &id)
{
    qDebug() << "Item removed for id:" << id;
    auto &stream = streams[id];
    stream.count--;
    if (stream.count < 1) {
        streams.remove(id);
        emit streamTitleChanged(id, QString());
    }
}

void ContentServer::pulseStreamNameHandler(const QUrl &id,
                                           const QString &name)
{
    qDebug() << "Pulse-audio stream name updated:" << id << name;

    auto &stream = streams[id];
    stream.id = id;
    stream.title = name;
    emit streamTitleChanged(id, name);
}

void ContentServer::shoutcastMetadataHandler(const QUrl &id,
                                             const QByteArray &metadata)
{
    qDebug() << "Shoutcast Metadata:" << metadata;

    QString data(metadata);
    QRegExp rx("StreamTitle=\'?([^\';]*)\'?", Qt::CaseInsensitive);
    int pos = 0;
    QString title;
    while ((pos = rx.indexIn(data, pos)) != -1) {
        title = rx.cap(1).trimmed();
        if (!title.isEmpty()) {
            qDebug() << "Stream title:" << title;
            break;
        }
        pos += rx.matchedLength();
    }

    auto &stream = streams[id];
    stream.id = id;
    stream.title = title;
    emit streamTitleChanged(id, title);
}

QList<ContentServer::PlaylistItemMeta>
ContentServer::parsePls(const QByteArray &data, const QString context)
{
    qDebug() << "Parsing PLS playlist";
    QMap<int,ContentServer::PlaylistItemMeta> map;
    int pos;

    // urls
    QRegExp rxFile("\\nFile(\\d\\d?)=([^\\n]*)", Qt::CaseInsensitive);
    pos = 0;
    while ((pos = rxFile.indexIn(data, pos)) != -1) {
        QString cap1 = rxFile.cap(1);
        QString cap2 = rxFile.cap(2);
        qDebug() << "cap:" << cap1 << cap2;

        bool ok;
        int n = cap1.toInt(&ok);
        if (ok) {
            auto url = Utils::urlFromText(cap2, context);
            if (!url.isEmpty()) {
                auto &item = map[n];
                item.url = url;
            } else {
                qWarning() << "Playlist item url is invalid";
            }
        } else {
            qWarning() << "Playlist item no is invalid";
        }

        pos += rxFile.matchedLength();
    }

    if (!map.isEmpty()) {
        // titles
        QRegExp rxTitle("\\nTitle(\\d\\d?)=([^\\n]*)", Qt::CaseInsensitive);
        pos = 0;
        while ((pos = rxTitle.indexIn(data, pos)) != -1) {
            QString cap1 = rxTitle.cap(1);
            QString cap2 = rxTitle.cap(2);
            qDebug() << "cap:" << cap1 << cap2;

            bool ok;
            int n = cap1.toInt(&ok);
            if (ok && map.contains(n)) {
                auto &item = map[n];
                item.title = cap2;
            }

            pos += rxTitle.matchedLength();
        }

        // length
        QRegExp rxLength("\\nLength(\\d\\d?)=([^\\n]*)", Qt::CaseInsensitive);
        pos = 0;
        while ((pos = rxLength.indexIn(data, pos)) != -1) {
            QString cap1 = rxLength.cap(1);
            QString cap2 = rxLength.cap(2);
            qDebug() << "cap:" << cap1 << cap2;

            bool ok;
            int n = cap1.toInt(&ok);
            if (ok && map.contains(n)) {
                bool ok;
                int length = cap2.toInt(&ok);
                if (ok) {
                    auto &item = map[n];
                    item.length = length < 0 ? 0 : length;
                }
            }

            pos += rxLength.matchedLength();
        }
    } else {
        qWarning() << "Playlist doesn't contain any URLs";
    }

    return map.values();
}

QList<ContentServer::PlaylistItemMeta>
ContentServer::parseM3u(const QByteArray &data, const QString context)
{
    qDebug() << "Parsing M3U playlist";

    QList<ContentServer::PlaylistItemMeta> list;

    QTextStream s(data, QIODevice::ReadOnly);
    s.setAutoDetectUnicode(true);

    while (!s.atEnd()) {
        auto line = s.readLine();
        qDebug() << "line:" << line;
        if (line.startsWith("#")) {
            // TODO: read title from M3U playlist
        } else {
            auto url = Utils::urlFromText(line, context);
            if (!url.isEmpty()) {
                PlaylistItemMeta item;
                item.url = url;
                list.append(item);
            }
        }
    }

    return list;
}

QList<ContentServer::PlaylistItemMeta>
ContentServer::parseXspf(const QByteArray &data, const QString context)
{
    qDebug() << "Parsing XSPF playlist";
    QList<ContentServer::PlaylistItemMeta> list;

    QDomDocument doc; QString error;
    if (doc.setContent(data, false, &error)) {
        auto tracks = doc.elementsByTagName("track");
        int l = tracks.length();
        for (int i = 0; i < l; ++i) {
            auto track = tracks.at(i).toElement();
            if (!track.isNull()) {
                auto ls = track.elementsByTagName("location");
                if (!ls.isEmpty()) {
                    auto l = ls.at(0).toElement();
                    if (!l.isNull()) {
                        qDebug() << "location:" << l.text();

                        auto url = Utils::urlFromText(l.text(), context);
                        if (!url.isEmpty()) {
                            PlaylistItemMeta item;
                            item.url = url;

                            auto ts = track.elementsByTagName("title");
                            if (!ts.isEmpty()) {
                                auto t = ts.at(0).toElement();
                                if (!t.isNull()) {
                                    qDebug() << "title:" << t.text();
                                    item.title = t.text();
                                }
                            }

                            auto ds = track.elementsByTagName("duration");
                            if (!ds.isEmpty()) {
                                auto d = ds.at(0).toElement();
                                if (!d.isNull()) {
                                    qDebug() << "duration:" << d.text();
                                    item.length = d.text().toInt();
                                }
                            }

                            list.append(item);
                        }
                    }
                }
            }
        }
    } else {
        qWarning() << "Playlist parse error:" << error;
    }

    return list;
}

MicDevice::MicDevice(QObject *parent) :
    QIODevice(parent)
{
}

void MicDevice::setActive(bool value)
{
    if (value != active) {
        active = value;
        if (active && !isOpen()) {
            open(QIODevice::WriteOnly);
        }
    }
}

bool MicDevice::isActive()
{
    return active;
}

qint64 MicDevice::readData(char* data, qint64 maxSize)
{
    Q_UNUSED(data)
    Q_UNUSED(maxSize)
    return 0;
}

qint64 MicDevice::writeData(const char* data, qint64 maxSize)
{
    auto worker = ContentServerWorker::instance();

    if (!worker->micItems.isEmpty()) {
        QByteArray d = QByteArray::fromRawData(data, static_cast<int>(maxSize));
        QByteArray d2;

        float volume = Settings::instance()->getMicVolume();
        if (volume != 1.0f) {
            // changing mic volume
            QDataStream sr(&d, QIODevice::ReadOnly);
            sr.setByteOrder(QDataStream::BigEndian);
            QDataStream sw(&d2, QIODevice::WriteOnly);
            sw.setByteOrder(QDataStream::BigEndian);

            qint16 sample;
            while (!sr.atEnd()) {
                sr >> sample;
                sample = static_cast<qint16>(sample * volume);
                sw << sample;
            }
        }

        auto i = worker->micItems.begin();
        while (i != worker->micItems.end()) {
            //qDebug() << "Mic item, remote addr:" << i->req->remoteAddress();
            if (!i->resp->isHeaderWritten()) {
                qWarning() << "Head not written";
                i->resp->end();
            }

            if (i->resp->isFinished()) {
                qWarning() << "Server request already finished, so removing mic item";
                i = worker->micItems.erase(i);
            } else {
                if (active) {
                    i->resp->write(d2.isEmpty() ? d : d2);
                } else {
                    qDebug() << "Mic dev is not active, so disconnecting server request";
                    i->resp->end();
                }
                ++i;
            }
        }
    }

    if (worker->micItems.isEmpty())
        worker->stopMic();

    return maxSize;
}

#ifdef PULSE
const int PulseDevice::timerDelta = 1;
bool PulseDevice::timerActive = false;
bool PulseDevice::muted = false;
pa_sample_spec PulseDevice::sampleSpec = {PA_SAMPLE_S16BE, 22050, 2};
pa_stream* PulseDevice::stream = nullptr;
uint32_t PulseDevice::connectedSinkInput = PA_INVALID_INDEX;
#ifdef SAILFISH
uint32_t PulseDevice::nullSink = PA_INVALID_INDEX;
uint32_t PulseDevice::primarySink = PA_INVALID_INDEX;
#endif
pa_mainloop* PulseDevice::ml = nullptr;
pa_mainloop_api* PulseDevice::mla = nullptr;
pa_context* PulseDevice::ctx = nullptr;
QHash<uint32_t, PulseDevice::Client> PulseDevice::clients = QHash<uint32_t, PulseDevice::Client>();
QHash<uint32_t, PulseDevice::SinkInput> PulseDevice::sinkInputs = QHash<uint32_t, PulseDevice::SinkInput>();

QString PulseDevice::subscriptionEventToStr(pa_subscription_event_type_t t)
{
    auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    QString facility_str("UNKNOWN");

    switch (facility) {
    case PA_SUBSCRIPTION_EVENT_SINK: facility_str = "SINK"; break;
    case PA_SUBSCRIPTION_EVENT_SOURCE: facility_str = "SOURCE"; break;
    case PA_SUBSCRIPTION_EVENT_SINK_INPUT: facility_str = "SINK_INPUT"; break;
    case PA_SUBSCRIPTION_EVENT_SOURCE_OUTPUT: facility_str = "SOURCE_OUTPUT"; break;
    case PA_SUBSCRIPTION_EVENT_MODULE: facility_str = "SOURCE_OUTPUT"; break;
    case PA_SUBSCRIPTION_EVENT_CLIENT: facility_str = "CLIENT"; break;
    case PA_SUBSCRIPTION_EVENT_SAMPLE_CACHE: facility_str = "SAMPLE_CACHE"; break;
    case PA_SUBSCRIPTION_EVENT_SERVER: facility_str = "SERVER"; break;
    case PA_SUBSCRIPTION_EVENT_AUTOLOAD: facility_str = "AUTOLOAD"; break;
    case PA_SUBSCRIPTION_EVENT_CARD: facility_str = "CARD"; break;
    }

    QString type_str("UNKNOWN");

    switch (type) {
    case PA_SUBSCRIPTION_EVENT_NEW: type_str = "NEW"; break;
    case PA_SUBSCRIPTION_EVENT_CHANGE: type_str = "CHANGE"; break;
    case PA_SUBSCRIPTION_EVENT_REMOVE: type_str = "REMOVE"; break;
    }

    return facility_str + " " + type_str;
}

void PulseDevice::subscriptionCallback(pa_context *ctx, pa_subscription_event_type_t t, uint32_t idx, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    qDebug() << "Pulse-audio subscriptionCallback:" << subscriptionEventToStr(t) << idx;

    auto facility = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
    auto type = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

    if (facility == PA_SUBSCRIPTION_EVENT_SINK_INPUT) {
        if (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_CHANGE) {
            pa_operation_unref(pa_context_get_sink_input_info(ctx, idx, sinkInputInfoCallback, nullptr));
        } else if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            qDebug() << "Removing pulse-audio sink input:" << idx;
            sinkInputs.remove(idx);
            discoverStream();
        }
    } else if (facility == PA_SUBSCRIPTION_EVENT_CLIENT) {
        if (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_CHANGE) {
            pa_operation_unref(pa_context_get_client_info(ctx, idx, clientInfoCallback, nullptr));
        } else if (type == PA_SUBSCRIPTION_EVENT_REMOVE) {
            qDebug() << "Removing pulse-audio client:" << idx;
            clients.remove(idx);
        }
#ifdef SAILFISH
    } else if (facility == PA_SUBSCRIPTION_EVENT_SINK) {
        if (type == PA_SUBSCRIPTION_EVENT_NEW || type == PA_SUBSCRIPTION_EVENT_CHANGE) {
            pa_operation_unref(pa_context_get_sink_info_by_index(ctx, idx, sinkInfoCallback, nullptr));
        }
#endif
    }
}

void PulseDevice::successSubscribeCallback(pa_context *ctx, int success, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    if (success) {
         pa_operation_unref(pa_context_get_client_info_list(ctx, clientInfoCallback, nullptr));
         pa_operation_unref(pa_context_get_sink_input_info_list(ctx, sinkInputInfoCallback, nullptr));
#ifdef SAILFISH
         pa_operation_unref(pa_context_get_sink_info_list(ctx, sinkInfoCallback, nullptr));
#endif
    }
}

void PulseDevice::stateCallback(pa_context *ctx, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    switch (pa_context_get_state(ctx)) {
        case PA_CONTEXT_CONNECTING:
            qDebug() << "Pulse-audio connecting";
            break;
        case PA_CONTEXT_AUTHORIZING:
            qDebug() << "Pulse-audio authorizing";
            break;
        case PA_CONTEXT_SETTING_NAME:
            qDebug() << "Pulse-audio setting name";
            break;
        case PA_CONTEXT_READY: {
            qDebug() << "Pulse-audio ready";
#ifdef SAILFISH
            auto mask = static_cast<pa_subscription_mask_t>(
                        PA_SUBSCRIPTION_MASK_SINK_INPUT|
                        PA_SUBSCRIPTION_MASK_CLIENT|
                        PA_SUBSCRIPTION_MASK_SINK);
#else
            auto mask = static_cast<pa_subscription_mask_t>(
                        PA_SUBSCRIPTION_MASK_SINK_INPUT|
                        PA_SUBSCRIPTION_MASK_CLIENT);
#endif
            pa_operation_unref(pa_context_subscribe(
                                   ctx, mask, successSubscribeCallback, nullptr));
            break;
        }
        case PA_CONTEXT_TERMINATED:
            qDebug() << "Pulse-audio terminated";
            break;
        case PA_CONTEXT_FAILED:
            qDebug() << "Pulse-audio failed";
            break;
        default:
            qDebug() << "Pulse-audio connection failure: " << pa_strerror(pa_context_errno(ctx));
    }
}

void PulseDevice::streamRequestCallback(pa_stream *stream, size_t nbytes, void *userdata)
{
    Q_ASSERT(stream);
    Q_UNUSED(userdata);

    if (nbytes <= 0) {
        qWarning() << "Pulse-audio stream nbytes <= 0";
        return;
    }

    const void *data;
    if (pa_stream_peek(stream, &data, &nbytes) < 0) {
        qWarning() << "Pulse-audio stream peek failed";
        return;
    }

    if (!data) {
        qWarning() << "Pulse-audio stream peek data is null";
        return;
    }

    if (nbytes <= 0) {
        qWarning() << "Pulse-audio stream peeked nbytes <= 0";
        return;
    }

    auto worker = ContentServerWorker::instance();
    worker->writePulseData(static_cast<const char*>(data), nbytes);

    pa_stream_drop(stream);
}

void PulseDevice::stopRecordStream()
{
    if (stream) {
        unmuteConnectedSinkInput();
        qDebug() << "Disconnecting pulse-audio stream";
        pa_stream_disconnect(stream);
        pa_stream_unref(stream);
        stream = nullptr;
        connectedSinkInput = PA_INVALID_INDEX;
    }
}

void PulseDevice::muteConnectedSinkInput()
{
#ifdef SAILFISH
    qDebug() << "Mute:";
    qDebug() << "  sink input:" << connectedSinkInput;
    qDebug() << "  null sink:" << nullSink;
    if (!muted && connectedSinkInput != PA_INVALID_INDEX && nullSink != PA_INVALID_INDEX) {
        qDebug() << "Muting sink input by moving it to null sink";
        pa_operation_unref(pa_context_move_sink_input_by_index(
                               ctx, connectedSinkInput, nullSink, nullptr, nullptr));
        muted = true;
    } else {
        qDebug() << "Cannot mute";
    }
#endif
}

void PulseDevice::unmuteConnectedSinkInput()
{
#ifdef SAILFISH
    qDebug() << "Unmute:";
    qDebug() << "  sink input:" << connectedSinkInput;
    qDebug() << "  primary sink:" << primarySink;
    if (connectedSinkInput != PA_INVALID_INDEX && primarySink != PA_INVALID_INDEX &&
            sinkInputs.contains(connectedSinkInput)) {
        qDebug() << "Unmuting sink input by moving it to primary sink";
        pa_operation_unref(pa_context_move_sink_input_by_index(
                               ctx, connectedSinkInput, primarySink, nullptr, nullptr));
    } else {
        qDebug() << "Cannot unmute";
    }
    muted = false;
#endif
}

bool PulseDevice::startRecordStream(pa_context *ctx, uint32_t si, const Client &client)
{
    stopRecordStream();

    qDebug() << "Creating new pulse-audio stream connected to sink input";
    stream = pa_stream_new(ctx, Jupii::APP_NAME, &sampleSpec, nullptr);
    pa_stream_set_read_callback(stream, streamRequestCallback, nullptr);
    connectedSinkInput = si;

    muteConnectedSinkInput();

    if (pa_stream_set_monitor_stream(stream, si) < 0) {
        qWarning() << "Pulse-audio stream set monitor error";
    } else if (pa_stream_connect_record(stream, nullptr, nullptr, PA_STREAM_NOFLAGS) < 0) {
        qWarning() << "Pulse-audio stream connect record error";
    } else {
        qDebug() << "Sink input successfully connected";
        return true;
    }

    // something went wrong, so reseting stream
    pa_stream_disconnect(stream);
    pa_stream_unref(stream);
    unmuteConnectedSinkInput();
    stream = nullptr;
    connectedSinkInput = PA_INVALID_INDEX;

    return false;
}

void PulseDevice::sinkInputInfoCallback(pa_context *ctx, const pa_sink_input_info *i, int eol, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    if (!eol) {
        qDebug() << "sinkInputInfoCallback:";
        qDebug() << "  index:" << i->index;
        qDebug() << "  name:" << i->name;
        qDebug() << "  client:" << i->client;
        qDebug() << "  has_volume:" << i->has_volume;
        qDebug() << "  mute:" << i->mute;
        qDebug() << "  volume.channels:" << i->volume.channels;
        qDebug() << "  volume.values[0]:" << i->volume.values[0];
        qDebug() << "  corked:" << i->corked;
        qDebug() << "  sink:" << i->sink;
        qDebug() << "  sample_spec:" << pa_sample_format_to_string(i->sample_spec.format) << " "
             << i->sample_spec.rate << " "
             << static_cast<uint>(i->sample_spec.channels);
        auto props = pa_proplist_to_string(i->proplist);
        qDebug() << "  props:\n" << props;
        pa_xfree(props);

        auto& si = sinkInputs[i->index];
        si.idx = i->index;
        si.clientIdx = i->client;
        si.name = i->name;
        si.corked = i->corked;
    } else {
        discoverStream();
    }
}

void PulseDevice::discoverStream()
{
    if (isInited()) {
        auto worker = ContentServerWorker::instance();
        if (worker->pulseDev) {
            QHash<uint32_t, SinkInput>::const_iterator i;
            for (i = sinkInputs.begin(); i != sinkInputs.end(); ++i) {
                const auto si = i.value();
                if (!si.corked && clients.contains(si.clientIdx)) {
                    const auto client = clients.value(si.clientIdx);
                    bool needUpdate = false;
                    if (connectedSinkInput != si.idx) {
                        qDebug() << "Starting recording for:";
                        qDebug() << "  sink input:" << si.idx << si.name;
                        qDebug() << "  client:" << client.idx << client.name;
                        if (startRecordStream(ctx, si.idx, client))
                            needUpdate = true;
                    } else {
                        qDebug() << "Sink is already connected";
                        needUpdate = true;
                    }

                    if (needUpdate) {
                        qDebug() << "Updating stream name to name of sink input's client:" << client.name;
                        worker->updatePulseStreamName(client.name);
                    } else {
                        worker->updatePulseStreamName(QString());
                    }

                    return;
                }
            }

            qDebug() << "No proper pulse-audio sink found";
        } else {
            qDebug() << "Pulse dev not created";
        }
        worker->updatePulseStreamName(QString());
        stopRecordStream();
    } else {
        qWarning() << "Pulse-audio is not inited";
    }
}

QList<PulseDevice::Client> PulseDevice::activeClients()
{
    QList<PulseDevice::Client> list;

    for (auto ci : clients.keys()) {
        for (auto& s : sinkInputs.values()) {
            if (s.clientIdx == ci) {
                list << clients[ci];
                break;
            }
        }
    }

    return list;
}

bool PulseDevice::isBlacklisted(const char* name)
{
#ifdef SAILFISH
    if (!strcmp(name, "ngfd") ||
        !strcmp(name, "feedback-event") ||
        !strcmp(name, "keyboard_0") ||
        !strcmp(name, "keyboard_1") ||
        !strcmp(name, "ngf-tonegen-plugin") ||
        !strcmp(name, "jolla keyboard")) {
        return true;
    }
#endif
    return false;
}

void PulseDevice::correctClientName(Client &client)
{
#ifdef SAILFISH
    if (client.name == "CubebUtils" && !client.binary.isEmpty()) {
        client.name = client.binary;
    } else if (client.name == "aliendalvik_audio_glue") {
        client.name = "Android";
    }
#endif
}

void PulseDevice::clientInfoCallback(pa_context *ctx, const pa_client_info *i, int eol, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    if (!eol) {
        qDebug() << "clientInfoCallback:";
        qDebug() << "  index:" << i->index;
        qDebug() << "  name:" << i->name;
        auto props = pa_proplist_to_string(i->proplist);
        qDebug() << "  props:\n" << props;
        pa_xfree(props);

        if (!isBlacklisted(i->name)) {
            auto& client = clients[i->index];
            client.idx = i->index;

            client.name = QString::fromLatin1(i->name);

            if (pa_proplist_contains(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY)) {
                const void* data; size_t size;
                if (pa_proplist_get(i->proplist, PA_PROP_APPLICATION_PROCESS_BINARY, &data, &size) >= 0) {
                    client.binary = QString::fromUtf8(static_cast<const char*>(data), size-1);
                }
            }
            if (pa_proplist_contains(i->proplist, PA_PROP_APPLICATION_ICON_NAME)) {
                const void* data; size_t size;
                if (pa_proplist_get(i->proplist, PA_PROP_APPLICATION_ICON_NAME, &data, &size) >= 0) {
                    client.icon = QString::fromUtf8(static_cast<const char*>(data), size-1);
                }
            }
            correctClientName(client);
        } else {
            qDebug() << "Client blacklisted";
            clients.remove(i->index);
        }
    }
}

#ifdef SAILFISH
void PulseDevice::sinkInfoCallback(pa_context *ctx, const pa_sink_info *i, int eol, void *userdata)
{
    Q_ASSERT(ctx);
    Q_UNUSED(userdata);

    if (!eol) {
        qDebug() << "sinkInfoCallback:";
        qDebug() << "  index:" << i->index;
        qDebug() << "  name:" << i->name;
        if (!strcmp(i->name, "sink.null")) {
            if (nullSink != i->index) {
                qDebug() << "Null sink index changed";
                nullSink = i->index;
            }
        } else if (!strcmp(i->name, "sink.primary")) {
            if (primarySink != i->index) {
                qDebug() << "Primary sink index changed";
                primarySink = i->index;
            }
        }
    }
}
#endif

void PulseDevice::timeEventCallback(pa_mainloop_api *mla, pa_time_event *e, const struct timeval *tv, void *userdata)
{
    Q_UNUSED(mla);
    Q_UNUSED(userdata);

    //qDebug() << "timeEventCallback";

    auto worker = ContentServerWorker::instance();
    if (worker->pulseDev) {
        if (worker->pulseItems.isEmpty()) {
            worker->stopPulse();
            return;
        } else if (!stream) {
            // sending null data to connected device because no sink is connected
            //const size_t size = sampleSpec.rate * 2 * sampleSpec.channels * timerDelta;
            const size_t size = sampleSpec.rate * 1 * sampleSpec.channels * timerDelta;
            worker->writePulseData(nullptr, size);
        }

        restartTimer(e, tv);
    }
}

void PulseDevice::exitSignalCallback(pa_mainloop_api *mla, pa_signal_event *e, int sig, void *userdata)
{
    Q_UNUSED(userdata);
    Q_UNUSED(mla);
    Q_UNUSED(e);
    Q_UNUSED(sig);
    qDebug() << "Pulse-audio exit signal";
}

bool PulseDevice::isInited()
{
    return ml && mla && ctx;
}

bool PulseDevice::setupContext()
{
    ml = pa_mainloop_new();
    mla = pa_mainloop_get_api(ml);

    if (pa_signal_init(mla) < 0) {
        qWarning() << "Cannot init pulse-audio signals";
        pa_mainloop_free(ml);
        ml = nullptr;
        mla = nullptr;
    } else {
        pa_signal_new(SIGINT, exitSignalCallback, nullptr);
        pa_signal_new(SIGTERM, exitSignalCallback, nullptr);

        ctx = pa_context_new(mla, Jupii::APP_NAME);
        if (!ctx) {
            qWarning() << "New pulse-audio context failed";
            pa_mainloop_free(ml);
            ml = nullptr;
            mla = nullptr;
        } else {
            pa_context_set_state_callback(ctx, stateCallback, nullptr);
            pa_context_set_subscribe_callback(ctx, subscriptionCallback, nullptr);

            if (pa_context_connect(ctx, nullptr, PA_CONTEXT_NOFLAGS, nullptr) < 0) {
                qWarning() << "Cannot connect pulse-audio context:" << pa_strerror(pa_context_errno(ctx));
                pa_context_unref(ctx);
                ctx = nullptr;
            } else {
                qDebug() << "Pulse-audio context setup successful";
                return true;
            }
        }
    }

    qWarning() << "Pulse-audio context setup error";
    return false;
}

bool PulseDevice::startTimer()
{
    if (!timerActive) {
        timeval tv; gettimeofday(&tv, nullptr); tv.tv_sec += timerDelta;
        if (!mla->time_new(mla, &tv, timeEventCallback, nullptr)) {
            qWarning() << "Timer event failed";
            return false;
        }
    }

    timerActive = true;
    return true;
}

void PulseDevice::restartTimer(pa_time_event *e, const struct timeval *tv)
{
    if (timerActive) {
        // restarting timer to current time + timerDelta
        timeval ntv = {tv->tv_sec + timerDelta, tv->tv_usec};
        mla->time_restart(e, &ntv);
    }
}

void PulseDevice::stopTimer()
{
    timerActive = false;
}

PulseDevice::PulseDevice(QObject *parent) :
    QObject(parent)
{
}

void ContentServerWorker::writePulseData(const char *data, size_t maxSize)
{
    if (!pulseItems.isEmpty()) {
        //QByteArray d = QByteArray::fromRawData(data, static_cast<int>(maxSize));
        QByteArray d;
        if (data) {
            //d = QByteArray(data, static_cast<int>(maxSize));
            d = QByteArray::fromRawData(data, static_cast<int>(maxSize));
        } else {
            // Writing null data
            d = QByteArray(static_cast<int>(maxSize),0);
        }

        auto i = pulseItems.begin();
        while (i != pulseItems.end()) {
            if (!i->resp->isHeaderWritten()) {
                qWarning() << "Head not written";
                i->resp->end();
            }
            if (i->resp->isFinished()) {
                qWarning() << "Server request already finished, so removing pulse item";
                auto id = i->id;
                i = pulseItems.erase(i);
                emit itemRemoved(id);
            } else {
                i->resp->write(d);
                ++i;
            }
        }
    } else {
        qDebug() << "No pulse items so stopping";
        stopPulse();
    }
}
#endif
