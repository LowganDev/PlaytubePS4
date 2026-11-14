#include <doctest/doctest.h>

#include <chrono>
#include <string>
#include <thread>

#include "app/shell/screens.hpp"
#include "app/shell/trickplay_preview.hpp"
#include "support/shell_harness.hpp"
#include "ui/video_source.hpp"

using namespace playtube;
using namespace playtube::test;

namespace {

// the player's picture, as the frame loop would ask for it
bool picture(Shell& shell) {
    ui::VideoSource* video = shell.application->videoSource();
    if (video == nullptr) {
        return false;
    }
    video->renderVideo();
    return video->videoLayer().rgb0 != nullptr;
}

}  // namespace

TEST_CASE("player: the streams of the first client that answers, with its User-Agent, video and separate audio") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140, 18}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));  // somewhere to come back to
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    CHECK(picture(shell));
    CHECK(shows(*shell.application, "Demo"));         // the title from the player reply
    CHECK(shows(*shell.application, "Lofi Girl"));    // the channel from the video page
    const auto video = shell.youtube.requestsTo("/media/137");
    const auto audio = shell.youtube.requestsTo("/media/140");
    REQUIRE(video.size() >= 2);  // the probe, then the player
    REQUIRE(audio.size() >= 2);  // the separate audio too: probed, then played
    CHECK(audio.back().range != audio.front().range);
    CHECK(video.back().userAgent.find("Safari") != std::string::npos);
    CHECK(audio.back().userAgent == video.back().userAgent);
    shell.press(Action::Back);
    CHECK(shell.application->videoSource() == nullptr);  // the engine released with the screen
}

TEST_CASE("player: a refused video says YouTube refused it and why, with Retry and Back") {
    Shell shell;
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(shell.application->hasModal());
    CHECK(shows(*shell.application,
                "YouTube refused this video: Connectez-vous pour confirmer que vous n'\xC3\xAAtes pas un robot"));
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    pressAnywhere(shell, "Try again");
    shell.frames(10);
    CHECK_FALSE(shell.application->hasModal());
    CHECK(picture(shell));
}

TEST_CASE("player: ↑ opens the menu row; ◯ closes it before leaving; Watch later only signed in") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    CHECK(findPressable(shell.application->root(), "Play") != nullptr);  // paused on its first picture in tests
    CHECK(findPressable(shell.application->root(), "Quality") != nullptr);
    CHECK(findPressable(shell.application->root(), "Speed") != nullptr);
    CHECK(findPressable(shell.application->root(), "Channel") != nullptr);
    CHECK(findPressable(shell.application->root(), "Watch later") == nullptr);
    shell.press(Action::Back);
    CHECK(findPressable(shell.application->root(), "Quality") == nullptr);
    CHECK(shell.application->videoSource() != nullptr);  // still playing
    shell.press(Action::Back);
    CHECK(shell.application->videoSource() == nullptr);
}

TEST_CASE("player: saving to watch later or a playlist from the menu, signed in") {
    Shell shell;
    signIn(shell);
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    pressAnywhere(shell, "Save");
    shell.frames(3);
    REQUIRE(shell.application->hasModal());
    CHECK(shows(*shell.application, "Save to\xE2\x80\xA6"));
    pressAnywhere(shell, "Playlist 2");
    shell.frames(3);
    const auto saved = shell.youtube.requestsTo("/youtubei/v1/browse/edit_playlist");
    REQUIRE(saved.size() == 1);
    CHECK(saved[0].body.find(R"("playlistId":"PLdemo02xxxxxxxxxxxxxxxxxxxxxxxxxx")") != std::string::npos);
    CHECK(saved[0].body.find(R"("addedVideoId":"dQw4w9WgXcQ")") != std::string::npos);
    pressAnywhere(shell, "Save");
    shell.frames(3);
    pressAnywhere(shell, "\xC3\x80 regarder plus tard");
    shell.frames(3);
    const auto later = shell.youtube.requestsTo("/youtubei/v1/browse/edit_playlist");
    REQUIRE(later.size() == 2);
    CHECK(later[1].body.find(R"("playlistId":"WL")") != std::string::npos);
}

TEST_CASE("player: rating from the menu, pressing the active one takes it back; subscribing to the channel") {
    Shell shell;
    signIn(shell);
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    const std::string like = "Like \xC2\xB7 19\xC2\xA0M";  // with the likes YouTube counts
    pressAnywhere(shell, like);
    shell.frames(3);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/like/like").size() == 1);
    pressAnywhere(shell, like);
    shell.frames(3);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/like/removelike").size() == 1);
    pressAnywhere(shell, "Dislike \xC2\xB7 521,783");  // with Return YouTube Dislike's count
    shell.frames(3);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/like/dislike").size() == 1);
    pressAnywhere(shell, "Subscribe");
    shell.frames(3);
    const auto subscribed = shell.youtube.requestsTo("/youtubei/v1/subscription/subscribe");
    REQUIRE(subscribed.size() == 1);
    CHECK(subscribed[0].body.find(R"("channelIds":["UCSJ4gkVC6NrvII8umztf0Ow"])") != std::string::npos);
    pressAnywhere(shell, "Subscribed");
    shell.frames(3);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/subscription/unsubscribe").size() == 1);
}

TEST_CASE("player: signed out, the menu has the comments and the channel but nothing for an account") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    CHECK(findPressable(shell.application->root(), "Comments") != nullptr);
    CHECK(findPressable(shell.application->root(), "Channel") != nullptr);
    CHECK(findPressable(shell.application->root(), "Dislike") == nullptr);
    CHECK(findPressable(shell.application->root(), "Save") == nullptr);
    CHECK(findPressable(shell.application->root(), "Subscribe") == nullptr);
    // ↑ goes from the playback row to the video's row, ↑ again closes the menu
    shell.press(Action::Up);
    CHECK(shell.application->focus().focused() == findPressable(shell.application->root(), "Comments"));
    shell.press(Action::Up);
    CHECK(findPressable(shell.application->root(), "Comments") == nullptr);
}

TEST_CASE("player: the comments in a panel over the playing video; a comment's replies; \xE2\x97\x8B closes it") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    pressAnywhere(shell, "Comments");
    shell.frames(5);
    REQUIRE(shell.application->hasModal());
    CHECK(showsSomewhere(*shell.application, "Premier commentaire de d\xC3\xA9monstration"));
    CHECK(shows(*shell.application, "@auteur1 \xC2\xB7 il y a 6 ans (modifi\xC3\xA9)"));
    CHECK(shows(*shell.application, "104\xC2\xA0k likes \xC2\xB7 611 replies"));
    CHECK(shell.application->videoSource() != nullptr);  // the video plays on under the panel
    shell.press(Action::Confirm);  // the first comment's replies
    shell.frames(5);
    CHECK(showsSomewhere(*shell.application, "R\xC3\xA9ponse "));
    for (int i = 0; i < 6; ++i) {
        shell.press(Action::Down);  // near the end: the next page
    }
    shell.frames(5);
    std::size_t continuations = 0;
    for (const auto& request : shell.youtube.requestsTo("/youtubei/v1/next")) {
        continuations += request.body.find("\"continuation\"") != std::string::npos ? 1 : 0;
    }
    CHECK(continuations >= 3);  // first page, replies, next page
    shell.press(Action::Back);
    shell.frames(3);
    CHECK_FALSE(shell.application->hasModal());
    CHECK(shell.application->videoSource() != nullptr);
}

TEST_CASE("player: comments when the video page failed the first time: asked again on \"Comments\"") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.youtube.failNext("/youtubei/v1/next", 500);
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    pressAnywhere(shell, "Comments");
    shell.frames(5);
    REQUIRE(shell.application->hasModal());
    CHECK(showsSomewhere(*shell.application, "Premier commentaire de d\xC3\xA9monstration"));
}

TEST_CASE("player: a playlist's \"Play all\" plays its first video; a card plays from that video") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    youtube::Video playlist;
    playlist.playlist = true;
    playlist.id = "PLaklDK5Yxuss";
    playlist.title = "Lofi House 2026";
    playlist.videoCountText = "198 vid\xC3\xA9os";
    app::openPlaylist(shell.session, playlist);
    shell.frames(5);
    CHECK(shows(*shell.application, "Lofi House 2026"));
    CHECK(shows(*shell.application, "198 vid\xC3\xA9os"));
    pressAnywhere(shell, "Play all");
    shell.frames(5);
    auto players = shell.youtube.requestsTo("/youtubei/v1/player");
    REQUIRE_FALSE(players.empty());
    CHECK(players.back().body.find("jICmkF585LA") != std::string::npos);
    shell.press(Action::Back);
    shell.frames(5);
}

TEST_CASE("player: a pasted link in the search opens the player") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Search));
    shell.frames(5);
    auto* field = findView<ui::TextField>(shell.application->root());
    REQUIRE(shell.application->focus().focus(field));
    typeText(shell, "https://youtu.be/dQw4w9WgXcQ");
    shell.frames(5);
    CHECK(shell.application->videoSource() != nullptr);
    const auto players = shell.youtube.requestsTo("/youtubei/v1/player");
    REQUIRE_FALSE(players.empty());
    CHECK(players[0].body.find("dQw4w9WgXcQ") != std::string::npos);
}

TEST_CASE("player: ◯ on the error leaves the player (no dead screen behind a dismissed dialog)") {
    Shell shell;
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");  // refused by every client
    shell.frames(10);
    REQUIRE(shell.application->hasModal());
    shell.press(Action::Back);
    shell.frames(5);
    CHECK_FALSE(shell.application->hasModal());
    CHECK(shell.application->videoSource() == nullptr);
    CHECK(shows(*shell.application, "Accent colour"));  // back on the settings
}

TEST_CASE("player: a refusal without YouTube's words says only that YouTube refused it") {
    Shell shell;
    for (const char* client : {"VISIONOS", "ANDROID_VR", "IOS", "ANDROID"}) {
        shell.youtube.setPlayer(client, R"({"playabilityStatus":{"status":"UNPLAYABLE"}})");
    }
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    CHECK(shows(*shell.application, "YouTube refused this video."));
}

TEST_CASE("player: the audio language chosen from the menu") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    const std::string base = shell.youtube.baseUrl();
    const std::string clen = "clen=" + std::to_string(test::readFixture("media/clip.mp4").size());
    auto audio = [&](const char* lang, const char* name, bool original) {
        return std::string(R"({"itag":140,"url":")") + base + "/media/140?lang=" + lang + "&" + clen +
               R"(","mimeType":"audio/mp4; codecs=\"mp4a.40.2\"","audioTrack":{"id":")" + lang +
               R"(","displayName":")" + name + R"(","audioIsDefault":)" + (original ? "true" : "false") + "}}";
    };
    shell.youtube.setPlayer(
        "VISIONOS",
        R"({"playabilityStatus":{"status":"OK"},"videoDetails":{"title":"Demo","lengthSeconds":"4"},)"
        R"("streamingData":{"adaptiveFormats":[{"itag":137,"url":")" + base + "/media/137?" + clen +
            R"(","mimeType":"video/mp4; codecs=\"avc1.640028\"","height":1080},)" +
            audio("de.3", "Allemand", false) + "," + audio("en-US.4", "Anglais (US) original", true) + "]}}");
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Up);
    pressAnywhere(shell, "Audio");
    REQUIRE(shell.application->hasModal());
    CHECK(shows(*shell.application, "Anglais (US) original"));
    pressAnywhere(shell, "Allemand");
    bool german = false;
    for (int i = 0; i < 300 && !german; ++i) {  // mpv opens it on its own thread
        shell.frames(1);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        for (const auto& r : shell.youtube.requestsTo("/media/140")) {
            german = german || r.target.find("lang=de.3") != std::string::npos;
        }
    }
    CHECK(german);
}

TEST_CASE("player: resumed after its addresses expired, the video asks for new ones and goes on") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    auto withExpiry = [&](const std::string& expire) {
        std::string reply = shell.youtube.playerReply({137, 140});
        for (std::size_t at = 0; (at = reply.find("?clen=", at)) != std::string::npos; at += 1) {
            reply.replace(at, 1, "?expire=" + expire + "&");
        }
        return reply;
    };
    shell.youtube.setPlayer("VISIONOS", withExpiry("4102444800"));  // 2100: still good
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    const std::size_t asked = shell.youtube.requestsTo("/youtubei/v1/player").size();
    shell.press(Action::Confirm);  // pause
    shell.press(Action::Confirm);  // play: the addresses still work, nothing asked
    shell.frames(3);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/player").size() == asked);
    // the same addresses, now past their time (as after a night's pause)
    shell.youtube.setPlayer("VISIONOS", withExpiry("1"));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    shell.youtube.setPlayer("VISIONOS", withExpiry("4102444800"));
    const std::size_t before = shell.youtube.requestsTo("/youtubei/v1/player").size();
    shell.press(Action::Confirm);  // pause
    shell.press(Action::Confirm);  // play
    shell.frames(10);
    CHECK(shell.youtube.requestsTo("/youtubei/v1/player").size() > before);
    CHECK_FALSE(shell.application->hasModal());
    CHECK(picture(shell));
}

TEST_CASE("player: a video starts again where it was left, and says so; its place is kept when it closes") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.session.progress().set("dQw4w9WgXcQ", 2, 4, 1);  // the clip is 4 s long: too short to resume
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    CHECK_MESSAGE(shell.application->toastText().find("Resumed") == std::string::npos, shell.application->toastText());  // within the first 30 s: from the start
    shell.press(Action::Back);
    shell.frames(3);
    CHECK(shell.session.progress().find("dQw4w9WgXcQ"));  // kept as it closed
    // a long video left at 2:05
    shell.session.progress().set("dQw4w9WgXcQ", 125, 600, 2);
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    CHECK(shell.application->toastText() == "Resumed at 2:05");
    shell.press(Action::Back);
}

TEST_CASE("player: while seeking, YouTube's storyboard picture of the target shows above the bar") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    std::string reply = shell.youtube.playerReply({137, 140});
    reply.insert(reply.size() - 1, R"(,"storyboards":{"playerStoryboardSpecRenderer":{"spec":")" +
                                       shell.youtube.baseUrl() +
                                       R"(/img/sb/$L/$N.jpg?x=1|160#90#2#5#5#2000#M$M#rs$S"}})");
    shell.youtube.setPlayer("VISIONOS", reply);
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    ui::View* found = anyNamed(*shell.application->top(), "trickplay preview");
    auto* preview = dynamic_cast<app::TrickplayPreview*>(found);
    REQUIRE(preview != nullptr);
    CHECK_FALSE(preview->visible);
    shell.press(Action::Right);
    shell.frames(1);
    CHECK(preview->visible);
    CHECK(preview->wantedUrl().find("/img/sb/0/M0.jpg?x=1&sigh=rs$S") != std::string::npos);
    shell.press(Action::Back);
}

TEST_CASE("player: a video's chapters show their title in the bar") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.youtube.setChapters({{0, "Intro"}, {1500, "Middle"}, {3000, "End"}});
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    CHECK((showsSomewhere(*shell.application, "Intro") || showsSomewhere(*shell.application, "Middle") ||
           showsSomewhere(*shell.application, "End")));  // the 4 s clip plays on in real time
    shell.press(Action::Back);
}

TEST_CASE("player: Return YouTube Dislike's counts in the menu; none asked for when the setting is off") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    REQUIRE(shell.youtube.requestsTo("/votes").size() == 1);
    CHECK(shell.youtube.requestsTo("/votes")[0].target == "/votes?videoId=dQw4w9WgXcQ");
    shell.press(Action::Up);  // the menu: signed out, the counts beside Comments
    CHECK(showsSomewhere(*shell.application, "19,500,302 likes \xC2\xB7 521,783 dislikes"));
    shell.press(Action::Back);
    shell.press(Action::Back);
    shell.session.settings().dislikes = false;
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    CHECK(shell.youtube.requestsTo("/votes").size() == 1);  // not asked again
    shell.press(Action::Back);
}

TEST_CASE("queue: △ on a card queues the video; at the end of a video the queue's first plays, before À suivre") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Home));
    shell.frames(5);
    std::vector<app::VideoCard*> cards;
    findViews<app::VideoCard>(shell.application->root(), cards);
    for (app::VideoCard* card : cards) {  // the first card that is not a live (a live cannot be queued)
        REQUIRE(shell.application->focus().focus(card));
        shell.press(Action::Options);
        if (!shell.session.watchQueue().empty()) {
            break;
        }
    }
    REQUIRE(shell.session.watchQueue().size() == 1);
    CHECK(shell.application->toastText() == "Added to the queue \xC2\xB7 1");
    shell.press(Action::Options);  // not twice
    CHECK(shell.session.watchQueue().size() == 1);
    const std::string queued = shell.session.watchQueue().front().id;
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    shell.press(Action::Up);  // the menu shows the queue
    CHECK(findPressable(shell.application->root(), "Queue (1)") != nullptr);
    shell.press(Action::Confirm);  // its first button, Play: tests open a video paused on its first picture
    // the clip (4 s) plays to its end: the queued video comes next, and leaves the queue
    bool replaced = false;
    for (int i = 0; i < 1000 && !replaced; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        shell.frames(1);
        picture(shell);
        for (const auto& r : shell.youtube.requestsTo("/youtubei/v1/player")) {
            replaced = replaced || r.body.find(queued) != std::string::npos;
        }
    }
    CHECK(replaced);
    CHECK(shell.session.watchQueue().empty());
}

TEST_CASE("player: signed in, the video goes into the account's YouTube history; not with the setting off") {
    Shell shell;
    shell.youtube.servePlayableMedia();
    shell.youtube.setPlayer("VISIONOS", shell.youtube.playerReply({137, 140}));
    shell.application->replace(app::makeTab(shell.session, app::Tab::Settings));
    app::openVideo(shell.session, "dQw4w9WgXcQ");  // signed out: nothing reported
    shell.frames(10);
    REQUIRE(picture(shell));
    shell.press(Action::Back);
    CHECK(shell.youtube.requestsTo("/api/stats/playback").empty());

    signIn(shell);
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    REQUIRE(picture(shell));
    const auto players = shell.youtube.requestsTo("/youtubei/v1/player");
    REQUIRE_FALSE(players.empty());
    CHECK(players.back().clientName == "TVHTML5");  // the account's reporting addresses
    auto started = shell.youtube.requestsTo("/api/stats/playback");
    REQUIRE(started.size() == 1);
    CHECK(started[0].authorization == "Bearer demo-access-token");
    CHECK(started[0].target.find("docid=dQw4w9WgXcQ") != std::string::npos);
    const auto cpn = started[0].target.find("&ver=2&cpn=");
    REQUIRE(cpn != std::string::npos);
    const std::string nonce = started[0].target.substr(cpn + 11, 16);
    shell.press(Action::Back);
    shell.frames(3);
    for (const auto& part : shell.youtube.requestsTo("/api/stats/watchtime")) {  // the same playback's parts
        CHECK(part.target.find("&cpn=" + nonce + "&") != std::string::npos);
        CHECK(part.authorization == "Bearer demo-access-token");
    }

    shell.session.settings().watchHistory = false;
    app::openVideo(shell.session, "dQw4w9WgXcQ");
    shell.frames(10);
    shell.press(Action::Back);
    CHECK(shell.youtube.requestsTo("/api/stats/playback").size() == 1);  // not reported again
}
