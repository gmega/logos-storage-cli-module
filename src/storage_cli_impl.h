#pragma once

#include <logos_module_context.h>
#include <logos_result.h>
#include <string>

/**
 * A synchronous, CLI-shaped facade over the asynchronous `storage_module`.
 */
class StorageCliImpl : public LogosModuleContext {
public:
  /**
   * Uploads a file to the local node and returns the completion result.
   */
  StdLogosResult publish(const std::string &input);
  /**
   * Downloads a file from the network onto the specified local path.
   * Returns the completion result.
   */
  StdLogosResult download(const std::string &cid, const std::string &output);

protected:
  /// Starts (and configures) the storage node when the module is loaded.
  void onContextReady() override;
};
