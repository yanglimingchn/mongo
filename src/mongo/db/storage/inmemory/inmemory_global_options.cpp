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

#include "mongo/platform/basic.h"

#include "mongo/base/status.h"
#include "mongo/db/storage/inmemory/inmemory_global_options.h"
#include "mongo/db/storage/inmemory/inmemory_record_store.h"
#include "mongo/util/log.h"
#include "mongo/util/options_parser/constraints.h"

namespace mongo {

InMemoryGlobalOptions inMemoryGlobalOptions;

Status InMemoryGlobalOptions::add(moe::OptionSection* options) {
    moe::OptionSection inMemoryOptions("InMemory options");

    // InMemory storage engine options
    inMemoryOptions.addOptionChaining("storage.inMemory.engineConfig.inMemorySizeGB",
                                        "inMemorySizeGB",
                                        moe::Int,
                                        "maximum amount of memory to allocate for cache; "
                                        "defaults to 1/2 of physical RAM").validRange(1, 10000);
    inMemoryOptions.addOptionChaining(
                          "storage.inMemory.engineConfig.statisticsLogDelaySecs",
                          "inMemoryStatisticsLogDelaySecs",
                          moe::Int,
                          "seconds to wait between each write to a statistics file in the dbpath; "
                          "0 means do not log statistics")
        .validRange(0, 100000)
        .setDefault(moe::Value(0));
    inMemoryOptions.addOptionChaining("storage.inMemory.engineConfig.journalCompressor",
                                        "inMemoryJournalCompressor",
                                        moe::String,
                                        "use a compressor for log records [none|snappy|zlib]")
        .format("(:?none)|(:?snappy)|(:?zlib)", "(none/snappy/zlib)")
        .setDefault(moe::Value(std::string("snappy")));
    inMemoryOptions.addOptionChaining("storage.inMemory.engineConfig.directoryForIndexes",
                                        "inMemoryDirectoryForIndexes",
                                        moe::Switch,
                                        "Put indexes and data in different directories");
    inMemoryOptions.addOptionChaining("storage.inMemory.engineConfig.configString",
                                        "inMemoryEngineConfigString",
                                        moe::String,
                                        "InMemory storage engine custom "
                                        "configuration settings").hidden();

    // InMemory collection options
    inMemoryOptions.addOptionChaining("storage.inMemory.collectionConfig.blockCompressor",
                                        "inMemoryCollectionBlockCompressor",
                                        moe::String,
                                        "block compression algorithm for collection data "
                                        "[none|snappy|zlib]")
        .format("(:?none)|(:?snappy)|(:?zlib)", "(none/snappy/zlib)")
        .setDefault(moe::Value(std::string("snappy")));
    inMemoryOptions.addOptionChaining("storage.inMemory.collectionConfig.configString",
                                        "inMemoryCollectionConfigString",
                                        moe::String,
                                        "InMemory custom collection configuration settings")
        .hidden();


    // InMemory index options
    inMemoryOptions.addOptionChaining("storage.inMemory.indexConfig.prefixCompression",
                                        "inMemoryIndexPrefixCompression",
                                        moe::Bool,
                                        "use prefix compression on row-store leaf pages")
        .setDefault(moe::Value(true));
    inMemoryOptions.addOptionChaining("storage.inMemory.indexConfig.configString",
                                        "inMemoryIndexConfigString",
                                        moe::String,
                                        "InMemory custom index configuration settings").hidden();

    return options->addSection(inMemoryOptions);
}

Status InMemoryGlobalOptions::store(const moe::Environment& params,
                                      const std::vector<std::string>& args) {
    // InMemory storage engine options
    if (params.count("storage.inMemory.engineConfig.inMemorySizeGB")) {
        inMemoryGlobalOptions.cacheSizeGB =
            params["storage.inMemory.engineConfig.inMemorySizeGB"].as<int>();
    }
    if (params.count("storage.syncPeriodSecs")) {
        inMemoryGlobalOptions.checkpointDelaySecs =
            static_cast<size_t>(params["storage.syncPeriodSecs"].as<double>());
    }
    if (params.count("storage.inMemory.engineConfig.statisticsLogDelaySecs")) {
        inMemoryGlobalOptions.statisticsLogDelaySecs =
            params["storage.inMemory.engineConfig.statisticsLogDelaySecs"].as<int>();
    }
    if (params.count("storage.inMemory.engineConfig.journalCompressor")) {
        inMemoryGlobalOptions.journalCompressor =
            params["storage.inMemory.engineConfig.journalCompressor"].as<std::string>();
    }
    if (params.count("storage.inMemory.engineConfig.directoryForIndexes")) {
        inMemoryGlobalOptions.directoryForIndexes =
            params["storage.inMemory.engineConfig.directoryForIndexes"].as<bool>();
    }
    if (params.count("storage.inMemory.engineConfig.configString")) {
        inMemoryGlobalOptions.engineConfig =
            params["storage.inMemory.engineConfig.configString"].as<std::string>();
        log() << "Engine custom option: " << inMemoryGlobalOptions.engineConfig;
    }

    // InMemory collection options
    if (params.count("storage.inMemory.collectionConfig.blockCompressor")) {
        inMemoryGlobalOptions.collectionBlockCompressor =
            params["storage.inMemory.collectionConfig.blockCompressor"].as<std::string>();
    }
    if (params.count("storage.inMemory.collectionConfig.configString")) {
        inMemoryGlobalOptions.collectionConfig =
            params["storage.inMemory.collectionConfig.configString"].as<std::string>();
        log() << "Collection custom option: " << inMemoryGlobalOptions.collectionConfig;
    }

    // InMemory index options
    if (params.count("storage.inMemory.indexConfig.prefixCompression")) {
        inMemoryGlobalOptions.useIndexPrefixCompression =
            params["storage.inMemory.indexConfig.prefixCompression"].as<bool>();
    }
    if (params.count("storage.inMemory.indexConfig.configString")) {
        inMemoryGlobalOptions.indexConfig =
            params["storage.inMemory.indexConfig.configString"].as<std::string>();
        log() << "Index custom option: " << inMemoryGlobalOptions.indexConfig;
    }

    return Status::OK();
}

}  // namespace mongo
