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
import com.google.common.collect.ImmutableMap;
import io.trino.plugin.hive.metastore.StorageFormat;

import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Properties;

import static com.google.common.base.MoreObjects.toStringHelper;
import static io.trino.hive.thrift.metastore.hive_metastoreConstants.FILE_INPUT_FORMAT;
import static io.trino.hive.thrift.metastore.hive_metastoreConstants.FILE_OUTPUT_FORMAT;
import static io.trino.hive.thrift.metastore.hive_metastoreConstants.META_TABLE_LOCATION;
import static io.trino.plugin.hive.util.SerdeConstants.SERIALIZATION_LIB;
import static java.util.Objects.requireNonNull;

public class GlutenStorage
{
    private final StorageFormat storageFormat;
    private final String location;
    private final Optional<GlutenHiveBucketProperty> bucketProperty;
    private final boolean skewed;
    private final Map<String, String> serdeParameters;
    private final Map<String, String> parameters;

    @JsonCreator
    public GlutenStorage(
            @JsonProperty("storageFormat") StorageFormat storageFormat,
            @JsonProperty("location") String location,
            @JsonProperty("bucketProperty") Optional<GlutenHiveBucketProperty> bucketProperty,
            @JsonProperty("skewed") boolean skewed,
            @JsonProperty("serdeParameters") Map<String, String> serdeParameters,
            @JsonProperty("parameters") Map<String, String> parameters)
    {
        this.storageFormat = requireNonNull(storageFormat, "storageFormat is null");
        this.location = requireNonNull(location, "location is null");
        this.bucketProperty = requireNonNull(bucketProperty, "bucketProperty is null");
        this.skewed = skewed;
        this.serdeParameters = ImmutableMap.copyOf(requireNonNull(serdeParameters, "serdeParameters is null"));
        this.parameters = ImmutableMap.copyOf(requireNonNull(parameters, "parameters is null"));
    }

    public static GlutenStorage fromSchema(Properties schema)
    {
        String serde = schema.getProperty(SERIALIZATION_LIB);
        String inputFormat = schema.getProperty(FILE_INPUT_FORMAT);
        String outputFormat = schema.getProperty(FILE_OUTPUT_FORMAT);
        StorageFormat storageFormat = StorageFormat.create(serde, inputFormat, outputFormat);

        String location = schema.getProperty(META_TABLE_LOCATION);

        //GlutenHiveBucketProperty glutenHiveBucketProperty = GlutenHiveBucketProperty.fromSchema(schema);

        // TODO: complete message?
        boolean skewed = false;
        Map<String, String> serdeParameters = ImmutableMap.of();
        Map<String, String> parameters = ImmutableMap.of();

        return new GlutenStorage(storageFormat, location, Optional.empty(), skewed, serdeParameters, parameters);
    }

    @JsonProperty
    public StorageFormat getStorageFormat()
    {
        return storageFormat;
    }

    @JsonProperty
    public String getLocation()
    {
        return location;
    }

    @JsonProperty
    public Optional<GlutenHiveBucketProperty> getBucketProperty()
    {
        return bucketProperty;
    }

    @JsonProperty
    public boolean isSkewed()
    {
        return skewed;
    }

    @JsonProperty
    public Map<String, String> getSerdeParameters()
    {
        return serdeParameters;
    }

    @JsonProperty
    public Map<String, String> getParameters()
    {
        return parameters;
    }

    @Override
    public String toString()
    {
        return toStringHelper(this)
                .add("skewed", skewed)
                .add("storageFormat", storageFormat)
                .add("location", location)
                .add("bucketProperty", bucketProperty)
                .add("serdeParameters", serdeParameters)
                .add("parameters", parameters)
                .toString();
    }

    @Override
    public boolean equals(Object o)
    {
        if (this == o) {
            return true;
        }
        if (o == null || getClass() != o.getClass()) {
            return false;
        }

        GlutenStorage storage = (GlutenStorage) o;
        return skewed == storage.skewed &&
                Objects.equals(storageFormat, storage.storageFormat) &&
                Objects.equals(location, storage.location) &&
                Objects.equals(bucketProperty, storage.bucketProperty) &&
                Objects.equals(serdeParameters, storage.serdeParameters) &&
                Objects.equals(parameters, storage.parameters);
    }

    @Override
    public int hashCode()
    {
        return Objects.hash(skewed, storageFormat, location, bucketProperty, serdeParameters, parameters);
    }
}
