#pragma once

#include "NetworkProtocol.h"
#include "ClientSettings.h"
#include "VoiceMixer.h"
#include "VoiceProcessing.h"
#include "VoiceCapture.h"
#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>
#include <windows.h>
#include <mmsystem.h>

class cVoiceChat
{
    struct PlaybackBuffer
    {
        WAVEHDR header = {};
        __int16 samples[VOICE_SAMPLES_PER_PACKET] = {};
        bool queued = false;
    };

    VoiceCapture _capture;
    HWAVEOUT _waveOut;
    PlaybackBuffer _playbackStorage[4];
    std::deque<PlaybackBuffer*> _playbackBuffers;
    std::deque<NetworkVoicePacket> _captureQueue;
    cOpusCodec _opus;
    cVoiceProcessing _processing;
    typedef cVoiceProcessing::Frame AudioFrame;
    cVoiceMixer _mixer;
    bool _networkWasActive = false;
    bool _recording;
    bool _transmitEnabled;
    bool _buttonTransmitEnabled = false;
    bool _muted = false;
    unsigned __int64 _lastVoiceSent = 0;
    bool _captureFailed = false;
    bool _playbackReady;
    static constexpr unsigned ChimeSamples = VoiceAudioFormat::SampleRate * 320 / 1000;
    unsigned _chimeSample = ChimeSamples;
    unsigned __int32 _sequence;

    // All application capture state and codec access stays on the UI thread.
    void collectCapture(bool pushToTalk)
    {
        if(!_recording)return;
        if(!_capture.update()){captureFailed();return;}
        if(_capture.takeEchoReset())_processing.reset();
        AudioFrame microphone,reference,clean;
        while(_capture.pop(microphone,reference)){
            _processing.process(microphone.data(),reference,pushToTalk);
            while(_processing.pop(clean)){
                NetworkVoicePacket packet;packet.playerId=-1;packet.sequence=_sequence++;
                if(!_opus.encode(clean.data(),packet))EncodeAdpcmVoicePacket(clean.data(),packet);
                _captureQueue.push_back(packet);
                while(_captureQueue.size()>16)_captureQueue.pop_front();
            }
        }
    }

    void captureFailed()
    {
        Error("Microphone start or buffer submission failed");
        _captureFailed = true;
        _transmitEnabled = _buttonTransmitEnabled = false;
        stopRecording();
    }

    void openPlayback()
    {
        WAVEFORMATEX format;
        ZeroMemory(&format, sizeof(format));
        format.wFormatTag = WAVE_FORMAT_PCM;
        format.nChannels = 1;
        format.nSamplesPerSec = VOICE_SAMPLE_RATE;
        format.wBitsPerSample = 16;
        format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
        format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;


        _playbackReady = waveOutOpen(&_waveOut, WAVE_MAPPER, &format, 0, 0, CALLBACK_NULL) == MMSYSERR_NOERROR;
    }

    void startRecording(){
        if(_recording||_captureFailed)return;
        // stopRecording already clears capture history and drains the processor.
        if(!_capture.start()){captureFailed();return;}
        _recording=true;
    }
    void stopRecording(){
        if(!_recording)return;
        _capture.stop();_recording=false;
        _captureQueue.clear();_processing.reset(false);
    }

    bool popCapturedPacket(NetworkVoicePacket& packet)
    {
        bool result = false;
        if (!_captureQueue.empty())
        {
            packet = _captureQueue.front();
            _captureQueue.pop_front();
            result = true;
        }
        return result;
    }

    void playPacket(const NetworkVoicePacket& packet)
    {
        if (!_playbackReady)
        {
            return;
        }

        _mixer.push(packet, GetTickCount64());
    }

    void servicePlayback()
    {
        if (!_playbackReady) return;
        cleanupPlaybackBuffers();
        while (_playbackBuffers.size() < 4)
        {
            AudioFrame frame = _mixer.render(GetTickCount64());
            for(auto& sample : frame) {
                if(_chimeSample >= ChimeSamples) break;
                const double t = _chimeSample++ / double(VoiceAudioFormat::SampleRate);
                const double noteTime = t < 0.175 ? t : t - 0.175;
                if(noteTime >= 0.145) continue;
                const double frequency = t < 0.175 ? 523.25 : 783.99;
                const double envelope = sin(3.141592653589793 * noteTime / 0.145);
                const int tone = (int)(1800 * envelope * envelope * sin(2 * 3.141592653589793 * frequency * noteTime));
                sample = (__int16)(std::max)(-32768, (std::min)(32767, (int)sample + tone));
            }
            if (!submitPlayback(frame)) break;
        }
    }

    bool submitPlayback(const AudioFrame& frame)
    {
        PlaybackBuffer* buffer = nullptr;
        for (auto& slot : _playbackStorage) if (!slot.queued) { buffer = &slot; break; }
        if (!buffer) return false;
        std::copy(frame.begin(), frame.end(), buffer->samples);
        buffer->header.lpData = reinterpret_cast<LPSTR>(&buffer->samples[0]);
        buffer->header.dwBufferLength = sizeof(buffer->samples);
        buffer->header.dwUser = reinterpret_cast<DWORD_PTR>(buffer);

        if (!(buffer->header.dwFlags & WHDR_PREPARED) &&
            waveOutPrepareHeader(_waveOut, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            return false;
        }
        if (waveOutWrite(_waveOut, &buffer->header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR)
        {
            return false;
        }
        buffer->queued = true;
        _playbackBuffers.push_back(buffer);
        return true;
    }

    void cleanupPlaybackBuffers(bool force = false)
    {
        if (force && _playbackReady && waveOutReset(_waveOut) != MMSYSERR_NOERROR) return;
        if (force)
        {
            _mixer.clear();
            _processing.resetEcho();
        }
        while (!_playbackBuffers.empty())
        {
            PlaybackBuffer* buffer = _playbackBuffers.front();
            if (!force && !(buffer->header.dwFlags & WHDR_DONE))
            {
                break;
            }
            buffer->queued = false;
            _playbackBuffers.pop_front();
        }
    }

public:
    void cancelMessageChime() { _chimeSample = ChimeSamples; }
    bool messageChime() {
        if(!_playbackReady) return false;
        _chimeSample = 0;
        return true;
    }

    cVoiceChat()
        : _waveOut(NULL),
          _recording(false),
          _transmitEnabled(false),
          _playbackReady(false),
          _sequence(0)
    {
        static_assert(VOICE_SAMPLE_RATE == cVoiceProcessing::SampleRate &&
            VOICE_SAMPLES_PER_PACKET == cVoiceProcessing::FrameSamples, "Voice DSP format mismatch");
        openPlayback();
    }

    ~cVoiceChat()
    {
        stopRecording();
        if (_playbackReady)
        {
            waveOutReset(_waveOut);
            cleanupPlaybackBuffers(true);
            for (auto& buffer : _playbackStorage)
                if (buffer.header.dwFlags & WHDR_PREPARED)
                    waveOutUnprepareHeader(_waveOut, &buffer.header, sizeof(WAVEHDR));
            waveOutClose(_waveOut);
        }
    }

    bool transmitting(const cNetworkRuntime& network) const
    {
        return _transmitEnabled && _recording &&
               (network.clientReady() || network.isHost()) &&
               _lastVoiceSent != 0 && GetTickCount64() - _lastVoiceSent < 250;
    }

    void resetTransmitMode()
    {
        _transmitEnabled = false;
        _buttonTransmitEnabled = false;
        _lastVoiceSent = 0;
        stopRecording();
        NetworkVoicePacket discarded;
        while (popCapturedPacket(discarded)) {}
    }

    bool speaking(__int32 player, const cNetworkRuntime& network) const
    {
        return player == network.localPlayerId() ? transmitting(network) : _mixer.speaking(player, GetTickCount64());
    }

    int userVolume(int player) const {return _mixer.volume(player);}
    void setUserVolume(int player,int percent,const cNetworkRuntime& network,PackedClientSettings& settings) {
        for (const auto& person : network.participants()) if (person.first == player) {
            settings.setUserVolume(person.second,percent);
            _mixer.setVolume(player,percent);
            break;
        }
    }

    bool muted() const { return _muted; }
    void mute() {
        if (_muted) return;
        _muted = true;
        resetTransmitMode();
        cleanupPlaybackBuffers(true);
    }
    bool micLatched() const {return _buttonTransmitEnabled;}
    void restoreMicLatch(bool enabled){_buttonTransmitEnabled=enabled;}
    bool micEnabled() const { return _transmitEnabled && _recording; }

    void update(cNetworkRuntime& network, bool talkKeyDown, const PackedClientSettings& settings, bool micClicked = false)
    {
        _capture.pollOutputDevice();
        if(_capture.takeOutputChange()){
            cleanupPlaybackBuffers(true);
            if(_playbackReady){
                for(auto& buffer:_playbackStorage)if(buffer.header.dwFlags&WHDR_PREPARED)waveOutUnprepareHeader(_waveOut,&buffer.header,sizeof(WAVEHDR));
                waveOutClose(_waveOut);
            }
            for(auto& buffer:_playbackStorage)buffer=PlaybackBuffer{};
            _playbackReady=false;openPlayback();
        }
        const bool networkActive = network.clientReady() || network.isHost();
        if (_networkWasActive && !networkActive) {cleanupPlaybackBuffers(true);_mixer.clearVolumes();}
        _mixer.retainPlayers(network.participants());
        for (const auto& person : network.participants())
            _mixer.setVolume(person.first,settings.userVolume(person.second));
        _networkWasActive = networkActive;
        if (!talkKeyDown) _captureFailed = false;
        if (micClicked) {
            if (_muted) _muted = false;
            else _buttonTransmitEnabled = !_buttonTransmitEnabled;
        }
        // PTT takes over from a latched mic; releasing the key must mute it.
        if (!_muted && talkKeyDown) _buttonTransmitEnabled = false;
        _transmitEnabled = !_muted && (_buttonTransmitEnabled || talkKeyDown);

        if (_transmitEnabled)
        {
            startRecording();
        }
        else
        {
            stopRecording();
        }

        NetworkVoicePacket packet;
        while (network.consumeVoicePacket(packet)) if (!_muted) playPacket(packet);
        servicePlayback();
        collectCapture(talkKeyDown);
        while (popCapturedPacket(packet))
        {
            if (_transmitEnabled && _recording && (network.clientReady() || network.isHost()))
            {
                network.sendVoice(packet);
                _lastVoiceSent = GetTickCount64();
            }
        }

    }
};
