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
// If upload_only=true, skips download even if remote is newer (safe while game is running).
// Creates a timestamped backup before any overwrite.
// Returns a SyncResult and sets out_message to a human-readable status.
SyncResult Sync(const char* local_path, std::string& out_message, bool upload_only = false);

// Returns the last status string (result of last sync)
const char* GetStatusString();

// Returns a live progress string during sync, e.g. "Downloading... 45%"
// Empty string when not actively syncing.
const char* GetProgressString();

// Returns the SyncResult of the most recently completed async sync.
SyncResult GetLastResult();

// Start a sync on a background thread. Returns immediately.
// If upload_only=true, will not download even if remote is newer.
// Check IsSyncing() to know when it finishes, GetLastResult() for the outcome.
void StartAsyncSync(const char* local_path, bool upload_only = false);

// True while an async sync is running.
bool IsSyncing();

} // namespace WebDAVSync
