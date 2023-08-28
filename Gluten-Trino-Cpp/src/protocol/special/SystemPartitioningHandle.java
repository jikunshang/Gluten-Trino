public final class SystemPartitioningHandle
        implements ConnectorPartitioningHandle
{
    public enum SystemPartitioning
    {
        SINGLE,
        FIXED,
        SOURCE,
        COORDINATOR_ONLY,
        ARBITRARY
    }

    public static final PartitioningHandle SINGLE_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.SINGLE, SystemPartitionFunction.SINGLE);
    public static final PartitioningHandle COORDINATOR_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.COORDINATOR_ONLY, SystemPartitionFunction.SINGLE);
    public static final PartitioningHandle FIXED_HASH_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.FIXED, SystemPartitionFunction.HASH);
    public static final PartitioningHandle FIXED_ARBITRARY_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.FIXED, SystemPartitionFunction.ROUND_ROBIN);
    public static final PartitioningHandle FIXED_BROADCAST_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.FIXED, SystemPartitionFunction.BROADCAST);
    public static final PartitioningHandle SCALED_WRITER_ROUND_ROBIN_DISTRIBUTION = createScaledWriterSystemPartitioning(SystemPartitionFunction.ROUND_ROBIN);
    public static final PartitioningHandle SCALED_WRITER_HASH_DISTRIBUTION = createScaledWriterSystemPartitioning(SystemPartitionFunction.HASH);
    public static final PartitioningHandle SOURCE_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.SOURCE, SystemPartitionFunction.UNKNOWN);
    public static final PartitioningHandle ARBITRARY_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.ARBITRARY, SystemPartitionFunction.UNKNOWN);
    public static final PartitioningHandle FIXED_PASSTHROUGH_DISTRIBUTION = createSystemPartitioning(SystemPartitioning.FIXED, SystemPartitionFunction.UNKNOWN);

    private static PartitioningHandle createSystemPartitioning(SystemPartitioning partitioning, SystemPartitionFunction function)
    {
        return new PartitioningHandle(Optional.empty(), Optional.empty(), new SystemPartitioningHandle(partitioning, function));
    }

    private static PartitioningHandle createScaledWriterSystemPartitioning(SystemPartitionFunction function)
    {
        return new PartitioningHandle(Optional.empty(), Optional.empty(), new SystemPartitioningHandle(SystemPartitioning.ARBITRARY, function), true);
    }

    private final SystemPartitioning partitioning;
    private final SystemPartitionFunction function;

    @JsonCreator
    public SystemPartitioningHandle(
            @JsonProperty("partitioning") SystemPartitioning partitioning,
            @JsonProperty("function") SystemPartitionFunction function)
    {
        this.partitioning = requireNonNull(partitioning, "partitioning is null");
        this.function = requireNonNull(function, "function is null");
    }

    @JsonProperty
    public SystemPartitioning getPartitioning()
    {
        return partitioning;
    }

    @JsonProperty
    public SystemPartitionFunction getFunction()
    {
        return function;
    }

    public String getPartitioningName()
    {
        return partitioning.name();
    }

    @Override
    public boolean isSingleNode()
    {
        return partitioning == SystemPartitioning.COORDINATOR_ONLY || partitioning == SystemPartitioning.SINGLE;
    }

    @Override
    public boolean isCoordinatorOnly()
    {
        return partitioning == SystemPartitioning.COORDINATOR_ONLY;
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
        SystemPartitioningHandle that = (SystemPartitioningHandle) o;
        return partitioning == that.partitioning &&
                function == that.function;
    }

    @Override
    public int hashCode()
    {
        return Objects.hash(partitioning, function);
    }

    @Override
    public String toString()
    {
        if (partitioning == SystemPartitioning.FIXED || partitioning == SystemPartitioning.ARBITRARY) {
            return function.toString();
        }
        return partitioning.toString();
    }

    public PartitionFunction getPartitionFunction(List<Type> partitionChannelTypes, boolean isHashPrecomputed, int[] bucketToPartition, BlockTypeOperators blockTypeOperators)
    {
        requireNonNull(partitionChannelTypes, "partitionChannelTypes is null");
        requireNonNull(bucketToPartition, "bucketToPartition is null");

        BucketFunction bucketFunction = function.createBucketFunction(partitionChannelTypes, isHashPrecomputed, bucketToPartition.length, blockTypeOperators);
        return new BucketPartitionFunction(bucketFunction, bucketToPartition);
    }

    public enum SystemPartitionFunction
    {
        SINGLE,
        HASH,
        ROUND_ROBIN,
        BROADCAST,
        UNKNOWN, {
            @Override
            public BucketFunction createBucketFunction(List<Type> partitionChannelTypes, boolean isHashPrecomputed, int bucketCount, BlockTypeOperators blockTypeOperators)
            {
                throw new UnsupportedOperationException();
            }
        },
        UNKNOWN;

        public abstract BucketFunction createBucketFunction(List<Type> partitionChannelTypes,
                boolean isHashPrecomputed,
                int bucketCount,
                BlockTypeOperators blockTypeOperators);

        private static class SingleBucketFunction
                implements BucketFunction
        {
            @Override
            public int getBucket(Page page, int position)
            {
                return 0;
            }
        }

        public static class RoundRobinBucketFunction
                implements BucketFunction
        {
            private final int bucketCount;
            private int counter;

            public RoundRobinBucketFunction(int bucketCount)
            {
                checkArgument(bucketCount > 0, "bucketCount must be at least 1");
                this.bucketCount = bucketCount;
            }

            @Override
            public int getBucket(Page page, int position)
            {
                int bucket = counter % bucketCount;
                counter = (counter + 1) & 0x7fff_ffff;
                return bucket;
            }

            @Override
            public String toString()
            {
                return toStringHelper(this)
                        .add("bucketCount", bucketCount)
                        .toString();
            }
        }
    }
}
