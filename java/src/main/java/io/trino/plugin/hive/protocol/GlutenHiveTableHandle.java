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

package io.trino.plugin.hive.protocol;

import com.fasterxml.jackson.annotation.JsonCreator;
import com.fasterxml.jackson.annotation.JsonProperty;
import io.trino.spi.protocol.GlutenConnectorTableHandle;
import io.trino.spi.protocol.GlutenSubfield;
import io.trino.spi.protocol.GlutenTupleDomain;

import java.util.List;
import java.util.Optional;

import static java.util.Objects.requireNonNull;

public class GlutenHiveTableHandle
        implements GlutenConnectorTableHandle
{
    private final String schemaName;
    private final String tableName;
    private final GlutenTupleDomain<GlutenSubfield> domainPredicate;
    private final Optional<List<List<String>>> analyzePartitionValues;

    @JsonCreator
    public GlutenHiveTableHandle(
            @JsonProperty("schemaName") String schemaName,
            @JsonProperty("tableName") String tableName,
            @JsonProperty("domainPredicate") GlutenTupleDomain<GlutenSubfield> domainPredicate,
            @JsonProperty("analyzePartitionValues") Optional<List<List<String>>> analyzePartitionValues)
    {
        this.schemaName = requireNonNull(schemaName, "schemaName is null");
        this.tableName = requireNonNull(tableName, "tableName is null");
        this.domainPredicate = requireNonNull(domainPredicate, "domainPredicate is null");
        this.analyzePartitionValues = requireNonNull(analyzePartitionValues, "analyzePartitionValues is null");
    }

    @JsonProperty
    public String getSchemaName()
    {
        return schemaName;
    }

    @JsonProperty
    public String getTableName()
    {
        return tableName;
    }

    @JsonProperty
    public GlutenTupleDomain<GlutenSubfield> getDomainPredicate()
    {
        return domainPredicate;
    }

    @JsonProperty
    public Optional<List<List<String>>> getAnalyzePartitionValues()
    {
        return analyzePartitionValues;
    }
}
