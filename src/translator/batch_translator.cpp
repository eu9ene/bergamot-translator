#include "batch_translator.h"
#include "batch.h"
#include "common/logging.h"
#include "data/corpus.h"
#include "data/text_input.h"
#include "translator/beam_search.h"

namespace marian {
namespace bergamot {

BatchTranslator::BatchTranslator(DeviceId const device,
                                 std::vector<Ptr<Vocab const>> &vocabs,
                                 Ptr<Options> options,
                                 const void * model_memory)
    : device_(device), options_(options), vocabs_(&vocabs), model_memory_(model_memory) {}

void BatchTranslator::initialize() {
  // Initializes the graph.
  if (options_->hasAndNotEmpty("shortlist")) {
    int srcIdx = 0, trgIdx = 1;
    bool shared_vcb = vocabs_->front() == vocabs_->back();
    slgen_ = New<data::LexicalShortlistGenerator>(options_, vocabs_->front(),
                                                  vocabs_->back(), srcIdx,
                                                  trgIdx, shared_vcb);
  }

  graph_ = New<ExpressionGraph>(true); // always optimize
  auto prec = options_->get<std::vector<std::string>>("precision", {"float32"});
  graph_->setDefaultElementType(typeFromString(prec[0]));
  graph_->setDevice(device_);
  graph_->getBackend()->configureDevice(options_);
  graph_->reserveWorkspaceMB(options_->get<size_t>("workspace"));
  if (model_memory_) { // If we have provided a byte array that contains the model memory, we can initialise the model from there, as opposed to from reading in the config file
    if ((uintptr_t)model_memory_ % 256 != 0) {
      std::cerr << "The provided memory is not aligned to 256 bytes and will crash when vector instructions are used on it." << std::endl;
      exit(1);
    }
    const std::vector<const void *> container = {model_memory_}; // Marian supports multiple models initialised in this manner hence std::vector. However we will only ever use 1 during decoding.
    scorers_ = createScorers(options_, container);
  } else {
    scorers_ = createScorers(options_);
  }
  for (auto scorer : scorers_) {
    scorer->init(graph_);
    if (slgen_) {
      scorer->setShortlistGenerator(slgen_);
    }
  }
  graph_->forward();
}

void BatchTranslator::translate(Batch &batch) {
  std::vector<data::SentenceTuple> batchVector;

  auto &sentences = batch.sentences();
  for (auto &sentence : sentences) {
    data::SentenceTuple sentence_tuple(sentence.lineNumber());
    Segment segment = sentence.getUnderlyingSegment();
    sentence_tuple.push_back(segment);
    batchVector.push_back(sentence_tuple);
  }

  size_t batchSize = batchVector.size();
  std::vector<size_t> sentenceIds;
  std::vector<int> maxDims;
  for (auto &ex : batchVector) {
    if (maxDims.size() < ex.size())
      maxDims.resize(ex.size(), 0);
    for (size_t i = 0; i < ex.size(); ++i) {
      if (ex[i].size() > (size_t)maxDims[i])
        maxDims[i] = (int)ex[i].size();
    }
    sentenceIds.push_back(ex.getId());
  }

  typedef marian::data::SubBatch SubBatch;
  typedef marian::data::CorpusBatch CorpusBatch;

  std::vector<Ptr<SubBatch>> subBatches;
  for (size_t j = 0; j < maxDims.size(); ++j) {
    subBatches.emplace_back(
        New<SubBatch>(batchSize, maxDims[j], vocabs_->at(j)));
  }

  std::vector<size_t> words(maxDims.size(), 0);
  for (size_t i = 0; i < batchSize; ++i) {
    for (size_t j = 0; j < maxDims.size(); ++j) {
      for (size_t k = 0; k < batchVector[i][j].size(); ++k) {
        subBatches[j]->data()[k * batchSize + i] = batchVector[i][j][k];
        subBatches[j]->mask()[k * batchSize + i] = 1.f;
        words[j]++;
      }
    }
  }

  for (size_t j = 0; j < maxDims.size(); ++j)
    subBatches[j]->setWords(words[j]);

  auto corpus_batch = Ptr<CorpusBatch>(new CorpusBatch(subBatches));
  corpus_batch->setSentenceIds(sentenceIds);

  auto trgVocab = vocabs_->back();
  auto search = New<BeamSearch>(options_, scorers_, trgVocab);

  auto histories = std::move(search->search(graph_, corpus_batch));
  batch.completeBatch(histories);
}

} // namespace bergamot
} // namespace marian
