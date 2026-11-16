// Button navigation on the hub (D-pad / stick, A, B, Y), next to the laser:
//  - feedback that every menu screen gets without doing anything itself: a light tick when a
//    D-pad / stick step moved the focus, two short ticks when the step hit the end of a list, and
//    for A the click ring on the focused element plus one standard tick;
//  - the rail and the Files header row as focus zones (reached at the content's edge);
//  - the Downloads tab's focused card, its main action on A and its Options menu on Y.

#include "app/app.hpp"

#include <algorithm>
#include <cmath>

namespace pixelvr {

namespace {
using VR = VulkanRenderer;
constexpr float kStepPush = 0.38f;  // the push navStep / navStepX step at
}  // namespace

// ---------------------------------------------------------------------------
// Feedback

int App::navHand() const {
    // The laser hand: with one controller that is the one in the hand. (Which button was pressed
    // is not known per hand: the actions are bound in SteamVR.)
    const int h = renderer_.pointerHand();
    if (h == 0 || h == 1) return h;
    if (renderer_.laserOn(1)) return 1;
    if (renderer_.laserOn(0)) return 0;
    return 1;
}

void App::navFocusTick() {
    if (settings_.hoverHapticsEnabled()) xr_.triggerHaptic(navHand(), 0.008f, 0.0f, settings_.hoverHapticsStrength());
}

void App::navEndTick() {
    if (!settings_.hoverHapticsEnabled()) return;
    xr_.triggerHaptic(navHand(), 0.004f, 0.0f, settings_.hoverHapticsStrength());
    navEndTickPending_ = true;  // the second one a moment later (navFeedbackBegin)
    navEndTickAt_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(70);
}

void App::navActivateTick() { xr_.triggerHaptic(navHand(), 0.020f, 0.0f, 0.4f); }

std::vector<int> App::navFocusSnapshot() const {
    // Everything a D-pad / stick step can move. Equal before and after a step = it hit an end.
    std::vector<int> s = {actionMenu_.visible ? 1 : 0, actionMenu_.page, actionMenu_.selectedOption,
                          settings_.isOpen() ? 1 : 0, settings_.section(), settings_.selected(),
                          downloadsWindow_ ? 1 : 0, dlFocus_, hubNavZone_, hubNavId_};
#ifdef PIXELVR_HAVE_FREETYPE
    s.push_back(browser_.isOpen() ? 1 : 0);
    s.push_back(browser_.selected());
    s.push_back(renderer_.browserFirst());
#endif
    return s;
}

void App::navFeedbackBegin(const XrInputState& in, std::chrono::steady_clock::time_point now) {
    if (navEndTickPending_ && now >= navEndTickAt_) {
        navEndTickPending_ = false;
        xr_.triggerHaptic(navHand(), 0.004f, 0.0f, settings_.hoverHapticsStrength());
    }
    navBefore_ = navFocusSnapshot();
    navStepFired_ = false;
    navStickOwned_ = false;
    if (!in.select && !in.togglePlay) return;
    bool menus = settings_.isOpen() || downloadsWindow_ || actionMenu_.visible || smbModal_.active;
#ifdef PIXELVR_HAVE_FREETYPE
    menus = menus || browser_.isOpen();
#endif
    menus = menus && !textEntry_.active;  // (the keyboard's keys are pointed at)
    const bool onTarget = renderer_.laserOnPanelTarget();
    if (!menus || in.select || onTarget) {
        // The trigger, or A with the laser on what it acts on: the ring under the laser (the
        // renderer checks the laser is on the panel; on the dock it also dips the button).
        renderer_.notifyPress();
        if (menus && onTarget) navActivateTick();
        return;
    }
    // A with the laser elsewhere: it acts on the focused element, so the ring shows there.
    if (renderer_.notifyFocusPress()) navActivateTick();
}

void App::navFeedbackEnd() {
    if (!navStepFired_) return;
    navStepFired_ = false;
    if (navFocusSnapshot() != navBefore_) navFocusTick();
    else navEndTick();
}

// ---------------------------------------------------------------------------
// Rail and header zones

std::vector<int> App::railNavItems() const {
    // Top to bottom: Files, Downloads, Settings, the web shortcuts, the page's sections, Lock Vault.
    std::vector<int> items = {0, 1, 2};
    for (int k = 0; k < renderer_.railShortcutCount(); ++k) items.push_back(10 + k);
    for (int s = 0; s < renderer_.railSubCount(); ++s) items.push_back(3 + s);
    if (renderer_.vaultUnlocked()) items.push_back(9);
    return items;
}

std::vector<int> App::headerNavItems() const {
    std::vector<int> items;
#ifdef PIXELVR_HAVE_FREETYPE
    // Left to right as drawn: Sort, View, Bookmark, + Folder; choosing a folder: + Folder, Move
    // here (Copy here / Use this folder), Cancel.
    if (picking()) {
        if (browser_.canCreateFolder()) items.push_back(6);
        items.push_back(7);
        items.push_back(8);
    } else {
        items = {5, 2, 3};
        if (browser_.canCreateFolder()) items.push_back(6);
    }
#endif
    return items;
}

int App::railEntryId() const {
    // The place / filter / section in use, else the tab in use.
    const int sub = renderer_.railActiveSub();
    if (sub >= 0) return 3 + sub;
    return settings_.isOpen() ? 2 : (downloadsWindow_ ? 1 : 0);
}

int App::updateHubNav(XrInputState& in, std::chrono::steady_clock::time_point now, int sbHover, bool hubActive) {
    headerPress_ = -1;
    crumbPress_ = -1;
    int press = -1;
#ifdef PIXELVR_HAVE_FREETYPE
    const bool hubShown = browser_.isOpen() || settings_.isOpen() || downloadsWindow_;
    if (!hubShown) hubNavZone_ = VR::kNavContent;  // the hub opens with the focus in the content
    if (!hubActive || textEntry_.active) {  // a dialog / the keyboard has the buttons (no focus drawn then)
        hubNavHoverSeen_ = sbHover;
        renderer_.setHubNavFocus(hubNavZone_, hubNavId_);
        return -1;
    }
    const bool files = browser_.isOpen() && !downloadsWindow_ && !settings_.isOpen();
    const bool headerZone = hubNavZone_ == VR::kNavHeader || hubNavZone_ == VR::kNavCrumb;
    // A trigger click acts where the laser points: the focus goes back to the content (a click on
    // the rail itself keeps the rail).
    if (in.select && !(hubNavZone_ == VR::kNavRail && sbHover >= 0)) hubNavZone_ = VR::kNavContent;
    if (headerZone && (!files || browser_.isScanning())) hubNavZone_ = VR::kNavContent;
    // Another tab came up (bumpers, a click): a focused section item meant the old tab's list.
    const int page = settings_.isOpen() ? 2 : (downloadsWindow_ ? 1 : 0);
    if (hubNavZone_ == VR::kNavRail && page != hubNavPage_ && hubNavId_ >= 3 && hubNavId_ <= 9) {
        hubNavId_ = railEntryId();
    }
    hubNavPage_ = page;
    // The laser moving onto another rail item while the rail has the focus takes the focus along.
    if (hubNavZone_ == VR::kNavRail && sbHover >= 0 && sbHover != hubNavHoverSeen_) hubNavId_ = sbHover;
    hubNavHoverSeen_ = sbHover;

    auto consumeStick = [&]() {  // the content must not move while the focus is outside it
        in.thumbstickX = 0.0f;
        in.thumbstickY = 0.0f;
        navStickOwned_ = true;  // (a zeroed stick would re-arm navStep's repeat: it steps every frame)
    };
    const int cols = (renderer_.browserViewMode() == VR::BrowserViewMode::MiniCards) ? 4
                   : (renderer_.browserViewMode() == VR::BrowserViewMode::CompactList) ? 1 : 3;

    if (hubNavZone_ == VR::kNavContent && sbHover < 0) {
        // Only a fresh push leaves the content (holding a direction walks to the edge and stops there).
        const bool horiz = std::fabs(in.thumbstickX) >= std::fabs(in.thumbstickY);
        const bool freshLeft = navArmedX_ && in.thumbstickX < -kStepPush && horiz;
        const bool freshUp = navArmed_ && in.thumbstickY > kStepPush && !horiz;
        if (freshLeft) {
            // Left in the first column: onto the rail (Files, Downloads, Settings alike). The
            // Buttons pages keep Left / Right for turning the page.
            bool edge = false;
            if (settings_.isOpen()) {
                edge = !settings_.showingButtons() &&
                       (settings_.compactPage() || settings_.rows().empty() || settings_.selected() % 2 == 0);
            } else if (downloadsWindow_) {
                edge = true;
            } else if (files && !browser_.isScanning()) {
                edge = browser_.selected() % cols == 0;
            }
            if (edge) {
                navStepX(in.thumbstickX, now);
                hubNavZone_ = VR::kNavRail;
                hubNavId_ = railEntryId();
                consumeStick();
            }
        } else if (freshUp && files && !browser_.isScanning() && browser_.selected() < cols) {
            // Up in the top row: onto the header button above the card (Sort / View / ...).
            const std::vector<int> items = headerNavItems();
            if (!items.empty()) {
                navStep(in.thumbstickY, now);
                hubNavZone_ = VR::kNavHeader;
                const int col = std::max(0, browser_.selected()) % cols;
                hubNavId_ = items[static_cast<std::size_t>(std::min(col, static_cast<int>(items.size()) - 1))];
                consumeStick();
            }
        }
    } else if (hubNavZone_ == VR::kNavRail) {
        const std::vector<int> items = railNavItems();
        const bool onIncog = hubNavId_ >= 20 && renderer_.railShortcutHasIncog(hubNavId_ - 20);
        int base = onIncog ? hubNavId_ - 10 : hubNavId_;  // the shortcut tab an Incog badge belongs to
        auto it = std::find(items.begin(), items.end(), base);
        if (it == items.end()) {  // gone (another tab, the vault locked): the item in use
            base = hubNavId_ = railEntryId();
            it = std::find(items.begin(), items.end(), base);
        }
        const int pos = (it == items.end()) ? 0 : static_cast<int>(it - items.begin());
        if (const int dy = navStep(in.thumbstickY, now)) {
            const int np = std::clamp(pos + dy, 0, static_cast<int>(items.size()) - 1);
            hubNavId_ = items[static_cast<std::size_t>(np)];
        }
        if (const int dx = navStepX(in.thumbstickX, now)) {
            if (dx > 0 && !onIncog && base >= 10 && base < 20 && renderer_.railShortcutHasIncog(base - 10)) {
                hubNavId_ = base + 10;  // Right on a browser shortcut: its Incog badge first
            } else if (dx > 0) {
                hubNavZone_ = VR::kNavContent;  // Right: back into the content
            } else if (onIncog) {
                hubNavId_ = base;
            }
        }
        consumeStick();
        if (hubNavZone_ == VR::kNavRail && in.togglePlay && sbHover < 0) press = hubNavId_;
    } else if (hubNavZone_ == VR::kNavHeader) {
        const std::vector<int> items = headerNavItems();
        auto it = std::find(items.begin(), items.end(), hubNavId_);
        if (items.empty()) {
            hubNavZone_ = VR::kNavContent;
        } else {
            if (it == items.end()) {
                hubNavId_ = items.front();
                it = items.begin();
            }
            const int pos = static_cast<int>(it - items.begin());
            if (const int dx = navStepX(in.thumbstickX, now)) {
                hubNavId_ = items[static_cast<std::size_t>(std::clamp(pos + dx, 0, static_cast<int>(items.size()) - 1))];
            }
            if (const int dy = navStep(in.thumbstickY, now)) {
                const std::vector<int> levels = renderer_.browserCrumbLevels();
                if (dy > 0) {
                    hubNavZone_ = VR::kNavContent;  // Down: back to the cards
                } else if (!levels.empty()) {
                    hubNavZone_ = VR::kNavCrumb;  // Up: the folder path, starting at the parent
                    hubNavId_ = levels.back();
                }
            }
            if (hubNavZone_ == VR::kNavHeader && in.togglePlay && sbHover < 0 &&
                renderer_.browserPageBtnHovered() < 0 && renderer_.browserCrumbHovered() < 0) {
                headerPress_ = hubNavId_;
            }
        }
        consumeStick();
    } else if (hubNavZone_ == VR::kNavCrumb) {
        const std::vector<int> levels = renderer_.browserCrumbLevels();
        auto it = std::find(levels.begin(), levels.end(), hubNavId_);
        if (levels.empty()) {
            hubNavZone_ = VR::kNavHeader;
            hubNavId_ = -1;  // (the first header button, next frame)
        } else {
            if (it == levels.end()) {
                hubNavId_ = levels.back();
                it = levels.end() - 1;
            }
            const int pos = static_cast<int>(it - levels.begin());
            if (const int dx = navStepX(in.thumbstickX, now)) {
                hubNavId_ = levels[static_cast<std::size_t>(std::clamp(pos + dx, 0, static_cast<int>(levels.size()) - 1))];
            }
            if (const int dy = navStep(in.thumbstickY, now); dy > 0) {
                hubNavZone_ = VR::kNavHeader;  // Down: the buttons again
                const std::vector<int> items = headerNavItems();
                hubNavId_ = items.empty() ? -1 : items.front();
            }
            if (hubNavZone_ == VR::kNavCrumb && in.togglePlay && sbHover < 0 &&
                renderer_.browserPageBtnHovered() < 0 && renderer_.browserCrumbHovered() < 0) {
                crumbPress_ = hubNavId_;
                hubNavZone_ = VR::kNavContent;  // that folder opens: its cards next
            }
        }
        consumeStick();
    }
    renderer_.setHubNavFocus(hubNavZone_, hubNavId_);
#else
    (void)in;
    (void)now;
    (void)sbHover;
    (void)hubActive;
#endif
    return press;
}

bool App::hubNavBack(XrInputState& in) {
    if (!in.back || hubNavZone_ == VR::kNavContent || actionMenu_.visible || textEntry_.active) return false;
    bool hubShown = settings_.isOpen() || downloadsWindow_;
#ifdef PIXELVR_HAVE_FREETYPE
    hubShown = hubShown || browser_.isOpen();
#endif
    if (!hubShown) return false;
    in.back = false;
    hubNavZone_ = VR::kNavContent;  // one step back: from the rail / header into the content
    renderer_.setHubNavFocus(hubNavZone_, hubNavId_);
    xr_.triggerHaptic(navHand(), 0.025f, 0.0f, 0.5f);
    return true;
}

// ---------------------------------------------------------------------------
// Downloads

void App::updateDownloadsNav(XrInputState& in, std::chrono::steady_clock::time_point now, int sbHover) {
#ifdef PIXELVR_HAVE_FREETYPE
    const auto& jobs = browser_.downloadJobs();
    const int n = static_cast<int>(jobs.size());
    const std::vector<int> shown = renderer_.downloadsShownJobs();  // (same order as the jobs)
    auto shownPos = [&](int job) {
        const auto it = std::find(shown.begin(), shown.end(), job);
        return it == shown.end() ? -1 : static_cast<int>(it - shown.begin());
    };
    // The focused job by index, checked against its address (indexes move when jobs go).
    if (dlFocus_ >= 0 && (dlFocus_ >= n || jobs[static_cast<std::size_t>(dlFocus_)].uri != dlFocusUri_)) {
        int found = -1;
        for (int k = 0; k < n && found < 0; ++k) {
            if (jobs[static_cast<std::size_t>(k)].uri == dlFocusUri_) found = k;
        }
        dlFocus_ = found;
    }
    if (shownPos(dlFocus_) < 0) {  // gone or filtered out: the card now in its place
        dlFocus_ = shown.empty() ? -1 : shown[static_cast<std::size_t>(std::clamp(dlFocusPos_, 0, static_cast<int>(shown.size()) - 1))];
    }
    // The laser moving onto a card focuses it (a laser resting on one does not undo D-pad steps).
    const int hov = renderer_.downloadsHovered();
    if (hov >= 0 && hov != dlHoverSeen_ && shownPos(hov) >= 0) dlFocus_ = hov;
    dlHoverSeen_ = hov;
    int pos = std::max(0, shownPos(dlFocus_));
    if (hubNavZone_ == VR::kNavContent && sbHover < 0 && !shown.empty()) {
        if (const int dy = navStep(in.thumbstickY, now)) {
            pos = std::clamp(pos + dy, 0, static_cast<int>(shown.size()) - 1);
            dlFocus_ = shown[static_cast<std::size_t>(pos)];
        }
    }
    dlFocusPos_ = pos;
    dlFocusUri_ = (dlFocus_ >= 0 && dlFocus_ < n) ? jobs[static_cast<std::size_t>(dlFocus_)].uri : std::string();
    renderer_.setDownloadsFocus(dlFocus_);
    // A with the laser off the cards and buttons: the focused card's main action.
    if (in.togglePlay && hubNavZone_ == VR::kNavContent && sbHover < 0 && dlFocus_ >= 0 &&
        renderer_.downloadsHovered() < 0 && renderer_.downloadsButtonHovered() < 0) {
        downloadPrimaryAction(dlFocus_, in);
        in.togglePlay = false;
    }
#else
    (void)in;
    (void)now;
    (void)sbHover;
#endif
}

void App::downloadPrimaryAction(int job, XrInputState& in) {
#ifdef PIXELVR_HAVE_FREETYPE
    const auto& jobs = browser_.downloadJobs();
    if (job < 0 || job >= static_cast<int>(jobs.size())) return;
    const int state = jobs[static_cast<std::size_t>(job)].state;
    if (state == 1) {
        openFinishedDownload(job, in);  // Play (a folder: open it)
    } else if (state == 0) {
        browser_.togglePause();
        xr_.triggerHaptic(navHand(), 0.020f, 0.0f, 0.4f);
    } else if (state == 2 || state == 3) {
        browser_.retryJob(job);  // same place as before; refused while its SD card is out
        xr_.triggerHaptic(navHand(), 0.020f, 0.0f, 0.4f);
    }
    // (Queued: nothing to do until it starts; Y > Remove from Queue takes it out.)
#else
    (void)job;
    (void)in;
#endif
}

void App::openFinishedDownload(int job, XrInputState& in) {
#ifdef PIXELVR_HAVE_FREETYPE
    const auto& jobs = browser_.downloadJobs();
    if (job < 0 || job >= static_cast<int>(jobs.size())) return;
    const Browser::DownloadJob j = jobs[static_cast<std::size_t>(job)];  // (a copy: the list may change)
    if (j.state != 1 || j.localPath.empty()) return;
    if (j.isDir) {
        browser_.openPath(j.localPath);
        downloadsWindow_ = false;
    } else {
        // Like a file from the file browser: the screen picker / VR question appear in the Files hub.
        downloadsWindow_ = false;
        browser_.setOpen(true);
        playFromBrowser(j.localPath);
        // The picker it may open must not take this press too.
        in.select = false;
        in.togglePlay = false;
    }
#else
    (void)job;
    (void)in;
#endif
}

void App::openDownloadMenu() {
#ifdef PIXELVR_HAVE_FREETYPE
    const auto& jobs = browser_.downloadJobs();
    bool anyFinished = false;
    for (const auto& j : jobs) anyFinished = anyFinished || j.state == 1 || j.state == 2 || j.state == 3;
    const bool haveJob = dlFocus_ >= 0 && dlFocus_ < static_cast<int>(jobs.size());
    if (!haveJob && !anyFinished) return;  // nothing to offer
    actionMenu_.visible = true;
    actionMenu_.page = menupage::DownloadJob;
    actionMenu_.title = "Download Options";
    actionMenu_.subtitle.clear();
    actionMenu_.options.clear();
    actionMenu_.optionKinds.clear();
    actionMenu_.editingRow = -1;
    actionMenu_.selectedOption = 0;
    dlMenuUri_.clear();
    if (haveJob) {
        const auto& j = jobs[static_cast<std::size_t>(dlFocus_)];
        dlMenuUri_ = j.uri;
        actionMenu_.subtitle = j.name;
        if (j.state == 1) {
            actionMenu_.options.push_back(j.isDir ? "Open Folder" : "Play");
            actionMenu_.options.push_back("Remove from List");
        } else if (j.state == 0) {
            actionMenu_.options.push_back(browser_.downloadPaused() ? "Resume" : "Pause");
            actionMenu_.options.push_back("Stop Download");  // asks first: its part is deleted
        } else if (j.state == 2 || j.state == 3) {
            actionMenu_.options.push_back("Retry");
            actionMenu_.options.push_back("Remove from List");
        } else {
            actionMenu_.options.push_back("Remove from Queue");
        }
    }
    if (anyFinished) actionMenu_.options.push_back("Clear Completed");
    actionMenu_.options.push_back("Cancel");
#endif
}

void App::chooseDownloadMenu(const std::string& opt, XrInputState& in) {
#ifdef PIXELVR_HAVE_FREETYPE
    actionMenu_.visible = false;
    in.select = in.togglePlay = false;  // (a question it opens must not take this press)
    if (opt == "Cancel") return;
    if (opt == "Clear Completed") {
        browser_.clearFinished();
        xr_.triggerHaptic(navHand(), 0.030f, 0.0f, 0.5f);
        return;
    }
    const auto& jobs = browser_.downloadJobs();
    int job = -1;
    for (int k = 0; k < static_cast<int>(jobs.size()) && job < 0; ++k) {
        if (jobs[static_cast<std::size_t>(k)].uri == dlMenuUri_) job = k;
    }
    if (job < 0) return;  // gone meanwhile
    const int state = jobs[static_cast<std::size_t>(job)].state;
    if (opt == "Play" || opt == "Open Folder") {
        openFinishedDownload(job, in);
        return;
    }
    if (opt == "Stop Download") {
        if (state == 0) askStopDownload();
        return;
    }
    if ((opt == "Pause" || opt == "Resume") && state == 0) {
        browser_.togglePause();
    } else if (opt == "Retry") {
        browser_.retryJob(job);
    } else if (opt == "Remove from List" || opt == "Remove from Queue") {
        browser_.removeJob(job);  // a finished file stays on disk; queued ones just leave the queue
    }
    xr_.triggerHaptic(navHand(), 0.020f, 0.0f, 0.4f);
#else
    (void)opt;
    (void)in;
#endif
}

void App::askStopDownload() {
#ifdef PIXELVR_HAVE_FREETYPE
    std::string name, uri;
    for (const auto& j : browser_.downloadJobs()) {
        if (j.state == 0) {
            name = j.name;
            uri = j.uri;
            break;
        }
    }
    if (uri.empty()) return;  // nothing running any more
    askConfirm("Stop Download?", "Stop \"" + name + "\"? What is already downloaded is deleted.", "Yes, Stop",
               [this, uri] {
                   for (const auto& j : browser_.downloadJobs()) {  // still the same transfer?
                       if (j.state == 0 && j.uri == uri) {
                           browser_.stopDownload();
                           break;
                       }
                   }
               });
#endif
}

}  // namespace pixelvr
