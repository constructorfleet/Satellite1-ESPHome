/*
 * This file is part of Snapcast integration for ESPHome.
 *
 * Copyright (C) 2025 Mischa Siekmann <FutureProofHomes Inc.>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>
#include <map>
#include <atomic>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>

#include "esp_transport.h"

#include "esphome/core/defines.h"
#include "esphome/components/json/json_util.h"

namespace esphome {
namespace snapcast {

enum class StreamStatus : uint8_t {
  UNKNOWN = 0,
  IDLE = 1,
  PLAYING = 2,
};

static inline StreamStatus parse_status(const char *s) {
  if (!s)
    return StreamStatus::UNKNOWN;
  if (s[0] == 'i')
    return StreamStatus::IDLE;
  if (s[0] == 'p')
    return StreamStatus::PLAYING;
  return StreamStatus::UNKNOWN;
}

struct StreamInfo {
  std::string id;
  StreamStatus status{StreamStatus::UNKNOWN};
  bool canControl{false};
  bool canPlay{false};
  bool canPause{false};
  bool canSeek{false};
  bool canGoNext{false};
  bool canGoPrevious{false};

  bool from_json(JsonObject stream_obj) {
    const char *sid = stream_obj["id"].as<const char *>();
    if (!sid)
      return false;
    id = sid;
    status = parse_status(stream_obj["status"].as<const char *>());

    if (stream_obj["properties"].is<JsonObject>()) {
      const JsonObject &props = stream_obj["properties"];
      canControl = props["canControl"].as<bool>();
      canPlay = props["canPlay"].as<bool>();
      canPause = props["canPause"].as<bool>();
      canSeek = props["canSeek"].as<bool>();
      canGoNext = props["canGoNext"].as<bool>();
      canGoPrevious = props["canGoPrevious"].as<bool>();
    }
    return true;
  }
};

struct ClientState {
  std::string group_id;
  std::string stream_id;
  std::string default_streamid{"default"};
  std::vector<std::string> group_members;
  std::map<std::string, StreamInfo> known_streams;
  int32_t latency = 0;
  uint8_t volume_percent = 100;
  bool muted = false;

  bool from_groups_json(JsonArray groups, std::string &client_id) {
    this->group_members.clear();
    for (JsonObject group_obj : groups) {
      JsonArray clients = group_obj["clients"].as<JsonArray>();
      for (JsonObject client_obj : clients) {
        const char *id = client_obj["id"].as<const char *>();
        if (!id)
          continue;
        if (id == client_id) {
          group_id = group_obj["id"].as<const char *>();
          stream_id = group_obj["stream_id"].as<const char *>();
          latency = client_obj["config"]["latency"].as<int32_t>();
          volume_percent = client_obj["config"]["volume"]["percent"].as<uint8_t>();
          muted = client_obj["config"]["volume"]["muted"].as<bool>();
          for (JsonObject member : clients) {
            const char *member_id = member["id"].as<const char *>();
            if (member_id) {
              group_members.emplace_back(member_id);
            }
          }
          return true;
        }
      }
    }
    return false;
  }
  void clear() {
    group_members.clear();
    known_streams.clear();
  }
};

using RpcResponseCb = std::function<void(JsonObject root)>;

struct RpcRequest {
  std::string method;
  uint32_t id;
  std::function<void(JsonObject params)> fill_params;
  RpcResponseCb on_response;  // optional
  uint32_t timeout_ms;
};

struct PendingCb {
  RpcResponseCb cb;
  uint32_t expires_ms;
};

class SnapcastClient;

class SnapcastControlSession {
 public:
  esp_err_t connect(const std::string &server, uint32_t port);
  esp_err_t disconnect();
  bool is_connected() const { return this->connected_; }

  void notification_loop();

  void set_on_stream_update(std::function<void(StreamStatus, std::string)> cb) {
    this->on_stream_update_ = std::move(cb);
  }

  void request_stop();
  bool send_rpc_async(const std::string &method, std::function<void(JsonObject)> fill_params, RpcResponseCb on_response,
                      uint32_t timeout_ms);

  ClientState state_snapshot() {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    ClientState copy = this->client_state_;
    xSemaphoreGive(state_mutex_);
    return copy;
  }

  std::string grp_id_snapshot() {
    xSemaphoreTake(state_mutex_, portMAX_DELAY);
    std::string copy = this->client_state_.group_id;
    xSemaphoreGive(state_mutex_);
    return copy;
  }

 protected:
  friend SnapcastClient;

  SemaphoreHandle_t config_mutex_{nullptr};
  std::string server_;
  uint32_t port_;
  std::string client_id_;
  bool connected_{false};

  TaskHandle_t task_handle_{nullptr};
  StackType_t *task_stack_buffer_{nullptr};
  StaticTask_t task_stack_;
  bool task_exiting_{false};
  bool task_should_run_{false};

  esp_transport_handle_t transport_{nullptr};
  std::string recv_buffer_;
  std::string line_buffer_;

  SemaphoreHandle_t state_mutex_{xSemaphoreCreateMutex()};
  ClientState client_state_;

  std::function<void(StreamStatus status, std::string stream_id)> on_stream_update_;

  QueueHandle_t rpc_queue_{nullptr};
  std::unordered_map<uint32_t, PendingCb> pending_;
  std::atomic<uint32_t> next_id_{1};

  void drain_rpc_queue_();
  void expire_pending_();

  void request_server_info_(std::function<void(const ClientState &)> cb = nullptr);
  void set_group_stream_(const std::string &stream_id);
  void isolate_client_(std::function<void(const ClientState &)> cb = nullptr);
  void group_request_stop(const ClientState &state);

  void update_from_server_obj_(const JsonObject &server_obj);
};

}  // namespace snapcast
}  // namespace esphome
