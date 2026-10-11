#pragma once
#include "VoiceCodec.h"
#include <array>
#include <deque>
#include <map>
#include <memory>
#include <cstdint>

// One decoder and bounded jitter queue per speaker. Only the audio/UI thread
// accesses these streams. Network arrival rate never sets the playback rate.
class cVoiceMixer
{
public:
    typedef std::array<int16_t, VOICE_SAMPLES_PER_PACKET> Frame;
private:
    struct Stream
    {
        OpusDecoder* decoder;
        std::deque<NetworkVoicePacket> packets;
        uint64_t lastArrival = 0, queuedAt = 0, speakingUntil = 0;
        uint32_t next = 0;
        bool haveSequence = false, playing = false;
        int concealed = 0;
        Stream() { int error; decoder = opus_decoder_create(VOICE_SAMPLE_RATE, 1, &error); }
        ~Stream() { if (decoder) opus_decoder_destroy(decoder); }
    };
    std::map<int32_t, std::unique_ptr<Stream>> _streams;
    float _gain = 1.f;

    std::map<int32_t,int> _volumes;

    static int32_t distance(uint32_t a, uint32_t b) { return static_cast<int32_t>(a - b); }

    static Frame read(Stream& stream, uint64_t now)
    {
        Frame frame = {};
        if (stream.packets.empty()) { stream.playing = false; return frame; }
        // A small lead-in absorbs uneven packet arrival without a shared queue
        // letting one speaker crowd out another. Short utterances still play.
        if (!stream.playing)
        {
            if (stream.packets.size() < 3 && now - stream.queuedAt < 40) return frame;
            stream.playing = true;
        }
        const NetworkVoicePacket& packet = stream.packets.front();
        if (stream.haveSequence && distance(packet.sequence, stream.next) > 0 &&
            stream.concealed < 2 && packet.codec == VOICE_CODEC_OPUS && stream.decoder)
        {
            // Conceal only confirmed sequence gaps. No packets can also mean
            // that the sender's automatic sensitivity intentionally went quiet.
            opus_decode(stream.decoder, NULL, 0, frame.data(), VOICE_SAMPLES_PER_PACKET, 0);
            ++stream.next;
            ++stream.concealed;
            return frame;
        }
        if (packet.codec == VOICE_CODEC_OPUS && stream.decoder)
        {
            if (opus_decode(stream.decoder, packet.data.data(), static_cast<int>(packet.data.size()),
                frame.data(), VOICE_SAMPLES_PER_PACKET, 0) < 0) frame.fill(0);
        }
        else if (packet.codec == VOICE_CODEC_ADPCM)
        {
            vector<__int16> decoded;
            DecodeAdpcmVoicePacket(packet, decoded);
            std::copy(decoded.begin(), decoded.end(), frame.begin());
        }
        stream.next = packet.sequence + 1;
        stream.haveSequence = true;
        stream.concealed = 0;
        stream.packets.pop_front();
        return frame;
    }

public:
    void clear() { _streams.clear(); _gain = 1.f; }
    int volume(int32_t player) const {auto it=_volumes.find(player);return it==_volumes.end()?100:it->second;}
    void setVolume(int32_t player,int percent) {
        percent=(std::max)(0,(std::min)(200,percent));
        if(percent==100)_volumes.erase(player);else _volumes[player]=percent;
    }
    void clearVolumes(){_volumes.clear();}
    template<class People> void retainPlayers(const People& people) {
        for(auto it=_volumes.begin();it!=_volumes.end();) {
            bool present=false;for(const auto& person:people)if(person.first==it->first){present=true;break;}
            if(!present)it=_volumes.erase(it);else ++it;
        }
    }
    bool speaking(int32_t player, uint64_t now) const
    {
        auto found = _streams.find(player);
        return found != _streams.end() && now < found->second->speakingUntil;
    }

    void push(const NetworkVoicePacket& packet, uint64_t now)
    {
        if (packet.frameSamples != VOICE_SAMPLES_PER_PACKET || packet.sampleRate != VOICE_SAMPLE_RATE ||
            packet.data.empty() || packet.data.size() > VOICE_MAX_OPUS_BYTES ||
            (packet.codec != VOICE_CODEC_OPUS && packet.codec != VOICE_CODEC_ADPCM)) return;
        auto found = _streams.find(packet.playerId);
        if (found == _streams.end())
        {
            if (_streams.size() >= 128) return;
            found = _streams.emplace(packet.playerId, std::unique_ptr<Stream>(new Stream)).first;
        }
        Stream& stream = *found->second;
        if (stream.haveSequence && distance(packet.sequence, stream.next) < 0) return;
        auto pos = stream.packets.begin();
        while (pos != stream.packets.end() && distance(pos->sequence, packet.sequence) < 0) ++pos;
        if (pos != stream.packets.end() && pos->sequence == packet.sequence) return;
        if (stream.packets.empty()) stream.queuedAt = now;
        stream.packets.insert(pos, packet);
        stream.lastArrival = now;
        if (stream.packets.size() > 12)
        {
            stream.packets.pop_front();
            // Catch up after a stall rather than concealing frames deliberately
            // dropped to bound latency (240 ms per speaker).
            stream.next = stream.packets.front().sequence;
            stream.concealed = 0;
        }
    }

    Frame render(uint64_t now)
    {
        std::array<int32_t, VOICE_SAMPLES_PER_PACKET> sum = {};
        for (auto it = _streams.begin(); it != _streams.end();)
        {
            Stream& stream = *it->second;
            if (now - stream.lastArrival > 30000) { it = _streams.erase(it); continue; }
            Frame frame = read(stream, now);
            int64_t energy = 0;
            for (int16_t sample : frame) energy += static_cast<int64_t>(sample) * sample;
            if (energy > VOICE_SAMPLES_PER_PACKET * 80LL * 80) stream.speakingUntil = now + 250;
            const float gain=volume(it->first)/100.f;
            for (size_t i = 0; i < frame.size(); ++i) sum[i] += (int32_t)(frame[i]*gain);
            ++it;
        }
        int32_t peak = 1;
        for (int32_t value : sum) peak = (std::max)(peak, value < 0 ? -value : value);
        float target = peak > 30000 ? 30000.f / peak : 1.f;
        _gain = target < _gain ? target : _gain + (target - _gain) * .05f;
        Frame result;
        for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<int16_t>(sum[i] * _gain);
        return result;
    }
};
