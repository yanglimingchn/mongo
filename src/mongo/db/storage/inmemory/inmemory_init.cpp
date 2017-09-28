/**
 *    Copyright (C) 2014 MongoDB Inc.
 *
 *    This program is free software: you can redistribute it and/or  modify
 *    it under the terms of the GNU Affero General Public License, version 3,
 *    as published by the Free Software Foundation.
 *
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

#include "mongo/base/init.h"
#include "mongo/db/catalog/collection_options.h"
#include "mongo/db/service_context_d.h"
#include "mongo/db/service_context.h"
#include "mongo/db/jsobj.h"
#include "mongo/db/storage/kv/kv_storage_engine.h"
#include "mongo/db/storage/storage_engine_lock_file.h"
#include "mongo/db/storage/storage_engine_metadata.h"
#include "mongo/db/storage/inmemory/inmemory_kv_engine.h"
#include "mongo/db/storage/inmemory/inmemory_global_options.h"
#include "mongo/db/storage/inmemory/inmemory_index.h"
#include "mongo/db/storage/inmemory/inmemory_parameters.h"
#include "mongo/db/storage/inmemory/inmemory_record_store.h"
#include "mongo/db/storage/inmemory/inmemory_server_status.h"
#include "mongo/db/storage/inmemory/inmemory_util.h"
#include "mongo/db/storage/storage_options.h"
#include "mongo/util/log.h"

namespace mongo {

namespace {
class InMemoryFactory : public StorageEngine::Factory {
public:
    virtual ~InMemoryFactory() {}
    virtual StorageEngine* create(const StorageGlobalParams& params,
                                  const StorageEngineLockFile& lockFile) const {
        if (lockFile.createdByUncleanShutdown()) {
            warning() << "Recovering data from the last clean checkpoint.";
        }

        size_t cacheSizeGB = inMemoryGlobalOptions.cacheSizeGB;
        if (cacheSizeGB == 0) {
            // Since the user didn't provide a cache size, choose a reasonable default value.
            // We want to reserve 1GB for the system and binaries, but it's not bad to
            // leave a fair amount left over for pagecache since that's compressed storage.
            ProcessInfo pi;
            double memSizeMB = pi.getMemSizeMB();
            if (memSizeMB > 0) {
                double cacheMB = (memSizeMB - 1024) * 0.6;
                cacheSizeGB = static_cast<size_t>(cacheMB / 1024);
                if (cacheSizeGB < 1)
                    cacheSizeGB = 1;
            }
        }
        const bool ephemeral = true;
        InMemoryKVEngine* kv = new InMemoryKVEngine(getCanonicalName().toString(),
                                                        params.dbpath,
                                                        inMemoryGlobalOptions.engineConfig,
                                                        cacheSizeGB,
                                                        false,
                                                        ephemeral,
                                                        params.repair);
        kv->setRecordStoreExtraOptions(inMemoryGlobalOptions.collectionConfig);
        kv->setSortedDataInterfaceExtraOptions(inMemoryGlobalOptions.indexConfig);
        // Intentionally leaked.
        new InMemoryServerStatusSection(kv);
        new InMemoryEngineRuntimeConfigParameter(kv);

        KVStorageEngineOptions options;
        options.directoryPerDB = params.directoryperdb;
        options.directoryForIndexes = inMemoryGlobalOptions.directoryForIndexes;
        options.forRepair = params.repair;
        return new KVStorageEngine(kv, options);
    }

    virtual StringData getCanonicalName() const {
        return kInMemoryEngineName;
    }

    virtual Status validateCollectionStorageOptions(const BSONObj& options) const {
        return InMemoryRecordStore::parseOptionsField(options).getStatus();
    }

    virtual Status validateIndexStorageOptions(const BSONObj& options) const {
        return InMemoryIndex::parseIndexOptions(options).getStatus();
    }

    virtual Status validateMetadata(const StorageEngineMetadata& metadata,
                                    const StorageGlobalParams& params) const {
        Status status =
            metadata.validateStorageEngineOption("directoryPerDB", params.directoryperdb);
        if (!status.isOK()) {
            return status;
        }

        status = metadata.validateStorageEngineOption("directoryForIndexes",
                                                      inMemoryGlobalOptions.directoryForIndexes);
        if (!status.isOK()) {
            return status;
        }

        return Status::OK();
    }

    virtual BSONObj createMetadataOptions(const StorageGlobalParams& params) const {
        BSONObjBuilder builder;
        builder.appendBool("directoryPerDB", params.directoryperdb);
        builder.appendBool("directoryForIndexes", inMemoryGlobalOptions.directoryForIndexes);
        return builder.obj();
    }
};
}  // namespace

MONGO_INITIALIZER_WITH_PREREQUISITES(InMemoryEngineInit, ("SetGlobalEnvironment"))
(InitializerContext* context) {
    getGlobalServiceContext()->registerStorageEngine(kInMemoryEngineName,
                                                     new InMemoryFactory());

    return Status::OK();
}
}
