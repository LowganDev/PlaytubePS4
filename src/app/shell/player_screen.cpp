#include <nanovg.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/shell/chrome.hpp"
#include "app/shell/clock.hpp"
#include "app/shell/trickplay_preview.hpp"
#include "app/shell/watch_history.hpp"
#include "youtube/storyboard.hpp"
#include "app/shell/chat_panel.hpp"
#include "app/shell/comments_panel.hpp"
#include "app/shell/screen.hpp"
#include "app/shell/screens.hpp"
#include "core/log.hpp"
#include "core/utf.hpp"
#include "platform/platform.hpp"
#include "player/engine.hpp"
#include "player/seek.hpp"
#include "ui/context.hpp"
#include "ui/draw.hpp"
#include "ui/i18n.hpp"
#include "ui/image_view.hpp"
#include "ui/label.hpp"
#include "ui/theme.hpp"
#include "ui/video_source.hpp"
#include "ui/widgets.hpp"
#include "youtube/playback.hpp"

namespace playtube::app {

namespace {

constexpr std::int64_t kBarMs = 4000;        // the bar hides after 4 s without input
constexpr std::int64_t kHoldGapMs = 300;     // presses closer than this are one held button
constexpr double kNextAheadSeconds = 10;     // "À suivre" shows 10 s before the end
constexpr double kMinDurationForNext = 30;   // not for very short videos
const ui::Color kSegmentColor{0xFF, 0xD4, 0x00, 0xFF};
const int kHeights[] = {360, 480, 720, 1080};

// The progress bar with the SponsorBlock segments drawn over it.
class SegmentBar : public ui::View {
public:
    SegmentBar() { height = ui::SizeSpec::fixed(8); }
    void set(double position, double duration, const std::vector<youtube::Segment>& segments) {
        if (position == position_ && duration == duration_ && segments.size() == segments_.size()) {
            return;
        }
        position_ = position;
        duration_ = duration;
        segments_ = segments;
        markDirty();
    }
    // where chapters begin: a gap in the bar at each (none at 0)
    void setChapters(std::vector<double> starts) {
        chapters_ = std::move(starts);
        markDirty();
    }
    void draw(NVGcontext* vg) const override {
        const float radius = frame.height / 2;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, 0, frame.width, frame.height, radius);
        nvgFillColor(vg, ui::nvgColor(ui::withAlpha(ui::theme().text, 0x40)));
        nvgFill(vg);
        if (duration_ <= 0) {
            return;
        }
        const auto at = [this](double seconds) {
            return static_cast<float>(std::min(1.0, std::max(0.0, seconds / duration_))) * frame.width;
        };
        for (const youtube::Segment& segment : segments_) {
            nvgBeginPath(vg);
            nvgRect(vg, at(segment.start), 0, std::max(2.0f, at(segment.end) - at(segment.start)), frame.height);
            nvgFillColor(vg, ui::nvgColor(kSegmentColor));
            nvgFill(vg);
        }
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, 0, at(position_), frame.height, radius);
        nvgFillColor(vg, ui::nvgColor(ui::theme().accent));
        nvgFill(vg);
        // chapters: a dark notch where each one begins (the bar reads as parts, as on YouTube)
        for (double start : chapters_) {
            if (start <= 0 || start >= duration_) {
                continue;
            }
            nvgBeginPath(vg);
            nvgRect(vg, at(start) - 2, 0, 4, frame.height);
            nvgFillColor(vg, nvgRGBA(0x10, 0x10, 0x10, 0xE0));
            nvgFill(vg);
        }
    }

private:
    double position_ = -1;
    double duration_ = 0;
    std::vector<youtube::Segment> segments_;
    std::vector<double> chapters_;
};

struct Extras {
    core::Result<youtube::NextReply> next = core::makeError(core::ErrorKind::Cancelled, "");
    std::vector<youtube::Segment> segments;
    std::optional<youtube::VideoState> state;  // signed in: the account's rating and subscription
    std::optional<youtube::Votes> votes;       // Return YouTube Dislike's, when on
};

player::EngineOptions engineOptions(const AppSession& session, const std::string& userAgent) {
    const std::string resources = platform::resourceDir();
    player::EngineOptions options;
#ifdef PLAYTUBE_PS4
    options.audioOutput = "sdl";
    options.decoderThreads = 4;  // of the 6 cores an app gets (the UI uses the others)
#endif
    if (session.synchronous) {
        options.audioOutput = "null";  // tests
    }
    options.fontsDir = resources + "/fonts";
    options.caFile = session.caFile();
    options.userAgent = userAgent;
    options.subtitleScale = subtitleScale(session.settings().subtitleSize);
    options.subtitleBackground = session.settings().subtitleBackground;
    return options;
}

// "521 783": a count with the language's grouping of thousands
std::string groupedNumber(std::int64_t value) {
    const std::string digits = std::to_string(value < 0 ? 0 : value);
    const std::string separator = ui::tr("number.group");
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i > 0 && (digits.size() - i) % 3 == 0) {
            out += separator;
        }
        out += digits[i];
    }
    return out;
}

// "21:47": when a video ends if it plays on from `position` at `speed` (the console's local time)
std::string endsAtText(const AppSession& session, double duration, double position, double speed) {
    const double left = (duration - position) / (speed > 0 ? speed : 1.0);
    const std::int64_t end = session.clockNow() + static_cast<std::int64_t>(left > 0 ? left + 0.5 : 0);
    return clock24(end, session.utcOffsetMinutes(end));
}

class PlayerScreen : public ShellScreen, public ui::VideoSource {
public:
    PlayerScreen(AppSession& session, std::string videoId, Queue queue, std::size_t index, bool shorts)
        : ShellScreen(session),
          videoId_(std::move(videoId)),
          maxHeight_(session.settings().maxHeight),
          queue_(std::move(queue)),
          shorts_(shorts),
          index_(index) {
        // from a playlist: its next video comes after this one (else the related videos); Shorts loop instead
        for (std::size_t i = index + 1; !shorts_ && queue_ && i < queue_->size(); ++i) {
            const youtube::Video& video = (*queue_)[i];
            if (!video.live && !video.id.empty() && (session.settings().shorts || !video.isShort)) {
                nextVideo_ = video;
                queueNext_ = i;
                break;
            }
        }
        // where it was left, unless barely started or watched to its end
        if (const auto progress = session.progress().find(videoId_)) {
            lastPosition_ = static_cast<double>(ProgressStore::resumeAt(*progress));
            resumedFrom_ = lastPosition_;
        }
        if (session.account().signedIn() && session.settings().watchHistory) {  // into the account's history
            history_ = std::make_unique<WatchHistory>(session, videoId_);
        }
        focusable = true;
        buildOverlay();
        bindControls();
    }
    ~PlayerScreen() override { release(); }

    void onShow() override {
        if (ui::Application* app = ui::Application::current()) {
            app->setVideoSource(this);
        }
        shown_ = true;
        if (!started_ || !engine_) {  // the first time, or back after something hid it (its engine went)
            started_ = true;
            showBar();
            start(lastPosition_);
        }
    }
    void onHide() override {
        shown_ = false;
        release();
    }

    bool renderVideo() override {
        if (!engine_) {
            return false;
        }
        for (player::EngineEvent event : engine_->drainEvents()) {
            onEngineEvent(event);
            if (!engine_) {
                return false;
            }
        }
        const bool fresh = engine_->renderIfDue(frame_);
        tick(fresh);
        return fresh;
    }

    platform::VideoLayer videoLayer() const override {
        platform::VideoLayer layer{frame_.pixels.empty() ? nullptr : frame_.pixels.data(), frame_.width,
                                   frame_.height, false};
        const std::string& fit = session_.settings().pictureFit;
        layer.fit = fit == "fill"      ? platform::VideoFit::Fill
                    : fit == "stretch" ? platform::VideoFit::Stretch
                                       : platform::VideoFit::Fit;
        layer.scale = session_.settings().pictureSize / 100.0;
        return layer;
    }

private:
    static std::int64_t now() { return static_cast<std::int64_t>(ui::context().timeMs); }

    // ---- overlay

    void buildOverlay() {
        auto* middle = add<ui::Stack>();
        middle->width = ui::SizeSpec::fill();
        middle->height = ui::SizeSpec::fill();
        middle->horizontal = ui::CrossAlign::Center;
        middle->vertical = ui::CrossAlign::Center;
        spinner_ = middle->add<ui::Spinner>();

        auto* bottom = add<ui::Box>(ui::Axis::Column);
        bottom->width = ui::SizeSpec::fill();
        bottom->height = ui::SizeSpec::fill();
        bottom->align = ui::Align::End;
        bottom->padding = {0, 96, 64, 96};
        bottom->gap = ui::theme().spacingL;

        // "À suivre": the next video, its picture, title and countdown, at the right
        auto* nextRow = bottom->add<ui::Box>(ui::Axis::Row);
        nextRow->width = ui::SizeSpec::fill();
        nextRow->align = ui::Align::End;
        next_ = nextRow->add<ui::Box>(ui::Axis::Row);
        next_->background = ui::withAlpha(ui::theme().surfaceRaised, 0xE6);
        next_->cornerRadius = ui::theme().radius;
        next_->padding = ui::Insets::all(ui::theme().spacingM);
        next_->gap = ui::theme().spacingM;
        next_->crossAlign = ui::CrossAlign::Center;
        next_->visible = false;
        nextImage_ = next_->add<ui::ImageView>();
        nextImage_->width = ui::SizeSpec::fixed(256);
        nextImage_->height = ui::SizeSpec::fixed(144);
        nextImage_->cornerRadius = 8;
        nextImage_->clip = true;
        auto* nextTexts = next_->add<ui::Box>(ui::Axis::Column);
        nextTexts->gap = ui::theme().spacingS;
        auto* nextHeading = nextTexts->add<ui::Label>(ui::tr("player.next"), bodyStrong());
        nextHeading->color = ui::theme().textSecondary;
        nextTitle_ = nextTexts->add<ui::Label>("", bodyStrong());
        nextTitle_->width = ui::SizeSpec::fixed(460);
        nextTitle_->maxLines = 2;
        nextCountdown_ = nextTexts->add<ui::Label>("", bodyText());
        nextCountdown_->color = ui::theme().textSecondary;

        menu_ = bottom->add<ui::Box>(ui::Axis::Column);
        menu_->gap = ui::theme().spacingM;
        menu_->visible = false;
        actions_ = menu_->add<ui::Box>(ui::Axis::Row);  // about the video
        actions_->gap = ui::theme().spacingM;
        actions_->crossAlign = ui::CrossAlign::Center;
        playback_ = menu_->add<ui::Box>(ui::Axis::Row);  // about the playback
        playback_->gap = ui::theme().spacingM;

        bar_ = bottom->add<ui::Box>(ui::Axis::Column);
        bar_->width = ui::SizeSpec::fill();
        bar_->background = ui::withAlpha(ui::theme().surfaceRaised, 0xD9);
        bar_->cornerRadius = ui::theme().radius;
        bar_->padding = ui::Insets::all(ui::theme().spacingL);
        bar_->gap = ui::theme().spacingM;
        title_ = bar_->add<ui::Label>("", titleText(ui::FontWeight::SemiBold));
        title_->width = ui::SizeSpec::fill();
        channel_ = bar_->add<ui::Label>("", bodyText());
        channel_->color = ui::theme().textSecondary;
        progress_ = bar_->add<SegmentBar>();
        progress_->width = ui::SizeSpec::fill();
        auto* times = bar_->add<ui::Box>(ui::Axis::Row);
        times->gap = ui::theme().spacingM;
        times->crossAlign = ui::CrossAlign::Center;
        live_ = times->add<ui::Label>(ui::tr("video.live"),
                                      ui::FontStyle{ui::theme().fontSmall, ui::FontWeight::SemiBold});
        live_->background = ui::Color{0xCC, 0x00, 0x00, 0xFF};
        live_->cornerRadius = 6;
        live_->padding = {2, 8, 2, 8};
        live_->visible = false;
        time_ = times->add<ui::Label>("", bodyText());
        time_->color = ui::theme().textSecondary;
        preview_ = add<TrickplayPreview>();  // over the bar
        preview_->setAnchor(progress_, bar_);
    }

    void showBar() {
        barUntil_ = now() + kBarMs;
        if (!bar_->visible) {
            bar_->visible = true;
            markDirty();
        }
    }

    void refreshBar(const player::Engine::State& state) {
        if (streams_.live) {
            progress_->visible = false;
            time_->visible = false;
            return;
        }
        const double position = seek_.pending() ? seek_.target() : state.position;
        std::string text = clockText(position) + " / " + clockText(state.duration);
        if (const int chapter = chapterAt(position); chapter >= 0) {  // the part it is in
            text += " \xC2\xB7 " + core::withoutEmoji(chapters_[static_cast<std::size_t>(chapter)].title);
        }
        if (state.duration > 0) {  // when it would end, playing on from here
            text += " \xC2\xB7 " +
                    ui::tr("player.ends_at", {endsAtText(session_, state.duration, position, speed_)});
        }
        if (text != time_->text()) {
            time_->setText(text);
        }
        preview_->update(seek_.pending(), position, state.duration, lastDirection_, clockText(position));
        progress_->set(position, state.duration, segments_);
    }

    // ---- the menu (↑ or ↓): the video's row (J'aime, Je n'aime pas, S'abonner, Commentaires, Enregistrer,
    // Chaîne) above the playback's (Pause/Lecture, Qualité, Vitesse, Audio, Sous-titres)

    void openMenu() {
        if (!engine_) {
            return;
        }
        actions_->clearChildren();
        playback_->clearChildren();
        like_ = dislike_ = subscribe_ = nullptr;
        const bool signedIn = session_.account().signedIn();
        if (signedIn) {
            like_ = actions_->add<ui::Button>(ui::tr("player.like"), [this] { later([this] { rate(true); }); });
            dislike_ = actions_->add<ui::Button>(ui::tr("player.dislike"), [this] { later([this] { rate(false); }); });
            if (!channelId().empty()) {
                subscribe_ = actions_->add<ui::Button>(ui::tr("channel.subscribe"),
                                                       [this] { later([this] { toggleSubscription(); }); });
            }
        }
        votesLabel_ = nullptr;
        if (!signedIn) {
            votesLabel_ = actions_->add<ui::Label>("", bodyText());
            votesLabel_->color = ui::theme().text;  // on a chip of its own: readable over any picture
            votesLabel_->background = ui::theme().control;
            votesLabel_->cornerRadius = ui::theme().radius;
            votesLabel_->padding = {ui::theme().spacingM - 2, ui::theme().spacingL - 4, ui::theme().spacingM - 2,
                                    ui::theme().spacingL - 4};
            votesLabel_->visible = false;
        }
        if (streams_.live && !liveChat_.empty()) {
            actions_->add<ui::Button>(ui::tr("player.live_chat"), [this] { later([this] { openLiveChat(); }); });
        }
        actions_->add<ui::Button>(ui::tr("player.comments"), [this] { later([this] { openComments(); }); });
        if (signedIn) {
            actions_->add<ui::Button>(ui::tr("player.save"), [this] { later([this] { save(); }); });
        }
        if (!channelId().empty()) {
            actions_->add<ui::Button>(ui::tr("video.channel"), [this] { later([this] { openChannelHere(); }); });
        }
        refreshActions();
        const bool paused = engine_->state().paused;
        auto* first = playback_->add<ui::Button>(ui::tr(paused ? "player.play" : "player.pause"),
                                                 [this] { later([this] { togglePause(); openMenu(); }); });
        if (!streams_.live) {
            playback_->add<ui::Button>(ui::tr("player.quality"), [this] { later([this] { chooseQuality(); }); });
            playback_->add<ui::Button>(ui::tr("player.speed"), [this] { later([this] { chooseSpeed(); }); });
        }
        if (streams_.audioTracks.size() > 1) {
            playback_->add<ui::Button>(ui::tr("player.audio"), [this] { later([this] { chooseAudio(); }); });
        }
        if (!streams_.captions.empty()) {
            playback_->add<ui::Button>(ui::tr("player.subtitles"), [this] { later([this] { chooseSubtitles(); }); });
        }
        if (!streams_.live && resumedFrom_ > 0) {  // it resumed where it was left: from the start instead
            playback_->add<ui::Button>(ui::tr("player.from_start"), [this] {
                later([this] {
                    if (engine_) {
                        engine_->seekTo(0);
                    }
                    closeMenu();
                });
            });
        }
        if (!session_.watchQueue().empty()) {
            playback_->add<ui::Button>(ui::tr("queue.button", {std::to_string(session_.watchQueue().size())}),
                                       [this] { later([this] { showQueue(); }); });
        }
        playback_->add<ui::Button>(ui::tr("player.volume"), [this] { later([this] { chooseVolume(); }); });
        playback_->add<ui::Button>(ui::tr("player.picture"), [this] { later([this] { choosePicture(); }); });
        menu_->visible = true;
        showBar();
        if (ui::Application* app = ui::Application::current()) {
            app->focus().focus(first);
        }
        markDirty();
    }

    // the rating and the subscription YouTube confirmed: the active rating is filled with the accent
    void refreshActions() {
        const youtube::Rating rating = state_ ? state_->rating : youtube::Rating::None;
        if (like_ != nullptr) {
            std::string text = ui::tr("player.like");
            if (state_ && !state_->likesText.empty()) {
                text += " \xC2\xB7 " + state_->likesText;
            }
            like_->setText(text);
            like_->setPrimary(rating == youtube::Rating::Like);
        }
        if (dislike_ != nullptr) {
            std::string text = ui::tr("player.dislike");
            if (votes_) {
                text += " \xC2\xB7 " + groupedNumber(votes_->dislikes);
            }
            dislike_->setText(text);
            dislike_->setPrimary(rating == youtube::Rating::Dislike);
        }
        if (votesLabel_ != nullptr) {  // signed out: no buttons to rate, the counts alone
            votesLabel_->setText(votes_ ? ui::tr("player.votes", {groupedNumber(votes_->likes),
                                                                  groupedNumber(votes_->dislikes)})
                                        : std::string());
            votesLabel_->visible = votes_.has_value();
        }
        if (subscribe_ != nullptr) {
            subscribe_->setText(ui::tr(state_ && state_->subscribed ? "channel.subscribed" : "channel.subscribe"));
        }
        markDirty();
    }

    // J'aime / Je n'aime pas; pressing the active one takes the rating back
    void rate(bool like) {
        if (rating_) {
            return;  // one change at a time
        }
        const youtube::Rating current = state_ ? state_->rating : youtube::Rating::None;
        const youtube::Rating pressed = like ? youtube::Rating::Like : youtube::Rating::Dislike;
        const youtube::Rating wanted = current == pressed ? youtube::Rating::None : pressed;
        rating_ = true;
        AppSession* shared = &session_;
        const std::string id = videoId_;
        run<core::Result<core::Unit>>(
            [shared, id, wanted] {
                return shared->signedIn<core::Unit>(
                    [&](const std::string& token) { return shared->youtube().rate(id, wanted, token); });
            },
            [this, wanted](core::Result<core::Unit> done) {
                rating_ = false;
                if (!done.ok()) {
                    PF_LOG_WARN("player: rating: %s", done.error().message.c_str());
                    toast(errorText(done.error()));
                    return;
                }
                if (!state_) {
                    state_ = youtube::VideoState{};
                }
                state_->rating = wanted;
                userRated_ = true;
                refreshActions();
            });
    }

    void toggleSubscription() {
        if (subscribing_) {
            return;
        }
        const std::string channel = channelId();
        const bool wanted = !(state_ && state_->subscribed);
        subscribing_ = true;
        AppSession* shared = &session_;
        run<core::Result<core::Unit>>(
            [shared, channel, wanted] {
                return shared->signedIn<core::Unit>(
                    [&](const std::string& token) { return shared->youtube().setSubscribed(channel, wanted, token); });
            },
            [this, wanted](core::Result<core::Unit> done) {
                subscribing_ = false;
                if (!done.ok()) {
                    PF_LOG_WARN("player: subscription: %s", done.error().message.c_str());
                    toast(errorText(done.error()));
                    return;
                }
                if (!state_) {
                    state_ = youtube::VideoState{};
                }
                state_->subscribed = wanted;
                userSubscribed_ = true;
                refreshActions();
            });
    }

    // "Enregistrer": "À regarder plus tard" and the account's playlists; the chosen one gets the video
    void save() {
        AppSession* shared = &session_;
        const std::string id = videoId_;
        run<core::Result<std::vector<youtube::PlaylistChoice>>>(
            [shared, id] {
                return shared->signedIn<std::vector<youtube::PlaylistChoice>>(
                    [&](const std::string& token) { return shared->youtube().playlistChoices(id, token); });
            },
            [this](core::Result<std::vector<youtube::PlaylistChoice>> choices) {
                if (!choices.ok()) {
                    PF_LOG_WARN("player: playlists: %s", choices.error().message.c_str());
                    toast(errorText(choices.error()));
                    return;
                }
                if (choices.value().empty()) {
                    return;
                }
                auto list = std::make_shared<std::vector<youtube::PlaylistChoice>>(std::move(choices.value()));
                std::vector<std::string> names;
                for (const youtube::PlaylistChoice& choice : *list) {
                    const std::string title = core::withoutEmoji(choice.title);
                    names.push_back(choice.contains ? ui::tr("player.save.contains", {title}) : title);
                }
                if (ui::Application* app = ui::Application::current()) {
                    app->showModal(ui::makeChoiceDialog(ui::tr("player.save.title"), names, 0,
                                                        [this, list](int index) {
                                                            addTo((*list)[static_cast<std::size_t>(index)]);
                                                        }));
                }
            });
    }

    void addTo(const youtube::PlaylistChoice& choice) {
        const std::string title = core::withoutEmoji(choice.title);
        if (choice.contains) {
            toast(ui::tr("player.save.contains", {title}));
            return;
        }
        AppSession* shared = &session_;
        const std::string id = videoId_;
        const std::string playlist = choice.id;
        run<core::Result<core::Unit>>(
            [shared, id, playlist] {
                return shared->signedIn<core::Unit>(
                    [&](const std::string& token) { return shared->youtube().addToPlaylist(playlist, id, token); });
            },
            [title](core::Result<core::Unit> added) {
                toast(added.ok() ? ui::tr("player.saved", {title}) : errorText(added.error()));
            });
    }

    // the comments panel on the right; the video keeps playing under it
    void openComments() {
        ui::Application* app = ui::Application::current();
        if (app == nullptr) {
            return;
        }
        if (!commentsChecked_) {
            fetchCommentsStart();
            return;
        }
        closeMenu();
        AppSession* shared = &session_;
        const core::CancelToken cancel = token_;
        app->showModal(std::make_unique<CommentsPanel>(
            commentsContinuation_, [this, shared, cancel](const std::string& token, CommentsPanel::Done done) {
                run<core::Result<youtube::CommentPage>>(
                    [shared, token, cancel] { return shared->youtube().comments(token, cancel); },
                    [done](core::Result<youtube::CommentPage> page) { done(std::move(page)); });
            }));
    }

    // the live chat in its panel over the video, its pages asked for as long as it is open
    void openLiveChat() {
        ui::Application* app = ui::Application::current();
        if (app == nullptr || liveChat_.empty()) {
            return;
        }
        closeMenu();
        AppSession* shared = &session_;
        const core::CancelToken cancel = token_;
        app->showModal(std::make_unique<ChatPanel>(
            liveChat_, [this, shared, cancel](const std::string& token, ChatPanel::Done done) {
                run<core::Result<youtube::ChatPage>>(
                    [shared, token, cancel] { return shared->youtube().liveChat(token, cancel); },
                    [done](core::Result<youtube::ChatPage> page) { done(std::move(page)); });
            }));
    }

    // the video page failed or has not answered: asked (again) for its comments' first page, then the panel
    void fetchCommentsStart() {
        if (commentsFetching_) {
            return;
        }
        commentsFetching_ = true;
        toast(ui::tr("ui.loading"));
        AppSession* shared = &session_;
        const std::string id = videoId_;
        const core::CancelToken cancel = token_;
        run<core::Result<youtube::NextReply>>(
            [shared, id, cancel] { return shared->youtube().next(id, cancel); },
            [this](core::Result<youtube::NextReply> next) {
                commentsFetching_ = false;
                if (!next.ok()) {
                    PF_LOG_WARN("player: comments: %s", next.error().message.c_str());
                    toast(errorText(next.error()));
                    return;
                }
                if (!commentsChecked_) {
                    commentsContinuation_ = next.value().commentsContinuation;
                    commentsChecked_ = true;
                }
                openComments();
            });
    }

    static void toast(const std::string& text) {
        if (ui::Application* app = ui::Application::current()) {
            app->toast(text);
        }
    }

    void closeMenu() {
        if (!menu_->visible) {
            return;
        }
        if (ui::Application* app = ui::Application::current()) {
            app->focus().focus(this);
        }
        menu_->visible = false;
        markDirty();
    }

    void chooseQuality() {
        std::vector<std::string> names;
        int selected = 3;
        for (int i = 0; i < 4; ++i) {
            names.push_back(std::to_string(kHeights[i]) + "p");
            selected = kHeights[i] == maxHeight_ ? i : selected;
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.quality"), names, selected, [this](int index) {
                maxHeight_ = kHeights[index];
                restart();  // the same second at the new quality
            }));
    }

    // YouTube's storyboards of the video, for the preview while seeking (not for a live)
    void setupPreview() {
        if (streams_.live || preview_ == nullptr) {
            return;
        }
        const auto board = youtube::parseStoryboard(streams_.storyboardSpec, streams_.durationSeconds);
        if (!board) {
            return;
        }
        player::TrickplayInfo info;
        info.width = board->width;
        info.height = board->height;
        info.tileWidth = board->columns;
        info.tileHeight = board->rows;
        info.thumbnailCount = board->count;
        info.intervalMs = board->intervalMs;
        const youtube::Storyboard sheets = *board;
        preview_->setPictures(info, [sheets](int sheet) { return sheets.sheetUrl(sheet); });
        PF_LOG_INFO("player: storyboard of %dx%d pictures", board->width, board->height);
    }

    // the queued videos, in order, with a way to empty the queue
    void showQueue() {
        const auto& queue = session_.watchQueue();
        std::string text;
        for (std::size_t i = 0; i < queue.size() && i < 8; ++i) {
            text += (text.empty() ? "" : "\n") + std::to_string(i + 1) + ". " + core::withoutEmoji(queue[i].title);
        }
        if (queue.size() > 8) {
            text += "\n\xE2\x80\xA6";
        }
        AppSession* shared = &session_;
        ui::Application::current()->showModal(ui::makeDialog(ui::tr("queue.title"), text,
                                                             {{ui::tr("queue.clear"), [this, shared] {
                                                                   shared->watchQueue().clear();
                                                                   closeMenu();
                                                               }},
                                                              {ui::tr("ui.close"), {}}}));
    }

    // ---- volume and picture (kept in the settings for the next videos)

    void chooseVolume() {
        std::vector<std::string> names;
        int selected = 0;
        for (int i = 0; i <= 10; ++i) {
            const int percent = 100 - i * 10;
            names.push_back(std::to_string(percent) + " %");
            selected = percent == session_.settings().volume ? i : selected;
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.volume"), names, selected, [this](int index) {
                session_.settings().volume = 100 - index * 10;
                session_.saveSettings();
                if (engine_) {
                    engine_->setVolume(session_.settings().volume);
                }
            }));
    }

    void choosePicture() {
        static const char* const kFits[] = {"fit", "fill", "stretch"};
        static const int kSizes[] = {100, 95, 90, 85, 80};
        std::vector<std::string> names;
        int selected = 0;
        for (int f = 0; f < 3; ++f) {
            for (int z = 0; z < (f == 0 ? 5 : 1); ++z) {  // a smaller picture only makes sense whole
                std::string name = ui::tr(std::string("player.picture.") + kFits[f]);
                if (f == 0 && z > 0) {
                    name += " \xC2\xB7 " + std::to_string(kSizes[z]) + " %";
                }
                if (session_.settings().pictureFit == kFits[f] &&
                    (f != 0 || session_.settings().pictureSize == kSizes[z])) {
                    selected = static_cast<int>(names.size());
                }
                names.push_back(name);
            }
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.picture"), names, selected, [this](int index) {
                Settings& settings = session_.settings();
                settings.pictureFit = index < 5 ? "fit" : index == 5 ? "fill" : "stretch";
                settings.pictureSize = index < 5 ? kSizes[index] : 100;
                session_.saveSettings();
                markDirty();
            }));
    }

    void chooseSpeed() {
        std::vector<std::string> names;
        int selected = 2;
        const auto& speeds = youtube::playbackSpeeds();
        for (std::size_t i = 0; i < speeds.size(); ++i) {
            char text[16];
            std::snprintf(text, sizeof text, "%g\xC3\x97", speeds[i]);
            names.emplace_back(text);
            selected = speeds[i] == speed_ ? static_cast<int>(i) : selected;
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.speed"), names, selected, [this](int index) {
                speed_ = youtube::playbackSpeeds()[static_cast<std::size_t>(index)];
                if (engine_) {
                    engine_->setSpeed(speed_);
                }
            }));
    }

    // YouTube's dubbed languages: the chosen one plays from the same moment, and stays for later restarts
    void chooseAudio() {
        std::vector<std::string> names;
        int selected = 0;
        for (std::size_t i = 0; i < streams_.audioTracks.size(); ++i) {
            names.push_back(streams_.audioTracks[i].name.empty() ? streams_.audioTracks[i].id
                                                                 : streams_.audioTracks[i].name);
            selected = streams_.audioTracks[i].id == streams_.audioTrack ? static_cast<int>(i) : selected;
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.audio"), names, selected, [this](int index) {
                const youtube::AudioTrack& track = streams_.audioTracks[static_cast<std::size_t>(index)];
                if (track.id == streams_.audioTrack || !engine_) {
                    return;
                }
                audioTrack_ = track.id;
                PF_LOG_INFO("player: audio %s", track.id.c_str());
                streams_.audioTrack = track.id;
                streams_.audio = track.url;
                engine_->load(streams_.video, std::max(lastPosition_, engine_->state().position), streams_.audio);
                engine_->setSpeed(speed_);
            }));
    }

    void chooseSubtitles() {
        std::vector<std::string> names{ui::tr("player.subtitles_off")};
        for (const youtube::CaptionTrack& track : streams_.captions) {
            names.push_back(track.name.empty() ? track.language : track.name);
        }
        ui::Application::current()->showModal(
            ui::makeChoiceDialog(ui::tr("player.subtitles"), names, captionIndex() + 1, [this](int index) {
                captionLanguage_ = index > 0 ? streams_.captions[static_cast<std::size_t>(index - 1)].language : "";
                applyCaption();
            }));
    }

    // the chosen language in the current streams' tracks (a restart may get them from another client), -1: none
    int captionIndex() const {
        for (std::size_t i = 0; i < streams_.captions.size() && !captionLanguage_.empty(); ++i) {
            if (streams_.captions[i].language == captionLanguage_) {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    // captions are added to mpv when chosen (each one is a download)
    void applyCaption() {
        if (!engine_) {
            return;
        }
        const int index = captionIndex();
        if (index < 0) {
            engine_->setSubtitleTrack(0);
            return;
        }
        engine_->addSubtitle(youtube::captionUrl(streams_.captions[static_cast<std::size_t>(index)]), true);
    }

    std::string channelId() const { return !streams_.channelId.empty() ? streams_.channelId : ownerId_; }

    void openChannelHere() {
        const std::string id = channelId();
        AppSession* shared = &session_;
        later([shared, id] {
            if (ui::Application* app = ui::Application::current()) {
                app->pop();  // the player goes: the channel is pushed where it was
                openChannel(*shared, id);
            }
        });
    }

    // ---- controls

    void bindControls() {
        // no hints: the bar and the buttons say enough while a video plays
        bind(platform::Action::Confirm, "", [this] {
            togglePause();
            return true;
        });
        bind(platform::Action::Left, "", [this] { return menu_->visible ? false : (seekBy(-1), true); });
        bind(platform::Action::Right, "", [this] { return menu_->visible ? false : (seekBy(1), true); });
        // with chapters: the previous / next one; else a minute back / on
        bind(platform::Action::L1, "", [this] {
            chapters_.empty() ? seekSeconds(-60) : seekChapter(-1);
            return true;
        });
        bind(platform::Action::R1, "", [this] {
            chapters_.empty() ? seekSeconds(60) : seekChapter(1);
            return true;
        });
        // Shorts: ↑ / ↓ the previous / next Short (the menu on △); else ↑ / ↓ open the menu
        bind(platform::Action::Up, "", [this] {
            return shorts_ && !menu_->visible ? (nextShort(-1), true) : menuStep(ui::Direction::Up);
        });
        bind(platform::Action::Down, "", [this] {
            return shorts_ && !menu_->visible ? (nextShort(1), true) : menuStep(ui::Direction::Down);
        });
        bind(platform::Action::Options, "", [this] {
            menu_->visible ? closeMenu() : openMenu();
            return true;
        });
        bind(platform::Action::Back, "", [this] {
            if (nextShown()) {
                cancelNext();
                return true;
            }
            if (menu_->visible) {
                closeMenu();
                return true;
            }
            return false;  // the application pops the player
        });
    }

    // ↑/↓: open the menu; in it, move between its two rows, and close it past them
    bool menuStep(ui::Direction direction) {
        if (!menu_->visible) {
            openMenu();
            return true;
        }
        ui::Application* app = ui::Application::current();
        if (app == nullptr || !app->focus().move(direction)) {
            closeMenu();
        }
        return true;
    }

    // the Short before or after this one in its list, in place of this player
    void nextShort(int direction) {
        const long long target = static_cast<long long>(index_) + direction;
        if (!queue_ || target < 0 || target >= static_cast<long long>(queue_->size())) {
            showBar();
            return;
        }
        AppSession* shared = &session_;
        const Queue queue = queue_;
        const std::size_t index = static_cast<std::size_t>(target);
        later([shared, queue, index] {
            if (ui::Application* app = ui::Application::current()) {
                app->replaceTop(makePlayer(*shared, (*queue)[index].id, queue, index, true));
            }
        });
    }

    void togglePause() {
        if (!engine_) {
            return;
        }
        const bool resuming = engine_->state().paused;
        if (resuming && streamsExpired()) {
            // after a long pause YouTube no longer serves these addresses (403): new ones, from the same second
            PF_LOG_INFO("player: %s: the streams expired during the pause, asked again", videoId_.c_str());
            lastPosition_ = engine_->state().position;
            restart();
            showBar();
            return;
        }
        engine_->setPaused(!engine_->state().paused);
        showBar();
    }

    // the addresses stop working at their "expire" time; a minute's margin for the time mpv takes to open them
    bool streamsExpired() const {
        const std::int64_t expiry = youtube::expiresAt(streams_);
        return expiry > 0 && session_.now() >= expiry - 60;
    }

    std::vector<double> chapterStarts() const {
        std::vector<double> starts;
        for (const youtube::Chapter& chapter : chapters_) {
            starts.push_back(chapter.start);
        }
        return starts;
    }

    int chapterAt(double seconds) const { return player::chapterAt(chapterStarts(), seconds); }

    // L1 / R1 with chapters: the previous one (or the start of this one), the next one
    void seekChapter(int direction) {
        if (!engine_ || streams_.live) {
            showBar();
            return;
        }
        const player::Engine::State state = engine_->state();
        const double from = seek_.pending() ? seek_.target() : state.position;
        const double target = player::chapterTarget(chapterStarts(), from, direction);
        if (target >= 0) {
            seek_.press(target - from, now(), from, state.duration);
        }
        showBar();
    }

    void seekBy(int direction) {
        const std::int64_t t = now();
        const bool held = direction == lastDirection_ && t - lastSeekPressMs_ < kHoldGapMs;
        if (!held) {
            holdStartMs_ = t;
        }
        lastDirection_ = direction;
        lastSeekPressMs_ = t;
        seekSeconds(direction * player::seekStep(t - holdStartMs_));
    }

    void seekSeconds(double delta) {
        if (!engine_ || streams_.live) {
            showBar();  // a live stream cannot be sought
            return;
        }
        const player::Engine::State state = engine_->state();
        seek_.press(delta, now(), state.position, state.duration);
        showBar();
        refreshBar(state);
    }

    // ---- loading

    void start(double at) {
        frameShown_ = false;
        const unsigned generation = ++startGeneration_;
        AppSession* shared = &session_;
        const std::string id = videoId_;
        const int height = maxHeight_;
        const std::string invidious = session_.settings().invidious;
        const core::CancelToken cancel = token_;
        run<core::Result<youtube::Streams>>(
            [shared, id, height, invidious, cancel] {
                return shared->youtube().streams(id, height, invidious, cancel);
            },
            [this, at, generation](core::Result<youtube::Streams> streams) {
                if (generation == startGeneration_) {  // a later start (another quality) supersedes this one
                    onStreams(std::move(streams), at);
                }
            });
        if (!extrasLoaded_) {
            extrasLoaded_ = true;
            const bool sponsorBlock = session_.settings().sponsorBlock;
            const std::vector<std::string> categories = session_.settings().sponsorCategories;
            const bool signedIn = session_.account().signedIn();
            const bool dislikes = session_.settings().dislikes;
            run<Extras>(
                [shared, id, sponsorBlock, categories, signedIn, dislikes, cancel] {
                    Extras extras;
                    extras.next = shared->youtube().next(id, cancel);
                    if (dislikes) {
                        auto votes = shared->youtube().votes(id, cancel);
                        if (votes.ok()) {
                            extras.votes = votes.value();
                        } else {
                            PF_LOG_INFO("player: votes: %s", votes.error().message.c_str());
                        }
                    }
                    if (signedIn) {
                        auto state = shared->signedIn<youtube::VideoState>(
                            [&](const std::string& token) { return shared->youtube().videoState(id, token, cancel); },
                            cancel);
                        if (state.ok()) {
                            extras.state = std::move(state.value());
                        } else {
                            PF_LOG_WARN("player: rating: %s", state.error().message.c_str());
                        }
                    }
                    if (sponsorBlock && !categories.empty()) {
                        auto segments = shared->youtube().segments(id, categories, cancel);
                        if (segments.ok()) {
                            extras.segments = std::move(segments.value());
                        }
                    }
                    return extras;
                },
                [this](Extras extras) { onExtras(std::move(extras)); });
        }
    }

    void onStreams(core::Result<youtube::Streams> streams, double at) {
        if (!shown_) {
            return;  // hidden meanwhile: onShow starts again
        }
        if (engine_ && engine_->state().position > 0) {
            at = engine_->state().position;  // it kept playing while the new streams were asked for
        }
        if (!streams.ok()) {
            PF_LOG_WARN("player: %s: %s", videoId_.c_str(), streams.error().message.c_str());
            showError(streams.error());
            return;
        }
        streams_ = std::move(streams.value());
        if (history_) {
            history_->setLive(streams_.live);
        }
        setupPreview();
        PF_LOG_INFO("player: %s from %s, %dp%s", videoId_.c_str(), streams_.client.c_str(), streams_.height,
                    streams_.live ? " live" : "");
        if (!streams_.title.empty()) {
            title_->setText(core::withoutEmoji(streams_.title));
        }
        live_->visible = streams_.live;
        for (const youtube::AudioTrack& track : streams_.audioTracks) {
            if (!audioTrack_.empty() && track.id == audioTrack_) {  // the language chosen before a restart
                streams_.audioTrack = track.id;
                streams_.audio = track.url;
            }
        }
        // a new engine for each start: the streams answer only to the User-Agent of the client that got them
        logDrops();
        engine_.reset();
        player::EngineOptions options = engineOptions(session_, streams_.userAgent);
        options.loop = shorts_;  // a Short starts again at its end, as on YouTube
        engine_ = player::Engine::create(options, [] {});
        if (!engine_) {
            showError(core::makeError(core::ErrorKind::BadResponse, "the player could not start"));
            return;
        }
        engine_->setSpeed(speed_);
        engine_->setVolume(session_.settings().volume);
        if (!streams_.hls.empty()) {
            engine_->loadPlaylist(streams_.hls, streams_.live ? 0 : at);
        } else {
            engine_->load(streams_.video, streams_.live ? 0 : at, streams_.audio);
        }
        if (session_.synchronous) {
            showFirstPictureNow();
        }
    }

    void onExtras(Extras extras) {
        if (!extras.next.ok()) {
            extrasLoaded_ = false;  // asked again with the next start (Réessayer)
        }
        segments_ = std::move(extras.segments);
        votes_ = extras.votes;
        refreshActions();
        skipped_.assign(segments_.size(), false);
        if (extras.state) {
            // read before a change the user made meanwhile: that change stays
            const youtube::VideoState confirmed = state_ ? *state_ : youtube::VideoState{};
            state_ = std::move(extras.state);
            if (userRated_) {
                state_->rating = confirmed.rating;
            }
            if (userSubscribed_) {
                state_->subscribed = confirmed.subscribed;
            }
            refreshActions();
        }
        if (extras.next.ok()) {
            const youtube::NextReply& next = extras.next.value();
            chapters_ = next.chapters;
            progress_->setChapters(chapterStarts());
            ownerId_ = next.owner.id;
            commentsContinuation_ = next.commentsContinuation;
            commentsChecked_ = true;
            liveChat_ = next.liveChat;
            channel_->setText(core::withoutEmoji(next.owner.name));
            for (const youtube::Video& video : next.related) {
                if (nextVideo_) {
                    break;  // the playlist's next video
                }
                if (!video.live && !video.id.empty() && (session_.settings().shorts || !video.isShort)) {
                    nextVideo_ = video;
                }
            }
        }
    }

    // Test mode: the first picture before the next frame, then paused on it (deterministic screenshots). Up to a
    // minute: the README's screenshots play YouTube's own streams.
    void showFirstPictureNow() {
        bool shown = false;
        for (int i = 0; i < 12000 && !shown && engine_; ++i) {
            for (player::EngineEvent event : engine_->drainEvents()) {
                onEngineEvent(event);
                shown = shown || event == player::EngineEvent::Error;
            }
            if (engine_ && engine_->renderIfDue(frame_)) {
                shown = true;
                frameShown_ = true;  // paused next: no later picture would tell the frame loop
                onEngineEvent(player::EngineEvent::FirstFrame);
            }
            if (!shown) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        if (engine_) {
            engine_->setPaused(true);
        }
    }

    void onEngineEvent(player::EngineEvent event) {
        switch (event) {
        case player::EngineEvent::FileLoaded:
            if (!captionLanguage_.empty()) {
                applyCaption();
            }
            break;
        case player::EngineEvent::FirstFrame:
            PF_LOG_INFO("player: %s", engine_->describe().c_str());
            if (resumedFrom_ > 0 && !resumeTold_) {
                resumeTold_ = true;
                if (ui::Application* app = ui::Application::current()) {
                    app->toast(ui::tr("player.resumed", {clockText(resumedFrom_)}));
                }
            }
            break;
        case player::EngineEvent::Seeked:
            break;
        case player::EngineEvent::EndOfFile:
            ended();
            break;
        case player::EngineEvent::Error:
            PF_LOG_WARN("player: %s: %s", videoId_.c_str(), engine_->state().error.c_str());
            showError(core::makeError(core::ErrorKind::Network, engine_->state().error));  // (the streams were checked)
            break;
        }
    }

    // ---- per frame

    void tick(bool freshPicture) {
        const std::int64_t t = now();
        const player::Engine::State state = engine_->state();
        if (frameShown_) {  // before the first picture mpv has no position yet: seeks would be lost
            if (const auto target = seek_.due(t)) {
                engine_->seekTo(*target);
            }
        }
        if (state.buffering != spinner_->visible || (!frameShown_ && !spinner_->visible)) {
            spinner_->visible = state.buffering || !frameShown_;
            markDirty();
        }
        frameShown_ = frameShown_ || freshPicture;
        if (frameShown_ && state.position > 0) {
            lastPosition_ = state.position;
            lastDuration_ = state.duration;
            if (history_) {
                history_->update(state.position, state.paused);
            }
            if (t - progressSavedMs_ > kProgressSaveMs) {  // kept on the console now and then while it plays
                progressSavedMs_ = t;
                recordProgress(true);
            }
        }
        if (bar_->visible) {
            refreshBar(state);
            if (t > barUntil_ && !state.paused && !seek_.pending() && !menu_->visible) {
                bar_->visible = false;
                markDirty();
            }
        }
        if (!bar_->visible) {  // no preview without its bar
            preview_->update(false, 0, 0, 0, {});
        }
        if (!seek_.pending() && frameShown_) {  // the pictures around the playing position, ready for a seek
            preview_->follow(state.position);
        }
        if (frameShown_) {
            skipSegment(state);
            updateNext(state);
        }
    }

    void skipSegment(const player::Engine::State& state) {
        if (seek_.pending() || streams_.live) {
            return;
        }
        const auto index = youtube::segmentToSkip(state.position, segments_, skipped_);
        if (!index) {
            return;
        }
        const youtube::Segment& segment = segments_[*index];
        skipped_[*index] = true;
        engine_->seekTo(segment.end);
        PF_LOG_INFO("player: %s segment skipped %.1f-%.1f", segment.category.c_str(), segment.start, segment.end);
        if (ui::Application* app = ui::Application::current()) {
            app->toast(ui::tr("player.skipped", {ui::tr("settings.sponsor." + segment.category),
                                                 clockText(segment.end - segment.start)}));
        }
    }

    // ---- "À suivre"

    bool nextShown() const { return next_->visible; }

    // what plays after this video: the queue's first (the user asked for it), else "À suivre" when autoplay is on
    std::optional<youtube::Video> upcoming() const {
        if (shorts_ || streams_.live) {
            return std::nullopt;
        }
        if (!session_.watchQueue().empty()) {
            return session_.watchQueue().front();
        }
        if (nextVideo_ && session_.settings().autoplay) {
            return nextVideo_;
        }
        return std::nullopt;
    }

    void updateNext(const player::Engine::State& state) {
        const auto upcomingVideo = upcoming();
        const bool queued = !session_.watchQueue().empty();
        if (!upcomingVideo || nextCancelled_ || (!queued && state.duration < kMinDurationForNext)) {
            return;
        }
        const double left = state.duration - state.position;
        if (left > kNextAheadSeconds) {
            if (next_->visible) {  // sought back
                next_->visible = false;
                markDirty();
            }
            return;
        }
        if (!next_->visible || nextShownId_ != upcomingVideo->id) {
            nextShownId_ = upcomingVideo->id;
            nextImage_->setUrl(upcomingVideo->thumbnail);
            nextTitle_->setText(core::withoutEmoji(upcomingVideo->title));
            next_->visible = true;
            markDirty();
        }
        const std::string text = ui::tr("player.next_in", {std::to_string(static_cast<long>(left + 0.999))});
        if (text != nextCountdown_->text()) {
            nextCountdown_->setText(text);
        }
    }

    void cancelNext() {
        nextCancelled_ = true;
        next_->visible = false;
        markDirty();
    }

    // where the video is, for the cards' bar and the next time it is opened (not a live)
    void recordProgress(bool save) {
        if (streams_.live || lastDuration_ <= 0 || !frameShown_) {
            return;
        }
        session_.progress().set(videoId_, static_cast<std::int64_t>(lastPosition_),
                                static_cast<std::int64_t>(lastDuration_), session_.now());
        if (save) {
            session_.progress().save();
        }
    }

    void ended() {
        lastPosition_ = lastDuration_;  // watched: its bar full, and it starts from the beginning next time
        recordProgress(true);
        // the queue first: what the user asked for
        if (!nextCancelled_ && !streams_.live && !session_.watchQueue().empty()) {
            const youtube::Video queued = session_.watchQueue().front();
            session_.watchQueue().erase(session_.watchQueue().begin());
            AppSession* shared = &session_;
            later([shared, id = queued.id] {
                if (ui::Application* app = ui::Application::current()) {
                    app->replaceTop(makePlayer(*shared, id));
                }
            });
            return;
        }
        // as for the card: not after a very short video (it would start with no countdown to cancel it)
        if (nextVideo_ && !nextCancelled_ && !streams_.live && session_.settings().autoplay &&
            lastDuration_ >= kMinDurationForNext) {
            AppSession* shared = &session_;
            const std::string id = nextVideo_->id;
            Queue queue = queueNext_ > 0 ? queue_ : nullptr;  // still in the playlist, or on to related videos
            const std::size_t index = queueNext_;
            later([shared, id, queue, index] {
                if (ui::Application* app = ui::Application::current()) {
                    app->replaceTop(makePlayer(*shared, id, queue, index));
                }
            });
            return;
        }
        leave();
    }

    // ---- leaving and errors

    // runs on the next frame: reached from the middle of renderVideo and of key handlers
    void leave() {
        later([] {
            if (ui::Application* app = ui::Application::current()) {
                app->pop();
            }
        });
    }

    // at the last position the video had (an engine that failed reports none any more)
    void restart() { start(lastPosition_); }

    void release() {
        if (ui::Application* app = ui::Application::current(); app != nullptr && app->videoSource() == this) {
            app->setVideoSource(nullptr);
        }
        if (engine_) {
            recordProgress(true);
        }
        if (history_) {
            history_->flush();
        }
        logDrops();
        engine_.reset();
    }

    // frames the decoder or the output could not keep up with, to judge the console on a resolution
    void logDrops() {
        if (engine_) {
            const player::Engine::Stats stats = engine_->stats();
            PF_LOG_INFO("player: frames dropped %ld by the decoder, %ld at output; last conversion %.1f ms",
                        stats.decoderDrops, stats.outputDrops, stats.renderMs);
        }
    }

    void showError(const core::Error& error) {
        ui::Application* app = ui::Application::current();
        if (app == nullptr || errorShown_) {
            return;
        }
        errorShown_ = true;
        spinner_->visible = false;
        // YouTube's own words when it refused the video, else what went wrong on the way
        const bool refused = error.kind == core::ErrorKind::Server && error.httpStatus == 0;
        const std::string reason = !refused                ? errorText(error)
                                   : error.message.empty() ? ui::tr("player.refused_plain")
                                                           : ui::tr("player.refused", {error.message});
        auto dialog = ui::makeDialog(ui::tr("player.error"), reason,
                                     {{ui::tr("player.retry"),
                                       [this] {
                                           errorShown_ = false;
                                           restart();
                                       }},
                                      {ui::tr("player.back"), [this] { leave(); }}});
        // ◯ is Retour: dismissing the dialog would leave a player with nothing to play
        dialog->bind(platform::Action::Back, ui::tr("player.back"), [this] {
            leave();
            return true;
        });
        app->showModal(std::move(dialog));
    }

    std::string videoId_;
    int maxHeight_;
    youtube::Streams streams_;
    std::string ownerId_;
    std::vector<youtube::Segment> segments_;
    std::vector<bool> skipped_;
    std::optional<youtube::Video> nextVideo_;
    std::string nextShownId_;  // the video the "À suivre" card shows
    Queue queue_;                // the playlist played, when there is one
    std::size_t queueNext_ = 0;  // where nextVideo_ is in it (0: not from it)
    std::unique_ptr<player::Engine> engine_;
    player::VideoFrame frame_;
    player::SeekAccumulator seek_;

    ui::Spinner* spinner_ = nullptr;
    ui::Box* next_ = nullptr;
    ui::ImageView* nextImage_ = nullptr;
    ui::Label* nextTitle_ = nullptr;
    ui::Label* nextCountdown_ = nullptr;
    ui::Box* menu_ = nullptr;
    ui::Box* actions_ = nullptr;
    ui::Box* playback_ = nullptr;
    ui::Button* like_ = nullptr;
    ui::Button* dislike_ = nullptr;
    ui::Button* subscribe_ = nullptr;
    std::optional<youtube::VideoState> state_;
    std::string commentsContinuation_;
    ui::Box* bar_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* channel_ = nullptr;
    SegmentBar* progress_ = nullptr;
    TrickplayPreview* preview_ = nullptr;
    std::unique_ptr<WatchHistory> history_;  // signed in, with the setting on
    std::string liveChat_;                    // a live's chat: its first page (from the video page)
    std::optional<youtube::Votes> votes_;
    ui::Label* votesLabel_ = nullptr;
    std::vector<youtube::Chapter> chapters_;
    ui::Label* live_ = nullptr;
    ui::Label* time_ = nullptr;

    double speed_ = 1;
    std::string captionLanguage_;  // the captions chosen, "" none
    std::string audioTrack_;       // the audio language chosen, "" the original
    double lastPosition_ = 0;
    bool shorts_ = false;
    std::size_t index_ = 0;
    double resumedFrom_ = 0;  // where it started again (0: from the start)
    bool resumeTold_ = false;
    std::int64_t progressSavedMs_ = 0;
    static constexpr std::int64_t kProgressSaveMs = 30000;
    double lastDuration_ = 0;
    unsigned startGeneration_ = 0;
    bool started_ = false;
    bool shown_ = false;
    bool rating_ = false;       // a rating is being sent
    bool subscribing_ = false;  // a subscription change is being sent
    bool commentsChecked_ = false;
    bool commentsFetching_ = false;
    bool userRated_ = false;       // the rating shown is one the user set (newer than any state read before)
    bool userSubscribed_ = false;  // the same for the subscription
    bool extrasLoaded_ = false;
    bool frameShown_ = false;
    bool errorShown_ = false;
    bool nextCancelled_ = false;
    std::int64_t barUntil_ = 0;
    std::int64_t lastSeekPressMs_ = 0;
    std::int64_t holdStartMs_ = 0;
    int lastDirection_ = 0;
};

}  // namespace

std::unique_ptr<ui::Screen> makePlayer(AppSession& session, const std::string& videoId, Queue queue,
                                       std::size_t index, bool shorts) {
    return std::make_unique<PlayerScreen>(session, videoId, std::move(queue), index, shorts);
}

void addToQueue(AppSession& session, const youtube::Video& video) {
    auto& queue = session.watchQueue();
    if (std::none_of(queue.begin(), queue.end(), [&](const youtube::Video& v) { return v.id == video.id; })) {
        queue.push_back(video);
    }
    if (ui::Application* app = ui::Application::current()) {
        app->toast(ui::tr("queue.added", {std::to_string(queue.size())}));
    }
}

void openShorts(AppSession& session, Queue queue, std::size_t index) {
    if (ui::Application* app = ui::Application::current(); app != nullptr && queue && index < queue->size()) {
        const std::string id = (*queue)[index].id;
        app->push(makePlayer(session, id, std::move(queue), index, true));
    }
}

void openQueue(AppSession& session, Queue queue, std::size_t index) {
    if (ui::Application* app = ui::Application::current(); app != nullptr && queue && index < queue->size()) {
        const std::string id = (*queue)[index].id;
        app->push(makePlayer(session, id, std::move(queue), index));
    }
}

void openVideo(AppSession& session, const std::string& videoId) {
    if (ui::Application* app = ui::Application::current(); app != nullptr && !videoId.empty()) {
        app->push(makePlayer(session, videoId));
    }
}

}  // namespace playtube::app
