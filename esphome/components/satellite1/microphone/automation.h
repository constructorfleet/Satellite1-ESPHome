#pragma once

#include "esphome/core/automation.h"
#include "sat1_microphone.h"

#include <vector>

namespace esphome {
namespace i2s_audio {

class PCMDataTrigger : public Trigger<const std::vector<int32_t> &> {
 public:
  explicit PCMDataTrigger(Sat1Microphone *mic) {
    mic->add_pcm_data_callback([this](const std::vector<int32_t> &data) { this->trigger(data); });
  }
};

}  // namespace microphone
}  // namespace esphome