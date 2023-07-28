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
package io.trino.plugin.tpch.protocol;

import com.fasterxml.jackson.annotation.JsonProperty;
import io.trino.spi.predicate.TupleDomain;
import io.trino.spi.protocol.MockColumnHandle;
import io.trino.spi.protocol.MockConnectorTableLayoutHandle;

public final class MockTpchTableLayoutHandle
        implements MockConnectorTableLayoutHandle
{
    private final MockTpchTableHandle table;
    private final TupleDomain<MockColumnHandle> predicate;

    public MockTpchTableLayoutHandle(MockTpchTableHandle table, TupleDomain<MockColumnHandle> predicate)
    {
        this.table = table;
        this.predicate = predicate;
    }

    @JsonProperty
    public MockTpchTableHandle getTable()
    {
        return table;
    }

    @JsonProperty
    public TupleDomain<MockColumnHandle> getPredicate()
    {
        return predicate;
    }
}