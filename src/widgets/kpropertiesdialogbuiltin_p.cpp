/*
    This file is part of the KDE project
    SPDX-FileCopyrightText: 1998, 1999 Torben Weis <weis@kde.org>
    SPDX-FileCopyrightText: 1999, 2000 Preston Brown <pbrown@kde.org>
    SPDX-FileCopyrightText: 2000 Simon Hausmann <hausmann@kde.org>
    SPDX-FileCopyrightText: 2000 David Faure <faure@kde.org>

    SPDX-License-Identifier: LGPL-2.0-or-later
*/
#include "kpropertiesdialogbuiltin_p.h"

#include "../utils_p.h"
#include "config-kiowidgets.h"
#include "kfileitem.h"
#include "kio_widgets_debug.h"

#include <gpudetection_p.h>
#include <kbuildsycocaprogressdialog.h>
#include <kdirnotify.h>
#include <kfileitemlistproperties.h>
#include <kio/chmodjob.h>
#include <kio/copyjob.h>
#include <kio/desktopexecparser.h>
#include <kio/directorysizejob.h>
#include <kio/jobuidelegate.h>
#include <kio/renamedialog.h>
#include <kio/statjob.h>
#include <kmountpoint.h>
#include <kprotocolinfo.h>
#include <kprotocolmanager.h>
#include <kurlrequester.h>

#include <KApplicationTrader>
#include <KAuthorized>
#include <KCapacityBar>
#include <KColorScheme>
#include <KCompletion>
#include <KConfigGroup>
#include <KDesktopFile>
#include <KDialogJobUiDelegate>
#include <KIO/ApplicationLauncherJob>
#include <KIO/FileSystemFreeSpaceJob>
#include <KIO/OpenFileManagerWindowJob>
#include <KIconButton>
#include <KJobUiDelegate>
#include <KJobWidgets>
#include <KLazyLocalizedString>
#include <KLineEdit>
#include <KMessageBox>
#include <KMessageWidget>
#include <KMimeTypeChooser>
#include <KMimeTypeEditor>
#include <KSeparator>
#include <KService>
#include <KSharedConfig>
#include <KShell>
#include <KSqueezedTextLabel>
#include <KSycoca>

#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>

#ifdef WITH_QTDBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusReply>
#endif

#include <QDialogButtonBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QLabel>
#include <QLayout>
#include <QLocale>
#include <QMimeDatabase>
#include <QProgressBar>
#include <QPushButton>
#include <QSizePolicy>
#include <QStyle>
#include <QtConcurrentRun>
#include <QTextDocumentFragment>

#include "ui_checksumswidget.h"
#include "ui_kfilepropspluginwidget.h"
#include "ui_kpropertiesdesktopadvbase.h"
#include "ui_kpropertiesdesktopbase.h"

#if HAVE_POSIX_ACL
#include "kacleditwidget.h"
#endif
#include <cerrno>
extern "C" {
#if HAVE_SYS_XATTR_H
#include <sys/xattr.h>
#endif
#if HAVE_SYS_EXTATTR_H
#include <sys/extattr.h>
#endif
#if HAVE_SYS_MOUNT_H
#include <sys/mount.h>
#endif
}

using namespace KDEPrivate;

static QString couldNotSaveMsg(const QString &path)
{
    return xi18nc("@info", "Could not save properties due to insufficient write access to:<nl/><filename>%1</filename>.", path);
}
static QString nameFromFileName(QString nameStr)
{
    if (nameStr.endsWith(QLatin1String(".desktop"))) {
        nameStr.chop(8);
    }
    // Make it human-readable (%2F => '/', ...)
    nameStr = KIO::decodeFileName(nameStr);
    return nameStr;
}

class KFilePropsPlugin::KFilePropsPluginPrivate
{
public:
    KFilePropsPluginPrivate()
        : m_ui(new Ui_KFilePropsPluginWidget())
    {
        m_ui->setupUi(&m_mainWidget);
    }

    ~KFilePropsPluginPrivate()
    {
        if (dirSizeJob) {
            dirSizeJob->kill();
        }
    }

    void hideMountPointLabels()
    {
        m_ui->fsLabel_Left->hide();
        m_ui->fsLabel->hide();

        m_ui->mountPointLabel_Left->hide();
        m_ui->mountPointLabel->hide();

        m_ui->mountSrcLabel_Left->hide();
        m_ui->mountSrcLabel->hide();
    }

    QWidget m_mainWidget;
    std::unique_ptr<Ui_KFilePropsPluginWidget> m_ui;
    KIO::DirectorySizeJob *dirSizeJob = nullptr;
    QTimer *dirSizeUpdateTimer = nullptr;
    bool bMultiple;
    bool bIconChanged;
    bool bKDesktopMode;
    bool bDesktopFile;
    QString mimeType;
    QString oldFileName;

    QString m_sRelativePath;
    bool m_bFromTemplate;

    /*
     * The initial filename
     */
    QString oldName;
};

KFilePropsPlugin::KFilePropsPlugin(KPropertiesDialog *_props)
    : KPropertiesDialogPlugin(_props)
    , d(new KFilePropsPluginPrivate)
{
    const auto itemsList = properties->items();
    d->bMultiple = (itemsList.count() > 1);
    d->bIconChanged = false;
    d->bDesktopFile = KDesktopPropsPlugin::supports(itemsList);
    // qDebug() << "KFilePropsPlugin::KFilePropsPlugin bMultiple=" << d->bMultiple;

    // We set this data from the first item, and we'll
    // check that the other items match against it, resetting when not.
    const KFileItem firstItem = properties->item();
    auto [url, isLocal] = firstItem.isMostLocalUrl();
    bool isReallyLocal = firstItem.url().isLocalFile();
    KMountPoint::Ptr mp;
    if (isLocal || isReallyLocal) {
        mp = KMountPoint::currentMountPoints(KMountPoint::DetailsNeededFlag::NeedMountOptions).findByPath(url.toLocalFile());
        if (!mp) {
            qCWarning(KIO_WIDGETS) << "Could not find mount point for" << url;
        }
    }
    bool bDesktopFile = firstItem.isDesktopFile();
    mode_t mode = firstItem.mode();
    bool hasDirs = firstItem.isDir() && !firstItem.isLink();
    bool hasRoot = url.path() == QLatin1String("/");
    bool hasAccessTime =
        mp ? !(mp->mountOptions().contains(QLatin1String("noatime")) || (hasDirs && mp->mountOptions().contains(QLatin1String("nodiratime")))) : true;
    QString iconStr = firstItem.iconName();
    QString directory = properties->url().adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).path();
    QString protocol = properties->url().scheme();
    d->bKDesktopMode = protocol == QLatin1String("desktop") || properties->currentDir().scheme() == QLatin1String("desktop");
    QString mimeComment = firstItem.mimeComment();
    d->mimeType = firstItem.mimetype();
    KIO::filesize_t totalSize = firstItem.size();
    QString magicMimeName;
    QString magicMimeComment;
    QMimeDatabase db;
    if (isLocal) {
        QMimeType magicMimeType = db.mimeTypeForFile(url.toLocalFile(), QMimeDatabase::MatchContent);
        if (magicMimeType.isValid() && !magicMimeType.isDefault()) {
            magicMimeName = magicMimeType.name();
            magicMimeComment = magicMimeType.comment();
        }
    }
#ifdef Q_OS_WIN
    if (isReallyLocal) {
        directory = QDir::toNativeSeparators(directory.mid(1));
    }
#endif

    // Those things only apply to 'single file' mode
    QString filename;
    bool isTrash = false;
    d->m_bFromTemplate = false;

    // And those only to 'multiple' mode
    uint iDirCount = hasDirs ? 1 : 0;
    uint iFileCount = 1 - iDirCount;

    properties->addPage(&d->m_mainWidget, i18nc("@title:tab File properties", "&General"));

    d->m_ui->symlinkTargetMessageWidget->hide();

    if (!d->bMultiple) {
        QString path;
        if (!d->m_bFromTemplate) {
            isTrash = (properties->url().scheme() == QLatin1String("trash"));
            // Extract the full name, but without file: for local files
            path = properties->url().toDisplayString(QUrl::PreferLocalFile);
        } else {
            path = Utils::concatPaths(properties->currentDir().path(), properties->defaultName());
            directory = properties->currentDir().toDisplayString(QUrl::PreferLocalFile);
        }

        if (d->bDesktopFile) {
            determineRelativePath(path);
        }

        // Extract the file name only
        filename = properties->defaultName();
        if (filename.isEmpty()) { // no template
            const QFileInfo finfo(firstItem.name()); // this gives support for UDS_NAME, e.g. for kio_trash or kio_system
            filename = finfo.fileName(); // Make sure only the file's name is displayed (#160964).
        } else {
            d->m_bFromTemplate = true;
            setDirty(); // to enforce that the copy happens
        }
        d->oldFileName = filename;

        // Make it human-readable
        filename = nameFromFileName(filename);
        d->oldName = filename;
    } else {
        // Multiple items: see what they have in common
        for (const auto &item : itemsList) {
            if (item == firstItem) {
                continue;
            }

            const QUrl url = item.url();
            // qDebug() << "KFilePropsPlugin::KFilePropsPlugin " << url.toDisplayString();
            // The list of things we check here should match the variables defined
            // at the beginning of this method.
            if (url.isLocalFile() != isLocal) {
                isLocal = false; // not all local
            }
            if (bDesktopFile && item.isDesktopFile() != bDesktopFile) {
                bDesktopFile = false; // not all desktop files
            }
            if (item.mode() != mode) {
                mode = static_cast<mode_t>(0);
            }
            if (KIO::iconNameForUrl(url) != iconStr) {
                iconStr = QStringLiteral("document-multiple");
            }
            if (url.adjusted(QUrl::RemoveFilename | QUrl::StripTrailingSlash).path() != directory) {
                directory.clear();
            }
            if (url.scheme() != protocol) {
                protocol.clear();
            }
            if (!mimeComment.isNull() && item.mimeComment() != mimeComment) {
                mimeComment.clear();
            }
            if (isLocal && !magicMimeComment.isNull()) {
                QMimeType magicMimeType = db.mimeTypeForFile(url.toLocalFile(), QMimeDatabase::MatchContent);
                if (magicMimeType.isValid() && magicMimeType.comment() != magicMimeComment) {
                    magicMimeName.clear();
                    magicMimeComment.clear();
                }
            }

            if (isLocal && url.path() == QLatin1String("/")) {
                hasRoot = true;
            }
            if (item.isDir() && !item.isLink()) {
                iDirCount++;
                hasDirs = true;
            } else {
                iFileCount++;
                totalSize += item.size();
            }
        }
    }

    if (!isReallyLocal && !protocol.isEmpty()) {
        directory += QLatin1String(" (") + protocol + QLatin1Char(')');
    }

    if (!isTrash //
        && (bDesktopFile || Utils::isDirMask(mode)) //
        && !d->bMultiple // not implemented for multiple
        && enableIconButton()) {
        d->m_ui->iconLabel->hide();

        const int bsize = 66 + (2 * d->m_ui->iconButton->style()->pixelMetric(QStyle::PM_ButtonMargin));
        d->m_ui->iconButton->setFixedSize(bsize, bsize);
        d->m_ui->iconButton->setIconSize(48);
        if (bDesktopFile && isLocal) {
            const KDesktopFile config(url.toLocalFile());
            if (config.hasDeviceType()) {
                d->m_ui->iconButton->setIconType(KIconLoader::Desktop, KIconLoader::Device);
            } else {
                d->m_ui->iconButton->setIconType(KIconLoader::Desktop, KIconLoader::Application);
            }
        } else {
            d->m_ui->iconButton->setIconType(KIconLoader::Desktop, KIconLoader::Place);
        }

        d->m_ui->iconButton->setIcon(iconStr);
        connect(d->m_ui->iconButton, &KIconButton::iconChanged, this, &KFilePropsPlugin::slotIconChanged);
    } else {
        d->m_ui->iconButton->hide();

        const int bsize = 66 + (2 * d->m_ui->iconLabel->style()->pixelMetric(QStyle::PM_ButtonMargin));
        d->m_ui->iconLabel->setFixedSize(bsize, bsize);
        d->m_ui->iconLabel->setPixmap(QIcon::fromTheme(iconStr, QIcon::fromTheme(QStringLiteral("unknown"))).pixmap(48));
    }

    KFileItemListProperties itemList(KFileItemList{firstItem});
    if (d->bMultiple || isTrash || hasRoot || !(d->m_bFromTemplate || itemList.supportsMoving())) {
        d->m_ui->fileNameLineEdit->hide();
        setFileNameReadOnly(true);
        if (d->bMultiple) {
            d->m_ui->fileNameLabel->setText(KIO::itemsSummaryString(iFileCount + iDirCount, iFileCount, iDirCount, 0, false));
        }
    } else {
        d->m_ui->fileNameLabel->hide();

        d->m_ui->fileNameLineEdit->setText(filename);
        connect(d->m_ui->fileNameLineEdit, &QLineEdit::textChanged, this, &KFilePropsPlugin::nameFileChanged);
    }
    d->m_ui->fileNameLineEdit->setPlaceholderText(hasDirs ? i18nc("@info:placeholder", "Enter folder name") : i18nc("@info:placeholder", "Enter file name"));

    // Mimetype widgets
    if (!mimeComment.isEmpty() && !isTrash) {
        d->m_ui->mimeCommentLabel->setText(mimeComment);
        d->m_ui->mimeCommentLabel->setToolTip(d->mimeType);

        const int hSpacing = properties->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing);
        d->m_ui->defaultHandlerLayout->setSpacing(hSpacing);

#ifndef Q_OS_WIN
        updateDefaultHandler(d->mimeType);
        connect(KSycoca::self(), &KSycoca::databaseChanged, this, [this] {
            updateDefaultHandler(d->mimeType);
        });

        connect(d->m_ui->configureMimeBtn, &QAbstractButton::clicked, this, &KFilePropsPlugin::slotEditFileType);
#endif

    } else {
        d->m_ui->typeLabel->hide();
        d->m_ui->mimeCommentLabel->hide();
        d->m_ui->configureMimeBtn->hide();

        d->m_ui->defaultHandlerLabel_Left->hide();
        d->m_ui->defaultHandlerIcon->hide();
        d->m_ui->defaultHandlerLabel->hide();
    }

#ifdef Q_OS_WIN
    d->m_ui->defaultHandlerLabel_Left->hide();
    d->m_ui->defaultHandlerIcon->hide();
    d->m_ui->defaultHandlerLabel->hide();
#endif

    if (!magicMimeComment.isEmpty() && magicMimeComment != mimeComment) {
        d->m_ui->magicMimeCommentLabel->setText(magicMimeComment);
        d->m_ui->magicMimeCommentLabel->setToolTip(magicMimeName);
    } else {
        d->m_ui->contentLabel->hide();
        d->m_ui->magicMimeCommentLabel->hide();
    }

    d->m_ui->configureMimeBtn->setVisible(KAuthorized::authorizeAction(QStringLiteral("editfiletype")) && !d->m_ui->defaultHandlerLabel->isHidden());

    // Location:
    if (!directory.isEmpty()) {
        d->m_ui->locationLabel->setText(directory);

        // Layout direction for this label is always LTR; but if we are in RTL mode,
        // align the text to the right, otherwise the text is on the wrong side of the dialog
        if (properties->layoutDirection() == Qt::RightToLeft) {
            d->m_ui->locationLabel->setAlignment(Qt::AlignRight);
        }
    }

    // Size widgets
    if (!hasDirs) { // Only files [and symlinks]
        d->m_ui->sizeLabel->setText(QStringLiteral("%1 (%2)").arg(KIO::convertSize(totalSize), QLocale().toString(totalSize)));
        d->m_ui->sizeBtnWidget->hide();
    } else { // Directory
        connect(d->m_ui->calculateSizeBtn, &QAbstractButton::clicked, this, &KFilePropsPlugin::slotSizeDetermine);
        connect(d->m_ui->stopCalculateSizeBtn, &QAbstractButton::clicked, this, &KFilePropsPlugin::slotSizeStop);

        if (auto filelight = KService::serviceByDesktopName(QStringLiteral("org.kde.filelight"))) {
            d->m_ui->sizeDetailsBtn->setText(i18nc("@action:button", "Explore in %1", filelight->name()));
            d->m_ui->sizeDetailsBtn->setIcon(QIcon::fromTheme(filelight->icon()));
            connect(d->m_ui->sizeDetailsBtn, &QPushButton::clicked, this, &KFilePropsPlugin::slotSizeDetails);
        } else {
            d->m_ui->sizeDetailsBtn->hide();
        }

        //         sizelay->addStretch(10); // so that the buttons don't grow horizontally

        // auto-launch for local dirs only, and not for '/'
        if (isLocal && !hasRoot) {
            d->m_ui->calculateSizeBtn->setText(i18n("Refresh"));
            slotSizeDetermine();
        } else {
            d->m_ui->stopCalculateSizeBtn->setEnabled(false);
        }
    }

    // Symlink widgets
    if (!d->bMultiple && firstItem.isLink()) {
        d->m_ui->symlinkTargetEdit->setPlaceholderText(i18nc("@info:placeholder", "Enter target location"));
        d->m_ui->symlinkTargetEdit->setText(firstItem.linkDest());
        connect(d->m_ui->symlinkTargetEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
            setDirty();
            d->m_ui->symlinkTargetOpenDir->setToolTip(xi18nc("@info:tooltip Go to path %1", "Show <filename>%1</filename>", text));
            d->m_ui->symlinkTargetOpenDir->setAccessibleDescription(QTextDocumentFragment::fromHtml(d->m_ui->symlinkTargetOpenDir->toolTip()).toPlainText());
        });
        d->m_ui->symlinkTargetOpenDir->setToolTip(xi18nc("@info:tooltip Go to path %1", "Show <filename>%1</filename>", firstItem.linkDest()));
        d->m_ui->symlinkTargetOpenDir->setAccessibleDescription(QTextDocumentFragment::fromHtml(d->m_ui->symlinkTargetOpenDir->toolTip()).toPlainText());

        connect(d->m_ui->symlinkTargetOpenDir, &QPushButton::clicked, this, [this] {
            // The most local URL is needed to resolve symlinks in virtual locations like "desktop:/"
            const QUrl url = properties->item().isMostLocalUrl().url;
            const QUrl resolvedTargetLocation = url.resolved(QUrl(d->m_ui->symlinkTargetEdit->text()));

            KIO::StatJob *statJob = KIO::stat(resolvedTargetLocation, KIO::StatJob::SourceSide, KIO::StatNoDetails, KIO::HideProgressInfo);
            connect(statJob, &KJob::finished, this, [this, statJob] {
                if (statJob->error()) {
                    d->m_ui->symlinkTargetMessageWidget->setText(statJob->errorString());
                    d->m_ui->symlinkTargetMessageWidget->animatedShow();
                    return;
                }

                KIO::highlightInFileManager({statJob->url()});
                properties->close();
            });
        });
    } else {
        d->m_ui->symlinkTargetLabel->hide();
        d->m_ui->symlinkTargetEdit->hide();
        d->m_ui->symlinkTargetOpenDir->hide();
    }

    // Time widgets
    if (!d->bMultiple) {
        QLocale locale;
        if (const QDateTime dt = firstItem.time(KFileItem::CreationTime); !dt.isNull()) {
            d->m_ui->createdTimeLabel->setText(locale.toString(dt, QLocale::LongFormat));
        } else {
            d->m_ui->createdTimeLabel->hide();
            d->m_ui->createdTimeLabel_Left->hide();
        }

        if (const QDateTime dt = firstItem.time(KFileItem::ModificationTime); !dt.isNull()) {
            d->m_ui->modifiedTimeLabel->setText(locale.toString(dt, QLocale::LongFormat));
        } else {
            d->m_ui->modifiedTimeLabel->hide();
            d->m_ui->modifiedTimeLabel_Left->hide();
        }

        if (const QDateTime dt = firstItem.time(KFileItem::AccessTime); !dt.isNull() && hasAccessTime) {
            d->m_ui->accessTimeLabel->setText(locale.toString(dt, QLocale::LongFormat));
        } else {
            d->m_ui->accessTimeLabel->hide();
            d->m_ui->accessTimeLabel_Left->hide();
        }
    } else {
        d->m_ui->createdTimeLabel->hide();
        d->m_ui->createdTimeLabel_Left->hide();
        d->m_ui->modifiedTimeLabel->hide();
        d->m_ui->modifiedTimeLabel_Left->hide();
        d->m_ui->accessTimeLabel->hide();
        d->m_ui->accessTimeLabel_Left->hide();
    }

    // File system and mount point widgets
    if (hasDirs) { // only for directories
        if (isLocal) {
            if (mp) {
                d->m_ui->fsLabel->setText(mp->mountType());
                d->m_ui->mountPointLabel->setText(mp->mountPoint());
                d->m_ui->mountSrcLabel->setText(mp->mountedFrom());
            } else {
                d->hideMountPointLabels();
            }
        } else {
            d->hideMountPointLabels();
        }

        KIO::FileSystemFreeSpaceJob *job = KIO::fileSystemFreeSpace(url);
        connect(job, &KJob::result, this, &KFilePropsPlugin::slotFreeSpaceResult);
    } else {
        d->m_ui->fsSeparator->hide();
        d->m_ui->freespaceLabel->hide();
        d->m_ui->capacityBar->hide();
        d->hideMountPointLabels();
    }

    // UDSEntry extra fields
    // To determine extra fields, use the original URL, not the mostLocalUrl.
    // e.g. trash:/foo will point to file:/...local/share/Trash/files/foo and therefore not have any fields.
    if (const auto extraFields = KProtocolInfo::extraFields(firstItem.url()); !d->bMultiple && !extraFields.isEmpty()) {
        int curRow = d->m_ui->gridLayout->rowCount();
        KSeparator *sep = new KSeparator(Qt::Horizontal, &d->m_mainWidget);
        d->m_ui->gridLayout->addWidget(sep, curRow++, 0, 1, 3);

        QLocale locale;
        for (int i = 0; i < extraFields.count(); ++i) {
            const auto &field = extraFields.at(i);

            QString text = firstItem.entry().stringValue(KIO::UDSEntry::UDS_EXTRA + i);
            if (field.type == KProtocolInfo::ExtraField::Invalid || text.isEmpty()) {
                continue;
            }

            if (field.type == KProtocolInfo::ExtraField::DateTime) {
                const QDateTime date = QDateTime::fromString(text, Qt::ISODate);
                if (!date.isValid()) {
                    continue;
                }

                text = locale.toString(date, QLocale::LongFormat);
            }

            auto *label = new QLabel(i18n("%1:", field.name), &d->m_mainWidget);
            d->m_ui->gridLayout->addWidget(label, curRow, 0, Qt::AlignRight);

            auto *squeezedLabel = new KSqueezedTextLabel(text, &d->m_mainWidget);
            if (properties->layoutDirection() == Qt::RightToLeft) {
                squeezedLabel->setAlignment(Qt::AlignRight);
            } else {
                squeezedLabel->setLayoutDirection(Qt::LeftToRight);
            }

            d->m_ui->gridLayout->addWidget(squeezedLabel, curRow++, 1);
            squeezedLabel->setTextInteractionFlags(Qt::TextBrowserInteraction | Qt::TextSelectableByKeyboard);
            label->setBuddy(squeezedLabel);
        }
    }
}

bool KFilePropsPlugin::enableIconButton() const
{
    const KFileItem item = properties->item();

    // desktop files are special, files in /usr/share/applications can be
    // edited by overlaying them in .local/share/applications
    // https://bugs.kde.org/show_bug.cgi?id=429613
    if (item.isDesktopFile()) {
        return true;
    }

    // If the current item is a directory, check if it's writable,
    // so we can create/update a .directory
    // Current item is a file, same thing: check if it is writable
    if (item.isWritable()) {
        // exclude remote dirs as changing the icon has no effect (bug 205954)
        if (item.isLocalFile() || item.url().scheme() == QLatin1String("desktop")) {
            return true;
        }
    }

    return false;
}

void KFilePropsPlugin::setFileNameReadOnly(bool readOnly)
{
    Q_ASSERT(readOnly); // false isn't supported

    if (readOnly) {
        Q_ASSERT(!d->m_bFromTemplate);

        d->m_ui->fileNameLineEdit->hide();

        d->m_ui->fileNameLabel->show();
        d->m_ui->fileNameLabel->setText(d->oldName); // will get overwritten if d->bMultiple
    }
}

void KFilePropsPlugin::slotEditFileType()
{
    QString mime;
    if (d->mimeType == QLatin1String("application/octet-stream")) {
        const int pos = d->oldFileName.lastIndexOf(QLatin1Char('.'));
        if (pos != -1) {
            mime = QLatin1Char('*') + QStringView(d->oldFileName).mid(pos);
        } else {
            mime = QStringLiteral("*");
        }
    } else {
        mime = d->mimeType;
    }
    KMimeTypeEditor::editMimeType(mime, properties->window());
}

void KFilePropsPlugin::slotIconChanged()
{
    d->bIconChanged = true;
    Q_EMIT changed();
}

void KFilePropsPlugin::nameFileChanged(const QString &text)
{
    properties->buttonBox()->button(QDialogButtonBox::Ok)->setEnabled(!text.isEmpty());
    Q_EMIT changed();
}

static QString relativeAppsLocation(const QString &file)
{
    // Don't resolve symlinks, so that editing /usr/share/applications/foo.desktop that is
    // a symlink works
    const QString absolute = QFileInfo(file).absoluteFilePath();
    const QStringList dirs = QStandardPaths::standardLocations(QStandardPaths::ApplicationsLocation);
    for (const QString &base : dirs) {
        QDir base_dir = QDir(base);
        if (base_dir.exists() && absolute.startsWith(base_dir.canonicalPath())) {
            return absolute.mid(base.length() + 1);
        }
    }
    return QString(); // return empty if the file is not in apps
}

void KFilePropsPlugin::determineRelativePath(const QString &path)
{
    // now let's make it relative
    d->m_sRelativePath = relativeAppsLocation(path);
}

void KFilePropsPlugin::slotFreeSpaceResult(KJob *_job)
{
    const auto job = qobject_cast<KIO::FileSystemFreeSpaceJob *>(_job);
    Q_ASSERT(job);
    if (!job->error()) {
        const qint64 size = job->size();
        const qint64 available = job->availableSize();
        const quint64 used = size - available;
        const int percentUsed = (size == 0) ? 0 : qRound(100.0 * qreal(used) / qreal(size));

        d->m_ui->capacityBar->setText(i18nc("Available space out of total partition size (percent used)",
                                            "%1 free of %2 (%3% used)",
                                            KIO::convertSize(available),
                                            KIO::convertSize(size),
                                            percentUsed));

        d->m_ui->capacityBar->setValue(percentUsed);
    } else {
        d->m_ui->capacityBar->setText(i18nc("@info:status", "Unknown size"));
        d->m_ui->capacityBar->setValue(0);
    }
}

void KFilePropsPlugin::slotDirSizeUpdate()
{
    KIO::filesize_t totalSize = d->dirSizeJob->totalSize();
    KIO::filesize_t totalFiles = d->dirSizeJob->totalFiles();
    KIO::filesize_t totalSubdirs = d->dirSizeJob->totalSubdirs();
    d->m_ui->sizeLabel->setText(i18n("Calculating... %1 (%2)\n%3, %4",
                                     KIO::convertSize(totalSize),
                                     QLocale().toString(totalSize),
                                     i18np("1 file", "%1 files", totalFiles),
                                     i18np("1 sub-folder", "%1 sub-folders", totalSubdirs)));
}

void KFilePropsPlugin::slotDirSizeFinished(KJob *job)
{
    if (job->error()) {
        d->m_ui->sizeLabel->setText(job->errorString());
    } else {
        KIO::filesize_t totalSize = d->dirSizeJob->totalSize();
        KIO::filesize_t totalFiles = d->dirSizeJob->totalFiles();
        KIO::filesize_t totalSubdirs = d->dirSizeJob->totalSubdirs();
        d->m_ui->sizeLabel->setText(QStringLiteral("%1 (%2)\n%3, %4")
                                        .arg(KIO::convertSize(totalSize),
                                             QLocale().toString(totalSize),
                                             i18np("1 file", "%1 files", totalFiles),
                                             i18np("1 sub-folder", "%1 sub-folders", totalSubdirs)));
    }
    d->m_ui->stopCalculateSizeBtn->setEnabled(false);
    // just in case you change something and try again :)
    d->m_ui->calculateSizeBtn->setText(i18n("Refresh"));
    d->m_ui->calculateSizeBtn->setEnabled(true);
    d->dirSizeJob = nullptr;
    delete d->dirSizeUpdateTimer;
    d->dirSizeUpdateTimer = nullptr;
}

void KFilePropsPlugin::slotSizeDetermine()
{
    d->m_ui->sizeLabel->setText(i18n("Calculating...\n"));
    // qDebug() << "properties->item()=" << properties->item() << "URL=" << properties->item().url();

    d->dirSizeJob = KIO::directorySize(properties->items());
    d->dirSizeUpdateTimer = new QTimer(this);
    connect(d->dirSizeUpdateTimer, &QTimer::timeout, this, &KFilePropsPlugin::slotDirSizeUpdate);
    d->dirSizeUpdateTimer->start(500);
    connect(d->dirSizeJob, &KJob::result, this, &KFilePropsPlugin::slotDirSizeFinished);
    d->m_ui->stopCalculateSizeBtn->setEnabled(true);
    d->m_ui->calculateSizeBtn->setEnabled(false);

    // also update the "Free disk space" display
    if (!d->m_ui->capacityBar->isHidden()) {
        const KFileItem item = properties->item();
        KIO::FileSystemFreeSpaceJob *job = KIO::fileSystemFreeSpace(item.url());
        connect(job, &KJob::result, this, &KFilePropsPlugin::slotFreeSpaceResult);
    }
}

void KFilePropsPlugin::slotSizeStop()
{
    if (d->dirSizeJob) {
        KIO::filesize_t totalSize = d->dirSizeJob->totalSize();
        d->m_ui->sizeLabel->setText(i18n("At least %1\n", KIO::convertSize(totalSize)));
        d->dirSizeJob->kill();
        d->dirSizeJob = nullptr;
    }
    if (d->dirSizeUpdateTimer) {
        d->dirSizeUpdateTimer->stop();
    }

    d->m_ui->stopCalculateSizeBtn->setEnabled(false);
    d->m_ui->calculateSizeBtn->setEnabled(true);
}

void KFilePropsPlugin::slotSizeDetails()
{
    // Open the current folder in filelight
    KService::Ptr service = KService::serviceByDesktopName(QStringLiteral("org.kde.filelight"));
    if (service) {
        auto *job = new KIO::ApplicationLauncherJob(service);
        job->setUrls({properties->url()});
        job->setUiDelegate(new KDialogJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, properties));
        job->start();
    }
}

KFilePropsPlugin::~KFilePropsPlugin() = default;

bool KFilePropsPlugin::supports(const KFileItemList & /*_items*/)
{
    return true;
}

void KFilePropsPlugin::applyChanges()
{
    if (d->dirSizeJob) {
        slotSizeStop();
    }

    // qDebug() << "KFilePropsPlugin::applyChanges";

    if (!d->m_ui->fileNameLineEdit->isHidden()) {
        QString n = d->m_ui->fileNameLineEdit->text();
        // Remove trailing spaces (#4345)
        while (!n.isEmpty() && n[n.length() - 1].isSpace()) {
            n.chop(1);
        }
        if (n.isEmpty()) {
            KMessageBox::error(properties, i18n("The new file name is empty."));
            properties->abortApplying();
            return;
        }

        // Do we need to rename the file ?
        // qDebug() << "oldname = " << d->oldName;
        // qDebug() << "newname = " << n;
        if (d->oldName != n || d->m_bFromTemplate) { // true for any from-template file
            KIO::CopyJob *job = nullptr;
            QUrl oldurl = properties->url();

            QString newFileName = KIO::encodeFileName(n);
            if (d->bDesktopFile && !newFileName.endsWith(QLatin1String(".desktop"))) {
                newFileName += QLatin1String(".desktop");
            }

            // Tell properties. Warning, this changes the result of properties->url() !
            properties->rename(newFileName);

            // Update also relative path (for apps)
            if (!d->m_sRelativePath.isEmpty()) {
                determineRelativePath(properties->url().toLocalFile());
            }

            // qDebug() << "New URL = " << properties->url();
            // qDebug() << "old = " << oldurl.url();

            // Don't remove the template !!
            if (!d->m_bFromTemplate) { // (normal renaming)
                job = KIO::moveAs(oldurl, properties->url());
            } else { // Copying a template
                job = KIO::copyAs(oldurl, properties->url());
            }
            KJobWidgets::setWindow(job, properties);
            connect(job, &KJob::result, this, &KFilePropsPlugin::slotCopyFinished);
            connect(job, &KIO::CopyJob::renamed, this, &KFilePropsPlugin::slotFileRenamed);
            return;
        }

        properties->updateUrl(properties->url());
        // Update also relative path (for apps)
        if (!d->m_sRelativePath.isEmpty()) {
            determineRelativePath(properties->url().toLocalFile());
        }
    }

    // No job, keep going
    slotCopyFinished(nullptr);
}

void KFilePropsPlugin::slotCopyFinished(KJob *job)
{
    // qDebug() << "KFilePropsPlugin::slotCopyFinished";
    if (job) {
        if (job->error()) {
            job->uiDelegate()->showErrorMessage();
            // Didn't work. Revert the URL to the old one
            properties->updateUrl(static_cast<KIO::CopyJob *>(job)->srcUrls().constFirst());
            properties->abortApplying(); // Don't apply the changes to the wrong file !
            return;
        }
    }

    Q_ASSERT(!properties->item().isNull());
    Q_ASSERT(!properties->item().url().isEmpty());

    // Save the file locally
    if (d->bDesktopFile && !d->m_sRelativePath.isEmpty()) {
        // qDebug() << "KFilePropsPlugin::slotCopyFinished " << d->m_sRelativePath;
        const QString newPath = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation) + QLatin1Char('/') + d->m_sRelativePath;
        const QUrl newURL = QUrl::fromLocalFile(newPath);
        // qDebug() << "KFilePropsPlugin::slotCopyFinished path=" << newURL;
        properties->updateUrl(newURL);
    }

    if (d->bKDesktopMode && d->bDesktopFile) {
        // Renamed? Update Name field
        // Note: The desktop KIO worker does this as well, but not when
        //       the file is copied from a template.
        if (d->m_bFromTemplate) {
            KIO::StatJob *job = KIO::stat(properties->url());
            job->exec();
            KIO::UDSEntry entry = job->statResult();

            KFileItem item(entry, properties->url());
            KDesktopFile config(item.localPath());
            KConfigGroup cg = config.desktopGroup();
            QString nameStr = nameFromFileName(properties->url().fileName());
            cg.writeEntry("Name", nameStr);
            cg.writeEntry("Name", nameStr, KConfigGroup::Persistent | KConfigGroup::Localized);
        }
    }

    if (!d->m_ui->symlinkTargetEdit->isHidden() && !d->bMultiple) {
        const KFileItem item = properties->item();
        const QString newTarget = d->m_ui->symlinkTargetEdit->text();
        if (newTarget != item.linkDest()) {
            // qDebug() << "Updating target of symlink to" << newTarget;
            KIO::Job *job = KIO::symlink(newTarget, item.url(), KIO::Overwrite);
            job->uiDelegate()->setAutoErrorHandlingEnabled(true);
            job->exec();
        }
    }

    // "Link to Application" templates need to be made executable
    // Instead of matching against a filename we check if the destination
    // is an Application now.
    if (d->m_bFromTemplate) {
        // destination is not necessarily local, use the src template
        KDesktopFile templateResult(static_cast<KIO::CopyJob *>(job)->srcUrls().constFirst().toLocalFile());
        if (templateResult.hasApplicationType()) {
            // We can either stat the file and add the +x bit or use the larger chmod() job
            // with a umask designed to only touch u+x.  This is only one KIO job, so let's
            // do that.

            KFileItem appLink(properties->item());
            KFileItemList fileItemList;
            fileItemList << appLink;

            // first 0100 adds u+x, second 0100 only allows chmod to change u+x
            KIO::Job *chmodJob = KIO::chmod(fileItemList, 0100, 0100, QString(), QString(), KIO::HideProgressInfo);
            chmodJob->exec();
        }
    }

    setDirty(false);
    Q_EMIT changesApplied();
}

void KFilePropsPlugin::applyIconChanges()
{
    if (d->m_ui->iconButton->isHidden() || !d->bIconChanged) {
        return;
    }
    // handle icon changes - only local files (or pseudo-local) for now
    // TODO: Use KTempFile and KIO::file_copy with overwrite = true
    QUrl url = properties->url();
    KIO::StatJob *job = KIO::mostLocalUrl(url);
    KJobWidgets::setWindow(job, properties);
    job->exec();
    url = job->mostLocalUrl();

    if (url.isLocalFile()) {
        QString path;

        if (Utils::isDirMask(properties->item().mode())) {
            path = url.toLocalFile() + QLatin1String("/.directory");
            // don't call updateUrl because the other tabs (i.e. permissions)
            // apply to the directory, not the .directory file.
        } else {
            path = url.toLocalFile();
        }

        // Get the default image
        QMimeDatabase db;
        const QString str = db.mimeTypeForFile(url.toLocalFile(), QMimeDatabase::MatchExtension).iconName();
        // Is it another one than the default ?
        QString sIcon;
        if (const QString currIcon = d->m_ui->iconButton->icon(); str != currIcon) {
            sIcon = currIcon;
        }
        // (otherwise write empty value)

        // qDebug() << "**" << path << "**";

        // If default icon and no .directory file -> don't create one
        if (!sIcon.isEmpty() || QFile::exists(path)) {
            KDesktopFile cfg(path);
            // qDebug() << "sIcon = " << (sIcon);
            // qDebug() << "str = " << (str);
            cfg.desktopGroup().writeEntry("Icon", sIcon);
            cfg.sync();

            cfg.reparseConfiguration();
            if (cfg.desktopGroup().readEntry("Icon") != sIcon) {
                properties->abortApplying();

                KMessageBox::error(nullptr, couldNotSaveMsg(path));
            }
        }
    }
}

void KFilePropsPlugin::updateDefaultHandler(const QString &mimeType)
{
    const bool isGeneric = d->mimeType == QLatin1String("application/octet-stream");

    const auto service = KApplicationTrader::preferredService(mimeType);
    if (!isGeneric && service) {
        const int iconSize = properties->style()->pixelMetric(QStyle::PM_SmallIconSize);
        d->m_ui->defaultHandlerIcon->setPixmap(QIcon::fromTheme(service->icon()).pixmap(iconSize));
        d->m_ui->defaultHandlerIcon->show();
        d->m_ui->defaultHandlerLabel->setText(service->name());
        d->m_ui->defaultHandlerLabel->setDisabled(false);
    } else {
        d->m_ui->defaultHandlerIcon->hide();
        if (isGeneric) {
            d->m_ui->defaultHandlerLabel->setText(i18n("No registered file type"));
        } else {
            d->m_ui->defaultHandlerLabel->setText(i18n("No associated application"));
        }
        d->m_ui->defaultHandlerLabel->setDisabled(true);
    }

    if (isGeneric) {
        d->m_ui->configureMimeBtn->setText(i18nc("@action:button Create new file type", "Create…"));
        d->m_ui->configureMimeBtn->setIcon(QIcon::fromTheme(QStringLiteral("document-new")));
        d->m_ui->configureMimeBtn->setToolTip(QString());
    } else {
        d->m_ui->configureMimeBtn->setText(i18nc("@action:button", "Change…"));
        d->m_ui->configureMimeBtn->setIcon(QIcon::fromTheme(QStringLiteral("configure")));
        d->m_ui->configureMimeBtn->setToolTip(i18nc("@info:tooltip", "Change the default application for opening files of type “%1”.", d->mimeType));
    }
}

void KFilePropsPlugin::slotFileRenamed(KIO::Job *, const QUrl &, const QUrl &newUrl)
{
    // This is called in case of an existing local file during the copy/move operation,
    // if the user chooses Rename.
    properties->updateUrl(newUrl);
}

void KFilePropsPlugin::postApplyChanges()
{
    // Save the icon only after applying the permissions changes (#46192)
    applyIconChanges();

#ifdef WITH_QTDBUS
    const KFileItemList items = properties->items();
    const QList<QUrl> lst = items.urlList();
    org::kde::KDirNotify::emitFilesChanged(QList<QUrl>(lst));
#endif
}

class KFilePermissionsPropsPlugin::KFilePermissionsPropsPluginPrivate
{
public:
    QFrame *m_frame = nullptr;
    QCheckBox *cbRecursive = nullptr;
    QLabel *explanationLabel = nullptr;
    QComboBox *ownerPermCombo = nullptr;
    QComboBox *groupPermCombo = nullptr;
    QComboBox *othersPermCombo = nullptr;
    QCheckBox *extraCheckbox = nullptr;
    mode_t partialPermissions;
    KFilePermissionsPropsPlugin::PermissionsMode pmode;
    bool canChangePermissions;
    bool isIrregular;
    bool hasExtendedACL;
    KACL extendedACL;
    KACL defaultACL;
    bool fileSystemSupportsACLs;

    QComboBox *grpCombo = nullptr;

    KLineEdit *usrEdit = nullptr;
    KLineEdit *grpEdit = nullptr;

    // Old permissions
    mode_t permissions;
    // Old group
    QString strGroup;
    // Old owner
    QString strOwner;
};

static constexpr mode_t UniOwner{S_IRUSR | S_IWUSR | S_IXUSR};
static constexpr mode_t UniGroup{S_IRGRP | S_IWGRP | S_IXGRP};
static constexpr mode_t UniOthers{S_IROTH | S_IWOTH | S_IXOTH};
static constexpr mode_t UniRead{S_IRUSR | S_IRGRP | S_IROTH};
static constexpr mode_t UniWrite{S_IWUSR | S_IWGRP | S_IWOTH};
static constexpr mode_t UniExec{S_IXUSR | S_IXGRP | S_IXOTH};
static constexpr mode_t UniSpecial{S_ISUID | S_ISGID | S_ISVTX};
static constexpr mode_t s_invalid_mode_t{static_cast<mode_t>(-1)};

// synced with PermissionsTarget
constexpr mode_t KFilePermissionsPropsPlugin::permissionsMasks[3] = {UniOwner, UniGroup, UniOthers};
constexpr mode_t KFilePermissionsPropsPlugin::standardPermissions[4] = {0, UniRead, UniRead | UniWrite, s_invalid_mode_t};

// synced with PermissionsMode and standardPermissions
static constexpr KLazyLocalizedString permissionsTexts[4][4] = {
    {kli18n("No Access"), kli18n("Can Only View"), kli18n("Can View & Modify"), {}},
    {kli18n("No Access"), kli18n("Can Only View Content"), kli18n("Can View & Modify Content"), {}},
    {{}, {}, {}, {}}, // no texts for links
    {kli18n("No Access"), kli18n("Can Only View/Read Content"), kli18n("Can View/Read & Modify/Write"), {}}};

KFilePermissionsPropsPlugin::KFilePermissionsPropsPlugin(KPropertiesDialog *_props)
    : KPropertiesDialogPlugin(_props)
    , d(new KFilePermissionsPropsPluginPrivate)
{
    const auto &[localUrl, isLocal] = properties->item().isMostLocalUrl();
    bool isTrash = (properties->url().scheme() == QLatin1String("trash"));
    KUser myself(KUser::UseEffectiveUID);
    const bool IamRoot = myself.isSuperUser();

    const KFileItem firstItem = properties->item();
    bool isLink = firstItem.isLink();
    bool isDir = firstItem.isDir(); // all dirs
    bool hasDir = firstItem.isDir(); // at least one dir
    d->permissions = firstItem.permissions(); // common permissions to all files
    d->partialPermissions = d->permissions; // permissions that only some files have (at first we take everything)
    d->isIrregular = isIrregular(d->permissions, isDir, isLink);
    d->strOwner = firstItem.user();
    d->strGroup = firstItem.group();
    d->hasExtendedACL = firstItem.ACL().isExtended() || firstItem.defaultACL().isValid();
    d->extendedACL = firstItem.ACL();
    d->defaultACL = firstItem.defaultACL();
    d->fileSystemSupportsACLs = false;

    if (properties->items().count() > 1) {
        // Multiple items: see what they have in common
        const KFileItemList items = properties->items();
        for (const auto &item : items) {
            if (item == firstItem) { // No need to check the first one again
                continue;
            }

            const bool isItemDir = item.isDir();
            const bool isItemLink = item.isLink();

            if (!d->isIrregular) {
                d->isIrregular |= isIrregular(item.permissions(), isItemDir == isDir, isItemLink == isLink);
            }

            d->hasExtendedACL = d->hasExtendedACL || item.hasExtendedACL();

            if (isItemLink != isLink) {
                isLink = false;
            }

            if (isItemDir != isDir) {
                isDir = false;
            }
            hasDir |= isItemDir;

            if (item.permissions() != d->permissions) {
                d->permissions &= item.permissions();
                d->partialPermissions |= item.permissions();
            }

            if (item.user() != d->strOwner) {
                d->strOwner.clear();
            }

            if (item.group() != d->strGroup) {
                d->strGroup.clear();
            }
        }
    }

    if (isLink) {
        d->pmode = PermissionsOnlyLinks;
    } else if (isDir) {
        d->pmode = PermissionsOnlyDirs;
    } else if (hasDir) {
        d->pmode = PermissionsMixed;
    } else {
        d->pmode = PermissionsOnlyFiles;
    }

    // keep only what's not in the common permissions
    d->partialPermissions = d->partialPermissions & ~d->permissions;

    bool isMyFile = false;

    if (isLocal && !d->strOwner.isEmpty()) { // local files, and all owned by the same person
        if (myself.isValid()) {
            isMyFile = (d->strOwner == myself.loginName());
        } else {
            qCWarning(KIO_WIDGETS) << "I don't exist ?! geteuid=" << KUserId::currentEffectiveUserId().toString();
        }
    } else {
        // We don't know, for remote files, if they are ours or not.
        // So we let the user change permissions, and
        // KIO::chmod will tell, if he had no right to do it.
        isMyFile = true;
    }

    d->canChangePermissions = (isMyFile || IamRoot) && (!isLink);

    // create GUI

    d->m_frame = new QFrame();
    properties->addPage(d->m_frame, i18n("&Permissions"));

    QBoxLayout *box = new QVBoxLayout(d->m_frame);

    QWidget *l;
    QString lbl;
    QGroupBox *gb;
    QFormLayout *gl;
    QPushButton *pbAdvancedPerm = nullptr;

    /* Group: Access Permissions */
    gb = new QGroupBox(i18n("Access Permissions"), d->m_frame);
    gb->setFlat(true);
    box->addWidget(gb);

    gl = new QFormLayout(gb);
    gl->setFormAlignment(Qt::AlignHCenter);

    l = d->explanationLabel = new QLabel(gb);
    if (isLink) {
        d->explanationLabel->setText(
            i18np("This file is a link and does not have permissions.", "All files are links and do not have permissions.", properties->items().count()));
    } else if (!d->canChangePermissions) {
        d->explanationLabel->setText(i18n("Only the owner can change permissions."));
    } else {
        d->explanationLabel->setFixedHeight(0);
    }
    gl->addWidget(l);

    l = d->ownerPermCombo = new QComboBox(gb);
    gl->addRow(i18n("O&wner:"), l);
    connect(d->ownerPermCombo, &QComboBox::activated, this, &KPropertiesDialogPlugin::changed);
    l->setWhatsThis(i18n("Specifies the actions that the owner is allowed to do."));

    l = d->groupPermCombo = new QComboBox(gb);
    gl->addRow(i18n("Gro&up:"), l);
    connect(d->groupPermCombo, &QComboBox::activated, this, &KPropertiesDialogPlugin::changed);
    l->setWhatsThis(i18n("Specifies the actions that the members of the group are allowed to do."));

    l = d->othersPermCombo = new QComboBox(gb);
    gl->addRow(i18n("O&thers:"), l);
    connect(d->othersPermCombo, &QComboBox::activated, this, &KPropertiesDialogPlugin::changed);
    l->setWhatsThis(
        i18n("Specifies the actions that all users, who are neither "
             "owner nor in the group, are allowed to do."));

    if (!isLink) {
        l = d->extraCheckbox = new QCheckBox(hasDir ? i18n("Only own&er can delete or rename contents") : i18n("Allow &executing file as program"), gb);
        connect(d->extraCheckbox, &QAbstractButton::clicked, this, &KPropertiesDialogPlugin::changed);
        gl->addRow(hasDir ? i18n("Delete or rename:") : i18n("Execute:"), l);
        l->setWhatsThis(hasDir ? i18n("Enable this option to allow only the folder's owner to "
                                      "delete or rename the contained files and folders. Other "
                                      "users can only add new files, which requires the 'Modify "
                                      "Content' permission.")
                               : i18n("Enable this option to mark the file as executable. This only makes "
                                      "sense for programs and scripts. It is required when you want to "
                                      "execute them."));

        pbAdvancedPerm = new QPushButton(i18n("A&dvanced Permissions"), gb);
        gl->addWidget(pbAdvancedPerm);
        connect(pbAdvancedPerm, &QAbstractButton::clicked, this, &KFilePermissionsPropsPlugin::slotShowAdvancedPermissions);
    } else {
        d->extraCheckbox = nullptr;
    }

    /**** Group: Ownership ****/
    gb = new QGroupBox(i18n("Ownership"), d->m_frame);
    gb->setFlat(true);
    box->addWidget(gb);

    gl = new QFormLayout(gb);
    gl->setFormAlignment(Qt::AlignHCenter);

    /*** Set Owner ***/
    lbl = i18n("User:");
    /* GJ: Don't autocomplete more than 1000 users. This is a kind of random
     * value. Huge sites having 10.000+ user have a fair chance of using NIS,
     * (possibly) making this unacceptably slow.
     * OTOH, it is nice to offer this functionality for the standard user.
     */
    int maxEntries = 1000;

    /* File owner: For root, offer a KLineEdit with autocompletion.
     * For a user, who can never chown() a file, offer a QLabel.
     */
    if (IamRoot && isLocal) {
        d->usrEdit = new KLineEdit(gb);
        KCompletion *kcom = d->usrEdit->completionObject();
        kcom->setOrder(KCompletion::Sorted);
        QStringList userNames = KUser::allUserNames(maxEntries);
        kcom->setItems(userNames);
        d->usrEdit->setCompletionMode((userNames.size() < maxEntries) ? KCompletion::CompletionAuto : KCompletion::CompletionNone);
        d->usrEdit->setText(d->strOwner);
        gl->addRow(lbl, d->usrEdit);
        connect(d->usrEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    } else {
        l = new QLabel(d->strOwner, gb);
        qobject_cast<QLabel *>(l)->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        l->setFocusPolicy(Qt::TabFocus);
        gl->addRow(lbl, l);
    }

    /*** Set Group ***/
    QStringList groupList = myself.groupNames();
    const bool isMyGroup = groupList.contains(d->strGroup);

    /* add the group the file currently belongs to ..
     * .. if it is not there already
     */
    if (!isMyGroup) {
        groupList += d->strGroup;
    }
    lbl = i18n("Group:");

    /* Set group: if possible to change:
     * - Offer a KLineEdit for root, since he can change to any group.
     * - Offer a QComboBox for a normal user, since he can change to a fixed
     *   (small) set of groups only.
     * If not changeable: offer a QLabel.
     */
    if (IamRoot && isLocal) {
        d->grpEdit = new KLineEdit(gb);
        KCompletion *kcom = new KCompletion;
        kcom->setItems(groupList);
        d->grpEdit->setCompletionObject(kcom, true);
        d->grpEdit->setAutoDeleteCompletionObject(true);
        d->grpEdit->setCompletionMode(KCompletion::CompletionAuto);
        d->grpEdit->setText(d->strGroup);
        gl->addRow(lbl, d->grpEdit);
        connect(d->grpEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    } else if ((groupList.count() > 1) && isMyFile && isLocal) {
        d->grpCombo = new QComboBox(gb);
        d->grpCombo->setObjectName(QStringLiteral("combogrouplist"));
        d->grpCombo->addItems(groupList);
        d->grpCombo->setCurrentIndex(groupList.indexOf(d->strGroup));
        gl->addRow(lbl, d->grpCombo);
        connect(d->grpCombo, &QComboBox::activated, this, &KPropertiesDialogPlugin::changed);
    } else {
        l = new QLabel(d->strGroup, gb);
        qobject_cast<QLabel *>(l)->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        l->setFocusPolicy(Qt::TabFocus);
        gl->addRow(lbl, l);
    }

    // "Apply recursive" checkbox
    if (hasDir && !isLink && !isTrash) {
        d->cbRecursive = new QCheckBox(i18n("Apply changes to all subfolders and their contents"), d->m_frame);
        connect(d->cbRecursive, &QAbstractButton::clicked, this, &KPropertiesDialogPlugin::changed);
        KSeparator *sep = new KSeparator();
        box->addWidget(sep);
        box->addWidget(d->cbRecursive);
    }

    updateAccessControls();

    if (isTrash) {
        // don't allow to change properties for file into trash
        enableAccessControls(false);
        if (pbAdvancedPerm) {
            pbAdvancedPerm->setEnabled(false);
        }
    }
}

#if HAVE_POSIX_ACL
static bool fileSystemSupportsACL(const QByteArray &path)
{
    bool fileSystemSupportsACLs = false;
#ifdef Q_OS_FREEBSD
    // FIXME: unbreak and enable this
    // Maybe use pathconf(2) to perform this check?
    // struct statfs buf;
    // fileSystemSupportsACLs = (statfs(path.data(), &buf) == 0) && (buf.f_flags & MNT_ACLS);
    Q_UNUSED(path);
#elif defined Q_OS_MACOS
    fileSystemSupportsACLs = getxattr(path.data(), "system.posix_acl_access", nullptr, 0, 0, XATTR_NOFOLLOW) >= 0 || errno == ENODATA;
#else
    fileSystemSupportsACLs = getxattr(path.data(), "system.posix_acl_access", nullptr, 0) >= 0 || errno == ENODATA;
#endif
    return fileSystemSupportsACLs;
}
#endif

void KFilePermissionsPropsPlugin::slotShowAdvancedPermissions()
{
    bool isDir = (d->pmode == PermissionsOnlyDirs) || (d->pmode == PermissionsMixed);
    QDialog dlg(properties);
    dlg.setWindowModality(Qt::WindowModal);
    dlg.setModal(true);
    dlg.setWindowTitle(i18n("Advanced Permissions"));

    QLabel *l;
    QLabel *cl[3];
    QGroupBox *gb;
    QGridLayout *gl;

    auto *vbox = new QVBoxLayout(&dlg);

    // Group: Access Permissions
    gb = new QGroupBox(i18n("Access Permissions"), &dlg);
    gb->setFlat(true);
    vbox->addWidget(gb);
    vbox->setAlignment(Qt::AlignHCenter);

    gl = new QGridLayout(gb);
    gl->addItem(new QSpacerItem(0, 10), 0, 0);

    QList<QWidget *> theNotSpecials;

    l = new QLabel(i18n("Class"), gb);
    gl->addWidget(l, 1, 0);
    theNotSpecials.append(l);

    QString readWhatsThis;
    QString readLabel;
    if (isDir) {
        readLabel = i18n("Show\nEntries");
        readWhatsThis = i18n("This flag allows viewing the content of the folder.");
    } else {
        readLabel = i18n("Read");
        readWhatsThis = i18n("The Read flag allows viewing the content of the file.");
    }

    QString writeWhatsThis;
    QString writeLabel;
    if (isDir) {
        writeLabel = i18n("Write\nEntries");
        writeWhatsThis = i18n(
            "This flag allows adding, renaming and deleting of files. "
            "Note that deleting and renaming can be limited using the Sticky flag.");
    } else {
        writeLabel = i18n("Write");
        writeWhatsThis = i18n("The Write flag allows modifying the content of the file.");
    }

    QString execLabel;
    QString execWhatsThis;
    if (isDir) {
        execLabel = i18nc("Enter folder", "Enter");
        execWhatsThis = i18n("Enable this flag to allow entering the folder.");
    } else {
        execLabel = i18n("Exec");
        execWhatsThis = i18n("Enable this flag to allow executing the file as a program.");
    }
    // GJ: Add space between normal and special modes
    QSize size = l->sizeHint();
    size.setWidth(size.width() + 15);
    l->setFixedSize(size);
    gl->addWidget(l, 1, 3);

    l = new QLabel(i18n("Special"), gb);
    gl->addWidget(l, 1, 4, 1, 1);
    QString specialWhatsThis;
    if (isDir) {
        specialWhatsThis = i18n(
            "Special flag. Valid for the whole folder, the exact "
            "meaning of the flag can be seen in the right hand column.");
    } else {
        specialWhatsThis = i18n(
            "Special flag. The exact meaning of the flag can be seen "
            "in the right hand column.");
    }
    l->setWhatsThis(specialWhatsThis);

    cl[0] = new QLabel(i18n("User"), gb);
    gl->addWidget(cl[0], 2, 0);
    theNotSpecials.append(cl[0]);

    cl[1] = new QLabel(i18n("Group"), gb);
    gl->addWidget(cl[1], 3, 0);
    theNotSpecials.append(cl[1]);

    cl[2] = new QLabel(i18n("Others"), gb);
    gl->addWidget(cl[2], 4, 0);
    theNotSpecials.append(cl[2]);

    QString setUidWhatsThis;
    if (isDir) {
        setUidWhatsThis = i18n(
            "If this flag is set, the owner of this folder will be "
            "the owner of all new files.");
    } else {
        setUidWhatsThis = i18n(
            "If this file is an executable and the flag is set, it will "
            "be executed with the permissions of the owner.");
    }

    QString setGidWhatsThis;
    if (isDir) {
        setGidWhatsThis = i18n(
            "If this flag is set, the group of this folder will be "
            "set for all new files.");
    } else {
        setGidWhatsThis = i18n(
            "If this file is an executable and the flag is set, it will "
            "be executed with the permissions of the group.");
    }

    QString stickyWhatsThis;
    if (isDir) {
        stickyWhatsThis = i18n(
            "If the Sticky flag is set on a folder, only the owner "
            "and root can delete or rename files. Otherwise everybody "
            "with write permissions can do this.");
    } else {
        stickyWhatsThis = i18n(
            "The Sticky flag on a file is ignored on Linux, but may "
            "be used on some systems");
    }
    mode_t aPermissions = 0;
    mode_t aPartialPermissions = 0;
    mode_t dummy1 = 0;
    mode_t dummy2 = 0;

    if (!d->isIrregular) {
        switch (d->pmode) {
        case PermissionsOnlyFiles:
            getPermissionMasks(aPartialPermissions, dummy1, aPermissions, dummy2);
            break;
        case PermissionsOnlyDirs:
        case PermissionsMixed:
            getPermissionMasks(dummy1, aPartialPermissions, dummy2, aPermissions);
            break;
        case PermissionsOnlyLinks:
            aPermissions = UniRead | UniWrite | UniExec | UniSpecial;
            break;
        }
    } else {
        aPermissions = d->permissions;
        aPartialPermissions = d->partialPermissions;
    }

    // Draw Checkboxes
    QCheckBox *cba[3][4];
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            auto *cb = new QCheckBox(gb);
            if (col != 3) {
                theNotSpecials.append(cb);
            }
            cba[row][col] = cb;
            cb->setChecked(aPermissions & fperm[row][col]);
            if (aPartialPermissions & fperm[row][col]) {
                cb->setTristate();
                cb->setCheckState(Qt::PartiallyChecked);
            } else if (d->cbRecursive && d->cbRecursive->isChecked()) {
                cb->setTristate();
            }

            cb->setEnabled(d->canChangePermissions);
            gl->addWidget(cb, row + 2, col + 1);
            switch (col) {
            case 0:
                cb->setText(readLabel);
                cb->setWhatsThis(readWhatsThis);
                break;
            case 1:
                cb->setText(writeLabel);
                cb->setWhatsThis(writeWhatsThis);
                break;
            case 2:
                cb->setText(execLabel);
                cb->setWhatsThis(execWhatsThis);
                break;
            case 3:
                switch (row) {
                case 0:
                    cb->setText(i18n("Set UID"));
                    cb->setWhatsThis(setUidWhatsThis);
                    break;
                case 1:
                    cb->setText(i18n("Set GID"));
                    cb->setWhatsThis(setGidWhatsThis);
                    break;
                case 2:
                    cb->setText(i18nc("File permission", "Sticky"));
                    cb->setWhatsThis(stickyWhatsThis);
                    break;
                }
                break;
            }
        }
    }
    gl->setColumnStretch(6, 10);
    vbox->addStretch(1);

#if HAVE_POSIX_ACL
    KACLEditWidget *extendedACLs = nullptr;

    // FIXME make it work with partial entries
    if (properties->items().count() == 1) {
        QByteArray path = QFile::encodeName(properties->item().url().toLocalFile());
        d->fileSystemSupportsACLs = fileSystemSupportsACL(path);
    }
    if (d->fileSystemSupportsACLs) {
        std::for_each(theNotSpecials.begin(), theNotSpecials.end(), std::mem_fn(&QWidget::hide));
        extendedACLs = new KACLEditWidget(&dlg);
        extendedACLs->setEnabled(d->canChangePermissions);
        vbox->addWidget(extendedACLs);
        if (d->extendedACL.isValid() && d->extendedACL.isExtended()) {
            extendedACLs->setACL(d->extendedACL);
        } else {
            extendedACLs->setACL(KACL(aPermissions));
        }

        if (d->defaultACL.isValid()) {
            extendedACLs->setDefaultACL(d->defaultACL);
        }

        if (properties->items().constFirst().isDir()) {
            extendedACLs->setAllowDefaults(true);
        }
    }
#endif
    // Separator above buttons
    auto *buttonSep = new KSeparator();
    vbox->addWidget(buttonSep);

    auto *buttonBox = new QDialogButtonBox(&dlg);
    buttonBox->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    vbox->addWidget(buttonBox);

    if (dlg.exec() != QDialog::Accepted) {
        return;
    }

    mode_t andPermissions = mode_t(~0);
    mode_t orPermissions = 0;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 4; ++col) {
            switch (cba[row][col]->checkState()) {
            case Qt::Checked:
                orPermissions |= fperm[row][col];
            // fall through
            case Qt::Unchecked:
                andPermissions &= ~fperm[row][col];
                break;
            case Qt::PartiallyChecked:
                break;
            }
        }
    }

    const KFileItemList items = properties->items();
    d->isIrregular = std::any_of(items.cbegin(), items.cend(), [this, andPermissions, orPermissions](const KFileItem &item) {
        return isIrregular((item.permissions() & andPermissions) | orPermissions, item.isDir(), item.isLink());
    });

    d->permissions = orPermissions;
    d->partialPermissions = andPermissions;

#if HAVE_POSIX_ACL
    // override with the acls, if present
    if (extendedACLs) {
        d->extendedACL = extendedACLs->getACL();
        d->defaultACL = extendedACLs->getDefaultACL();
        d->hasExtendedACL = d->extendedACL.isExtended() || d->defaultACL.isValid();
        d->permissions = d->extendedACL.basePermissions();
        d->permissions |= (andPermissions | orPermissions) & (S_ISUID | S_ISGID | S_ISVTX);
    }
#endif

    dlg.setMinimumSize(dlg.sizeHint());

    updateAccessControls();
    Q_EMIT changed();
}

KFilePermissionsPropsPlugin::~KFilePermissionsPropsPlugin() = default;

bool KFilePermissionsPropsPlugin::supports(const KFileItemList &items)
{
    return std::any_of(items.cbegin(), items.cend(), [](const KFileItem &item) {
        return KProtocolManager::supportsPermissions(item.url());
    });
}

// sets a combo box in the Access Control frame
void KFilePermissionsPropsPlugin::setComboContent(QComboBox *combo, PermissionsTarget target, mode_t permissions, mode_t partial)
{
    combo->clear();
    if (d->isIrregular) { // #176876
        return;
    }

    if (d->pmode == PermissionsOnlyLinks) {
        combo->addItem(i18n("Link"));
        combo->setCurrentIndex(0);
        return;
    }

    mode_t tMask = permissionsMasks[target];
    int textIndex;
    for (textIndex = 0; standardPermissions[textIndex] != s_invalid_mode_t; ++textIndex) {
        if ((standardPermissions[textIndex] & tMask) == (permissions & tMask & (UniRead | UniWrite))) {
            break;
        }
    }
    Q_ASSERT(standardPermissions[textIndex] != s_invalid_mode_t); // must not happen, would be irreglar

    const auto permsTexts = permissionsTexts[static_cast<int>(d->pmode)];
    for (int i = 0; !permsTexts[i].isEmpty(); ++i) {
        combo->addItem(permsTexts[i].toString());
    }

    if (partial & tMask & ~UniExec) {
        combo->addItem(i18n("Varying (No Change)"));
        combo->setCurrentIndex(3);
    } else {
        combo->setCurrentIndex(textIndex);
    }
}

// permissions are irregular if they can't be displayed in a combo box.
bool KFilePermissionsPropsPlugin::isIrregular(mode_t permissions, bool isDir, bool isLink)
{
    if (isLink) { // links are always ok
        return false;
    }

    mode_t p = permissions;
    if (p & (S_ISUID | S_ISGID)) { // setuid/setgid -> irregular
        return true;
    }
    if (isDir) {
        p &= ~S_ISVTX; // ignore sticky on dirs

        // check supported flag combinations
        mode_t p0 = p & UniOwner;
        if ((p0 != 0) && (p0 != (S_IRUSR | S_IXUSR)) && (p0 != UniOwner)) {
            return true;
        }
        p0 = p & UniGroup;
        if ((p0 != 0) && (p0 != (S_IRGRP | S_IXGRP)) && (p0 != UniGroup)) {
            return true;
        }
        p0 = p & UniOthers;
        if ((p0 != 0) && (p0 != (S_IROTH | S_IXOTH)) && (p0 != UniOthers)) {
            return true;
        }
        return false;
    }
    if (p & S_ISVTX) { // sticky on file -> irregular
        return true;
    }

    // check supported flag combinations
    mode_t p0 = p & UniOwner;
    bool usrXPossible = !p0; // true if this file could be an executable
    if (p0 & S_IXUSR) {
        if ((p0 == S_IXUSR) || (p0 == (S_IWUSR | S_IXUSR))) {
            return true;
        }
        usrXPossible = true;
    } else if (p0 == S_IWUSR) {
        return true;
    }

    p0 = p & UniGroup;
    bool grpXPossible = !p0; // true if this file could be an executable
    if (p0 & S_IXGRP) {
        if ((p0 == S_IXGRP) || (p0 == (S_IWGRP | S_IXGRP))) {
            return true;
        }
        grpXPossible = true;
    } else if (p0 == S_IWGRP) {
        return true;
    }
    if (p0 == 0) {
        grpXPossible = true;
    }

    p0 = p & UniOthers;
    bool othXPossible = !p0; // true if this file could be an executable
    if (p0 & S_IXOTH) {
        if ((p0 == S_IXOTH) || (p0 == (S_IWOTH | S_IXOTH))) {
            return true;
        }
        othXPossible = true;
    } else if (p0 == S_IWOTH) {
        return true;
    }

    // check that there either all targets are executable-compatible, or none
    return (p & UniExec) && !(usrXPossible && grpXPossible && othXPossible);
}

// enables/disabled the widgets in the Access Control frame
void KFilePermissionsPropsPlugin::enableAccessControls(bool enable)
{
    d->ownerPermCombo->setEnabled(enable);
    d->groupPermCombo->setEnabled(enable);
    d->othersPermCombo->setEnabled(enable);
    if (d->extraCheckbox) {
        d->extraCheckbox->setEnabled(enable);
    }
    if (d->cbRecursive) {
        d->cbRecursive->setEnabled(enable);
    }
}

// updates all widgets in the Access Control frame
void KFilePermissionsPropsPlugin::updateAccessControls()
{
    setComboContent(d->ownerPermCombo, PermissionsOwner, d->permissions, d->partialPermissions);
    setComboContent(d->groupPermCombo, PermissionsGroup, d->permissions, d->partialPermissions);
    setComboContent(d->othersPermCombo, PermissionsOthers, d->permissions, d->partialPermissions);

    switch (d->pmode) {
    case PermissionsOnlyLinks:
        enableAccessControls(false);
        break;
    case PermissionsOnlyFiles:
        enableAccessControls(d->canChangePermissions && !d->isIrregular && !d->hasExtendedACL);
        if (d->canChangePermissions) {
            d->explanationLabel->setText(
                d->isIrregular || d->hasExtendedACL
                    ? i18np("This file uses advanced permissions", "These files use advanced permissions.", properties->items().count())
                    : QString());
        }
        if (d->partialPermissions & UniExec) {
            d->extraCheckbox->setTristate();
            d->extraCheckbox->setCheckState(Qt::PartiallyChecked);
        } else {
            d->extraCheckbox->setTristate(false);
            d->extraCheckbox->setChecked(d->permissions & UniExec);
        }
        break;
    case PermissionsOnlyDirs:
        enableAccessControls(d->canChangePermissions && !d->isIrregular && !d->hasExtendedACL);
        // if this is a dir, and we can change permissions, don't dis-allow
        // recursive, we can do that for ACL setting.
        if (d->cbRecursive) {
            d->cbRecursive->setEnabled(d->canChangePermissions && !d->isIrregular);
        }

        if (d->canChangePermissions) {
            d->explanationLabel->setText(
                d->isIrregular || d->hasExtendedACL
                    ? i18np("This folder uses advanced permissions.", "These folders use advanced permissions.", properties->items().count())
                    : QString());
        }
        if (d->partialPermissions & S_ISVTX) {
            d->extraCheckbox->setTristate();
            d->extraCheckbox->setCheckState(Qt::PartiallyChecked);
        } else {
            d->extraCheckbox->setTristate(false);
            d->extraCheckbox->setChecked(d->permissions & S_ISVTX);
        }
        break;
    case PermissionsMixed:
        enableAccessControls(d->canChangePermissions && !d->isIrregular && !d->hasExtendedACL);
        if (d->canChangePermissions) {
            d->explanationLabel->setText(d->isIrregular || d->hasExtendedACL ? i18n("These files use advanced permissions.") : QString());
        }
        if (d->partialPermissions & S_ISVTX) {
            d->extraCheckbox->setTristate();
            d->extraCheckbox->setCheckState(Qt::PartiallyChecked);
        } else {
            d->extraCheckbox->setTristate(false);
            d->extraCheckbox->setChecked(d->permissions & S_ISVTX);
        }
        break;
    }
}

// gets masks for files and dirs from the Access Control frame widgets
void KFilePermissionsPropsPlugin::getPermissionMasks(mode_t &andFilePermissions, mode_t &andDirPermissions, mode_t &orFilePermissions, mode_t &orDirPermissions)
{
    andFilePermissions = mode_t(~UniSpecial);
    andDirPermissions = mode_t(~(S_ISUID | S_ISGID));
    orFilePermissions = 0;
    orDirPermissions = 0;
    if (d->isIrregular) {
        return;
    }

    mode_t m = standardPermissions[d->ownerPermCombo->currentIndex()];
    if (m != s_invalid_mode_t) {
        orFilePermissions |= m & UniOwner;
        if ((m & UniOwner)
            && ((d->pmode == PermissionsMixed) || ((d->pmode == PermissionsOnlyFiles) && (d->extraCheckbox->checkState() == Qt::PartiallyChecked)))) {
            andFilePermissions &= ~(S_IRUSR | S_IWUSR);
        } else {
            andFilePermissions &= ~(S_IRUSR | S_IWUSR | S_IXUSR);
            if ((m & S_IRUSR) && (d->extraCheckbox->checkState() == Qt::Checked)) {
                orFilePermissions |= S_IXUSR;
            }
        }

        orDirPermissions |= m & UniOwner;
        if (m & S_IRUSR) {
            orDirPermissions |= S_IXUSR;
        }
        andDirPermissions &= ~(S_IRUSR | S_IWUSR | S_IXUSR);
    }

    m = standardPermissions[d->groupPermCombo->currentIndex()];
    if (m != s_invalid_mode_t) {
        orFilePermissions |= m & UniGroup;
        if ((m & UniGroup)
            && ((d->pmode == PermissionsMixed) || ((d->pmode == PermissionsOnlyFiles) && (d->extraCheckbox->checkState() == Qt::PartiallyChecked)))) {
            andFilePermissions &= ~(S_IRGRP | S_IWGRP);
        } else {
            andFilePermissions &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
            if ((m & S_IRGRP) && (d->extraCheckbox->checkState() == Qt::Checked)) {
                orFilePermissions |= S_IXGRP;
            }
        }

        orDirPermissions |= m & UniGroup;
        if (m & S_IRGRP) {
            orDirPermissions |= S_IXGRP;
        }
        andDirPermissions &= ~(S_IRGRP | S_IWGRP | S_IXGRP);
    }

    m = d->othersPermCombo->currentIndex() >= 0 ? standardPermissions[d->othersPermCombo->currentIndex()] : s_invalid_mode_t;
    if (m != s_invalid_mode_t) {
        orFilePermissions |= m & UniOthers;
        if ((m & UniOthers)
            && ((d->pmode == PermissionsMixed) || ((d->pmode == PermissionsOnlyFiles) && (d->extraCheckbox->checkState() == Qt::PartiallyChecked)))) {
            andFilePermissions &= ~(S_IROTH | S_IWOTH);
        } else {
            andFilePermissions &= ~(S_IROTH | S_IWOTH | S_IXOTH);
            if ((m & S_IROTH) && (d->extraCheckbox->checkState() == Qt::Checked)) {
                orFilePermissions |= S_IXOTH;
            }
        }

        orDirPermissions |= m & UniOthers;
        if (m & S_IROTH) {
            orDirPermissions |= S_IXOTH;
        }
        andDirPermissions &= ~(S_IROTH | S_IWOTH | S_IXOTH);
    }

    if (((d->pmode == PermissionsMixed) || (d->pmode == PermissionsOnlyDirs)) && (d->extraCheckbox->checkState() != Qt::PartiallyChecked)) {
        andDirPermissions &= ~S_ISVTX;
        if (d->extraCheckbox->checkState() == Qt::Checked) {
            orDirPermissions |= S_ISVTX;
        }
    }
}

void KFilePermissionsPropsPlugin::applyChanges()
{
    mode_t orFilePermissions;
    mode_t orDirPermissions;
    mode_t andFilePermissions;
    mode_t andDirPermissions;

    if (!d->canChangePermissions) {
        properties->abortApplying();
        return;
    }

    if (!d->isIrregular) {
        getPermissionMasks(andFilePermissions, andDirPermissions, orFilePermissions, orDirPermissions);
    } else {
        orFilePermissions = d->permissions;
        andFilePermissions = d->partialPermissions;
        orDirPermissions = d->permissions;
        andDirPermissions = d->partialPermissions;
    }

    QString owner;
    QString group;
    if (d->usrEdit) {
        owner = d->usrEdit->text();
    }
    if (d->grpEdit) {
        group = d->grpEdit->text();
    } else if (d->grpCombo) {
        group = d->grpCombo->currentText();
    }

    const bool recursive = d->cbRecursive && d->cbRecursive->isChecked();

    if (!recursive) {
        if (owner == d->strOwner) {
            owner.clear();
        }

        if (group == d->strGroup) {
            group.clear();
        }
    }

    bool permissionChange = false;

    const KFileItemList items = properties->items();
    KFileItemList files;
    KFileItemList dirs;
    for (const auto &item : items) {
        const auto perms = item.permissions();
        if (item.isDir()) {
            dirs.append(item);
            if (!permissionChange && (recursive || perms != ((perms & andDirPermissions) | orDirPermissions))) {
                permissionChange = true;
            }
            continue;
        }

        if (item.isFile()) {
            files.append(item);
            if (!permissionChange && perms != ((perms & andFilePermissions) | orFilePermissions)) {
                permissionChange = true;
            }
        }
    }

    const bool ACLChange = (d->extendedACL != properties->item().ACL());
    const bool defaultACLChange = (d->defaultACL != properties->item().defaultACL());

    if (owner.isEmpty() && group.isEmpty() && !recursive && !permissionChange && !ACLChange && !defaultACLChange) {
        return;
    }

    auto processACLChanges = [this, ACLChange, defaultACLChange](KIO::ChmodJob *chmodJob) {
        if (!d->fileSystemSupportsACLs) {
            return;
        }

        if (ACLChange) {
            chmodJob->addMetaData(QStringLiteral("ACL_STRING"), d->extendedACL.isValid() ? d->extendedACL.asString() : QStringLiteral("ACL_DELETE"));
        }

        if (defaultACLChange) {
            chmodJob->addMetaData(QStringLiteral("DEFAULT_ACL_STRING"), d->defaultACL.isValid() ? d->defaultACL.asString() : QStringLiteral("ACL_DELETE"));
        }
    };

    auto chmodDirs = [=, this]() {
        if (dirs.isEmpty()) {
            setDirty(false);
            Q_EMIT changesApplied();
            return;
        }

        auto *dirsJob = KIO::chmod(dirs, orDirPermissions, ~andDirPermissions, owner, group, recursive);
        processACLChanges(dirsJob);

        connect(dirsJob, &KJob::result, this, [this, dirsJob]() {
            if (dirsJob->error()) {
                dirsJob->uiDelegate()->showErrorMessage();
            }

            setDirty(false);
            Q_EMIT changesApplied();
        });
    };

    // Change permissions in two steps, first files, then dirs

    if (!files.isEmpty()) {
        auto *filesJob = KIO::chmod(files, orFilePermissions, ~andFilePermissions, owner, group, false);
        processACLChanges(filesJob);

        connect(filesJob, &KJob::result, this, [=]() {
            if (filesJob->error()) {
                filesJob->uiDelegate()->showErrorMessage();
            }

            chmodDirs();
        });
        return;
    }

    // No files to change? OK, now process dirs (if any)
    chmodDirs();
}

class KChecksumsPlugin::KChecksumsPluginPrivate
{
public:
    QWidget m_widget;
    Ui::ChecksumsWidget m_ui;

    QFileSystemWatcher fileWatcher;
    QString m_md5;
    QString m_sha1;
    QString m_sha256;
    QString m_sha512;

    bool m_multiFileMode{false};
    bool m_md5Matches{false};
    bool m_sha1Matches{false};
    bool m_sha256Matches{false};
    bool m_sha512Matches{false};
};

KChecksumsPlugin::KChecksumsPlugin(KPropertiesDialog *dialog)
    : KPropertiesDialogPlugin(dialog)
    , d(new KChecksumsPluginPrivate)
{
    d->m_ui.setupUi(&d->m_widget);
    properties->addPage(&d->m_widget, i18nc("@title:tab", "C&hecksums"));

    d->m_ui.md5CopyButton->hide();
    d->m_ui.sha1CopyButton->hide();
    d->m_ui.sha256CopyButton->hide();
    d->m_ui.sha512CopyButton->hide();

    connect(d->m_ui.lineEdit, &QLineEdit::textChanged, this, [this](const QString &text) {
        slotVerifyChecksum(text.toLower());
    });

    connect(d->m_ui.md5Button, &QPushButton::clicked, this, &KChecksumsPlugin::slotShowMd5);
    connect(d->m_ui.sha1Button, &QPushButton::clicked, this, &KChecksumsPlugin::slotShowSha1);
    connect(d->m_ui.sha256Button, &QPushButton::clicked, this, &KChecksumsPlugin::slotShowSha256);
    connect(d->m_ui.sha512Button, &QPushButton::clicked, this, &KChecksumsPlugin::slotShowSha512);

    // NOTE we only watch the first file for changes
    // - if there is only one file, then this is the only path
    // - if there are multiple file, the first one is the one we're comparing against
    // - properly watching all paths would be complicated because we would have to
    //   work around limits to the number of files supported by QFileSystemWatcher...
    d->fileWatcher.addPath(properties->item().localPath());
    connect(&d->fileWatcher, &QFileSystemWatcher::fileChanged, this, &KChecksumsPlugin::slotInvalidateCache);

    auto clipboard = QApplication::clipboard();
    connect(d->m_ui.md5CopyButton, &QPushButton::clicked, this, [=, this]() {
        clipboard->setText(d->m_md5);
    });

    connect(d->m_ui.sha1CopyButton, &QPushButton::clicked, this, [=, this]() {
        clipboard->setText(d->m_sha1);
    });

    connect(d->m_ui.sha256CopyButton, &QPushButton::clicked, this, [=, this]() {
        clipboard->setText(d->m_sha256);
    });

    connect(d->m_ui.sha512CopyButton, &QPushButton::clicked, this, [=, this]() {
        clipboard->setText(d->m_sha512);
    });

    connect(d->m_ui.pasteButton, &QPushButton::clicked, this, [=, this]() {
        d->m_ui.lineEdit->setText(clipboard->text());
    });

    setDefaultState();

    if (properties->items().count() == 1 && detectAlgorithm(clipboard->text()) != QCryptographicHash::Md4) {
        d->m_ui.lineEdit->setText(clipboard->text());
    }
}

KChecksumsPlugin::~KChecksumsPlugin() = default;

bool KChecksumsPlugin::supports(const KFileItemList &items)
{
    if (items.count() < 1) {
        return false;
    }

    for (const auto &i : items) {
        if (!i.isFile() || i.localPath().isEmpty() || !i.isReadable() || i.isLink()) {
            return false;
        }
    }

    return true;
}

void KChecksumsPlugin::slotInvalidateCache()
{
    d->m_md5 = QString();
    d->m_sha1 = QString();
    d->m_sha256 = QString();
    d->m_sha512 = QString();
}

void KChecksumsPlugin::slotShowMd5()
{
    d->m_ui.md5LineEdit->show(); // Show before hiding. This way keyboard focus goes from md5Button to md5LineEdit.
    d->m_ui.md5Button->hide();
    d->m_ui.md5Label->setBuddy(d->m_ui.md5LineEdit);
    d->m_ui.horizontalSpacerMd5->changeSize(0, 0);

    showChecksum(QCryptographicHash::Md5, d->m_ui.md5LineEdit, d->m_ui.md5CopyButton);
}

void KChecksumsPlugin::slotShowSha1()
{
    d->m_ui.sha1LineEdit->show(); // Show before hiding. This way keyboard focus goes from sha1Button to sha1LineEdit.
    d->m_ui.sha1Button->hide();
    d->m_ui.sha1Label->setBuddy(d->m_ui.sha1LineEdit);
    d->m_ui.horizontalSpacerSha1->changeSize(0, 0);

    showChecksum(QCryptographicHash::Sha1, d->m_ui.sha1LineEdit, d->m_ui.sha1CopyButton);
}

void KChecksumsPlugin::slotShowSha256()
{
    d->m_ui.sha256LineEdit->show();
    d->m_ui.sha256Button->hide();
    d->m_ui.sha256Label->setBuddy(d->m_ui.sha256LineEdit);
    d->m_ui.horizontalSpacerSha256->changeSize(0, 0);

    showChecksum(QCryptographicHash::Sha256, d->m_ui.sha256LineEdit, d->m_ui.sha256CopyButton);
}

void KChecksumsPlugin::slotShowSha512()
{
    d->m_ui.sha512LineEdit->show();
    d->m_ui.sha512Button->hide();
    d->m_ui.sha512Label->setBuddy(d->m_ui.sha512LineEdit);
    d->m_ui.horizontalSpacerSha512->changeSize(0, 0);

    showChecksum(QCryptographicHash::Sha512, d->m_ui.sha512LineEdit, d->m_ui.sha512CopyButton);
}

void KChecksumsPlugin::slotVerifyChecksum(const QString &input)
{
    auto algorithm = detectAlgorithm(input);

    // Input is not a supported hash algorithm.
    if (algorithm == QCryptographicHash::Md4) {
        if (input.isEmpty()) {
            setDefaultState();
        } else {
            setInvalidChecksumState();
        }
        return;
    }

    const QString checksum = cachedChecksum(algorithm);

    // Checksum already in cache.
    if (!checksum.isEmpty()) {
        const bool isMatch = (checksum == input);
        if (isMatch) {
            setMatchState();
        } else {
            setMismatchState();
        }

        return;
    }

    // Calculate checksum in another thread.
    using CheckType = QPair<QString, bool>;

    auto futureWatcher = new QFutureWatcher<CheckType>(this);
    connect(futureWatcher, &QFutureWatcher<CheckType>::finished, this, [=, this]() {
        const QString checksum = futureWatcher->result().first;
        futureWatcher->deleteLater();

        cacheChecksum(checksum, algorithm);

        switch (algorithm) {
        case QCryptographicHash::Md5:
            slotShowMd5();
            break;
        case QCryptographicHash::Sha1:
            slotShowSha1();
            break;
        case QCryptographicHash::Sha256:
            slotShowSha256();
            break;
        case QCryptographicHash::Sha512:
            slotShowSha512();
            break;
        default:
            break;
        }

        const bool isMatch = (checksum == input);
        if (isMatch) {
            setMatchState();
        } else {
            setMismatchState();
        }
    });

    // Notify the user about the background computation.
    setVerifyState();

    auto future = QtConcurrent::run(&KChecksumsPlugin::computeChecksum, algorithm, properties->items());
    futureWatcher->setFuture(future);
}

bool KChecksumsPlugin::isMd5(const QString &input)
{
    QRegularExpression regex(QStringLiteral("^[a-f0-9]{32}$"));
    return regex.match(input).hasMatch();
}

bool KChecksumsPlugin::isSha1(const QString &input)
{
    QRegularExpression regex(QStringLiteral("^[a-f0-9]{40}$"));
    return regex.match(input).hasMatch();
}

bool KChecksumsPlugin::isSha256(const QString &input)
{
    QRegularExpression regex(QStringLiteral("^[a-f0-9]{64}$"));
    return regex.match(input).hasMatch();
}

bool KChecksumsPlugin::isSha512(const QString &input)
{
    QRegularExpression regex(QStringLiteral("^[a-f0-9]{128}$"));
    return regex.match(input).hasMatch();
}

QPair<QString, bool> KChecksumsPlugin::computeChecksum(QCryptographicHash::Algorithm algorithm, const KFileItemList &items)
{
    auto getChecksum = [&](const QString &path) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            return QString();
        }

        QCryptographicHash hash(algorithm);
        hash.addData(&file);

        return QString::fromLatin1(hash.result().toHex());
    };

    QString comparedSum = getChecksum(items.first().localPath());
    bool matches = true;

    for (qsizetype i = 1; i < items.count(); ++i) {
        auto sum = getChecksum(items[i].localPath());

        if (sum != comparedSum) {
            matches = false;
            break;
        }
    }

    return {comparedSum, matches};
}

QCryptographicHash::Algorithm KChecksumsPlugin::detectAlgorithm(const QString &input)
{
    if (isMd5(input)) {
        return QCryptographicHash::Md5;
    }

    if (isSha1(input)) {
        return QCryptographicHash::Sha1;
    }

    if (isSha256(input)) {
        return QCryptographicHash::Sha256;
    }

    if (isSha512(input)) {
        return QCryptographicHash::Sha512;
    }

    // Md4 used as negative error code.
    return QCryptographicHash::Md4;
}

void KChecksumsPlugin::setDefaultState()
{
    QColor defaultColor = d->m_widget.palette().color(QPalette::Base);

    QPalette palette = d->m_widget.palette();
    palette.setColor(QPalette::Base, defaultColor);

    d->m_ui.feedbackLabel->hide();
    d->m_ui.lineEdit->setPalette(palette);
    d->m_ui.lineEdit->setToolTip(QString());

    if (properties->items().count() > 1) {
        d->m_multiFileMode = true;

        d->m_ui.label->hide();
        d->m_ui.kseparator->hide();
        d->m_ui.lineEdit->hide();
        d->m_ui.pasteButton->hide();
    }
}

void KChecksumsPlugin::setInvalidChecksumState()
{
    KColorScheme colorScheme(QPalette::Active, KColorScheme::View);
    QColor warningColor = colorScheme.background(KColorScheme::NegativeBackground).color();

    QPalette palette = d->m_widget.palette();
    palette.setColor(QPalette::Base, warningColor);

    d->m_ui.feedbackLabel->setText(i18n("Invalid checksum."));
    d->m_ui.feedbackLabel->show();
    d->m_ui.lineEdit->setPalette(palette);
    d->m_ui.lineEdit->setToolTip(i18nc("@info:tooltip", "The given input is not a valid MD5, SHA1 or SHA256 checksum."));
}

void KChecksumsPlugin::setMatchState()
{
    KColorScheme colorScheme(QPalette::Active, KColorScheme::View);
    QColor positiveColor = colorScheme.background(KColorScheme::PositiveBackground).color();

    QPalette palette = d->m_widget.palette();
    palette.setColor(QPalette::Base, positiveColor);

    d->m_ui.feedbackLabel->setText(i18n("Checksums match."));
    d->m_ui.feedbackLabel->show();
    d->m_ui.lineEdit->setPalette(palette);
    d->m_ui.lineEdit->setToolTip(i18nc("@info:tooltip", "The computed checksum and the expected checksum match."));
}

void KChecksumsPlugin::setMismatchState()
{
    KColorScheme colorScheme(QPalette::Active, KColorScheme::View);
    QColor warningColor = colorScheme.background(KColorScheme::NegativeBackground).color();

    QPalette palette = d->m_widget.palette();
    palette.setColor(QPalette::Base, warningColor);

    d->m_ui.feedbackLabel->setText(
        i18n("<p>Checksums do not match.</p>"
             "This may be due to a faulty download. Try re-downloading the file.<br/>"
             "If the verification still fails, contact the source of the file."));
    d->m_ui.feedbackLabel->show();
    d->m_ui.feedbackLabel->setWordWrap(true);
    d->m_ui.lineEdit->setPalette(palette);
    d->m_ui.lineEdit->setToolTip(i18nc("@info:tooltip", "The computed checksum and the expected checksum differ."));
}

void KChecksumsPlugin::setVerifyState()
{
    // Users can paste a checksum at any time, so reset to default.
    setDefaultState();

    d->m_ui.feedbackLabel->setText(i18nc("@info:progress computation in the background", "Verifying checksum…"));
    d->m_ui.feedbackLabel->show();
}

void KChecksumsPlugin::showChecksum(QCryptographicHash::Algorithm algorithm, QLineEdit *label, QPushButton *copyButton)
{
    const QString checksum = cachedChecksum(algorithm);

    // Reset colors before calculating
    KColorScheme colorScheme(QPalette::Active, KColorScheme::View);
    QPalette palette = d->m_widget.palette();
    QColor defaultColor = d->m_widget.palette().color(QPalette::Base);
    palette.setColor(QPalette::Base, defaultColor);
    label->setPalette(palette);

    // Checksum in cache, nothing else to do
    if (!checksum.isEmpty()) {
        label->setText(checksum);
        label->setCursorPosition(0);
        copyButton->show();

        if (d->m_multiFileMode) {
            d->m_ui.feedbackLabel->show();
            d->m_ui.kseparator->show();

            if (cachedMultiFileMatch(algorithm)) {
                QColor positiveColor = colorScheme.background(KColorScheme::PositiveBackground).color();
                palette.setColor(QPalette::Base, positiveColor);
                label->setPalette(palette);
                d->m_ui.feedbackLabel->setText(i18n("The checksums of all files are identical."));
            } else {
                QColor negativeColor = colorScheme.background(KColorScheme::NegativeBackground).color();
                palette.setColor(QPalette::Base, negativeColor);
                label->setPalette(palette);
                d->m_ui.feedbackLabel->setText(i18n("The selected files have different checksums."));
            }
        }

        return;
    } else {
        label->setText(i18nc("@info:progress", "Calculating…"));
    }

    // Calculate checksum in another thread
    using CheckType = QPair<QString, bool>;

    auto futureWatcher = new QFutureWatcher<CheckType>(this);
    connect(futureWatcher, &QFutureWatcher<CheckType>::finished, this, [=, this]() {
        const CheckType result = futureWatcher->result();
        futureWatcher->deleteLater();

        cacheChecksum(result.first, algorithm);
        cacheMultiFileMatch(result.second, algorithm);

        showChecksum(algorithm, label, copyButton); // actually show cached result
    });

    auto future = QtConcurrent::run(&KChecksumsPlugin::computeChecksum, algorithm, properties->items());
    futureWatcher->setFuture(future);
}

QString KChecksumsPlugin::cachedChecksum(QCryptographicHash::Algorithm algorithm) const
{
    switch (algorithm) {
    case QCryptographicHash::Md5:
        return d->m_md5;
    case QCryptographicHash::Sha1:
        return d->m_sha1;
    case QCryptographicHash::Sha256:
        return d->m_sha256;
    case QCryptographicHash::Sha512:
        return d->m_sha512;
    default:
        break;
    }

    return QString();
}

void KChecksumsPlugin::cacheChecksum(const QString &checksum, QCryptographicHash::Algorithm algorithm)
{
    switch (algorithm) {
    case QCryptographicHash::Md5:
        d->m_md5 = checksum;
        break;
    case QCryptographicHash::Sha1:
        d->m_sha1 = checksum;
        break;
    case QCryptographicHash::Sha256:
        d->m_sha256 = checksum;
        break;
    case QCryptographicHash::Sha512:
        d->m_sha512 = checksum;
        break;
    default:
        return;
    }
}

bool KChecksumsPlugin::cachedMultiFileMatch(QCryptographicHash::Algorithm algorithm) const
{
    switch (algorithm) {
    case QCryptographicHash::Md5:
        return d->m_md5Matches;
    case QCryptographicHash::Sha1:
        return d->m_sha1Matches;
    case QCryptographicHash::Sha256:
        return d->m_sha256Matches;
    case QCryptographicHash::Sha512:
        return d->m_sha512Matches;
    default:
        break;
    }

    return false;
}

void KChecksumsPlugin::cacheMultiFileMatch(const bool &isMatch, QCryptographicHash::Algorithm algorithm)
{
    switch (algorithm) {
    case QCryptographicHash::Md5:
        d->m_md5Matches = isMatch;
        break;
    case QCryptographicHash::Sha1:
        d->m_sha1Matches = isMatch;
        break;
    case QCryptographicHash::Sha256:
        d->m_sha256Matches = isMatch;
        break;
    case QCryptographicHash::Sha512:
        d->m_sha512Matches = isMatch;
        break;
    default:
        return;
    }
}

class KUrlPropsPlugin::KUrlPropsPluginPrivate
{
public:
    QFrame *m_frame;
    KUrlRequester *URLEdit;
    QString URLStr;
    bool fileNameReadOnly = false;
};

KUrlPropsPlugin::KUrlPropsPlugin(KPropertiesDialog *_props)
    : KPropertiesDialogPlugin(_props)
    , d(new KUrlPropsPluginPrivate)
{
    d->m_frame = new QFrame();
    properties->addPage(d->m_frame, i18n("U&RL"));
    QVBoxLayout *layout = new QVBoxLayout(d->m_frame);

    QLabel *l;
    l = new QLabel(d->m_frame);
    l->setObjectName(QStringLiteral("Label_1"));
    l->setText(i18n("URL:"));
    layout->addWidget(l);

    d->URLEdit = new KUrlRequester(d->m_frame);
    layout->addWidget(d->URLEdit);

    KIO::StatJob *job = KIO::mostLocalUrl(properties->url());
    KJobWidgets::setWindow(job, properties);
    job->exec();
    QUrl url = job->mostLocalUrl();

    if (url.isLocalFile()) {
        QString path = url.toLocalFile();

        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) {
            return;
        }

        KDesktopFile config(path);
        const KConfigGroup dg = config.desktopGroup();
        d->URLStr = dg.readPathEntry("URL", QString());

        if (!d->URLStr.isEmpty()) {
            d->URLEdit->setUrl(QUrl(d->URLStr));
        }
    }

    connect(d->URLEdit, &KUrlRequester::textChanged, this, &KPropertiesDialogPlugin::changed);

    layout->addStretch(1);
}

KUrlPropsPlugin::~KUrlPropsPlugin() = default;

void KUrlPropsPlugin::setFileNameReadOnly(bool ro)
{
    d->fileNameReadOnly = ro;
}

bool KUrlPropsPlugin::supports(const KFileItemList &_items)
{
    if (_items.count() != 1) {
        return false;
    }
    const KFileItem &item = _items.first();
    // check if desktop file
    if (!item.isDesktopFile()) {
        return false;
    }

    // open file and check type
    const auto [url, isLocal] = item.isMostLocalUrl();
    if (!isLocal) {
        return false;
    }

    KDesktopFile config(url.toLocalFile());
    return config.hasLinkType();
}

void KUrlPropsPlugin::applyChanges()
{
    KIO::StatJob *job = KIO::mostLocalUrl(properties->url());
    KJobWidgets::setWindow(job, properties);
    job->exec();
    const QUrl url = job->mostLocalUrl();

    if (!url.isLocalFile()) {
        KMessageBox::error(nullptr, i18n("Could not save properties. Only entries on local file systems are supported."));
        properties->abortApplying();
        return;
    }

    QString path = url.toLocalFile();
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        KMessageBox::error(nullptr, couldNotSaveMsg(path));
        properties->abortApplying();
        return;
    }

    KDesktopFile config(path);
    KConfigGroup dg = config.desktopGroup();
    dg.writeEntry("Type", QStringLiteral("Link"));
    dg.writePathEntry("URL", d->URLEdit->url().toString());
    // Users can't create a Link .desktop file with a Name field,
    // but distributions can. Update the Name field in that case,
    // if the file name could have been changed.
    if (!d->fileNameReadOnly && dg.hasKey("Name")) {
        const QString nameStr = nameFromFileName(properties->url().fileName());
        dg.writeEntry("Name", nameStr);
        dg.writeEntry("Name", nameStr, KConfigBase::Persistent | KConfigBase::Localized);
    }

    setDirty(false);
}

/* ----------------------------------------------------
 *
 * KDesktopPropsPlugin
 *
 * -------------------------------------------------- */

class KDesktopPropsPlugin::KDesktopPropsPluginPrivate
{
public:
    KDesktopPropsPluginPrivate()
        : w(new Ui_KPropertiesDesktopBase)
        , m_frame(new QFrame())
    {
    }
    ~KDesktopPropsPluginPrivate()
    {
        delete w;
    }
    QString command() const;
    Ui_KPropertiesDesktopBase *w;
    QWidget *m_frame = nullptr;
    std::unique_ptr<Ui_KPropertiesDesktopAdvBase> m_uiAdvanced;

    QString m_origCommandStr;
    QString m_terminalOptionStr;
    QString m_suidUserStr;
    QString m_origDesktopFile;
    bool m_terminalBool;
    bool m_suidBool;
    // Corresponds to "PrefersNonDefaultGPU=" (added in destop-entry-spec 1.4)
    bool m_runOnDiscreteGpuBool;
    bool m_startupBool;
};

KDesktopPropsPlugin::KDesktopPropsPlugin(KPropertiesDialog *_props)
    : KPropertiesDialogPlugin(_props)
    , d(new KDesktopPropsPluginPrivate)
{
    QMimeDatabase db;

    d->w->setupUi(d->m_frame);

    properties->addPage(d->m_frame, i18n("&Application"));

    bool bKDesktopMode = properties->url().scheme() == QLatin1String("desktop") || properties->currentDir().scheme() == QLatin1String("desktop");

    d->w->pathEdit->setMode(KFile::Directory | KFile::LocalOnly);
    d->w->pathEdit->lineEdit()->setAcceptDrops(false);

    connect(d->w->nameEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->genNameEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->commentEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->envarsEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->programEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->argumentsEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->w->pathEdit, &KUrlRequester::textChanged, this, &KPropertiesDialogPlugin::changed);

    connect(d->w->browseButton, &QAbstractButton::clicked, this, &KDesktopPropsPlugin::slotBrowseExec);
    connect(d->w->addFiletypeButton, &QAbstractButton::clicked, this, &KDesktopPropsPlugin::slotAddFiletype);
    connect(d->w->delFiletypeButton, &QAbstractButton::clicked, this, &KDesktopPropsPlugin::slotDelFiletype);
    connect(d->w->advancedButton, &QAbstractButton::clicked, this, &KDesktopPropsPlugin::slotAdvanced);

    // now populate the page

    KIO::StatJob *job = KIO::mostLocalUrl(_props->url());
    KJobWidgets::setWindow(job, _props);
    job->exec();
    QUrl url = job->mostLocalUrl();

    if (!url.isLocalFile()) {
        return;
    }

    d->m_origDesktopFile = url.toLocalFile();

    QFile f(d->m_origDesktopFile);
    if (!f.open(QIODevice::ReadOnly)) {
        return;
    }

    KDesktopFile _config(d->m_origDesktopFile);
    KConfigGroup config = _config.desktopGroup();
    QString nameStr = _config.readName();
    QString genNameStr = _config.readGenericName();
    QString commentStr = _config.readComment();
    QString commandStr = config.readEntry("Exec", QString());

    d->m_origCommandStr = commandStr;
    QString pathStr = config.readEntry("Path", QString()); // not readPathEntry, see kservice.cpp
    d->m_terminalBool = config.readEntry("Terminal", false);
    d->m_terminalOptionStr = config.readEntry("TerminalOptions");
    d->m_suidBool = config.readEntry("X-KDE-SubstituteUID", false);
    d->m_suidUserStr = config.readEntry("X-KDE-Username");
    if (KIO::hasDiscreteGpu()) {
        if (config.hasKey("PrefersNonDefaultGPU")) {
            d->m_runOnDiscreteGpuBool = config.readEntry("PrefersNonDefaultGPU", false);
        } else {
            d->m_runOnDiscreteGpuBool = config.readEntry("X-KDE-RunOnDiscreteGpu", false);
        }
    }
    if (config.hasKey("StartupNotify")) {
        d->m_startupBool = config.readEntry("StartupNotify", true);
    } else {
        d->m_startupBool = config.readEntry("X-KDE-StartupNotify", true);
    }

    const QStringList mimeTypes = config.readXdgListEntry("MimeType");

    if (nameStr.isEmpty() || bKDesktopMode) {
        // We'll use the file name if no name is specified
        // because we _need_ a Name for a valid file.
        // But let's do it in apply, not here, so that we pick up the right name.
        setDirty();
    }
    d->w->nameEdit->setText(nameStr);
    d->w->genNameEdit->setText(genNameStr);
    d->w->commentEdit->setText(commentStr);

    QStringList execLine = KShell::splitArgs(commandStr);
    QStringList enVars = {};

    if (!execLine.isEmpty()) {
        // check for apps that use the env executable
        // to set the environment
        if (execLine[0] == QLatin1String("env")) {
            execLine.pop_front();
        }
        for (const auto &env : execLine) {
            if (execLine.length() <= 1) {
                // Don't empty out the list. If the last element contains an equal sign we have to treat it as part of the
                // program name lest we have no program
                // https://bugs.kde.org/show_bug.cgi?id=465290
                break;
            }
            if (!env.contains(QLatin1String("="))) {
                break;
            }
            enVars += env;
            execLine.pop_front();
        }

        Q_ASSERT(!execLine.isEmpty());
        d->w->programEdit->setText(execLine.takeFirst());
    } else {
        d->w->programEdit->clear();
    }
    d->w->argumentsEdit->setText(KShell::joinArgs(execLine));
    d->w->envarsEdit->setText(KShell::joinArgs(enVars));

    d->w->pathEdit->lineEdit()->setText(pathStr);

    // was: d->w->filetypeList->setFullWidth(true);
    //  d->w->filetypeList->header()->setStretchEnabled(true, d->w->filetypeList->columns()-1);

    for (QStringList::ConstIterator it = mimeTypes.begin(); it != mimeTypes.end();) {
        QMimeType p = db.mimeTypeForName(*it);
        ++it;
        QString preference;
        if (it != mimeTypes.end()) {
            bool numeric;
            (*it).toInt(&numeric);
            if (numeric) {
                preference = *it;
                ++it;
            }
        }
        if (p.isValid()) {
            QTreeWidgetItem *item = new QTreeWidgetItem();
            item->setText(0, p.name());
            item->setText(1, p.comment());
            item->setText(2, preference);
            d->w->filetypeList->addTopLevelItem(item);
        }
    }
    d->w->filetypeList->resizeColumnToContents(0);
}

KDesktopPropsPlugin::~KDesktopPropsPlugin() = default;

void KDesktopPropsPlugin::slotAddFiletype()
{
    QMimeDatabase db;
    KMimeTypeChooserDialog dlg(i18n("Add File Type for %1", properties->url().fileName()),
                               i18n("Select one or more file types to add:"),
                               QStringList(), // no preselected mimetypes
                               QString(),
                               QStringList(),
                               KMimeTypeChooser::Comments | KMimeTypeChooser::Patterns,
                               d->m_frame);

    if (dlg.exec() == QDialog::Accepted) {
        const QStringList list = dlg.chooser()->mimeTypes();
        for (const QString &mimetype : list) {
            QMimeType p = db.mimeTypeForName(mimetype);
            if (!p.isValid()) {
                continue;
            }

            bool found = false;
            int count = d->w->filetypeList->topLevelItemCount();
            for (int i = 0; !found && i < count; ++i) {
                if (d->w->filetypeList->topLevelItem(i)->text(0) == mimetype) {
                    found = true;
                }
            }
            if (!found) {
                QTreeWidgetItem *item = new QTreeWidgetItem();
                item->setText(0, p.name());
                item->setText(1, p.comment());
                d->w->filetypeList->addTopLevelItem(item);
            }
            d->w->filetypeList->resizeColumnToContents(0);
        }
    }
    Q_EMIT changed();
}

void KDesktopPropsPlugin::slotDelFiletype()
{
    QTreeWidgetItem *cur = d->w->filetypeList->currentItem();
    if (cur) {
        delete cur;
        Q_EMIT changed();
    }
}

void KDesktopPropsPlugin::checkCommandChanged()
{
    if (KIO::DesktopExecParser::executableName(d->command()) != KIO::DesktopExecParser::executableName(d->m_origCommandStr)) {
        d->m_origCommandStr = d->command();
    }
}

void KDesktopPropsPlugin::applyChanges()
{
    // qDebug();
    KIO::StatJob *job = KIO::mostLocalUrl(properties->url());
    KJobWidgets::setWindow(job, properties);
    job->exec();
    const QUrl url = job->mostLocalUrl();

    if (!url.isLocalFile()) {
        KMessageBox::error(nullptr, i18n("Could not save properties. Only entries on local file systems are supported."));
        properties->abortApplying();
        return;
    }

    const QString path(url.toLocalFile());

    // make sure the directory exists
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::ReadWrite)) {
        KMessageBox::error(nullptr, couldNotSaveMsg(path));
        properties->abortApplying();
        return;
    }

    // If the command is changed we reset certain settings that are strongly
    // coupled to the command.
    checkCommandChanged();

    KDesktopFile origConfig(d->m_origDesktopFile);
    std::unique_ptr<KDesktopFile> _config(origConfig.copyTo(path));
    KConfigGroup config = _config->desktopGroup();
    config.writeEntry("Type", QStringLiteral("Application"));
    config.writeEntry("Comment", d->w->commentEdit->text());
    config.writeEntry("Comment", d->w->commentEdit->text(), KConfigGroup::Persistent | KConfigGroup::Localized); // for compat
    config.writeEntry("GenericName", d->w->genNameEdit->text());
    config.writeEntry("GenericName", d->w->genNameEdit->text(), KConfigGroup::Persistent | KConfigGroup::Localized); // for compat
    config.writeEntry("Exec", d->command());
    config.writeEntry("Path", d->w->pathEdit->lineEdit()->text()); // not writePathEntry, see kservice.cpp

    // Write mimeTypes
    QStringList mimeTypes;
    int count = d->w->filetypeList->topLevelItemCount();
    for (int i = 0; i < count; ++i) {
        QTreeWidgetItem *item = d->w->filetypeList->topLevelItem(i);
        QString preference = item->text(2);
        mimeTypes.append(item->text(0));
        if (!preference.isEmpty()) {
            mimeTypes.append(preference);
        }
    }

    // qDebug() << mimeTypes;
    config.writeXdgListEntry("MimeType", mimeTypes);

    if (!d->w->nameEdit->isHidden()) {
        QString nameStr = d->w->nameEdit->text();
        config.writeEntry("Name", nameStr);
        config.writeEntry("Name", nameStr, KConfigGroup::Persistent | KConfigGroup::Localized);
    }

    config.writeEntry("Terminal", d->m_terminalBool);
    config.writeEntry("TerminalOptions", d->m_terminalOptionStr);
    config.writeEntry("X-KDE-SubstituteUID", d->m_suidBool);
    config.writeEntry("X-KDE-Username", d->m_suidUserStr);
    if (KIO::hasDiscreteGpu()) {
        config.writeEntry("PrefersNonDefaultGPU", d->m_runOnDiscreteGpuBool);
    }
    config.writeEntry("StartupNotify", d->m_startupBool);
    config.sync();

    // KSycoca update needed?
    bool updateNeeded = !relativeAppsLocation(path).isEmpty();
    if (updateNeeded) {
        KBuildSycocaProgressDialog::rebuildKSycoca(d->m_frame);
    }

    setDirty(false);
}

void KDesktopPropsPlugin::slotBrowseExec()
{
    QUrl f = QFileDialog::getOpenFileUrl(d->m_frame);
    if (f.isEmpty()) {
        return;
    }

    if (!f.isLocalFile()) {
        KMessageBox::information(d->m_frame, i18n("Only executables on local file systems are supported."));
        return;
    }

    const QString path = f.toLocalFile();
    d->w->programEdit->setText(path);
}

void KDesktopPropsPlugin::slotAdvanced()
{
    auto *dlg = new QDialog(d->m_frame);
    dlg->setObjectName(QStringLiteral("KPropertiesDesktopAdv"));
    dlg->setWindowModality(Qt::WindowModal);
    dlg->setModal(true);
    dlg->setAttribute(Qt::WA_DeleteOnClose);
    dlg->setWindowTitle(i18n("Advanced Options for %1", properties->url().fileName()));

    d->m_uiAdvanced.reset(new Ui_KPropertiesDesktopAdvBase);
    QWidget *mainWidget = new QWidget(dlg);
    d->m_uiAdvanced->setupUi(mainWidget);

    QDialogButtonBox *buttonBox = new QDialogButtonBox(dlg);
    buttonBox->setStandardButtons(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttonBox, &QDialogButtonBox::accepted, dlg, &QDialog::accept);
    connect(buttonBox, &QDialogButtonBox::rejected, dlg, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(dlg);
    layout->addWidget(mainWidget);
    layout->addWidget(buttonBox);

    // If the command is changed we reset certain settings that are strongly
    // coupled to the command.
    checkCommandChanged();

    // check to see if we use konsole if not do not add the nocloseonexit
    // because we don't know how to do this on other terminal applications
    KConfigGroup confGroup(KSharedConfig::openConfig(), QStringLiteral("General"));
    QString preferredTerminal = confGroup.readPathEntry("TerminalApplication", QStringLiteral("konsole"));

    bool terminalCloseBool = false;

    if (preferredTerminal == QLatin1String("konsole")) {
        terminalCloseBool = d->m_terminalOptionStr.contains(QLatin1String("--noclose"));
        d->m_uiAdvanced->terminalCloseCheck->setChecked(terminalCloseBool);
        d->m_terminalOptionStr.remove(QStringLiteral("--noclose"));
    } else {
        d->m_uiAdvanced->terminalCloseCheck->hide();
    }

    d->m_uiAdvanced->terminalCheck->setChecked(d->m_terminalBool);
    d->m_uiAdvanced->terminalEdit->setText(d->m_terminalOptionStr);
    d->m_uiAdvanced->terminalCloseCheck->setEnabled(d->m_terminalBool);
    d->m_uiAdvanced->terminalEdit->setEnabled(d->m_terminalBool);
    d->m_uiAdvanced->terminalEditLabel->setEnabled(d->m_terminalBool);

    d->m_uiAdvanced->suidCheck->setChecked(d->m_suidBool);
    d->m_uiAdvanced->suidEdit->setText(d->m_suidUserStr);
    d->m_uiAdvanced->suidEdit->setEnabled(d->m_suidBool);
    d->m_uiAdvanced->suidEditLabel->setEnabled(d->m_suidBool);

    if (KIO::hasDiscreteGpu()) {
        d->m_uiAdvanced->discreteGpuCheck->setChecked(d->m_runOnDiscreteGpuBool);
    } else {
        d->m_uiAdvanced->discreteGpuGroupBox->hide();
    }

    d->m_uiAdvanced->startupInfoCheck->setChecked(d->m_startupBool);

    // Provide username completion up to 1000 users.
    const int maxEntries = 1000;
    QStringList userNames = KUser::allUserNames(maxEntries);
    if (userNames.size() < maxEntries) {
        KCompletion *kcom = new KCompletion;
        kcom->setOrder(KCompletion::Sorted);
        d->m_uiAdvanced->suidEdit->setCompletionObject(kcom, true);
        d->m_uiAdvanced->suidEdit->setAutoDeleteCompletionObject(true);
        d->m_uiAdvanced->suidEdit->setCompletionMode(KCompletion::CompletionAuto);
        kcom->setItems(userNames);
    }

    connect(d->m_uiAdvanced->terminalEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->terminalCloseCheck, &QAbstractButton::toggled, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->terminalCheck, &QAbstractButton::toggled, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->suidCheck, &QAbstractButton::toggled, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->suidEdit, &QLineEdit::textChanged, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->discreteGpuCheck, &QAbstractButton::toggled, this, &KPropertiesDialogPlugin::changed);
    connect(d->m_uiAdvanced->startupInfoCheck, &QAbstractButton::toggled, this, &KPropertiesDialogPlugin::changed);

    QObject::connect(dlg, &QDialog::accepted, this, [this]() {
        d->m_terminalOptionStr = d->m_uiAdvanced->terminalEdit->text().trimmed();
        d->m_terminalBool = d->m_uiAdvanced->terminalCheck->isChecked();
        d->m_suidBool = d->m_uiAdvanced->suidCheck->isChecked();
        d->m_suidUserStr = d->m_uiAdvanced->suidEdit->text().trimmed();
        if (KIO::hasDiscreteGpu()) {
            d->m_runOnDiscreteGpuBool = d->m_uiAdvanced->discreteGpuCheck->isChecked();
        }
        d->m_startupBool = d->m_uiAdvanced->startupInfoCheck->isChecked();

        if (d->m_uiAdvanced->terminalCloseCheck->isChecked()) {
            d->m_terminalOptionStr.append(QLatin1String(" --noclose"));
        }
    });

    dlg->show();
}

bool KDesktopPropsPlugin::supports(const KFileItemList &_items)
{
    if (_items.count() != 1) {
        return false;
    }

    const KFileItem &item = _items.first();

    // check if desktop file
    if (!item.isDesktopFile()) {
        return false;
    }

    // open file and check type
    const auto [url, isLocal] = item.isMostLocalUrl();
    if (!isLocal) {
        return false;
    }

    KDesktopFile config(url.toLocalFile());
    return config.hasApplicationType() && KAuthorized::authorize(KAuthorized::RUN_DESKTOP_FILES) && KAuthorized::authorize(KAuthorized::SHELL_ACCESS);
}

QString KDesktopPropsPlugin::KDesktopPropsPluginPrivate::command() const
{
    QStringList execSplit = KShell::splitArgs(w->envarsEdit->text()) + QStringList(w->programEdit->text()) + KShell::splitArgs(w->argumentsEdit->text());

    if (KShell::splitArgs(w->envarsEdit->text()).length()) {
        execSplit.push_front(QLatin1String("env"));
    }

    return KShell::joinArgs(execSplit);
}

#include "moc_kpropertiesdialogbuiltin_p.cpp"
