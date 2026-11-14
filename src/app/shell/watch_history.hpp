#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace playtube::app {

class AppSession;

// One report of watching, as YouTube's apps send them: `start` once (the video goes into the account's history),
// then the parts watched, `from` to `to` in seconds (the position YouTube resumes from on the account's other
// devices).
struct WatchReport {
    bool start = false;
    double from = 0;
    double to = 0;
    bool paused = false;
};

// When to report (pure: the player's positions in, the reports out). The start at the first position; a part
// after 30 s of it, when the video pauses, when it jumps (a seek: the part before the jump), and at flush() (the
// player closes or hides). A live video gets its start only.
class WatchSegments {
public:
    static constexpr double kPart = 30;  // seconds of video a part covers at most
    static constexpr double kJump = 5;   // a position this far from the last one: a seek

    explicit WatchSegments(bool live = false) : live_(live) {}
    void setLive(bool live) { live_ = live; }
    std::vector<WatchReport> update(double position, bool paused);
    std::optional<WatchReport> flush();

private:
    std::optional<WatchReport> part(bool paused);

    bool live_ = false;
    bool started_ = false;
    bool paused_ = false;
    double from_ = 0;
    double last_ = 0;
};

// Sends a video's reports for the signed-in account (setting "Historique YouTube"): the addresses asked once from
// YouTube (Client::playbackTracking), then each report on the session's pool. The jobs are not cancelled with the
// player: the last part goes out after it closed. A failure is logged and the rest are dropped (no retries).
class WatchHistory {
public:
    WatchHistory(AppSession& session, std::string videoId);
    void setLive(bool live) { segments_.setLive(live); }
    void update(double position, bool paused);
    void flush();

private:
    struct Shared;
    void send(const WatchReport& report);

    AppSession& session_;
    std::string videoId_;
    WatchSegments segments_;
    std::shared_ptr<Shared> shared_;
};

}  // namespace playtube::app
