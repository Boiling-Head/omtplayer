// omt_imgui.cpp

//
//    BHM OMT Decoder
//    Copyright (c) 2026 Ranav Adhikari
//
//    This software is licensed under the MIT License.
//    See LICENSE file for details.
//
//    --- Third-party dependencies ---

//    This software uses:
//    - Dear ImGui (MIT License)
//    - SDL2 (zlib License)
//    - Open Media Transport (OMT)
//
//    All third-party libraries retain their original licenses.


// -----------------------------------------------------------------------------
// OMT receiver with SDL2 + ImGui control UI.
//
// Features:
//  - Separate video and control windows
//  - Settings persistence (URL, audio device, fullscreen, preferred video format)
//  - Video format switch using real OMTPreferredVideoFormat values
//  - Efficient upload paths for BGRA and UYVY (no extra CPU conversion here)
//  - Statistics HUD using omt_receive_getvideostatistics/audiostatistics
//  - Timecode overlay (toggle)
//  - Auto-reconnect (toggle, interval, retry counter, lost-stream detection)
//  - Time sync / PTP info (local analysis stub using Timestamp)
//  - GPU stats (CPU-side texture upload timing)
//  - Low-latency analysis tools (fps, jitter, audio queue delay)
//  - Remote OMT Discovery via omt_discovery_getaddresses
//  - Audio mixer UI: master gain, mute, audio delay display, max delay clamp
//  - Closing either window terminates the app
// -----------------------------------------------------------------------------

#include <iostream>
#include <stdexcept>
#include <cstring>
#include <vector>
#include <string>
#include <fstream>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cinttypes>   // for PRIu64
#include <future>

#define SDL_MAIN_HANDLED
#include "libomt.h"
#include <SDL.h>

// Dear ImGui
#include "imgui/imgui.h"
#include "imgui/backends/imgui_impl_sdl2.h"
#include "imgui/backends/imgui_impl_sdlrenderer2.h"

static const char* kControlWindowTitle = "Boiling Head Media OMT Decoder";
static const char* kDefaultVideoWindowTitle = "No OMT Source";
static const char* kAppVersion = "1.0.0.14";
static const char* kSupportUrl = "https://www.boilinghead.com/fundraiser/buy-me-a-beer/";
static const char* kOmtProjectUrl = "https://github.com/openmediatransport";

// RAII for SDL lifetime
struct SDLGuard {
    SDLGuard() {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_TIMER | SDL_INIT_AUDIO) != 0) {
            throw std::runtime_error(std::string("SDL_Init failed: ") + SDL_GetError());
        }
    }
    ~SDLGuard() {
        SDL_Quit();
    }
};

// --------------------------------------------------------
// App settings persisted to omt_gui.cfg
// --------------------------------------------------------
struct AppSettings {
    std::string url         = "omt://192.168.0.103:6400";
    std::string discoveryServer;
    int         audioIndex  = 0;
    bool        fullscreen  = false;
    int         videoFormat = OMTPreferredVideoFormat_UYVYorBGRA; // good default
};

static const char* kSettingsFile = "omt_gui.cfg";

void LoadSettings(AppSettings& s)
{
    std::ifstream f(kSettingsFile);
    if (!f.is_open()) return;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(f, line)) {
        lines.push_back(line);
    }

    if (!lines.empty() && !lines[0].empty())
        s.url = lines[0];

    int offset = 1;
    if (lines.size() >= 5) {
        s.discoveryServer = lines[1];
        offset = 2;
    }

    if (lines.size() > (size_t)offset) {
        try {
            s.audioIndex = std::stoi(lines[offset]);
        } catch (...) {
            s.audioIndex = 0;
        }
    }
    if (lines.size() > (size_t)(offset + 1)) {
        s.fullscreen = (lines[offset + 1] == "1");
    }
    if (lines.size() > (size_t)(offset + 2)) {
        try {
            s.videoFormat = std::stoi(lines[offset + 2]);
        } catch (...) {
            s.videoFormat = OMTPreferredVideoFormat_UYVYorBGRA;
        }
    }
}

void SaveSettings(const AppSettings& s)
{
    std::ofstream f(kSettingsFile, std::ios::trunc);
    if (!f.is_open()) return;

    f << s.url << "\n";
    f << s.discoveryServer << "\n";
    f << s.audioIndex << "\n";
    f << (s.fullscreen ? "1" : "0") << "\n";
    f << s.videoFormat << "\n";
}

// --------------------------------------------------------
// Runtime statistics and helpers
// --------------------------------------------------------
struct RuntimeStats {
    // Local counters
    uint64_t videoFrames   = 0;
    uint64_t audioFrames   = 0;

    double   avgVideoFps   = 0.0;
    double   instVideoFps  = 0.0;
    double   avgAudioFps   = 0.0;
    double   instAudioFps  = 0.0;

    Uint32   lastVideoMs   = 0;
    Uint32   lastAudioMs   = 0;
    double   lastFrameIntervalMs  = 0.0;
    double   minFrameIntervalMs   = 0.0;
    double   maxFrameIntervalMs   = 0.0;

    // Approx bandwidth from local cals or OMT stats
    double   approxVideoMbpsLocal = 0.0; // from frame sizes
    double   approxVideoMbpsOmt   = 0.0; // from omt_receive_getvideostatistics

    // Audio queue delay
    double   audioQueueMs   = 0.0;

    // Auto-reconnect attempts
    uint32_t reconnectAttempts = 0;

    // GPU-ish
    double   maxUploadMs    = 0.0;
};

static std::string FormatMs(double ms)
{
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f ms", ms);
    return std::string(buf);
}

static std::string FormatOmtTimestamp(int64_t timestampTicks)
{
    if (timestampTicks <= 0)
        return "N/A";

    const double TICKS_PER_SECOND = 10'000'000.0; // 1 second = 10,000,000 ticks
    double totalSeconds = timestampTicks / TICKS_PER_SECOND;

    int hours   = static_cast<int>(totalSeconds / 3600.0);
    int minutes = static_cast<int>((totalSeconds - hours * 3600.0) / 60.0);
    int seconds = static_cast<int>(totalSeconds) % 60;
    int millis  = static_cast<int>((totalSeconds - std::floor(totalSeconds)) * 1000.0);

    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", hours, minutes, seconds, millis);
    return std::string(buf);
}

// Convert ticks to milliseconds (double)
static double TicksToMs(int64_t ticks)
{
    if (ticks <= 0) return 0.0;
    const double TICKS_PER_MS = 10'000.0; // 1ms = 10,000 ticks
    return ticks / TICKS_PER_MS;
}

static void CopyToBuffer(char* dst, size_t dstSize, const std::string& value)
{
    if (!dst || dstSize == 0) return;

    std::snprintf(dst, dstSize, "%s", value.c_str());
}

static void OpenSupportPage()
{
    if (SDL_OpenURL(kSupportUrl) != 0) {
        std::cerr << "Failed to open support URL: " << SDL_GetError() << "\n";
    }
}

static void OpenOmtProjectPage()
{
    if (SDL_OpenURL(kOmtProjectUrl) != 0) {
        std::cerr << "Failed to open OMT project URL: " << SDL_GetError() << "\n";
    }
}

static std::vector<std::string> QueryOmtSources()
{
    std::vector<std::string> sources;

    int count = 0;
    char** addrs = omt_discovery_getaddresses(&count);
    if (addrs && count > 0) {
        for (int i = 0; i < count; ++i) {
            if (addrs[i] && addrs[i][0] != '\0') {
                sources.emplace_back(addrs[i]);
            }
        }
    }

    std::sort(sources.begin(), sources.end());
    sources.erase(std::unique(sources.begin(), sources.end()), sources.end());
    return sources;
}

// --------------------------------------------------------

int main(int, char**)
{
    try {
        SDLGuard sdl;

        // -----------------------------
        // Load persistent settings
        // -----------------------------
        AppSettings settings;
        LoadSettings(settings);

        // -----------------------------
        // SDL Windows + Renderers
        // -----------------------------
        int videoWidth  = 1280;
        int videoHeight = 720;

        SDL_Window* videoWindow = SDL_CreateWindow(
            kDefaultVideoWindowTitle,
            SDL_WINDOWPOS_CENTERED,
            SDL_WINDOWPOS_CENTERED,
            videoWidth,
            videoHeight,
            SDL_WINDOW_RESIZABLE
        );
        if (!videoWindow) {
            std::cerr << "SDL_CreateWindow (video) failed: " << SDL_GetError() << "\n";
            return 1;
        }

        SDL_Renderer* videoRenderer = SDL_CreateRenderer(
            videoWindow, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
        );
        if (!videoRenderer) {
            std::cerr << "SDL_CreateRenderer (video) failed: " << SDL_GetError() << "\n";
            SDL_DestroyWindow(videoWindow);
            return 1;
        }

        // Control window
        SDL_Window* ctrlWindow = SDL_CreateWindow(
            kControlWindowTitle,
            SDL_WINDOWPOS_CENTERED + 100,
            SDL_WINDOWPOS_CENTERED + 100,
            900,
            500,
            SDL_WINDOW_RESIZABLE
        );
        if (!ctrlWindow) {
            std::cerr << "SDL_CreateWindow (controls) failed: " << SDL_GetError() << "\n";
            SDL_DestroyRenderer(videoRenderer);
            SDL_DestroyWindow(videoWindow);
            return 1;
        }

        SDL_Renderer* ctrlRenderer = SDL_CreateRenderer(
            ctrlWindow, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC
        );
        if (!ctrlRenderer) {
            std::cerr << "SDL_CreateRenderer (controls) failed: " << SDL_GetError() << "\n";
            SDL_DestroyWindow(ctrlWindow);
            SDL_DestroyRenderer(videoRenderer);
            SDL_DestroyWindow(videoWindow);
            return 1;
        }

        // Apply fullscreen from settings (video window only)
        bool isFullscreen = settings.fullscreen;
        if (isFullscreen) {
            SDL_SetWindowFullscreen(videoWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
        }

        // -----------------------------
        // ImGui initialization
        // -----------------------------
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO(); (void)io;

        ImGui::StyleColorsDark();

        ImGui_ImplSDL2_InitForSDLRenderer(ctrlWindow, ctrlRenderer);
        ImGui_ImplSDLRenderer2_Init(ctrlRenderer);

        // -----------------------------
        // OMT & audio/video state
        // -----------------------------
        omt_receive_t* recv = nullptr;
        SDL_Texture*   videoTexture = nullptr;
        int texWidth  = 0;
        int texHeight = 0;
        Uint32 texPixelFormat = 0; // SDL pixel format currently used by texture

        SDL_AudioDeviceID audioDev = 0;
        bool              audioFormatSet = false;
        std::vector<float> audioInterleaved;

        bool isRunning   = true;
        bool isConnected = false;
        std::string connectionStatus = "Disconnected.";

        // Stats / analysis
        RuntimeStats stats;
        Uint32 appStartMs = SDL_GetTicks();

        // Last video timestamp & nominal framerate
        int64_t lastVideoTimestampTicks = -1;
        int     lastVideoFrameRateN     = 0;
        int     lastVideoFrameRateD     = 1;

        // GUI state
        char urlBuffer[256];
        std::memset(urlBuffer, 0, sizeof(urlBuffer));
        CopyToBuffer(urlBuffer, sizeof(urlBuffer), settings.url);

        char discoveryServerBuffer[256];
        std::memset(discoveryServerBuffer, 0, sizeof(discoveryServerBuffer));
        CopyToBuffer(discoveryServerBuffer,
                     sizeof(discoveryServerBuffer),
                     settings.discoveryServer);

        int preferredVideoFormat = settings.videoFormat;
        if (preferredVideoFormat < OMTPreferredVideoFormat_UYVY ||
            preferredVideoFormat > OMTPreferredVideoFormat_P216)
        {
            preferredVideoFormat = OMTPreferredVideoFormat_UYVYorBGRA;
        }

        bool showStatsHud        = true;
        bool showTimecode        = true;
        bool autoReconnect       = false;
        float autoReconnectSec   = 3.0f;
        Uint32 lastReconnectCheckMs = SDL_GetTicks();
        Uint32 lastGoodVideoMs   = 0;  // for lost-stream detection

        bool showSyncInfo        = false;
        bool showLatencyTools    = true;
        bool showGpuStats        = false;

        // Audio mixer
        float audioMasterGain    = 1.0f;
        bool  audioMute          = false;
        float maxAudioDelayMs    = 200.0f; // clamp queue above this

        // Remote discovery
        std::vector<std::string> discoveredSources;
        int selectedDiscoveryIndex = -1;
        std::string discoveryStatus = "Discovery not refreshed yet.";
        bool autoScanSources = false;
        bool discoveryInProgress = false;
        Uint32 lastDiscoveryRefreshMs = 0;
        const Uint32 DISCOVERY_REFRESH_MS = 2000;
        std::future<std::vector<std::string>> discoveryFuture;

        auto apply_discovery_server_setting = [&]() {
            omt_settings_set_string("DiscoveryServer", discoveryServerBuffer);
        };
        apply_discovery_server_setting();

        // Pre-enumerate audio devices
        std::vector<std::string> audioDevices;
        int numDevices = SDL_GetNumAudioDevices(0);
        for (int i = 0; i < numDevices; ++i) {
            const char* name = SDL_GetAudioDeviceName(i, 0);
            audioDevices.emplace_back(name ? name : "(unknown)");
        }
        std::cout << "Found " << audioDevices.size() << " audio devices.\n";

        int selectedAudioIndex = settings.audioIndex;
        if (selectedAudioIndex < 0 || selectedAudioIndex >= (int)audioDevices.size())
            selectedAudioIndex = 0;

        auto close_audio_device = [&]() {
            if (audioDev) {
                SDL_ClearQueuedAudio(audioDev);
                SDL_CloseAudioDevice(audioDev);
                audioDev = 0;
            }
            audioFormatSet = false;
            audioInterleaved.clear();
            stats.audioQueueMs = 0.0;
        };

        auto clear_video_state = [&]() {
            if (videoTexture) {
                SDL_DestroyTexture(videoTexture);
                videoTexture = nullptr;
            }
            texWidth = 0;
            texHeight = 0;
            texPixelFormat = 0;
            lastVideoTimestampTicks = -1;
            lastVideoFrameRateN = 0;
            lastVideoFrameRateD = 1;
            lastGoodVideoMs = 0;
        };

        auto reset_stream_stats = [&]() {
            stats.videoFrames = 0;
            stats.audioFrames = 0;
            stats.avgVideoFps = 0.0;
            stats.instVideoFps = 0.0;
            stats.avgAudioFps = 0.0;
            stats.instAudioFps = 0.0;
            stats.lastVideoMs = 0;
            stats.lastAudioMs = 0;
            stats.lastFrameIntervalMs = 0.0;
            stats.minFrameIntervalMs = 0.0;
            stats.maxFrameIntervalMs = 0.0;
            stats.approxVideoMbpsLocal = 0.0;
            stats.approxVideoMbpsOmt = 0.0;
            stats.audioQueueMs = 0.0;
            stats.maxUploadMs = 0.0;
        };

        auto disconnect = [&]() {
            isConnected = false;
            connectionStatus = "Disconnected.";
            SDL_SetWindowTitle(videoWindow, kDefaultVideoWindowTitle);
            if (recv) {
                omt_receive_destroy(recv);
                recv = nullptr;
            }
            close_audio_device();
            clear_video_state();

            std::cout << "Disconnected.\n";
        };

        auto connect = [&]() {
            apply_discovery_server_setting();

            if (urlBuffer[0] == '\0') {
                std::cerr << "No OMT source selected.\n";
                isConnected = false;
                connectionStatus = "No OMT source selected.";
                return;
            }

            if (recv) {
                omt_receive_destroy(recv);
                recv = nullptr;
            }
            close_audio_device();
            clear_video_state();
            reset_stream_stats();

            OMTFrameType frameTypes =
                static_cast<OMTFrameType>(OMTFrameType_Video | OMTFrameType_Audio);

            OMTPreferredVideoFormat fmt =
                static_cast<OMTPreferredVideoFormat>(preferredVideoFormat);

            recv = omt_receive_create(
                urlBuffer,
                frameTypes,
                fmt,
                OMTReceiveFlags_None
            );
            if (!recv) {
                std::cerr << "Failed to create OMT receiver.\n";
                isConnected = false;
                connectionStatus = "Failed to create OMT receiver.";
            } else {
                std::cout << "Connected to: " << urlBuffer
                          << " | Preferred format enum: " << preferredVideoFormat << "\n";
                isConnected         = true;
                audioFormatSet      = false;
                lastGoodVideoMs     = SDL_GetTicks();
                connectionStatus = std::string("Connected: ") + urlBuffer;
                SDL_SetWindowTitle(videoWindow, urlBuffer);
            }
        };

        auto switch_to_source = [&](const std::string& source) {
            bool wasConnected = isConnected;

            if (wasConnected) {
                disconnect();
            } else {
                close_audio_device();
                clear_video_state();
                reset_stream_stats();
            }

            CopyToBuffer(urlBuffer, sizeof(urlBuffer), source);
            settings.url = urlBuffer;
            connectionStatus = std::string("Selected: ") + urlBuffer;
            SDL_SetWindowTitle(videoWindow, urlBuffer);

            if (wasConnected) {
                connect();
            }
        };

        auto apply_discovery_results = [&](std::vector<std::string> sources) {
            apply_discovery_server_setting();

            std::string previousSelection;
            if (selectedDiscoveryIndex >= 0 &&
                selectedDiscoveryIndex < (int)discoveredSources.size())
            {
                previousSelection = discoveredSources[selectedDiscoveryIndex];
            } else if (urlBuffer[0] != '\0') {
                previousSelection = urlBuffer;
            }

            discoveredSources = std::move(sources);
            selectedDiscoveryIndex = -1;

            if (!discoveredSources.empty()) {
                auto it = std::find(discoveredSources.begin(),
                                    discoveredSources.end(),
                                    previousSelection);
                selectedDiscoveryIndex =
                    (it != discoveredSources.end())
                        ? (int)std::distance(discoveredSources.begin(), it)
                        : 0;

                CopyToBuffer(urlBuffer,
                             sizeof(urlBuffer),
                             discoveredSources[selectedDiscoveryIndex]);
                settings.url = urlBuffer;
            }

            discoveryStatus =
                "Discovery found " + std::to_string(discoveredSources.size()) + " source";
            if (discoveredSources.size() != 1) {
                discoveryStatus += "s";
            }
            discoveryStatus += ".";

            std::cout << discoveryStatus << "\n";
            lastDiscoveryRefreshMs = SDL_GetTicks();
            return discoveredSources.size();
        };

        auto start_discovery_scan = [&]() {
            if (discoveryInProgress) return;

            apply_discovery_server_setting();
            discoveryStatus = "Scanning for OMT sources...";
            discoveryInProgress = true;
            lastDiscoveryRefreshMs = SDL_GetTicks();
            discoveryFuture = std::async(std::launch::async, []() {
                return QueryOmtSources();
            });
        };

        auto poll_discovery_scan = [&]() {
            if (!discoveryInProgress || !discoveryFuture.valid()) return;

            auto status = discoveryFuture.wait_for(std::chrono::milliseconds(0));
            if (status == std::future_status::ready) {
                apply_discovery_results(discoveryFuture.get());
                discoveryInProgress = false;
            }
        };

        // For OMTStatistics
        OMTStatistics videoStats{};
        OMTStatistics audioStats{};
        Uint32 lastOmtStatsMs = SDL_GetTicks();

        // -----------------------------
        // Main loop
        // -----------------------------
        while (isRunning) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                ImGui_ImplSDL2_ProcessEvent(&e);

                if (e.type == SDL_QUIT) {
                    isRunning = false;
                }
                else if (e.type == SDL_WINDOWEVENT &&
                         e.window.event == SDL_WINDOWEVENT_CLOSE) {
                    Uint32 id = e.window.windowID;
                    if (id == SDL_GetWindowID(ctrlWindow) ||
                        id == SDL_GetWindowID(videoWindow)) {
                        // Close either window => exit program
                        isRunning = false;
                    }
                }
                else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                    if (isFullscreen) {
                        isFullscreen = false;
                        settings.fullscreen = false;
                        SDL_SetWindowFullscreen(videoWindow, 0);
                    }
                }
                else if (e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_f) {
                    isFullscreen = !isFullscreen;
                    settings.fullscreen = isFullscreen;
                    if (isFullscreen)
                        SDL_SetWindowFullscreen(videoWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
                    else
                        SDL_SetWindowFullscreen(videoWindow, 0);
                }
            }

            Uint32 nowMs = SDL_GetTicks();
            poll_discovery_scan();

            if (autoScanSources &&
                nowMs - lastDiscoveryRefreshMs >= DISCOVERY_REFRESH_MS)
            {
                start_discovery_scan();
            }

            // Auto-reconnect if enabled and not connected
            if (autoReconnect && !isConnected) {
                Uint32 elapsed = nowMs - lastReconnectCheckMs;
                if (elapsed >= (Uint32)(autoReconnectSec * 1000.0f)) {
                    std::cout << "Auto-reconnect attempt " << (stats.reconnectAttempts + 1)
                              << "...\n";
                    ++stats.reconnectAttempts;
                    lastReconnectCheckMs = nowMs;
                    connect();
                }
            }

            // If connected, receive frames
            if (isConnected && recv) {
                // ----- Audio -----
                {
                    OMTMediaFrame* aframe = omt_receive(recv, OMTFrameType_Audio, 0);
                    if (aframe && aframe->Type == OMTFrameType_Audio) {
                        ++stats.audioFrames;
                        Uint32 thisAudioMs = nowMs;
                        if (stats.lastAudioMs != 0) {
                            double dtSec = (double)(thisAudioMs - stats.lastAudioMs) / 1000.0;
                            if (dtSec > 0.0) {
                                stats.instAudioFps = 1.0 / dtSec;
                                const double alpha = 0.1;
                                stats.avgAudioFps =
                                    (1.0 - alpha) * stats.avgAudioFps +
                                    alpha * stats.instAudioFps;
                            }
                        }
                        stats.lastAudioMs = thisAudioMs;

                        if (aframe->Codec == OMTCodec_FPA1) {
                            int sampleRate        = aframe->SampleRate;
                            int channels          = aframe->Channels;
                            int samplesPerChannel = aframe->SamplesPerChannel;
                            int totalSamples      = samplesPerChannel * channels;
                            int requiredBytes     = totalSamples * (int)sizeof(float);

                            if (!aframe->Data || sampleRate <= 0 ||
                                channels <= 0 || channels > 32 ||
                                samplesPerChannel <= 0 || totalSamples <= 0 ||
                                aframe->DataLength < requiredBytes)
                            {
                                std::cerr << "Skipping invalid audio frame.\n";
                            }
                            else {
                            if (!audioFormatSet && numDevices > 0) {
                                SDL_AudioSpec want{}, have{};
                                want.freq = sampleRate;
                                want.channels = static_cast<Uint8>(channels);
                                want.format = AUDIO_F32;
                                want.samples = 1024;
                                want.callback = nullptr;

                                const char* deviceName = nullptr;
                                if (!audioDevices.empty() &&
                                    selectedAudioIndex >= 0 &&
                                    selectedAudioIndex < (int)audioDevices.size())
                                {
                                    deviceName = audioDevices[selectedAudioIndex].c_str();
                                }

                                audioDev = SDL_OpenAudioDevice(
                                    deviceName, 0, &want, &have, 0
                                );
                                if (!audioDev) {
                                    std::cerr << "SDL_OpenAudioDevice failed: "
                                              << SDL_GetError() << "\n";
                                } else {
                                    audioFormatSet = true;
                                    SDL_PauseAudioDevice(audioDev, 0);
                                    std::cout << "Audio device opened: "
                                              << (deviceName ? deviceName : "(default)")
                                              << " | " << have.freq << " Hz, "
                                              << (int)have.channels << " ch\n";
                                }
                            }

                            if (audioFormatSet && audioDev != 0) {
                                const float* base = static_cast<const float*>(aframe->Data);
                                int planeSize = samplesPerChannel;

                                audioInterleaved.resize((size_t)totalSamples);
                                for (int s = 0; s < samplesPerChannel; ++s) {
                                    for (int ch = 0; ch < channels; ++ch) {
                                        const float* plane = base + ch * planeSize;
                                        float sample = plane[s];
                                        // Apply mixer gain / mute
                                        if (audioMute) {
                                            sample = 0.0f;
                                        } else {
                                            sample *= audioMasterGain;
                                        }
                                        audioInterleaved[(size_t)s * channels + ch] = sample;
                                    }
                                }

                                const Uint8* bytes =
                                    reinterpret_cast<const Uint8*>(audioInterleaved.data());
                                Uint32 byteCount =
                                    (Uint32)(audioInterleaved.size() * sizeof(float));

                                if (SDL_QueueAudio(audioDev, bytes, byteCount) != 0) {
                                    std::cerr << "SDL_QueueAudio failed: "
                                              << SDL_GetError() << "\n";
                                }

                                // Audio queue delay
                                Uint32 queuedBytes = SDL_GetQueuedAudioSize(audioDev);
                                double queuedSamples =
                                    (double)queuedBytes / (channels * sizeof(float));
                                double queuedSeconds =
                                    (sampleRate > 0)
                                        ? (queuedSamples / (double)sampleRate)
                                        : 0.0;
                                stats.audioQueueMs = queuedSeconds * 1000.0;

                                // If queue grows beyond maxAudioDelayMs -> clamp latency
                                if (stats.audioQueueMs > (double)maxAudioDelayMs) {
                                    std::cout << "Audio queue delay (" << stats.audioQueueMs
                                              << " ms) exceeded max allowed ("
                                              << maxAudioDelayMs
                                              << " ms). Clearing audio buffer.\n";
                                    SDL_ClearQueuedAudio(audioDev);
                                    stats.audioQueueMs = 0.0;
                                }
                            }
                            }
                        }
                    }
                }

                // ----- Video -----
                OMTMediaFrame* vframe = omt_receive(recv, OMTFrameType_Video, 0);
                if (vframe && vframe->Type == OMTFrameType_Video) {
                    ++stats.videoFrames;

                    // Save timestamp / FR for timecode & info
                    lastVideoTimestampTicks = vframe->Timestamp;
                    lastVideoFrameRateN     = vframe->FrameRateN;
                    lastVideoFrameRateD     = (vframe->FrameRateD == 0) ? 1 : vframe->FrameRateD;

                    // Local FPS / jitter
                    Uint32 thisVideoMs = nowMs;
                    if (stats.lastVideoMs != 0) {
                        double dtMs  = (double)(thisVideoMs - stats.lastVideoMs);
                        double dtSec = dtMs / 1000.0;
                        stats.lastFrameIntervalMs = dtMs;
                        if (stats.minFrameIntervalMs == 0.0 ||
                            dtMs < stats.minFrameIntervalMs)
                            stats.minFrameIntervalMs = dtMs;
                        if (dtMs > stats.maxFrameIntervalMs)
                            stats.maxFrameIntervalMs = dtMs;

                        if (dtSec > 0.0) {
                            stats.instVideoFps = 1.0 / dtSec;
                            const double alpha = 0.1;
                            stats.avgVideoFps =
                                (1.0 - alpha) * stats.avgVideoFps +
                                alpha * stats.instVideoFps;
                        }
                    } else {
                        stats.minFrameIntervalMs = 0.0;
                        stats.maxFrameIntervalMs = 0.0;
                    }
                    stats.lastVideoMs = thisVideoMs;
                    lastGoodVideoMs   = thisVideoMs;

                    // Rough local bandwidth from current frame only
                    if (vframe->DataLength > 0 && stats.lastFrameIntervalMs > 0.0) {
                        double bytesPerSec =
                            (double)vframe->DataLength *
                            (1000.0 / stats.lastFrameIntervalMs);
                        stats.approxVideoMbpsLocal =
                            (bytesPerSec * 8.0) / (1024.0 * 1024.0);
                    }

                    // Create / resize video texture & upload
                    // We handle BGRA and UYVY efficiently; others can be added if needed.
                    Uint32 desiredFormat = 0;
                    if (vframe->Codec == OMTCodec_BGRA) {
                        desiredFormat = SDL_PIXELFORMAT_BGRA32;
                    }
                    else if (vframe->Codec == OMTCodec_UYVY) {
                        desiredFormat = SDL_PIXELFORMAT_UYVY;
                    }
                    else {
                        // Unsupported for direct display right now
                        desiredFormat = 0;
                    }

                    if (desiredFormat != 0) {
                        int w = vframe->Width;
                        int h = vframe->Height;
                        int srcPitch = vframe->Stride;
                        bool validVideoFrame =
                            vframe->Data && vframe->DataLength > 0 &&
                            w > 0 && h > 0 && srcPitch > 0;

                        if (!validVideoFrame) {
                            std::cerr << "Skipping invalid video frame.\n";
                        }
                        else if (!videoTexture ||
                            w != texWidth || h != texHeight ||
                            desiredFormat != texPixelFormat)
                        {
                            if (videoTexture) SDL_DestroyTexture(videoTexture);
                            texWidth       = w;
                            texHeight      = h;
                            texPixelFormat = desiredFormat;

                            videoTexture = SDL_CreateTexture(
                                videoRenderer,
                                texPixelFormat,
                                SDL_TEXTUREACCESS_STREAMING,
                                texWidth, texHeight
                            );
                            if (!videoTexture) {
                                std::cerr << "SDL_CreateTexture failed: "
                                          << SDL_GetError() << "\n";
                            } else {
                                std::cout << "Created video texture "
                                          << texWidth << "x" << texHeight
                                          << " fmt=" << texPixelFormat << "\n";
                            }
                        }

                        if (validVideoFrame && videoTexture) {
                            Uint32 uploadStart = SDL_GetTicks();

                            void* pixels = nullptr;
                            int pitch = 0;
                            if (SDL_LockTexture(videoTexture, nullptr, &pixels, &pitch) == 0) {
                                const uint8_t* src =
                                    static_cast<const uint8_t*>(vframe->Data);
                                uint8_t* dst = static_cast<uint8_t*>(pixels);

                                // For both BGRA32 and UYVY, we can treat as raw bytes
                                int rowBytes = std::min(srcPitch, pitch);
                                int copyRows = std::min(
                                    h,
                                    vframe->DataLength / srcPitch);

                                for (int y = 0; y < copyRows; ++y) {
                                    std::memcpy(dst + y * pitch,
                                                src + y * srcPitch,
                                                rowBytes);
                                }

                                SDL_UnlockTexture(videoTexture);
                            }

                            Uint32 uploadEnd = SDL_GetTicks();
                            double uploadMs =
                                (double)(uploadEnd - uploadStart);
                            if (uploadMs > stats.maxUploadMs)
                                stats.maxUploadMs = uploadMs;
                        }
                    }
                }

                // Lost-stream detection: if connected but no video for a while => treat as disconnect
                if (isConnected && lastGoodVideoMs != 0) {
                    const Uint32 LOST_STREAM_MS = 5000; // 5s without video
                    if (nowMs - lastGoodVideoMs > LOST_STREAM_MS) {
                        std::cerr << "No video for " << LOST_STREAM_MS
                                  << " ms, marking as disconnected.\n";
                        disconnect();
                        lastReconnectCheckMs = nowMs; // so auto-reconnect kicks in from now
                    }
                }
            }

            // Query OMT statistics occasionally
            if (recv && isConnected) {
                Uint32 sinceStats = nowMs - lastOmtStatsMs;
                if (sinceStats > 500) { // ~2Hz
                    lastOmtStatsMs = nowMs;
                    std::memset(&videoStats, 0, sizeof(videoStats));
                    std::memset(&audioStats, 0, sizeof(audioStats));

                    omt_receive_getvideostatistics(recv, &videoStats);
                    omt_receive_getaudiostatistics(recv, &audioStats);

                    // BytesReceivedSinceLast is bytes since last call
                    double dtSec = sinceStats / 1000.0;
                    if (dtSec > 0.0 && videoStats.BytesReceivedSinceLast > 0) {
                        double bytesPerSec =
                            (double)videoStats.BytesReceivedSinceLast / dtSec;
                        stats.approxVideoMbpsOmt =
                            (bytesPerSec * 8.0) / (1024.0 * 1024.0);
                    }
                }
            }

            // -------------------------
            // Render video window
            // -------------------------
            SDL_SetRenderDrawColor(videoRenderer, 0, 0, 0, 255);
            SDL_RenderClear(videoRenderer);

            if (videoTexture) {
                int winW, winH;
                SDL_GetWindowSize(videoWindow, &winW, &winH);

                float srcAspect = (texHeight > 0)
                    ? (float)texWidth / (float)texHeight
                    : 1.0f;
                float winAspect = (winH > 0)
                    ? (float)winW / (float)winH
                    : 1.0f;

                SDL_Rect dstRect{};
                if (winAspect > srcAspect) {
                    dstRect.h = winH;
                    dstRect.w = (int)(winH * srcAspect);
                    dstRect.x = (winW - dstRect.w) / 2;
                    dstRect.y = 0;
                } else {
                    dstRect.w = winW;
                    dstRect.h = (int)(winW / srcAspect);
                    dstRect.x = 0;
                    dstRect.y = (winH - dstRect.h) / 2;
                }

                SDL_RenderCopy(videoRenderer, videoTexture, nullptr, &dstRect);
            }

            SDL_RenderPresent(videoRenderer);

            // -------------------------
            // ImGui frame (Control)
            // -------------------------
            ImGui_ImplSDL2_NewFrame();
            ImGui_ImplSDLRenderer2_NewFrame();
            ImGui::NewFrame();

            // Snap ImGui window to full control window size
            ImGui::SetNextWindowPos(ImVec2(0, 0));
            ImGui::SetNextWindowSize(io.DisplaySize);

            ImGuiWindowFlags windowFlags =
                ImGuiWindowFlags_NoResize |
                ImGuiWindowFlags_NoMove;

            ImGui::Begin(kControlWindowTitle, nullptr, windowFlags);

            ImGui::TextDisabled("Version %s", kAppVersion);
            ImGui::SameLine();
            if (ImGui::Button("About")) {
                ImGui::OpenPopup("About OMT Decoder");
            }
            ImGui::SameLine();
            if (ImGui::Button("Support")) {
                OpenSupportPage();
            }

            if (ImGui::BeginPopupModal("About OMT Decoder", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("Boiling Head Media OMT Decoder");
                ImGui::Text("Version %s", kAppVersion);
                ImGui::Separator();
                ImGui::TextWrapped(
                    "Receives OMT sources and provides video monitoring, audio output, "
                    "statistics, latency tools, and discovery controls.");
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Open Source Credits");
                ImGui::BulletText("Built with Open Media Transport (OMT)");
                ImGui::BulletText("Uses Dear ImGui for the control interface");
                ImGui::BulletText("Uses SDL2 for windows, rendering, audio, and URL opening");
                ImGui::BulletText("OMT project: %s", kOmtProjectUrl);
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Licensing");
                ImGui::TextWrapped(
                    "Open Media Transport is free, open source, royalty-free, and MIT licensed. "
                    "Dear ImGui is MIT licensed. SDL2 is provided under the zlib license. "
                    "Third-party components remain copyright of their respective authors and "
                    "are distributed under their own license terms.");
                ImGui::Spacing();
                if (ImGui::Button("Open OMT GitHub")) {
                    OpenOmtProjectPage();
                }
                ImGui::SameLine();
                if (ImGui::Button("Support Developer")) {
                    OpenSupportPage();
                }
                ImGui::SameLine();
                if (ImGui::Button("Close")) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }

            ImGui::Separator();

            // Connection
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "Connection");

            const char* currentSource =
                (selectedDiscoveryIndex >= 0 &&
                 selectedDiscoveryIndex < (int)discoveredSources.size())
                    ? discoveredSources[selectedDiscoveryIndex].c_str()
                    : (discoveredSources.empty() ? "No OMT sources found" : discoveredSources[0].c_str());

            if (ImGui::BeginCombo("Available OMT Sources", currentSource)) {
                for (int i = 0; i < (int)discoveredSources.size(); ++i) {
                    bool isSelected = (selectedDiscoveryIndex == i);
                    if (ImGui::Selectable(discoveredSources[i].c_str(), isSelected)) {
                        selectedDiscoveryIndex = i;
                        switch_to_source(discoveredSources[i]);
                    }
                    if (isSelected)
                        ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("Auto-scan sources", &autoScanSources);
            ImGui::SameLine();
            ImGui::BeginDisabled(discoveryInProgress);
            if (ImGui::Button("Scan Now")) {
                start_discovery_scan();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImVec4 discoveryColor = discoveredSources.empty()
                ? ImVec4(1.0f, 0.55f, 0.45f, 1.0f)
                : ImVec4(0.55f, 1.0f, 0.65f, 1.0f);
            ImGui::TextColored(discoveryColor, "%s", discoveryStatus.c_str());

            bool hasSelectedSource =
                selectedDiscoveryIndex >= 0 &&
                selectedDiscoveryIndex < (int)discoveredSources.size() &&
                urlBuffer[0] != '\0';
            ImGui::BeginDisabled(!hasSelectedSource && !isConnected);
            if (ImGui::Button(isConnected ? "Disconnect" : "Connect")) {
                if (!isConnected) {
                    connect();
                } else {
                    disconnect();
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(!isConnected);
            if (ImGui::Button("Reset")) {
                if (isConnected) {
                    std::cout << "Resetting connection (disconnect + reconnect)...\n";
                    disconnect();
                    connect();
                } else {
                    std::cout << "Reset pressed while disconnected (no-op).\n";
                }
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Refresh")) {
                if (isConnected) {
                    std::cout << "Refreshing active connection...\n";
                    disconnect();
                    connect();
                } else if (!discoveryInProgress) {
                    start_discovery_scan();
                }
            }
            ImGui::TextColored(
                isConnected ? ImVec4(0.55f, 1.0f, 0.65f, 1.0f)
                            : ImVec4(0.85f, 0.85f, 0.85f, 1.0f),
                "%s",
                connectionStatus.c_str());

            ImGui::Separator();

            // Video format switch (using OMTPreferredVideoFormat enums)
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "Preferred Video Format");
            const char* fmtLabels[] = {
                "UYVY (fastest, no alpha)",
                "UYVY or BGRA (BGRA if alpha)",
                "BGRA (always BGRA)",
                "UYVY or UYVA (alpha)",
                "UYVY/UYVA or P216/PA16 (hi-bit-depth)",
                "P216 (hi-bit-depth YUV)"
            };
            int fmtCount = 6;
            int fmtIndex = preferredVideoFormat;
            if (fmtIndex < 0 || fmtIndex >= fmtCount) fmtIndex = OMTPreferredVideoFormat_UYVYorBGRA;

            if (ImGui::Combo("Format", &fmtIndex, fmtLabels, fmtCount)) {
                preferredVideoFormat = fmtIndex;
                settings.videoFormat = fmtIndex;
                // New connections will use this format; we do not reconnect immediately.
            }
            ImGui::TextWrapped(
                "UYVY-based formats are generally the fastest on CPU.\n"
                "This viewer uploads UYVY and BGRA directly to GPU textures.");

            ImGui::Separator();

            // Audio output + mixer
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.4f, 1.0f), "Audio Output");
            if (!audioDevices.empty()) {
                const char* preview = audioDevices[selectedAudioIndex].c_str();
                if (ImGui::BeginCombo("Device", preview)) {
                    for (int n = 0; n < (int)audioDevices.size(); ++n) {
                        bool isSelected = (selectedAudioIndex == n);
                        if (ImGui::Selectable(audioDevices[n].c_str(), isSelected)) {
                            selectedAudioIndex = n;
                            settings.audioIndex = n;
                            close_audio_device();
                        }
                        if (isSelected)
                            ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            } else {
                ImGui::TextColored(ImVec4(1,0,0,1), "No audio devices found");
            }

            ImGui::Separator();
            ImGui::Text("Audio Mixer");
            ImGui::SliderFloat("Master Gain", &audioMasterGain, 0.0f, 3.0f, "%.2f");
            ImGui::SameLine();
            ImGui::Checkbox("Mute", &audioMute);
            ImGui::SliderFloat("Max Audio Delay (ms)", &maxAudioDelayMs, 10.0f, 1000.0f, "%.0f");
            ImGui::Text("Current audio queue delay: %.1f ms", stats.audioQueueMs);

            ImGui::Separator();

            // Display / fullscreen
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 1.0f, 1.0f), "Display");
            bool fullscreenCheckbox = isFullscreen;
            if (ImGui::Checkbox("Fullscreen (Video Window)", &fullscreenCheckbox)) {
                isFullscreen = fullscreenCheckbox;
                settings.fullscreen = isFullscreen;
                if (isFullscreen)
                    SDL_SetWindowFullscreen(videoWindow, SDL_WINDOW_FULLSCREEN_DESKTOP);
                else
                    SDL_SetWindowFullscreen(videoWindow, 0);
            }

            ImGui::Separator();

            // Auto-reconnect
            ImGui::TextColored(ImVec4(0.9f, 0.7f, 0.3f, 1.0f), "Auto-Reconnect");
            ImGui::Checkbox("Enable Auto-Reconnect", &autoReconnect);
            ImGui::SliderFloat("Interval (seconds)", &autoReconnectSec, 1.0f, 30.0f, "%.1f");
            if (autoReconnect) {
                float secondsSinceLastCheck = (nowMs - lastReconnectCheckMs) / 1000.0f;
                float nextRetryIn = std::max(0.0f, autoReconnectSec - secondsSinceLastCheck);
                ImGui::Text("Next retry in ~%.1f s", nextRetryIn);
            }
            ImGui::Text("Reconnect attempts: %u", stats.reconnectAttempts);

            ImGui::Separator();

            // Statistics HUD & latency tools toggles
            ImGui::Checkbox("Show Statistics HUD", &showStatsHud);
            ImGui::SameLine();
            ImGui::Checkbox("Show Latency Tools", &showLatencyTools);

            if (showStatsHud) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.9f, 1.0f), "Statistics (Local + OMT)");

                Uint32 uptimeMs = nowMs - appStartMs;
                ImGui::Text("Uptime: %.1f s", uptimeMs / 1000.0f);

                ImGui::Text("Video frames (local): %" PRIu64, stats.videoFrames);
                ImGui::Text("Audio frames (local): %" PRIu64, stats.audioFrames);

                ImGui::Text("Video FPS: avg %.2f  (inst %.2f)",
                            stats.avgVideoFps, stats.instVideoFps);
                ImGui::Text("Audio FPS: avg %.2f  (inst %.2f)",
                            stats.avgAudioFps, stats.instAudioFps);

                ImGui::Text("Last video interval: %s",
                            FormatMs(stats.lastFrameIntervalMs).c_str());
                ImGui::Text("Frame interval range: min %s  /  max %s",
                            FormatMs(stats.minFrameIntervalMs).c_str(),
                            FormatMs(stats.maxFrameIntervalMs).c_str());

                ImGui::Text("Approx video bandwidth (local): %.2f Mbit/s",
                            stats.approxVideoMbpsLocal);
                ImGui::Text("Approx video bandwidth (OMT stats): %.2f Mbit/s",
                            stats.approxVideoMbpsOmt);

                // Show OMT statistics details
                ImGui::Separator();
                ImGui::Text("OMT Video Stats:");
                ImGui::Text("  BytesReceived:        %" PRId64, videoStats.BytesReceived);
                ImGui::Text("  BytesReceivedSinceLast: %" PRId64, videoStats.BytesReceivedSinceLast);
                ImGui::Text("  Frames:              %" PRId64, videoStats.Frames);
                ImGui::Text("  FramesSinceLast:     %" PRId64, videoStats.FramesSinceLast);
                ImGui::Text("  FramesDropped:       %" PRId64, videoStats.FramesDropped);
                ImGui::Text("  CodecTime (ms):      %" PRId64, videoStats.CodecTime);
                ImGui::Text("  CodecTimeSinceLast:  %" PRId64, videoStats.CodecTimeSinceLast);

                ImGui::Text("OMT Audio Stats:");
                ImGui::Text("  BytesReceived:        %" PRId64, audioStats.BytesReceived);
                ImGui::Text("  Frames:              %" PRId64, audioStats.Frames);
            }

            // Timecode overlay
            ImGui::Separator();
            ImGui::Checkbox("Show Timecode", &showTimecode);
            if (showTimecode) {
                std::string tc = FormatOmtTimestamp(lastVideoTimestampTicks);
                ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f),
                                   "Last video timestamp: %s", tc.c_str());
                double nominalFps = 0.0;
                if (lastVideoFrameRateD != 0)
                    nominalFps = (double)lastVideoFrameRateN / (double)lastVideoFrameRateD;
                ImGui::Text("Nominal sender framerate: %.3f fps", nominalFps);
            }

            // Low-latency analysis
            if (showLatencyTools) {
                ImGui::Separator();
                ImGui::TextColored(ImVec4(0.9f, 0.5f, 0.5f, 1.0f), "Low-Latency Analysis (local)");

                if (stats.lastFrameIntervalMs > 0.0) {
                    ImGui::Text("Inter-frame time: %s",
                                FormatMs(stats.lastFrameIntervalMs).c_str());
                } else {
                    ImGui::Text("Inter-frame time: N/A (waiting for frames)");
                }

                ImGui::Text("Audio queue delay: %.1f ms", stats.audioQueueMs);

                double approxEndToEndMs = stats.audioQueueMs + stats.lastFrameIntervalMs;
                ImGui::Text("Approx AV end-to-end (very rough): %.1f ms", approxEndToEndMs);

                if (stats.lastVideoMs != 0) {
                    ImGui::Text("Time since last video frame: %.1f ms",
                                (double)(nowMs - stats.lastVideoMs));
                }
            }

            // Time sync / PTP-ish info (no explicit PTP, but show relative timing)
            ImGui::Separator();
            ImGui::Checkbox("Show Time Sync / PTP Info (local estimate)", &showSyncInfo);
            if (showSyncInfo) {
                ImGui::TextWrapped(
                    "This is a local time-sync estimate based on OMT timestamps.\n"
                    "True PTP integration would require separate clock sync APIs.");
                if (lastVideoTimestampTicks > 0) {
                    std::string tc = FormatOmtTimestamp(lastVideoTimestampTicks);
                    ImGui::Text("Stream time (from OMT timestamp): %s", tc.c_str());

                    // Compare to local app uptime
                    double streamMs = TicksToMs(lastVideoTimestampTicks);
                    double appMs    = (double)(nowMs - appStartMs);
                    double diffMs   = appMs - streamMs;
                    ImGui::Text("Local app time: %.0f ms since start", appMs);
                    ImGui::Text("Stream time:    %.0f ms since source", streamMs);
                    ImGui::Text("Local - Stream: %.0f ms (sign indicates lead/lag)", diffMs);
                } else {
                    ImGui::Text("No stream timestamp received yet.");
                }
            }

            // GPU stats (upload timing only)
            ImGui::Separator();
            ImGui::Checkbox("Show GPU Stats (upload timing)", &showGpuStats);
            if (showGpuStats) {
                ImGui::TextWrapped(
                    "Currently measuring CPU-side texture upload time only.\n"
                    "This is still useful to see if uploads spike on heavy frames.");
                ImGui::Text("Max upload time recorded: %s",
                            FormatMs(stats.maxUploadMs).c_str());
            }

            ImGui::Separator();
            ImGui::Text("Status: %s", isConnected ? "Connected" : "Disconnected");

            ImGui::End();

            // Render control window
            SDL_SetRenderDrawColor(ctrlRenderer, 20, 20, 20, 255);
            SDL_RenderClear(ctrlRenderer);

            ImGui::Render();
            ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ctrlRenderer);
            SDL_RenderPresent(ctrlRenderer);
        }

        // Save settings
        SaveSettings(settings);

        // Cleanup
        if (videoTexture) SDL_DestroyTexture(videoTexture);
        close_audio_device();
        if (recv) omt_receive_destroy(recv);

        ImGui_ImplSDLRenderer2_Shutdown();
        ImGui_ImplSDL2_Shutdown();
        ImGui::DestroyContext();

        SDL_DestroyRenderer(ctrlRenderer);
        SDL_DestroyWindow(ctrlWindow);

        SDL_DestroyRenderer(videoRenderer);
        SDL_DestroyWindow(videoWindow);

        return 0;
    }
    catch (const std::exception& ex) {
        std::cerr << "Fatal error: " << ex.what() << "\n";
        return 1;
    }
}
