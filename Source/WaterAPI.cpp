#include "WaterAPI.h"

//==============================================================================
// Internal helpers
//==============================================================================
namespace
{
    // Build an authorization header string
    juce::StringPairArray makeAuthHeader (const juce::String& token)
    {
        juce::StringPairArray headers;
        headers.set ("Authorization", "Bearer " + token);
        headers.set ("Accept", "application/json");
        return headers;
    }

    // Minimal JSON string-value extractor (avoids pulling in a JSON library dependency)
    // Handles both quoted strings and unquoted numbers for simple flat objects.
    juce::String extractJSONString (const juce::String& json, const juce::String& key)
    {
        juce::String needle = "\"" + key + "\"";
        int idx = json.indexOf (needle);
        if (idx < 0) return {};

        idx += needle.length();
        // skip whitespace + colon
        while (idx < json.length() && (json[idx] == ' ' || json[idx] == ':' || json[idx] == '\t'))
            idx++;

        if (idx >= json.length()) return {};

        if (json[idx] == '"')
        {
            // quoted string
            idx++;
            juce::String result;
            while (idx < json.length() && json[idx] != '"')
            {
                if (json[idx] == '\\' && idx + 1 < json.length())
                {
                    idx++;  // skip escape char
                    result += json[idx];
                }
                else
                {
                    result += json[idx];
                }
                idx++;
            }
            return result;
        }
        else
        {
            // unquoted value (number, bool, null)
            juce::String result;
            while (idx < json.length() && json[idx] != ',' && json[idx] != '}' && json[idx] != ']' && json[idx] != ' ')
                result += json[idx++];
            return result.trim();
        }
    }

    // Returns the body of the HTTP response, or empty string on failure.
    // Sets statusCode out-param.
    juce::String httpGET (const juce::URL& url,
                          const juce::StringPairArray& headers,
                          int& statusCode)
    {
        juce::URL::InputStreamOptions opts (juce::URL::ParameterHandling::inAddress);

        juce::String extraHeaders;
        for (auto& k : headers.getAllKeys())
            extraHeaders += k + ": " + headers.getValue (k, {}) + "\r\n";

        auto stream = url.createInputStream (opts.withExtraHeaders (extraHeaders)
                                                 .withHttpRequestCmd ("GET")
                                                 .withStatusCode (&statusCode)
                                                 .withConnectionTimeoutMs (15000));

        if (stream == nullptr) return {};
        return stream->readEntireStreamAsString();
    }

    juce::String httpPOST (const juce::URL& url,
                           const juce::String& bodyJSON,
                           const juce::StringPairArray& extraHeaders,
                           int& statusCode)
    {
        juce::URL::InputStreamOptions opts (juce::URL::ParameterHandling::inAddress);

        juce::String allHeaders = "Content-Type: application/json\r\n";
        for (auto& k : extraHeaders.getAllKeys())
            allHeaders += k + ": " + extraHeaders.getValue (k, {}) + "\r\n";

        auto stream = url.withPOSTData (bodyJSON)
                         .createInputStream (opts.withExtraHeaders (allHeaders)
                                                 .withHttpRequestCmd ("POST")
                                                 .withStatusCode (&statusCode)
                                                 .withConnectionTimeoutMs (15000));

        if (stream == nullptr) return {};
        return stream->readEntireStreamAsString();
    }
}

//==============================================================================
void WaterAPI::runAsync (std::function<void()> work)
{
    // Spin up a lambda on a background thread; JUCE thread pool is heavyweight
    // so we use std::thread with a detach pattern here.
    std::thread ([w = std::move (work)]() mutable { w(); }).detach();
}

//==============================================================================
void WaterAPI::login (const juce::String& email,
                      const juce::String& password,
                      std::function<void(bool, juce::String, juce::String)> callback)
{
    runAsync ([email, password, cb = std::move (callback)]()
    {
        juce::String body = "{\"email\":\"" + email.replace ("\"", "\\\"")
                          + "\",\"password\":\"" + password.replace ("\"", "\\\"") + "\"}";

        int statusCode = 0;
        juce::URL url (juce::String (kBaseURL) + "/api/plugin/v1/auth");
        juce::StringPairArray noHeaders;
        juce::String response = httpPOST (url, body, noHeaders, statusCode);

        bool ok     = (statusCode >= 200 && statusCode < 300);
        juce::String token = ok ? extractJSONString (response, "access_token") : juce::String{};
        juce::String error = ok ? juce::String{} : extractJSONString (response, "error");
        if (error.isEmpty() && !ok) error = "Login failed (HTTP " + juce::String (statusCode) + ")";

        juce::MessageManager::callAsync ([cb, ok, token, error]() { cb (ok, token, error); });
    });
}

//==============================================================================
juce::Array<TrackInfo> WaterAPI::parseTracksJSON (const juce::String& json)
{
    juce::Array<TrackInfo> results;

    // Find the "tracks" array
    int arrStart = json.indexOf ("\"tracks\"");
    if (arrStart < 0) return results;

    arrStart = json.indexOf (arrStart, "[");
    if (arrStart < 0) return results;

    int depth = 0;
    int objStart = -1;
    for (int i = arrStart; i < json.length(); ++i)
    {
        juce::juce_wchar c = json[i];
        if (c == '[' || c == '{')
        {
            depth++;
            if (c == '{' && depth == 2)
                objStart = i;
        }
        else if (c == ']' || c == '}')
        {
            if (c == '}' && depth == 2 && objStart >= 0)
            {
                juce::String obj = json.substring (objStart, i + 1);
                TrackInfo t;
                t.id       = extractJSONString (obj, "id");
                t.name     = extractJSONString (obj, "name");
                t.key      = extractJSONString (obj, "key");
                t.tags     = extractJSONString (obj, "tags");
                t.idType   = extractJSONString (obj, "id_type");
                t.demoHash = extractJSONString (obj, "demo_hash");
                // JSON null parses as the literal string "null" — treat as empty
                if (t.demoHash == "null") t.demoHash = {};

                juce::String bpmStr = extractJSONString (obj, "bpm");
                t.bpm = bpmStr.getDoubleValue();

                juce::String durStr = extractJSONString (obj, "duration");
                t.duration = durStr.getIntValue();

                if (t.id.isNotEmpty())
                    results.add (t);
                objStart = -1;
            }
            depth--;
            if (depth == 0) break;
        }
    }
    return results;
}

//==============================================================================
void WaterAPI::fetchTracks (const juce::String& token,
                            int                 page,
                            const juce::String& search,
                            std::function<void(bool, juce::Array<TrackInfo>)> callback)
{
    runAsync ([token, page, search, cb = std::move (callback)]()
    {
        juce::String urlStr = juce::String (kBaseURL) + "/api/plugin/v1/tracks"
                            + "?page=" + juce::String (page)
                            + "&search=" + juce::URL::addEscapeChars (search, true);

        int statusCode = 0;
        juce::URL url (urlStr);
        auto headers = makeAuthHeader (token);
        juce::String response = httpGET (url, headers, statusCode);

        bool ok = (statusCode >= 200 && statusCode < 300);
        juce::Array<TrackInfo> tracks;
        if (ok) tracks = parseTracksJSON (response);

        juce::MessageManager::callAsync ([cb, ok, tracks]() { cb (ok, tracks); });
    });
}

//==============================================================================
void WaterAPI::fetchAllTracks (const juce::String& token,
                                const juce::String& search,
                                std::function<void(bool, juce::Array<TrackInfo>)> callback)
{
    runAsync ([token, search, cb = std::move (callback)]()
    {
        juce::Array<TrackInfo> allTracks;
        constexpr int kPerPage = 100;
        int page = 1;
        bool anyOk = false;

        while (page <= 10)   // safety cap: never fetch more than 1000 tracks
        {
            juce::String urlStr = juce::String (kBaseURL) + "/api/plugin/v1/tracks"
                                + "?page="     + juce::String (page)
                                + "&per_page=" + juce::String (kPerPage)
                                + "&search="   + juce::URL::addEscapeChars (search, true);

            int statusCode = 0;
            juce::String response = httpGET (juce::URL (urlStr), makeAuthHeader (token), statusCode);

            if (statusCode < 200 || statusCode >= 300)
                break;

            anyOk = true;
            auto pageTracks = parseTracksJSON (response);
            if (pageTracks.isEmpty()) break;

            allTracks.addArray (pageTracks);

            // If the API advertises a total, stop when we have it all
            juce::String totalStr = extractJSONString (response, "total");
            int total = totalStr.getIntValue();
            if (total > 0 && allTracks.size() >= total) break;

            // If API advertises no total: stop when last page was partial
            // (use min(kPerPage,50) as threshold so we handle APIs that ignore per_page)
            if (total <= 0 && pageTracks.size() < 50) break;

            page++;
        }

        juce::MessageManager::callAsync ([cb, anyOk, allTracks]() { cb (anyOk, allTracks); });
    });
}

//==============================================================================
void WaterAPI::uploadFile (const juce::File&   file,
                           const juce::String& token,
                           std::function<void(bool, juce::String)> callback)
{
    runAsync ([file, token, cb = std::move (callback)]()
    {
        if (!file.existsAsFile())
        {
            juce::MessageManager::callAsync ([cb]() { cb (false, "File not found"); });
            return;
        }

        // Build multipart body manually
        juce::String boundary = "WaterBoundary" + juce::String (juce::Time::currentTimeMillis());
        juce::MemoryOutputStream bodyStream;

        // Part header
        juce::String partHeader = "--" + boundary + "\r\n"
                                + "Content-Disposition: form-data; name=\"file\"; filename=\""
                                + file.getFileName() + "\"\r\n"
                                + "Content-Type: application/octet-stream\r\n\r\n";
        bodyStream.writeString (partHeader);

        // File data
        {
            juce::FileInputStream fis (file);
            if (fis.openedOk())
                bodyStream.writeFromInputStream (fis, -1);
        }
        bodyStream.writeString ("\r\n--" + boundary + "--\r\n");

        juce::MemoryBlock bodyData (bodyStream.getData(), bodyStream.getDataSize());

        juce::String extraHeaders = "Authorization: Bearer " + token + "\r\n"
                                  + "Content-Type: multipart/form-data; boundary=" + boundary + "\r\n";

        int statusCode = 0;
        juce::URL url (juce::String (kBaseURL) + "/api/plugin/v1/upload");

        juce::URL::InputStreamOptions opts (juce::URL::ParameterHandling::inAddress);
        auto stream = url.withPOSTData (bodyData)
                         .createInputStream (opts.withExtraHeaders (extraHeaders)
                                                 .withHttpRequestCmd ("POST")
                                                 .withStatusCode (&statusCode)
                                                 .withConnectionTimeoutMs (60000));

        juce::String response;
        if (stream != nullptr)
            response = stream->readEntireStreamAsString();

        bool ok = (statusCode >= 200 && statusCode < 300);
        juce::String loopId = ok ? extractJSONString (response, "loop_id") : juce::String{};
        juce::String error  = ok ? juce::String{} : ("Upload failed (HTTP " + juce::String (statusCode) + ")");

        juce::MessageManager::callAsync ([cb, ok, loopId, error]()
        {
            cb (ok, ok ? loopId : error);
        });
    });
}

//==============================================================================
juce::URL WaterAPI::getAudioStreamURL (const juce::String& trackId,
                                       const juce::String& demoHash)
{
    if (demoHash.isNotEmpty())
    {
        // BRAIN-processed: file stored as {demoHash}.mp3 in public demo bucket
        return juce::URL (juce::String (kSupabaseURL)
                        + "/storage/v1/object/public/demo/" + demoHash + ".mp3");
    }
    // Unprocessed: proxy streams from private material bucket (requires Bearer auth)
    return juce::URL (juce::String (kBaseURL) + "/api/melody/demo/" + trackId);
}

//==============================================================================
void WaterAPI::getDownloadURL (const juce::String& trackId,
                               const juce::String& token,
                               std::function<void(bool, juce::String)> callback)
{
    // Not used currently — kept as future hook for signed download URLs
    juce::MessageManager::callAsync ([cb = std::move (callback)]()
    {
        cb (false, {});
    });
}

//==============================================================================
void WaterAPI::authStart (std::function<void(bool, juce::String)> callback)
{
    runAsync ([cb = std::move (callback)]()
    {
        int statusCode = 0;
        juce::URL url (juce::String (kBaseURL) + "/api/plugin/v1/auth/start");
        juce::StringPairArray noHeaders;
        juce::String response = httpGET (url, noHeaders, statusCode);

        bool ok = (statusCode >= 200 && statusCode < 300);
        juce::String sessionId = ok ? extractJSONString (response, "session_id") : juce::String{};

        juce::MessageManager::callAsync ([cb, ok, sessionId]() { cb (ok && sessionId.isNotEmpty(), sessionId); });
    });
}

//==============================================================================
void WaterAPI::authPoll (const juce::String& sessionId,
                         std::function<void(bool, juce::String, juce::String)> callback)
{
    runAsync ([sessionId, cb = std::move (callback)]()
    {
        int statusCode = 0;
        juce::URL url (juce::String (kBaseURL) + "/api/plugin/v1/auth/poll?session_id="
                       + juce::URL::addEscapeChars (sessionId, true));
        juce::StringPairArray noHeaders;
        juce::String response = httpGET (url, noHeaders, statusCode);

        if (statusCode == 202)
        {
            // Still pending
            juce::MessageManager::callAsync ([cb]() { cb (false, {}, {}); });
            return;
        }

        bool ok = (statusCode >= 200 && statusCode < 300);
        juce::String access  = ok ? extractJSONString (response, "access_token")  : juce::String{};
        juce::String refresh = ok ? extractJSONString (response, "refresh_token") : juce::String{};

        juce::MessageManager::callAsync ([cb, ok, access, refresh]() { cb (ok && access.isNotEmpty(), access, refresh); });
    });
}

//==============================================================================
void WaterAPI::authRefresh (const juce::String& refreshToken,
                            std::function<void(bool, juce::String, juce::String)> callback)
{
    runAsync ([refreshToken, cb = std::move (callback)]()
    {
        juce::String body = "{\"refresh_token\":\"" + refreshToken.replace ("\"", "\\\"") + "\"}";

        int statusCode = 0;
        juce::URL url (juce::String (kBaseURL) + "/api/plugin/v1/auth/refresh");
        juce::StringPairArray noHeaders;
        juce::String response = httpPOST (url, body, noHeaders, statusCode);

        bool ok      = (statusCode >= 200 && statusCode < 300);
        juce::String access  = ok ? extractJSONString (response, "access_token")  : juce::String{};
        juce::String refresh = ok ? extractJSONString (response, "refresh_token") : juce::String{};

        juce::MessageManager::callAsync ([cb, ok, access, refresh]() { cb (ok && access.isNotEmpty(), access, refresh); });
    });
}
