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

#include "mongo/platform/basic.h"
#include "mongo/util/log.h"

#include "hse_impl.h"
#include "hse_kvscursor.h"
#include "hse_stats.h"
#include "hse_util.h"

#include <chrono>
#include <cstdlib>
#include <thread>

using namespace std;
using hse::Status;
using mongo::warning;

using hse_stat::_hseKvsCursorCreateCounter;
using hse_stat::_hseKvsCursorCreateLatency;
using hse_stat::_hseKvsCursorDestroyCounter;
using hse_stat::_hseKvsCursorDestroyLatency;
using hse_stat::_hseKvsCursorReadCounter;
using hse_stat::_hseKvsCursorReadLatency;

namespace {
int RETRY_FIB_SEQ_EAGAIN[] = {1, 2, 3, 5, 8, 13};
int FIB_LEN = 6;
}  // namespace

// KVDB interface
namespace hse {

KvsCursor* create_cursor(KVSHandle kvs, KVDBData& prefix, bool forward, ClientTxn* lnkd_txn) {
    return new KvsCursor(kvs, prefix, forward, lnkd_txn);
}

void KvsCursor::_kvs_cursor_create(ClientTxn* lnkd_txn) {
    int retries = 0;
    int flags = 0;
    unsigned long long sleepTime = 0;
    struct hse_kvdb_txn* kvdb_txn = nullptr;

    if (lnkd_txn)
        kvdb_txn = lnkd_txn->get_kvdb_txn();

    if (!_forward)
        flags |= HSE_CURSOR_CREATE_REV;

    /* [HSE_REVISIT] This loop retries indefinitely on an EAGAIN. */
    while (true) {
        if (retries < FIB_LEN) {
            sleepTime = RETRY_FIB_SEQ_EAGAIN[retries % FIB_LEN];
        } else {
            sleepTime = RETRY_FIB_SEQ_EAGAIN[FIB_LEN - 1];
            if (retries % 20 == 0)
                warning() << "HSE: kvs_cursor_create returning EAGAIN after " << retries
                          << " retries";
        }

        _hseKvsCursorCreateCounter.add();
        auto lt = _hseKvsCursorCreateLatency.begin();
        Status st = Status{::hse_kvs_cursor_create(
            _kvs, flags, kvdb_txn, (const void*)_pfx.data(), _pfx.len(), &_cursor)};
        _hseKvsCursorCreateLatency.end(lt);
        if (st.ok())
            break;

        if (st.getErrno() != EAGAIN)
            throw KVDBException("non EAGAIN failure from hse_kvs_cursor_create()");

        this_thread::sleep_for(chrono::milliseconds(sleepTime));

        retries++;
    }
}

KvsCursor::KvsCursor(KVSHandle handle, KVDBData& prefix, bool forward, ClientTxn* lnkd_txn)
    : _kvs((struct hse_kvs*)handle),
      _pfx(prefix),
      _forward(forward),
      _cursor(0),
      _start(0),
      _end(0),
      _curr(0),
      _kvs_key(0),
      _kvs_klen(0),
      _kvs_seek_key(0),
      _kvs_seek_klen(0),
      _kvs_val(0),
      _kvs_vlen(0) {
    _kvs_cursor_create(lnkd_txn);
}

KvsCursor::~KvsCursor() {
    _hseKvsCursorDestroyCounter.add();
    auto lt = _hseKvsCursorDestroyLatency.begin();
    ::hse_kvs_cursor_destroy(_cursor);
    _hseKvsCursorDestroyLatency.end(lt);
}

Status KvsCursor::update(ClientTxn* lnkd_txn) {
    Status st{};

    // Recreating cursor and seeking to last point. Copy out key before destroying the cursor.
    // Skip a key after seek if the last op was a read.
    bool lastOpWasRead = !_kvs_seek_key && _kvs_key;
    const void* skey = _kvs_seek_key ?: _kvs_key;
    size_t sklen = _kvs_seek_klen ?: _kvs_klen;
    auto seekKey = KVDBData((const uint8_t*)skey, (int)sklen, true);

    _hseKvsCursorDestroyCounter.add();
    auto lt = _hseKvsCursorDestroyLatency.begin();
    ::hse_kvs_cursor_destroy(_cursor);
    _hseKvsCursorDestroyLatency.end(lt);

    _kvs_cursor_create(lnkd_txn);
    st = Status{::hse_kvs_cursor_seek(
        _cursor, 0, seekKey.data(), seekKey.len(), &_kvs_seek_key, &_kvs_seek_klen)};
    if (st.ok() && lastOpWasRead) {
        // Last op was a read, if seek didn't land on the key we had read, it was deleted. Don't
        // skip.
        if (seekKey.len() == _kvs_seek_klen &&
            0 == memcmp(seekKey.data(), _kvs_seek_key, _kvs_seek_klen)) {
            bool eof;

            _read_kvs(eof);
        }
    }

    return st;
}

Status KvsCursor::seek(const KVDBData& key, const KVDBData* kmax, KVDBData* pos) {
    Status st{};

    st = Status{
        ::hse_kvs_cursor_seek(_cursor, 0, key.data(), key.len(), &_kvs_seek_key, &_kvs_seek_klen)};
    if (st.ok()) {
        if (pos)
            *pos = KVDBData((const uint8_t*)_kvs_seek_key, (int)_kvs_seek_klen);
    }

    return st;
}

Status KvsCursor::read(KVDBData& key, KVDBData& val, bool& eof) {
    // We have guaranteed that the only possible error value returned is ECANCELED, which
    // we will return eagerly even if the "next" value might be from the connector itself.
    int ret = _read_kvs(eof);
    if (ret)
        return ret;

    if (!eof) {
        key = KVDBData((const uint8_t*)_kvs_key, (int)_kvs_klen);
        val = KVDBData((const uint8_t*)_kvs_val, (int)_kvs_vlen);
    }

    return 0;
}

int KvsCursor::_read_kvs(bool& eof) {
    Status st{};
    bool _eof;

    _kvs_seek_key = 0;
    _kvs_seek_klen = 0;

    _hseKvsCursorReadCounter.add();
    auto lt = _hseKvsCursorReadLatency.begin();
    st = Status{
        ::hse_kvs_cursor_read(_cursor, 0, &_kvs_key, &_kvs_klen, &_kvs_val, &_kvs_vlen, &_eof)};
    _hseKvsCursorReadLatency.end(lt);

    eof = _eof;
    return st.getErrno();
}

Status KvsCursor::save() {
    return 0;
}

Status KvsCursor::restore() {
    return 0;
}
}  // namespace hse
