/*
    This file is part of the KDE project
    SPDX-FileCopyrightText: 2002, 2003 Dawit Alemayehu <adawit@kde.org>
    SPDX-FileCopyrightText: 1999 Simon Hausmann <hausmann@kde.org>
    SPDX-FileCopyrightText: 1999 Yves Arrouye <yves@realnames.com>

    Advanced web shortcuts
    SPDX-FileCopyrightText: 2001 Andreas Hochsteger <e9625392@student.tuwien.ac.at>

    SPDX-License-Identifier: GPL-2.0-or-later
*/

#ifndef KURIIKWSFILTERENG_H
#define KURIIKWSFILTERENG_H

#include "kuriikwsfiltereng_private_export.h"
#include "searchproviderregistry_p.h"
#include <QMap>
#include <QObject>
#include <QStringList>
#include <QUrl>

class QStringEncoder;
class SearchProvider;

namespace KIO
{
class KURIIKWSFILTERENG_PRIVATE_EXPORT KURISearchFilterEngine : public QObject
{
    Q_OBJECT
public:
    typedef QMap<QString, QString> SubstMap;

    QByteArray name() const;
    char keywordDelimiter() const;
    QString defaultSearchEngine() const;
    QStringList favoriteEngineList() const;
    SearchProvider *webShortcutQuery(const QString &typedString, QString &searchTerm) const;
    SearchProvider *autoWebSearchQuery(const QString &typedString, const QString &defaultShortcut = QString()) const;
    QUrl formatResult(const QString &url, const QString &cset1, const QString &cset2, const QString &query, bool isMalformed) const;

    SearchProviderRegistry *registry();

    static KURISearchFilterEngine *self();
    static QStringList defaultSearchProviders();

protected:
    QUrl formatResult(const QString &url, const QString &cset1, const QString &cset2, const QString &query, bool isMalformed, SubstMap &map) const;

private:
    KURISearchFilterEngine();
    ~KURISearchFilterEngine();

    Q_SLOT void configure();
    Q_DISABLE_COPY_MOVE(KURISearchFilterEngine)

    QStringList modifySubstitutionMap(SubstMap &map, const QString &query) const;
    QString substituteQuery(const QString &url, SubstMap &map, const QString &userquery, QStringEncoder &codec) const;

    SearchProviderRegistry m_registry;
    QString m_defaultWebShortcut;
    QStringList m_preferredWebShortcuts;
    bool m_bWebShortcutsEnabled;
    bool m_bUseOnlyPreferredWebShortcuts;
    char m_cKeywordDelimiter;
    bool m_reloadRegistry = false; // We would load it twice otherwise
};
}

#endif // KURIIKWSFILTERENG_H
