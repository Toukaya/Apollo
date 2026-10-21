/**
 * @file src/platform/windows/display_wgc.cpp
 * @brief Definitions for WinRT Windows.Graphics.Capture API
 */
// platform includes
#include <dxgi1_2.h>

// Gross hack to work around MINGW-packages#22160
#define ____FIReference_1_boolean_INTERFACE_DEFINED__

#include <Windows.Graphics.Capture.Interop.h>
#include <winrt/windows.foundation.h>
#include <winrt/windows.foundation.metadata.h>
#include <winrt/windows.graphics.directx.direct3d11.h>

// local includes
#include "display.h"
#include "misc.h"
#include "src/logging.h"
#include "display_wgc_window.h"

namespace platf {
  using namespace std::literals;
}

namespace winrt {
  using namespace Windows::Foundation;
  using namespace Windows::Foundation::Metadata;
  using namespace Windows::Graphics::Capture;
  using namespace Windows::Graphics::DirectX::Direct3D11;

  extern "C" {
    HRESULT __stdcall CreateDirect3D11DeviceFromDXGIDevice(::IDXGIDevice *dxgiDevice, ::IInspectable **graphicsDevice);
  }

  /**
   * Windows structures sometimes have compile-time GUIDs. GCC supports this, but in a roundabout way.
   * If WINRT_IMPL_HAS_DECLSPEC_UUID is true, then the compiler supports adding this attribute to a struct. For example, Visual Studio.
   * If not, then MinGW GCC has a workaround to assign a GUID to a structure.
   */
  struct
#if WINRT_IMPL_HAS_DECLSPEC_UUID
    __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
#endif
    IDirect3DDxgiInterfaceAccess: ::IUnknown {
    virtual HRESULT __stdcall GetInterface(REFIID id, void **object) = 0;
  };
}  // namespace winrt
#if !WINRT_IMPL_HAS_DECLSPEC_UUID
static constexpr GUID GUID__IDirect3DDxgiInterfaceAccess = {
  0xA9B3D012,
  0x3DF2,
  0x4EE3,
  {0xB8, 0xD1, 0x86, 0x95, 0xF4, 0x57, 0xD3, 0xC1}
  // compare with __declspec(uuid(...)) for the struct above.
};

template<>
constexpr auto __mingw_uuidof<winrt::IDirect3DDxgiInterfaceAccess>() -> GUID const & {
  return GUID__IDirect3DDxgiInterfaceAccess;
}
#endif

namespace platf::dxgi {
  wgc_capture_t::wgc_capture_t() {
    InitializeConditionVariable(&frame_present_cv);
  }

  wgc_capture_t::~wgc_capture_t() {
    if (capture_session) {
      capture_session.Close();
    }
    if (frame_pool) {
      frame_pool.Close();
    }
    item = nullptr;
    capture_session = nullptr;
    frame_pool = nullptr;
  }

  /**
   * @brief Initialize the Windows.Graphics.Capture backend.
   * @return 0 on success, -1 on failure.
   */
  int wgc_capture_t::init(display_base_t *display, const ::video::config_t &config) {
    HRESULT status;
    dxgi::dxgi_t dxgi;
    winrt::com_ptr<::IInspectable> d3d_comhandle;
    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows!"sv;
        return -1;
      }
      if (FAILED(status = display->device->QueryInterface(IID_IDXGIDevice, (void **) &dxgi))) {
        BOOST_LOG(error) << "Failed to query DXGI interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
      if (FAILED(status = winrt::CreateDirect3D11DeviceFromDXGIDevice(*&dxgi, d3d_comhandle.put()))) {
        BOOST_LOG(error) << "Failed to query WinRT DirectX interface from device [0x"sv << util::hex(status).to_string_view() << ']';
        return -1;
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire device: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }

    DXGI_OUTPUT_DESC output_desc;
    uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();
    display->output->GetDesc(&output_desc);

    auto monitor_factory = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
    if (monitor_factory == nullptr ||
        FAILED(status = monitor_factory->CreateForMonitor(output_desc.Monitor, winrt::guid_of<winrt::IGraphicsCaptureItem>(), winrt::put_abi(item)))) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to acquire display: [0x"sv << util::hex(status).to_string_view() << ']';
      return -1;
    }

    if (config.dynamicRange) {
      display->capture_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    } else {
      display->capture_format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }

    try {
      frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(uwp_device, static_cast<winrt::Windows::Graphics::DirectX::DirectXPixelFormat>(display->capture_format), 2, item.Size());
      capture_session = frame_pool.CreateCaptureSession(item);
      frame_pool.FrameArrived({this, &wgc_capture_t::on_frame_arrived});
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to create capture session: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
        capture_session.IsBorderRequired(false);
      } else {
        BOOST_LOG(warning) << "Can't disable colored border around capture area on this version of Windows";
      }
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to disable border around capture area: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
        capture_session.MinUpdateInterval(winrt::TimeSpan{ 10000000 / (config.framerate * 2) });
      }
      else {
        BOOST_LOG(warning) << "Can't set MinUpdateInterval";
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Screen capture may not be fully supported on this device for this release of Windows: failed to set MinUpdateInterval: [0x"sv << util::hex(e.code()).to_string_view() << ']';
    }
    try {
      capture_session.StartCapture();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(error) << "Screen capture is not supported on this device for this release of Windows: failed to start capture: [0x"sv << util::hex(e.code()).to_string_view() << ']';
      return -1;
    }
    return 0;
  }

  /**
   * This function runs in a separate thread spawned by the frame pool and is a producer of frames.
   * To maintain parity with the original display interface, this frame will be consumed by the capture thread.
   * Acquire a read-write lock, make the produced frame available to the capture thread, then wake the capture thread.
   */
  void wgc_capture_t::on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
    try {
      frame = sender.TryGetNextFrame();
    } catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Failed to capture frame: "sv << e.code();
      return;
    }
    if (frame != nullptr) {
      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }

      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  }

  /**
   * @brief Get the next frame from the producer thread.
   * If not available, the capture thread blocks until one is, or the wait times out.
   * @param timeout how long to wait for the next frame
   * @param out a texture containing the frame just captured
   * @param out_time the timestamp of the frame just captured
   */
  capture_e wgc_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out, uint64_t &out_time) {
    // this CONSUMER runs in the capture thread
    release_frame();

    AcquireSRWLockExclusive(&frame_lock);
    if (produced_frame == nullptr && SleepConditionVariableSRW(&frame_present_cv, &frame_lock, timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&frame_lock);
      if (GetLastError() == ERROR_TIMEOUT) {
        return capture_e::timeout;
      } else {
        return capture_e::error;
      }
    }
    if (produced_frame) {
      consumed_frame = produced_frame;
      produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&frame_lock);
    if (consumed_frame == nullptr) {  // spurious wakeup
      return capture_e::timeout;
    }

    auto capture_access = consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
    if (capture_access == nullptr) {
      return capture_e::error;
    }
    capture_access->GetInterface(IID_ID3D11Texture2D, (void **) out);
    out_time = consumed_frame.SystemRelativeTime().count();  // raw ticks from query performance counter
    return capture_e::ok;
  }

  capture_e wgc_capture_t::release_frame() {
    if (consumed_frame != nullptr) {
      consumed_frame.Close();
      consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  int wgc_capture_t::set_cursor_visible(bool x) {
    try {
      if (capture_session.IsCursorCaptureEnabled() != x) {
        capture_session.IsCursorCaptureEnabled(x);
      }
      return 0;
    } catch (winrt::hresult_error &) {
      return -1;
    }
  }

  int display_wgc_ram_t::init(const ::video::config_t &config, const std::string &display_name) {
    if (display_base_t::init(config, display_name) || dup.init(this, config)) {
      return -1;
    }

    texture.reset();
    return 0;
  }

  /**
   * @brief Get the next frame from the Windows.Graphics.Capture API and copy it into a new snapshot texture.
   * @param pull_free_image_cb call this to get a new free image from the video subsystem.
   * @param img_out the captured frame is returned here
   * @param timeout how long to wait for the next frame
   * @param cursor_visible whether to capture the cursor
   */
  capture_e display_wgc_ram_t::snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor_visible) {
    HRESULT status;
    texture2d_t src;
    uint64_t frame_qpc;
    dup.set_cursor_visible(cursor_visible);
    auto capture_status = dup.next_frame(timeout, &src, frame_qpc);
    if (capture_status != capture_e::ok) {
      return capture_status;
    }

    auto frame_timestamp = std::chrono::steady_clock::now() - qpc_time_difference(qpc_counter(), frame_qpc);
    D3D11_TEXTURE2D_DESC desc;
    src->GetDesc(&desc);

    // Create the staging texture if it doesn't exist. It should match the source in size and format.
    if (texture == nullptr) {
      capture_format = desc.Format;
      BOOST_LOG(info) << "Capture format ["sv << dxgi_format_to_string(capture_format) << ']';

      D3D11_TEXTURE2D_DESC t {};
      t.Width = width;
      t.Height = height;
      t.MipLevels = 1;
      t.ArraySize = 1;
      t.SampleDesc.Count = 1;
      t.Usage = D3D11_USAGE_STAGING;
      t.Format = capture_format;
      t.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      auto status = device->CreateTexture2D(&t, nullptr, &texture);

      if (FAILED(status)) {
        BOOST_LOG(error) << "Failed to create staging texture [0x"sv << util::hex(status).to_string_view() << ']';
        return capture_e::error;
      }
    }

    // It's possible for our display enumeration to race with mode changes and result in
    // mismatched image pool and desktop texture sizes. If this happens, just reinit again.
    if (desc.Width != width || desc.Height != height) {
      BOOST_LOG(info) << "Capture size changed ["sv << width << 'x' << height << " -> "sv << desc.Width << 'x' << desc.Height << ']';
      return capture_e::reinit;
    }
    // It's also possible for the capture format to change on the fly. If that happens,
    // reinitialize capture to try format detection again and create new images.
    if (capture_format != desc.Format) {
      BOOST_LOG(info) << "Capture format changed ["sv << dxgi_format_to_string(capture_format) << " -> "sv << dxgi_format_to_string(desc.Format) << ']';
      return capture_e::reinit;
    }

    // Copy from GPU to CPU
    device_ctx->CopyResource(texture.get(), src.get());

    if (!pull_free_image_cb(img_out)) {
      return capture_e::interrupted;
    }
    auto img = (img_t *) img_out.get();

    // Map the staging texture for CPU access (making it inaccessible for the GPU)
    if (FAILED(status = device_ctx->Map(texture.get(), 0, D3D11_MAP_READ, 0, &img_info))) {
      BOOST_LOG(error) << "Failed to map texture [0x"sv << util::hex(status).to_string_view() << ']';

      return capture_e::error;
    }

    // Now that we know the capture format, we can finish creating the image
    if (complete_img(img, false)) {
      device_ctx->Unmap(texture.get(), 0);
      img_info.pData = nullptr;
      return capture_e::error;
    }

    std::copy_n((std::uint8_t *) img_info.pData, height * img_info.RowPitch, (std::uint8_t *) img->data);

    // Unmap the staging texture to allow GPU access again
    device_ctx->Unmap(texture.get(), 0);
    img_info.pData = nullptr;

    if (img) {
      img->frame_timestamp = frame_timestamp;
    }

    return capture_e::ok;
  }

  capture_e display_wgc_ram_t::release_snapshot() {
    return dup.release_frame();
  }
}  // namespace platf::dxgi

namespace platf::seamless {
  struct window_capture_t::impl_t {
    winrt::GraphicsCaptureItem item {nullptr};
    winrt::Direct3D11CaptureFramePool frame_pool {nullptr};
    winrt::GraphicsCaptureSession capture_session {nullptr};

    // The producer (the frame pool's thread) hands the newest frame to the
    // consumer (the capture thread) under this lock, exactly as the monitor
    // capture does.
    SRWLOCK frame_lock {SRWLOCK_INIT};
    CONDITION_VARIABLE frame_present_cv {CONDITION_VARIABLE_INIT};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame produced_frame {nullptr};
    winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame consumed_frame {nullptr};
    // The newest texture, kept with our own reference so a pass that changes one
    // window can still copy the others' last content.
    winrt::com_ptr<ID3D11Texture2D> last_frame;
    bool running = false;

    ~impl_t() {
      close();
    }

    void close() {
      running = false;
      if (capture_session) {
        capture_session.Close();
        capture_session = nullptr;
      }
      if (frame_pool) {
        frame_pool.Close();
        frame_pool = nullptr;
      }
      item = nullptr;
      if (consumed_frame) {
        consumed_frame.Close();
        consumed_frame = nullptr;
      }
      if (produced_frame) {
        produced_frame.Close();
        produced_frame = nullptr;
      }
    }

    void on_frame_arrived(winrt::Direct3D11CaptureFramePool const &sender, winrt::IInspectable const &) {
      winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame frame {nullptr};
      try {
        frame = sender.TryGetNextFrame();
      }
      catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Seamless: window frame capture failed: 0x"sv << util::hex(e.code()).to_string_view();
        return;
      }
      if (frame == nullptr) {
        return;
      }

      AcquireSRWLockExclusive(&frame_lock);
      if (produced_frame) {
        produced_frame.Close();
      }
      produced_frame = frame;
      ReleaseSRWLockExclusive(&frame_lock);
      WakeConditionVariable(&frame_present_cv);
    }
  };

  window_capture_t::window_capture_t() : _impl(std::make_unique<impl_t>()) {}

  window_capture_t::~window_capture_t() = default;

  int window_capture_t::init(HWND hwnd, ID3D11Device *device, std::uint32_t framerate) {
    if (hwnd == nullptr || device == nullptr) {
      return -1;
    }

    try {
      if (!winrt::GraphicsCaptureSession::IsSupported()) {
        BOOST_LOG(warning) << "Seamless: this system cannot capture a window"sv;
        return -1;
      }

      // The frame pool must hand out textures from the same device as the canvas,
      // or the cell copy becomes a cross device transfer.
      winrt::com_ptr<::IDXGIDevice> dxgi;
      if (FAILED(device->QueryInterface(IID_IDXGIDevice, dxgi.put_void()))) {
        BOOST_LOG(warning) << "Seamless: the canvas device has no DXGI device"sv;
        return -1;
      }
      winrt::com_ptr<::IInspectable> d3d_comhandle;
      if (FAILED(winrt::CreateDirect3D11DeviceFromDXGIDevice(dxgi.get(), d3d_comhandle.put()))) {
        BOOST_LOG(warning) << "Seamless: no WinRT device for the canvas device"sv;
        return -1;
      }
      auto uwp_device = d3d_comhandle.as<winrt::IDirect3DDevice>();

      auto interop = winrt::get_activation_factory<winrt::GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
      if (interop == nullptr ||
          FAILED(interop->CreateForWindow(hwnd, winrt::guid_of<winrt::IGraphicsCaptureItem>(),
                                          winrt::put_abi(_impl->item)))) {
        BOOST_LOG(info) << "Seamless: window "sv << (std::uint32_t) (std::uintptr_t) hwnd
                        << " cannot be captured, it leaves the table"sv;
        return -1;
      }

      _impl->frame_pool = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
        uwp_device, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2,
        _impl->item.Size());
      _impl->capture_session = _impl->frame_pool.CreateCaptureSession(_impl->item);
      _impl->frame_pool.FrameArrived({_impl.get(), &impl_t::on_frame_arrived});

      // The window is drawn by the client, so its own pointer does not travel.
      _impl->capture_session.IsCursorCaptureEnabled(false);

      try {
        if (winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"IsBorderRequired")) {
          _impl->capture_session.IsBorderRequired(false);
        }
      }
      catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Seamless: cannot disable the capture border: 0x"sv << util::hex(e.code()).to_string_view();
      }

      try {
        if (framerate > 0 && winrt::ApiInformation::IsPropertyPresent(L"Windows.Graphics.Capture.GraphicsCaptureSession", L"MinUpdateInterval")) {
          _impl->capture_session.MinUpdateInterval(winrt::TimeSpan {10000000 / (framerate * 2)});
        }
      }
      catch (winrt::hresult_error &e) {
        BOOST_LOG(warning) << "Seamless: cannot bound the window update interval: 0x"sv << util::hex(e.code()).to_string_view();
      }

      _impl->capture_session.StartCapture();
      _impl->running = true;
      return 0;
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Seamless: window capture failed: 0x"sv << util::hex(e.code()).to_string_view();
      _impl->close();
      return -1;
    }
  }

  bool window_capture_t::valid() const {
    return _impl->running && _impl->capture_session != nullptr;
  }

  capture_e window_capture_t::next_frame(std::chrono::milliseconds timeout, ID3D11Texture2D **out,
                                         std::uint64_t &out_time) {
    if (!valid()) {
      return capture_e::error;
    }

    release_frame();

    AcquireSRWLockExclusive(&_impl->frame_lock);
    if (_impl->produced_frame == nullptr &&
        SleepConditionVariableSRW(&_impl->frame_present_cv, &_impl->frame_lock, (DWORD) timeout.count(), 0) == 0) {
      ReleaseSRWLockExclusive(&_impl->frame_lock);
      return GetLastError() == ERROR_TIMEOUT ? capture_e::timeout : capture_e::error;
    }
    if (_impl->produced_frame) {
      _impl->consumed_frame = _impl->produced_frame;
      _impl->produced_frame = nullptr;
    }
    ReleaseSRWLockExclusive(&_impl->frame_lock);

    if (_impl->consumed_frame == nullptr) {
      return capture_e::timeout;
    }

    // This runs on the capture thread: a winrt error escaping here would take the
    // process down, so the extraction is guarded like the frame arrival is.
    try {
      auto access = _impl->consumed_frame.Surface().as<winrt::IDirect3DDxgiInterfaceAccess>();
      if (access == nullptr || FAILED(access->GetInterface(IID_ID3D11Texture2D, (void **) out)) || *out == nullptr) {
        return capture_e::error;
      }
    }
    catch (winrt::hresult_error &e) {
      BOOST_LOG(warning) << "Seamless: window frame extraction failed: 0x"sv << util::hex(e.code()).to_string_view();
      return capture_e::error;
    }

    _impl->last_frame.copy_from(*out);

    out_time = _impl->consumed_frame.SystemRelativeTime().count();
    return capture_e::ok;
  }

  ID3D11Texture2D *window_capture_t::latest_texture() const {
    return _impl->last_frame.get();
  }

  capture_e window_capture_t::release_frame() {
    if (_impl->consumed_frame) {
      _impl->consumed_frame.Close();
      _impl->consumed_frame = nullptr;
    }
    return capture_e::ok;
  }

  void window_capture_t::close() {
    _impl->close();
  }
}  // namespace platf::seamless
