#include <cstdlib>
#include <future>
#include <iostream>
#include <sstream>

#include "common/definitions.h"
#include "common/timer.h"
#include "common/utils.h"
#include "marian.h"
#include "translator/history.h"
#include "translator/output_collector.h"
#include "translator/output_printer.h"
#include "translator/parser.h"
#include "translator/response.h"
#include "translator/service.h"

typedef std::future<marian::bergamot::Response> ResponseFuture;

void marian_decoder_minimal(std::vector<ResponseFuture> &responseFutures,
                            marian::Ptr<marian::Vocab const> targetVocab,
                            marian::Ptr<marian::Options> options) {

  bool doNbest = options->get<bool>("n-best");
  auto collector =
      marian::New<marian::OutputCollector>(options->get<std::string>("output"));

  // There is a dependency of vocabs here.
  auto printer = marian::New<marian::OutputPrinter>(options, targetVocab);
  if (options->get<bool>("quiet-translation"))
    collector->setPrintingStrategy(marian::New<marian::QuietPrinting>());

  for (auto &responseFuture : responseFutures) {
    responseFuture.wait();
    const marian::bergamot::Response &response = responseFuture.get();
    auto histories = response.histories();
    for (auto &history : histories) {
      std::stringstream best1;
      std::stringstream bestn;
      printer->print(history, best1, bestn);
      collector->Write((long)history->getLineNum(), best1.str(), bestn.str(),
                       doNbest);
    }
  }
}

int main(int argc, char *argv[]) {
  auto cp = marian::bergamot::createConfigParser();
  auto options = cp.parseOptions(argc, argv, true);
  marian::timer::Timer decoderTimer;

  marian::bergamot::Service service(options);
  // Read a large input text blob from stdin

  size_t maxiBatchLines = options->get<int>("maxi-batch");
  std::vector<ResponseFuture> maxiBatchResponseFutures;

  size_t lineNumber{0}, maxiBatches{0}, lineNumberBegin{0};
  std::string line;
  std::ostringstream std_input;
  while (std::getline(std::cin, line)) {
    ++lineNumber;
    std_input << line << "\n";
    if (lineNumber % maxiBatchLines == 0) {
      std::string input = std_input.str();
      ResponseFuture responseFuture =
          service.translatePart(std::move(input), lineNumberBegin);
      maxiBatchResponseFutures.push_back(std::move(responseFuture));
      // Reset;
      std_input.str("");
      ++maxiBatches;
      lineNumberBegin = lineNumber;
      LOG(info, "{} maxibatches constructed, {} lines covered", maxiBatches,
          lineNumber);
    }
  }

  // Wait on future until marian::bergamot::Response is complete

  marian_decoder_minimal(maxiBatchResponseFutures, service.targetVocab(),
                         options);

  LOG(info, "Total time: {:.5f}s wall", decoderTimer.elapsed());
  service.stop();
  return 0;
}
