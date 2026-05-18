#pragma once

#include <JuceHeader.h>

//==============================================================================
// Data model for a single track returned from the Water API
//==============================================================================
struct TrackInfo
{
    juce::String id;
    juce::String name;
    juce::String key;
    juce::String tags;    // dot-separated genre/mood tags
    juce::String idType;  // "melody", "beat", "topline", "song"
    double       bpm      { 0.0 };
    int          duration { 0 };
    juce::String demoHash;
};

//==============================================================================
// WaterAPI — static async helpers.  All network I/O runs on background threads;
// callbacks are dispatched to the message thread via MessageManager::callAsync.
//==============================================================================
class WaterAPI
{
public:
    static constexpr const char* kBaseURL     = "https://water.95ent.ai";
    static constexpr const char* kSupabaseURL = "https://hlqwvctfmxljjmosfcqq.supabase.co";

    //==========================================================================
    // Auth
    //==========================================================================
    /** POST /api/plugin/v1/auth  {email, password}
        callback(success, accessToken, errorMessage) */
    static void login (const juce::String& email,
                       const juce::String& password,
                       std::function<void(bool, juce::String, juce::String)> callback);

    /** GET /api/plugin/v1/auth/start
        callback(success, sessionId) */
    static void authStart (std::function<void(bool, juce::String)> callback);

    /** GET /api/plugin/v1/auth/poll?session_id=
        callback(ok, accessToken, refreshToken)
        ok=false + empty tokens = still pending (202)
        ok=false + "expired" = session expired */
    static void authPoll (const juce::String& sessionId,
                          std::function<void(bool, juce::String, juce::String)> callback);

    /** POST /api/plugin/v1/auth/refresh  {refresh_token}
        callback(success, newAccessToken, newRefreshToken) */
    static void authRefresh (const juce::String& refreshToken,
                             std::function<void(bool, juce::String, juce::String)> callback);

    //==========================================================================
    // Library
    //==========================================================================
    /** GET /api/plugin/v1/tracks?page=&search=&per_page=
        callback(success, tracks) */
    static void fetchTracks (const juce::String& token,
                             int                 page,
                             const juce::String& search,
                             std::function<void(bool, juce::Array<TrackInfo>)> callback);

    /** Fetches ALL pages of the catalogue (per_page=100, up to 1000 tracks).
        Stops early when a page returns fewer rows than requested, or when
        the API's "total" field is satisfied.  callback(success, allTracks) */
    static void fetchAllTracks (const juce::String& token,
                                const juce::String& search,
                                std::function<void(bool, juce::Array<TrackInfo>)> callback);

    /** POST /api/plugin/v1/tracks/{id}/download — returns signed download URL.
        Fallback when demo_hash is empty. callback(success, url) */
    static void getDownloadURL (const juce::String& trackId,
                                const juce::String& token,
                                std::function<void(bool, juce::String)> callback);

    //==========================================================================
    // Upload
    //==========================================================================
    /** POST /api/plugin/v1/upload   multipart file field
        callback(success, loopId) */
    static void uploadFile (const juce::File&   file,
                            const juce::String& token,
                            std::function<void(bool, juce::String)> callback);

    //==========================================================================
    // Audio stream
    //==========================================================================
    /** Returns the URL for streaming a demo audio clip.
        Uses direct Supabase public storage when demoHash is set (BRAIN-processed),
        otherwise falls back to the Water proxy which handles signed-URL streams. */
    static juce::URL getAudioStreamURL (const juce::String& trackId,
                                        const juce::String& demoHash);

private:
    WaterAPI() = delete;

    // Internal helper: runs block on a background thread then fires callback on message thread
    static void runAsync (std::function<void()> work);

    // Parse the tracks JSON array; returns empty array on failure
    static juce::Array<TrackInfo> parseTracksJSON (const juce::String& json);
};
