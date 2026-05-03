/**
 * @file src/platform/windows/mic_write.h
 * @brief Windows microphone redirection writer.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <Audioclient.h>
#include <mmdeviceapi.h>

#include "apollo_vmic.h"
#include "src/platform/common.h"

namespace platf::audio {
  template<typename T>
  inline void release_com(T *ptr) {
    if (ptr) {
      ptr->Release();
    }
  }

  struct mic_input_format_t {
    int sampleRate {};
    int channels {};
    int sampleFormatId {};  // matches LI_MIC_FMT_* from Limelight.h
    int bytesPerSample {};
    int frameDurationMs {};
  };

  class mic_write_wasapi_t: public mic_redirect_backend_t {
  public:
    mic_write_wasapi_t(std::string backend_name = "steam_streaming_microphone",
                       std::vector<std::wstring> autodetect_patterns = {},
                       std::string requested_device_name = {});
    ~mic_write_wasapi_t();

    std::string_view backend_id() const override;
    int init() override;
    int write_data(const char *data, std::size_t len, std::uint16_t sequence_number, std::uint32_t timestamp) override;
    void cleanup();

    void set_input_format(const mic_input_format_t &fmt);

  private:
    bool initialize_device();
    bool find_target_device(EDataFlow flow, std::wstring &device_id, std::string &device_name);
    void render_loop();

    util::safe_ptr<IMMDeviceEnumerator, release_com<IMMDeviceEnumerator>> device_enum;
    util::safe_ptr<IAudioClient, release_com<IAudioClient>> audio_client;
    IAudioRenderClient *audio_render = nullptr;
    std::vector<BYTE> active_format_storage;
    WAVEFORMATEX active_format {};
    UINT32 buffer_frame_count = 0;
    std::string backend_name;
    std::string requested_device_name;
    std::vector<std::wstring> autodetect_patterns;
    std::string target_device_name;
    bool first_packet_written_logged = false;
    util::safe_ptr_v2<void, BOOL, CloseHandle> render_event;
    std::mutex queue_mutex;
    std::deque<float> pending_frames;
    std::thread render_thread;
    std::atomic<bool> stop_render_thread {false};
    std::uint16_t expected_sequence_number = 0;
    bool has_playout_cursor = false;
    bool playout_started = false;
    bool playout_wait_logged = false;
    mic_input_format_t input_format;
    bool duplicate_to_stereo = false;
  };
}  // namespace platf::audio
