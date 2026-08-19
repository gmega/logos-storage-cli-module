#pragma once

#include <string>
#include <logos_result.h>
#include <logos_module_context.h>

/**
 * A synchronous, CLI-shaped facade over the asynchronous `storage_module`.
 *
 * The public methods below ARE the module's API (universal authoring model);
 * they block until the underlying transfer finishes and print a progress bar
 * to the module host's stdout while it runs.
 *
 * The storage node is started when the module is loaded (`onContextReady`)
 * and stopped when it is unloaded (destructor), so callers never manage the
 * node lifecycle themselves.
 */
class StorageCliImpl : public LogosModuleContext
{
public:
    /**
     * Uploads a file to the local node. Returns `true` if upload succeeded,
     * false otherwise.
     */
    StdLogosResult publish(const std::string& input);
    /**
     * Downloads a file from the network onto the specified local path.
     * Returns `true` if download succeeded, false otherwise.
     */
    StdLogosResult download(const std::string& cid, const std::string& output);

protected:
    /// Starts (and configures) the storage node when the module is loaded.
    void onContextReady() override;
};
