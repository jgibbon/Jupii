/* Copyright (C) 2018 Michal Kosciesza <michal@mkiol.net>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include <QDebug>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QFile>
#include <QDir>
#include <QList>

#ifdef SAILFISH
#include <sailfishapp.h>
#endif

#include "somafmmodel.h"

SomafmModel::SomafmModel(QObject *parent) :
    ListModel(new SomafmItem, parent)
{
    QDir dir;
    QString jfile;
    //QPixmap pm(QLatin1String(":/images/jupii-64.png"));

#ifdef SAILFISH
    dir = QDir(SailfishApp::pathTo("somafm").toLocalFile());
    jfile = SailfishApp::pathTo("somafm/somafm.json").toLocalFile();
#endif

    QFile f(jfile);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        qWarning() << "File" << jfile << "can't be opened";
        return;
    }

    auto doc = QJsonDocument::fromJson(f.readAll());
    f.close();
    if (doc.isEmpty() || !doc.isObject()) {
        qWarning() << "Can't parse json file" << jfile;
        return;
    }

    auto obj = doc.object();
    if (!obj.contains("channels") || !obj["channels"].isArray()) {
        qWarning() << "No channels";
        return;
    }

    QList<ListItem*> items;
    auto channels = obj["channels"].toArray();
    for (const auto c : channels) {
        if (c.isObject()) {
            auto obj = c.toObject();
            auto icon = QUrl::fromLocalFile(dir.filePath(obj["icon"].toString()));
            items << new SomafmItem(
                            obj["id"].toString(),
                            obj["title"].toString(),
                            obj["description"].toString(),
                            QUrl(obj["url"].toString()),
#ifdef SAILFISH
                            icon,
#else
                            QIcon(icon.toLocalFile()),
#endif
                            this
                        );
        }
    }

    appendRows(items);
}

SomafmItem::SomafmItem(const QString &id,
                   const QString &name,
                   const QString &description,
                   const QUrl &url,
#ifdef SAILFISH
                   const QUrl &icon,
#else
                   const QIcon &icon,
#endif
                   QObject *parent) :
    ListItem(parent),
    m_id(id),
    m_name(name),
    m_description(description),
    m_url(url),
    m_icon(icon)
{
}

QHash<int, QByteArray> SomafmItem::roleNames() const
{
    QHash<int, QByteArray> names;
    names[IdRole] = "id";
    names[NameRole] = "name";
    names[DescriptionRole] = "description";
    names[UrlRole] = "url";
    names[IconRole] = "icon";
    return names;
}

QVariant SomafmItem::data(int role) const
{
    switch(role) {
    case IdRole:
        return id();
    case NameRole:
        return name();
    case DescriptionRole:
        return description();
    case UrlRole:
        return url();
    case IconRole:
        return icon();
    default:
        return QVariant();
    }
}