/**
 *    SPDX-License-Identifier: AGPL-3.0-only
 *
 *    Copyright (C) 2017-2020 Micron Technology, Inc.
 *
 *    This code is derived from and modifies the mongo-rocks project.
 *
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU Affero General Public License for more details.
 *
 *    You should have received a copy of the GNU Affero General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *    As a special exception, the copyright holders give permission to link the
 *    code of portions of this program with the OpenSSL library under certain
 *    conditions as described in each individual source file and distribute
 *    linked combinations including the program with the OpenSSL library. You
 *    must comply with the GNU Affero General Public License in all respects for
 *    all of the code used other than as permitted herein. If you modify file(s)
 *    with this exception, you may extend this exception to your version of the
 *    file(s), but you are not obligated to do so. If you do not wish to do so,
 *    delete this exception statement from your version. If you delete this
 *    exception statement from all source files in the program, then also delete
 *    it in the license file.
 */

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kStorage

#include "mongo/db/storage/journal_listener.h"
#include "mongo/platform/basic.h"
#include "mongo/stdx/memory.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

#include "hse.h"
#include "hse_durability_manager.h"
#include "hse_record_store.h"
#include "hse_util.h"

using hse::DUR_LAG;
using hse::KVDB;
using hse::KVDBData;

using namespace std;
using namespace std::chrono;

namespace mongo {
/* Start KVDBDurabilityManager */

KVDBDurabilityManager::KVDBDurabilityManager(hse::KVDB& db, bool durable, int forceLag)
    : _db(db),
      _numSyncs(0),
      _forceLag(forceLag),
      _durable(durable),
      _oplogVisibilityManager(nullptr),
      _journalListener(&NoOpJournalListener::instance) {

    if (_durable) {
        _journalFlusher = stdx::make_unique<KVDBJournalFlusher>(*this);
        _journalFlusher->go();
    }
}

void KVDBDurabilityManager::setJournalListener(JournalListener* jl) {
    std::lock_guard<std::mutex> lock(_journalListenerMutex);
    _journalListener = jl;
}

void KVDBDurabilityManager::setOplogVisibilityManager(KVDBCappedVisibilityManager* kcvm) {
    std::lock_guard<std::mutex> lock(_oplogMutex);
    if (kcvm) {
        // [HSE_REVISIT] In an earlier version of the code we knew things about how many
        //              times the _oplogVisibilityManager could be set to a non-NULL
        //              value. It's unclear how and whether to bring back that sort of
        //              constraint. The issue is hit in the unit tests, at the least,
        //              where a durability manager persists across two instances of
        //              a KVDBOplogStore being created.
        _oplogVisibilityManager = kcvm;
    } else {
        _oplogVisibilityManager = nullptr;
    }
}

void KVDBDurabilityManager::sync() {
    if (!_durable)
        return;

    std::lock_guard<std::mutex> lock(_journalListenerMutex);
    JournalListener::Token token = _journalListener->getToken();

    int64_t newBound;

    _oplogMutex.lock();
    if (_oplogVisibilityManager) {
        // All records prior to the current commitBoundary are known to be durable after
        // this sync.
        newBound = _oplogVisibilityManager->getCommitBoundary();
    }

    invariantHseSt(_db.kvdb_sync());

    if (_oplogVisibilityManager) {
        // Some oplog records may have been persisted as a result of this sync. Notify
        // the visibility manager about the records newly persisted.
        // [HSE_REVISIT] Avoid calling this if the newBound hasn't changed. The only case
        // to handle is when persistBoundary changes to something other than what we
        // notified the visibility manager about (truncate/init/any reset).
        _oplogVisibilityManager->durableCallback(newBound);
    }
    _oplogMutex.unlock();

    _syncMutex.lock();
    _numSyncs++;
    _syncMutex.unlock();
    _syncDoneCV.notify_all();  // Notify all waitUntilDurable threads that a sync just completed.

    _journalListener->onDurable(token);
}

void KVDBDurabilityManager::waitUntilDurable() {
    _numWaits++;
    auto waitUndo = MakeGuard([&] { _numWaits--; });

    if (!_durable)
        return;

    stdx::unique_lock<stdx::mutex> lk(_syncMutex);
    const auto waitFor = _numSyncs + 1;
    _journalFlusher->notifyFlusher();
    _syncDoneCV.wait(lk, [&] { return (_numSyncs > waitFor) || _shuttingDown.load(); });
}

void KVDBDurabilityManager::prepareForShutdown() {
    // make sure no threads are waiting on syncs.
    this->sync();
    _shuttingDown.store(true);
    _syncDoneCV.notify_all();

    while (_numWaits.load()) {
        sleepmillis(1);
    }

    if (_journalFlusher) {
        _journalFlusher->shutdown();
        _journalFlusher.reset();
    }
}

/* End KVDBDurabilityManager */

/* Start KVDBJournalFlusher */
KVDBJournalFlusher::KVDBJournalFlusher(KVDBDurabilityManager& durabilityManager)
    : BackgroundJob(false /* deleteSelf */),
      _durabilityManager(durabilityManager),
      _flushPending(false) {}

std::string KVDBJournalFlusher::name() const {
    return "KVDBJournalFlusher";
}

void KVDBJournalFlusher::notifyFlusher() {
    {
        stdx::unique_lock<stdx::mutex> lk(_jFlushMutex);
        _flushPending = true;
    }
    _jFlushCV.notify_one();
}

void KVDBJournalFlusher::run() {
    Client::initThread(name().c_str());

    uint64_t now_ms, last_ms, lag_ms;
    unsigned int commit_ms = DUR_LAG;

    LOG(1) << "starting " << name() << " thread";

    if (storageGlobalParams.journalCommitIntervalMs > 0)
        commit_ms = storageGlobalParams.journalCommitIntervalMs;
    now_ms = last_ms = lag_ms = 0;

    while (!_shuttingDown.load()) {
        now_ms = std::chrono::duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                     .count();
        lag_ms = (now_ms > last_ms) ? now_ms - last_ms : 0;

        if (lag_ms < commit_ms) {
            stdx::unique_lock<stdx::mutex> lk(_jFlushMutex);
            _jFlushCV.wait_until(
                lk, steady_clock::now() + std::chrono::milliseconds(commit_ms - lag_ms), [&] {
                    return _flushPending || _shuttingDown.load();
                });
            _flushPending = false;
        }

        if (_shuttingDown.load())
            break;

        try {
            last_ms =
                std::chrono::duration_cast<milliseconds>(steady_clock::now().time_since_epoch())
                    .count();
            _durabilityManager.sync();
        } catch (const UserException& e) {
            invariantHse(e.getCode() == ErrorCodes::ShutdownInProgress);
        }
    }
    LOG(1) << "stopping " << name() << " thread";
}

void KVDBJournalFlusher::shutdown() {
    _shuttingDown.store(true);
    this->notifyFlusher();
    wait();
}
/* End KVDBJournalFlusher */

}  // namespace mongo
