#include "src/trino_bridge.h"

#include <fmt/core.h>
#include <jni.h>
#include <boost/stacktrace.hpp>

#include "glog/logging.h"
#include "protocol/trino_protocol.h"
#include "src/utils/Configs.h"
#include "types/PrestoToVeloxQueryPlan.h"
#include "types/PrestoToVeloxSplit.h"
#include "types/TrinoTaskId.h"
#include "utils.h"
#include "utils/JniUtils.h"
#include "velox/common/base/Exceptions.h"
#include "velox/common/memory/Memory.h"
#include "velox/common/memory/MemoryPool.h"
#include "velox/connectors/Connector.h"
#include "velox/core/PlanFragment.h"
#include "velox/core/QueryConfig.h"
#include "velox/exec/Driver.h"
#include "velox/exec/PartitionedOutputBufferManager.h"
#include "velox/exec/Task.h"
#include "velox/exec/tests/utils/PlanBuilder.h"
#include "velox/type/Type.h"

#include "NativeConfigs.h"

using namespace facebook;

using namespace io::trino;
using namespace io::trino::bridge;

DECLARE_bool(velox_exception_user_stacktrace_enabled);
DECLARE_bool(velox_memory_leak_check_enabled);

static std::shared_ptr<velox::memory::MemoryPool> pool_ = nullptr;

namespace {
using namespace velox;
using namespace io::trino;

extern "C" void __cxa_pure_virtual() {
  VLOG(google::ERROR) << "Exception: Pure-virtual function called. \n"
                      << boost::stacktrace::stacktrace();
}

class PartitionOutputData {
 public:
  PartitionOutputData() : sequence_(0), noMore_(false), listenerRegistered_(false) {}

  size_t getSequence() const { return sequence_; }

  bool getListenerRegistered() const { return listenerRegistered_; }

  void registerListener() { listenerRegistered_ = true; }

  void consumeListener() { listenerRegistered_ = false; }

  bool noMoreData() const { return noMore_; }

  size_t getOutputDataNum() const { return noMore_ ? data_.size() - 1 : data_.size(); }

  size_t getDataSize(size_t index) const {
    VELOX_CHECK_LT(index, data_.size());
    return data_[index]->computeChainDataLength();
  }

  std::vector<std::unique_ptr<folly::IOBuf>> popWithLock(size_t num) {
    return withLock([this](size_t num) { return pop(num); }, num);
  }

  std::vector<std::unique_ptr<folly::IOBuf>> pop(size_t num) {
    num = std::min(num, getOutputDataNum());
    std::vector<std::unique_ptr<folly::IOBuf>> out(num);

    for (size_t i = 0; i < num; ++i) {
      out[i] = std::move(data_[i]);
    }

    data_.erase(data_.begin(), data_.begin() + num);

    return out;
  }

  void enqueueWithLock(size_t seq, std::vector<std::unique_ptr<folly::IOBuf>>& data) {
    withLock([this, &seq, &data]() mutable { enqueue(seq, data); });
  }

  void enqueue(size_t seq, std::vector<std::unique_ptr<folly::IOBuf>>& data) {
    size_t current_seq = getSequence();
    VELOX_CHECK_LE(seq, current_seq, "Unsupport skipping output data.");
    size_t seq_diff = current_seq - seq;
    if (seq_diff >= data.size()) {
      return;
    }
    // Note: PartitionOutput operator will enqueue the serialized pages into the
    // corresponding PartitionOutputBuffer and DestinitionBuffer, when the noMoreSplit is
    // set and all drivers are finished, noMoreData() will be called and enqueue a nullptr
    // to all page sequences.
    for (; seq_diff < data.size(); ++seq_diff) {
      auto ptr = data[seq_diff].release();
      data_.emplace_back(ptr);
      ++sequence_;
      if (!ptr) {
        noMore_ = true;
      }
    }
  }

  template <typename F, typename... Args>
  std::invoke_result_t<F, Args&&...> withLock(F&& func, Args&&... args) const {
    {
      std::lock_guard<std::recursive_mutex> lg(lock_);
      return func(args...);
    }
  }

 private:
  mutable std::recursive_mutex lock_;
  size_t sequence_;
  std::vector<std::unique_ptr<folly::IOBuf>> data_;
  bool noMore_;
  bool listenerRegistered_;
};

struct TaskHandle;

using TaskHandlePtr = boost::intrusive_ptr<TaskHandle>;

struct TaskHandle {
  using TaskPtr = std::shared_ptr<velox::exec::Task>;

  static TaskHandlePtr createTaskHandle(const TrinoTaskId& id, TaskPtr task_ptr,
                                        size_t numPartitions = 1,
                                        bool broadcast = false) {
    return TaskHandlePtr(
        new TaskHandle(id, std::move(task_ptr), numPartitions, broadcast));
  }

  TrinoTaskId taskId;
  TaskPtr task;
  std::vector<std::unique_ptr<PartitionOutputData>> outputs;
  bool isBroadcast;

 private:
  TaskHandle(const TrinoTaskId& id, TaskPtr task_ptr, size_t numPartitions,
             bool broadcast)
      : taskId(id),
        task(std::move(task_ptr)),
        outputs(numPartitions),
        isBroadcast(broadcast),
        ref_count_(0) {
    for (size_t partitionId = 0; partitionId < numPartitions; ++partitionId) {
      outputs[partitionId] = std::make_unique<PartitionOutputData>();
    }
  }

  friend void intrusive_ptr_add_ref(TaskHandle*);
  friend void intrusive_ptr_release(TaskHandle*);

  void addRef() { ref_count_.fetch_add(1); }

  void release() {
    if (ref_count_.fetch_sub(1) == 1) {
      delete this;
    }
  }

  std::atomic<int32_t> ref_count_;
};

void intrusive_ptr_add_ref(TaskHandle* handle) { handle->addRef(); }

void intrusive_ptr_release(TaskHandle* handle) { handle->release(); }

class JniHandle {
 public:
  explicit JniHandle(const NativeConfigsPtr& nativeConfigs,
                     const NativeSqlTaskExecutionManagerPtr& javaManager)
      : convertor_pool_(velox::memory::addDefaultLeafMemoryPool()),
        nativeConfigs_(nativeConfigs),
        javaManager_(javaManager) {
    driverExecutor_ = getDriverCPUExecutor(nativeConfigs_->getMaxWorkerThreads());
    exchangeIOExecutor_ =
        getExchangeIOCPUExecutor(nativeConfigs_->getExchangeClientThreads());
  }

  TaskHandlePtr createTaskHandle(const TrinoTaskId& id,
                                 const protocol::PlanFragment& plan) {
    return withWLock([&id, &plan, this]() {
      if (auto iter = taskMap_.find(id.fullId()); iter != taskMap_.end()) {
        return iter->second.get();
      }
      size_t numPartitions = 0;
      if (plan.partitioningScheme.bucketToPartition) {
        numPartitions =
            *std::max_element(plan.partitioningScheme.bucketToPartition->begin(),
                              plan.partitioningScheme.bucketToPartition->end()) +
            1;
      } else {
        VLOG(google::ERROR) << fmt::format("No partition buffer number in task {}.",
                                           id.fullId());
      }

      bool isBroadcast = false;
      if (auto handle =
              std::dynamic_pointer_cast<io::trino::protocol::SystemPartitioningHandle>(
                  plan.partitioningScheme.partitioning.handle.connectorHandle)) {
        if (handle->function == io::trino::protocol::SystemPartitionFunction::BROADCAST) {
          VLOG(google::INFO) << fmt::format("Task {} contains broadcast output buffer.",
                                            id.fullId());
          isBroadcast = true;
        }
      }

      VLOG(google::INFO) << fmt::format("Task {} contains {} output buffer.", id.fullId(),
                                        numPartitions);

      auto queryCtx = std::make_shared<core::QueryCtx>(
          driverExecutor_.get(), std::move(nativeConfigs_->getQueryConfigs()),
          std::move(nativeConfigs_->getConnectorConfigs()),
          memory::MemoryAllocator::getInstance(),
          memory::defaultMemoryManager().addRootPool(
              id.fullId(), nativeConfigs_->getQueryMaxMemoryPerNode()));

      VeloxInteractiveQueryPlanConverter convertor(convertor_pool_.get());
      core::PlanFragment fragment =
          convertor.toVeloxQueryPlan(plan, nullptr, id.fullId());

      VLOG(google::INFO) << fmt::format("Task {},\n PlanFragment: {}", id.fullId(),
                                        fragment.planNode->toString(true, true));

      auto task = exec::Task::create(id.fullId(), std::move(fragment), id.id(), queryCtx);
      auto iter =
          taskMap_.insert({id.fullId(), TaskHandle::createTaskHandle(
                                            id, task, numPartitions, isBroadcast)});
      return iter.first->second.get();
    });
  }

  TaskHandlePtr getTaskHandle(const TrinoTaskId& id) {
    return withRLock([&id, this]() -> TaskHandle* {
      if (auto iter = taskMap_.find(id.fullId()); iter != taskMap_.end()) {
        return iter->second.get();
      } else {
        return nullptr;
      }
    });
  }

  bool removeTask(const TrinoTaskId& id) {
    return withWLock([this, &id]() {
      if (auto taskIter = taskMap_.find(id.fullId()); taskIter != taskMap_.end()) {
        auto&& task = taskIter->second->task;

        printTaskStatus(id, task);

        taskMap_.erase(taskIter);
        return true;
      } else {
        return false;
      }
    });
  }

  NativeConfigsPtr getConfig() { return nativeConfigs_; }

  NativeSqlTaskExecutionManager* getNativeSqlTaskExecutionManager() {
    return javaManager_.get();
  }

 private:
  void printTaskStatus(const TrinoTaskId& id, const std::shared_ptr<exec::Task>& task) {
    std::stringstream ss;
    ss << fmt::format("Task {} status:\n", id.fullId());

    auto&& taskStatus = task->taskStats();
    ss << fmt::format("\tSplitProcessingTime: {} ms, TaskExecutionTime: {} ms\n",
                      taskStatus.executionEndTimeMs - taskStatus.firstSplitStartTimeMs,
                      taskStatus.endTimeMs - taskStatus.executionStartTimeMs)
       << fmt::format("\tSplits: {}, Drivers: {}\n", taskStatus.numTotalSplits,
                      taskStatus.numTotalDrivers);
    ss << "\tPipeline status:\n";

    for (size_t pipelineId = 0; pipelineId < taskStatus.pipelineStats.size();
         ++pipelineId) {
      auto&& pipelineStatus = taskStatus.pipelineStats[pipelineId];
      ss << fmt::format("\t\tPipeline {}: {} {}\n", pipelineId,
                        pipelineStatus.inputPipeline ? "input" : "",
                        pipelineStatus.outputPipeline ? "output" : "");
      for (size_t opId = 0; opId < pipelineStatus.operatorStats.size(); ++opId) {
        auto&& opStatus = pipelineStatus.operatorStats[opId];
        ss << fmt::format("\t\t\tOp {}, {}: ", opStatus.operatorId, opStatus.operatorType)
           << "\n";

        if (opId == 0 && pipelineStatus.inputPipeline) {
          ss << fmt::format("\t\t\t\tRaw Input: {} rows, {} bytes\n",
                            opStatus.rawInputPositions, opStatus.rawInputBytes);
        }

        ss << fmt::format("\t\t\t\tInput: {} vectors, {} rows, {} bytes\n",
                          opStatus.inputVectors, opStatus.inputPositions,
                          opStatus.inputBytes);

        ss << fmt::format("\t\t\t\tOutput: {} vectors, {} rows, {} bytes\n",
                          opStatus.outputVectors, opStatus.outputPositions,
                          opStatus.outputBytes);

        for (auto&& metric : opStatus.runtimeStats) {
          ss << "\t\t\t\t" << metric.first << ":";
          metric.second.printMetric(ss);
          ss << "\n";
        }
      }
    }
    VLOG(google::INFO) << ss.str();
  }

  template <typename F, typename... Args>
  std::invoke_result_t<F, Args&&...> withWLock(F&& func, Args&&... args) const {
    taskMapLock_.lock();
    auto guard = folly::makeGuard([this]() { taskMapLock_.unlock(); });
    return func(args...);
  }

  template <typename F, typename... Args>
  std::invoke_result_t<F, Args&&...> withRLock(F&& func, Args&&... args) const {
    taskMapLock_.lock_shared();
    auto guard = folly::makeGuard([this]() { taskMapLock_.unlock_shared(); });
    return func(args...);
  }

 private:
  // Variables below must be singleton under process.
  std::shared_ptr<memory::MemoryPool> convertor_pool_;
  mutable std::shared_mutex taskMapLock_;
  NativeConfigsPtr nativeConfigs_;
  NativeSqlTaskExecutionManagerPtr javaManager_;
  std::unordered_map<std::string, TaskHandlePtr> taskMap_;
  std::shared_ptr<folly::CPUThreadPoolExecutor> driverExecutor_;
  std::shared_ptr<folly::IOThreadPoolExecutor> exchangeIOExecutor_;
};

template <typename F, typename... Args>
void tryLogException(F&& func, Args&&... args) {
  using tuple_type = std::tuple<std::decay_t<Args>...>;
  tuple_type t{std::forward<Args>(args)...};
  try {
    std::apply(std::forward<F>(func), t);
  } catch (const std::exception& e) {
    JniUtils::logError(JniUtils::getJNIEnv(), __FILE__, __LINE__, e.what());
    JniUtils::throwJavaRuntimeException(JniUtils::getJNIEnv(), e.what());
  }
}

template <typename F, typename T, typename... Args>
T tryLogExceptionWithReturnValue(F&& func, const T& returnValueOnError, Args&&... args) {
  using tuple_type = std::tuple<std::decay_t<Args>...>;
  tuple_type t{std::forward<Args>(args)...};
  try {
    return std::apply(std::forward<F>(func), t);
  } catch (const std::exception& e) {
    JniUtils::logError(JniUtils::getJNIEnv(), __FILE__, __LINE__, e.what());
    JniUtils::throwJavaRuntimeException(JniUtils::getJNIEnv(), e.what());
  }
  return returnValueOnError;
}

};  // namespace

std::shared_ptr<velox::Config> getConnectorConfig() {
  // std::unordered_map<std::string, std::string> configs = {};
  std::shared_ptr<velox::connector::Connector> connector =
      velox::connector::getConnector(kHiveConnectorId);
  // auto got = confMap_.find(kCaseSensitive);
  // if (got != confMap_.end())
  // {
  //   configs[velox::connector::hive::HiveConfig::kCaseSensitive] = got->second;
  // }
  return std::make_shared<velox::core::MemConfig>(
      connector->connectorProperties()->values());
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
  JNIEnv* env;
  if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION) != JNI_OK) {
    return JNI_ERR;
  }
  GLOBAL_JAVA_VM = vm;

  io::trino::bridge::Unsafe::instance().initialize(env);
  FLAGS_velox_exception_user_stacktrace_enabled = true;
  // See folly CPUThreadPoolExecutor.h
  // It's better to close dynamic cpu thread pool executor since the thread pool in Java
  // side is increased only if it needs more but never get decreased.
  FLAGS_dynamic_cputhreadpoolexecutor = false;

  return JNI_VERSION;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM* vm, void* reserved) {
  GLOBAL_JAVA_VM = nullptr;
}

// return value:
// 0 : create successful
// 1 : task already exists
// others : failed
JNIEXPORT jlong JNICALL Java_io_trino_jni_TrinoBridge_createTask(JNIEnv* env, jobject obj,
                                                                 jlong handlePtr,
                                                                 jstring jTaskId,
                                                                 jstring jPlanFragment) {
  return tryLogExceptionWithReturnValue(
      [env, handlePtr, jTaskId, jPlanFragment]() {
        io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));

        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        if (!handle) {
          JniUtils::throwJavaRuntimeException(env, "Empty JniHandle!!!");
          return -1;
        }
        auto config = handle->getConfig();

        uint32_t concurrentLifespans = 10;

        std::string planFragment(JniUtils::jstringToString(env, jPlanFragment));
        JniUtils::logDebug(
            env, __FILE__, __LINE__,
            "Task " + taskId.fullId() + " gets PlanFragment Json: " + planFragment);
        nlohmann::json json = nlohmann::json::parse(planFragment);
        std::shared_ptr<io::trino::protocol::PlanFragment> mockPlanFragment;
        from_json(json, mockPlanFragment);
        if (!mockPlanFragment) {
          JniUtils::throwJavaRuntimeException(env, "mockPlanFragment is null.");
          return -1;
        }

        TaskHandlePtr taskHandle = handle->createTaskHandle(taskId, *mockPlanFragment);

        taskHandle->task->stateChangeFuture(0)
            .via(getDriverCPUExecutor().get())
            .thenValue([handle, taskId](folly::Unit unit) {
              handle->getNativeSqlTaskExecutionManager()->requestUpdateNativeTaskStatus(
                  taskId);
            });

        int32_t maxDriverPerTask = std::max(
            1, std::min(config->getMaxDriversPerTask(), config->getTaskConcurrency()));
        velox::exec::Task::start(taskHandle->task, maxDriverPerTask, concurrentLifespans);
        JniUtils::logDebug(env, __FILE__, __LINE__,
                           "Task " + taskId.fullId() + " is started, maxDriverPerTask=" +
                               std::to_string(maxDriverPerTask));

        return 0;
      },
      -1);
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_removeTask(JNIEnv* env, jobject obj,
                                                                jlong handlePtr,
                                                                jstring jTaskId) {
  JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
  io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
  handle->removeTask(taskId);
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_addSplits(JNIEnv* env, jobject obj,
                                                               jlong handlePtr,
                                                               jstring jTaskId,
                                                               jstring jSplitInfo) {
  return tryLogException([env, handlePtr, jTaskId, jSplitInfo]() {
    io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
    JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
    TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
    if (taskHandle) {
      std::shared_ptr<velox::exec::Task> task = taskHandle->task;
      std::string splitInfo = JniUtils::jstringToString(env, jSplitInfo);
      nlohmann::json jsonSplits = nlohmann::json::parse(splitInfo);
      std::shared_ptr<io::trino::protocol::SplitAssignmentsMessage> splitsPtr;
      from_json(jsonSplits, splitsPtr);

      for (auto&& splitAssignment : splitsPtr->splitAssignments) {
        long maxSplitSequenceId = -1;
        for (auto& split : splitAssignment.splits) {
          velox::exec::Split veloxSplit = io::trino::toVeloxSplit(split);
          if (veloxSplit.hasConnectorSplit()) {
            maxSplitSequenceId = std::max(maxSplitSequenceId, split.sequenceId);
            task->addSplitWithSequence(split.planNodeId, std::move(veloxSplit),
                                       split.sequenceId);
          }
        }
        task->setMaxSplitSequenceId(splitAssignment.planNodeId, maxSplitSequenceId);

        if (splitAssignment.noMoreSplits) {
          task->noMoreSplits(splitAssignment.planNodeId);
        }
      }
    } else {
      std::cerr << "Not found task id " << taskId.fullId() << " when call addSplits."
                << std::endl;
    }
  });
}

JNIEXPORT jlong JNICALL Java_io_trino_jni_TrinoBridge_init(JNIEnv* env, jobject obj,
                                                           jstring configJson,
                                                           jobject manager) {
  // init JniHandle and return to Java
  static std::vector<std::unique_ptr<JniHandle>> jniHandleHolder;
  FLAGS_velox_memory_leak_check_enabled = true;

  auto config =
      std::make_shared<NativeConfigs>(JniUtils::jstringToString(env, configJson));

  for (auto& [moduleName, level] : config->getLogVerboseModules()) {
    google::SetVLOGLevel(moduleName.c_str(), level);
  }

  JniHandle* handle = new JniHandle(
      std::move(config), std::make_shared<NativeSqlTaskExecutionManager>(manager));
  jniHandleHolder.emplace_back(handle);

  static auto veloxInitializer = std::make_shared<VeloxInitializer>();

  return reinterpret_cast<int64_t>(handle);
}

JNIEXPORT jlong JNICALL Java_io_trino_jni_TrinoBridge_close(JNIEnv* env, jobject obj,
                                                            jlong handlePtr) {
  return tryLogExceptionWithReturnValue(
      [handlePtr]() {
        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        delete handle;
        return 0;
      },
      -1);
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_noMoreBuffers(
    JNIEnv* env, jobject obj, jlong handlePtr, jstring jTaskId, jint jNumPartitions) {
  tryLogException([env, handlePtr, jTaskId, jNumPartitions]() -> void {
    io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
    JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
    if (!handle) {
      JniUtils::throwJavaRuntimeException(env, "Empty handle!!!");
      return;
    }
    TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
    if (!taskHandle || !taskHandle->task) {
      JniUtils::throwJavaRuntimeException(
          env, "Task " + taskId.fullId() + " has already finished.");
      return;
    }
    if (taskHandle->isBroadcast) {
      for (int destination = 0; destination < jNumPartitions; ++destination) {
        taskHandle->task->updateBroadcastOutputBuffers(destination, true);
      }
    }
  });
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_registerOutputPartitionListener(
    JNIEnv* env, jobject obj, jlong handlePtr, jstring jTaskId, jint jPartitionId,
    jlong jSequence, jlong maxBytes) {
  tryLogException([env, handlePtr, jTaskId, jPartitionId, jSequence, maxBytes]() {
    io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
    JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
    TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
    if (!handle) {
      JniUtils::logError(env, __FILE__, __LINE__, "Empty handle!!!");
      return;
    }
    if (taskHandle) {
      auto manager = velox::exec::PartitionedOutputBufferManager::getInstance().lock();
      int destination = jPartitionId;

      VELOX_CHECK(!taskHandle->outputs[destination]->getListenerRegistered())
      taskHandle->outputs[destination]->registerListener();
      bool exist = manager->getData(
          taskId.fullId(), destination, maxBytes, jSequence,
          [taskHandle, handle, destination](
              std::vector<std::unique_ptr<folly::IOBuf>> pages,
              int64_t sequence) mutable {
            taskHandle->outputs[destination]->enqueueWithLock(sequence, pages);
            taskHandle->outputs[destination]->consumeListener();
            handle->getNativeSqlTaskExecutionManager()->requestFetchNativeOutput(
                taskHandle->taskId, destination);
          });

      if (!exist) {
        // For the case that the result in the buffer manager is removed, but java side is
        // not notified.
        handle->getNativeSqlTaskExecutionManager()->requestFetchNativeOutput(
            taskHandle->taskId, destination);
      }
    }
  });
}

JNIEXPORT jint JNICALL Java_io_trino_jni_TrinoBridge_getBufferStep1(
    JNIEnv* env, jobject obj, jlong handlePtr, jstring jTaskId, jint jPartitionId) {
  return tryLogExceptionWithReturnValue(
      [env, handlePtr, jTaskId, jPartitionId]() {
        io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
        if (!handle) {
          JniUtils::logWarning(env, __FILE__, __LINE__, "Empty handle!!!");
          return 0;
        }
        if (taskHandle) {
          int destination = jPartitionId;
          size_t data_num = taskHandle->outputs[destination]->withLock(
              [&taskId, &destination, &taskHandle](PartitionOutputData& data) {
                if (data.noMoreData() && data.getOutputDataNum() == 0) {
                  auto manager =
                      velox::exec::PartitionedOutputBufferManager::getInstance().lock();
                  manager->deleteResults(taskId.fullId(), destination);
                }
                return data.getOutputDataNum();
              },
              *taskHandle->outputs[destination]);

          return static_cast<jint>(data_num);
        } else {
          JniUtils::logError(env, __FILE__, __LINE__, "Task does not exist!");
          return 0;
        }
      },
      0);
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_getBufferStep2(
    JNIEnv* env, jobject obj, jlong handlePtr, jstring jTaskId, jint jPartitionId,
    jint results_num, jintArray jLengthArray) {
  return tryLogException(
      [env, handlePtr, jTaskId, jPartitionId, results_num, jLengthArray]() {
        io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
        size_t destination = jPartitionId;
        taskHandle->outputs[destination]->withLock(
            [env, &jLengthArray, results_num, destination, &taskHandle]() {
              for (auto index = 0; index < results_num; index++) {
                int32_t size = taskHandle->outputs[destination]->getDataSize(index);
                env->SetIntArrayRegion(jLengthArray, index, 1, &size);
              }
            });
        return;
      });
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_getBufferStep3(
    JNIEnv* env, jobject obj, jlong handlePtr, jstring jTaskId, jint jPartitionId,
    jint results_num, jlongArray jAddressArray) {
  tryLogException([env, handlePtr, jTaskId, jPartitionId, results_num, jAddressArray]() {
    io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
    JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
    TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);
    jboolean isCopy{false};
    auto* addressArray =
        reinterpret_cast<int64_t*>(env->GetLongArrayElements(jAddressArray, &isCopy));
    size_t destination = jPartitionId;

    auto pages = taskHandle->outputs[destination]->popWithLock(results_num);
    for (size_t index = 0; index < results_num; index++) {
      uint8_t* dst_addr = (uint8_t*)(addressArray[index]);
      const auto buf = pages[index].get();
      auto curBuf = buf;
      size_t start_offset = 0;
      do {
        std::memcpy(dst_addr + start_offset, curBuf->data(), curBuf->length());
        start_offset += curBuf->length();
        curBuf = curBuf->next();
      } while (buf != curBuf);
    }
  });
}

JNIEXPORT jstring JNICALL Java_io_trino_jni_TrinoBridge_getTaskStatus(JNIEnv* env,
                                                                      jobject obj,
                                                                      jlong handlePtr,
                                                                      jstring jTaskId) {
  return tryLogExceptionWithReturnValue(
      [env, handlePtr, jTaskId]() {
        io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);

        if (!taskHandle) {  // not found, return empty.
          io::trino::protocol::TaskStatus emptyTaskStatus;
          nlohmann::json j;
          io::trino::protocol::to_json(j, emptyTaskStatus);
          return env->NewStringUTF(j.dump().c_str());
        } else {
          std::shared_ptr<velox::exec::Task> task = taskHandle->task;
          io::trino::protocol::TaskStatus taskStatus =
              getTaskStatus(task, taskId.fullId());
          nlohmann::json j;
          io::trino::protocol::to_json(j, taskStatus);
          return env->NewStringUTF(j.dump().c_str());
        }
      },
      static_cast<jstring>(nullptr));
}

JNIEXPORT jstring JNICALL Java_io_trino_jni_TrinoBridge_getTaskInfo(JNIEnv* env,
                                                                    jobject obj,
                                                                    jlong handlePtr,
                                                                    jstring jTaskId) {
  return tryLogExceptionWithReturnValue(
      [env, handlePtr, jTaskId]() {
        io::trino::TrinoTaskId taskId(JniUtils::jstringToString(env, jTaskId));
        JniHandle* handle = reinterpret_cast<JniHandle*>(handlePtr);
        TaskHandlePtr taskHandle = handle->getTaskHandle(taskId);

        if (!taskHandle) {  // not found, return empty.
          io::trino::protocol::TaskInfo emptyTaskInfo;
          nlohmann::json j;
          io::trino::protocol::to_json(j, emptyTaskInfo);
          return env->NewStringUTF(j.dump().c_str());
        } else {
          std::shared_ptr<velox::exec::Task> task = taskHandle->task;
          io::trino::protocol::TaskInfo taskInfo = getTaskInfo(task, taskId.fullId());
          nlohmann::json j;
          io::trino::protocol::to_json(j, taskInfo);
          return env->NewStringUTF(j.dump().c_str());
        }
      },
      static_cast<jstring>(nullptr));
}

JNIEXPORT void JNICALL Java_io_trino_jni_TrinoBridge_registerConnector(
    JNIEnv* jniEnv, jobject cls, jstring jCatalogProperties) {
  return tryLogException([jniEnv, jCatalogProperties]() {
    const auto& registeredConnectors = velox::connector::getAllConnectors();

    std::string catalogProperties = JniUtils::jstringToString(jniEnv, jCatalogProperties);
    nlohmann::json j = nlohmann::json::parse(catalogProperties);
    std::shared_ptr<io::trino::protocol::CatalogProperties> catalogPropertiesPtr;
    from_json(j, catalogPropertiesPtr);
    std::string connectorName = catalogPropertiesPtr->connectorName;
    std::string catalogName = catalogPropertiesPtr->catalogName;
    std::map<std::string, std::string> propertyValues = catalogPropertiesPtr->properties;

    // Check if the connector is already registered, if not, register it.
    if (registeredConnectors.empty() || registeredConnectors.count(connectorName) == 0) {
      std::unordered_map<std::string, std::string> connectorConf(propertyValues.begin(),
                                                                 propertyValues.end());
      std::shared_ptr<const velox::Config> properties =
          std::make_shared<const velox::core::MemConfig>(std::move(connectorConf));
      std::shared_ptr<velox::connector::Connector> connector =
          velox::connector::getConnectorFactory(connectorName)
              ->newConnector(catalogName, std::move(properties));
      velox::connector::registerConnector(connector);
    }
  });
}
