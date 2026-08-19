#include "storage_cli_impl.h"
#include "logos_sdk.h"

#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>

using nlohmann::json;

namespace {

constexpr const char *kNodeConfig = R"({
    "log-level": "INFO",
    "nat": "auto",
    "data-dir": "/tmp/logos-storage",
    "network": "logos.test"
})";

constexpr int64_t kChunkSize = 65536;

// We'll use two promises: one for synchronizing node startup, and another for
// upload/download operation results.
std::promise<bool> gStarted;
// The start future is potentially called by several different threads, so we
// need a shared future.
std::shared_future<bool> gStartedFut = gStarted.get_future().share();
// gResult is initialized during an asynchronous operation dispatch, set in
// the callback once, and consumed by the dispatcher exactly once, so we can
// use a regular future.
std::promise<std::string> gResult;
// Serializes upload/download operations.
std::mutex g_opLock;

int64_t gTransferBytes = 0;
int64_t gTransferTotal = 0;

void echo(const std::string &line, bool endline = true) {
  std::cout << "[storage_cli] " << line;
  if (endline) {
    std::cout << '\n';
  }
  std::cout.flush();
}

void printProgress() {
  if (gTransferTotal == 0) {
    echo(" " + std::to_string(gTransferBytes) + " bytes");
    return;
  }
  int64_t completed = std::min(gTransferBytes, gTransferTotal);
  echo("  " + std::to_string(completed * 100 / gTransferTotal) + "% (" +
       std::to_string(completed) + " of " + std::to_string(gTransferTotal) +
       " bytes)");
}

json parsePayload(const std::string &payload) {
  json j = json::parse(payload, nullptr, false);
  if (j.is_discarded()) {
    echo("Failed to parse payload: " + payload);
    return {};
  }
  return j;
}

void onProgress(const std::string &payload) {
  gTransferBytes += parsePayload(payload).value("bytes", int64_t{0});
  printProgress();
}

void onDone(const std::string &payload) { gResult.set_value(payload); }

StdLogosResult syncTransferOp(const std::string &what, int64_t total,
                              const std::function<StdLogosResult()> &op) {
  echo("Waiting for node to start.");
  if (!gStartedFut.get()) {
    return StdLogosResult{
        .success = false, .value = {}, .error = "Node start failed"};
  }
  echo("Node is started, attempting to run " + what + " operation.");

  // This will block attempts to run multiple operations at once.
  // This is not a limitation in storage but of our state tracking.
  std::scoped_lock lock(g_opLock);

  gResult = std::promise<std::string>();
  gTransferBytes = 0;
  gTransferTotal = total;

  // Actually sends the operation to Storage.
  StdLogosResult started = op();
  if (!started.success) {
    return started;
  }

  // Storage has its own internal op timeout so I don't need one here.
  const std::string result = gResult.get_future().get();
  const json payload = parsePayload(result);
  return StdLogosResult{.success = payload.value("success", false),
                        .value = payload,
                        .error = ""};
}

} // namespace

void StorageCliImpl::onContextReady() {
  StorageModule &storage = modules().storage_module;

  storage.onStorageStart([](const std::string &payload) {
    const json j = parsePayload(payload);
    echo("Node started with result: " + j.dump());
    gStarted.set_value(j.value("success", false));
  });

  storage.onStorageUploadProgress(&onProgress);
  storage.onStorageDownloadProgress(&onProgress);
  storage.onStorageUploadDone(&onDone);
  storage.onStorageDownloadDone(&onDone);

  echo("starting storage node (data-dir /tmp/logos-storage, network "
       "logos.test)...");

  if (!storage.init(kNodeConfig)) {
    echo("failed to initialize storage module. We'll assume it has already "
         "been initialized.");
    gStarted.set_value(true);
    return;
  }
  if (!storage.start()) {
    echo("node start was rejected. We'll assume it has already been started.");
    gStarted.set_value(true);
  }
}

StdLogosResult StorageCliImpl::publish(const std::string &input) {
  // the module, so the file has to exist there for the upload to make sense.
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(input, ec);
  const auto size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec) {
    echo("upload failed: cannot read " + input + " (" + ec.message() + ")");
    return {.success = false, .value = {}, .error = ec.message()};
  }

  echo("uploading " + path.string() + " (" + std::to_string(size) + " bytes)");

  return syncTransferOp("upload", size, [&] {
    return modules().storage_module.uploadUrl(path.string(), kChunkSize);
  });
}

StdLogosResult StorageCliImpl::download(const std::string &cid,
                                        const std::string &output) {
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(output, ec);
  if (ec) {
    echo("download failed: bad output path " + output + " (" + ec.message() +
         ")");
    return {.success = false, .value = {}, .error = ec.message()};
  }

  echo("Downloading " + cid + " to " + path.string());

  // TODO fetch manifest and compute the total for download.
  return syncTransferOp("download", 0, [&] {
    return modules().storage_module.downloadToUrl(cid, path.string(), false,
                                                  kChunkSize);
  });
}
