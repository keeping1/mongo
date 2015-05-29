/**
 *    Copyright (C) 2015 MongoDB Inc.
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

#define MONGO_LOG_DEFAULT_COMPONENT ::mongo::logger::LogComponent::kQuery

#include "mongo/platform/basic.h"

#include <memory>
#include <string>

#include "mongo/base/disallow_copying.h"
#include "mongo/db/auth/authorization_session.h"
#include "mongo/db/catalog/collection.h"
#include "mongo/db/catalog/cursor_manager.h"
#include "mongo/db/clientcursor.h"
#include "mongo/db/commands.h"
#include "mongo/db/db_raii.h"
#include "mongo/db/exec/working_set_common.h"
#include "mongo/db/global_timestamp.h"
#include "mongo/db/repl/oplog.h"
#include "mongo/db/service_context.h"
#include "mongo/db/query/cursor_responses.h"
#include "mongo/db/query/find.h"
#include "mongo/db/query/getmore_request.h"
#include "mongo/db/stats/counters.h"
#include "mongo/util/log.h"
#include "mongo/util/scopeguard.h"

namespace mongo {

    /**
     * A command for running getMore() against an existing cursor registered with a CursorManager.
     * Used to generate the next batch of results for a ClientCursor.
     *
     * Can be used in combination with any cursor-generating command (e.g. find, aggregate,
     * listIndexes).
     */
    class GetMoreCmd : public Command {
        MONGO_DISALLOW_COPYING(GetMoreCmd);
    public:
        GetMoreCmd() : Command("getMore") { }

        bool isWriteCommandForConfigServer() const override { return false; }

        bool slaveOk() const override { return false; }

        bool slaveOverrideOk() const override { return true; }

        bool maintenanceOk() const override { return false; }

        bool adminOnly() const override { return false; }

        void help(std::stringstream& help) const override {
            help << "retrieve more results from an existing cursor";
        }

        /**
         * A getMore command increments the getMore counter, not the command counter.
         */
        bool shouldAffectCommandCounter() const override { return false; }

        std::string parseNs(const std::string& dbname, const BSONObj& cmdObj) const override {
            return GetMoreRequest::parseNs(dbname, cmdObj);
        }

        Status checkAuthForCommand(ClientBasic* client,
                                   const std::string& dbname,
                                   const BSONObj& cmdObj) override {
            StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
            if (!parseStatus.isOK()) {
                return parseStatus.getStatus();
            }
            const GetMoreRequest& request = parseStatus.getValue();

            return AuthorizationSession::get(client)->checkAuthForGetMore(request.nss,
                                                                          request.cursorid);
        }

        bool run(OperationContext* txn,
                 const std::string& dbname,
                 BSONObj& cmdObj,
                 int options,
                 std::string& errmsg,
                 BSONObjBuilder& result) override {
            // Counted as a getMore, not as a command.
            globalOpCounters.gotGetMore();

            if (txn->getClient()->isInDirectClient()) {
                return appendCommandStatus(result,
                                           Status(ErrorCodes::IllegalOperation,
                                                  "Cannot run getMore command from eval()"));
            }

            StatusWith<GetMoreRequest> parseStatus = GetMoreRequest::parseFromBSON(dbname, cmdObj);
            if (!parseStatus.isOK()) {
                return appendCommandStatus(result, parseStatus.getStatus());
            }
            const GetMoreRequest& request = parseStatus.getValue();

            // Depending on the type of cursor being operated on, we hold locks for the whole
            // getMore, or none of the getMore, or part of the getMore.  The three cases in detail:
            //
            // 1) Normal cursor: we lock with "ctx" and hold it for the whole getMore.
            // 2) Cursor owned by global cursor manager: we don't lock anything.  These cursors
            //    don't own any collection state.
            // 3) Agg cursor: we lock with "ctx", then release, then relock with "unpinDBLock" and
            //    "unpinCollLock".  This is because agg cursors handle locking internally (hence the
            //    release), but the pin and unpin of the cursor must occur under the collection
            //    lock. We don't use our AutoGetCollectionForRead "ctx" to relock, because
            //    AutoGetCollectionForRead checks the sharding version (and we want the relock for
            //    the unpin to succeed even if the sharding version has changed).
            //
            // Note that we declare our locks before our ClientCursorPin, in order to ensure that
            // the pin's destructor is called before the lock destructors (so that the unpin occurs
            // under the lock).
            std::unique_ptr<AutoGetCollectionForRead> ctx;
            std::unique_ptr<Lock::DBLock> unpinDBLock;
            std::unique_ptr<Lock::CollectionLock> unpinCollLock;

            CursorManager* cursorManager;
            CursorManager* globalCursorManager = CursorManager::getGlobalCursorManager();
            if (globalCursorManager->ownsCursorId(request.cursorid)) {
                cursorManager = globalCursorManager;
            }
            else {
                ctx.reset(new AutoGetCollectionForRead(txn, request.nss));
                Collection* collection = ctx->getCollection();
                if (!collection) {
                    return appendCommandStatus(result,
                                               Status(ErrorCodes::OperationFailed,
                                                      "collection dropped between getMore calls"));
                }
                cursorManager = collection->getCursorManager();
            }

            ClientCursorPin ccPin(cursorManager, request.cursorid);
            ClientCursor* cursor = ccPin.c();
            if (!cursor) {
                // We didn't find the cursor.
                return appendCommandStatus(result, Status(ErrorCodes::CursorNotFound, str::stream()
                    << "Cursor not found, cursor id: " << request.cursorid));
            }

            if (request.nss.ns() != cursor->ns()) {
                return appendCommandStatus(result, Status(ErrorCodes::Unauthorized, str::stream()
                    << "Requested getMore on namespace '" << request.nss.ns()
                    << "', but cursor belongs to a different namespace"));
            }

            const bool hasOwnMaxTime = CurOp::get(txn)->isMaxTimeSet();

            // Validation related to awaitData.
            if (isCursorAwaitData(cursor)) {
                invariant(isCursorTailable(cursor));

                if (!hasOwnMaxTime) {
                    Status status(ErrorCodes::BadValue,
                                  str::stream() << "Must set maxTimeMS on a getMore if the initial "
                                                << "query had 'awaitData' set: " << cmdObj);
                    return appendCommandStatus(result, status);
                }

                if (cursor->isAggCursor()) {
                    Status status(ErrorCodes::BadValue,
                                  "awaitData cannot be set on an aggregation cursor");
                    return appendCommandStatus(result, status);
                }
            }

            // On early return, get rid of the cursor.
            ScopeGuard cursorFreer = MakeGuard(&GetMoreCmd::cleanupCursor, txn, &ccPin, request);

            if (!cursor->hasRecoveryUnit()) {
                // Start using a new RecoveryUnit.
                cursor->setOwnedRecoveryUnit(
                    getGlobalServiceContext()->getGlobalStorageEngine()->newRecoveryUnit());
            }

            // Swap RecoveryUnit(s) between the ClientCursor and OperationContext.
            ScopedRecoveryUnitSwapper ruSwapper(cursor, txn);

            // Reset timeout timer on the cursor since the cursor is still in use.
            cursor->setIdleTime(0);

            // If there is no time limit set directly on this getMore command, but the operation
            // that spawned this cursor had a time limit set, then we have to apply any leftover
            // time to this getMore.
            if (!hasOwnMaxTime) {
                CurOp::get(txn)->setMaxTimeMicros(cursor->getLeftoverMaxTimeMicros());
            }
            txn->checkForInterrupt(); // May trigger maxTimeAlwaysTimeOut fail point.

            if (cursor->isAggCursor()) {
                // Agg cursors handle their own locking internally.
                ctx.reset(); // unlocks
            }

            PlanExecutor* exec = cursor->getExecutor();
            exec->restoreState(txn);

            // If we're tailing a capped collection, retrieve a monotonically increasing insert
            // counter.
            uint64_t lastInsertCount = 0;
            if (isCursorAwaitData(cursor)) {
                invariant(ctx->getCollection()->isCapped());
                lastInsertCount = ctx->getCollection()->getCappedInsertNotifier()->getCount();
            }

            CursorId respondWithId = 0;
            BSONArrayBuilder nextBatch;
            BSONObj obj;
            PlanExecutor::ExecState state;
            int numResults = 0;
            Status batchStatus = generateBatch(cursor, request, &nextBatch, &state, &numResults);
            if (!batchStatus.isOK()) {
                return appendCommandStatus(result, batchStatus);
            }

            // If this is an await data cursor, and we hit EOF without generating any results, then
            // we block waiting for new oplog data to arrive.
            if (isCursorAwaitData(cursor) && state == PlanExecutor::IS_EOF && numResults == 0) {
                // Retrieve the notifier which we will wait on until new data arrives. We make sure
                // to do this in the lock because once we drop the lock it is possible for the
                // collection to become invalid. The notifier itself will outlive the collection if
                // the collection is dropped, as we keep a shared_ptr to it.
                auto notifier = ctx->getCollection()->getCappedInsertNotifier();

                // Save the PlanExecutor and drop our locks.
                exec->saveState();
                ctx.reset();

                // Block waiting for data.
                Microseconds timeout(CurOp::get(txn)->getRemainingMaxTimeMicros());
                notifier->waitForInsert(lastInsertCount, timeout);
                notifier.reset();

                ctx.reset(new AutoGetCollectionForRead(txn, request.nss));
                exec->restoreState(txn);

                // We woke up because either the timed_wait expired, or there was more data. Either
                // way, attempt to generate another batch of results.
                batchStatus = generateBatch(cursor, request, &nextBatch, &state, &numResults);
                if (!batchStatus.isOK()) {
                    return appendCommandStatus(result, batchStatus);
                }
            }

            if (shouldSaveCursorGetMore(state, exec, isCursorTailable(cursor))) {
                respondWithId = request.cursorid;

                exec->saveState();

                // If maxTimeMS was set directly on the getMore rather than being rolled over
                // from a previous find, then don't roll remaining micros over to the next
                // getMore.
                if (!hasOwnMaxTime) {
                    cursor->setLeftoverMaxTimeMicros(CurOp::get(txn)->getRemainingMaxTimeMicros());
                }

                cursor->incPos(numResults);

                if (isCursorTailable(cursor) && state == PlanExecutor::IS_EOF) {
                    // Rather than swapping their existing RU into the client cursor, tailable
                    // cursors should get a new recovery unit.
                    ruSwapper.dismiss();
                }
            }
            else {
                CurOp::get(txn)->debug().cursorExhausted = true;
            }

            appendGetMoreResponseObject(respondWithId, request.nss.ns(), nextBatch.arr(), &result);

            if (respondWithId) {
                cursorFreer.Dismiss();

                // If we are operating on an aggregation cursor, then we dropped our collection lock
                // earlier and need to reacquire it in order to clean up our ClientCursorPin.
                if (cursor->isAggCursor()) {
                    invariant(NULL == ctx.get());
                    unpinDBLock.reset(
                        new Lock::DBLock(txn->lockState(), request.nss.db(), MODE_IS));
                    unpinCollLock.reset(
                        new Lock::CollectionLock(txn->lockState(), request.nss.ns(), MODE_IS));
                }
            }

            return true;
        }

        /**
         * Uses 'cursor' and 'request' to fill out 'nextBatch' with the batch of result documents to
         * be returned by this getMore.
         *
         * Returns the number of documents in the batch in *numResults, which must be initialized to
         * zero by the caller. Returns the final ExecState returned by the cursor in *state.
         *
         * Returns an OK status if the batch was successfully generated, and a non-OK status if the
         * PlanExecutor encounters a failure.
         */
        Status generateBatch(ClientCursor* cursor,
                             const GetMoreRequest& request,
                             BSONArrayBuilder* nextBatch,
                             PlanExecutor::ExecState* state,
                             int* numResults) {
            PlanExecutor* exec = cursor->getExecutor();
            const bool isAwaitData = isCursorAwaitData(cursor);

            // If an awaitData getMore is killed during this process due to our max time expiring at
            // an interrupt point, we just continue as normal and return rather than reporting a
            // timeout to the user.
            BSONObj obj;
            try {
                while (PlanExecutor::ADVANCED == (*state = exec->getNext(&obj, NULL))) {
                    // If adding this object will cause us to exceed the BSON size limit, then we
                    // stash it for later.
                    if (nextBatch->len() + obj.objsize() > BSONObjMaxUserSize && *numResults > 0) {
                        exec->enqueue(obj);
                        break;
                    }

                    // Add result to output buffer.
                    nextBatch->append(obj);
                    (*numResults)++;

                    if (enoughForGetMore(request.batchSize.value_or(0),
                                         *numResults, nextBatch->len())) {
                        break;
                    }
                }
            }
            catch (const UserException& except) {
                if (isAwaitData && except.getCode() == ErrorCodes::ExceededTimeLimit) {
                    // We ignore exceptions from interrupt points due to max time expiry for
                    // awaitData cursors.
                }
                else {
                    throw;
                }
            }

            if (PlanExecutor::FAILURE == *state) {
                const std::unique_ptr<PlanStageStats> stats(exec->getStats());
                error() << "GetMore executor error, stats: " << Explain::statsToBSON(*stats);
                return Status(ErrorCodes::OperationFailed,
                              str::stream() << "GetMore executor error: "
                                            << WorkingSetCommon::toStatusString(obj));
            }
            else if (PlanExecutor::DEAD == *state) {
                return Status(ErrorCodes::OperationFailed,
                              str::stream() << "Plan executor killed during getMore command, "
                                            << "ns: " << request.nss.ns());
            }

            return Status::OK();
        }

        /**
         * Called via a ScopeGuard on early return in order to ensure that the ClientCursor gets
         * cleaned up properly.
         */
        static void cleanupCursor(OperationContext* txn,
                                  ClientCursorPin* ccPin,
                                  const GetMoreRequest& request) {
            ClientCursor* cursor = ccPin->c();

            std::unique_ptr<Lock::DBLock> unpinDBLock;
            std::unique_ptr<Lock::CollectionLock> unpinCollLock;

            if (cursor->isAggCursor()) {
                unpinDBLock.reset(new Lock::DBLock(txn->lockState(), request.nss.db(), MODE_IS));
                unpinCollLock.reset(
                    new Lock::CollectionLock(txn->lockState(), request.nss.ns(), MODE_IS));
            }

            ccPin->deleteUnderlying();
        }

    } getMoreCmd;

} // namespace mongo
