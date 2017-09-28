// wiredtiger_recovery_unit.cpp

/**
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

#include "mongo/base/checked_cast.h"
#include "mongo/base/init.h"
#include "mongo/bson/bsonobjbuilder.h"
#include "mongo/db/storage/inmemory/inmemory_recovery_unit.h"
#include "mongo/db/storage/inmemory/inmemory_session_cache.h"
#include "mongo/db/storage/inmemory/inmemory_util.h"
#include "mongo/stdx/condition_variable.h"
#include "mongo/stdx/mutex.h"
#include "mongo/util/concurrency/ticketholder.h"
#include "mongo/util/log.h"
#include "mongo/util/mongoutils/str.h"
#include "mongo/util/stacktrace.h"

namespace mongo {
namespace {
// SnapshotIds need to be globally unique, as they are used in a WorkingSetMember to
// determine if documents changed, but a different recovery unit may be used across a getMore,
// so there is a chance the snapshot ID will be reused.
AtomicUInt64 nextSnapshotId{1};
}  // namespace

InMemoryRecoveryUnit::InMemoryRecoveryUnit(InMemorySessionCache* sc)
    : _sessionCache(sc),
      _session(NULL),
      _inUnitOfWork(false),
      _active(false),
      _mySnapshotId(nextSnapshotId.fetchAndAdd(1)),
      _everStartedWrite(false) {}

InMemoryRecoveryUnit::~InMemoryRecoveryUnit() {
    invariant(!_inUnitOfWork);
    _abort();
    if (_session) {
        _sessionCache->releaseSession(_session);
        _session = NULL;
    }
}

void InMemoryRecoveryUnit::reportState(BSONObjBuilder* b) const {
    b->append("wt_inUnitOfWork", _inUnitOfWork);
    b->append("wt_active", _active);
    b->append("wt_everStartedWrite", _everStartedWrite);
    b->appendNumber("wt_mySnapshotId", static_cast<long long>(_mySnapshotId));
    if (_active)
        b->append("wt_millisSinceCommit", _timer.millis());
}

void InMemoryRecoveryUnit::prepareForCreateSnapshot(OperationContext* opCtx) {
    invariant(!_active);  // Can't already be in a WT transaction.
    invariant(!_inUnitOfWork);
    invariant(!_readFromMajorityCommittedSnapshot);

    // Starts the WT transaction that will be the basis for creating a named snapshot.
    getSession(opCtx);
    _areWriteUnitOfWorksBanned = true;
}

void InMemoryRecoveryUnit::_commit() {
    try {
        if (_session && _active) {
            _txnClose(true);
        }

        for (Changes::const_iterator it = _changes.begin(), end = _changes.end(); it != end; ++it) {
            (*it)->commit();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void InMemoryRecoveryUnit::_abort() {
    try {
        if (_session && _active) {
            _txnClose(false);
        }

        for (Changes::const_reverse_iterator it = _changes.rbegin(), end = _changes.rend();
             it != end;
             ++it) {
            Change* change = *it;
            LOG(2) << "CUSTOM ROLLBACK " << demangleName(typeid(*change));
            change->rollback();
        }
        _changes.clear();

        invariant(!_active);
    } catch (...) {
        std::terminate();
    }
}

void InMemoryRecoveryUnit::beginUnitOfWork(OperationContext* opCtx) {
    invariant(!_areWriteUnitOfWorksBanned);
    invariant(!_inUnitOfWork);
    _inUnitOfWork = true;
    _everStartedWrite = true;
}

void InMemoryRecoveryUnit::commitUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _commit();
}

void InMemoryRecoveryUnit::abortUnitOfWork() {
    invariant(_inUnitOfWork);
    _inUnitOfWork = false;
    _abort();
}

void InMemoryRecoveryUnit::_ensureSession() {
    if (!_session) {
        _session = _sessionCache->getSession();
    }
}

bool InMemoryRecoveryUnit::waitUntilDurable() {
    invariant(!_inUnitOfWork);
    // _session may be nullptr. We cannot _ensureSession() here as that needs shutdown protection.
    _sessionCache->waitUntilDurable(false);
    return true;
}

void InMemoryRecoveryUnit::registerChange(Change* change) {
    invariant(_inUnitOfWork);
    _changes.push_back(change);
}

InMemoryRecoveryUnit* InMemoryRecoveryUnit::get(OperationContext* txn) {
    invariant(txn);
    return checked_cast<InMemoryRecoveryUnit*>(txn->recoveryUnit());
}

void InMemoryRecoveryUnit::assertInActiveTxn() const {
    fassert(58575, _active);
}

InMemorySession* InMemoryRecoveryUnit::getSession(OperationContext* opCtx) {
    if (!_active) {
        _txnOpen(opCtx);
    }
    return _session;
}

InMemorySession* InMemoryRecoveryUnit::getSessionNoTxn(OperationContext* opCtx) {
    _ensureSession();
    return _session;
}

void InMemoryRecoveryUnit::abandonSnapshot() {
    invariant(!_inUnitOfWork);
    if (_active) {
        // Can't be in a WriteUnitOfWork, so safe to rollback
        _txnClose(false);
    }
    _areWriteUnitOfWorksBanned = false;
}

void* InMemoryRecoveryUnit::writingPtr(void* data, size_t len) {
    // This API should not be used for anything other than the MMAP V1 storage engine
    MONGO_UNREACHABLE;
}

void InMemoryRecoveryUnit::setOplogReadTill(const RecordId& id) {
    _oplogReadTill = id;
}

void InMemoryRecoveryUnit::_txnClose(bool commit) {
    invariant(_active);
    WT_SESSION* s = _session->getSession();
    if (commit) {
        invariantWTOK(s->commit_transaction(s, NULL));
        LOG(3) << "WT commit_transaction for snapshot id " << _mySnapshotId;
    } else {
        invariantWTOK(s->rollback_transaction(s, NULL));
        LOG(3) << "WT rollback_transaction for snapshot id " << _mySnapshotId;
    }
    _active = false;
    _mySnapshotId = nextSnapshotId.fetchAndAdd(1);
    _oplogReadTill = RecordId();
}

SnapshotId InMemoryRecoveryUnit::getSnapshotId() const {
    // TODO: use actual wiredtiger txn id
    return SnapshotId(_mySnapshotId);
}

Status InMemoryRecoveryUnit::setReadFromMajorityCommittedSnapshot() {
    auto snapshotName = _sessionCache->snapshotManager().getMinSnapshotForNextCommittedRead();
    if (!snapshotName) {
        return {ErrorCodes::ReadConcernMajorityNotAvailableYet,
                "Read concern majority reads are currently not possible."};
    }

    _majorityCommittedSnapshot = *snapshotName;
    _readFromMajorityCommittedSnapshot = true;
    return Status::OK();
}

boost::optional<SnapshotName> InMemoryRecoveryUnit::getMajorityCommittedSnapshot() const {
    if (!_readFromMajorityCommittedSnapshot)
        return {};
    return _majorityCommittedSnapshot;
}

void InMemoryRecoveryUnit::_txnOpen(OperationContext* opCtx) {
    invariant(!_active);
    _ensureSession();

    WT_SESSION* s = _session->getSession();

    if (_readFromMajorityCommittedSnapshot) {
        _majorityCommittedSnapshot =
            _sessionCache->snapshotManager().beginTransactionOnCommittedSnapshot(s);
    } else {
        invariantWTOK(s->begin_transaction(s, NULL));
    }

    LOG(3) << "WT begin_transaction for snapshot id " << _mySnapshotId;
    _timer.reset();
    _active = true;
}

// ---------------------

InMemoryCursor::InMemoryCursor(const std::string& uri,
                                   uint64_t tableId,
                                   bool forRecordStore,
                                   OperationContext* txn) {
    _tableID = tableId;
    _ru = InMemoryRecoveryUnit::get(txn);
    _session = _ru->getSession(txn);
    _cursor = _session->getCursor(uri, tableId, forRecordStore);
    if (!_cursor) {
        error() << "no cursor for uri: " << uri;
    }
}

InMemoryCursor::~InMemoryCursor() {
    _session->releaseCursor(_tableID, _cursor);
    _cursor = NULL;
}

void InMemoryCursor::reset() {
    invariantWTOK(_cursor->reset(_cursor));
}

WT_SESSION* InMemoryCursor::getWTSession() {
    return _session->getSession();
}
}
