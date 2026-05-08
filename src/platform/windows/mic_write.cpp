/**
 * @file src/platform/windows/mic_write.cpp
 * @brief Windows microphone redirection writer.
 */
#include "mic_write.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <fstream>
#include <ios>
#include <string_view>
#include <vector>

#include <Audioclient.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <synchapi.h>
#include <windows.h>

#include "PolicyConfig.h"
#include "misc.h"
#include "src/audio.h"
#include "src/config.h"
#include "src/logging.h"

extern "C" {
#include <moonlight-common-c/src/Limelight.h>
}

namespace platf::audio {
  namespace {
    constexpr PROPERTYKEY PKEY_Device_DeviceDesc {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      2
    };
    constexpr PROPERTYKEY PKEY_Device_FriendlyName {
      {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
      14
    };
    constexpr PROPERTYKEY PKEY_DeviceInterface_FriendlyName {
      {0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}},
      2
    };

    // The WASAPI render endpoint for Steam Streaming Microphone always uses
    // 2-channel, 32-bit float, 48 kHz. This is the contract with the Steam
    // audio driver and must not be changed.
    constexpr std::uint32_t decoded_sample_rate = 48000;
    constexpr REFERENCE_TIME buffer_duration_100ns = 1000000;
    constexpr std::size_t max_queued_frames = decoded_sample_rate;

    template<typename T>
    void co_task_free(T *ptr) {
      if (ptr) {
        CoTaskMemFree(ptr);
      }
    }

    using device_t = util::safe_ptr<IMMDevice, release_com<IMMDevice>>;
    using collection_t = util::safe_ptr<IMMDeviceCollection, release_com<IMMDeviceCollection>>;
    using prop_t = util::safe_ptr<IPropertyStore, release_com<IPropertyStore>>;
    using policy_t = util::safe_ptr<IPolicyConfig, release_com<IPolicyConfig>>;
    using wstring_t = util::safe_ptr<WCHAR, co_task_free<WCHAR>>;
    using waveformat_t = util::safe_ptr<WAVEFORMATEX, co_task_free<WAVEFORMATEX>>;

    class prop_var_t {
    public:
      prop_var_t() {
        PropVariantInit(&value);
      }

      ~prop_var_t() {
        PropVariantClear(&value);
      }

      PROPVARIANT value;
    };

    struct parsed_waveformat_t {
      WORD channels {};
      DWORD sample_rate {};
      WORD bits_per_sample {};
      WORD valid_bits_per_sample {};
      WORD block_align {};
      DWORD channel_mask {};
      bool is_float {};
    };

    struct endpoint_format_info_t {
      std::string mix_format {"unavailable"};
      std::string device_format {"unavailable"};
      bool recommended_active {};
    };

    std::wstring get_prop_string(IPropertyStore *prop, REFPROPERTYKEY key) {
      prop_var_t value;
      if (FAILED(prop->GetValue(key, &value.value)) || value.value.vt != VT_LPWSTR || value.value.pwszVal == nullptr) {
        return {};
      }

      return value.value.pwszVal;
    }

    bool contains_case_insensitive(std::wstring haystack, std::wstring needle) {
      std::transform(haystack.begin(), haystack.end(), haystack.begin(), ::towlower);
      std::transform(needle.begin(), needle.end(), needle.begin(), ::towlower);
      return haystack.find(needle) != std::wstring::npos;
    }

    std::wstring endpoint_label(EDataFlow flow) {
      return flow == eCapture ? L"capture" : L"render";
    }

    parsed_waveformat_t parse_waveformat(const WAVEFORMATEX *format) {
      parsed_waveformat_t parsed {};
      if (format == nullptr) {
        return parsed;
      }

      parsed.channels = format->nChannels;
      parsed.sample_rate = format->nSamplesPerSec;
      parsed.bits_per_sample = format->wBitsPerSample;
      parsed.valid_bits_per_sample = format->wBitsPerSample;
      parsed.block_align = format->nBlockAlign;

      if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto *extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(format);
        parsed.valid_bits_per_sample = extensible->Samples.wValidBitsPerSample ? extensible->Samples.wValidBitsPerSample : format->wBitsPerSample;
        parsed.channel_mask = extensible->dwChannelMask;

        parsed.is_float = extensible->SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT &&
                          format->wBitsPerSample == 32 &&
                          parsed.valid_bits_per_sample == 32;
      } else if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT &&
                 format->wBitsPerSample == 32) {
        parsed.is_float = true;
      }

      return parsed;
    }

    std::string waveformat_to_pretty_string(const WAVEFORMATEX *format) {
      const auto parsed = parse_waveformat(format);
      if (format == nullptr) {
        return "unavailable";
      }

      std::string result = parsed.is_float ? "float32" : "pcm";
      result += ", ";
      result += std::to_string(parsed.valid_bits_per_sample ? parsed.valid_bits_per_sample : parsed.bits_per_sample);
      result += "-bit";
      result += ", ";
      result += std::to_string(parsed.sample_rate);
      result += " Hz, ";
      result += std::to_string(parsed.channels);
      result += "ch";
      if (parsed.channel_mask != 0) {
        result += ", mask=0x";
        result += util::hex(parsed.channel_mask).to_string();
      }
      return result;
    }

    bool is_recoverable_device_error(HRESULT status) {
      return status == AUDCLNT_E_DEVICE_INVALIDATED ||
             status == AUDCLNT_E_RESOURCES_INVALIDATED ||
             status == AUDCLNT_E_SERVICE_NOT_RUNNING;
    }

    std::uint16_t sequence_distance(std::uint16_t newer, std::uint16_t older) {
      return static_cast<std::uint16_t>(newer - older);
    }

    bool recover_device(mic_write_wasapi_t &writer, HRESULT status, const char *operation) {
      if (!is_recoverable_device_error(status)) {
        return false;
      }

      BOOST_LOG(warning) << "Microphone playback device needs reinitialization after failure while " << operation
                         << ": 0x" << util::hex(status).to_string_view();

      writer.cleanup();
      return writer.init() == 0;
    }

    std::vector<BYTE> make_recommended_steam_mic_device_waveformat() {
      WAVEFORMATEXTENSIBLE pcm_format {};
      pcm_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      pcm_format.Format.nChannels = 2;
      pcm_format.Format.nSamplesPerSec = decoded_sample_rate;
      pcm_format.Format.wBitsPerSample = 32;
      pcm_format.Samples.wValidBitsPerSample = 32;
      pcm_format.Format.nBlockAlign = static_cast<WORD>(pcm_format.Format.nChannels * (pcm_format.Format.wBitsPerSample / 8));
      pcm_format.Format.nAvgBytesPerSec = pcm_format.Format.nSamplesPerSec * pcm_format.Format.nBlockAlign;
      pcm_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      pcm_format.SubFormat = KSDATAFORMAT_SUBTYPE_PCM;
      pcm_format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

      std::vector<BYTE> storage(sizeof(pcm_format));
      std::memcpy(storage.data(), &pcm_format, sizeof(pcm_format));
      return storage;
    }

    std::vector<BYTE> make_required_steam_mic_render_waveformat() {
      WAVEFORMATEXTENSIBLE float_format {};
      float_format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      float_format.Format.nChannels = 2;
      float_format.Format.nSamplesPerSec = decoded_sample_rate;
      float_format.Format.wBitsPerSample = 32;
      float_format.Samples.wValidBitsPerSample = 32;
      float_format.Format.nBlockAlign = static_cast<WORD>(float_format.Format.nChannels * sizeof(float));
      float_format.Format.nAvgBytesPerSec = float_format.Format.nSamplesPerSec * float_format.Format.nBlockAlign;
      float_format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      float_format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
      float_format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;

      std::vector<BYTE> storage(sizeof(float_format));
      std::memcpy(storage.data(), &float_format, sizeof(float_format));
      return storage;
    }

    bool is_recommended_steam_mic_device_format(const WAVEFORMATEX *format) {
      const auto parsed = parse_waveformat(format);
      return parsed.channels == 2 &&
             parsed.sample_rate == decoded_sample_rate &&
             parsed.valid_bits_per_sample == 32;
    }

    endpoint_format_info_t query_endpoint_format_info(IMMDeviceEnumerator *device_enum, const std::wstring &device_id) {
      endpoint_format_info_t info;

      policy_t policy;
      auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        reinterpret_cast<void **>(&policy)
      );
      if (FAILED(status) || !policy) {
        return info;
      }

      waveformat_t current_format;
      status = policy->GetDeviceFormat(device_id.c_str(), false, &current_format);
      if (SUCCEEDED(status) && current_format) {
        info.device_format = waveformat_to_pretty_string(current_format.get());
        info.recommended_active = is_recommended_steam_mic_device_format(current_format.get());
      }

      device_t device;
      status = device_enum ? device_enum->GetDevice(device_id.c_str(), &device) : E_FAIL;
      if (FAILED(status) || !device) {
        return info;
      }

      util::safe_ptr<IAudioClient, release_com<IAudioClient>> local_audio_client;
      status = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&local_audio_client));
      if (FAILED(status) || !local_audio_client) {
        return info;
      }

      waveformat_t mix_format;
      status = local_audio_client->GetMixFormat(&mix_format);
      if (SUCCEEDED(status) && mix_format) {
        info.mix_format = waveformat_to_pretty_string(mix_format.get());
      }

      return info;
    }

    // ===== Persistent Steam mic prev-format tracking =====
    //
    // ensure_recommended_steam_mic_format() mutates the WASAPI endpoint format
    // of the Steam Streaming Microphone device(s) to the Apollo-required
    // 48 kHz / stereo / 32-bit format. Without persistence, an abnormal exit
    // (SIGKILL, host crash, power loss) leaves the device stuck at this
    // format forever. We persist (device_id -> previous WAVEFORMATEX bytes)
    // records to %LOCALAPPDATA%\Apollo\steam_mic_prev_format.bin so that:
    //   1. Normal cleanup can restore the original format and remove the
    //      record from the file.
    //   2. The next Apollo startup can sweep the file and revert any
    //      orphan mutations left over from a prior abnormal exit.
    //
    // File layout (binary, little-endian, repeated per record):
    //   uint32_t  device_id_wchar_count   (number of WCHARs, no NUL)
    //   WCHAR     device_id[device_id_wchar_count]
    //   uint32_t  format_byte_count
    //   uint8_t   format_bytes[format_byte_count]
    //
    // The format_bytes hold a verbatim WAVEFORMATEX (or WAVEFORMATEXTENSIBLE,
    // or any larger driver-defined extension) of size sizeof(WAVEFORMATEX) +
    // WAVEFORMATEX::cbSize. They are passed back to IPolicyConfig::SetDeviceFormat
    // unchanged on revert.
    //
    // A 4-byte file magic "APMF" prefixes the file so we can reject corrupt
    // or future-format files without misinterpreting bytes as record sizes.

    constexpr std::uint32_t k_steam_mic_prev_format_magic = 0x464D5041;  // 'APMF' little-endian

    struct prev_format_entry_t {
      std::wstring device_id;
      std::vector<BYTE> format_bytes;
    };

    std::wstring get_prev_format_file_path() {
      const wchar_t *local_app_data = _wgetenv(L"LOCALAPPDATA");
      if (!local_app_data || !*local_app_data) {
        return std::wstring();
      }

      std::wstring dir = std::wstring(local_app_data) + L"\\Apollo";
      // CreateDirectoryW returns nonzero on success; if it already exists the
      // last error is ERROR_ALREADY_EXISTS, which is fine.
      CreateDirectoryW(dir.c_str(), nullptr);

      return dir + L"\\steam_mic_prev_format.bin";
    }

    template<typename T>
    bool read_pod(std::ifstream &f, T &out) {
      f.read(reinterpret_cast<char *>(&out), sizeof(T));
      return f.good();
    }

    template<typename T>
    void write_pod(std::ofstream &f, const T &v) {
      f.write(reinterpret_cast<const char *>(&v), sizeof(T));
    }

    std::vector<prev_format_entry_t> load_all_prev_format_entries() {
      std::vector<prev_format_entry_t> entries;

      std::wstring path = get_prev_format_file_path();
      if (path.empty()) {
        return entries;
      }

      std::ifstream f(path.c_str(), std::ios::binary);
      if (!f.is_open()) {
        return entries;
      }

      std::uint32_t magic = 0;
      if (!read_pod(f, magic) || magic != k_steam_mic_prev_format_magic) {
        // Missing or wrong magic. Treat as no entries (and the file will be
        // overwritten next time we successfully append).
        return entries;
      }

      while (f.peek() != EOF) {
        std::uint32_t id_wchar_count = 0;
        if (!read_pod(f, id_wchar_count)) {
          break;
        }
        // Sanity bound to refuse pathological files. Device IDs are typically
        // shorter than 256 WCHARs.
        if (id_wchar_count == 0 || id_wchar_count > 4096) {
          break;
        }

        std::wstring device_id;
        device_id.resize(id_wchar_count);
        f.read(reinterpret_cast<char *>(device_id.data()),
               static_cast<std::streamsize>(id_wchar_count) * sizeof(wchar_t));
        if (!f.good()) {
          break;
        }

        std::uint32_t fmt_byte_count = 0;
        if (!read_pod(f, fmt_byte_count)) {
          break;
        }
        if (fmt_byte_count < sizeof(WAVEFORMATEX) || fmt_byte_count > 65536) {
          break;
        }

        std::vector<BYTE> fmt_bytes(fmt_byte_count);
        f.read(reinterpret_cast<char *>(fmt_bytes.data()),
               static_cast<std::streamsize>(fmt_byte_count));
        if (!f.good()) {
          break;
        }

        entries.push_back({std::move(device_id), std::move(fmt_bytes)});
      }

      return entries;
    }

    bool write_all_prev_format_entries(const std::vector<prev_format_entry_t> &entries) {
      std::wstring path = get_prev_format_file_path();
      if (path.empty()) {
        return false;
      }

      // std::ios::trunc rewrites the file from scratch -- there is no append
      // semantic for a header+record format. Callers are expected to load,
      // mutate, and rewrite in one go.
      std::ofstream f(path.c_str(), std::ios::binary | std::ios::trunc);
      if (!f.is_open()) {
        return false;
      }

      write_pod(f, k_steam_mic_prev_format_magic);

      for (const auto &e : entries) {
        const std::uint32_t id_wchar_count = static_cast<std::uint32_t>(e.device_id.size());
        write_pod(f, id_wchar_count);
        f.write(reinterpret_cast<const char *>(e.device_id.data()),
                static_cast<std::streamsize>(id_wchar_count) * sizeof(wchar_t));

        const std::uint32_t fmt_byte_count = static_cast<std::uint32_t>(e.format_bytes.size());
        write_pod(f, fmt_byte_count);
        f.write(reinterpret_cast<const char *>(e.format_bytes.data()),
                static_cast<std::streamsize>(fmt_byte_count));
      }

      return f.good();
    }

    void append_prev_format_entry(const std::wstring &device_id, const std::vector<BYTE> &format_bytes) {
      auto entries = load_all_prev_format_entries();

      // If the device already has a recorded prev format, do not overwrite it.
      // The earliest captured prev format is the one that should be restored;
      // a later capture would just record our already-mutated format.
      for (const auto &e : entries) {
        if (e.device_id == device_id) {
          return;
        }
      }

      entries.push_back({device_id, format_bytes});
      if (!write_all_prev_format_entries(entries)) {
        BOOST_LOG(warning) << "Couldn't persist Steam microphone prev-format record for device";
      }
    }

    void remove_prev_format_entry(const std::wstring &device_id) {
      auto entries = load_all_prev_format_entries();

      const auto before = entries.size();
      entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [&device_id](const prev_format_entry_t &e) {
                         return e.device_id == device_id;
                       }),
        entries.end());

      if (entries.size() != before) {
        write_all_prev_format_entries(entries);
      }
    }

    // Apply a previously-recorded format back onto a device via IPolicyConfig.
    // Returns true on success.
    bool restore_device_format(IPolicyConfig *policy,
                               const std::wstring &device_id,
                               const std::vector<BYTE> &format_bytes) {
      if (policy == nullptr || format_bytes.size() < sizeof(WAVEFORMATEX)) {
        return false;
      }

      // SetDeviceFormat takes a non-const WAVEFORMATEX*. Make a writable copy
      // so we don't violate the const-ness of the input vector.
      std::vector<BYTE> mutable_bytes = format_bytes;
      auto *fmt = reinterpret_cast<WAVEFORMATEX *>(mutable_bytes.data());

      WAVEFORMATEXTENSIBLE discarded {};
      HRESULT hr = policy->SetDeviceFormat(
        device_id.c_str(),
        fmt,
        reinterpret_cast<WAVEFORMATEX *>(&discarded));
      return SUCCEEDED(hr);
    }
    // ===== End persistent Steam mic prev-format tracking =====

    // ensure_recommended_steam_mic_format returns true if the device is at the
    // recommended format after this call (whether because it already was or
    // because we changed it). If out_did_mutate is non-null, it is set to true
    // ONLY when this call actually changed the device format -- callers use
    // that signal to know when they need to revert on cleanup.
    bool ensure_recommended_steam_mic_format(const std::wstring &device_id,
                                             const std::string &target_device_name,
                                             EDataFlow flow,
                                             bool *out_did_mutate = nullptr) {
      if (out_did_mutate != nullptr) {
        *out_did_mutate = false;
      }

      policy_t policy;
      auto status = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        reinterpret_cast<void **>(&policy)
      );
      if (FAILED(status) || !policy) {
        BOOST_LOG(warning) << "Couldn't create audio policy config for Steam microphone format setup: 0x"
                           << util::hex(status).to_string_view();
        return false;
      }

      waveformat_t current_format;
      status = policy->GetDeviceFormat(device_id.c_str(), false, &current_format);
      if (FAILED(status) || !current_format) {
        BOOST_LOG(warning) << "Couldn't query Steam microphone " << to_utf8(endpoint_label(flow)) << " device format for [" << target_device_name << "]: 0x"
                           << util::hex(status).to_string_view();
        return false;
      }

      if (is_recommended_steam_mic_device_format(current_format.get())) {
        return true;
      }

      // Capture the full previous format (including any driver-specific tail
      // bytes) so we can restore it later. cbSize on a WAVEFORMATEX includes
      // only the bytes after the base struct.
      const std::size_t prev_format_size = sizeof(WAVEFORMATEX) + current_format->cbSize;
      std::vector<BYTE> previous_format_bytes(prev_format_size);
      std::memcpy(previous_format_bytes.data(), current_format.get(), prev_format_size);

      auto recommended_format_storage = make_recommended_steam_mic_device_waveformat();
      auto *recommended_format = reinterpret_cast<WAVEFORMATEX *>(recommended_format_storage.data());
      WAVEFORMATEXTENSIBLE previous_format {};
      status = policy->SetDeviceFormat(device_id.c_str(), recommended_format, reinterpret_cast<WAVEFORMATEX *>(&previous_format));
      if (FAILED(status)) {
        BOOST_LOG(warning) << "Couldn't set Steam microphone " << to_utf8(endpoint_label(flow))
                           << " device format to stereo 32-bit 48k for [" << target_device_name << "]: 0x"
                           << util::hex(status).to_string_view();
        return false;
      }

      // Persist the prev format so that an abnormal Apollo exit (or a missed
      // cleanup path) can be recovered from the next time Apollo starts up.
      // Always persist before we report success: if persistence fails we still
      // attempt to revert in cleanup() via the in-memory member tracking, but
      // we lose the orphan-sweep guarantee.
      append_prev_format_entry(device_id, previous_format_bytes);

      if (out_did_mutate != nullptr) {
        *out_did_mutate = true;
      }

      BOOST_LOG(info) << "Changed Steam microphone " << to_utf8(endpoint_label(flow)) << " device format for [" << target_device_name
                      << "] to [pcm, 32-bit, 48000 Hz, 2ch]";
      return true;
    }

    // Try to revert the format mutation for a single device_id using the
    // persisted prev-format file. Returns true if a record was found AND the
    // SetDeviceFormat call succeeded (in which case the record is removed
    // from the file). Returns false on any failure or missing record.
    bool revert_steam_mic_format_for_device(const std::wstring &device_id) {
      if (device_id.empty()) {
        return false;
      }

      auto entries = load_all_prev_format_entries();
      const prev_format_entry_t *match = nullptr;
      for (const auto &e : entries) {
        if (e.device_id == device_id) {
          match = &e;
          break;
        }
      }
      if (match == nullptr) {
        return false;
      }

      policy_t policy;
      HRESULT hr = CoCreateInstance(
        CLSID_CPolicyConfigClient,
        nullptr,
        CLSCTX_ALL,
        IID_IPolicyConfig,
        reinterpret_cast<void **>(&policy));
      if (FAILED(hr) || !policy) {
        BOOST_LOG(warning) << "Couldn't create audio policy config to revert Steam microphone format: 0x"
                           << util::hex(hr).to_string_view();
        return false;
      }

      if (!restore_device_format(policy.get(), device_id, match->format_bytes)) {
        BOOST_LOG(warning) << "Couldn't restore Steam microphone format for device (SetDeviceFormat failed)";
        return false;
      }

      remove_prev_format_entry(device_id);
      BOOST_LOG(info) << "Restored Steam microphone device format from persisted record";
      return true;
    }

    HRESULT initialize_shared_audio_client(IAudioClient *audio_client, const WAVEFORMATEX *format, DWORD stream_flags) {
      return audio_client->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        stream_flags,
        buffer_duration_100ns,
        0,
        format,
        nullptr
      );
    }

    // Convert a raw PCM byte buffer to floats, appending to out_floats.
    // Returns the number of per-channel sample frames appended.
    int pcm_to_float(const char *data, std::size_t len, int sampleFormatId,
                     std::deque<float> &out_floats) {
      switch (sampleFormatId) {
        case LI_MIC_FMT_S16LE: {
          const int count = static_cast<int>(len / sizeof(int16_t));
          const int16_t *src = reinterpret_cast<const int16_t *>(data);
          for (int i = 0; i < count; ++i) {
            int16_t raw;
            std::memcpy(&raw, src + i, sizeof(raw));
            out_floats.push_back(std::clamp(static_cast<float>(raw) / 32768.0f, -1.0f, 1.0f));
          }
          return count;
        }
        case LI_MIC_FMT_S24LE: {
          // 3-byte little-endian signed samples
          const int count = static_cast<int>(len / 3);
          const uint8_t *src = reinterpret_cast<const uint8_t *>(data);
          for (int i = 0; i < count; ++i) {
            int32_t raw = static_cast<int32_t>(src[i * 3]) |
                          (static_cast<int32_t>(src[i * 3 + 1]) << 8) |
                          (static_cast<int32_t>(src[i * 3 + 2]) << 16);
            // Sign-extend from 24 bits
            if (raw & 0x800000) { raw |= static_cast<int32_t>(0xFF000000); }
            out_floats.push_back(std::clamp(static_cast<float>(raw) / 8388608.0f, -1.0f, 1.0f));
          }
          return count;
        }
        case LI_MIC_FMT_S32LE: {
          const int count = static_cast<int>(len / sizeof(int32_t));
          const int32_t *src = reinterpret_cast<const int32_t *>(data);
          for (int i = 0; i < count; ++i) {
            int32_t raw;
            std::memcpy(&raw, src + i, sizeof(raw));
            out_floats.push_back(std::clamp(static_cast<float>(raw) / 2147483648.0f, -1.0f, 1.0f));
          }
          return count;
        }
        case LI_MIC_FMT_F32LE:
        default: {
          const int count = static_cast<int>(len / sizeof(float));
          const float *src = reinterpret_cast<const float *>(data);
          for (int i = 0; i < count; ++i) {
            float raw;
            std::memcpy(&raw, src + i, sizeof(raw));
            out_floats.push_back(std::clamp(raw, -1.0f, 1.0f));
          }
          return count;
        }
      }
    }

  }  // namespace

  mic_write_wasapi_t::mic_write_wasapi_t(std::string backend_name,
                                         std::vector<std::wstring> autodetect_patterns,
                                         std::string requested_device_name):
      backend_name {std::move(backend_name)},
      requested_device_name {std::move(requested_device_name)},
      autodetect_patterns {std::move(autodetect_patterns)} {
  }

  mic_write_wasapi_t::~mic_write_wasapi_t() {
    cleanup();
  }

  std::string_view mic_write_wasapi_t::backend_id() const {
    return backend_name;
  }

  void mic_write_wasapi_t::set_input_format(const mic_input_format_t &fmt) {
    input_format = fmt;
  }

  bool mic_write_wasapi_t::find_target_device(EDataFlow flow, std::wstring &device_id, std::string &device_name) {
    collection_t collection;
    HRESULT status = device_enum->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(status) || !collection) {
      BOOST_LOG(error) << "Couldn't enumerate " << to_utf8(endpoint_label(flow))
                       << " devices for microphone redirection: 0x" << util::hex(status).to_string_view();
      return false;
    }

    std::wstring requested_name = requested_device_name.empty() ? std::wstring {} : from_utf8(requested_device_name);
    auto patterns = autodetect_patterns;
    if (patterns.empty()) {
      patterns = flow == eCapture ?
        std::vector<std::wstring> {L"Microphone (Steam Streaming Microphone)", L"Steam Streaming Microphone"} :
        std::vector<std::wstring> {L"Steam Streaming Microphone", L"Speakers (Steam Streaming Microphone)"};
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT index = 0; index < count; ++index) {
      device_t device;
      if (FAILED(collection->Item(index, &device)) || !device) {
        continue;
      }

      wstring_t id;
      if (FAILED(device->GetId(&id)) || !id) {
        continue;
      }

      prop_t prop;
      if (FAILED(device->OpenPropertyStore(STGM_READ, &prop)) || !prop) {
        continue;
      }

      auto friendly_name = get_prop_string(prop.get(), PKEY_Device_FriendlyName);
      auto interface_name = get_prop_string(prop.get(), PKEY_DeviceInterface_FriendlyName);
      auto description = get_prop_string(prop.get(), PKEY_Device_DeviceDesc);

      if (requested_name.empty() &&
          (contains_case_insensitive(friendly_name, L"16ch") ||
           contains_case_insensitive(interface_name, L"16ch") ||
           contains_case_insensitive(description, L"16ch"))) {
        continue;
      }

      bool matched = false;
      if (!requested_name.empty()) {
        matched = friendly_name == requested_name || interface_name == requested_name || description == requested_name || id.get() == requested_name;
      } else {
        for (const auto &pattern : patterns) {
          if (contains_case_insensitive(friendly_name, pattern) ||
              contains_case_insensitive(interface_name, pattern) ||
              contains_case_insensitive(description, pattern)) {
            matched = true;
            break;
          }
        }
      }

      if (!matched) {
        continue;
      }

      device_id = id.get();
      device_name = to_utf8(!friendly_name.empty() ? friendly_name : interface_name);
      return true;
    }

    return false;
  }

  bool mic_write_wasapi_t::initialize_device() {
    std::wstring render_device_id;
    if (!find_target_device(eRender, render_device_id, target_device_name)) {
      if (requested_device_name.empty()) {
        BOOST_LOG(warning) << "No supported Steam Streaming Microphone playback device found. Install the Steam audio drivers and ensure "
                           << "\"Speakers (Steam Streaming Microphone)\" is available.";
        ::audio::mic_debug_on_backend_error("Steam Streaming Microphone was not found on the host. Install the local Steam audio drivers and ensure Speakers (Steam Streaming Microphone) is available.");
      } else {
        BOOST_LOG(warning) << "Requested microphone device not found: " << requested_device_name;
        ::audio::mic_debug_on_backend_error("Requested microphone render device was not found: " + requested_device_name);
      }
      return false;
    }

    std::wstring capture_device_id;
    std::string capture_device_name;
    if (!find_target_device(eCapture, capture_device_id, capture_device_name)) {
      BOOST_LOG(warning) << "Couldn't find the paired Steam microphone capture endpoint. Host applications may read from a stale or mismatched format.";
      ::audio::mic_debug_on_backend_error("Could not find the paired Microphone (Steam Streaming Microphone) capture endpoint");
      return false;
    }

    bool render_did_mutate = false;
    const bool render_format_enforced = ensure_recommended_steam_mic_format(render_device_id, target_device_name, eRender, &render_did_mutate);
    if (render_did_mutate) {
      mutated_device_ids.push_back(render_device_id);
    }
    bool capture_did_mutate = false;
    const bool capture_format_enforced = ensure_recommended_steam_mic_format(capture_device_id, capture_device_name, eCapture, &capture_did_mutate);
    if (capture_did_mutate) {
      mutated_device_ids.push_back(capture_device_id);
    }
    const auto render_endpoint_info = query_endpoint_format_info(device_enum.get(), render_device_id);
    const auto capture_endpoint_info = query_endpoint_format_info(device_enum.get(), capture_device_id);
    const bool recommended_format_active = render_endpoint_info.recommended_active && capture_endpoint_info.recommended_active;
    const bool recommended_format_enforced = render_format_enforced || capture_format_enforced;

    device_t device;
    HRESULT status = device_enum->GetDevice(render_device_id.c_str(), &device);
    if (FAILED(status) || !device) {
      BOOST_LOG(error) << "Couldn't open microphone playback device [" << target_device_name << "]: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not open host microphone render device [" + target_device_name + "]");
      return false;
    }

    status = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr, (void **) &audio_client);
    if (FAILED(status) || !audio_client) {
      BOOST_LOG(error) << "Couldn't activate microphone playback client [" << target_device_name << "]: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not activate the host microphone playback client for [" + target_device_name + "]");
      return false;
    }

    waveformat_t mix_format;
    status = audio_client->GetMixFormat(&mix_format);
    if (FAILED(status) || !mix_format) {
      BOOST_LOG(error) << "Couldn't get microphone playback mix format for [" << target_device_name << "]: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not query the Steam Streaming Microphone endpoint mix format");
      return false;
    }

    const auto endpoint_mix_string = waveformat_to_pretty_string(mix_format.get());
    active_format_storage = make_required_steam_mic_render_waveformat();
    auto *required_render_format = reinterpret_cast<WAVEFORMATEX *>(active_format_storage.data());

    const auto init_status = initialize_shared_audio_client(audio_client.get(), required_render_format, AUDCLNT_STREAMFLAGS_EVENTCALLBACK);
    if (FAILED(init_status)) {
      BOOST_LOG(error) << "Couldn't initialize microphone playback client [" << target_device_name
                       << "] with required format [float32, 32-bit, 48000 Hz, 2ch]: 0x"
                       << util::hex(init_status).to_string_view();
      ::audio::mic_debug_on_backend_error("Steam Streaming Microphone must support 2ch, 32-bit float, 48000 Hz");
      return false;
    }

    std::memset(&active_format, 0, sizeof(active_format));
    std::memcpy(&active_format, active_format_storage.data(), std::min(active_format_storage.size(), sizeof(active_format)));

    status = audio_client->GetBufferSize(&buffer_frame_count);
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't query microphone playback buffer size: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not query the microphone playback buffer size");
      return false;
    }

    status = audio_client->GetService(IID_IAudioRenderClient, (void **) &audio_render);
    if (FAILED(status) || !audio_render) {
      BOOST_LOG(error) << "Couldn't acquire microphone playback render client: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not acquire the microphone playback render client");
      return false;
    }

    render_event.reset(CreateEvent(nullptr, FALSE, FALSE, nullptr));
    if (!render_event) {
      BOOST_LOG(error) << "Couldn't create microphone playback event handle: 0x" << util::hex(GetLastError()).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not create the microphone playback event handle");
      return false;
    }

    status = audio_client->SetEventHandle(render_event.get());
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't set microphone playback event handle: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not set the microphone playback event handle");
      return false;
    }

    // Determine mono→stereo duplication based on negotiated client channel count.
    duplicate_to_stereo = (input_format.channels == 1);

    const auto render_format_string = waveformat_to_pretty_string(required_render_format);
    const std::string channel_mapping = duplicate_to_stereo
      ? "Duplicate mono client PCM input to stereo render channels"
      : "Pass stereo client PCM input to stereo render channels";

    BOOST_LOG(info) << "Client microphone redirection target: " << target_device_name
                    << " [mix=" << endpoint_mix_string
                    << ", render-device=" << render_endpoint_info.device_format
                    << ", capture-device=" << capture_endpoint_info.device_format
                    << ", render=" << render_format_string
                    << ", client-fmt=" << input_format.sampleRate << "Hz/"
                    << input_format.channels << "ch/" << (input_format.bytesPerSample * 8) << "bit"
                    << ", frame=" << input_format.frameDurationMs << "ms"
                    << ", mono-to-stereo=" << (duplicate_to_stereo ? "yes" : "no")
                    << ']';

    BOOST_LOG(info) << "Paired Steam microphone capture endpoint: " << capture_device_name
                    << " [mix=" << capture_endpoint_info.mix_format
                    << ", device=" << capture_endpoint_info.device_format
                    << ", recommended=" << (recommended_format_active ? "active" : "inactive")
                    << ", enforced=" << (recommended_format_enforced ? "yes" : "no")
                    << ']';

    ::audio::mic_debug_on_backend_target(target_device_name, active_format.nChannels, active_format.nSamplesPerSec);
    ::audio::mic_debug_on_backend_format(endpoint_mix_string, render_format_string, false, channel_mapping);
    ::audio::mic_debug_on_backend_endpoint_formats(
      render_endpoint_info.device_format,
      capture_device_name,
      capture_endpoint_info.mix_format,
      capture_endpoint_info.device_format,
      recommended_format_enforced,
      recommended_format_active
    );

    status = audio_client->Start();
    if (FAILED(status)) {
      BOOST_LOG(error) << "Couldn't start microphone playback client: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not start the microphone playback client");
      return false;
    }

    stop_render_thread = false;
    render_thread = std::thread {[this]() { render_loop(); }};

    return true;
  }

  int mic_write_wasapi_t::init() {
    // If no explicit input_format was provided, default to 48kHz mono s16le 10ms.
    if (input_format.sampleRate == 0) {
      input_format.sampleRate    = 48000;
      input_format.channels      = 1;
      input_format.sampleFormatId = LI_MIC_FMT_S16LE;
      input_format.bytesPerSample = 2;
      input_format.frameDurationMs = 10;
    }

    HRESULT status = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL, IID_IMMDeviceEnumerator, (void **) &device_enum);
    if (FAILED(status) || !device_enum) {
      BOOST_LOG(error) << "Couldn't create device enumerator for microphone redirection: 0x" << util::hex(status).to_string_view();
      ::audio::mic_debug_on_backend_error("Could not create the Windows audio device enumerator for microphone redirection");
      return -1;
    }

    if (!initialize_device()) {
      return -1;
    }

    ::audio::mic_debug_on_backend_initialized(std::string {backend_name});

    return 0;
  }

  int mic_write_wasapi_t::write_data(const char *data, std::size_t len, std::uint16_t sequence_number, std::uint32_t timestamp) {
    if (!audio_client || audio_render == nullptr || data == nullptr || len == 0 || !render_event) {
      BOOST_LOG(warning) << "Client microphone packet rejected because the WASAPI write path is not ready"
                         << " [seq=" << sequence_number
                         << ", ts=" << timestamp
                         << ", len=" << len
                         << ", audio_client=" << static_cast<bool>(audio_client)
                         << ", audio_render=" << static_cast<bool>(audio_render != nullptr)
                         << ", render_event=" << static_cast<bool>(render_event)
                         << ", data=" << static_cast<bool>(data != nullptr) << ']';
      return -1;
    }

    if (active_format.nChannels != 2 || active_format.nSamplesPerSec != decoded_sample_rate || active_format.wBitsPerSample != 32) {
      ::audio::mic_debug_on_render_error(sequence_number, "Steam Streaming Microphone is not running at the required 2ch, 32-bit float, 48000 Hz format");
      return -1;
    }

    bool stale_packet = false;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);

      if (has_playout_cursor) {
        const auto behind = sequence_distance(expected_sequence_number, sequence_number);
        if (behind != 0 && behind < 0x8000) {
          stale_packet = true;
        }
      }

      if (!stale_packet) {
        // Convert raw PCM bytes directly to float and push into pending_frames.
        const int samples_appended = pcm_to_float(data, len, input_format.sampleFormatId, pending_frames);

        if (!has_playout_cursor) {
          expected_sequence_number = sequence_number;
          has_playout_cursor = true;
        }
        ++expected_sequence_number;

        if (pending_frames.size() > max_queued_frames) {
          const auto frames_to_trim = pending_frames.size() - max_queued_frames;
          pending_frames.erase(pending_frames.begin(), pending_frames.begin() + static_cast<std::ptrdiff_t>(frames_to_trim));
        }

        float peak = 0.0f;
        // Compute peak over the just-appended samples
        const std::size_t start = pending_frames.size() > static_cast<std::size_t>(samples_appended)
          ? pending_frames.size() - static_cast<std::size_t>(samples_appended) : 0;
        for (std::size_t i = start; i < pending_frames.size(); ++i) {
          float a = std::fabs(pending_frames[i]);
          if (a > peak) { peak = a; }
        }

        const double normalized_level = static_cast<double>(peak);
        const bool silent = peak < 0.015625f;
        ::audio::mic_debug_on_packet_decoded(sequence_number, normalized_level, silent);
        ::audio::mic_debug_on_packet_rendered(sequence_number, normalized_level, silent);

        if (!first_packet_written_logged) {
          first_packet_written_logged = true;
          BOOST_LOG(info) << "Client PCM microphone audio is being rendered into [" << target_device_name << ']';
        }
      }
    }

    if (stale_packet) {
      ::audio::mic_debug_on_packet_dropped(sequence_number, "Dropped a stale microphone packet that arrived after its playout deadline");
      return 0;
    }

    SetEvent(render_event.get());
    return static_cast<int>(len);
  }

  void mic_write_wasapi_t::render_loop() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
    platf::adjust_thread_priority(platf::thread_priority_e::high);

    // Prebuffer: 4 frames worth of samples at the input format's rate.
    const std::size_t frame_samples = static_cast<std::size_t>(input_format.sampleRate) *
                                      static_cast<std::size_t>(input_format.frameDurationMs) / 1000 *
                                      static_cast<std::size_t>(input_format.channels);
    const std::size_t target_prebuffer_frames = frame_samples * 4;

    while (!stop_render_thread) {
      if (!audio_client || audio_render == nullptr || !render_event) {
        break;
      }

      const auto wait_result = WaitForSingleObject(render_event.get(), 20);
      if (stop_render_thread) {
        break;
      }
      if (wait_result != WAIT_OBJECT_0 && wait_result != WAIT_TIMEOUT) {
        BOOST_LOG(debug) << "Microphone render wait failed for [" << target_device_name << "]: 0x"
                         << util::hex(GetLastError()).to_string_view();
        continue;
      }

      UINT32 padding = 0;
      auto status = audio_client->GetCurrentPadding(&padding);
      if (FAILED(status)) {
        BOOST_LOG(debug) << "Couldn't query microphone playback padding for [" << target_device_name << "]: 0x"
                         << util::hex(status).to_string_view();
        if (is_recoverable_device_error(status)) {
          ::audio::mic_debug_on_backend_error("Steam microphone playback device was invalidated during rendering. Restart the stream.");
          break;
        }
        continue;
      }

      if (padding > buffer_frame_count) {
        padding = 0;
      }

      const auto frames_available = buffer_frame_count - padding;
      if (frames_available == 0) {
        continue;
      }

      UINT32 queued_frames = 0;
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        queued_frames = std::min<UINT32>(frames_available, static_cast<UINT32>(pending_frames.size()));
      }

      const auto buffered_frames_total = static_cast<std::size_t>(padding) + queued_frames;

      if (!playout_started) {
        if (buffered_frames_total < target_prebuffer_frames) {
          if (!playout_wait_logged) {
            playout_wait_logged = true;
            BOOST_LOG(debug) << "Waiting for microphone playout prebuffer on [" << target_device_name << "], queued "
                             << queued_frames << " frames, padding " << padding << ", total buffered "
                             << buffered_frames_total << " of " << target_prebuffer_frames << " target frames";
          }
          continue;
        }

        playout_started = true;
        playout_wait_logged = false;
        BOOST_LOG(debug) << "Microphone playout prebuffer ready on [" << target_device_name << "] with "
                         << queued_frames << " queued frames, padding " << padding
                         << ", total buffered " << buffered_frames_total << " frames";
      }

      if (queued_frames == 0) {
        continue;
      }

      BYTE *buffer = nullptr;
      status = audio_render->GetBuffer(queued_frames, &buffer);
      if (FAILED(status) || buffer == nullptr) {
        BOOST_LOG(debug) << "Couldn't acquire microphone playback buffer for [" << target_device_name << "]: 0x"
                         << util::hex(status).to_string_view();
        if (FAILED(status) && is_recoverable_device_error(status)) {
          ::audio::mic_debug_on_backend_error("Steam microphone playback device was invalidated while acquiring a render buffer. Restart the stream.");
          break;
        }
        continue;
      }

      auto *dst = reinterpret_cast<float *>(buffer);
      {
        std::lock_guard<std::mutex> lock(queue_mutex);
        for (UINT32 frame = 0; frame < queued_frames; ++frame) {
          if (duplicate_to_stereo) {
            // Client sent mono: duplicate the single channel to both L and R.
            const float sample = pending_frames.front();
            pending_frames.pop_front();
            dst[static_cast<std::size_t>(frame) * 2]     = sample;
            dst[static_cast<std::size_t>(frame) * 2 + 1] = sample;
          } else {
            // Client sent stereo: take L then R interleaved.
            const float left  = pending_frames.front(); pending_frames.pop_front();
            const float right = pending_frames.front(); pending_frames.pop_front();
            dst[static_cast<std::size_t>(frame) * 2]     = left;
            dst[static_cast<std::size_t>(frame) * 2 + 1] = right;
          }
        }
      }

      status = audio_render->ReleaseBuffer(queued_frames, 0);
      if (FAILED(status)) {
        BOOST_LOG(debug) << "Couldn't release microphone playback buffer for [" << target_device_name << "]: 0x"
                         << util::hex(status).to_string_view();
        if (is_recoverable_device_error(status)) {
          ::audio::mic_debug_on_backend_error("Steam microphone playback device was invalidated while releasing a render buffer. Restart the stream.");
          break;
        }
      }
    }

    CoUninitialize();
  }

  void mic_write_wasapi_t::cleanup() {
    stop_render_thread = true;
    if (render_event) {
      SetEvent(render_event.get());
    }

    if (render_thread.joinable()) {
      render_thread.join();
    }

    if (audio_client) {
      audio_client->Stop();
    }

    if (audio_render != nullptr) {
      audio_render->Release();
      audio_render = nullptr;
    }

    audio_client.reset();
    device_enum.reset();

    // Revert any device-format mutations performed during initialize_device.
    // Each successful revert removes the corresponding entry from the
    // persistence file so the startup sweep won't try to revert again.
    // We iterate even if revert fails -- next startup will retry via the
    // sweep.
    for (const auto &device_id : mutated_device_ids) {
      if (revert_steam_mic_format_for_device(device_id)) {
        BOOST_LOG(info) << "Reverted Steam microphone device format on cleanup";
      } else {
        BOOST_LOG(warning) << "Couldn't revert Steam microphone device format on cleanup -- "
                              "startup sweep will retry on next launch";
      }
    }
    mutated_device_ids.clear();

    active_format_storage.clear();
    buffer_frame_count = 0;
    active_format = {};
    target_device_name.clear();
    first_packet_written_logged = false;
    render_event.reset();
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      pending_frames.clear();
    }
    expected_sequence_number = 0;
    has_playout_cursor = false;
    playout_started = false;
    playout_wait_logged = false;
  }

  // Public startup sweep: revert any Steam Streaming Microphone format
  // mutations that were persisted but never cleaned up (e.g. because the
  // previous Apollo process was killed before it could revert). This is the
  // belt-and-suspenders fallback: it catches abnormal-exit cases where the
  // mic_write_wasapi_t::cleanup() path never ran.
  void sweep_orphan_steam_mic_formats() {
    auto entries = load_all_prev_format_entries();
    if (entries.empty()) {
      return;
    }

    BOOST_LOG(info) << "Steam microphone format orphan sweep: " << entries.size()
                    << " candidate entry/entries found.";

    policy_t policy;
    HRESULT hr = CoCreateInstance(
      CLSID_CPolicyConfigClient,
      nullptr,
      CLSCTX_ALL,
      IID_IPolicyConfig,
      reinterpret_cast<void **>(&policy));
    if (FAILED(hr) || !policy) {
      BOOST_LOG(warning) << "Steam microphone format orphan sweep: failed to create policy config: 0x"
                         << util::hex(hr).to_string_view();
      return;
    }

    // Keep entries we couldn't restore (e.g. device temporarily disconnected,
    // transient COM failure) so a later Apollo launch with a healthier system
    // state can retry. Drop only the entries we successfully restored.
    std::vector<prev_format_entry_t> remaining;
    remaining.reserve(entries.size());
    for (auto &e : entries) {
      if (restore_device_format(policy.get(), e.device_id, e.format_bytes)) {
        BOOST_LOG(info) << "Steam microphone format orphan sweep: restored device format";
      } else {
        BOOST_LOG(warning) << "Steam microphone format orphan sweep: SetDeviceFormat failed for an entry; "
                              "keeping record for retry on next launch.";
        remaining.push_back(std::move(e));
      }
    }

    // Rewrite the file with only the entries that still need to be restored.
    write_all_prev_format_entries(remaining);
  }
}  // namespace platf::audio
