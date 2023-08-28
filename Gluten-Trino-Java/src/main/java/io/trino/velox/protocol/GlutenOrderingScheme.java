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
package io.trino.velox.protocol;

import com.fasterxml.jackson.annotation.JsonCreator;
import com.fasterxml.jackson.annotation.JsonProperty;
import io.trino.spi.connector.SortOrder;

import java.util.List;

import static com.google.common.collect.ImmutableList.toImmutableList;

public class GlutenOrderingScheme
{
    private final List<Ordering> orderBy;

    @JsonCreator
    public GlutenOrderingScheme(@JsonProperty("orderBy") List<Ordering> orderBy)
    {
        this.orderBy = orderBy;
    }

    @JsonProperty
    public List<Ordering> getOrderBy()
    {
        return orderBy;
    }

    public List<GlutenVariableReferenceExpression> getOrderByVariables()
    {
        return orderBy.stream().map(Ordering::getVariable).collect(toImmutableList());
    }

    public static class Ordering
    {
        private final GlutenVariableReferenceExpression variable;
        private final SortOrder sortOrder;

        @JsonCreator
        public Ordering(@JsonProperty("variable") GlutenVariableReferenceExpression variable,
                @JsonProperty("sortOrder") SortOrder sortOrder)
        {
            this.variable = variable;
            this.sortOrder = sortOrder;
        }

        @JsonProperty
        public GlutenVariableReferenceExpression getVariable()
        {
            return variable;
        }

        @JsonProperty
        public SortOrder getSortOrder()
        {
            return sortOrder;
        }
    }
}
