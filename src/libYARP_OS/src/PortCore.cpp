/*
 * Copyright (C) 2006-2019 Istituto Italiano di Tecnologia (IIT)
 * Copyright (C) 2006-2010 RobotCub Consortium
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms of the
 * BSD-3-Clause license. See the accompanying LICENSE file for details.
 */

#include <yarp/os/impl/PortCore.h>

#include <yarp/os/Bottle.h>
#include <yarp/os/DummyConnector.h>
#include <yarp/os/InputProtocol.h>
#include <yarp/os/Name.h>
#include <yarp/os/Network.h>
#include <yarp/os/PortInfo.h>
#include <yarp/os/RosNameSpace.h>
#include <yarp/os/StringOutputStream.h>
#include <yarp/os/SystemInfo.h>
#include <yarp/os/Time.h>
#include <yarp/os/impl/BufferedConnectionWriter.h>
#include <yarp/os/impl/ConnectionRecorder.h>
#include <yarp/os/impl/Logger.h>
#include <yarp/os/impl/PlatformUnistd.h>
#include <yarp/os/impl/PortCoreInputUnit.h>
#include <yarp/os/impl/PortCoreOutputUnit.h>
#include <yarp/os/impl/StreamConnectionReader.h>

#include <cstdio>
#include <vector>

#ifdef YARP_HAS_ACE
#    include <ace/INET_Addr.h>
#    include <ace/Sched_Params.h>
// In one the ACE headers there is a definition of "main" for WIN32
#    ifdef main
#        undef main
#    endif
#endif

#if defined(__linux__) // used for configuring scheduling properties
#    include <dirent.h>
#    include <sys/types.h>
#    include <unistd.h>
#endif


//#define YMSG(x) printf x;
//#define YTRACE(x) YMSG(("at %s\n", x))

#define YMSG(x)
#define YTRACE(x)

using namespace yarp::os::impl;
using namespace yarp::os;
using namespace yarp;

PortCore::PortCore() :
        m_stateSemaphore(1),
        m_packetMutex(),
        m_connectionChangeSemaphore(1),
        m_log("port", Logger::get()),
        m_face(nullptr),
        m_reader(nullptr),
        m_adminReader(nullptr),
        m_readableCreator(nullptr),
        m_eventReporter(nullptr),
        m_listening(false),
        m_running(false),
        m_starting(false),
        m_closing(false),
        m_finished(false),
        m_finishing(false),
        m_waitBeforeSend(true),
        m_waitAfterSend(true),
        m_controlRegistration(true),
        m_interruptible(true),
        m_interrupted(false),
        m_manual(false),
        m_events(0),
        m_connectionListeners(0),
        m_inputCount(0),
        m_outputCount(0),
        m_dataOutputCount(0),
        m_flags(PORTCORE_IS_INPUT | PORTCORE_IS_OUTPUT),
        verbosity(1),
        logNeeded(false),
        timeout(-1),
        counter(1),
        prop(nullptr),
        contactable(nullptr),
        mutex(nullptr),
        mutexOwned(false),
        envelopeWriter(true),
        typeMutex(),
        checkedType(false)
{
}

PortCore::~PortCore()
{
    close();
    removeCallbackLock();
}


bool PortCore::listen(const Contact& address, bool shouldAnnounce)
{
    // If we're using ACE, we really need to have it initialized before
    // this point.
    if (!NetworkBase::initialized()) {
        YARP_ERROR(m_log, "YARP not initialized; create a yarp::os::Network object before using ports");
        return false;
    }

    YTRACE("PortCore::listen");

    m_stateSemaphore.wait();

    // This method assumes we are not already on the network.
    // We can assume this because it is not a user-facing class,
    // and we carefully never call this method again without
    // calling close().
    yAssert(m_listening == false);
    yAssert(m_running == false);
    yAssert(m_closing == false);
    yAssert(m_finished == false);
    yAssert(m_face == nullptr);

    // Try to put the port on the network, using the user-supplied
    // address (which may be incomplete).  You can think of
    // this as getting a server socket.
    this->m_address = address;
    setName(address.getRegName());
    if (timeout > 0) {
        this->m_address.setTimeout(timeout);
    }
    m_face = Carriers::listen(this->m_address);

    // We failed, abort.
    if (m_face == nullptr) {
        m_stateSemaphore.post();
        return false;
    }

    // Update our address if it was incomplete.
    if (this->m_address.getPort() <= 0) {
        this->m_address = m_face->getLocalAddress();
        if (this->m_address.getRegName() == "...") {
            this->m_address.setName(std::string("/") + this->m_address.getHost() + "_" + NetType::toString(this->m_address.getPort()));
            setName(this->m_address.getRegName());
        }
    }

    // Move into listening phase
    m_listening = true;
    m_log.setPrefix(address.getRegName().c_str());
    m_stateSemaphore.post();

    // Now that we are on the network, we can let the name server know this.
    if (shouldAnnounce) {
        if (!(NetworkBase::getLocalMode() && NetworkBase::getQueryBypass() == nullptr)) {
            std::string portName = address.getRegName();
            Bottle cmd;
            Bottle reply;
            cmd.addString("announce");
            cmd.addString(portName);
            ContactStyle style;
            NetworkBase::writeToNameServer(cmd, reply, style);
        }
    }

    // Success!
    return true;
}


void PortCore::setReadHandler(PortReader& reader)
{
    // Don't even try to do this when the port is hot, it'll burn you
    yAssert(m_running == false);
    yAssert(this->m_reader == nullptr);
    this->m_reader = &reader;
}

void PortCore::setAdminReadHandler(PortReader& reader)
{
    // Don't even try to do this when the port is hot, it'll burn you
    yAssert(m_running == false);
    yAssert(this->m_adminReader == nullptr);
    this->m_adminReader = &reader;
}

void PortCore::setReadCreator(PortReaderCreator& creator)
{
    // Don't even try to do this when the port is hot, it'll burn you
    yAssert(m_running == false);
    yAssert(this->m_readableCreator == nullptr);
    this->m_readableCreator = &creator;
}


void PortCore::run()
{
    YTRACE("PortCore::run");

    // This is the server thread for the port.  We listen on
    // the network and handle any incoming connections.
    // We don't touch those connections, just shove them
    // in a list and move on.  It is important that this
    // thread doesn't make a connecting client wait just
    // because some other client is slow.

    // We assume that listen() has succeeded and that
    // start() has been called.
    yAssert(m_listening == true);
    yAssert(m_running == false);
    yAssert(m_closing == false);
    yAssert(m_finished == false);
    yAssert(m_starting == true);

    // Enter running phase
    m_running = true;
    m_starting = false;

    // This post is matched with a wait in start()
    m_stateSemaphore.post();

    YTRACE("PortCore::run running");

    // Enter main loop, where we block on incoming connections.
    // The loop is exited when PortCore#closing is set.  One last
    // connection will be needed to de-block this thread and ensure
    // that it checks PortCore#closing.
    bool shouldStop = false;
    while (!shouldStop) {

        // Block and wait for a connection
        InputProtocol* ip = nullptr;
        ip = m_face->read();

        m_stateSemaphore.wait();

        // Attach the connection to this port and update its timeout setting
        if (ip != nullptr) {
            ip->attachPort(contactable);
            YARP_DEBUG(m_log, "PortCore received something");
            if (timeout > 0) {
                ip->setTimeout(timeout);
            }
        }

        // Check whether we should shut down
        shouldStop |= m_closing;

        // Increment a global count of connection events
        m_events++;

        m_stateSemaphore.post();

        // It we are not shutting down, spin off the connection.
        // It starts life as an input connection (although it
        // may later morph into an output).
        if (!shouldStop) {
            if (ip != nullptr) {
                addInput(ip);
            }
            YARP_DEBUG(m_log, "PortCore spun off a connection");
            ip = nullptr;
        }

        // If the connection wasn't spun off, just shut it down.
        if (ip != nullptr) {
            ip->close();
            delete ip;
            ip = nullptr;
        }

        // Remove any defunct connections.
        reapUnits();

        // Notify anyone listening for connection changes.
        // This should be using a condition variable once we have them,
        // this is not solid TODO
        m_stateSemaphore.wait();
        for (int i = 0; i < m_connectionListeners; i++) {
            m_connectionChangeSemaphore.post();
        }
        m_connectionListeners = 0;
        m_stateSemaphore.post();
    }

    YTRACE("PortCore::run closing");

    // The server thread is shutting down.
    m_stateSemaphore.wait();
    for (int i = 0; i < m_connectionListeners; i++) {
        m_connectionChangeSemaphore.post();
    }
    m_connectionListeners = 0;
    m_finished = true;
    m_stateSemaphore.post();
}


void PortCore::close()
{
    closeMain();

    if (prop != nullptr) {
        delete prop;
        prop = nullptr;
    }
    modifier.releaseOutModifier();
    modifier.releaseInModifier();
}


bool PortCore::start()
{
    YTRACE("PortCore::start");

    // This wait will, on success, be matched by a post in run()
    m_stateSemaphore.wait();

    // We assume that listen() has been called.
    yAssert(m_listening == true);
    yAssert(m_running == false);
    yAssert(m_starting == false);
    yAssert(m_finished == false);
    yAssert(m_closing == false);
    m_starting = true;

    // Start the server thread.
    bool started = ThreadImpl::start();
    if (!started) {
        // run() won't be happening
        m_stateSemaphore.post();
    } else {
        // run() will signal stateSema once it is active
        m_stateSemaphore.wait();
        yAssert(m_running == true);

        // release stateSema for its normal task of controlling access to state
        m_stateSemaphore.post();
    }
    return started;
}


bool PortCore::manualStart(const char* sourceName)
{
    // This variant of start() does not create a server thread.
    // That is appropriate if we never expect to receive incoming
    // connections for any reason.  No incoming data, no requests
    // for state information, no requests to change connections,
    // nothing.  We set the port's name to something fake, and
    // act like nothing is wrong.
    m_interruptible = false;
    m_manual = true;
    setName(sourceName);
    return true;
}


void PortCore::resume()
{
    // We are no longer interrupted.
    m_interrupted = false;
}

void PortCore::interrupt()
{
    // This is a no-op if there is no server thread.
    if (!m_listening) {
        return;
    }

    // Ignore any future incoming data
    m_interrupted = true;

    // What about data that is already coming in?
    // If interruptible is not currently set, no worries, the user
    // did not or will not end up blocked on a read.
    if (!m_interruptible) {
        return;
    }

    // Since interruptible is set, it is possible that the user
    // may be blocked on a read.  We send an empty message,
    // which is reserved for giving blocking readers a chance to
    // update their state.
    m_stateSemaphore.wait();
    if (m_reader != nullptr) {
        YARP_DEBUG(m_log, "sending update-state message to listener");
        StreamConnectionReader sbr;
        lockCallback();
        m_reader->read(sbr);
        unlockCallback();
    }
    m_stateSemaphore.post();
}


void PortCore::closeMain()
{
    YTRACE("PortCore::closeMain");

    m_stateSemaphore.wait();

    // We may not have anything to do.
    if (m_finishing || !(m_running || m_manual)) {
        YTRACE("PortCore::closeMainNothingToDo");
        m_stateSemaphore.post();
        return;
    }

    YTRACE("PortCore::closeMainCentral");

    // Move into official "finishing" phase.
    m_finishing = true;
    YARP_DEBUG(m_log, "now preparing to shut down port");
    m_stateSemaphore.post();

    // Start disconnecting inputs.  We ask the other side of the
    // connection to do this, so it won't come as a surprise.
    // The details of how disconnection works vary by carrier.
    // While we are doing this, the server thread may be still running.
    // This is because other ports may need to talk to the server
    // to organize details of how a connection should be broken down.
    bool done = false;
    std::string prevName;
    while (!done) {
        done = true;
        std::string removeName;
        m_stateSemaphore.wait();
        for (auto unit : m_units) {
            if ((unit != nullptr) && unit->isInput() &&!unit->isDoomed()) {
                Route r = unit->getRoute();
                std::string s = r.getFromName();
                if (s.length() >= 1 && s[0] == '/' && s != getName() && s != prevName) {
                    removeName = s;
                    done = false;
                    break;
                }
            }
        }
        m_stateSemaphore.post();
        if (!done) {
            YARP_DEBUG(m_log, std::string("requesting removal of connection from ") + removeName);
            bool result = NetworkBase::disconnect(removeName,
                                                  getName(),
                                                  true);
            if (!result) {
                NetworkBase::disconnectInput(getName(),
                                             removeName,
                                             true);
            }
            prevName = removeName;
        }
    }

    // Start disconnecting outputs.  We don't negotiate with the
    // other side, we just break them down.
    done = false;
    while (!done) {
        done = true;
        Route removeRoute;
        m_stateSemaphore.wait();
        for (auto unit : m_units) {
            if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
                removeRoute = unit->getRoute();
                if (removeRoute.getFromName() == getName()) {
                    done = false;
                    break;
                }
            }
        }
        m_stateSemaphore.post();
        if (!done) {
            removeUnit(removeRoute, true);
        }
    }

    m_stateSemaphore.wait();
    bool stopRunning = m_running;
    m_stateSemaphore.post();

    // If the server thread is still running, we need to bring it down.
    if (stopRunning) {
        // Let the server thread know we no longer need its services.
        m_stateSemaphore.wait();
        m_closing = true;
        m_stateSemaphore.post();

        // Wake up the server thread the only way we can, by sending
        // a message to it.  Then join it, it is done.
        if (!m_manual) {
            OutputProtocol* op = m_face->write(m_address);
            if (op != nullptr) {
                op->close();
                delete op;
            }
            join();
        }

        // We should be finished now.
        m_stateSemaphore.wait();
        yAssert(m_finished == true);
        m_stateSemaphore.post();

        // Clean up our connection list. We couldn't do this earlier,
        // because the server thread was running.
        closeUnits();

        // Reset some state flags.
        m_stateSemaphore.wait();
        m_finished = false;
        m_closing = false;
        m_running = false;
        m_stateSemaphore.post();
    }

    // There should be no other threads at this point and we
    // can stop listening on the network.
    if (m_listening) {
        yAssert(m_face != nullptr);
        m_face->close();
        delete m_face;
        m_face = nullptr;
        m_listening = false;
    }

    // Check if the client is waiting for input.  If so, wake them up
    // with the bad news.  An empty message signifies a request to
    // check the port state.
    if (m_reader != nullptr) {
        YARP_DEBUG(m_log, "sending end-of-port message to listener");
        StreamConnectionReader sbr;
        m_reader->read(sbr);
        m_reader = nullptr;
    }

    // We may need to unregister the port with the name server.
    if (stopRunning) {
        std::string name = getName();
        if (name != std::string("")) {
            if (m_controlRegistration) {
                NetworkBase::unregisterName(name);
            }
        }
    }

    // We are done with the finishing process.
    m_finishing = false;

    // We are fresh as a daisy.
    yAssert(m_listening == false);
    yAssert(m_running == false);
    yAssert(m_starting == false);
    yAssert(m_closing == false);
    yAssert(m_finished == false);
    yAssert(m_finishing == false);
    yAssert(m_face == nullptr);
}


int PortCore::getEventCount()
{
    // How many times has the server thread spun off a connection.
    m_stateSemaphore.wait();
    int ct = m_events;
    m_stateSemaphore.post();
    return ct;
}


void PortCore::closeUnits()
{
    // Empty the PortCore#units list. This is only possible when
    // the server thread is finished.
    m_stateSemaphore.wait();
    yAssert(m_finished == true);
    m_stateSemaphore.post();

    // In the "finished" phase, nobody else touches the units,
    // so we can go ahead and shut them down and delete them.
    for (auto& i : m_units) {
        PortCoreUnit* unit = i;
        if (unit != nullptr) {
            YARP_DEBUG(m_log, "closing a unit");
            unit->close();
            YARP_DEBUG(m_log, "joining a unit");
            unit->join();
            delete unit;
            YARP_DEBUG(m_log, "deleting a unit");
            i = nullptr;
        }
    }

    // Get rid of all our nulls.  Done!
    m_units.clear();
}

void PortCore::reapUnits()
{
    // Connections that should be shut down get tagged as "doomed"
    // but aren't otherwise touched until it is safe to do so.
    m_stateSemaphore.wait();
    if (!m_finished) {
        for (auto unit : m_units) {
            if ((unit != nullptr) && unit->isDoomed() && !unit->isFinished()) {
                std::string s = unit->getRoute().toString();
                YARP_DEBUG(m_log, std::string("Informing connection ") + s + " that it is doomed");
                unit->close();
                YARP_DEBUG(m_log, std::string("Closed connection ") + s);
                unit->join();
                YARP_DEBUG(m_log, std::string("Joined thread of connection ") + s);
            }
        }
    }
    m_stateSemaphore.post();
    cleanUnits();
}

void PortCore::cleanUnits(bool blocking)
{
    // We will remove any connections that have finished operating from
    // the PortCore#units list.

    // Depending on urgency, either wait for a safe time to do this
    // or skip if unsafe.
    if (blocking) {
        m_stateSemaphore.wait();
    } else {
        blocking = m_stateSemaphore.check();
        if (!blocking) {
            return;
        }
    }

    // We will update our connection counts as a by-product.
    int updatedInputCount = 0;
    int updatedOutputCount = 0;
    int updatedDataOutputCount = 0;
    YARP_DEBUG(m_log, "/ routine check of connections to this port begins");
    if (!m_finished) {

        // First, we delete and null out any defunct entries in the list.
        for (auto& i : m_units) {
            PortCoreUnit* unit = i;
            if (unit != nullptr) {
                YARP_DEBUG(m_log, std::string("| checking connection ") + unit->getRoute().toString() + " " + unit->getMode());
                if (unit->isFinished()) {
                    std::string con = unit->getRoute().toString();
                    YARP_DEBUG(m_log, std::string("|   removing connection ") + con);
                    unit->close();
                    unit->join();
                    delete unit;
                    i = nullptr;
                    YARP_DEBUG(m_log, std::string("|   removed connection ") + con);
                } else {
                    // No work to do except updating connection counts.
                    if (!unit->isDoomed()) {
                        if (unit->isOutput()) {
                            updatedOutputCount++;
                            if (unit->getMode().empty()) {
                                updatedDataOutputCount++;
                            }
                        }
                        if (unit->isInput()) {
                            if (unit->getRoute().getFromName() != "admin") {
                                updatedInputCount++;
                            }
                        }
                    }
                }
            }
        }

        // Now we do some awkward shuffling (list class may be from ACE
        // or STL, if ACE it is quite limited).  We move the nulls to
        // the end of the list ...
        unsigned int rem = 0;
        for (unsigned int i2 = 0; i2 < m_units.size(); i2++) {
            if (m_units[i2] != nullptr) {
                if (rem < i2) {
                    m_units[rem] = m_units[i2];
                    m_units[i2] = nullptr;
                }
                rem++;
            }
        }

        // ... Now we get rid of the null entries
        for (unsigned int i3 = 0; i3 < m_units.size() - rem; i3++) {
            m_units.pop_back();
        }
    }

    // Finalize the connection counts.
    m_dataOutputCount = updatedDataOutputCount;
    m_stateSemaphore.post();
    m_packetMutex.lock();
    m_inputCount = updatedInputCount;
    m_outputCount = updatedOutputCount;
    m_packetMutex.unlock();
    YARP_DEBUG(m_log, "\\ routine check of connections to this port ends");
}


void PortCore::addInput(InputProtocol* ip)
{
    // This method is only called by the server thread in its running phase.
    // It wraps up a network connection as a unit and adds it to
    // PortCore#units.  The unit will have its own thread associated
    // with it.

    yAssert(ip != nullptr);
    m_stateSemaphore.wait();
    PortCoreUnit* unit = new PortCoreInputUnit(*this,
                                               getNextIndex(),
                                               ip,
                                               false);
    yAssert(unit != nullptr);
    unit->start();
    m_units.push_back(unit);
    YMSG(("there are now %d units\n", (int)m_units.size()));
    m_stateSemaphore.post();
}


void PortCore::addOutput(OutputProtocol* op)
{
    // This method is called from threads associated with input
    // connections.
    // It wraps up a network connection as a unit and adds it to
    // PortCore#units.  The unit will start with its own thread
    // associated with it, but that thread will be very short-lived
    // if the port is not configured to do background writes.

    yAssert(op != nullptr);
    m_stateSemaphore.wait();
    if (!m_finished) {
        PortCoreUnit* unit = new PortCoreOutputUnit(*this, getNextIndex(), op);
        yAssert(unit != nullptr);
        unit->start();
        m_units.push_back(unit);
    }
    m_stateSemaphore.post();
}


bool PortCore::isUnit(const Route& route, int index)
{
    // Check if a connection with a specified route (and optional ID) is present
    bool needReap = false;
    if (!m_finished) {
        for (auto unit : m_units) {
            if (unit != nullptr) {
                Route alt = unit->getRoute();
                std::string wild = "*";
                bool ok = true;
                if (index >= 0) {
                    ok = ok && (unit->getIndex() == index);
                }
                if (ok) {
                    if (route.getFromName() != wild) {
                        ok = ok && (route.getFromName() == alt.getFromName());
                    }
                    if (route.getToName() != wild) {
                        ok = ok && (route.getToName() == alt.getToName());
                    }
                    if (route.getCarrierName() != wild) {
                        ok = ok && (route.getCarrierName() == alt.getCarrierName());
                    }
                }
                if (ok) {
                    needReap = true;
                    break;
                }
            }
        }
    }
    return needReap;
}


bool PortCore::removeUnit(const Route& route, bool synch, bool* except)
{
    // This is a request to remove a connection.  It could arise from any
    // input thread.

    if (except != nullptr) {
        YARP_DEBUG(m_log, std::string("asked to remove connection in the way of ") + route.toString());
        *except = false;
    } else {
        YARP_DEBUG(m_log, std::string("asked to remove connection ") + route.toString());
    }

    // Scan for units that match the given route, and collect their IDs.
    // Mark them as "doomed".
    std::vector<int> removals;
    m_stateSemaphore.wait();
    bool needReap = false;
    if (!m_finished) {
        for (auto unit : m_units) {
            if (unit != nullptr) {
                Route alt = unit->getRoute();
                std::string wild = "*";
                bool ok = true;
                if (route.getFromName() != wild) {
                    ok = ok && (route.getFromName() == alt.getFromName());
                }
                if (route.getToName() != wild) {
                    ok = ok && (route.getToName() == alt.getToName());
                }
                if (route.getCarrierName() != wild) {
                    if (except == nullptr) {
                        ok = ok && (route.getCarrierName() == alt.getCarrierName());
                    } else {
                        if (route.getCarrierName() == alt.getCarrierName()) {
                            *except = true;
                            ok = false;
                        }
                    }
                }

                if (ok) {
                    YARP_DEBUG(m_log,
                               std::string("removing connection ") + alt.toString());
                    removals.push_back(unit->getIndex());
                    unit->setDoomed();
                    needReap = true;
                }
            }
        }
    }
    m_stateSemaphore.post();

    // If we find one or more matches, we need to do some work.
    // We've marked those matches as "doomed" so they'll get cleared
    // up eventually, but we can speed this up by waking up the
    // server thread.
    if (needReap) {
        YARP_DEBUG(m_log, "one or more connections need prodding to die");

        if (m_manual) {
            // No server thread, no problems.
            reapUnits();
        } else {
            // Send a blank message to make up server thread.
            OutputProtocol* op = m_face->write(m_address);
            if (op != nullptr) {
                op->close();
                delete op;
            }
            YARP_DEBUG(m_log, "sent message to prod connection death");

            if (synch) {
                // Wait for connections to be cleaned up.
                YARP_DEBUG(m_log, "synchronizing with connection death");
                bool cont = false;
                do {
                    m_stateSemaphore.wait();
                    for (int removal : removals) {
                        cont = isUnit(route, removal);
                        if (cont) {
                            break;
                        }
                    }
                    if (cont) {
                        m_connectionListeners++;
                    }
                    m_stateSemaphore.post();
                    if (cont) {
                        m_connectionChangeSemaphore.wait();
                    }
                } while (cont);
            }
        }
    }
    return needReap;
}


bool PortCore::addOutput(const std::string& dest,
                         void* id,
                         OutputStream* os,
                         bool onlyIfNeeded)
{
    YARP_UNUSED(id);
    YARP_DEBUG(m_log, std::string("asked to add output to ") + dest);

    // Buffer to store text describing outcome (successful connection,
    // or a failure).
    BufferedConnectionWriter bw(true);

    // Look up the address we'll be connectioning to.
    Contact parts = Name(dest).toAddress();
    Contact contact = NetworkBase::queryName(parts.getRegName());
    Contact address = contact;

    // If we can't find it, say so and abort.
    if (!address.isValid()) {
        bw.appendLine(std::string("Do not know how to connect to ") + dest);
        if (os != nullptr) {
            bw.write(*os);
        }
        return false;
    }

    // We clean all existing connections to the desired destination,
    // optionally stopping if we find one with the right carrier.
    if (onlyIfNeeded) {
        // Remove any existing connections between source and destination
        // with a different carrier.  If we find a connection already
        // present with the correct carrier, then we are done.
        bool except = false;
        removeUnit(Route(getName(), address.getRegName(), address.getCarrier()), true, &except);
        if (except) {
            // Connection already present.
            YARP_DEBUG(m_log, std::string("output already present to ") + dest);
            bw.appendLine(std::string("Desired connection already present from ") + getName() + " to " + dest);
            if (os != nullptr) {
                bw.write(*os);
            }
            return true;
        }
    } else {
        // Remove any existing connections between source and destination.
        removeUnit(Route(getName(), address.getRegName(), "*"), true);
    }

    // Set up a named route for this connection.
    std::string aname = address.getRegName();
    if (aname.empty()) {
        aname = address.toURI(false);
    }
    Route r(getName(),
            aname,
            ((!parts.getCarrier().empty()) ? parts.getCarrier() : address.getCarrier()));
    r.setToContact(contact);

    // Check for any restrictions on the port.  Perhaps it can only
    // read, or write.
    bool allowed = true;
    std::string err;
    std::string append;
    int f = getFlags();
    bool allow_output = (f & PORTCORE_IS_OUTPUT) != 0;
    bool rpc = (f & PORTCORE_IS_RPC) != 0;
    Name name(r.getCarrierName() + std::string("://test"));
    std::string mode = name.getCarrierModifier("log");
    bool is_log = (!mode.empty());
    if (is_log) {
        if (mode != "in") {
            err = "Logger configured as log." + mode + ", but only log.in is supported";
            allowed = false;
        } else {
            append = "; " + r.getFromName() + " will forward messages and replies (if any) to " + r.getToName();
        }
    }
    if (!allow_output) {
        if (!is_log) {
            bool push = false;
            Carrier* c = Carriers::getCarrierTemplate(r.getCarrierName());
            if (c != nullptr) {
                push = c->isPush();
            }
            if (push) {
                err = "Outputs not allowed";
                allowed = false;
            }
        }
    } else if (rpc) {
        if (m_dataOutputCount >= 1 && !is_log) {
            err = "RPC output already connected";
            allowed = false;
        }
    }

    // If we found a relevant restriction, abort.
    if (!allowed) {
        bw.appendLine(err);
        if (os != nullptr) {
            bw.write(*os);
        }
        return false;
    }

    // Ok! We can go ahead and make a connection.
    OutputProtocol* op = nullptr;
    if (timeout > 0) {
        address.setTimeout(timeout);
    }
    op = Carriers::connect(address);
    if (op != nullptr) {
        op->attachPort(contactable);
        if (timeout > 0) {
            op->setTimeout(timeout);
        }

        bool ok = op->open(r);
        if (!ok) {
            YARP_DEBUG(m_log, "open route error");
            delete op;
            op = nullptr;
        }
    }

    // No connection, abort.
    if (op == nullptr) {
        bw.appendLine(std::string("Cannot connect to ") + dest);
        if (os != nullptr) {
            bw.write(*os);
        }
        return false;
    }

    // Ok, we have a connection, now add it to PortCore#units
    if (op->getConnection().isPush()) {
        // This is the normal case
        addOutput(op);
    } else {
        // This is the case for connections that are initiated
        // in the opposite direction to native YARP connections.
        // Native YARP has push connections, initiated by the
        // sender.  HTTP and ROS have pull connections, initiated
        // by the receiver.
        // We invert the route, flip the protocol direction, and add.
        r.swapNames();
        op->rename(r);
        InputProtocol* ip = &(op->getInput());
        m_stateSemaphore.wait();
        if (!m_finished) {
            PortCoreUnit* unit = new PortCoreInputUnit(*this,
                                                       getNextIndex(),
                                                       ip,
                                                       true);
            yAssert(unit != nullptr);
            unit->start();
            m_units.push_back(unit);
        }
        m_stateSemaphore.post();
    }

    // Communicated the good news.
    bw.appendLine(std::string("Added connection from ") + getName() + " to " + dest + append);
    if (os != nullptr) {
        bw.write(*os);
    }
    cleanUnits();
    return true;
}


void PortCore::removeOutput(const std::string& dest, void* id, OutputStream* os)
{
    YARP_UNUSED(id);
    // All the real work done by removeUnit().
    BufferedConnectionWriter bw(true);
    if (removeUnit(Route("*", dest, "*"), true)) {
        bw.appendLine(std::string("Removed connection from ") + getName() + " to " + dest);
    } else {
        bw.appendLine(std::string("Could not find an outgoing connection to ") + dest);
    }
    if (os != nullptr) {
        bw.write(*os);
    }
    cleanUnits();
}

void PortCore::removeInput(const std::string& src, void* id, OutputStream* os)
{
    YARP_UNUSED(id);
    // All the real work done by removeUnit().
    BufferedConnectionWriter bw(true);
    if (removeUnit(Route(src, "*", "*"), true)) {
        bw.appendLine(std::string("Removing connection from ") + src + " to " + getName());
    } else {
        bw.appendLine(std::string("Could not find an incoming connection from ") + src);
    }
    if (os != nullptr) {
        bw.write(*os);
    }
    cleanUnits();
}

void PortCore::describe(void* id, OutputStream* os)
{
    YARP_UNUSED(id);
    cleanUnits();

    // Buffer to store a human-readable description of the port's
    // state.
    BufferedConnectionWriter bw(true);

    m_stateSemaphore.wait();

    // Report name and address.
    bw.appendLine(std::string("This is ") + m_address.getRegName() + " at " + m_address.toURI());

    // Report outgoing connections.
    int oct = 0;
    for (auto unit : m_units) {
        if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
            Route route = unit->getRoute();
            std::string msg = "There is an output connection from " + route.getFromName() + " to " + route.getToName() + " using " + route.getCarrierName();
            bw.appendLine(msg);
            oct++;
        }
    }
    if (oct < 1) {
        bw.appendLine("There are no outgoing connections");
    }

    // Report incoming connections.
    int ict = 0;
    for (auto unit : m_units) {
        if ((unit != nullptr) && unit->isInput() && !unit->isFinished()) {
            Route route = unit->getRoute();
            if (!route.getCarrierName().empty()) {
                std::string msg = "There is an input connection from " + route.getFromName() + " to " + route.getToName() + " using " + route.getCarrierName();
                bw.appendLine(msg);
                ict++;
            }
        }
    }
    if (ict < 1) {
        bw.appendLine("There are no incoming connections");
    }

    m_stateSemaphore.post();

    // Send description across network, or print it.
    if (os != nullptr) {
        bw.write(*os);
    } else {
        StringOutputStream sos;
        bw.write(sos);
        printf("%s\n", sos.toString().c_str());
    }
}


void PortCore::describe(PortReport& reporter)
{
    cleanUnits();

    m_stateSemaphore.wait();

    // Report name and address of port.
    PortInfo baseInfo;
    baseInfo.tag = yarp::os::PortInfo::PORTINFO_MISC;
    std::string portName = m_address.getRegName();
    baseInfo.message = std::string("This is ") + portName + " at " + m_address.toURI();
    reporter.report(baseInfo);

    // Report outgoing connections.
    int oct = 0;
    for (auto unit : m_units) {
        if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
            Route route = unit->getRoute();
            std::string msg = "There is an output connection from " + route.getFromName() + " to " + route.getToName() + " using " + route.getCarrierName();
            PortInfo info;
            info.message = msg;
            info.tag = yarp::os::PortInfo::PORTINFO_CONNECTION;
            info.incoming = false;
            info.portName = portName;
            info.sourceName = route.getFromName();
            info.targetName = route.getToName();
            info.carrierName = route.getCarrierName();
            reporter.report(info);
            oct++;
        }
    }
    if (oct < 1) {
        PortInfo info;
        info.tag = yarp::os::PortInfo::PORTINFO_MISC;
        info.message = "There are no outgoing connections";
        reporter.report(info);
    }

    // Report incoming connections.
    int ict = 0;
    for (auto unit : m_units) {
        if ((unit != nullptr) && unit->isInput() && !unit->isFinished()) {
            Route route = unit->getRoute();
            std::string msg = "There is an input connection from " + route.getFromName() + " to " + route.getToName() + " using " + route.getCarrierName();
            PortInfo info;
            info.message = msg;
            info.tag = yarp::os::PortInfo::PORTINFO_CONNECTION;
            info.incoming = true;
            info.portName = portName;
            info.sourceName = route.getFromName();
            info.targetName = route.getToName();
            info.carrierName = route.getCarrierName();
            reporter.report(info);
            ict++;
        }
    }
    if (ict < 1) {
        PortInfo info;
        info.tag = yarp::os::PortInfo::PORTINFO_MISC;
        info.message = "There are no incoming connections";
        reporter.report(info);
    }

    m_stateSemaphore.post();
}


void PortCore::setReportCallback(yarp::os::PortReport* reporter)
{
    m_stateSemaphore.wait();
    if (reporter != nullptr) {
        m_eventReporter = reporter;
    }
    m_stateSemaphore.post();
}

void PortCore::resetReportCallback()
{
    m_stateSemaphore.wait();
    m_eventReporter = nullptr;
    m_stateSemaphore.post();
}

void PortCore::report(const PortInfo& info)
{
    // We are in the context of one of the input or output threads,
    // so our contact with the PortCore must be absolutely minimal.
    //
    // It is safe to pick up the address of the reporter if this is
    // kept constant over the lifetime of the input/output threads.

    if (m_eventReporter != nullptr) {
        m_eventReporter->report(info);
    }
}


bool PortCore::readBlock(ConnectionReader& reader, void* id, OutputStream* os)
{
    YARP_UNUSED(id);
    YARP_UNUSED(os);
    bool result = true;

    // We are in the context of one of the input threads,
    // so our contact with the PortCore must be absolutely minimal.
    //
    // It is safe to pick up the address of the reader since this is
    // constant over the lifetime of the input threads.

    if (this->m_reader != nullptr && !m_interrupted) {
        m_interruptible = false; // No mutexing; user of interrupt() has to be careful.

        bool haveOutputs = (m_outputCount != 0); // No mutexing, but failure modes are benign.

        if (logNeeded && haveOutputs) {
            // Normally, yarp doesn't pay attention to the content of
            // messages received by the client.  Likewise, the content
            // of replies are not monitored.  However it may sometimes
            // be useful to log this traffic.

            ConnectionRecorder recorder;
            recorder.init(&reader);
            lockCallback();
            result = this->m_reader->read(recorder);
            unlockCallback();
            recorder.fini();
            // send off a log of this transaction to whoever wants it
            sendHelper(recorder, PORTCORE_SEND_LOG);
        } else {
            // YARP is not needed as a middleman
            lockCallback();
            result = this->m_reader->read(reader);
            unlockCallback();
        }

        m_interruptible = true;
    } else {
        // Read and ignore message, there is no where to send it.
        YARP_DEBUG(Logger::get(), "data received in PortCore, no reader for it");
        Bottle b;
        result = b.read(reader);
    }
    return result;
}


bool PortCore::send(const PortWriter& writer,
                    PortReader* reader,
                    const PortWriter* callback)
{
    // check if there is any modifier
    // we need to protect this part while the modifier
    // plugin is loading or unloading!
    modifier.outputMutex.lock();
    if (modifier.outputModifier != nullptr) {
        if (!modifier.outputModifier->acceptOutgoingData(writer)) {
            modifier.outputMutex.unlock();
            return false;
        }
        modifier.outputModifier->modifyOutgoingData(writer);
    }
    modifier.outputMutex.unlock();
    if (!logNeeded) {
        return sendHelper(writer, PORTCORE_SEND_NORMAL, reader, callback);
    }
    // logging is desired, so we need to wrap up and log this send
    // (and any reply it gets)  -- TODO not yet supported
    return sendHelper(writer, PORTCORE_SEND_NORMAL, reader, callback);
}

bool PortCore::sendHelper(const PortWriter& writer,
                          int mode,
                          PortReader* reader,
                          const PortWriter* callback)
{
    if (m_interrupted || m_finishing) {
        return false;
    }

    bool all_ok = true;
    bool gotReply = false;
    int logCount = 0;
    std::string envelopeString = envelope;

    // Pass a message to all output units for sending on.  We could
    // be doing more here to cache the serialization of the message
    // and reuse it across output connections.  However, one key
    // optimization is present: external blocks written by
    // yarp::os::ConnectionWriter::appendExternalBlock are never
    // copied.  So for example the core image array in a yarp::sig::Image
    // is untouched by the port communications code.

    YMSG(("------- send in real\n"));

    // Give user the chance to know that this object is about to be
    // written.
    writer.onCommencement();

    // All user-facing parts of this port will be blocked on this
    // operation, so we'll want to be snappy. How long the
    // operation lasts will depend on these flags:
    //   * waitAfterSend
    //   * waitBeforeSend
    // set by setWaitAfterSend() and setWaitBeforeSend().
    m_stateSemaphore.wait();

    // If the port is shutting down, abort.
    if (m_finished) {
        m_stateSemaphore.post();
        return false;
    }

    YMSG(("------- send in\n"));
    // Prepare a "packet" for tracking a single message which
    // may travel by multiple outputs.
    m_packetMutex.lock();
    PortCorePacket* packet = packets.getFreePacket();
    yAssert(packet != nullptr);
    packet->setContent(&writer, false, callback);
    m_packetMutex.unlock();

    // Scan connections, placing message everyhere we can.
    for (auto unit : m_units) {
        if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
            bool log = (!unit->getMode().empty());
            if (log) {
                // Some connections are for logging only.
                logCount++;
            }
            bool ok = (mode == PORTCORE_SEND_NORMAL) ? (!log) : (log);
            if (!ok) {
                continue;
            }
            bool waiter = m_waitAfterSend || (mode == PORTCORE_SEND_LOG);
            YMSG(("------- -- inc\n"));
            m_packetMutex.lock();
            packet->inc(); // One more connection carrying message.
            m_packetMutex.unlock();
            YMSG(("------- -- presend\n"));
            bool gotReplyOne = false;
            // Send the message off on this connection.
            void* out = unit->send(writer,
                                   reader,
                                   (callback != nullptr) ? callback : (&writer),
                                   (void*)packet,
                                   envelopeString,
                                   waiter,
                                   m_waitBeforeSend,
                                   &gotReplyOne);
            gotReply = gotReply || gotReplyOne;
            YMSG(("------- -- send\n"));
            if (out != nullptr) {
                // We got back a report of a message already sent.
                m_packetMutex.lock();
                ((PortCorePacket*)out)->dec(); // Message on one fewer connections.
                packets.checkPacket((PortCorePacket*)out);
                m_packetMutex.unlock();
            }
            if (waiter) {
                if (unit->isFinished()) {
                    all_ok = false;
                }
            }
            YMSG(("------- -- dec\n"));
        }
    }
    YMSG(("------- pack check\n"));
    m_packetMutex.lock();

    // We no longer concern ourselves with the message.
    // It may or may not be traveling on some connections.
    // But that is not our problem anymore.
    packet->dec();

    packets.checkPacket(packet);
    m_packetMutex.unlock();
    YMSG(("------- packed\n"));
    YMSG(("------- send out\n"));
    if (mode == PORTCORE_SEND_LOG) {
        if (logCount == 0) {
            logNeeded = false;
        }
    }
    m_stateSemaphore.post();
    YMSG(("------- send out real\n"));

    if (m_waitAfterSend && reader != nullptr) {
        all_ok = all_ok && gotReply;
    }

    return all_ok;
}


bool PortCore::isWriting()
{
    bool writing = false;

    m_stateSemaphore.wait();

    // Check if any port is currently writing.  TODO optimize
    // this query by counting down with notifyCompletion().
    if (!m_finished) {
        for (auto unit : m_units) {
            if ((unit != nullptr) && !unit->isFinished() && unit->isBusy()) {
                writing = true;
            }
        }
    }

    m_stateSemaphore.post();

    return writing;
}


int PortCore::getInputCount()
{
    cleanUnits(false);
    m_packetMutex.lock();
    int result = m_inputCount;
    m_packetMutex.unlock();
    return result;
}

int PortCore::getOutputCount()
{
    cleanUnits(false);
    m_packetMutex.lock();
    int result = m_outputCount;
    m_packetMutex.unlock();
    return result;
}


void PortCore::notifyCompletion(void* tracker)
{
    YMSG(("starting notifyCompletion\n"));
    m_packetMutex.lock();
    if (tracker != nullptr) {
        ((PortCorePacket*)tracker)->dec();
        packets.checkPacket((PortCorePacket*)tracker);
    }
    m_packetMutex.unlock();
    YMSG(("stopping notifyCompletion\n"));
}


bool PortCore::setEnvelope(PortWriter& envelope)
{
    envelopeWriter.restart();
    bool ok = envelope.write(envelopeWriter);
    if (ok) {
        setEnvelope(envelopeWriter.toString());
    }
    return ok;
}


void PortCore::setEnvelope(const std::string& envelope)
{
    this->envelope = envelope;
    for (unsigned int i = 0; i < this->envelope.length(); i++) {
        // It looks like envelopes are constrained to be printable ASCII?
        // I'm not sure why this would be.  TODO check.
        if (this->envelope[i] < 32) {
            this->envelope = this->envelope.substr(0, i);
            break;
        }
    }
    YARP_DEBUG(m_log, std::string("set envelope to ") + this->envelope);
}

std::string PortCore::getEnvelope()
{
    return envelope;
}

bool PortCore::getEnvelope(PortReader& envelope)
{
    StringInputStream sis;
    sis.add(this->envelope);
    sis.add("\r\n");
    StreamConnectionReader sbr;
    Route route;
    sbr.reset(sis, nullptr, route, 0, true);
    return envelope.read(sbr);
}

// Shorthand to create a nested (tag, val) pair to add to a message.
#define STANZA(name, tag, val) \
    Bottle name;               \
    (name).addString(tag);     \
    (name).addString(val);
#define STANZA_INT(name, tag, val) \
    Bottle name;                   \
    (name).addString(tag);         \
    (name).addInt32(val);

// Make an RPC connection to talk to a ROS API, send a message, get reply.
// NOTE: ROS support can now be moved out of here, once all documentation
// of older ways to interoperate with it are purged and people stop
// doing it.
static bool __pc_rpc(const Contact& c,
                     const char* carrier,
                     Bottle& writer,
                     Bottle& reader,
                     bool verbose)
{
    ContactStyle style;
    style.quiet = !verbose;
    style.timeout = 4;
    style.carrier = carrier;
    bool ok = Network::write(c, writer, reader, style);
    return ok;
}

// ACE is sometimes confused by localhost aliases, in a ROS-incompatible
// way.  This method does a quick sanity check if we are using ROS.
static bool __tcp_check(const Contact& c)
{
#ifdef YARP_HAS_ACE
    ACE_INET_Addr addr;
    int result = addr.set(c.getPort(), c.getHost().c_str());
    if (result != 0) {
        fprintf(stderr, "ACE choked on %s:%d\n", c.getHost().c_str(), c.getPort());
    }
    result = addr.set(c.getPort(), "127.0.0.1");
    if (result != 0) {
        fprintf(stderr, "ACE choked on 127.0.0.1:%d\n", c.getPort());
    }
    result = addr.set(c.getPort(), "127.0.1.1");
    if (result != 0) {
        fprintf(stderr, "ACE choked on 127.0.1.1:%d\n", c.getPort());
    }
#endif
    return true;
}

bool PortCore::adminBlock(ConnectionReader& reader,
                          void* id,
                          OutputStream* os)
{
    YARP_UNUSED(os);
    Bottle cmd;
    Bottle result;

    // We've received a message to the port that is marked as administrative.
    // That means that instead of passing it along as data to the user of the
    // port, the port itself is responsible for reading and responding to
    // it.  So let's read the message and see what we're supposed to do.
    cmd.read(reader);

    YARP_SPRINTF2(m_log, debug, "Port %s received command %s", getName().c_str(), cmd.toString().c_str());

    StringOutputStream cache;

    int vocab = cmd.get(0).asVocab();

    // We support ROS client API these days.  Here we recode some long ROS
    // command names, just for convenience.
    if (cmd.get(0).asString() == "publisherUpdate") {
        vocab = yarp::os::createVocab('r', 'p', 'u', 'p');
    }
    if (cmd.get(0).asString() == "requestTopic") {
        vocab = yarp::os::createVocab('r', 't', 'o', 'p');
    }
    if (cmd.get(0).asString() == "getPid") {
        vocab = yarp::os::createVocab('p', 'i', 'd');
    }
    if (cmd.get(0).asString() == "getBusInfo") {
        vocab = yarp::os::createVocab('b', 'u', 's');
    }

    std::string infoMsg;
    switch (vocab) {
    case yarp::os::createVocab('h', 'e', 'l', 'p'):
        // We give a list of the most useful administrative commands.
        result.addVocab(yarp::os::createVocab('m', 'a', 'n', 'y'));
        result.addString("[help]                  # give this help");
        result.addString("[ver]                   # report protocol version information");
        result.addString("[add] $portname         # add an output connection");
        result.addString("[add] $portname $car    # add an output with a given protocol");
        result.addString("[del] $portname         # remove an input or output connection");
        result.addString("[list] [in]             # list input connections");
        result.addString("[list] [out]            # list output connections");
        result.addString("[list] [in]  $portname  # give details for input");
        result.addString("[list] [out] $portname  # give details for output");
        result.addString("[prop] [get]            # get all user-defined port properties");
        result.addString("[prop] [get] $prop      # get a user-defined port property (prop, val)");
        result.addString("[prop] [set] $prop $val # set a user-defined port property (prop, val)");
        result.addString("[prop] [get] $portname  # get Qos properties of a connection to/from a port");
        result.addString("[prop] [set] $portname  # set Qos properties of a connection to/from a port");
        result.addString("[prop] [get] $cur_port  # get information about current process (e.g., scheduling priority, pid)");
        result.addString("[prop] [set] $cur_port  # set properties of the current process (e.g., scheduling priority, pid)");
        result.addString("[atch] [out] $prop      # attach a portmonitor plug-in to the port's output");
        result.addString("[atch] [in]  $prop      # attach a portmonitor plug-in to the port's input");
        result.addString("[dtch] [out]            # detach portmonitor plug-in from the port's output");
        result.addString("[dtch] [in]             # detach portmonitor plug-in from the port's input");
        //result.addString("[atch] $portname $prop  # attach a portmonitor plug-in to the connection to/from $portname");
        //result.addString("[dtch] $portname        # detach any portmonitor plug-in from the connection to/from $portname");
        break;
    case yarp::os::createVocab('v', 'e', 'r'):
        // Gives a version number for the administrative commands.
        // It is distinct from YARP library versioning.
        result.addVocab(Vocab::encode("ver"));
        result.addInt32(1);
        result.addInt32(2);
        result.addInt32(3);
        break;
    case yarp::os::createVocab('a', 'd', 'd'): {
        // Add an output to the port.
        std::string output = cmd.get(1).asString();
        std::string carrier = cmd.get(2).asString();
        if (!carrier.empty()) {
            output = carrier + ":/" + output;
        }
        addOutput(output, id, &cache, false);
        std::string r = cache.toString();
        int v = (r[0] == 'A') ? 0 : -1;
        result.addInt32(v);
        result.addString(r);
    } break;
    case yarp::os::createVocab('a', 't', 'c', 'h'): {
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('o', 'u', 't'): {
            std::string propString = cmd.get(2).asString();
            Property prop(propString.c_str());
            std::string errMsg;
            if (!attachPortMonitor(prop, true, errMsg)) {
                result.clear();
                result.addVocab(Vocab::encode("fail"));
                result.addString(errMsg);
            } else {
                result.clear();
                result.addVocab(Vocab::encode("ok"));
            }
        } break;
        case yarp::os::createVocab('i', 'n'): {
            std::string propString = cmd.get(2).asString();
            Property prop(propString.c_str());
            std::string errMsg;
            if (!attachPortMonitor(prop, false, errMsg)) {
                result.clear();
                result.addVocab(Vocab::encode("fail"));
                result.addString(errMsg);
            } else {
                result.clear();
                result.addVocab(Vocab::encode("ok"));
            }
        } break;
        default:
            result.clear();
            result.addVocab(Vocab::encode("fail"));
            result.addString("attach command must be followd by [out] or [in]");
        };
    } break;
    case yarp::os::createVocab('d', 't', 'c', 'h'): {
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('o', 'u', 't'): {
            if (dettachPortMonitor(true)) {
                result.addVocab(Vocab::encode("ok"));
            } else {
                result.addVocab(Vocab::encode("fail"));
            }
        } break;
        case yarp::os::createVocab('i', 'n'): {
            if (dettachPortMonitor(false)) {
                result.addVocab(Vocab::encode("ok"));
            } else {
                result.addVocab(Vocab::encode("fail"));
            }
        } break;
        default:
            result.clear();
            result.addVocab(Vocab::encode("fail"));
            result.addString("detach command must be followd by [out] or [in]");
        };
    } break;
    case yarp::os::createVocab('d', 'e', 'l'): {
        // Delete any inputs or outputs involving the named port.
        removeOutput(cmd.get(1).asString(), id, &cache);
        std::string r1 = cache.toString();
        cache.reset();
        removeInput(cmd.get(1).asString(), id, &cache);
        std::string r2 = cache.toString();
        int v = (r1[0] == 'R' || r2[0] == 'R') ? 0 : -1;
        result.addInt32(v);
        if (r1[0] == 'R' && r2[0] != 'R') {
            result.addString(r1);
        } else if (r1[0] != 'R' && r2[0] == 'R') {
            result.addString(r2);
        } else {
            result.addString(r1 + r2);
        }
    } break;
    case yarp::os::createVocab('l', 'i', 's', 't'):
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('i', 'n'): {
            // Return a list of all input connections.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            for (auto unit : m_units) {
                if ((unit != nullptr) && unit->isInput() && !unit->isFinished()) {
                    Route route = unit->getRoute();
                    if (target.empty()) {
                        const std::string& name = route.getFromName();
                        if (!name.empty()) {
                            result.addString(name);
                        }
                    } else if (route.getFromName() == target) {
                        STANZA(bfrom, "from", route.getFromName());
                        STANZA(bto, "to", route.getToName());
                        STANZA(bcarrier, "carrier", route.getCarrierName());
                        result.addList() = bfrom;
                        result.addList() = bto;
                        result.addList() = bcarrier;
                        Carrier* carrier = Carriers::chooseCarrier(route.getCarrierName());
                        if (carrier->isConnectionless()) {
                            STANZA_INT(bconnectionless, "connectionless", 1);
                            result.addList() = bconnectionless;
                        }
                        if (!carrier->isPush()) {
                            STANZA_INT(breverse, "push", 0);
                            result.addList() = breverse;
                        }
                        delete carrier;
                    }
                }
            }
            m_stateSemaphore.post();
        } break;
        case yarp::os::createVocab('o', 'u', 't'):
        default:
        {
            // Return a list of all output connections.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            for (auto unit : m_units) {
                if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
                    Route route = unit->getRoute();
                    if (target.empty()) {
                        result.addString(route.getToName());
                    } else if (route.getToName() == target) {
                        STANZA(bfrom, "from", route.getFromName());
                        STANZA(bto, "to", route.getToName());
                        STANZA(bcarrier, "carrier", route.getCarrierName());
                        result.addList() = bfrom;
                        result.addList() = bto;
                        result.addList() = bcarrier;
                        Carrier* carrier = Carriers::chooseCarrier(route.getCarrierName());
                        if (carrier->isConnectionless()) {
                            STANZA_INT(bconnectionless, "connectionless", 1);
                            result.addList() = bconnectionless;
                        }
                        if (!carrier->isPush()) {
                            STANZA_INT(breverse, "push", 0);
                            result.addList() = breverse;
                        }
                        delete carrier;
                    }
                }
            }
            m_stateSemaphore.post();
        }
        }
        break;

    case yarp::os::createVocab('s', 'e', 't'):
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('i', 'n'): {
            // Set carrier parameters on a given input connection.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            if (target.empty()) {
                result.addInt32(-1);
                result.addString("target port is not specified.\r\n");
            } else {
                if (target == getName()) {
                    yarp::os::Property property;
                    property.fromString(cmd.toString());
                    std::string errMsg;
                    if (!setParamPortMonitor(property, false, errMsg)) {
                        result.clear();
                        result.addVocab(Vocab::encode("fail"));
                        result.addString(errMsg);
                    } else {
                        result.clear();
                        result.addVocab(Vocab::encode("ok"));
                    }
                } else {
                    for (auto unit : m_units) {
                        if ((unit != nullptr) && unit->isInput() && !unit->isFinished()) {
                            Route route = unit->getRoute();
                            if (route.getFromName() == target) {
                                yarp::os::Property property;
                                property.fromString(cmd.toString());
                                unit->setCarrierParams(property);
                                result.addInt32(0);
                                std::string msg = "Configured connection from ";
                                msg += route.getFromName();
                                msg += "\r\n";
                                result.addString(msg);
                                break;
                            }
                        }
                    }
                }
                if (result.size() == 0) {
                    result.addInt32(-1);
                    std::string msg = "Could not find an incoming connection from ";
                    msg += target;
                    msg += "\r\n";
                    result.addString(msg);
                }
            }
            m_stateSemaphore.post();
        } break;
        case yarp::os::createVocab('o', 'u', 't'):
        default:
        {
            // Set carrier parameters on a given output connection.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            if (target.empty()) {
                result.addInt32(-1);
                result.addString("target port is not specified.\r\n");
            } else {
                if (target == getName()) {
                    yarp::os::Property property;
                    property.fromString(cmd.toString());
                    std::string errMsg;
                    if (!setParamPortMonitor(property, true, errMsg)) {
                        result.clear();
                        result.addVocab(Vocab::encode("fail"));
                        result.addString(errMsg);
                    } else {
                        result.clear();
                        result.addVocab(Vocab::encode("ok"));
                    }
                } else {
                    for (auto unit : m_units) {
                        if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
                            Route route = unit->getRoute();
                            if (route.getToName() == target) {
                                yarp::os::Property property;
                                property.fromString(cmd.toString());
                                unit->setCarrierParams(property);
                                result.addInt32(0);
                                std::string msg = "Configured connection to ";
                                msg += route.getToName();
                                msg += "\r\n";
                                result.addString(msg);
                                break;
                            }
                        }
                    }
                }
                if (result.size() == 0) {
                    result.addInt32(-1);
                    std::string msg = "Could not find an incoming connection to ";
                    msg += target;
                    msg += "\r\n";
                    result.addString(msg);
                }
            }
            m_stateSemaphore.post();
        }
        }
        break;

    case yarp::os::createVocab('g', 'e', 't'):
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('i', 'n'): {
            // Get carrier parameters for a given input connection.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            if (target.empty()) {
                result.addInt32(-1);
                result.addString("target port is not specified.\r\n");
            } else if (target == getName()) {
                yarp::os::Property property;
                std::string errMsg;
                if (!getParamPortMonitor(property, false, errMsg)) {
                    result.clear();
                    result.addVocab(Vocab::encode("fail"));
                    result.addString(errMsg);
                } else {
                    result.clear();
                    result.addDict() = property;
                }
            } else {
                for (auto unit : m_units) {
                    if ((unit != nullptr) && unit->isInput() && !unit->isFinished()) {
                        Route route = unit->getRoute();
                        if (route.getFromName() == target) {
                            yarp::os::Property property;
                            unit->getCarrierParams(property);
                            result.addDict() = property;
                            break;
                        }
                    }
                }
                if (result.size() == 0) {
                    result.addInt32(-1);
                    std::string msg = "Could not find an incoming connection from ";
                    msg += target;
                    msg += "\r\n";
                    result.addString(msg);
                }
            }
            m_stateSemaphore.post();
        } break;
        case yarp::os::createVocab('o', 'u', 't'):
        default:
        {
            // Get carrier parameters for a given output connection.
            std::string target = cmd.get(2).asString();
            m_stateSemaphore.wait();
            if (target.empty()) {
                result.addInt32(-1);
                result.addString("target port is not specified.\r\n");
            } else if (target == getName()) {
                yarp::os::Property property;
                std::string errMsg;
                if (!getParamPortMonitor(property, true, errMsg)) {
                    result.clear();
                    result.addVocab(Vocab::encode("fail"));
                    result.addString(errMsg);
                } else {
                    result.clear();
                    result.addDict() = property;
                }
            } else {
                for (auto unit : m_units) {
                    if ((unit != nullptr) && unit->isOutput() && !unit->isFinished()) {
                        Route route = unit->getRoute();
                        if (route.getToName() == target) {
                            yarp::os::Property property;
                            property.fromString(cmd.toString());
                            unit->getCarrierParams(property);
                            result.addDict() = property;
                            break;
                        }
                    }
                }
                if (result.size() == 0) {
                    result.addInt32(-1);
                    std::string msg = "Could not find an incoming connection to ";
                    msg += target;
                    msg += "\r\n";
                    result.addString(msg);
                }
            }
            m_stateSemaphore.post();
        }
        }
        break;

    case yarp::os::createVocab('r', 'p', 'u', 'p'): {
        // When running against a ROS name server, we need to
        // support ROS-style callbacks for connecting publishers
        // with subscribers.  Note: this should not be necessary
        // anymore, now that a dedicated yarp::os::Node class
        // has been implemented, but is still needed for older
        // ways of interfacing with ROS without using dedicated
        // node ports.
        YARP_SPRINTF1(m_log, debug, "publisherUpdate! --> %s", cmd.toString().c_str());
        std::string topic = RosNameSpace::fromRosName(cmd.get(2).asString());
        Bottle* pubs = cmd.get(3).asList();
        if (pubs != nullptr) {
            Property listed;
            for (size_t i = 0; i < pubs->size(); i++) {
                std::string pub = pubs->get(i).asString();
                listed.put(pub, 1);
            }
            Property present;
            m_stateSemaphore.wait();
            for (auto unit : m_units) {
                if ((unit != nullptr) && unit->isPupped()) {
                    std::string me = unit->getPupString();
                    present.put(me, 1);
                    if (!listed.check(me)) {
                        unit->setDoomed();
                    }
                }
            }
            m_stateSemaphore.post();
            for (size_t i = 0; i < pubs->size(); i++) {
                std::string pub = pubs->get(i).asString();
                if (!present.check(pub)) {
                    YARP_SPRINTF1(m_log, debug, "ROS ADD %s", pub.c_str());
                    Bottle req;
                    Bottle reply;
                    req.addString("requestTopic");
                    NestedContact nc(getName());
                    req.addString(nc.getNodeName());
                    req.addString(topic);
                    Bottle& lst = req.addList();
                    Bottle& sublst = lst.addList();
                    sublst.addString("TCPROS");
                    YARP_SPRINTF2(m_log, debug, "Sending [%s] to %s", req.toString().c_str(), pub.c_str());
                    Contact c = Contact::fromString(pub);
                    if (!__pc_rpc(c, "xmlrpc", req, reply, false)) {
                        fprintf(stderr, "Cannot connect to ROS subscriber %s\n", pub.c_str());
                        // show diagnosics
                        __pc_rpc(c, "xmlrpc", req, reply, true);
                        __tcp_check(c);
                    } else {
                        Bottle* pref = reply.get(2).asList();
                        std::string hostname;
                        std::string carrier;
                        int portnum = 0;
                        if (reply.get(0).asInt32() != 1) {
                            fprintf(stderr, "Failure looking up topic %s: %s\n", topic.c_str(), reply.toString().c_str());
                        } else if (pref == nullptr) {
                            fprintf(stderr, "Failure looking up topic %s: expected list of protocols\n", topic.c_str());
                        } else if (pref->get(0).asString() != "TCPROS") {
                            fprintf(stderr, "Failure looking up topic %s: unsupported protocol %s\n", topic.c_str(), pref->get(0).asString().c_str());
                        } else {
                            Value hostname2 = pref->get(1);
                            Value portnum2 = pref->get(2);
                            hostname = hostname2.asString();
                            portnum = portnum2.asInt32();
                            carrier = "tcpros+role.pub+topic.";
                            carrier += topic;
                            YARP_SPRINTF3(m_log, debug, "topic %s available at %s:%d", topic.c_str(), hostname.c_str(), portnum);
                        }
                        if (portnum != 0) {
                            Contact addr(hostname, portnum);
                            OutputProtocol* op = nullptr;
                            Route r = Route(getName(), pub, carrier);
                            op = Carriers::connect(addr);
                            if (op == nullptr) {
                                fprintf(stderr, "NO CONNECTION\n");
                                std::exit(1);
                            } else {
                                op->attachPort(contactable);
                                op->open(r);
                            }
                            Route route = op->getRoute();
                            route.swapNames();
                            op->rename(route);
                            InputProtocol* ip = &(op->getInput());
                            m_stateSemaphore.wait();
                            PortCoreUnit* unit = new PortCoreInputUnit(*this,
                                                                       getNextIndex(),
                                                                       ip,
                                                                       true);
                            yAssert(unit != nullptr);
                            unit->setPupped(pub);
                            unit->start();
                            m_units.push_back(unit);
                            m_stateSemaphore.post();
                        }
                    }
                }
            }
        }
        result.addInt32(1);
        result.addString("ok");
        reader.requestDrop(); // ROS needs us to close down.
    } break;
    case yarp::os::createVocab('r', 't', 'o', 'p'): {
        // ROS-style query for topics.
        YARP_SPRINTF1(m_log, debug, "requestTopic! --> %s", cmd.toString().c_str());
        result.addInt32(1);
        NestedContact nc(getName());
        result.addString(nc.getNodeName());
        Bottle& lst = result.addList();
        Contact addr = getAddress();
        lst.addString("TCPROS");
        lst.addString(addr.getHost());
        lst.addInt32(addr.getPort());
        reader.requestDrop(); // ROS likes to close down.
    } break;
    case yarp::os::createVocab('p', 'i', 'd'): {
        // ROS-style query for PID.
        result.addInt32(1);
        result.addString("");
        result.addInt32(yarp::os::impl::getpid());
        reader.requestDrop(); // ROS likes to close down.
    } break;
    case yarp::os::createVocab('b', 'u', 's'): {
        // ROS-style query for bus information - we support this
        // in yarp::os::Node but not otherwise.
        result.addInt32(1);
        result.addString("");
        result.addList().addList();
        reader.requestDrop(); // ROS likes to close down.
    } break;
    case yarp::os::createVocab('p', 'r', 'o', 'p'): {
        // Set/get arbitrary properties on a port.
        switch (cmd.get(1).asVocab()) {
        case yarp::os::createVocab('g', 'e', 't'): {
            Property* p = acquireProperties(false);
            if (p != nullptr) {
                if (!cmd.get(2).isNull()) {
                    // request: "prop get /portname"
                    std::string portName = cmd.get(2).asString();
                    bool bFound = false;
                    if ((!portName.empty()) && (portName[0] == '/')) {
                        // check for their own name
                        if (portName == getName()) {
                            bFound = true;
                            result.clear();
                            Bottle& sched = result.addList();
                            sched.addString("sched");
                            Property& sched_prop = sched.addDict();
                            sched_prop.put("tid", (int)this->getTid());
                            sched_prop.put("priority", this->getPriority());
                            sched_prop.put("policy", this->getPolicy());

                            SystemInfo::ProcessInfo info = SystemInfo::getProcessInfo();
                            Bottle& proc = result.addList();
                            proc.addString("process");
                            Property& proc_prop = proc.addDict();
                            proc_prop.put("pid", info.pid);
                            proc_prop.put("name", (info.pid != -1) ? info.name : "unknown");
                            proc_prop.put("arguments", (info.pid != -1) ? info.arguments : "unknown");
                            proc_prop.put("priority", info.schedPriority);
                            proc_prop.put("policy", info.schedPolicy);

                            SystemInfo::PlatformInfo pinfo = SystemInfo::getPlatformInfo();
                            Bottle& platform = result.addList();
                            platform.addString("platform");
                            Property& platform_prop = platform.addDict();
                            platform_prop.put("os", pinfo.name);
                            platform_prop.put("hostname", m_address.getHost());

                            int f = getFlags();
                            bool is_input = (f & PORTCORE_IS_INPUT) != 0;
                            bool is_output = (f & PORTCORE_IS_OUTPUT) != 0;
                            bool is_rpc = (f & PORTCORE_IS_RPC) != 0;
                            Bottle& port = result.addList();
                            port.addString("port");
                            Property& port_prop = port.addDict();
                            port_prop.put("is_input", is_input);
                            port_prop.put("is_output", is_output);
                            port_prop.put("is_rpc", is_rpc);
                            port_prop.put("type", getType().getName());
                        } else {
                            for (auto unit : m_units) {
                                if ((unit != nullptr) && !unit->isFinished()) {
                                    Route route = unit->getRoute();
                                    std::string coreName = (unit->isOutput()) ? route.getToName() : route.getFromName();
                                    if (portName == coreName) {
                                        bFound = true;
                                        int priority = unit->getPriority();
                                        int policy = unit->getPolicy();
                                        int tos = getTypeOfService(unit);
                                        int tid = (int)unit->getTid();
                                        result.clear();
                                        Bottle& sched = result.addList();
                                        sched.addString("sched");
                                        Property& sched_prop = sched.addDict();
                                        sched_prop.put("tid", tid);
                                        sched_prop.put("priority", priority);
                                        sched_prop.put("policy", policy);
                                        Bottle& qos = result.addList();
                                        qos.addString("qos");
                                        Property& qos_prop = qos.addDict();
                                        qos_prop.put("tos", tos);
                                    }
                                } // end isFinished()
                            }     // end for loop
                        }         // end portName == getname()

                        if (!bFound) { // cannot find any port matches the requested one
                            result.clear();
                            result.addVocab(Vocab::encode("fail"));
                            std::string msg = "cannot find any connection to/from ";
                            msg = msg + portName;
                            result.addString(msg);
                        }
                    } // end of (portName[0] == '/')
                    else {
                        result.add(p->find(cmd.get(2).asString()));
                    }
                } else {
                    result.fromString(p->toString());
                }
            }
            releaseProperties(p);
        } break;
        case yarp::os::createVocab('s', 'e', 't'): {
            Property* p = acquireProperties(false);
            bool bOk = true;
            if (p != nullptr) {
                p->put(cmd.get(2).asString(), cmd.get(3));
                // setting scheduling properties of all threads within the process
                // scope through the admin port
                // e.g. prop set /current_port (process ((priority 30) (policy 1)))
                Bottle& process = cmd.findGroup("process");
                if (!process.isNull()) {
                    std::string portName = cmd.get(2).asString();
                    if ((!portName.empty()) && (portName[0] == '/')) {
                        // check for their own name
                        if (portName == getName()) {
                            bOk = false;
                            Bottle* process_prop = process.find("process").asList();
                            if (process_prop != nullptr) {
                                int prio = -1;
                                int policy = -1;
                                if (process_prop->check("priority")) {
                                    prio = process_prop->find("priority").asInt32();
                                }
                                if (process_prop->check("policy")) {
                                    policy = process_prop->find("policy").asInt32();
                                }
                                bOk = setProcessSchedulingParam(prio, policy);
                            }
                        }
                    }
                }
                // check if we need to set the PortCoreUnit scheduling policy
                // e.g., "prop set /portname (sched ((priority 30) (policy 1)))"
                // The priority and policy values on Linux are:
                // SCHED_OTHER : policy=0, priority=[0 ..  0]
                // SCHED_FIFO  : policy=1, priority=[1 .. 99]
                // SCHED_RR    : policy=2, priority=[1 .. 99]
                Bottle& sched = cmd.findGroup("sched");
                if (!sched.isNull()) {
                    if ((!cmd.get(2).asString().empty()) && (cmd.get(2).asString()[0] == '/')) {
                        bOk = false;
                        for (auto unit : m_units) {
                            if ((unit != nullptr) && !unit->isFinished()) {
                                Route route = unit->getRoute();
                                std::string portName = (unit->isOutput()) ? route.getToName() : route.getFromName();

                                if (portName == cmd.get(2).asString()) {
                                    Bottle* sched_prop = sched.find("sched").asList();
                                    if (sched_prop != nullptr) {
                                        int prio = -1;
                                        int policy = -1;
                                        if (sched_prop->check("priority")) {
                                            prio = sched_prop->find("priority").asInt32();
                                        }
                                        if (sched_prop->check("policy")) {
                                            policy = sched_prop->find("policy").asInt32();
                                        }
                                        bOk = (unit->setPriority(prio, policy) != -1);
                                    } else {
                                        bOk = false;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }

                // check if we need to set the packet QOS policy
                // e.g., "prop set /portname (qos ((priority HIGH)))"
                // e.g., "prop set /portname (qos ((dscp AF12)))"
                // e.g., "prop set /portname (qos ((tos 12)))"
                Bottle& qos = cmd.findGroup("qos");
                if (!qos.isNull()) {
                    if ((!cmd.get(2).asString().empty()) && (cmd.get(2).asString()[0] == '/')) {
                        bOk = false;
                        for (auto unit : m_units) {
                            if ((unit != nullptr) && !unit->isFinished()) {
                                Route route = unit->getRoute();
                                std::string portName = (unit->isOutput()) ? route.getToName() : route.getFromName();
                                if (portName == cmd.get(2).asString()) {
                                    Bottle* qos_prop = qos.find("qos").asList();
                                    if (qos_prop != nullptr) {
                                        if (qos_prop->check("priority")) {
                                            NetInt32 priority = qos_prop->find("priority").asVocab();
                                            // set the packet DSCP value based on some predefined priority levels
                                            // the expected levels are: LOW, NORM, HIGH, CRIT
                                            int dscp;
                                            switch (priority) {
                                            case yarp::os::createVocab('L', 'O', 'W'):
                                                dscp = 10;
                                                break;
                                            case yarp::os::createVocab('N', 'O', 'R', 'M'):
                                                dscp = 0;
                                                break;
                                            case yarp::os::createVocab('H', 'I', 'G', 'H'):
                                                dscp = 36;
                                                break;
                                            case yarp::os::createVocab('C', 'R', 'I', 'T'):
                                                dscp = 44;
                                                break;
                                            default:
                                                dscp = -1;
                                            };
                                            if (dscp >= 0) {
                                                bOk = setTypeOfService(unit, dscp << 2);
                                            }
                                        } else if (qos_prop->check("dscp")) {
                                            QosStyle::PacketPriorityDSCP dscp_class = QosStyle::getDSCPByVocab(qos_prop->find("dscp").asVocab());
                                            int dscp = -1;
                                            if (dscp_class == QosStyle::DSCP_Invalid) {
                                                dscp = qos_prop->find("dscp").asInt32();
                                            } else {
                                                dscp = (int)dscp_class;
                                            }
                                            if ((dscp >= 0) && (dscp < 64)) {
                                                bOk = setTypeOfService(unit, dscp << 2);
                                            }
                                        } else if (qos_prop->check("tos")) {
                                            int tos = qos_prop->find("tos").asInt32();
                                            // set the TOS value (backward compatibility)
                                            bOk = setTypeOfService(unit, tos);
                                        }
                                    } else {
                                        bOk = false;
                                    }
                                    break;
                                }
                            }
                        }
                    }
                }
            }
            releaseProperties(p);
            result.addVocab((bOk) ? Vocab::encode("ok") : Vocab::encode("fail"));
        } break;
        default:
            result.addVocab(Vocab::encode("fail"));
            result.addString("property action not known");
            break;
        }
    } break;
    default:
    {
        bool ok = false;
        if (m_adminReader != nullptr) {
            DummyConnector con;
            cmd.write(con.getWriter());
            lockCallback();
            ok = m_adminReader->read(con.getReader());
            unlockCallback();
            if (ok) {
                result.read(con.getReader());
            }
        }
        if (!ok) {
            result.addVocab(Vocab::encode("fail"));
            result.addString("send [help] for list of valid commands");
        }
    } break;
    }

    ConnectionWriter* writer = reader.getWriter();
    if (writer != nullptr) {
        result.write(*writer);
    }

    /**
     * @brief We introduce a nonsense arbitrary delay in the calls to the port administrator
     * for debugging purpose. This is indeed a temporary feature and will be removed soon.
     * The delay is applied if the "NONSENSE_ADMIN_DELAY" environment variable is set.
     */
    std::string nonsense_delay = NetworkBase::getEnvironment("NONSENSE_ADMIN_DELAY");
    if (!nonsense_delay.empty()) {
        yarp::os::SystemClock::delaySystem(atof(nonsense_delay.c_str()));
    }

    return true;
}


bool PortCore::setTypeOfService(PortCoreUnit* unit, int tos)
{
    if (unit == nullptr) {
        return false;
    }

    if (unit->isOutput()) {
        auto* outUnit = dynamic_cast<PortCoreOutputUnit*>(unit);
        if (outUnit != nullptr) {
            OutputProtocol* op = outUnit->getOutPutProtocol();
            if (op != nullptr) {
                return op->getOutputStream().setTypeOfService(tos);
            }
        }
    }

    // Some of the input units may have output stream object to write back to
    // the connection (e.g., tcp ack and reply). Thus the QoS preferences should be
    // also configured for them.


    if (unit->isInput()) {
        auto* inUnit = dynamic_cast<PortCoreInputUnit*>(unit);
        if (inUnit != nullptr) {
            InputProtocol* ip = inUnit->getInPutProtocol();
            if ((ip != nullptr) && ip->getOutput().isOk()) {
                return ip->getOutput().getOutputStream().setTypeOfService(tos);
            }
        }
    }
    // if there is nothing to be set, returns true
    return true;
}

int PortCore::getTypeOfService(PortCoreUnit* unit)
{
    if (unit == nullptr) {
        return -1;
    }

    if (unit->isOutput()) {
        auto* outUnit = dynamic_cast<PortCoreOutputUnit*>(unit);
        if (outUnit != nullptr) {
            OutputProtocol* op = outUnit->getOutPutProtocol();
            if (op != nullptr) {
                return op->getOutputStream().getTypeOfService();
            }
        }
    }

    // Some of the input units may have output stream object to write back to
    // the connection (e.g., tcp ack and reply). Thus the QoS preferences should be
    // also configured for them.


    if (unit->isInput()) {
        auto* inUnit = dynamic_cast<PortCoreInputUnit*>(unit);
        if (inUnit != nullptr) {
            InputProtocol* ip = inUnit->getInPutProtocol();
            if ((ip != nullptr) && ip->getOutput().isOk()) {
                return ip->getOutput().getOutputStream().getTypeOfService();
            }
        }
    }
    return -1;
}

// attach a portmonitor plugin to the port or to a specific connection
bool PortCore::attachPortMonitor(yarp::os::Property& prop, bool isOutput, std::string& errMsg)
{
    // attach to the current port
    Carrier* portmonitor = Carriers::chooseCarrier("portmonitor");
    if (portmonitor == nullptr) {
        errMsg = "Portmonitor carrier modifier cannot be find or it is not enabled in Yarp!";
        return false;
    }

    if (isOutput) {
        dettachPortMonitor(true);
        prop.put("source", getName());
        prop.put("destination", "");
        prop.put("sender_side", 1);
        prop.put("receiver_side", 0);
        prop.put("carrier", "");
        modifier.outputMutex.lock();
        modifier.outputModifier = portmonitor;
        if (!modifier.outputModifier->configureFromProperty(prop)) {
            modifier.releaseOutModifier();
            errMsg = "Failed to configure the portmonitor plug-in";
            modifier.outputMutex.unlock();
            return false;
        }
        modifier.outputMutex.unlock();
    } else {
        dettachPortMonitor(false);
        prop.put("source", "");
        prop.put("destination", getName());
        prop.put("sender_side", 0);
        prop.put("receiver_side", 1);
        prop.put("carrier", "");
        modifier.inputMutex.lock();
        modifier.inputModifier = portmonitor;
        if (!modifier.inputModifier->configureFromProperty(prop)) {
            modifier.releaseInModifier();
            errMsg = "Failed to configure the portmonitor plug-in";
            modifier.inputMutex.unlock();
            return false;
        }
        modifier.inputMutex.unlock();
    }
    return true;
}

// detach the portmonitor from the port or specific connection
bool PortCore::dettachPortMonitor(bool isOutput)
{
    if (isOutput) {
        modifier.outputMutex.lock();
        modifier.releaseOutModifier();
        modifier.outputMutex.unlock();
    } else {
        modifier.inputMutex.lock();
        modifier.releaseInModifier();
        modifier.inputMutex.unlock();
    }
    return true;
}

bool PortCore::setParamPortMonitor(yarp::os::Property& param,
                                   bool isOutput,
                                   std::string& errMsg)
{
    if (isOutput) {
        modifier.outputMutex.lock();
        if (modifier.outputModifier == nullptr) {
            errMsg = "No port modifer is attached to the output";
            modifier.outputMutex.unlock();
            return false;
        }
        modifier.outputModifier->setCarrierParams(param);
        modifier.outputMutex.unlock();
    } else {
        modifier.inputMutex.lock();
        if (modifier.inputModifier == nullptr) {
            errMsg = "No port modifer is attached to the input";
            modifier.inputMutex.unlock();
            return false;
        }
        modifier.inputModifier->setCarrierParams(param);
        modifier.inputMutex.unlock();
    }
    return true;
}

bool PortCore::getParamPortMonitor(yarp::os::Property& param,
                                   bool isOutput,
                                   std::string& errMsg)
{
    if (isOutput) {
        modifier.outputMutex.lock();
        if (modifier.outputModifier == nullptr) {
            errMsg = "No port modifer is attached to the output";
            modifier.outputMutex.unlock();
            return false;
        }
        modifier.outputModifier->getCarrierParams(param);
        modifier.outputMutex.unlock();
    } else {
        modifier.inputMutex.lock();
        if (modifier.inputModifier == nullptr) {
            errMsg = "No port modifer is attached to the input";
            modifier.inputMutex.unlock();
            return false;
        }
        modifier.inputModifier->getCarrierParams(param);
        modifier.inputMutex.unlock();
    }
    return true;
}

void PortCore::reportUnit(PortCoreUnit* unit, bool active)
{
    YARP_UNUSED(active);
    if (unit != nullptr) {
        bool isLog = (!unit->getMode().empty());
        if (isLog) {
            logNeeded = true;
        }
    }
}

bool PortCore::setProcessSchedulingParam(int priority, int policy)
{
#if defined(__linux__)
    // set the sched properties of all threads within the process
    struct sched_param sch_param;
    sch_param.__sched_priority = priority;

    DIR* dir;
    char path[PATH_MAX];
    sprintf(path, "/proc/%d/task/", yarp::os::impl::getpid());

    dir = opendir(path);
    if (dir == nullptr) {
        return false;
    }

    struct dirent* d;
    char* end;
    int tid = 0;
    bool ret = true;
    while ((d = readdir(dir)) != nullptr) {
        if (isdigit((unsigned char)*d->d_name) == 0) {
            continue;
        }

        tid = (pid_t)strtol(d->d_name, &end, 10);
        if (d->d_name == end || ((end != nullptr) && (*end != 0))) {
            closedir(dir);
            return false;
        }
        ret &= (sched_setscheduler(tid, policy, &sch_param) == 0);
    }
    closedir(dir);
    return ret;
#elif defined(YARP_HAS_ACE) // for other platforms
    // TODO: Check how to set the scheduling properties of all process's threads in Windows
    ACE_Sched_Params param(policy, (ACE_Sched_Priority)priority, ACE_SCOPE_PROCESS);
    int ret = ACE_OS::set_scheduling_params(param, yarp::os::impl::getpid());
    return (ret != -1);
#else
    return false;
#endif
}

Property* PortCore::acquireProperties(bool readOnly)
{
    m_stateSemaphore.wait();
    if (!readOnly) {
        if (prop == nullptr) {
            prop = new Property();
        }
    }
    return prop;
}

void PortCore::releaseProperties(Property* prop)
{
    YARP_UNUSED(prop);
    m_stateSemaphore.post();
}

bool PortCore::removeIO(const Route& route, bool synch)
{
    return removeUnit(route, synch);
}

void PortCore::setName(const std::string& name)
{
    this->m_name = name;
}

std::string PortCore::getName()
{
    return m_name;
}

int PortCore::getNextIndex()
{
    int result = counter;
    counter++;
    if (counter < 0) {
        counter = 1;
    }
    return result;
}

const Contact& PortCore::getAddress() const
{
    return m_address;
}

void PortCore::resetPortName(const std::string& str)
{
    m_address.setName(str);
}

yarp::os::PortReaderCreator* PortCore::getReadCreator()
{
    return m_readableCreator;
}

void PortCore::setControlRegistration(bool flag)
{
    m_controlRegistration = flag;
}

bool PortCore::isListening() const
{
    return m_listening;
}

bool PortCore::isManual() const
{
    return m_manual;
}

bool PortCore::isInterrupted() const
{
    return m_interrupted;
}

void PortCore::setTimeout(float timeout)
{
    this->timeout = timeout;
}

void PortCore::setVerbosity(int level)
{
    verbosity = level;
}

int PortCore::getVerbosity()
{
    return verbosity;
}

bool PortCore::setCallbackLock(yarp::os::Mutex* mutex)
{
    removeCallbackLock();
    if (mutex != nullptr) {
        this->mutex = mutex;
        mutexOwned = false;
    } else {
        this->mutex = new yarp::os::Mutex();
        mutexOwned = true;
    }
    return true;
}

bool PortCore::removeCallbackLock()
{
    if (mutexOwned && (mutex != nullptr)) {
        delete mutex;
    }
    mutex = nullptr;
    mutexOwned = false;
    return true;
}

bool PortCore::lockCallback()
{
    if (mutex == nullptr) {
        return false;
    }
    mutex->lock();
    return true;
}

bool PortCore::tryLockCallback()
{
    if (mutex == nullptr) {
        return true;
    }
    return mutex->try_lock();
}

void PortCore::unlockCallback()
{
    if (mutex == nullptr) {
        return;
    }
    mutex->unlock();
}


yarp::os::impl::PortDataModifier& PortCore::getPortModifier()
{
    return modifier;
}

void PortCore::checkType(PortReader& reader)
{
    typeMutex.lock();
    if (!checkedType) {
        if (!typ.isValid()) {
            typ = reader.getReadType();
        }
        checkedType = true;
    }
    typeMutex.unlock();
}

yarp::os::Type PortCore::getType()
{
    typeMutex.lock();
    Type t = typ;
    typeMutex.unlock();
    return t;
}

void PortCore::promiseType(const Type& typ)
{
    typeMutex.lock();
    this->typ = typ;
    typeMutex.unlock();
}
