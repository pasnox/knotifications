/*
   Copyright (C) 2005-2009 by Olivier Goffart <ogoffart at kde.org>
   Copyright (C) 2008 by Dmitry Suzdalev <dimsuz@gmail.com>
   Copyright (C) 2014 by Martin Klapetek <mklapetek@kde.org>

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 2.1 of the License, or (at your option) version 3, or any
   later version accepted by the membership of KDE e.V. (or its
   successor approved by the membership of KDE e.V.), which shall
   act as a proxy defined in Section 6 of version 3 of the license.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library.  If not, see <http://www.gnu.org/licenses/>.

 */

#include "notifybypopup.h"
#include "imageconverter.h"

#include "knotifyconfig.h"
#include "knotification.h"
#include "debug_p.h"

#include <QBuffer>
#include <QGuiApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusServiceWatcher>
#include <QDBusMessage>
#include <QXmlStreamReader>
#include <QMap>
#include <QHash>
#include <QPointer>
#include <QMutableListIterator>
#include <QThread>
#include <QFontMetrics>
#include <QIcon>
#include <QUrl>

#include <kconfiggroup.h>

static const char dbusServiceName[] = "org.freedesktop.Notifications";
static const char dbusInterfaceName[] = "org.freedesktop.Notifications";
static const char dbusPath[] = "/org/freedesktop/Notifications";

class NotifyByPopupPrivate {
public:
    NotifyByPopupPrivate(NotifyByPopup *parent) : q(parent) {}

    /**
     * Sends notification to DBus "org.freedesktop.notifications" interface.
     * @param id knotify-sid identifier of notification
     * @param config notification data
     * @param update If true, will request the DBus service to update
                     the notification with new data from \c notification
     *               Otherwise will put new notification on screen
     * @return true for success or false if there was an error.
     */
    bool sendNotificationToServer(KNotification *notification, const KNotifyConfig &config, bool update = false);

    /**
     * Find the caption and the icon name of the application
     */
    void getAppCaptionAndIconName(const KNotifyConfig &config, QString *appCaption, QString *iconName);
    /*
     * Query the dbus server for notification capabilities
     * If no DBus server is present, use fallback capabilities for KPassivePopup
     */
    void queryPopupServerCapabilities();

    /**
     * DBus notification daemon capabilities cache.
     * Do not use this variable. Use #popupServerCapabilities() instead.
     * @see popupServerCapabilities
     */
    QStringList popupServerCapabilities;

    /**
     * In case we still don't know notification server capabilities,
     * we need to query those first. That's done in an async way
     * so we queue all notifications while waiting for the capabilities
     * to return, then process them from this queue
     */
    QList<QPair<KNotification*, KNotifyConfig> > notificationQueue;
    /**
     * Whether the DBus notification daemon capability cache is up-to-date.
     */
    bool dbusServiceCapCacheDirty;

    /*
     * As we communicate with the notification server over dbus
     * we use only ids, this is for fast KNotifications lookup
     */
    QHash<uint, QPointer<KNotification>> notifications;


    NotifyByPopup * const q;
};

//---------------------------------------------------------------------------------------

NotifyByPopup::NotifyByPopup(QObject *parent)
  : KNotificationPlugin(parent),
    d(new NotifyByPopupPrivate(this))
{
    d->dbusServiceCapCacheDirty = true;

     bool connected = QDBusConnection::sessionBus().connect(QString(), // from any service
                                                               QString::fromLatin1(dbusPath),
                                                               QString::fromLatin1(dbusInterfaceName),
                                                               QStringLiteral("ActionInvoked"),
                                                               this,
                                                               SLOT(onNotificationActionInvoked(uint,QString)));
    if (!connected) {
        qCWarning(LOG_KNOTIFICATIONS) << "warning: failed to connect to ActionInvoked dbus signal";
    }

    connected = QDBusConnection::sessionBus().connect(QString(), // from any service
                                                        QString::fromLatin1(dbusPath),
                                                        QString::fromLatin1(dbusInterfaceName),
                                                        QStringLiteral("NotificationClosed"),
                                                        this,
                                                        SLOT(onNotificationClosed(uint,uint)));
    if (!connected) {
        qCWarning(LOG_KNOTIFICATIONS) << "warning: failed to connect to NotificationClosed dbus signal";
    }
}


NotifyByPopup::~NotifyByPopup()
{
    delete d;
}

void NotifyByPopup::notify(KNotification *notification, KNotifyConfig *notifyConfig)
{
    notify(notification, *notifyConfig);
}

void NotifyByPopup::notify(KNotification *notification, const KNotifyConfig &notifyConfig)
{
    if (d->notifications.contains(notification->id())) {
        // notification is already on the screen, do nothing
        finish(notification);
        return;
    }

    if (d->dbusServiceCapCacheDirty) {
        // if we don't have the server capabilities yet, we need to query for them first;
        // as that is an async dbus operation, we enqueue the notification and process them
        // when we receive dbus reply with the server capabilities
        d->notificationQueue.append(qMakePair(notification, notifyConfig));
        d->queryPopupServerCapabilities();
    } else {
        if (!d->sendNotificationToServer(notification, notifyConfig)) {
            finish(notification); //an error occurred.
        }
    }
}

void NotifyByPopup::update(KNotification *notification, KNotifyConfig *notifyConfig)
{
    update(notification, *notifyConfig);
}

void NotifyByPopup::update(KNotification *notification, const KNotifyConfig &notifyConfig)
{
    d->sendNotificationToServer(notification, notifyConfig, true);
}

void NotifyByPopup::close(KNotification *notification)
{
    uint id = d->notifications.key(notification, 0);

    if (id == 0) {
        qCDebug(LOG_KNOTIFICATIONS) << "not found dbus id to close" << notification->id();
        return;
    }

    QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(dbusServiceName), QString::fromLatin1(dbusPath),
                                                    QString::fromLatin1(dbusInterfaceName), QStringLiteral("CloseNotification"));
    QList<QVariant> args;
    args.append(id);
    m.setArguments(args);

    // send(..) does not block
    bool queued = QDBusConnection::sessionBus().send(m);

    if (!queued) {
        qCWarning(LOG_KNOTIFICATIONS) << "Failed to queue dbus message for closing a notification";
    }

    QMutableListIterator<QPair<KNotification*, KNotifyConfig> > iter(d->notificationQueue);
    while (iter.hasNext()) {
        auto &item = iter.next();
        if (item.first == notification) {
            iter.remove();
        }
    }
}

void NotifyByPopup::onNotificationActionInvoked(uint notificationId, const QString &actionKey)
{
    auto iter = d->notifications.find(notificationId);
    if (iter == d->notifications.end()) {
        return;
    }

    KNotification *n = *iter;
    if (n) {
        if (actionKey == QLatin1String("default")) {
            emit actionInvoked(n->id(), 0);
        } else {
            emit actionInvoked(n->id(), actionKey.toUInt());
        }
    } else {
        d->notifications.erase(iter);
    }
}

void NotifyByPopup::onNotificationClosed(uint dbus_id, uint reason)
{
    auto iter = d->notifications.find(dbus_id);
    if (iter == d->notifications.end()) {
        return;
    }
    KNotification *n = *iter;
    d->notifications.remove(dbus_id);

    if (n) {
        emit finished(n);
        // The popup bubble is the only user facing part of a notification,
        // if the user closes the popup, it means he wants to get rid
        // of the notification completely, including playing sound etc
        // Therefore we close the KNotification completely after closing
        // the popup, but only if the reason is 2, which means "user closed"
        if (reason == 2) {
            n->close();
        }
    }
}

void NotifyByPopup::onServerReply(QDBusPendingCallWatcher *watcher)
{
    // call deleteLater first, since we might return in the middle of the function
    watcher->deleteLater();
    KNotification *notification = watcher->property("notificationObject").value<KNotification*>();
    if (!notification) {
        qCWarning(LOG_KNOTIFICATIONS) << "Invalid notification object passed in DBus reply watcher; notification will probably break";
        return;
    }

    QDBusPendingReply<uint> reply = *watcher;

    d->notifications.insert(reply.argumentAt<0>(), notification);
}

void NotifyByPopup::onServerCapabilitiesReceived(const QStringList &capabilities)
{
    d->popupServerCapabilities = capabilities;
    d->dbusServiceCapCacheDirty = false;

    // re-run notify() on all enqueued notifications
    for (int i = 0, total = d->notificationQueue.size(); i < total; ++i) {
        notify(d->notificationQueue.at(i).first, d->notificationQueue.at(i).second);
    }

    d->notificationQueue.clear();
}

void NotifyByPopupPrivate::getAppCaptionAndIconName(const KNotifyConfig &notifyConfig, QString *appCaption, QString *iconName)
{
    KConfigGroup globalgroup(&(*notifyConfig.eventsfile), QStringLiteral("Global"));
    *appCaption = globalgroup.readEntry("Name", globalgroup.readEntry("Comment", notifyConfig.appname));

    KConfigGroup eventGroup(&(*notifyConfig.eventsfile), QStringLiteral("Event/%1").arg(notifyConfig.eventid));
    if (eventGroup.hasKey("IconName")) {
        *iconName = eventGroup.readEntry("IconName", notifyConfig.appname);
    } else {
        *iconName = globalgroup.readEntry("IconName", notifyConfig.appname);
    }
}

bool NotifyByPopupPrivate::sendNotificationToServer(KNotification *notification, const KNotifyConfig &notifyConfig_nocheck, bool update)
{
    uint updateId = notifications.key(notification, 0);

    if (update) {
        if (updateId == 0) {
            // we have nothing to update; the notification we're trying to update
            // has been already closed
            return false;
        }
    }

    QDBusMessage dbusNotificationMessage = QDBusMessage::createMethodCall(QString::fromLatin1(dbusServiceName), QString::fromLatin1(dbusPath), QString::fromLatin1(dbusInterfaceName), QStringLiteral("Notify"));

    QList<QVariant> args;

    QString appCaption;
    QString iconName;
    getAppCaptionAndIconName(notifyConfig_nocheck, &appCaption, &iconName);

    //did the user override the icon name?
    if (!notification->iconName().isEmpty()) {
        iconName = notification->iconName();
    }

    args.append(appCaption); // app_name
    args.append(updateId);  // notification to update
    args.append(iconName); // app_icon

    QString title = notification->title().isEmpty() ? appCaption : notification->title();
    QString text = notification->text();

    if (!popupServerCapabilities.contains(QLatin1String("body-markup"))) {
        title = q->stripRichText(title);
        text = q->stripRichText(text);
    }

    args.append(title); // summary
    args.append(text); // body

    // freedesktop.org spec defines action list to be list like
    // (act_id1, action1, act_id2, action2, ...)
    //
    // assign id's to actions like it's done in fillPopup() method
    // (i.e. starting from 1)
    QStringList actionList;
    if (popupServerCapabilities.contains(QLatin1String("actions"))) {
        QString defaultAction = notification->defaultAction();
        if (!defaultAction.isEmpty()) {
            actionList.append(QStringLiteral("default"));
            actionList.append(defaultAction);
        }
        int actId = 0;
        const auto listActions = notification->actions();
        for (const QString &actionName : listActions) {
            actId++;
            actionList.append(QString::number(actId));
            actionList.append(actionName);
        }
    }

    args.append(actionList); // actions

    QVariantMap hintsMap;
    // Add the application name to the hints.
    // According to freedesktop.org spec, the app_name is supposed to be the application's "pretty name"
    // but in some places it's handy to know the application name itself
    if (!notification->appName().isEmpty()) {
        hintsMap[QStringLiteral("x-kde-appname")] = notification->appName();
    }

    if (!notification->eventId().isEmpty()) {
        hintsMap[QStringLiteral("x-kde-eventId")] = notification->eventId();
    }

    if (notification->flags() & KNotification::SkipGrouping) {
        hintsMap[QStringLiteral("x-kde-skipGrouping")] = 1;
    }

    if (!notification->urls().isEmpty()) {
        hintsMap[QStringLiteral("x-kde-urls")] = QUrl::toStringList(notification->urls());
    }

    if (!(notification->flags() & KNotification::Persistent)) {
        hintsMap[QStringLiteral("transient")] = true;
    }

    QString desktopFileName = QGuiApplication::desktopFileName();
    if (!desktopFileName.isEmpty()) {
        // handle apps which set the desktopFileName property with filename suffix,
        // due to unclear API dox (https://bugreports.qt.io/browse/QTBUG-75521)
        if (desktopFileName.endsWith(QLatin1String(".desktop"))) {
            desktopFileName.chop(8);
        }
        hintsMap[QStringLiteral("desktop-entry")] = desktopFileName;
    }

    int urgency = -1;
    switch (notification->urgency()) {
    case KNotification::DefaultUrgency:
        break;
    case KNotification::LowUrgency:
        urgency = 0;
        break;
    case KNotification::NormalUrgency:
        Q_FALLTHROUGH();
    // freedesktop.org notifications only know low, normal, critical
    case KNotification::HighUrgency:
        urgency = 1;
        break;
    case KNotification::CriticalUrgency:
        urgency = 2;
        break;
    }

    if (urgency > -1) {
        hintsMap[QStringLiteral("urgency")] = urgency;
    }

    const QVariantMap hints = notification->hints();
    for (auto it = hints.constBegin(); it != hints.constEnd(); ++it) {
        hintsMap[it.key()] = it.value();
    }

    //FIXME - reenable/fix
    // let's see if we've got an image, and store the image in the hints map
    if (!notification->pixmap().isNull()) {
        QByteArray pixmapData;
        QBuffer buffer(&pixmapData);
        buffer.open(QIODevice::WriteOnly);
        notification->pixmap().save(&buffer, "PNG");
        buffer.close();
        hintsMap[QStringLiteral("image_data")] = ImageConverter::variantForImage(QImage::fromData(pixmapData));
    }

    args.append(hintsMap); // hints

    // Persistent     => 0  == infinite timeout
    // CloseOnTimeout => -1 == let the server decide
    int timeout = (notification->flags() & KNotification::Persistent) ? 0 : -1;

    args.append(timeout); // expire timeout

    dbusNotificationMessage.setArguments(args);

    QDBusPendingCall notificationCall = QDBusConnection::sessionBus().asyncCall(dbusNotificationMessage, -1);

    //parent is set to the notification so that no-one ever accesses a dangling pointer on the notificationObject property
    QDBusPendingCallWatcher *watcher = new QDBusPendingCallWatcher(notificationCall, notification);
    watcher->setProperty("notificationObject", QVariant::fromValue<KNotification*>(notification));

    QObject::connect(watcher, &QDBusPendingCallWatcher::finished,
                     q, &NotifyByPopup::onServerReply);

    return true;
}

void NotifyByPopupPrivate::queryPopupServerCapabilities()
{
    if (dbusServiceCapCacheDirty) {
        QDBusMessage m = QDBusMessage::createMethodCall(QString::fromLatin1(dbusServiceName),
                                                        QString::fromLatin1(dbusPath),
                                                        QString::fromLatin1(dbusInterfaceName),
                                                        QStringLiteral("GetCapabilities"));

        QDBusConnection::sessionBus().callWithCallback(m,
                                                       q,
                                                       SLOT(onServerCapabilitiesReceived(QStringList)),
                                                       nullptr,
                                                       -1);
    }
}
