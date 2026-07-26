#pragma once
// ---------------------------------------------------------------------------
// SoundPlayer — queued WAV playback for the voice alerts.
//
// Built on plain SDL2 audio (SDL_LoadWAV + SDL_QueueAudio) rather than
// SDL_mixer, so it adds NO dependency: SDL2 is already linked for the video
// window on every platform we target.
//
// WHY A QUEUE. OpenTX plays announcements through audioQueue and never preempts
// (radio/src/audio.cpp): a clip in progress finishes, and the next is played
// after it. That is what makes concatenated speech work at all -- "battery",
// "fifteen", "volts" are three files that must be heard in order, and it also
// means two events firing together are both heard instead of one cutting the
// other off. SDL_QueueAudio gives exactly this for free: samples are appended to
// the device queue and drain in order.
//
// Format conversion is done once per clip at load time via SDL_AudioCVT, so the
// EdgeTX pack (mono 32 kHz in practice) plays on a device opened at our own
// format without per-frame resampling. Clips are cached: each file is loaded and
// converted once, then reused, so an alert repeating every few seconds does no
// disk IO.
// ---------------------------------------------------------------------------

#include <SDL.h>

#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace apfpv {

class SoundPlayer {
public:
    /// `dir` is the folder holding the clips (word files and num/NNNN.wav).
    bool init(const std::string& dir) {
        _dir = dir;
        if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            std::fprintf(stderr, "sound: SDL audio init failed: %s\n", SDL_GetError());
            return false;
        }
        SDL_AudioSpec want{};
        want.freq     = 32000;      // the EdgeTX pack's native rate; avoids resampling
        want.format   = AUDIO_S16SYS;
        want.channels = 1;
        want.samples  = 1024;
        want.callback = nullptr;    // queue-driven, not callback-driven
        _dev = SDL_OpenAudioDevice(nullptr, 0, &want, &_have, 0);
        if (!_dev) {
            std::fprintf(stderr, "sound: no audio device: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(_dev, 0);
        std::fprintf(stderr, "sound: %d Hz %d ch, clips from %s\n",
                     _have.freq, _have.channels, _dir.c_str());
        return true;
    }

    void shutdown() {
        if (_dev) { SDL_CloseAudioDevice(_dev); _dev = 0; }
        for (auto& kv : _cache) SDL_free(kv.second.buf);
        _cache.clear();
    }

    void setEnabled(bool on) {
        _enabled = on;
        if (!on) clear();          // silence immediately when muted mid-announcement
    }
    bool enabled() const { return _enabled; }

    /// Queue a whole playlist. Clips play in order, after anything already queued.
    void play(const std::vector<std::string>& clips) {
        if (!_enabled || !_dev) return;
        // Guard against runaway backlog: if an alert repeats faster than it plays,
        // dropping the new one is better than building a lengthening delay between
        // the event and what the pilot hears.
        if (SDL_GetQueuedAudioSize(_dev) > (Uint32) (_have.freq * 2 * 3)) return;   // ~3 s
        for (const std::string& c : clips) {
            const Clip* cl = load(c);
            if (cl) SDL_QueueAudio(_dev, cl->buf, cl->len);
        }
    }

    void clear() { if (_dev) SDL_ClearQueuedAudio(_dev); }

private:
    struct Clip { Uint8* buf = nullptr; Uint32 len = 0; };

    /// Load + convert once, then cache. Returns null (and warns once) if missing.
    const Clip* load(const std::string& name) {
        auto it = _cache.find(name);
        if (it != _cache.end()) return it->second.buf ? &it->second : nullptr;

        Clip clip;
        SDL_AudioSpec spec{};
        Uint8* wav = nullptr; Uint32 wavLen = 0;
        std::string path = _dir + "/" + name;
        if (!SDL_LoadWAV(path.c_str(), &spec, &wav, &wavLen)) {
            std::fprintf(stderr, "sound: missing clip %s (%s)\n", path.c_str(), SDL_GetError());
            _cache[name] = clip;                 // negative-cache so it warns once
            return nullptr;
        }
        if (spec.freq == _have.freq && spec.format == _have.format &&
            spec.channels == _have.channels) {
            clip.buf = wav; clip.len = wavLen;   // already our format
        } else {
            SDL_AudioCVT cvt{};
            if (SDL_BuildAudioCVT(&cvt, spec.format, spec.channels, spec.freq,
                                  _have.format, _have.channels, _have.freq) < 0) {
                std::fprintf(stderr, "sound: cannot convert %s\n", name.c_str());
                SDL_FreeWAV(wav);
                _cache[name] = clip;
                return nullptr;
            }
            cvt.len = (int) wavLen;
            cvt.buf = (Uint8*) SDL_malloc((size_t) cvt.len * cvt.len_mult);
            std::memcpy(cvt.buf, wav, wavLen);
            SDL_FreeWAV(wav);
            SDL_ConvertAudio(&cvt);
            clip.buf = cvt.buf;
            clip.len = (Uint32) cvt.len_cvt;
        }
        _cache[name] = clip;
        return &_cache[name];
    }

    std::string          _dir;
    SDL_AudioDeviceID    _dev = 0;
    SDL_AudioSpec        _have{};
    bool                 _enabled = true;
    std::map<std::string, Clip> _cache;
};

} // namespace apfpv
