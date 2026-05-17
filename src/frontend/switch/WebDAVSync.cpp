#include "WebDAVSync.h"
#include "PlatformConfig.h"

#include <curl/curl.h>
#include <mbedtls/md5.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <string>

namespace WebDAVSync
{

static char s_status_str[128]   = "Never synced";
static char s_progress_str[128] = "";

static void dbg(const char* fmt, ...)
{
    FILE* f = fopen("/switch/melonds/webdav_debug.log", "a");
    if (!f) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(f, fmt, args);
    va_end(args);
    fprintf(f, "\n");
    fclose(f);
}

// ---- libcurl helpers --------------------------------------------------------

static size_t write_to_file_cb(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    FILE* f = (FILE*)userdata;
    return fwrite(ptr, size, nmemb, f);
}

static size_t write_to_string_cb(void* ptr, size_t size, size_t nmemb, void* userdata)
{
    std::string* s = (std::string*)userdata;
    s->append((char*)ptr, size * nmemb);
    return size * nmemb;
}

static size_t header_cb(char* buffer, size_t size, size_t nitems, void* userdata)
{
    std::string* headers = (std::string*)userdata;
    headers->append(buffer, size * nitems);
    return size * nitems;
}

static CURL* make_curl(const char* url)
{
    CURL* curl = curl_easy_init();
    if (!curl) return nullptr;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_USERNAME, Config::WebDAVUsername);
    curl_easy_setopt(curl, CURLOPT_PASSWORD, Config::WebDAVPassword);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    return curl;
}

// Build the manifest key (path relative to WebDAV root, no leading slash, .srm extension)
static std::string manifest_key(const char* local_path)
{
    const char* slash = strrchr(local_path, '/');
    const char* filename = slash ? slash + 1 : local_path;

    std::string fname = filename;
    if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".sav")
        fname = fname.substr(0, fname.size() - 4) + ".srm";

    std::string remote = Config::WebDAVRemotePath;
    if (!remote.empty() && remote.front() == '/') remote = remote.substr(1);
    if (!remote.empty() && remote.back() != '/') remote += '/';
    return remote + fname;
}

// Build the full remote URL for a local save file path
static std::string remote_url(const char* local_path)
{
    const char* slash = strrchr(local_path, '/');
    const char* filename = slash ? slash + 1 : local_path;

    std::string url = Config::WebDAVURL;
    if (!url.empty() && url.back() != '/') url += '/';
    std::string remote = Config::WebDAVRemotePath;
    if (!remote.empty() && remote.front() == '/') remote = remote.substr(1);
    if (!remote.empty() && remote.back() != '/') remote += '/';
    url += remote;

    std::string fname = filename;
    if (fname.size() > 4 && fname.substr(fname.size() - 4) == ".sav")
        fname = fname.substr(0, fname.size() - 4) + ".srm";

    char* escaped = curl_easy_escape(nullptr, fname.c_str(), (int)fname.size());
    if (escaped) { url += escaped; curl_free(escaped); }
    else url += fname;
    return url;
}

// Get remote file Last-Modified as time_t. Returns 0 on failure.
static time_t get_remote_mtime(const char* url)
{
    CURL* curl = make_curl(url);
    if (!curl) return 0;

    std::string headers;
    curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headers);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    dbg("HEAD result: curl=%d http=%ld", (int)res, http_code);

    if (res != CURLE_OK || http_code == 404) return 0;

    const char* needle = "last-modified: ";
    size_t pos = headers.find(needle);
    if (pos == std::string::npos) { dbg("No Last-Modified header"); return 0; }

    pos += strlen(needle);
    size_t end = headers.find("\r\n", pos);
    std::string date_str = headers.substr(pos, end - pos);

    static const char* months[] = {"Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"};
    static const int days_before_month[] = {0,31,59,90,120,151,181,212,243,273,304,334};
    char mon_str[4] = {};
    int day = 0, year = 0, hour = 0, min = 0, sec = 0, mon = 0;
    if (sscanf(date_str.c_str(), "%*3s, %d %3s %d %d:%d:%d GMT",
               &day, mon_str, &year, &hour, &min, &sec) != 6)
        return 0;
    for (int i = 0; i < 12; i++)
        if (strncmp(mon_str, months[i], 3) == 0) { mon = i; break; }

    int y = year;
    int prev = y - 1;
    long leaps = (prev/4 - 1969/4) - (prev/100 - 1969/100) + (prev/400 - 1969/400);
    long days  = (long)(y - 1970) * 365 + leaps;
    days += days_before_month[mon];
    if (mon > 1 && (y % 4 == 0 && (y % 100 != 0 || y % 400 == 0))) days++;
    days += day - 1;
    return (time_t)(days * 86400LL + hour * 3600 + min * 60 + sec);
}

struct ProgressCtx { bool uploading; };

static int progress_cb(void* userdata, curl_off_t dltotal, curl_off_t dlnow,
                       curl_off_t ultotal, curl_off_t ulnow)
{
    ProgressCtx* ctx = (ProgressCtx*)userdata;
    if (ctx->uploading && ultotal > 0)
        snprintf(s_progress_str, sizeof(s_progress_str),
                 "Uploading... %d%% (%lld KB)", (int)(ulnow * 100 / ultotal), (long long)(ulnow / 1024));
    else if (!ctx->uploading && dltotal > 0)
        snprintf(s_progress_str, sizeof(s_progress_str),
                 "Downloading... %d%% (%lld KB)", (int)(dlnow * 100 / dltotal), (long long)(dlnow / 1024));
    return 0; // returning non-zero aborts the transfer
}

// Upload local file via HTTP PUT
static bool upload(const char* local_path, const char* url)
{
    FILE* f = fopen(local_path, "rb");
    if (!f) return false;

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    CURL* curl = make_curl(url);
    if (!curl) { fclose(f); return false; }

    ProgressCtx pctx = { true };
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READDATA, f);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE, fsize);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);

    snprintf(s_progress_str, sizeof(s_progress_str), "Uploading...");
    CURLcode res = curl_easy_perform(curl);
    s_progress_str[0] = '\0';

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t uploaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_UPLOAD_T, &uploaded);
    curl_easy_cleanup(curl);
    fclose(f);

    if (res == CURLE_OK && (http_code == 200 || http_code == 201 || http_code == 204))
    {
        snprintf(s_status_str, sizeof(s_status_str), "Uploaded save (%lld KB)", (long long)(uploaded / 1024));
        return true;
    }
    return false;
}

// Download remote file to a temp path, then atomically replace local_path
static bool download(const char* url, const char* local_path)
{
    std::string tmp = std::string(local_path) + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return false;

    CURL* curl = make_curl(url);
    if (!curl) { fclose(f); return false; }

    ProgressCtx pctx = { false };
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_cb);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &pctx);

    snprintf(s_progress_str, sizeof(s_progress_str), "Downloading...");
    CURLcode res = curl_easy_perform(curl);
    s_progress_str[0] = '\0';

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_off_t downloaded = 0;
    curl_easy_getinfo(curl, CURLINFO_SIZE_DOWNLOAD_T, &downloaded);
    curl_easy_cleanup(curl);
    fclose(f);

    if (res != CURLE_OK || http_code != 200)
    {
        remove(tmp.c_str());
        return false;
    }

    snprintf(s_status_str, sizeof(s_status_str), "Downloaded save (%lld KB)", (long long)(downloaded / 1024));
    remove(local_path);
    rename(tmp.c_str(), local_path);
    return true;
}

// Backup local file to /switch/melonds/backups/<name>-YYMMDD-HHMMSS
static void backup_local(const char* local_path)
{
    const char* slash = strrchr(local_path, '/');
    const char* filename = slash ? slash + 1 : local_path;

    char backup_dir[256];
    snprintf(backup_dir, sizeof(backup_dir), "/switch/melonds/backups");
    mkdir(backup_dir, 0777);

    time_t now = time(nullptr);
    struct tm* tm = localtime(&now);
    char timestamp[32];
    strftime(timestamp, sizeof(timestamp), "%y%m%d-%H%M%S", tm);

    char backup_path[512];
    snprintf(backup_path, sizeof(backup_path), "%s/%s-%s", backup_dir, filename, timestamp);

    FILE* src = fopen(local_path, "rb");
    FILE* dst = fopen(backup_path, "wb");
    if (src && dst)
    {
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), src)) > 0)
            fwrite(buf, 1, n, dst);
    }
    if (src) fclose(src);
    if (dst) fclose(dst);
}

// ---- RetroArch manifest helpers ---------------------------------------------

// Compute MD5 of a file, return 32-char hex string or "" on failure
static std::string md5_of_file(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) return "";

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts_ret(&ctx);

    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        mbedtls_md5_update_ret(&ctx, buf, n);
    fclose(f);

    unsigned char digest[16];
    mbedtls_md5_finish_ret(&ctx, digest);
    mbedtls_md5_free(&ctx);

    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i*2, 3, "%02x", digest[i]);
    return std::string(hex);
}

// Download manifest.server as a string. Returns "" on failure.
static std::string download_manifest(const std::string& manifest_url)
{
    CURL* curl = make_curl(manifest_url.c_str());
    if (!curl) return "";

    std::string body;
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || (http_code != 200 && http_code != 0))
    {
        dbg("Manifest download: curl=%d http=%ld", (int)res, http_code);
        if (http_code == 404) return "[]"; // no manifest yet — start fresh
        return "";
    }
    return body;
}

// Upload a string as manifest.server via HTTP PUT
static bool upload_manifest(const std::string& manifest_url, const std::string& content)
{
    CURL* curl = make_curl(manifest_url.c_str());
    if (!curl) return false;

    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
    curl_easy_setopt(curl, CURLOPT_READFUNCTION, nullptr);

    // Use a local copy for the read callback
    struct ReadCtx { const char* data; size_t len; size_t pos; };
    ReadCtx ctx = { content.c_str(), content.size(), 0 };

    curl_easy_setopt(curl, CURLOPT_READFUNCTION, +[](char* buf, size_t size, size_t nmemb, void* ud) -> size_t {
        ReadCtx* c = (ReadCtx*)ud;
        size_t remain = c->len - c->pos;
        size_t n = size * nmemb < remain ? size * nmemb : remain;
        memcpy(buf, c->data + c->pos, n);
        c->pos += n;
        return n;
    });
    curl_easy_setopt(curl, CURLOPT_READDATA, &ctx);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE, (long)content.size());

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    dbg("Manifest upload: curl=%d http=%ld", (int)res, http_code);
    return res == CURLE_OK && (http_code == 200 || http_code == 201 || http_code == 204);
}

// Update manifest.server on the server: set the hash for key to new_hash.
// The manifest is a JSON array: [{"path":"saves/foo.srm","hash":"abcd..."},...]
static void update_manifest(const char* local_path, const std::string& new_hash)
{
    std::string base_url = Config::WebDAVURL;
    if (!base_url.empty() && base_url.back() != '/') base_url += '/';
    std::string manifest_url = base_url + "manifest.server";

    std::string key = manifest_key(local_path);
    dbg("Updating manifest: key=%s hash=%s", key.c_str(), new_hash.c_str());

    std::string manifest = download_manifest(manifest_url);
    if (manifest.empty()) { dbg("Could not download manifest, skipping"); return; }

    // Remove ALL existing entries for this key (handles duplicates from prior bugs),
    // then append a single correct entry.
    // Entries look like: {"path": "saves/foo.srm","hash": "abcd..."} or compact form.
    // We search for both spaced and compact forms.
    auto remove_entries = [&](const std::string& path_token) {
        size_t pos = 0;
        while ((pos = manifest.find(path_token, pos)) != std::string::npos)
        {
            // Find the opening { of this entry
            size_t brace_open = manifest.rfind('{', pos);
            if (brace_open == std::string::npos) break;
            // Find the closing }
            size_t brace_close = manifest.find('}', pos);
            if (brace_close == std::string::npos) break;
            brace_close++; // include '}'
            // Also eat any trailing comma + whitespace or leading comma
            size_t erase_start = brace_open;
            size_t erase_end   = brace_close;
            // Check for preceding comma+whitespace
            size_t before = erase_start;
            while (before > 0 && (manifest[before-1] == ' ' || manifest[before-1] == '\n' || manifest[before-1] == '\r')) before--;
            if (before > 0 && manifest[before-1] == ',') { erase_start = before - 1; }
            // Check for trailing comma+whitespace if no preceding comma
            else {
                while (erase_end < manifest.size() && (manifest[erase_end] == ' ' || manifest[erase_end] == '\n' || manifest[erase_end] == '\r')) erase_end++;
                if (erase_end < manifest.size() && manifest[erase_end] == ',') erase_end++;
            }
            manifest.erase(erase_start, erase_end - erase_start);
            pos = erase_start;
        }
    };
    // Try both spacing variants
    remove_entries("\"path\": \"" + key + "\"");
    remove_entries("\"path\":\"" + key + "\"");

    // Append single clean entry
    std::string new_entry = "\n  {\"path\": \"" + key + "\", \"hash\": \"" + new_hash + "\"}";
    size_t close = manifest.rfind(']');
    if (close != std::string::npos)
    {
        bool has_entries = manifest.find('{') != std::string::npos;
        manifest.insert(close, (has_entries ? "," : "") + new_entry);
    }
    else
    {
        manifest = "[" + new_entry + "\n]";
    }

    upload_manifest(manifest_url, manifest);
}

// ---- Public API -------------------------------------------------------------

SyncResult Sync(const char* local_path, std::string& out_message)
{
    dbg("=== Sync called: %s", local_path);
    dbg("URL: %s  User: %s", Config::WebDAVURL, Config::WebDAVUsername);

    if (Config::WebDAVURL[0] == '\0' || Config::WebDAVUsername[0] == '\0')
    {
        dbg("ERROR: WebDAV not configured");
        out_message = "WebDAV not configured";
        return Sync_NoConfig;
    }

    std::string url = remote_url(local_path);
    dbg("Remote URL: %s", url.c_str());

    struct stat st;
    time_t local_mtime = 0;
    bool local_exists = (stat(local_path, &st) == 0);
    if (local_exists) local_mtime = st.st_mtime;
    dbg("Local: exists=%d mtime=%ld", (int)local_exists, (long)local_mtime);

    time_t remote_mtime = get_remote_mtime(url.c_str());
    bool remote_exists = (remote_mtime > 0);
    dbg("Remote: exists=%d mtime=%ld", (int)remote_exists, (long)remote_mtime);

    time_t last_sync = (time_t)Config::WebDAVLastSync;
    dbg("LastSync=%ld", (long)last_sync);

    bool local_changed  = local_exists  && (local_mtime  > last_sync);
    bool remote_changed = remote_exists && (remote_mtime > last_sync);
    dbg("local_changed=%d remote_changed=%d", (int)local_changed, (int)remote_changed);

    if (!local_exists && !remote_exists)
    {
        dbg("RESULT: No save file found locally or remotely");
        out_message = "No save file found locally or remotely";
        return Sync_UpToDate;
    }

    if (local_changed && remote_changed)
    {
        if (local_mtime >= remote_mtime)
        {
            if (upload(local_path, url.c_str()))
            {
                std::string hash = md5_of_file(local_path);
                if (!hash.empty()) update_manifest(local_path, hash);
                Config::WebDAVLastSync = (int)time(nullptr);
                snprintf(s_status_str, sizeof(s_status_str), "Conflict: kept local (newer)");
                out_message = s_status_str;
                return Sync_Conflict;
            }
        }
        else
        {
            backup_local(local_path);
            if (download(url.c_str(), local_path))
            {
                Config::WebDAVLastSync = (int)time(nullptr);
                snprintf(s_status_str, sizeof(s_status_str), "Conflict: kept remote (newer), local backed up");
                out_message = s_status_str;
                return Sync_Conflict;
            }
        }
        out_message = "Conflict resolution failed";
        return Sync_Error;
    }

    if (local_changed || (!remote_exists && local_exists))
    {
        if (upload(local_path, url.c_str()))
        {
            std::string hash = md5_of_file(local_path);
            dbg("Upload MD5: %s", hash.c_str());
            if (!hash.empty()) update_manifest(local_path, hash);
            Config::WebDAVLastSync = (int)time(nullptr);
            // s_status_str already set by upload() with size info
            out_message = s_status_str;
            return Sync_Uploaded;
        }
        snprintf(s_status_str, sizeof(s_status_str), "Upload failed");
        out_message = s_status_str;
        return Sync_Error;
    }

    if (remote_changed || (!local_exists && remote_exists))
    {
        if (local_exists) backup_local(local_path);
        if (download(url.c_str(), local_path))
        {
            Config::WebDAVLastSync = (int)time(nullptr);
            // s_status_str already set by download() with size info
            out_message = s_status_str;
            return Sync_Downloaded;
        }
        snprintf(s_status_str, sizeof(s_status_str), "Download failed");
        out_message = s_status_str;
        return Sync_Error;
    }

    snprintf(s_status_str, sizeof(s_status_str), "Save already up to date");
    out_message = s_status_str;
    return Sync_UpToDate;
}

const char* GetStatusString()
{
    return s_status_str;
}

const char* GetProgressString()
{
    return s_progress_str;
}

} // namespace WebDAVSync
