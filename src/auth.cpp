/*
 * This file is part of buteo-sync-plugin-carddav package
 *
 * Copyright (C) 2014 Jolla Ltd. and/or its subsidiary(-ies).
 *
 * Contributors: Chris Adams <chris.adams@jolla.com>
 *
 * This program/library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * version 2.1 as published by the Free Software Foundation.
 *
 * This program/library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program/library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include "auth_p.h"
#include <sailfishkeyprovider.h>
#include <QtDebug>

namespace {
    QString skp_storedKey(const QString &provider, const QString &service, const QString &key)
    {
        QString retn;
        char *value = NULL;
        int success = SailfishKeyProvider_storedKey(provider.toLatin1(), service.toLatin1(), key.toLatin1(), &value);
        if (value) {
            if (success == 0) {
                retn = QString::fromLatin1(value);
            }
            free(value);
        }
        return retn;
    }
}

Auth::Auth(QObject *parent)
    : QObject(parent)
    , m_account(0)
    , m_ident(0)
    , m_session(0)
{
}

Auth::~Auth()
{
    delete m_account;
    if (m_ident && m_session) {
        m_ident->destroySession(m_session);
    }
    delete m_ident;
}

void Auth::signIn(int accountId)
{
    m_account = m_manager.account(accountId);
    if (!m_account) {
        qWarning() << Q_FUNC_INFO << "unable to load account" << accountId;
        emit signInError();
        return;
    }

    // determine which service to sign in with.
    Accounts::Service srv;
    Accounts::ServiceList services = m_account->services();
    Q_FOREACH (const Accounts::Service &s, services) {
        if (s.serviceType().toLower() == QStringLiteral("carddav")) {
            srv = s;
            break;
        }
    }

    if (!srv.isValid()) {
        qWarning() << Q_FUNC_INFO << "unable to find carddav service for account" << accountId;
        emit signInError();
        return;
    }

    // determine the remote server URL from the account settings, and then sign in.
    m_account->selectService(srv);
    m_serverUrl = m_account->value("CardDAVServerUrl").toString(); // TODO: use "Remote database" for consistency?
    if (m_serverUrl.isEmpty()) {
        qWarning() << Q_FUNC_INFO << "no valid server url setting in account" << accountId;
        emit signInError();
        return;
    }

    m_ident = m_account->credentialsId() > 0 ? SignOn::Identity::existingIdentity(m_account->credentialsId()) : 0;
    if (!m_ident) {
        qWarning() << Q_FUNC_INFO << "no valid credentials for account" << accountId;
        emit signInError();
        return;
    }

    Accounts::AccountService accSrv(m_account, srv);
    QString method = accSrv.authData().method();
    QString mechanism = accSrv.authData().mechanism();
    SignOn::AuthSession *session = m_ident->createSession(method);
    if (!session) {
        qWarning() << Q_FUNC_INFO << "unable to create authentication session with account" << accountId;
        emit signInError();
        return;
    }

    QString providerName = m_account->providerName();
    QString clientId = skp_storedKey(providerName, QString(), QStringLiteral("client_id"));
    QString clientSecret = skp_storedKey(providerName, QString(), QStringLiteral("client_secret"));
    QString consumerKey = skp_storedKey(providerName, QString(), QStringLiteral("consumer_key"));
    QString consumerSecret = skp_storedKey(providerName, QString(), QStringLiteral("consumer_secret"));

    QVariantMap signonSessionData = accSrv.authData().parameters();
    signonSessionData.insert("UiPolicy", SignOn::NoUserInteractionPolicy);
    if (!clientId.isEmpty()) signonSessionData.insert("ClientId", clientId);
    if (!clientSecret.isEmpty()) signonSessionData.insert("ClientSecret", clientSecret);
    if (!consumerKey.isEmpty()) signonSessionData.insert("ConsumerKey", consumerKey);
    if (!consumerSecret.isEmpty()) signonSessionData.insert("ConsumerSecret", consumerSecret);

    connect(session, SIGNAL(response(SignOn::SessionData)),
            this, SLOT(signOnResponse(SignOn::SessionData)),
            Qt::UniqueConnection);
    connect(session, SIGNAL(error(SignOn::Error)),
            this, SLOT(signOnError(SignOn::Error)),
            Qt::UniqueConnection);

    session->setProperty("accountId", accountId);
    session->setProperty("mechanism", mechanism);
    session->setProperty("signonSessionData", signonSessionData);
    session->process(SignOn::SessionData(signonSessionData), mechanism);
}

void Auth::signOnResponse(const SignOn::SessionData &response)
{
    QString username, password, accessToken;
    Q_FOREACH (const QString &key, response.propertyNames()) {
        if (key.toLower() == QStringLiteral("username")) {
            username = response.getProperty(key).toString();
        } else if (key.toLower() == QStringLiteral("secret")) {
            password = response.getProperty(key).toString();
        } else if (key.toLower() == QStringLiteral("password")) {
            password = response.getProperty(key).toString();
        } else if (key.toLower() == QStringLiteral("accesstoken")) {
            accessToken = response.getProperty(key).toString();
        }
    }

    // we need both username+password, OR accessToken.
    if (!accessToken.isEmpty()) {
        emit signInCompleted(m_serverUrl, QString(), QString(), accessToken);
    } else if (!username.isEmpty() && !password.isEmpty()) {
        emit signInCompleted(m_serverUrl, username, password, QString());
    } else {
        qWarning() << Q_FUNC_INFO << "authentication succeeded, but couldn't find valid credentials";
        emit signInError();
    }
}

void Auth::signOnError(const SignOn::Error &error)
{
    qWarning() << Q_FUNC_INFO << "authentication error:" << error.type() << ":" << error.message();
    emit signInError();
    return;
}
