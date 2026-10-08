#include "storage_cli_impl.h"
#include "logos_sdk.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <iostream>
#include <mutex>
#include <system_error>
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

// We'll use two promises: one for synchronising node startup, and another for
// upload/download operation results.
std::promise<bool> gStarted;
// The start future is potentially called by several different threads, so we
// need a shared future.
std::shared_future<bool> gStartedFut = gStarted.get_future().share();
// A rejected start can also emit a completion event. Resolve startup only once.
std::once_flag gStartedOnce;
// gResult is initialised during an asynchronous operation dispatch, set in
// the callback once, and consumed by the dispatcher exactly once, so we can
// use a regular future.
std::promise<std::string> gResult;
// Serialises upload/download operations.
std::mutex gOpLock;

int64_t gTransferBytes = 0;
int64_t gTransferTotal = 0;

void echo(const std::string &line, bool endline = true) {
  std::cout << line;
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
  if (!j.is_object()) {
    echo("Failed to parse payload: " + payload);
    return json::object();
  }
  return j;
}

void onProgress(const std::string &payload) {
  const json j = parsePayload(payload);
  gTransferBytes += j.value("bytes", int64_t{0});
  gTransferTotal = j.value("total", gTransferTotal);
  printProgress();
}

void onDone(const std::string &payload) { gResult.set_value(payload); }

void onStarted(bool success) {
  std::call_once(gStartedOnce, [success] { gStarted.set_value(success); });
}

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
  std::scoped_lock lock(gOpLock);

  gResult = std::promise<std::string>();
  gTransferBytes = 0;
  gTransferTotal = total;

  // Actually sends the operation to Storage.
  StdLogosResult started = op();
  if (!started.success) {
    return started;
  }

  const std::string result = gResult.get_future().get();
  const json payload = parsePayload(result);
  const bool success = payload.value("success", false);
  return StdLogosResult{.success = success,
                        .value = payload,
                        .error = success ? "" : payload.value("error", "Transfer failed")};
}

} // namespace

void StorageCliImpl::onContextReady() {
  StorageModule &storage = modules().storage_module;

  storage.onStorageStart([](const std::string &payload) {
    const json j = parsePayload(payload);
    echo("Node started with result: " + j.dump());
    onStarted(j.value("success", false));
  });

  storage.onStorageUploadProgress(&onProgress);
  storage.onStorageDownloadProgress(&onProgress);
  storage.onStorageUploadDone(&onDone);
  storage.onStorageDownloadDone(&onDone);

  echo("starting storage node (data-dir /tmp/logos-storage, network "
       "logos.test)...");

  if (storage.isRunning()) {
    onStarted(true);
    return;
  }
  if (!storage.init(kNodeConfig)) {
    echo("failed to initialise storage module");
    onStarted(false);
    return;
  }
  if (!storage.start()) {
    echo("node start was rejected");
    onStarted(false);
  }
}

StdLogosResult StorageCliImpl::publish(const std::string &input) {
  // Paths are resolved in the daemon process. Callers should use absolute paths.
  std::error_code ec;
  const std::filesystem::path path = std::filesystem::absolute(input, ec);
  if (ec) {
    return {.success = false, .value = {}, .error = ec.message()};
  }
  const auto size = static_cast<int64_t>(std::filesystem::file_size(path, ec));
  if (ec) {
    echo("upload failed: cannot read " + input + " (" + ec.message() + ")");
    return {.success = false, .value = {}, .error = ec.message()};
  }

  echo("uploading " + path.string() + " (" + std::to_string(size) + " bytes)");

  return syncTransferOp("upload", size, [&] {
    return modules().storage_module.uploadUrl(path.string(), kChunkSize, true);
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

  // The download progress events supply the total size.
  return syncTransferOp("download", 0, [&] {
    return modules().storage_module.downloadToUrl(cid, path.string(), false,
                                                  kChunkSize, false, true);
  });
}
