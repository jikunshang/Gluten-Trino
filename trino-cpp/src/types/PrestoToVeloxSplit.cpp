/*
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "src/types/PrestoToVeloxSplit.h"
#include <optional>
#include "velox/connectors/hive/HiveConnectorSplit.h"
#include "velox/connectors/tpch/TpchConnectorSplit.h"
#include "velox/exec/Exchange.h"

using namespace facebook::velox;
using namespace facebook;

namespace io::trino {

namespace {

dwio::common::FileFormat toVeloxFileFormat(const protocol::String& format) {
  if (format == "com.facebook.hive.orc.OrcInputFormat") {
    return dwio::common::FileFormat::DWRF;
  } else if (format == "org.apache.hadoop.hive.ql.io.parquet.MapredParquetInputFormat") {
    return dwio::common::FileFormat::PARQUET;
  } else {
    VELOX_FAIL("Unknown file format {}", format);
  }
}

}  // anonymous namespace

velox::exec::Split toVeloxSplit(const protocol::ScheduledSplit& scheduledSplit) {
  const auto& connectorSplit = scheduledSplit.split.connectorSplit;
  const auto splitGroupId = -1;
  if (auto hiveSplit =
          std::dynamic_pointer_cast<const protocol::HiveSplit>(connectorSplit)) {
    std::unordered_map<std::string, std::optional<std::string>> partitionKeys;
    for (const auto& entry : hiveSplit->partitionKeys) {
      VLOG(1) << entry.name << " " << entry.value;
      std::optional<std::string> partitionValue;
      if (entry.value.empty()) {
        partitionValue = std::nullopt;
      } else {
        // Transfer __HIVE_DEFAULT_PARTITION__ to 0
        // __HIVE_DEFAULT_PARTITION__ is set to '\\N' in trino, see HivePartitionKey.java
        if (entry.value == "\\N") {
          partitionValue = std::make_optional<std::string>("0");
        } else {
          partitionValue = std::make_optional<std::string>(entry.value);
        }
      }
      partitionKeys.emplace(entry.name, partitionValue);
    }
    std::unordered_map<std::string, std::string> customSplitInfo;
    for (const auto& [key, value] : hiveSplit->fileSplit.customSplitInfo) {
      customSplitInfo[key] = value;
    }
    std::shared_ptr<std::string> extraFileInfo;
    return velox::exec::Split(
        std::make_shared<connector::hive::HiveConnectorSplit>(
            scheduledSplit.split.connectorId.catalogName, hiveSplit->fileSplit.path,
            toVeloxFileFormat(hiveSplit->storage.storageFormat.inputFormat),
            hiveSplit->fileSplit.start, hiveSplit->fileSplit.length, partitionKeys,
            hiveSplit->tableBucketNumber
                ? std::optional<int>(*hiveSplit->tableBucketNumber)
                : std::nullopt,
            customSplitInfo, extraFileInfo),
        splitGroupId);
  }
  if (auto remoteSplit =
          std::dynamic_pointer_cast<const protocol::RemoteSplit>(connectorSplit)) {
    return velox::exec::Split(
        std::make_shared<exec::RemoteConnectorSplit>(remoteSplit->location.location),
        splitGroupId);
  }
  if (auto tpchSplit =
          std::dynamic_pointer_cast<const protocol::TpchSplit>(connectorSplit)) {
    return velox::exec::Split(std::make_shared<connector::tpch::TpchConnectorSplit>(
                                  "tpch", tpchSplit->totalParts, tpchSplit->partNumber),
                              splitGroupId);
  }
  // if (std::dynamic_pointer_cast<const protocol::EmptySplit>(connectorSplit)) {
  //   // We return NULL for empty splits to signal to do nothing.
  //   return velox::exec::Split(nullptr, splitGroupId);
  // }

  VELOX_CHECK(false, "Unknown split type {}", connectorSplit->_type);
}

}  // namespace io::trino
