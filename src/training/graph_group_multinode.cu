#include "training/graph_group_multinode.h"

#include "kernels/tensor_operators.h"

namespace marian {

void MultiNodeGraphGroup::setScheduler(Ptr<Scheduler> scheduler) {
  scheduler_ = scheduler;
  // optimizer has to be registered last to see a change of learning rate
  scheduler_->registerTrainingObserver(scheduler_);

  for (auto opt : gpuShardsOpts_) {
    scheduler_->registerTrainingObserver(opt);
  }
}

Tensor MultiNodeGraphGroup::newTensor(int size, int device) {
  Tensor t;
  Ptr<TensorAllocator> allocator = New<TensorAllocator>(device);
  allocator->reserveExact(size * sizeof(float));
  allocator->allocate(t, {1, size});
  allocators_.push_back(allocator);
  return t;
}

void MultiNodeGraphGroup::initFirstRun(Ptr<data::Batch> batch) {
  // Initialize client graphs (incl. params) and builders
  for (size_t i = 0; i < graphs_.size(); ++i) {
    THREAD_GUARD(
        builders_[i]->build(graphs_[i], batch);
        graphs_[i]->forward();
    );
  }
  cudaStreamSynchronize(0);
  // Initialize variables for server shard(s) on this node
  initServerShards();
  // Initialize client variables for inter-node communication
  initClientCommunicationVars();
  // Launch server thread to communicate with clients
  launchServerThread();
  // Launch compute/communicate overlap threads if enabled
  if (commOverlap_) {
    launchCommOverlapThreads();
  }
}

void MultiNodeGraphGroup::initMPI() {
#if MPI_FOUND
  MPI_Comm_size(MPI_COMM_WORLD, &mpi_comm_world_size_);
  MPI_Comm_rank(MPI_COMM_WORLD, &mpi_my_rank_);
#endif
}

void MultiNodeGraphGroup::initServerShards(bool initFullSendReceiveBuffer) {
  // Initialize server shard sizes for all nodes (remote + current)
  size_t totalParamsGradsSize = graphs_[0]->params()->vals()->size();
  size_t nodeShardSize = ceilf(((float) totalParamsGradsSize) / mpi_comm_world_size_);
  size_t remainingTotalSize = totalParamsGradsSize;
  for (int node = 0; node < mpi_comm_world_size_; node++) {
    size_t size = std::min(nodeShardSize, remainingTotalSize);
    nodeShardSizes_.push_back(size);
    remainingTotalSize -= size;
  }

  // Initialize this shard's params and grads
  size_t thisNodeSize = nodeShardSizes_[mpi_my_rank_];
  size_t gpuShardSize = ceilf(((float) thisNodeSize) / devices_.size());
  size_t offset = 0;

  for (int gpu = 0; gpu < devices_.size(); gpu++) {
    size_t size = std::min(gpuShardSize, thisNodeSize - offset);
    Tensor gpuParams = newTensor(size, devices_[gpu]);
    gpuParams->copyFrom(graphs_[0]->params()->vals()->subtensor(offset, size));
    gpuShardsParams_.push_back(gpuParams);
    gpuShardsGrads_.push_back(newTensor(size, devices_[gpu]));
    gpuShardSizes_.push_back(size);
    offset += size;
  }

  // Initialize full send/receive buffer
  if (initFullSendReceiveBuffer) {
    serverShardBuffer_ = std::vector<float>(nodeShardSizes_[mpi_my_rank_]);
  }
}

void MultiNodeGraphGroup::setupClientsOfNodesAndDevices() {
  int index = 0, node = 0, nClientsSeen = 0;
  numberClientsOfNodes_ = std::vector<int>(mpi_comm_world_size_, 0);
  while (index < multiNodeDevices_.size()) {
    if (numberClientsOfNodes_[node] == 0) {
      numberClientsOfNodes_[node] = (size_t) multiNodeDevices_[index];
      nClientsSeen = 0;
    } else if (nClientsSeen < numberClientsOfNodes_[node]) {
      if (node == mpi_my_rank_) {
        devices_.push_back((size_t)multiNodeDevices_[index]);
      }
      nClientsSeen++;
    } else {
      node++;
      index--;
    }
    index++;
  }
}

void MultiNodeGraphGroup::initClientCommunicationVars(bool initBuffers) { // @TODO: Integrate with clients / drop-rate / comm-overlap
  for (int gpu = 0; gpu < devices_.size(); gpu++) {
    if (initBuffers) {
      size_t size = nodeShardSizes_[mpi_my_rank_];
      clientCommBufferParams_.push_back(std::vector<float>(size));
      clientCommBufferGrads_.push_back(std::vector<float>(size));
    }
    if (commOverlap_) {
      size_t fullSize = graphs_[0]->params()->vals()->size();
      // Running sum of gradients
      Tensor sumGrads = newTensor(fullSize, devices_[gpu]);
      Element(functional::_1 = 0, sumGrads);
      cudaStreamSynchronize(0);
      gpuSummedGrads_.push_back(sumGrads);
      // Communication gradients buffer
      commBufferGrads_.push_back(newTensor(fullSize, devices_[gpu]));
      // Communication parameters buffer
      Tensor bufferParams = newTensor(fullSize, devices_[gpu]);
      bufferParams->copyFrom(graphs_[0]->params()->vals());
      commBufferParams_.push_back(bufferParams);
    }
  }
}

void MultiNodeGraphGroup::launchServerThread() {
#if MPI_FOUND
  serverShardThread_ = new std::thread([this] {
    int nCommunicatingNodes = mpi_comm_world_size_; // keep track of number of nodes still communicating with this shard
    MPI_Status status;
    do {
      // Receive grads from any client
      unsigned long messageInfo[4];
      MPI_Recv(&messageInfo, 4, MPI_UNSIGNED_LONG, MPI_ANY_SOURCE, MPI_TAG_GRAD_PUSH_, MPI_COMM_WORLD, &status);
      if (messageInfo[MSG_INFO_STATUS_] == STATUS_NODE_FINISHED_) {
        nCommunicatingNodes--;
        continue;
      } // register finished node and skip to next loop iteration
      MPI_Recv(serverShardBuffer_.data(), nodeShardSizes_[mpi_my_rank_], MPI_FLOAT, status.MPI_SOURCE, MPI_TAG_GRAD_PUSH_, MPI_COMM_WORLD, MPI_STATUS_IGNORE);

      // Update shard params asynchronously over GPUs
      std::vector<std::thread> threads;
      size_t offset = 0;
      for (int gpu = 0; gpu < devices_.size(); gpu++) {
        size_t size = gpuShardSizes_[gpu];

        threads.emplace_back(std::thread([=](int gpu, size_t offset, size_t size, size_t batchWords) {
          std::lock_guard<std::mutex> guard(mutexGpuShards_[gpu]);

          // Copy grads to appropriate GPU
          cudaMemcpy(gpuShardsGrads_[gpu]->data(), &serverShardBuffer_.at(offset), size * sizeof(float), cudaMemcpyHostToDevice);
          cudaStreamSynchronize(0);

          // Run optimizer on GPU
          if (scaleLearningRate_ && batchWords > 0) {
            gpuShardsOpts_[gpu]->update(gpuShardsParams_[gpu], gpuShardsGrads_[gpu], batchWords / avgBatchWords_);
          } else {
            gpuShardsOpts_[gpu]->update(gpuShardsParams_[gpu], gpuShardsGrads_[gpu]);
          }
          cudaStreamSynchronize(0);
          // Copy params from GPU
          cudaMemcpy(&serverShardBuffer_.at(offset), gpuShardsParams_[gpu]->data(), size * sizeof(float), cudaMemcpyDeviceToHost);
          cudaStreamSynchronize(0);
        }, gpu, offset, size, messageInfo[MSG_INFO_BATCHWORDS_]));

        offset += size;
      }
      for (auto &&t : threads) { t.join(); }

      // Send updated params to same client
      MPI_Ssend(serverShardBuffer_.data(), nodeShardSizes_[mpi_my_rank_], MPI_FLOAT, status.MPI_SOURCE,
                MPI_TAG_PARAM_PUSH_, MPI_COMM_WORLD);

    } while (nCommunicatingNodes != 0);
  });
#endif
}

void MultiNodeGraphGroup::synchronizeWithServerShards(Tensor newGrads, Tensor oldParams, int gpu, size_t batchWords, std::mutex *optionalBlockMutex) {
  #if MPI_FOUND
  size_t offset = 0;
  for (int node = 0; node < mpi_comm_world_size_; node++) {
    size_t nodeSize = nodeShardSizes_[node];

    // Update remotely if node != this node
    if (node != mpi_my_rank_) {

      // Copy grads from GPU
      cudaMemcpy(clientCommBufferGrads_[gpu].data(), newGrads->subtensor(offset, nodeSize)->data(), nodeSize * sizeof(float), cudaMemcpyDeviceToHost);
      cudaStreamSynchronize(0);

      {
        std::unique_lock<std::mutex> uniqueAccess = (optionalBlockMutex == nullptr) ? std::unique_lock<std::mutex>() : std::unique_lock<std::mutex>(*optionalBlockMutex, std::try_to_lock); // Lock mutex if provided

        // Send grads to server
        size_t messageInfo[4];
        messageInfo[MSG_INFO_SIZE_] = nodeSize;
        messageInfo[MSG_INFO_CLIENT_] = gpu;
        messageInfo[MSG_INFO_BATCHWORDS_] = batchWords;
        messageInfo[MSG_INFO_STATUS_] = STATUS_NODE_TRAINING_;
        MPI_Ssend(&messageInfo, 4, MPI_UNSIGNED_LONG, node, MPI_TAG_GRAD_PUSH_, MPI_COMM_WORLD);
        MPI_Ssend(clientCommBufferGrads_[gpu].data(), nodeSize, MPI_FLOAT, node, MPI_TAG_GRAD_PUSH_, MPI_COMM_WORLD);

        // Receive updated params from server
        MPI_Recv(clientCommBufferParams_[gpu].data(), nodeSize, MPI_FLOAT, node, MPI_TAG_PARAM_PUSH_, MPI_COMM_WORLD,
                 MPI_STATUS_IGNORE);
      }

      // Copy params to GPU
      cudaMemcpy(oldParams->subtensor(offset, nodeSize)->data(), clientCommBufferParams_[gpu].data(), nodeSize * sizeof(float), cudaMemcpyHostToDevice);
      cudaStreamSynchronize(0);


      // Update locally if node == this node
    } else {
      size_t localOffset = offset;
      std::vector<std::thread> threads;

      for (int gpu = 0; gpu < devices_.size(); gpu++) {
        size_t gpuSize = gpuShardSizes_[gpu];

        threads.emplace_back(std::thread([=](int gpu, size_t offset, size_t size) {
          std::lock_guard<std::mutex> guard(mutexGpuShards_[gpu]);

          // Copy grads to appropriate GPU
          gpuShardsGrads_[gpu]->copyFrom(newGrads->subtensor(offset, size));
          // Run optimizer on GPU
          if (scaleLearningRate_ && batchWords > 0) {
            gpuShardsOpts_[gpu]->update(gpuShardsParams_[gpu], gpuShardsGrads_[gpu], batchWords / avgBatchWords_);
          } else {
            gpuShardsOpts_[gpu]->update(gpuShardsParams_[gpu], gpuShardsGrads_[gpu]);
          }
          cudaStreamSynchronize(0);
          // Copy params back to current GPU
          oldParams->subtensor(offset, size)->copyFrom(gpuShardsParams_[gpu]);
        }, gpu, localOffset, gpuSize));

        localOffset += gpuSize;
      }
      for (auto &&t : threads) { t.join(); }
    }

    offset += nodeSize;
  }
  #endif
}

void MultiNodeGraphGroup::launchCommOverlapThreads() {
#if MPI_FOUND
  for (int gpu = 0; gpu < devices_.size(); gpu++) {
    clientCommThreads_.emplace_back(new std::thread([this](int gpu) {
      do {
        // Wait for GPU (client) to fill buffers pointers
        std::unique_lock<std::mutex> uniqueLock(mutexCommBuffersFilled_[gpu]);
        while (!commBuffersFilled_[gpu]) {
          cvCommBuffersFilled_[gpu].wait(uniqueLock);
        }

        if (stopClientCommThreads_) { break; }

        // Synchronize with server shards
        synchronizeWithServerShards(commBufferGrads_[gpu], commBufferParams_[gpu], gpu, scaleLearningRate_ ? gpuCommittedWordCounts_[gpu] : 0, commOverlapSingleActive_ ? &mutexCommChannel_ : nullptr);

        // Indicate that buffers can be read from and filled again
        commBuffersFilled_[gpu] = false;

      } while (!stopClientCommThreads_);
    }, gpu));
  }
#endif
}

void MultiNodeGraphGroup::execute(Ptr<data::Batch> batch) {
  if (!firstBatchProcessed_) {
    initFirstRun(batch);
    firstBatchProcessed_ = true;
  }

  auto task = [this](Ptr<data::Batch> batch) {
    static size_t i = 0;
    thread_local Ptr<ExpressionGraph> graph;
    thread_local Ptr<models::ModelBase> builder;
    thread_local size_t t = 0;
    thread_local size_t numSeenWords = 0;

    thread_local Tensor accGradients;
    thread_local Ptr<TensorAllocator> accAlloc;

    thread_local size_t my_id = 0;

    if (!graph) {
      std::lock_guard<std::mutex> lock(mutexClientInit_);
      my_id = i;
      graph = graphs_[i];
      builder = builders_[i++];
    }

    auto costNode = builder->build(graph, batch);

    graph->forward();
    float cost = costNode->scalar();
    graph->backward();

    // Get batch stats
    size_t batchWords = batch->words();

    Tensor gradients;
    if (!commOverlap_ && tau_ > 1) {
      if (t == 0) {
        accAlloc = New<TensorAllocator>(graph->getDevice());
        accAlloc->reserveExact(graph->params()->grads()->memory()->size());
        accAlloc->allocate(accGradients, graph->params()->grads()->shape());
        accGradients->set(0);
      }

      Element(functional::_1 += functional::_2, accGradients, graph->params()->grads());
      gradients = accGradients;
      numSeenWords += batchWords; // Keep track of how many words we've calculated the error from
    } else {
      gradients = graph->params()->grads();
      numSeenWords = batchWords;
    }

    t++;

    cudaStreamSynchronize(0);

    if (!commOverlap_ && t % tau_ == 0) {
      synchronizeWithServerShards(gradients, graph->params()->vals(), my_id, numSeenWords);
      numSeenWords = 0;

      if (tau_ > 1) {
        gradients->set(0);
      }
    }

    if (scheduler_) {
      boost::upgrade_lock<boost::shared_mutex> lock(schedulerMutex_);
      {
        boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
        scheduler_->update(cost, batch);
      }

      if (scheduler_->saving()) {
        boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
        //if(movingAvg_)
        //  fetchParams(graph->params()->vals(), paramsAvg_);
        this->save(graph);
      }

      if (scheduler_->validating()) {
        boost::upgrade_to_unique_lock<boost::shared_mutex> uniqueLock(lock);
        //if(movingAvg_)
        //  fetchParams(graph->params()->vals(), paramsAvg_);
        scheduler_->validate(graphs_);
      }
    }

    // Overlapping computations with communication
    if (commOverlap_) {

      // Add computed gradients to local running sum
      Element(functional::_1 = functional::_1 + functional::_2, gpuSummedGrads_[my_id], gradients);
      cudaStreamSynchronize(0);
      // Sum up word counts if batch flexible learning rate is enabled
      if (scaleLearningRate_) {
        gpuSummedWordCounts_[my_id] += numSeenWords;
      }

      // If reached max number of compute iterations per synchronisation, wait for communication channel to finish syncing
      if (maxNumberComputeIters_ != -1 && ++numberComputeIters_[my_id] >= maxNumberComputeIters_) {
        std::lock_guard<std::mutex> wait(mutexCommBuffersFilled_[my_id]);
        numberComputeIters_[my_id] = 0;
      }

      // If communication channel ready, swap graph's pointers with secondary buffers
      if (!commBuffersFilled_[my_id]) {
        std::unique_lock<std::mutex> tryLock(mutexCommBuffersFilled_[my_id], std::try_to_lock);
        if (tryLock.owns_lock()) {

          // Copy summed grads to communication buffer
          commBufferGrads_[my_id]->copyFrom(gpuSummedGrads_[my_id]);
          // Copy parameters from communication buffer
          graph->params()->vals()->copyFrom(commBufferParams_[my_id]);

          // Commit summed word counts if batch-flexible-lr enabled
          if (scaleLearningRate_) {
            gpuCommittedWordCounts_[my_id] = gpuSummedWordCounts_[my_id];
            gpuSummedWordCounts_[my_id] = 0;
          }

          // Notify communication thread that buffers have been read and filled
          commBuffersFilled_[my_id] = true;
          cvCommBuffersFilled_[my_id].notify_one();

          // Apply summed gradients to new parameters
          localOpts_[my_id]->update(graph->params()->vals(), gpuSummedGrads_[my_id]);
          cudaStreamSynchronize(0);
          // Clear summed gradients
          Element(functional::_1 = 0, gpuSummedGrads_[my_id]);
          cudaStreamSynchronize(0);

          numberComputeIters_[my_id] = 0;
        }

      }

    }

  };

  pool_->enqueue(task, batch);
}

void MultiNodeGraphGroup::signalFinishedToServerShards() {
  #if MPI_FOUND
  unsigned long messageInfo[4];
  messageInfo[MSG_INFO_STATUS_] = STATUS_NODE_FINISHED_;
  for (int node = 0; node < mpi_comm_world_size_; node++) {
    MPI_Ssend(&messageInfo, 4, MPI_UNSIGNED_LONG, node, MPI_TAG_GRAD_PUSH_, MPI_COMM_WORLD);
  }
  #endif
}

void MultiNodeGraphGroup::shutDownServerShardThread() {
  serverShardThread_->join(); // Wait for server thread to finish communicating (with unfinished nodes)
}

void MultiNodeGraphGroup::shutDownCommOverlapThreads() {
  stopClientCommThreads_ = true;
  for (int gpu = 0; gpu < devices_.size(); gpu++) {
    commBuffersFilled_[gpu] = true;
    cvCommBuffersFilled_[gpu].notify_one(); // Unblock thread from lock, then join it
    clientCommThreads_[gpu]->join();
  }
}

}