#pragma once

// What interchange.export needs from the host for AAF and OMF (P.2, interchange/AudioPostExport.h): the audio engine
// that renders each clip's audio, and the renderer of Mixdown Video's reference file. The app and wabcut-cli provide
// them (exporter::ClipAudioRenderer / ReferenceVideoRenderer); without them an AAF links the original media (reported)
// and an OMF is refused. Bundle with the other services (ServiceBundle).

#include <wabcut/command/ExecutionContext.h>
#include <wabcut/interchange/AudioPostExport.h>

#include <memory>

namespace wabcut::command {

class AudioPostServices : public CommandServices {
public:
    explicit AudioPostServices(std::shared_ptr<interchange::AudioPostRenderer> audio,
                               std::shared_ptr<interchange::AudioPostVideoRenderer> video = {})
        : m_audio(std::move(audio)), m_video(std::move(video)) {}
    interchange::AudioPostRenderer* audio() const { return m_audio.get(); }
    interchange::AudioPostVideoRenderer* video() const { return m_video.get(); }

private:
    std::shared_ptr<interchange::AudioPostRenderer> m_audio;
    std::shared_ptr<interchange::AudioPostVideoRenderer> m_video;
};

} // namespace wabcut::command
