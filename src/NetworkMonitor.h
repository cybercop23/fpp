#pragma once
/*
 * This file is part of the Falcon Player (FPP) and is Copyright (C)
 * 2013-2022 by the Falcon Player Developers.
 *
 * The Falcon Player (FPP) is free software, and is covered under
 * multiple Open Source licenses.  Please see the included 'LICENSES'
 * file for descriptions of what files are covered by each license.
 *
 * This source file is covered under the LGPL v2.1 as described in the
 * included LICENSE.LGPL file.
 */

#include <functional>
#include <list>
#include <map>
#include <mutex>
#include <string>

class NetworkMonitor {
public:
    enum class NetEventType {
        NEW_LINK,
        DEL_LINK,
        NEW_ADDR,
        DEL_ADDR
    };

    NetworkMonitor() :
        curId(0) {}
    ~NetworkMonitor() {}

    void Init(std::map<int, std::function<bool(int)>>& callbacks);

    int registerCallback(std::function<void(NetEventType, int, const std::string&)>& callback);
    int addCallback(std::function<void(NetEventType, int, const std::string&)>& callback) { return registerCallback(callback); }
    void removeCallback(int id);

    static NetworkMonitor INSTANCE;

private:
    void callCallbacks(NetEventType, int, const std::string& n);
    // Guards `callbacks` against concurrent access: register/remove run on the
    // main thread (channel-output Init/Close, MultiSync setup) while dispatch
    // runs on the netlink/epoll thread.  Held across dispatch so removeCallback
    // cannot return (letting the caller destroy the captured object) while a
    // callback is still executing - an unsynchronized std::map here let a
    // netlink event during teardown corrupt the tree and crash at a later free.
    std::mutex callbackLock;
    std::map<int, std::function<void(NetEventType, int, const std::string&)>> callbacks;
    int curId;
};
