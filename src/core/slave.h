// -*- c++ -*-
/*
    This file is part of the KDE libraries
    SPDX-FileCopyrightText: 2000 Waldo Bastian <bastian@kde.org>
    SPDX-FileCopyrightText: 2000 Stephan Kulow <coolo@kde.org>

    SPDX-License-Identifier: LGPL-2.0-only
*/

#ifndef KIO_SLAVE_H
#define KIO_SLAVE_H

#include "kio/slaveinterface.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QObject>

namespace KIO
{

class WorkerThread;
class SlaveKeeper;
class SimpleJob;
class Scheduler;
class SchedulerPrivate;
class DataProtocol;
class ConnectedSlaveQueue;
class ProtoQueue;
class SimpleJobPrivate;
class UserNotificationHandler;

// Do not use this class directly, outside of KIO. Only use the Slave pointer
// that is returned by the scheduler for passing it around.
class Slave : public KIO::SlaveInterface
{
    Q_OBJECT
public:
    explicit Slave(const QString &protocol, QObject *parent = nullptr);

    ~Slave() override;

    /**
     * Sends the given command to the kioslave.
     * Called by the jobs.
     * @param cmd command id
     * @param arr byte array containing data
     */
    virtual void send(int cmd, const QByteArray &arr = QByteArray());

    /**
     * The actual protocol used to handle the request.
     *
     * This method will return a different protocol than
     * the one obtained by using protocol() if a
     * proxy-server is used for the given protocol.  This
     * usually means that this method will return "http"
     * when the actual request was to retrieve a resource
     * from an "ftp" server by going through a proxy server.
     *
     * @return the actual protocol (io-slave) that handled the request
     */
    QString slaveProtocol() const;

    /**
     * @return Host this slave is (was?) connected to
     */
    QString host() const;

    /**
     * @return port this slave is (was?) connected to
     */
    quint16 port() const;

    /**
     * @return User this slave is (was?) logged in as
     */
    QString user() const;

    /**
     * @return Passwd used to log in
     */
    QString passwd() const;

    /**
     * Creates a new slave.
     *
     * @param protocol the protocol
     * @param url is the url
     * @param error is the error code on failure and undefined else.
     * @param error_text is the error text on failure and undefined else.
     *
     * @return 0 on failure, or a pointer to a slave otherwise.
     */
    static Slave *createSlave(const QString &protocol, const QUrl &url, int &error, QString &error_text);

    // == communication with connected kioslave ==
    // whenever possible prefer these methods over the respective
    // methods in connection()
    /**
     * Suspends the operation of the attached kioslave.
     */
    virtual void suspend();

    /**
     * Resumes the operation of the attached kioslave.
     */
    virtual void resume();

    /**
     * Tells whether the kioslave is suspended.
     * @return true if the kioslave is suspended.
     */
    virtual bool suspended();

    // == end communication with connected kioslave ==
private:
    friend class Scheduler;
    friend class SchedulerPrivate;
    friend class DataProtocol;
    friend class SlaveKeeper;
    friend class ConnectedSlaveQueue;
    friend class ProtoQueue;
    friend class SimpleJobPrivate;
    friend class UserNotificationHandler;

    void setPID(qint64);
    qint64 slave_pid() const;

    void setJob(KIO::SimpleJob *job);
    KIO::SimpleJob *job() const;

    /**
     * Force termination
     */
    void kill();

    /**
     * @return true if the slave survived the last mission.
     */
    bool isAlive() const;

    /**
     * Set host for url
     * @param host to connect to.
     * @param port to connect to.
     * @param user to login as
     * @param passwd to login with
     */
    virtual void setHost(const QString &host, quint16 port, const QString &user, const QString &passwd);

    /**
     * Clear host info.
     */
    void resetHost();

    /**
     * Configure slave
     */
    virtual void setConfig(const MetaData &config);

    /**
     * The protocol this slave handles.
     *
     * @return name of protocol handled by this slave, as seen by the user
     */
    QString protocol() const;

    void setProtocol(const QString &protocol);

    /**
     * Puts the kioslave associated with @p url at halt, and return it to klauncher, in order
     * to let another application connect to it and finish the job.
     * This is for the krunner case: type a URL in krunner, it will start downloading
     * to find the MIME type (KRun), and then hold the slave, publish the held slave using,
     * this method, and the final application can continue the same download by requesting
     * the same URL.
     */
    virtual void hold(const QUrl &url);

    /**
     * @return The number of seconds this slave has been idle.
     */
    int idleTime() const;

    /**
     * Marks this slave as idle.
     */
    void setIdle();

    void ref();
    void deref();
    void aboutToDelete();

    void setWorkerThread(WorkerThread *thread);

public Q_SLOTS: // TODO KF6: make all three slots private
    void accept();
    void gotInput();
    void timeout();

Q_SIGNALS:
    void slaveDied(KIO::Slave *slave);

private:
    WorkerThread *m_workerThread = nullptr; // only set for in-process workers
    QString m_protocol;
    QString m_slaveProtocol;
    QString m_host;
    QString m_user;
    QString m_passwd;
    KIO::ConnectionServer *m_slaveconnserver;
    KIO::SimpleJob *m_job = nullptr;
    qint64 m_pid = 0; // only set for out-of-process workers
    quint16 m_port = 0;
    bool m_dead = false;
    QElapsedTimer m_contact_started;
    QElapsedTimer m_idleSince;
    int m_refCount = 1;
};

}

#endif
