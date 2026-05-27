#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "tirtc/audio.h"
#include "tirtc/audio_io.h"
#include "tirtc/av.h"
#include "tirtc/trp.h"
#include "tirtc/video_io.h"

namespace {

constexpr int kExitRuntimeFailed = 1;
constexpr int kExitArgError = 2;
constexpr int kConnectTimeoutMs = 15000;
constexpr int kDefaultDurationSeconds = 60;
constexpr int kMaxStreamId = 15;
constexpr auto kProgressLogInterval = std::chrono::seconds(1);

using Clock = std::chrono::steady_clock;

struct CliOptions {
  std::string app_id;
  std::string endpoint;
  std::string device_id;
  std::string token;
  int duration_seconds = kDefaultDurationSeconds;
  int require_audio = 1;
  int require_video = 1;
  bool has_audio_stream_id = false;
  bool has_video_stream_id = false;
  uint8_t audio_stream_id = 0;
  uint8_t video_stream_id = 0;
};

struct ClientStats {
  std::mutex mutex;
  std::condition_variable condition;
  Clock::time_point process_started = Clock::now();
  Clock::time_point connected_at{};
  bool connected = false;
  bool disconnected = false;
  bool failed = false;
  int error_code = 0;
  std::string error_message;
  uint64_t audio_frames = 0;
  uint64_t video_frames = 0;
  uint64_t audio_bytes = 0;
  uint64_t video_bytes = 0;
  std::optional<int64_t> first_audio_ms;
  std::optional<int64_t> first_video_ms;
};

struct ClientStatsSample {
  bool connected = false;
  uint64_t audio_frames = 0;
  uint64_t video_frames = 0;
  uint64_t audio_bytes = 0;
  uint64_t video_bytes = 0;
};

struct Resources {
  bool runtime_initialized = false;
  bool connect_started = false;
  bool audio_attached = false;
  bool video_attached = false;
  bool video_view_attached = false;
  TirtcConn* connection = nullptr;
  TirtcAudioOutput* audio_output = nullptr;
  TirtcVideoOutput* video_output = nullptr;
  TirtcAudioAout* audio_sink = nullptr;
  TirtcVideoVout* video_sink = nullptr;
};

std::string json_escape(const std::string& value) {
  std::ostringstream out;
  for (unsigned char ch : value) {
    switch (ch) {
      case '\\':
        out << "\\\\";
        break;
      case '"':
        out << "\\\"";
        break;
      case '\n':
        out << "\\n";
        break;
      case '\r':
        out << "\\r";
        break;
      case '\t':
        out << "\\t";
        break;
      default:
        if (ch < 0x20) {
          out << "\\u00";
          const char* hex = "0123456789abcdef";
          out << hex[(ch >> 4) & 0x0f] << hex[ch & 0x0f];
        } else {
          out << static_cast<char>(ch);
        }
        break;
    }
  }
  return out.str();
}

int64_t elapsed_ms(Clock::time_point start, Clock::time_point end = Clock::now()) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
}

ClientStatsSample capture_stats_sample(const ClientStats& stats) {
  ClientStatsSample sample{};
  sample.connected = stats.connected;
  sample.audio_frames = stats.audio_frames;
  sample.video_frames = stats.video_frames;
  sample.audio_bytes = stats.audio_bytes;
  sample.video_bytes = stats.video_bytes;
  return sample;
}

void set_failure(ClientStats* stats, int error_code, const std::string& message) {
  std::lock_guard<std::mutex> lock(stats->mutex);
  if (!stats->failed) {
    stats->failed = true;
    stats->error_code = error_code;
    stats->error_message = message;
  }
  stats->condition.notify_all();
}

std::string owned_string_to_string(TirtcOwnedString* owned_message) {
  std::string message;
  if (owned_message != nullptr && owned_message->data != nullptr) {
    message = owned_message->data;
  }
  tirtc_owned_string_release(owned_message);
  return message;
}

void on_conn_state_changed(TirtcConn*, TirtcConnState state, TirtcError error, void* user_data) {
  auto* stats = static_cast<ClientStats*>(user_data);
  std::lock_guard<std::mutex> lock(stats->mutex);
  if (state == TIRTC_CONN_STATE_CONNECTED) {
    stats->connected = true;
    stats->connected_at = Clock::now();
  } else if (state == TIRTC_CONN_STATE_DISCONNECTED) {
    stats->disconnected = true;
    if (!stats->connected && error != TIRTC_ERROR_OK) {
      stats->failed = true;
      stats->error_code = static_cast<int>(error);
      stats->error_message = "connection disconnected before connected";
    }
  }
  stats->condition.notify_all();
}

void on_audio_output_error(TirtcAudioOutput*, TirtcError error, TirtcOwnedString* owned_message,
                           void* user_data) {
  const std::string message = owned_string_to_string(owned_message);
  set_failure(static_cast<ClientStats*>(user_data), static_cast<int>(error),
              message.empty() ? "audio output error" : message);
}

void on_video_output_error(TirtcVideoOutput*, TirtcError error, TirtcOwnedString* owned_message,
                           void* user_data) {
  const std::string message = owned_string_to_string(owned_message);
  set_failure(static_cast<ClientStats*>(user_data), static_cast<int>(error),
              message.empty() ? "video output error" : message);
}

void on_audio_frame(TirtcAudioAout*, const TirtcAudioPcmFrame* frame, void* user_data) {
  auto* stats = static_cast<ClientStats*>(user_data);
  std::lock_guard<std::mutex> lock(stats->mutex);
  stats->audio_frames += 1;
  if (frame != nullptr) {
    stats->audio_bytes += frame->data_bytes;
  }
  if (!stats->first_audio_ms.has_value() && stats->connected) {
    stats->first_audio_ms = elapsed_ms(stats->connected_at);
  }
  stats->condition.notify_all();
}

void on_audio_sink_error(TirtcAudioAout*, TirtcError error, void* user_data) {
  set_failure(static_cast<ClientStats*>(user_data), static_cast<int>(error), "audio sink error");
}

void on_video_frame(TirtcVideoVout*, const TirtcVideoPixelFrame* frame, void* user_data) {
  auto* stats = static_cast<ClientStats*>(user_data);
  std::lock_guard<std::mutex> lock(stats->mutex);
  stats->video_frames += 1;
  if (frame != nullptr) {
    for (uint32_t index = 0; index < frame->plane_count; ++index) {
      stats->video_bytes += frame->plane_bytes[index];
    }
  }
  if (!stats->first_video_ms.has_value() && stats->connected) {
    stats->first_video_ms = elapsed_ms(stats->connected_at);
  }
  stats->condition.notify_all();
}

void on_video_sink_error(TirtcVideoVout*, TirtcError error, void* user_data) {
  set_failure(static_cast<ClientStats*>(user_data), static_cast<int>(error), "video sink error");
}

void print_usage() {
  std::cout
      << "Usage: tirtc_client_example --app-id ID --endpoint URL --device-id ID --token TOKEN "
         "[--audio-stream-id 0..15] [--video-stream-id 0..15|--stream-id 0..15] "
         "[--duration-seconds N] [--require-audio 0|1] [--require-video 0|1]\n";
}

bool parse_bool(const char* value, int* out) {
  if (std::strcmp(value, "0") == 0) {
    *out = 0;
    return true;
  }
  if (std::strcmp(value, "1") == 0) {
    *out = 1;
    return true;
  }
  return false;
}

bool parse_int_range(const char* value, int min_value, int max_value, int* out) {
  char* end = nullptr;
  const long parsed = std::strtol(value, &end, 10);
  if (end == value || *end != '\0' || parsed < min_value || parsed > max_value) {
    return false;
  }
  *out = static_cast<int>(parsed);
  return true;
}

std::string join_names(const std::vector<std::string>& names) {
  std::ostringstream out;
  for (size_t index = 0; index < names.size(); ++index) {
    if (index > 0) {
      out << ", ";
    }
    out << names[index];
  }
  return out.str();
}

int parse_args(int argc, char** argv, CliOptions* options, std::string* error_message) {
  bool has_stream_id = false;
  uint8_t stream_id = 0;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "-h" || arg == "--help") {
      print_usage();
      return 1;
    }
    auto require_value = [&](const std::string& name) -> const char* {
      if (index + 1 >= argc) {
        *error_message = "missing value for " + name;
        return nullptr;
      }
      index += 1;
      return argv[index];
    };
    if (arg == "--app-id") {
      const char* value = require_value(arg);
      if (value == nullptr)
        return kExitArgError;
      options->app_id = value;
    } else if (arg == "--endpoint") {
      const char* value = require_value(arg);
      if (value == nullptr)
        return kExitArgError;
      options->endpoint = value;
    } else if (arg == "--device-id") {
      const char* value = require_value(arg);
      if (value == nullptr)
        return kExitArgError;
      options->device_id = value;
    } else if (arg == "--token") {
      const char* value = require_value(arg);
      if (value == nullptr)
        return kExitArgError;
      options->token = value;
    } else if (arg == "--duration-seconds") {
      const char* value = require_value(arg);
      int parsed = 0;
      if (value == nullptr)
        return kExitArgError;
      if (!parse_int_range(value, 1, 24 * 60 * 60, &parsed)) {
        *error_message = "--duration-seconds must be a positive integer";
        return kExitArgError;
      }
      options->duration_seconds = parsed;
    } else if (arg == "--require-audio") {
      const char* value = require_value(arg);
      if (value == nullptr || !parse_bool(value, &options->require_audio)) {
        *error_message = "--require-audio must be 0 or 1";
        return kExitArgError;
      }
    } else if (arg == "--require-video") {
      const char* value = require_value(arg);
      if (value == nullptr || !parse_bool(value, &options->require_video)) {
        *error_message = "--require-video must be 0 or 1";
        return kExitArgError;
      }
    } else if (arg == "--audio-stream-id") {
      const char* value = require_value(arg);
      int parsed = 0;
      if (value == nullptr || !parse_int_range(value, 0, kMaxStreamId, &parsed)) {
        *error_message = "--audio-stream-id must be in 0..15";
        return kExitArgError;
      }
      options->audio_stream_id = static_cast<uint8_t>(parsed);
      options->has_audio_stream_id = true;
    } else if (arg == "--video-stream-id" || arg == "--stream-id") {
      const char* value = require_value(arg);
      int parsed = 0;
      if (value == nullptr || !parse_int_range(value, 0, kMaxStreamId, &parsed)) {
        *error_message = arg + " must be in 0..15";
        return kExitArgError;
      }
      if (arg == "--stream-id") {
        has_stream_id = true;
        stream_id = static_cast<uint8_t>(parsed);
      } else {
        options->video_stream_id = static_cast<uint8_t>(parsed);
        options->has_video_stream_id = true;
      }
    } else {
      *error_message = "unknown argument: " + arg;
      return kExitArgError;
    }
  }

  if (has_stream_id && options->has_video_stream_id) {
    *error_message = "--stream-id and --video-stream-id are mutually exclusive";
    return kExitArgError;
  }
  if (has_stream_id) {
    options->video_stream_id = stream_id;
    options->has_video_stream_id = true;
  }
  std::vector<std::string> missing_required_args;
  if (options->app_id.empty()) {
    missing_required_args.push_back("--app-id");
  }
  if (options->endpoint.empty()) {
    missing_required_args.push_back("--endpoint");
  }
  if (options->device_id.empty()) {
    missing_required_args.push_back("--device-id");
  }
  if (options->token.empty()) {
    missing_required_args.push_back("--token");
  }
  if (!missing_required_args.empty()) {
    *error_message = "missing required argument(s): " + join_names(missing_required_args);
    return kExitArgError;
  }
  if (options->require_audio != 0 && !options->has_audio_stream_id) {
    *error_message = "--audio-stream-id is required when --require-audio is 1";
    return kExitArgError;
  }
  if (options->require_video != 0 && !options->has_video_stream_id) {
    *error_message = "--stream-id or --video-stream-id is required when --require-video is 1";
    return kExitArgError;
  }
  if (options->has_audio_stream_id && options->has_video_stream_id &&
      options->audio_stream_id == options->video_stream_id) {
    *error_message = "audio and video stream ids must be different";
    return kExitArgError;
  }
  return 0;
}

void print_summary(const ClientStats& stats, bool passed) {
  const int64_t duration_ms = elapsed_ms(stats.process_started);
  std::cout << "TIRTC_CLIENT_SUMMARY {"
            << "\"status\":\"" << (passed ? "passed" : "failed") << "\","
            << "\"duration_ms\":" << duration_ms << ","
            << "\"audio_frames\":" << stats.audio_frames << ","
            << "\"video_frames\":" << stats.video_frames << ","
            << "\"audio_bytes\":" << stats.audio_bytes << ","
            << "\"video_bytes\":" << stats.video_bytes << ","
            << "\"first_audio_ms\":";
  if (stats.first_audio_ms.has_value()) {
    std::cout << *stats.first_audio_ms;
  } else {
    std::cout << "null";
  }
  std::cout << ",\"first_video_ms\":";
  if (stats.first_video_ms.has_value()) {
    std::cout << *stats.first_video_ms;
  } else {
    std::cout << "null";
  }
  std::cout << ",\"error_code\":" << (passed ? 0 : stats.error_code) << ","
            << "\"error_message\":\"" << json_escape(passed ? "" : stats.error_message) << "\"}"
            << std::endl;
}

void print_progress(const ClientStatsSample& sample, const Resources& resources,
                    Clock::time_point process_started) {
  std::cerr << "TIRTC_CLIENT_PROGRESS elapsed_ms=" << elapsed_ms(process_started)
            << " connected=" << (sample.connected ? 1 : 0)
            << " audio_frames=" << sample.audio_frames << " audio_bytes=" << sample.audio_bytes
            << " video_frames=" << sample.video_frames << " video_bytes=" << sample.video_bytes;

  if (resources.connection != nullptr) {
    TirtcConnMetricsSnapshot snapshot{};
    const TirtcError error = tirtc_metrics_conn_get_snapshot(resources.connection, &snapshot);
    if (error == TIRTC_ERROR_OK) {
      std::cerr << " conn_has_start=" << snapshot.has_connect_start
                << " conn_has_connected=" << snapshot.has_connected;
    } else {
      std::cerr << " conn_snapshot_error=" << static_cast<int>(error);
    }
  }

  if (resources.audio_output != nullptr) {
    TirtcAudioOutputMetricsSnapshot metrics{};
    const TirtcError metrics_error =
        tirtc_metrics_audio_output_get_snapshot(resources.audio_output, &metrics);
    if (metrics_error == TIRTC_ERROR_OK) {
      std::cerr << " audio_input_kbps=" << metrics.input_bitrate_kbps
                << " audio_input_pps=" << metrics.input_packet_rate
                << " audio_render_rate=" << metrics.render_callback_rate
                << " audio_pending_undecoded_ms=" << metrics.pending.undecoded_duration_ms
                << " audio_pending_decoded_ms=" << metrics.pending.decoded_duration_ms
                << " audio_stutter_count=" << metrics.stutter.session_stutter_count
                << " audio_stutter_ms=" << metrics.stutter.session_stutter_total_ms;
    } else {
      std::cerr << " audio_metrics_error=" << static_cast<int>(metrics_error);
    }

    TirtcAudioOutputDebugSnapshot debug{};
    const TirtcError debug_error =
        tirtc_audio_output_get_debug_snapshot(resources.audio_output, &debug);
    if (debug_error == TIRTC_ERROR_OK) {
      std::cerr << " audio_codec=" << static_cast<int>(debug.codec)
                << " audio_sample_rate_hz=" << debug.sample_rate_hz
                << " audio_channels=" << debug.channels;
    } else {
      std::cerr << " audio_debug_error=" << static_cast<int>(debug_error);
    }
  }

  if (resources.video_output != nullptr) {
    TirtcVideoOutputMetricsSnapshot metrics{};
    const TirtcError metrics_error =
        tirtc_metrics_video_output_get_snapshot(resources.video_output, &metrics);
    if (metrics_error == TIRTC_ERROR_OK) {
      std::cerr << " video_input_kbps=" << metrics.input_bitrate_kbps
                << " video_input_fps=" << metrics.input_fps
                << " video_decoded_fps=" << metrics.decoded_fps
                << " video_render_fps=" << metrics.render_fps
                << " video_pending_undecoded_ms=" << metrics.pending.undecoded_duration_ms
                << " video_pending_decoded_ms=" << metrics.pending.decoded_duration_ms
                << " video_stutter_count=" << metrics.stutter.session_stutter_count
                << " video_stutter_ms=" << metrics.stutter.session_stutter_total_ms;
    } else {
      std::cerr << " video_metrics_error=" << static_cast<int>(metrics_error);
    }

    TirtcVideoOutputDebugSnapshot debug{};
    const TirtcError debug_error =
        tirtc_video_output_get_debug_snapshot(resources.video_output, &debug);
    if (debug_error == TIRTC_ERROR_OK) {
      std::cerr << " video_codec=" << static_cast<int>(debug.codec)
                << " video_width=" << debug.width << " video_height=" << debug.height
                << " video_decoder_backend=" << static_cast<int>(debug.resolved_decoder_backend);
    } else {
      std::cerr << " video_debug_error=" << static_cast<int>(debug_error);
    }
  }

  std::cerr << std::endl;
}

void cleanup(Resources* resources) {
  if (resources->audio_attached && resources->audio_output != nullptr) {
    (void)tirtc_audio_output_detach(resources->audio_output);
    resources->audio_attached = false;
  }
  if (resources->video_attached && resources->video_output != nullptr) {
    (void)tirtc_video_output_detach(resources->video_output);
    resources->video_attached = false;
  }
  if (resources->video_view_attached && resources->video_output != nullptr) {
    (void)tirtc_video_output_detach_view(resources->video_output);
    resources->video_view_attached = false;
  }
  if (resources->connect_started && resources->connection != nullptr) {
    (void)tirtc_conn_disconnect(resources->connection);
    resources->connect_started = false;
  }
  if (resources->audio_output != nullptr) {
    tirtc_audio_output_destroy(resources->audio_output);
    resources->audio_output = nullptr;
  }
  if (resources->video_output != nullptr) {
    tirtc_video_output_destroy(resources->video_output);
    resources->video_output = nullptr;
  }
  if (resources->audio_sink != nullptr) {
    (void)tirtc_audio_aout_close(resources->audio_sink);
    tirtc_audio_aout_destroy(resources->audio_sink);
    resources->audio_sink = nullptr;
  }
  if (resources->video_sink != nullptr) {
    (void)tirtc_video_vout_close(resources->video_sink);
    tirtc_video_vout_destroy(resources->video_sink);
    resources->video_sink = nullptr;
  }
  if (resources->connection != nullptr) {
    tirtc_conn_destroy(resources->connection);
    resources->connection = nullptr;
  }
  if (resources->runtime_initialized) {
    tirtc_uninit();
    resources->runtime_initialized = false;
  }
}

bool check_error(TirtcError error, const std::string& message, ClientStats* stats) {
  if (error == TIRTC_ERROR_OK) {
    return true;
  }
  set_failure(stats, static_cast<int>(error), message);
  return false;
}

bool create_runtime_objects(const CliOptions& options, ClientStats* stats, Resources* resources) {
  TirtcConnCallbacks conn_callbacks{};
  conn_callbacks.on_state_changed = on_conn_state_changed;
  if (!check_error(tirtc_conn_create(nullptr, &resources->connection), "tirtc_conn_create failed",
                   stats) ||
      !check_error(tirtc_conn_set_callbacks(resources->connection, &conn_callbacks, stats),
                   "tirtc_conn_set_callbacks failed", stats)) {
    return false;
  }

  const bool use_audio = options.has_audio_stream_id;
  const bool use_video = options.has_video_stream_id;
  if (use_audio) {
    TirtcAudioCallbackAoutOptions sink_options{};
    sink_options.on_frame = on_audio_frame;
    sink_options.on_error = on_audio_sink_error;
    sink_options.user_data = stats;
    TirtcAudioOutputObserver observer{};
    observer.on_error = on_audio_output_error;
    if (!check_error(tirtc_audio_aout_create_callback(&sink_options, &resources->audio_sink),
                     "tirtc_audio_aout_create_callback failed", stats) ||
        !check_error(tirtc_audio_output_create(&resources->audio_output),
                     "tirtc_audio_output_create failed", stats) ||
        !check_error(tirtc_audio_output_set_observer(resources->audio_output, &observer, stats),
                     "tirtc_audio_output_set_observer failed", stats) ||
        !check_error(tirtc_audio_output_set_aout(resources->audio_output, resources->audio_sink),
                     "tirtc_audio_output_set_aout failed", stats)) {
      return false;
    }
    TirtcAudioOutputOptions audio_options{};
    audio_options.volume_percent = 100;
    audio_options.agc_level = TIRTC_AUDIO_AGC_LEVEL_DISABLED;
    audio_options.ans_level = TIRTC_AUDIO_ANS_LEVEL_DISABLED;
    if (!check_error(tirtc_audio_output_set_options(resources->audio_output, &audio_options),
                     "tirtc_audio_output_set_options failed", stats) ||
        !check_error(tirtc_audio_output_attach(resources->audio_output, resources->connection,
                                               options.audio_stream_id),
                     "tirtc_audio_output_attach failed", stats)) {
      return false;
    }
    resources->audio_attached = true;
  }

  if (use_video) {
    TirtcVideoCallbackVoutOptions sink_options{};
    sink_options.on_frame = on_video_frame;
    sink_options.on_error = on_video_sink_error;
    sink_options.user_data = stats;
    TirtcVideoOutputObserver observer{};
    observer.on_error = on_video_output_error;
    if (!check_error(tirtc_video_vout_create_callback(&sink_options, &resources->video_sink),
                     "tirtc_video_vout_create_callback failed", stats) ||
        !check_error(tirtc_video_output_create(&resources->video_output),
                     "tirtc_video_output_create failed", stats) ||
        !check_error(tirtc_video_output_set_observer(resources->video_output, &observer, stats),
                     "tirtc_video_output_set_observer failed", stats) ||
        !check_error(tirtc_video_output_attach_view(resources->video_output, resources->video_sink),
                     "tirtc_video_output_attach_view failed", stats)) {
      return false;
    }
    resources->video_view_attached = true;
    if (!check_error(tirtc_video_output_attach(resources->video_output, resources->connection,
                                               options.video_stream_id),
                     "tirtc_video_output_attach failed", stats)) {
      return false;
    }
    resources->video_attached = true;
  }
  return true;
}

bool connect_and_wait(const CliOptions& options, ClientStats* stats, Resources* resources) {
  TirtcConnConnectOptions connect_options{};
  connect_options.remote_id = options.device_id.c_str();
  connect_options.token = options.token.c_str();
  if (!check_error(tirtc_conn_connect(resources->connection, &connect_options),
                   "tirtc_conn_connect failed", stats)) {
    return false;
  }
  resources->connect_started = true;

  std::unique_lock<std::mutex> lock(stats->mutex);
  const bool connected = stats->condition.wait_for(
      lock, std::chrono::milliseconds(kConnectTimeoutMs),
      [&]() { return stats->connected || stats->failed || stats->disconnected; });
  if (!connected || !stats->connected) {
    if (!stats->failed) {
      stats->failed = true;
      stats->error_code = kExitRuntimeFailed;
      stats->error_message = "connect timeout";
    }
    return false;
  }
  return true;
}

bool run_window_and_check(const CliOptions& options, ClientStats* stats,
                          const Resources& resources) {
  const auto deadline = Clock::now() + std::chrono::seconds(options.duration_seconds);
  auto next_progress = Clock::now() + kProgressLogInterval;
  std::unique_lock<std::mutex> lock(stats->mutex);
  while (!stats->failed && Clock::now() < deadline) {
    const auto wait_until = std::min(deadline, next_progress);
    stats->condition.wait_until(lock, wait_until);
    const auto now = Clock::now();
    if (!stats->failed && now >= next_progress) {
      const ClientStatsSample sample = capture_stats_sample(*stats);
      const Clock::time_point process_started = stats->process_started;
      lock.unlock();
      print_progress(sample, resources, process_started);
      lock.lock();
      next_progress = now + kProgressLogInterval;
    }
  }
  if (stats->failed) {
    return false;
  }
  if (options.require_audio != 0 && stats->audio_frames == 0) {
    stats->failed = true;
    stats->error_code = kExitRuntimeFailed;
    stats->error_message = "required audio frames were not received";
    return false;
  }
  if (options.require_video != 0 && stats->video_frames == 0) {
    stats->failed = true;
    stats->error_code = kExitRuntimeFailed;
    stats->error_message = "required video frames were not received";
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  CliOptions options{};
  ClientStats stats{};
  std::string arg_error;
  const int parse_result = parse_args(argc, argv, &options, &arg_error);
  if (parse_result == 1) {
    return 0;
  }
  if (parse_result != 0) {
    stats.error_code = kExitArgError;
    stats.error_message = arg_error;
    print_summary(stats, false);
    std::cerr << "[client-example] " << arg_error << std::endl;
    return kExitArgError;
  }

  Resources resources{};
  bool passed = false;
  do {
    TirtcInitOptions init_options{};
    init_options.app_id = options.app_id.c_str();
    init_options.endpoint = options.endpoint.c_str();
    init_options.console_log_enabled = 0;
    if (!check_error(tirtc_init(&init_options), "tirtc_init failed", &stats)) {
      break;
    }
    resources.runtime_initialized = true;
    if (!create_runtime_objects(options, &stats, &resources)) {
      break;
    }
    if (!connect_and_wait(options, &stats, &resources)) {
      break;
    }
    passed = run_window_and_check(options, &stats, resources);
  } while (false);

  cleanup(&resources);
  {
    std::lock_guard<std::mutex> lock(stats.mutex);
    if (!passed && !stats.failed) {
      stats.failed = true;
      stats.error_code = kExitRuntimeFailed;
      stats.error_message = "runtime failed";
    }
    print_summary(stats, passed);
  }
  return passed ? 0 : kExitRuntimeFailed;
}
