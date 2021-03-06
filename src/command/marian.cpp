#include "marian.h"

#include "training/graph_group_async.h"
#include "training/graph_group_async_drop.h"
#include "training/graph_group_singleton.h"
#include "training/graph_group_multinode.h"
#include "training/graph_group_multinode_drop.h"
#include "training/graph_group_sync.h"
#include "training/training.h"

#if MPI_FOUND
#include <mpi.h>
#endif

bool configureMPI(int argc, char** argv);

void terminateMPI();

int main(int argc, char** argv) {
  using namespace marian;

  auto options = New<Config>(argc, argv);
  auto devices = options->get<std::vector<size_t>>("devices");

  bool useMultiNode = options->get<bool>("multi-node") && configureMPI(argc, argv);

  if(useMultiNode) {
    if(options->get<float>("grad-dropping-rate") > 0.0) {
      LOG(info, "Launching Sparse Multi-Node Graph Group");
      New<Train<MultiNodeSparseGraphGroup>>(options)->run();
    } else {
      LOG(info, "Launching Multi-Node Graph Group");
      New<Train<MultiNodeGraphGroup>>(options)->run();
    }
  } else if(devices.size() > 1) {
    if(options->get<bool>("sync-sgd"))
      New<Train<SyncGraphGroup>>(options)->run();
    else if(options->get<float>("grad-dropping-rate") > 0.0)
      New<Train<AsyncGraphGroupDrop>>(options)->run();
    else
      New<Train<AsyncGraphGroup>>(options)->run();
  } else {
    New<Train<SingletonGraph>>(options)->run();
  }

  if (options->get<bool>("multi-node"))
    terminateMPI();

  return 0;
}

bool configureMPI(int argc, char** argv) {
  int provided_thread_mode = 0;
  #if MPI_FOUND
  MPI_Init_thread(&argc, &argv, MPI_THREAD_MULTIPLE, &provided_thread_mode);
  MPI_Comm_set_errhandler(MPI_COMM_WORLD, MPI_ERRORS_RETURN); // Enable if occasional truncation errors
  ABORT_IF(provided_thread_mode < MPI_THREAD_MULTIPLE, "Your version of MPI does not support multi-threaded communication.");
  #endif
  return true;
}

void terminateMPI() {
  #if MPI_FOUND
  MPI_Finalize();
  #endif
}
