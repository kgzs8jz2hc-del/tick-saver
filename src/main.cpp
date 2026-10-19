// Tick Saver - input buffering / tick saving study tool for Geometry Dash 2.2 (Geode).
//
// HALF-TICK DECISION (requirement 6)
// ----------------------------------
// One "tick" == one call of GJBaseGameLayer::processCommands(dt) on the PlayLayer.
// The tick counter is an integer; there are NO half ticks. An input belongs to the
// tick in whose processCommands call GJBaseGameLayer::handleButton fires for it
// (i.e. sub-step timing is floored to the step that consumes it). Snapshots are
// taken at the START of a tick (before inputs of that tick are applied), and each
// snapshot stores the dt that tick was run with, so replays use identical steps
// even if a TPS bypass / time warp is active.
//
// OVERVIEW
// --------
//  * every tick (when N > 0): snapshot via PlayLayer::createCheckpoint (retained)
//    into a ring buffer of depth 2N+4
//  * every click/release is logged with its tick (handleButton hook)
//  * destroyPlayer is swallowed and remembered; at the start of the next tick we
//    try shifts +1,-1,+2,-2..+-N of the most recent input: load an older snapshot,
//    replay the log, and see whether the player survives ~100 ms past the death tick
//  * first surviving shift is committed (state rebuilt at tick T+1, ring rewritten,
//    log rewritten); if none survives the original death is replayed normally
//  * all replay work runs under g.inReplay, so injected inputs are not re-logged
//    and the death hook only records "trial died" instead of killing the player

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/loader/SettingV3.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <utility>
#include <vector>

using namespace geode::prelude;

namespace ts {

constexpr int kShiftTable[] = {0, 1, 2, 4, 8, 16};
constexpr int kShiftCount = 6;
constexpr double kSurviveSeconds = 0.100;

struct Snap {
    int64_t tick;
    CheckpointObject* cp; // retained by us
    float dt;
};

struct Input {
    int64_t tick;
    bool down;
    int button;
    bool p1;
    bool synthetic; // produced by a shift (must be injected if it lies in the future)
};

struct State {
    int shiftIdx = 0;
    int64_t tick = 0;
    float lastDt = 1.f / 240.f;

    std::deque<Snap> ring;
    std::vector<Input> log;           // sorted by tick
    std::vector<Input> pendingInject; // synthetic inputs scheduled for future ticks

    // re-entrancy guards
    bool inReplay = false;  // we are driving physics ourselves
    bool injecting = false; // we are feeding a synthetic input to the real run
    bool letDie = false;    // let the next destroyPlayer through

    bool trialDied = false;
    bool pending = false;
    bool cheated = false;
    int64_t deathTick = 0;
    PlayerObject* deathPlayer = nullptr;
    GameObject* deathObj = nullptr;
};

inline State g;

inline int maxShift() { return kShiftTable[g.shiftIdx]; }
inline size_t ringDepth() { return static_cast<size_t>(2 * maxShift() + 4); }

inline void freeSnap(Snap& s) {
    if (s.cp) {
        s.cp->release();
        s.cp = nullptr;
    }
}

inline void clearRing() {
    for (auto& s : g.ring) freeSnap(s);
    g.ring.clear();
}

inline void resetAll() {
    clearRing();
    g.log.clear();
    g.pendingInject.clear();
    g.tick = 0;
    g.pending = false;
    g.inReplay = false;
    g.injecting = false;
    g.letDie = false;
    g.trialDied = false;
    g.cheated = false;
    g.deathPlayer = nullptr;
    g.deathObj = nullptr;
}

inline void insertSorted(std::vector<Input>& v, Input const& in) {
    auto it = std::upper_bound(v.begin(), v.end(), in.tick,
        [](int64_t t, Input const& e) { return t < e.tick; });
    v.insert(it, in);
}

// Re-create the held-button state that the log says is active at tick t.
// Must be called with g.inReplay (or g.injecting) set so nothing is logged.
inline void applyHeldState(GJBaseGameLayer* self, std::vector<Input> const& log, int64_t t) {
    for (int b = 1; b <= 3; ++b) {
        self->handleButton(false, b, true);
        self->handleButton(false, b, false);
    }
    std::map<std::pair<int, bool>, bool> held;
    for (auto const& e : log) {
        if (e.tick >= t) break;
        held[{e.button, e.p1}] = e.down;
    }
    for (auto const& [key, isDown] : held) {
        if (isDown) self->handleButton(true, key.first, key.second);
    }
}

inline float dtFor(int64_t t, float curDt) {
    for (auto const& s : g.ring) {
        if (s.tick == t) return s.dt;
    }
    return curDt;
}

// Phase A (collect == false): replay from `base` and report whether the player
// survives until T + surviveTicks.
// Phase B (collect == true): replay again, taking a snapshot at the start of every
// tick up to and including T+1 (the state we continue the real run from).
inline bool runTrial(GJBaseGameLayer* self, PlayLayer* pl, Snap const& base,
                     std::vector<Input> const& cand, int64_t T, float curDt,
                     int surviveTicks, bool collect, std::vector<Snap>* out) {
    g.trialDied = false;
    pl->loadFromCheckpoint(base.cp);
    applyHeldState(self, cand, base.tick);

    size_t ei = 0;
    while (ei < cand.size() && cand[ei].tick < base.tick) ++ei;

    int64_t last = collect ? (T + 1) : (T + surviveTicks);
    for (int64_t t = base.tick; t <= last; ++t) {
        if (collect) {
            auto* cp = pl->createCheckpoint();
            if (cp) cp->retain();
            out->push_back({t, cp, dtFor(t, curDt)});
            if (t == T + 1) break; // inputs of T+1 are applied by the real run
        }
        while (ei < cand.size() && cand[ei].tick <= t) {
            self->handleButton(cand[ei].down, cand[ei].button, cand[ei].p1);
            ++ei;
        }
        self->processCommands(dtFor(t, curDt));
        if (g.trialDied) return false;
    }
    return true;
}

// Called at the start of tick T+1 with g.inReplay already set.
// Returns true if a shifted replay was committed.
inline bool tryRescue(GJBaseGameLayer* self, PlayLayer* pl, float curDt) {
    int const N = maxShift();
    int64_t const T = g.deathTick;

    int li = -1;
    for (int i = static_cast<int>(g.log.size()) - 1; i >= 0; --i) {
        if (g.log[i].tick <= T) { li = i; break; }
    }
    if (li < 0) return false;
    Input const I = g.log[li];

    int64_t prevSame = 0;
    for (int i = li - 1; i >= 0; --i) {
        if (g.log[i].button == I.button && g.log[i].p1 == I.p1) { prevSame = g.log[i].tick; break; }
    }

    int surviveTicks = static_cast<int>(std::ceil(kSurviveSeconds / std::max(1e-6f, curDt)));
    surviveTicks = std::clamp(surviveTicks, 1, 240);

    // Save the present (post-swallowed-death) state so a failed rescue is lossless.
    CheckpointObject* cur = pl->createCheckpoint();
    if (cur) cur->retain();
    auto savedQueue = pl->m_queuedButtons; // real inputs waiting for the next tick

    bool committed = false;
    for (int k = 1; k <= N && !committed; ++k) {
        for (int sgn : {1, -1}) {
            int64_t nt = I.tick + static_cast<int64_t>(sgn) * k;
            if (nt < 0 || nt < prevSame) continue;

            int64_t const need = std::min(I.tick, nt);
            Snap const* base = nullptr;
            for (auto it = g.ring.rbegin(); it != g.ring.rend(); ++it) {
                if (it->tick <= need && it->cp) { base = &*it; break; }
            }
            if (!base) continue;
            int64_t const baseTick = base->tick;

            auto cand = g.log;
            cand[li].tick = nt;
            cand[li].synthetic = true;
            std::stable_sort(cand.begin(), cand.end(),
                [](Input const& a, Input const& b) { return a.tick < b.tick; });

            pl->m_queuedButtons.clear();
            if (!runTrial(self, pl, *base, cand, T, curDt, surviveTicks, false, nullptr)) continue;

            std::vector<Snap> snaps;
            pl->m_queuedButtons.clear();
            bool ok = runTrial(self, pl, *base, cand, T, curDt, surviveTicks, true, &snaps);
            if (!ok || snaps.empty() || snaps.back().tick != T + 1 || !snaps.back().cp) {
                for (auto& s : snaps) freeSnap(s);
                continue;
            }

            // ---- commit ----
            pl->loadFromCheckpoint(snaps.back().cp);
            applyHeldState(self, cand, T + 1);

            while (!g.ring.empty() && g.ring.back().tick >= baseTick) {
                freeSnap(g.ring.back());
                g.ring.pop_back();
            }
            for (auto& s : snaps) {
                if (s.tick <= T) g.ring.push_back(s); // ownership moves to the ring
                else freeSnap(s);
            }

            g.log = std::move(cand);
            g.pendingInject.clear();
            for (auto const& e : g.log) {
                if (e.synthetic && e.tick >= T + 1) g.pendingInject.push_back(e);
            }
            g.tick = T + 1;
            g.cheated = true;
            committed = true;
            log::info("Tick Saver: saved death at tick {} (input {} shifted {:+})", T, I.tick, sgn * k);
            break;
        }
    }

    pl->m_queuedButtons = savedQueue;
    if (!committed) {
        pl->loadFromCheckpoint(cur);
        applyHeldState(self, g.log, T + 1);
    }
    if (cur) cur->release();
    return committed;
}

inline void cycleShift() {
    g.shiftIdx = (g.shiftIdx + 1) % kShiftCount;
    Mod::get()->setSavedValue("shift-index", g.shiftIdx);
    int n = maxShift();
    std::string msg = n == 0
        ? std::string("Tick Saver: off")
        : fmt::format("Tick Saver: +/-{} ticks (~{:.1f} ms)", n, n * 1000.0 / 240.0);
    log::info("{}", msg);
    Notification::create(msg, NotificationIcon::None, 1.0f)->show();
}

} // namespace ts

$execute {
    int saved = Mod::get()->getSavedValue<int>("shift-index", 0);
    ts::g.shiftIdx = std::clamp(saved, 0, ts::kShiftCount - 1);

    listenForKeybindSettingPresses("cycle-shift",
        [](Keybind const&, bool down, bool repeat, double) {
            if (down && !repeat) ts::cycleShift();
            return false;
        });
}

class $modify(TSBase, GJBaseGameLayer) {
    void processCommands(float dt) {
        if (ts::g.inReplay) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        auto* pl = PlayLayer::get();
        auto* self = static_cast<GJBaseGameLayer*>(this);
        if (!pl || static_cast<GJBaseGameLayer*>(pl) != self) {
            GJBaseGameLayer::processCommands(dt);
            return;
        }

        auto& g = ts::g;
        g.lastDt = dt;

        // 1) resolve a swallowed death from the previous tick
        if (g.pending) {
            bool saved = false;
            if (ts::maxShift() > 0) {
                g.inReplay = true;
                saved = ts::tryRescue(self, pl, dt);
                g.inReplay = false;
            }
            g.pending = false;
            if (!saved) {
                g.letDie = true;
                pl->destroyPlayer(g.deathPlayer, g.deathObj);
                g.letDie = false;
            }
        }

        if (ts::maxShift() > 0) {
            // 2) feed synthetic (shifted) inputs that now fall due
            if (!g.pendingInject.empty()) {
                g.injecting = true;
                for (size_t i = 0; i < g.pendingInject.size();) {
                    if (g.pendingInject[i].tick <= g.tick) {
                        auto const e = g.pendingInject[i];
                        g.pendingInject.erase(g.pendingInject.begin() + i);
                        self->handleButton(e.down, e.button, e.p1);
                    } else {
                        ++i;
                    }
                }
                g.injecting = false;
            }

            // 3) snapshot the state at the start of this tick
            bool dead = pl->m_player1 && pl->m_player1->m_isDead;
            if (!dead) {
                if (auto* cp = pl->createCheckpoint()) {
                    cp->retain();
                    g.ring.push_back({g.tick, cp, dt});
                }
                while (g.ring.size() > ts::ringDepth()) {
                    ts::freeSnap(g.ring.front());
                    g.ring.pop_front();
                }
            }
        } else if (!g.ring.empty()) {
            ts::clearRing(); // feature switched off: free everything
        }

        GJBaseGameLayer::processCommands(dt);
        g.tick++;
    }

    void handleButton(bool down, int button, bool isPlayer1) {
        auto& g = ts::g;
        if (!g.inReplay && !g.injecting) {
            auto* pl = PlayLayer::get();
            if (pl && static_cast<GJBaseGameLayer*>(pl) == static_cast<GJBaseGameLayer*>(this)) {
                ts::insertSorted(g.log, {g.tick, down, button, isPlayer1, false});
            }
        }
        GJBaseGameLayer::handleButton(down, button, isPlayer1);
    }
};

class $modify(TSPlay, PlayLayer) {
    bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
        ts::resetAll();
        return PlayLayer::init(level, useReplay, dontCreateObjects);
    }

    void resetLevel() {
        if (ts::g.inReplay) {
            PlayLayer::resetLevel();
            return;
        }
        ts::resetAll();
        PlayLayer::resetLevel();
    }

    void onQuit() {
        ts::resetAll();
        PlayLayer::onQuit();
    }

    void destroyPlayer(PlayerObject* player, GameObject* obj) {
        auto& g = ts::g;
        if (g.letDie || ts::maxShift() == 0 || obj == m_anticheatSpike) {
            PlayLayer::destroyPlayer(player, obj);
            return;
        }
        if (g.inReplay) { // death inside a trial replay: just record it
            g.trialDied = true;
            return;
        }
        if (g.pending) return; // already swallowed one this tick
        g.pending = true;
        g.deathTick = g.tick;
        g.deathPlayer = player;
        g.deathObj = obj;
        // swallowed: the original is called later only if no shift survives
    }

    void levelComplete() {
        // A run that used a save is not legitimate: complete it as a test run so
        // nothing is submitted or saved.
        if (ts::g.cheated) m_isTestMode = true;
        PlayLayer::levelComplete();
    }
};
