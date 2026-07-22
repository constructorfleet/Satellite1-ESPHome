#include "wake_audio_stream.h"

#ifdef USE_ESP32

#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include <cerrno>
#include <cstring>

namespace esphome {
namespace wake_audio_stream {

static const char *const TAG = "wake_audio_stream";

// microWakeWord feeds mono int16 PCM at 16 kHz.
static const size_t BYTES_PER_SECOND = 16000 * sizeof(int16_t);

// Max UDP payload we send per datagram. Kept below a typical MTU to avoid IP fragmentation. Must be a
// multiple of sizeof(int16_t) so we never split a sample across datagrams.
static const size_t SEND_CHUNK_SIZE = 1024;
static const uint8_t PACKET_MAGIC[] = {'W', 'W', 'D', '2'};
static const size_t FIXED_HEADER_SIZE = 18;
static const uint8_t AUDIO_CHANNELS = 1;
static const uint8_t BITS_PER_SAMPLE = 16;
static const uint8_t AUDIO_ENCODING_PCM_SIGNED_LE = 1;
static const uint32_t AUDIO_SAMPLE_RATE = 16000;
static const size_t MAX_ASSISTANT_ID_BYTES = 64;

static void write_u16_be(uint8_t *buffer, uint16_t value) {
  buffer[0] = static_cast<uint8_t>(value >> 8);
  buffer[1] = static_cast<uint8_t>(value);
}

static void write_u32_be(uint8_t *buffer, uint32_t value) {
  buffer[0] = static_cast<uint8_t>(value >> 24);
  buffer[1] = static_cast<uint8_t>(value >> 16);
  buffer[2] = static_cast<uint8_t>(value >> 8);
  buffer[3] = static_cast<uint8_t>(value);
}

float WakeAudioStream::get_setup_priority() const { return setup_priority::AFTER_CONNECTION; }

void WakeAudioStream::setup() {
  this->assistant_id_ = App.get_name();
  if (this->assistant_id_.empty() || this->assistant_id_.size() > MAX_ASSISTANT_ID_BYTES) {
    ESP_LOGE(TAG, "Device name must contain 1 to %u bytes for UDP assistant identity",
             (unsigned) MAX_ASSISTANT_ID_BYTES);
    this->mark_failed();
    return;
  }

  size_t ring_bytes = BYTES_PER_SECOND * this->buffer_duration_ms_ / 1000;
  // Round up to a whole number of send chunks so draining is clean.
  ring_bytes = ((ring_bytes + SEND_CHUNK_SIZE - 1) / SEND_CHUNK_SIZE) * SEND_CHUNK_SIZE;
  this->ring_buffer_ = RingBuffer::create(ring_bytes);
  if (this->ring_buffer_ == nullptr) {
    ESP_LOGE(TAG, "Failed to allocate %u byte ring buffer", (unsigned) ring_bytes);
    this->mark_failed();
    return;
  }
  this->packet_header_size_ = FIXED_HEADER_SIZE + this->assistant_id_.size();
  this->packet_buffer_.resize(this->packet_header_size_ + SEND_CHUNK_SIZE);
  memcpy(this->packet_buffer_.data(), PACKET_MAGIC, sizeof(PACKET_MAGIC));
  this->packet_buffer_[sizeof(PACKET_MAGIC)] = static_cast<uint8_t>(this->assistant_id_.size());
  this->packet_buffer_[5] = AUDIO_CHANNELS;
  this->packet_buffer_[6] = BITS_PER_SAMPLE;
  this->packet_buffer_[7] = AUDIO_ENCODING_PCM_SIGNED_LE;
  write_u32_be(this->packet_buffer_.data() + 8, AUDIO_SAMPLE_RATE);
  memcpy(this->packet_buffer_.data() + FIXED_HEADER_SIZE, this->assistant_id_.data(), this->assistant_id_.size());
}

void WakeAudioStream::dump_config() {
  ESP_LOGCONFIG(TAG, "Wake Audio Stream:");
  ESP_LOGCONFIG(TAG, "  Destination: %u.%u.%u.%u:%u", this->remote_ip_[0], this->remote_ip_[1], this->remote_ip_[2],
                this->remote_ip_[3], this->remote_port_);
  ESP_LOGCONFIG(TAG, "  Buffer duration: %u ms", (unsigned) this->buffer_duration_ms_);
  ESP_LOGCONFIG(TAG, "  Assistant ID: %s", this->assistant_id_.c_str());
  ESP_LOGCONFIG(TAG, "  Format: %u Hz, %u-bit, %u channel(s), signed little-endian PCM", AUDIO_SAMPLE_RATE,
                BITS_PER_SAMPLE, AUDIO_CHANNELS);
  ESP_LOGCONFIG(TAG, "  Enabled on boot: %s", YESNO(this->enabled_));
}

bool WakeAudioStream::start_socket_() {
  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  memset(&this->dest_addr_, 0, sizeof(this->dest_addr_));

  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(this->remote_port_);
  // s_addr is stored in network byte order, i.e. the octets laid out as [a, b, c, d] in memory.
  memcpy(&server_addr.sin_addr.s_addr, this->remote_ip_, sizeof(this->remote_ip_));

  memcpy(&this->dest_addr_, &server_addr, sizeof(server_addr));

  this->socket_ = socket::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
  if (this->socket_ == nullptr) {
    ESP_LOGE(TAG, "Could not create socket");
    return false;
  }
  int err = this->socket_->setblocking(false);
  if (err != 0) {
    ESP_LOGE(TAG, "Could not set non-blocking mode: errno %d", errno);
    this->socket_ = nullptr;
    return false;
  }

  this->socket_running_ = true;
  return true;
}

void WakeAudioStream::stop_socket_() {
  if (this->socket_ != nullptr) {
    this->socket_->close();
    this->socket_ = nullptr;
  }
  this->socket_running_ = false;
}

void WakeAudioStream::set_enabled(bool enabled) {
  if (enabled == this->enabled_) {
    return;
  }
  this->enabled_ = enabled;
  if (enabled) {
    this->packet_sequence_ = 0;
    ESP_LOGI(TAG, "Wake-word audio capture enabled -> %u.%u.%u.%u:%u", this->remote_ip_[0], this->remote_ip_[1],
             this->remote_ip_[2], this->remote_ip_[3], this->remote_port_);
  } else {
    ESP_LOGI(TAG, "Wake-word audio capture disabled");
    this->stop_socket_();
    if (this->ring_buffer_ != nullptr) {
      this->ring_buffer_->reset();
    }
  }
}

void WakeAudioStream::push_audio(const std::vector<uint8_t> &data) {
  // Runs on the microphone task. Only buffer while enabled; the ring buffer is thread-safe and overwrites
  // the oldest data if the main loop cannot drain it fast enough.
  if (!this->enabled_ || this->ring_buffer_ == nullptr) {
    return;
  }
  this->ring_buffer_->write(data.data(), data.size());
}

void WakeAudioStream::loop() {
  if (!this->enabled_ || this->ring_buffer_ == nullptr) {
    return;
  }

  if (!this->socket_running_) {
    if (!this->start_socket_()) {
      // Back off; retry on a later loop.
      return;
    }
  }

  while (this->ring_buffer_->available() >= SEND_CHUNK_SIZE) {
    size_t read_bytes =
        this->ring_buffer_->read(this->packet_buffer_.data() + this->packet_header_size_, SEND_CHUNK_SIZE, 0);
    if (read_bytes == 0) {
      break;
    }
    write_u32_be(this->packet_buffer_.data() + 12, this->packet_sequence_++);
    write_u16_be(this->packet_buffer_.data() + 16, static_cast<uint16_t>(read_bytes));
    ssize_t sent = this->socket_->sendto(this->packet_buffer_.data(), this->packet_header_size_ + read_bytes, 0,
                                         (struct sockaddr *) &this->dest_addr_, sizeof(this->dest_addr_));
    if (sent < 0) {
      // ENOMEM/EAGAIN just means the stack is momentarily busy; drop this chunk and try again next loop.
      ESP_LOGV(TAG, "sendto failed: errno %d", errno);
      break;
    }
  }
}

}  // namespace wake_audio_stream
}  // namespace esphome

#endif  // USE_ESP32
