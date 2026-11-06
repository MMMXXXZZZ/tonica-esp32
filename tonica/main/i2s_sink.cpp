#include "i2s_sink.h"
#include "driver/i2s_pdm.h"
#include <cstring>
#include "esp_log.h"
#include "listenica/platform.h"

static const char* TAG = "i2s";
static constexpr int kDmaDesc = 8;
static constexpr int kDmaFrames = 511;

I2sSink::I2sSink(int bck, int ws, int dout, int mclk) {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = kDmaDesc;
  cc.dma_frame_num = kDmaFrames;
  cc.auto_clear = true;  // output silence when starved/paused
  ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx_, nullptr));
  i2s_std_config_t sc = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .mclk = mclk < 0 ? I2S_GPIO_UNUSED : (gpio_num_t)mclk,
              .bclk = (gpio_num_t)bck,
              .ws = (gpio_num_t)ws,
              .dout = (gpio_num_t)dout,
              .din = I2S_GPIO_UNUSED,
              .invert_flags = {.mclk_inv = false, .bclk_inv = false, .ws_inv = false},
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_, &sc));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_));
  enabled_ = true;
  ESP_LOGI(TAG, "I2S up: BCK=%d WS=%d DOUT=%d MCLK=%d, 44.1 kHz s16 stereo", bck, ws, dout, mclk);
}

I2sSink::I2sSink(Pdm, int left, int right, int clk) {
  pdm_ = true;
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  cc.dma_desc_num = kDmaDesc;
  cc.dma_frame_num = kDmaFrames;
  cc.auto_clear = true;
  ESP_ERROR_CHECK(i2s_new_channel(&cc, &tx_, nullptr));
  i2s_pdm_tx_config_t pc = {
      .clk_cfg = I2S_PDM_TX_CLK_DAC_DEFAULT_CONFIG(44100),
      .slot_cfg = I2S_PDM_TX_SLOT_DAC_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
      .gpio_cfg =
          {
              .clk = (gpio_num_t)clk,
              .dout = (gpio_num_t)left,
              .dout2 = (gpio_num_t)right,
              .invert_flags = {.clk_inv = false},
          },
  };
  ESP_ERROR_CHECK(i2s_channel_init_pdm_tx_mode(tx_, &pc));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_));
  enabled_ = true;
  ESP_LOGI(TAG, "PDM DAC up: L=GPIO%d R=GPIO%d (clk GPIO%d), 44.1 kHz; add RC low-pass per channel", left, right, clk);
}

void I2sSink::write(const int16_t* frames, size_t n) {
  size_t bytes = n * 4, done = 0;
  while (done < bytes) {
    size_t w = 0;
    esp_err_t e = i2s_channel_write(tx_, (const uint8_t*)frames + done, bytes - done, &w, 1000);
    if (e != ESP_OK && e != ESP_ERR_TIMEOUT) {
      ESP_LOGW(TAG, "i2s write: %s", esp_err_to_name(e));
      break;
    }
    done += w;
  }
  frames_ += n;
}

void I2sSink::flush() {
  // Dropping DMA content: disable + re-enable clears the queued buffers.
  if (enabled_) {
    i2s_channel_disable(tx_);
    i2s_channel_enable(tx_);
  }
}

void I2sSink::set_paused(bool) {}

size_t I2sSink::latency_frames() { return size_t(kDmaDesc) * kDmaFrames; }
