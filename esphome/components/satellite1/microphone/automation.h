#pragma once

#include "esphome/core/automation.h"
#include "sat1_microphone.h"

#include <vector>

namespace esphome {
namespace i2s_audio {

class PCMDataTrigger : public Trigger<const std::vector<uint8_t> &> {
 public:
  explicit PCMDataTrigger(Microphone *mic) {
    mic->add_pcm_data_callback([this](const std::vector<uint8_t> &data) { this->trigger(data); });
  }
};

}  // namespace microphone
}  // namespace esphome