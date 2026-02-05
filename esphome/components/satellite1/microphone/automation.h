#pragma once

#include "esphome/core/automation.h"
#include "sat1_microphone.h"

#include <vector>

namespace esphome {
namespace i2s_audio {

class PCMDataTrigger : public Trigger<const int32_t*, size_t> {
 public:
  explicit PCMDataTrigger(Sat1Microphone *mic) {
    mic->add_pcm_data_callback(
      [this](const int32_t* data, size_t count) {
        this->trigger(data, count);
      }
    );
  }
};

}  // namespace microphone
}  // namespace esphome