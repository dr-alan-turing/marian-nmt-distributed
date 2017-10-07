#pragma once

#include "marian.h"

#include "common/config.h"
#include "data/batch_generator.h"
#include "data/corpus.h"
#include "models/model_task.h"
#include "training/scheduler.h"
#include "training/validator.h"
#include "rescorer/score_collector.h"

namespace marian {

using namespace data;

class Rescorer {
private:
  Ptr<models::ModelBase> builder_;

public:
  Rescorer(Ptr<Options> options) :
    builder_(models::from_options(options)) {}

  void load(Ptr<ExpressionGraph> graph, const std::string& modelFile) {
    builder_->load(graph, modelFile);
  }

  Expr build(Ptr<ExpressionGraph> graph, Ptr<data::CorpusBatch> batch) {
    return builder_->build(graph, batch);
  }
};

template <class Model>
class Rescore : public ModelTask {
private:
  Ptr<Config> options_;
  Ptr<Corpus> corpus_;
  Ptr<ExpressionGraph> graph_;
  Ptr<Model> model_;

public:
  Rescore(Ptr<Config> options)
      : options_(options),
        corpus_(New<Corpus>(options_)),
        graph_(New<ExpressionGraph>(true)) {
    corpus_->prepare();

    auto device = options_->get<std::vector<size_t>>("devices").front();
    graph_->setDevice(device);
    graph_->reserveWorkspaceMB(options_->get<size_t>("workspace"));

    auto modelFile = options_->get<std::string>("model");
    auto modelOptions = New<Config>(*options);
    try {
      modelOptions->loadModelParameters(modelFile);
    } catch(std::runtime_error& e) {
      LOG(warn)->warn("No model settings found in model file");
    }

    Ptr<Options> temp = New<Options>();
    temp->merge(options);
    temp->set("inference", true);
    temp->set("cost-type", "ce-rescore");
    model_ = New<Model>(temp);

    model_->load(graph_, modelFile);
  }

  void run() {
    Ptr<BatchGenerator<Corpus>> batchGenerator
        = New<BatchGenerator<Corpus>>(corpus_, options_);
    batchGenerator->prepare(false);

    auto output = New<ScoreCollector>();

    bool summarize = options_->get<bool>("summarize");
    float sumCost = 0;
    size_t sumWords = 0;
    while(*batchGenerator) {
      auto batch = batchGenerator->next();

      auto costNode = model_->build(graph_, batch);
      graph_->forward();

      std::vector<float> scores;
      costNode->val()->get(scores);

      for(auto s : scores)
        sumCost += s;
      sumWords += batch->back()->batchWords();

      if(!summarize) {
        for(size_t i = 0; i < batch->size(); ++i) {
          output->Write(batch->getSentenceIds()[i], scores[i]);
        }
      }
    }

    if(summarize)
      std::cout << "Perplexity: " << std::exp(-(float)sumCost / (float)sumWords) << std::endl;
  }
};

}
