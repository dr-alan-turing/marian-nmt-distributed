#pragma once

#if MPI_FOUND
#include "mpi.h"
#endif

#include <future>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/thread/locks.hpp>
#include <boost/thread/shared_mutex.hpp>

#include "3rd_party/threadpool.h"
#include "common/definitions.h"
#include "data/batch_generator.h"
#include "optimizers/optimizers.h"
#include "training/dropper.h"
#include "training/scheduler.h"
#include "training/sparse_tensor.h"
#include "training/training.h"
#include "training/validator.h"
#include "training/graph_group.h"

namespace marian {

/**
 * @brief Multi-node graph group for asynchronous training over multiple machines each with one or multiple GPUs
 */
template <class Builder>
class MultiNodeAsyncGraphGroup : public GraphGroup {
public:
  typedef Builder builder_type;
  typedef typename Builder::dataset_type dataset_type;

  virtual void setScheduler(Ptr<Scheduler<dataset_type>> scheduler);

private:

  // Variables inherited from AsyncGraphGroup

  bool firstBatchProcessed_{false};

  std::vector<Ptr<Builder>> builders_;
  std::vector<Ptr<ExpressionGraph>> graphs_;
  std::vector<size_t> devices_;

  Ptr<Scheduler<dataset_type>> scheduler_;

  std::mutex mutexClientInit_;

  boost::shared_mutex schedulerMutex_;

  std::vector<Tensor> paramsAvg_;
  std::vector<Ptr<TensorAllocator>> paramsAllocAvg_;
  bool movingAvg_{false};
  float mvDecay_{0.9999};

  ThreadPool * pool_;

  std::vector<Ptr<TensorAllocator>> allocators_;

  size_t tau_{1};

  size_t batchIter_ = 0; // For dividing batches amongst nodes

  // MPI variables

  int mpi_my_rank_{0};
  int mpi_comm_world_size_{1};

  static const int MPI_TAG_GRAD_PUSH_{0};
  static const int MPI_TAG_GRAD_PUSH_SPARSE1_{1}, MPI_TAG_GRAD_PUSH_SPARSE2_{2}, MPI_TAG_GRAD_PUSH_SPARSE3_{3};
  static const int MPI_TAG_BATCH_WORDS_PUSH_{4};
  static const int MPI_TAG_PARAM_PUSH_{5};
  static const int MPI_TAG_PARAM_PUSH_SPARSE1_{6}, MPI_TAG_PARAM_PUSH_SPARSE2_{7}, MPI_TAG_PARAM_PUSH_SPARSE3_{8};

  // Server (shard) thread variables

  std::thread * serverShardThread_;

  std::vector<float> serverShardBuffer_;

  std::vector<Ptr<OptimizerBase>> gpuShardsOpts_;
  std::vector<Tensor> gpuShardsParams_;
  std::vector<Tensor> gpuShardsGrads_;

  std::vector<std::mutex> mutexGpuShards_;

  // Client communication variables

  std::vector<std::vector<float>> clientCommBufferParams_; // per client (GPU)
  std::vector<std::vector<float>> clientCommBufferGrads_;

  std::vector<size_t> nodeShardSizes_;
  std::vector<size_t> gpuShardSizes_;

  std::vector<size_t> multiNodeDevices_;

  static const unsigned int MSG_INFO_SIZE_{0}, MSG_INFO_CLIENT_{1}, MSG_INFO_BATCHWORDS_{2}, MSG_INFO_STATUS_{3};
  static const unsigned int STATUS_NODE_TRAINING_{0}, STATUS_NODE_FINISHED_{1};

  // Sparse communication variables

  double dropRate_;

  std::vector<int> serverShardSparseBuffer1_;
  std::vector<float> serverShardSparseBuffer2_;

  std::vector<std::vector<int>> clientShardSparseBuffer1_;
  std::vector<std::vector<float>> clientShardSparseBuffer2_;

  std::vector<int> numberClientsOfNodes_;
  std::vector<std::vector<size_t>> clientSizesOfNodes_;
  std::vector<std::vector<std::vector<Tensor>>> clientsParams_; // => clientsParams_[shard][node][client]

  std::vector<SparseTensor> localSparseGrads_;
  std::vector<SparseTensor> shardSparseGrads_;
  std::vector<SparseTensor> tmpSparseDeltas_;
  std::vector<SparseTensor> localSparseDeltas_;

  std::vector<std::vector<std::vector<GradientDrop>>> fetchDroppers_; // => fetchDroppers_[shard][node][client]
  std::vector<std::vector<GradientDrop>> gradientDroppers_; // => gradientDroppers_[gpu][node]
  std::vector<Tensor> tmpDeltas_;

  // Computations/communication overlap variables

  bool commOverlap_; // Overlapping computation during communication
  int maxNumberComputeIters_; // Max number of compute iterations that a node can do per synchronisation
  std::vector<size_t> numberComputeIters_; // Current number of compute iterations of each client since last synchronisation

  bool commOverlapSingleActive_; // Whether only one overlap thread can use communication channel at any time
  std::mutex mutexCommChannel_; // Mutex to limit communication channel to one overlapping thread (if commOverlapSingleActive_ == true)

  std::vector<std::thread*> clientCommThreads_;
  bool stopClientCommThreads_{false};

  std::vector<Tensor> commBufferParams_;
  std::vector<Tensor> commBufferGrads_;

  std::vector<Tensor> gpuSummedGrads_;
  std::vector<size_t> gpuSummedWordCounts_;
  std::vector<size_t> gpuCommittedWordCounts_;
  std::vector<Ptr<OptimizerBase>> localOpts_;

  std::vector<bool> commBuffersFilled_;
  std::vector<std::mutex> mutexCommBuffersFilled_;
  std::vector<std::condition_variable> cvCommBuffersFilled_;

  /**
   * @brief Allocate new tensor on given GPU and store allocator
   *
   * @param size Number of floats to allocate
   * @param device GPU
   * @return Allocated tensor
   */
  Tensor newTensor(int size, int device);

  /**
   * @brief Initialize graphs and variables for MPI, remote communicator, server shard, sparse communication
   * and overlapping compute/communicate, and launch server and client communication threads
   *
   * @param batch Batch to build initial graph with
   */
  void initFirstRun(Ptr<data::Batch> batch);

  /**
   * @brief Initialize variables relevant to MPI, i.e. size of cluster and rank of this node
   */
  void initMPI();

  /**
   * @brief Initialize server shard, i.e. sizes, parameters, gradients and buffers
   */
  void initServerShard();

  /**
   * @brief Initialize sparse variables for server shards, i.e. number and sizes of clients of every node, relevant sparse variables and send/receive buffers @TODO: Further clean-up
   */
  void initServerShardSparseVars();

  /**
   * @brief Get number of clients of every node by communicating with all nodes in cluster @TODO: Communication will not be necessary once run-time option is implemented
   */
  void setupClientsOfNodesAndDevices();

  /**
   * @brief Determine size for all clients of every node
   */
  void setupClientSizesOfNodes();

  /**
   * @brief Initialize client buffers for remote communication (synchronisation)
   */
  void initRemoteCommunicationVars();

  /*
   * @brief Launch independent thread which continually receives gradients assigned to this shard from any client, runs the shard optimizer and sends back the updated parameters
   * @TODO: Implement batch-flexible-lr in non-sparse by sending messageInfo through MPI with number of batch words
   */
  void launchServerShardThread();

  /**
   * @brief Send new gradients to the server shards and receive the updated (global) parameters
   *
   * @param newGrads Gradients to send
   * @param oldParams Parameters to replace
   * @param gpu GPU/client performing synchronize (to access appropriate buffers etc.)
   * @param batchWords Number of batch words to pass to server shard optimizers
   * @param optionalBlockMutex Optional mutex that has to be locked during synchronization
   */
  void synchronizeWithServerShards(Tensor newGrads, Tensor oldParams, int gpu, size_t batchWords = 0, std::mutex * optionalBlockMutex = nullptr);

  /*
   * @brief Launch independent thread which continually receives sparse gradients assigned to this shard from any client,
   * runs the shard optimizer and sends back sparse deltas given the updated parameters (sparse communication)
   */
  void launchSparseServerShardThread();

  /**
   * @brief Send new sparse gradients to the server shards, receive sparse deltas and apply these to the parameters to get the updated (global) parameters
   *
   * @param newGrads Gradients to send sparsely
   * @param oldParams Parameters to update with deltas
   * @param gpu GPU/client performing synchronize (to access appropriate buffers etc.)
   * @param batchWords Number of batch words to pass to server shard optimizers
   * @param optionalBlockMutex Optional mutex that has to be locked during synchronization
   */
  void sparseSynchronizeWithServerShards(Tensor newGrads, Tensor oldParams, int gpu, size_t batchWords = 0, std::mutex * optionalBlockMutex = nullptr);

  /**
   * @brief Launch independent threads which continually synchronize their client's gradients/parameters whenever the respective communication buffers are full
   */
  void launchCommOverlapThreads();

  /**
   * @brief Execute given batch on this node, pushing/pulling the resulting gradients/parameters to/from the server shards
   * or -- if comm. overlap enabled -- to/from the communication buffers, summing gradients locally if the communication thread is busy
   *
   * @param batch Batch on which to perform forward and backward passes
   */
  void execute(Ptr<data::Batch> batch);

  /**
   * @brief Notify server shards that this node has finished training
   */
  void signalFinishedToServerShards();

  /**
   * @brief Safely shut down the launched server shard thread
   */
  void shutDownServerShardThread();

  /**
   * @brief Safely shut down the launched communication overlap threads
   */
  void shutDownCommOverlapThreads();

public:

  /**
   * @brief (Constructor) Configure settings and initialize graphs, shard optimizers, local optimizers, graph builders, etc. and their associated variables
   */
  template <class... Args>
  MultiNodeAsyncGraphGroup(Ptr<Config> options, Args... args)
      : GraphGroup(options),
        multiNodeDevices_{options_->get<std::vector<size_t>>("multi-node-devices")},
        dropRate_{options_->get<double>("multi-node-drop-rate")},
        commOverlap_{options_->get<bool>("multi-node-overlap")},
        maxNumberComputeIters_{options_->get<int>("multi-node-max-compute")},
        commOverlapSingleActive_{options_->get<bool>("multi-node-single-comm")},
        movingAvg_{options_->get<bool>("moving-average")},
        mvDecay_{(float)options_->get<double>("moving-decay")},
        tau_{options_->get<size_t>("tau")} {
    initMPI();
    setupClientsOfNodesAndDevices();
    gpuSummedWordCounts_ = std::vector<size_t>(devices_.size(), 0);
    gpuCommittedWordCounts_ = std::vector<size_t>(devices_.size(), 0);
    commBuffersFilled_ = std::vector<bool>(devices_.size(), false);
    mutexCommBuffersFilled_ = std::vector<std::mutex>{devices_.size()};
    cvCommBuffersFilled_ = std::vector<std::condition_variable>(devices_.size());
    numberComputeIters_ = std::vector<size_t>(devices_.size(), 0);
    mutexGpuShards_ = std::vector<std::mutex>(devices_.size());
    pool_ = new marian::ThreadPool(devices_.size(), devices_.size());
    for(auto device : devices_) {
      auto graph = New<ExpressionGraph>();
      graph->setDevice(device);
      graph->reserveWorkspaceMB(options_->get<size_t>("workspace"));
      graphs_.push_back(graph);
      gpuShardsOpts_.push_back(Optimizer(options_));
      localOpts_.push_back(Optimizer(options_)); // => for simple SGD opt: localOpts_.push_back(Optimizer<Sgd>(0.0001, keywords::clip=Clipper<Norm>(1)));
      builders_.push_back(New<Builder>(options_, args...));
    }
  }

  /**
   * @brief (Destructor) Shut down server shard thread and (if comm. overlap enabled) communication overlap threads
   */
  ~MultiNodeAsyncGraphGroup() {
    LOG(info)->info("Shutting down MultiNodeAsyncGraphGroup threads");
    if (firstBatchProcessed_) {
      if (commOverlap_) { shutDownCommOverlapThreads(); }
      signalFinishedToServerShards(); // notify other nodes that this node has finished training
      shutDownServerShardThread();
    }
    delete pool_;
    LOG(info)->info("Shutdown successful");
  }

  /**
   * @brief Update any client model with given batch if batch is assigned to this node
   *
   * @param batch Batch to use in update
   */
  void update(Ptr<data::Batch> batch) {
    if (batchIter_ % mpi_comm_world_size_ == mpi_my_rank_) { // Only take batch assigned to this node (@INFO: Changing seed randomizer across nodes instead of this gives worse results)
      execute(batch);
    }
    batchIter_++;
  }

  /**
   * @brief Load models from disk if file exists and setting is not disabled
   */
  void load() {
    if(!options_->get<bool>("no-reload")) {
      std::string init = options_->get<std::string>("model");
      if(boost::filesystem::exists(init)) {
        size_t i = 0;
        if(scheduler_)
          scheduler_->load(init);
        for(auto graph : graphs_)
          builders_[i++]->load(graph, init);
      }
    }
  }

  /**
   * @brief Save model of first client's graph to disk
   *
   * @param final Whether this is the final save
   */
  void save(bool final = false) { save(graphs_[0], final); }

  /**
   * @brief Save model of given graph to disk
   *
   * @param final Whether this is the final save
   */
  void save(Ptr<ExpressionGraph> graph, bool final = false) {
    int idx = 0;
    for(int i = 0; i < graphs_.size(); ++i) {
      if(graph == graphs_[i]) {
        idx = i;
        break;
      }
    }

    if(options_->get<bool>("overwrite")) {
      std::string name = options_->get<std::string>("model");

      builders_[idx]->save(graphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    } else {
      std::string name = options_->get<std::string>("model");

      if(!final) {
        std::string numberOfBatches
            = scheduler_ ? std::to_string(scheduler_->numberOfBatches()) :
              "unknown";
        std::string nameOverwrite = name;
        nameOverwrite.replace(
            name.size() - 4, 4, ".iter" + numberOfBatches + ".npz");
        builders_[idx]->save(graphs_[idx], nameOverwrite);
      }

      builders_[idx]->save(graphs_[idx], name, true);
      if(scheduler_)
        scheduler_->save(name);
    }
  }

  /**
   * @brief Collect statistics from first client's graph
   *
   * @return Statisticsi of first client's graph
   */
  Ptr<data::BatchStats> collectStats() {
    return builders_[0]->collectStats(graphs_[0]);
  }
};

}
