#pragma once
// ---------------------------------------------------------------------------
// SoundEvents — OpenTX-style condition table for voice/sound alerts, driven by
// the values we actually have: the MSP DisplayPort OSD canvas plus the arm bit.
//
// WHY IT IS BUILT THIS WAY
//
// OpenTX drives audio from a table of "special functions": each row pairs a
// CONDITION with an ACTION, the table is walked every cycle, and a per-row
// repeat delay decides whether a still-true condition re-announces
// (radio/src/functions.cpp:105, isRepeatDelayElapsed). We keep that table shape
// because it is the right one -- declarative rows a user can edit -- but we
// FIRE ON STATE CHANGE rather than on "condition is true", matching the
// arm-triggered recording already in this player: remember the previous state,
// and act on the transition. A pilot wants to hear "battery low" at the moment
// it becomes low, not on whichever cycle the table happened to be walked.
//
// WHERE THE VALUES COME FROM. A census of the live wire (18 s, msposd -d)
// showed only three MSP commands ever arrive:
//     cmd 182 MSP_DISPLAYPORT  x4104   the OSD glyph stream
//     cmd 101 MSP_STATUS       x18     flightModeFlags -> the arm bit
//     cmd   2 MSP_FC_VARIANT   x18     "BTFL"
// There is NO MSP_ANALOG (110) and NO MSP_BATTERY_STATE (130): msposd does not
// poll them, so battery voltage, mAh and RSSI never arrive as numbers. They ARE
// on screen though, as glyphs in the DisplayPort canvas we already decode. So
// numeric variables are SCRAPED FROM THE CANVAS TEXT. That sounds indirect, but
// it is strictly more general -- anything the pilot can see becomes available,
// with no dependency on msposd requesting extra commands.
//
// Scraping is PATTERN-based, not position-based (no "voltage lives at row 1,
// col 3"), so it survives OSD layout changes and different Betaflight элements
// arrangements. Betaflight renders e.g. "16.2V", "1250mAh", "98%".
//
// UNKNOWN IS NOT ZERO. A variable that could not be scraped is absent, and a
// row referencing an absent variable never fires. Treating "not found" as 0
// would announce "battery critical" the moment the OSD scrolls or the link
// drops, which is exactly the wrong behaviour in flight.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cctype>
#include <map>
#include <string>
#include <vector>

namespace apfpv {

// --------------------------------------------------------------------------
// Variables extracted from the OSD canvas (plus `armed`, which is structured).
// --------------------------------------------------------------------------
struct EventVars {
    std::map<std::string, double> num;   // vbat, mah, lq, alt, speed, ...
    bool armed = false;
    bool haveArmed = false;

    bool get(const std::string& k, double& v) const {
        auto it = num.find(k);
        if (it == num.end()) return false;
        v = it->second;
        return true;
    }
};

// --------------------------------------------------------------------------
// Betaflight OSD font symbols (src/main/drivers/osd_symbols.h). The UNITS are
// font glyphs, not letters -- this is the whole reason values are read from the
// parser's WRITE_STRING runs rather than from an ASCII view of the canvas.
// Confirmed against a live capture documented in msp_osd.h:
//     03 0f 02 40 96 30 2e 30 32 06
//     glyphs {0x96,'0','.','0','2',0x06} = the battery-voltage field
// i.e. battery icon, ASCII digits, then SYM_VOLT.
// --------------------------------------------------------------------------
enum : uint8_t {
    SYM_RSSI      = 0x01,
    SYM_VOLT      = 0x06,
    SYM_MAH       = 0x07,
    SYM_METRE     = 0x0C,
    SYM_PCT       = '%',    // percent IS ascii in this font
};

// --------------------------------------------------------------------------
// Parse ONE WRITE_STRING run into whatever variable it represents.
//
// Feed this from the DpTap installed on feedDisplayPort. A run is a single OSD
// element, so the number and its unit symbol are guaranteed to belong together
// -- no risk of pairing a number with a neighbouring field's unit.
//
// Digits and '.' are ASCII; the unit is a symbol byte that may appear before or
// after the number depending on the element, so both sides are checked.
// --------------------------------------------------------------------------
inline void feedOsdRun(const uint8_t* g, int n, EventVars& out) {
    if (!g || n <= 0) return;
    // Locate a contiguous ASCII number inside the run.
    int s0 = -1, s1 = -1;
    bool dot = false;
    for (int i = 0; i < n; i++) {
        bool isNum = (g[i] >= '0' && g[i] <= '9') || (g[i] == '.' && !dot && s0 >= 0);
        if (isNum) {
            if (g[i] == '.') dot = true;
            if (s0 < 0) s0 = i;
            s1 = i;
        } else if (s0 >= 0) {
            break;                       // first number only; runs hold one value
        }
    }
    if (s0 < 0) return;
    char buf[24] = {0};
    int len = s1 - s0 + 1;
    if (len >= (int) sizeof(buf)) return;
    std::memcpy(buf, g + s0, len);
    double val = std::atof(buf);

    // The unit symbol sits immediately after the number, or immediately before
    // the leading icon+number pair. Check both, nearest first.
    uint8_t after  = (s1 + 1 < n) ? g[s1 + 1] : 0;
    uint8_t before = (s0 - 1 >= 0) ? g[s0 - 1] : 0;
    auto unit = [&](uint8_t u) { return after == u || before == u; };

    if (unit(SYM_VOLT)) {
        // Bound it to plausible pack voltages so a stray value cannot pose as vbat.
        if (val >= 2.0 && val <= 60.0) out.num["vbat"] = val;
    } else if (unit(SYM_MAH)) {
        out.num["mah"] = val;
    } else if (unit(SYM_RSSI) || unit(SYM_PCT)) {
        if (val >= 0.0 && val <= 100.0) out.num["lq"] = val;
    } else if (unit(SYM_METRE)) {
        out.num["alt"] = val;
    }
    // No recognised unit -> not a variable we know. Deliberately ignored rather
    // than guessed at: a wrong mapping here would fire the wrong alert.
}

// --------------------------------------------------------------------------
// One row of the condition table.
// --------------------------------------------------------------------------
struct SoundRule {
    std::string name;                 // for logs
    std::string var;                  // "armed" or a scraped numeric name
    enum Op { IsTrue, IsFalse, Lt, Gt } op = IsTrue;
    double      threshold = 0;
    double      hyst = 0;             // deadband; see below
    // A SEQUENCE, exactly like OpenTX's audioQueue: an announcement is built by
    // concatenating small clips ("battry" + "0030" + "percent0" = "battery thirty
    // percent"). Each token is either a word file under sounds/ or "#var", which
    // is replaced at fire time by the spoken value of that variable.
    std::vector<std::string> seq;
    int         repeatSec = 0;        // 0 = fire once per activation (OpenTX repeat=0)

    // --- runtime state (the "previous state" half of the edge trigger) ---
    bool      active = false;         // was the condition true last evaluation?
    long long lastFiredMs = 0;
};

// --------------------------------------------------------------------------
// The table + its evaluation.
//
// Playback is injected as a callback so this header stays free of any audio
// backend, exactly like WifiLink keeps the OS out of the player.
// --------------------------------------------------------------------------
class SoundEvents {
public:
    /// Receives the whole playlist for one event; the backend queues the clips
    /// back-to-back (OpenTX plays a queue, it does not preempt).
    using PlayFn = void (*)(const std::vector<std::string>& clips, void* user);

    void setPlayer(PlayFn fn, void* user) { _play = fn; _user = user; }

    /// Parse the config. Unknown keys and malformed lines are skipped with a
    /// warning rather than aborting -- a typo should cost one alert, not all of them.
    bool load(const std::string& path) {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) return false;
        char line[512];
        int lineNo = 0;
        while (std::fgets(line, sizeof(line), f)) {
            lineNo++;
            std::string s(line);
            // Strip comments, but ONLY where '#' begins a word: the value-slot syntax
            // is "#var" (say=battry,#vbat,volt1), so a bare find('#') would silently
            // truncate every spoken-value rule -- which it did, until this was fixed.
            for (size_t h = 0; h < s.size(); h++) {
                if (s[h] != '#') continue;
                if (h == 0 || std::isspace((unsigned char) s[h - 1])) { s = s.substr(0, h); break; }
            }
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r' ||
                                  s.back() == ' '  || s.back() == '\t')) s.pop_back();
            if (s.empty()) continue;

            // event <name> when=<expr> sound=<file> [repeat=N] [hyst=X]
            if (s.compare(0, 6, "event ") != 0) {
                std::fprintf(stderr, "sounds: %s:%d ignored (expected 'event ...')\n",
                             path.c_str(), lineNo);
                continue;
            }
            SoundRule r;
            std::vector<std::string> tok;
            for (size_t i = 6; i < s.size();) {
                while (i < s.size() && std::isspace((unsigned char) s[i])) i++;
                size_t st = i;
                while (i < s.size() && !std::isspace((unsigned char) s[i])) i++;
                if (i > st) tok.push_back(s.substr(st, i - st));
            }
            if (tok.empty()) continue;
            r.name = tok[0];
            for (size_t i = 1; i < tok.size(); i++) {
                const std::string& t = tok[i];
                size_t eq = t.find('=');
                if (eq == std::string::npos) continue;
                std::string k = t.substr(0, eq), v = t.substr(eq + 1);
                if      (k == "sound" || k == "say") {
                    // Comma-separated tokens: word files and/or #var value slots.
                    size_t st = 0;
                    while (st <= v.size()) {
                        size_t c = v.find(',', st);
                        std::string t = v.substr(st, (c == std::string::npos ? v.size() : c) - st);
                        if (!t.empty()) r.seq.push_back(t);
                        if (c == std::string::npos) break;
                        st = c + 1;
                    }
                }
                else if (k == "repeat") r.repeatSec = std::atoi(v.c_str());
                else if (k == "hyst")   r.hyst = std::atof(v.c_str());
                else if (k == "when")   { if (!parseWhen(v, r)) r.var.clear(); }
            }
            if (r.var.empty() || r.seq.empty()) {
                std::fprintf(stderr, "sounds: %s:%d '%s' skipped (bad when= or missing sound=/say=)\n",
                             path.c_str(), lineNo, r.name.c_str());
                continue;
            }
            _rules.push_back(r);
        }
        std::fclose(f);
        std::fprintf(stderr, "sounds: %zu rule(s) from %s\n", _rules.size(), path.c_str());
        return !_rules.empty();
    }

    /// Evaluate the table. Call at whatever rate suits (~2-5 Hz is plenty).
    ///
    /// EDGE-TRIGGERED: a rule fires when its condition transitions false->true.
    /// While it stays true, it re-fires only if repeatSec > 0 and that long has
    /// passed -- OpenTX's repeat semantics, but anchored to the transition
    /// rather than to table-walk timing.
    void evaluate(const EventVars& v, long long nowMs) {
        for (SoundRule& r : _rules) {
            bool truth;
            if (!conditionTruth(r, v, truth)) continue;   // variable absent -> never fires

            if (truth && !r.active) {                     // ---- the edge ----
                r.active = true;
                fire(r, nowMs, v);
            } else if (truth && r.active) {
                if (r.repeatSec > 0 && nowMs - r.lastFiredMs >= (long long) r.repeatSec * 1000)
                    fire(r, nowMs, v);
            } else if (!truth && r.active) {
                // Condition cleared: re-arm so the next crossing announces again.
                r.active = false;
            }
        }
    }

    size_t ruleCount() const { return _rules.size(); }

private:
    static bool parseWhen(const std::string& w, SoundRule& r) {
        if (w == "armed")  { r.var = "armed"; r.op = SoundRule::IsTrue;  return true; }
        if (w == "!armed") { r.var = "armed"; r.op = SoundRule::IsFalse; return true; }
        size_t p = w.find_first_of("<>");
        if (p == std::string::npos || p == 0 || p + 1 >= w.size()) return false;
        r.var       = w.substr(0, p);
        r.op        = (w[p] == '<') ? SoundRule::Lt : SoundRule::Gt;
        r.threshold = std::atof(w.c_str() + p + 1);
        return true;
    }

    /// Returns false when the rule cannot be judged (variable not present).
    static bool conditionTruth(const SoundRule& r, const EventVars& v, bool& truth) {
        if (r.var == "armed") {
            if (!v.haveArmed) return false;
            truth = (r.op == SoundRule::IsTrue) ? v.armed : !v.armed;
            return true;
        }
        double val;
        if (!v.get(r.var, val)) return false;
        // HYSTERESIS. Crossing in needs the plain threshold; releasing needs to
        // clear it by `hyst`. Without this a value resting on the threshold
        // toggles active/inactive and announces on every wobble.
        if (r.op == SoundRule::Lt)
            truth = r.active ? (val < r.threshold + r.hyst) : (val < r.threshold);
        else
            truth = r.active ? (val > r.threshold - r.hyst) : (val > r.threshold);
        return true;
    }

    /// Expand the token sequence into a playlist and hand it to the backend.
    ///
    /// Numbers follow OpenTX's scheme -- sounds/num/NNNN.wav, ONE clip per value, so
    /// "30" is a single recording rather than "three"+"zero". The extracted pack covers
    /// 0..100; a value outside that range, or one whose clip is absent, has its slot
    /// DROPPED rather than substituted, because a misspoken number is worse than a
    /// missing one ("battery zero" when the reading was simply unavailable).
    void fire(SoundRule& r, long long nowMs, const EventVars& v) {
        r.lastFiredMs = nowMs;
        std::vector<std::string> clips;
        for (const std::string& t : r.seq) {
            if (t.size() > 1 && t[0] == '#') {
                double val = 0;
                if (!v.get(t.substr(1), val)) continue;       // unknown -> omit the slot
                long iv = (long) (val + 0.5);                 // nearest; pack has no decimals
                if (iv < 0 || iv > 100) continue;
                char nb[32];
                std::snprintf(nb, sizeof(nb), "num/%04ld.wav", iv);
                clips.emplace_back(nb);
            } else {
                clips.push_back(t.find('.') == std::string::npos ? t + ".wav" : t);
            }
        }
        if (clips.empty()) return;
        std::string joined;
        for (auto& c : clips) { joined += c; joined += ' '; }
        std::fprintf(stderr, "sounds: %s -> %s\n", r.name.c_str(), joined.c_str());
        if (_play) _play(clips, _user);
    }

    std::vector<SoundRule> _rules;
    PlayFn _play = nullptr;
    void*  _user = nullptr;
};

} // namespace apfpv
