#pragma once

#include <string>

namespace WebDAVSync
{

// Result codes
enum SyncResult
{
    Sync_OK,
    Sync_NoConfig,
    Sync_UpToDate,
    Sync_Uploaded,
    Sync_Downloaded,
    Sync_Conflict,
    Sync_Error,
};

// Run a full sync for a given local save file path.
// Compares local vs remote modification times against last_sync_time.
// Creates a timestamped backup before any overwrite.
// Returns a SyncResult and sets out_message to a human-readable status.
SyncResult Sync(const char* local_path, std::string& out_message);

// Returns the last status string (result of last sync)
const char* GetStatusString();

// Returns a live progress string during sync, e.g. "Downloading... 45%"
// Empty string when not actively syncing.
const char* GetProgressString();

} // namespace WebDAVSync
