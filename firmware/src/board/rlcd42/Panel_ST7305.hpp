/*----------------------------------------------------------------------------/
  Panel_ST7305 - LovyanGFX panel driver for ST7305 reflective LCD (RLCD)

  Target: Waveshare ESP32-S3-RLCD-4.2 (400x300 monochrome, no backlight)

  Based on:
   - LovyanGFX Panel_GDEW0154M09 (FreeBSD license) as buffered-panel template
   - Waveshare reference driver / kylehase ESPHome-ST7305-RLCD for the
     init sequence and the 2x4 pixel-block memory layout
/----------------------------------------------------------------------------*/
#pragma once

#include <lgfx/v1/panel/Panel_HasBuffer.hpp>
#include <lgfx/v1/misc/range.hpp>

namespace lgfx
{
 inline namespace v1
 {

  struct Panel_ST7305 : public Panel_HasBuffer
  {
    Panel_ST7305(void);

    bool init(bool use_reset) override;

    color_depth_t setColorDepth(color_depth_t depth) override;

    void setInvert(bool invert) override;
    void setSleep(bool flg) override;
    void setPowerSave(bool flg) override;

    void waitDisplay(void) override {}
    bool displayBusy(void) override { return false; }
    void display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h) override;

    void writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor) override;
    void writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma) override;
    void writePixels(pixelcopy_t* param, uint32_t len, bool use_dma) override;

    uint32_t readCommand(uint_fast16_t, uint_fast8_t, uint_fast8_t) override { return 0; }
    uint32_t readData(uint_fast8_t, uint_fast8_t) override { return 0; }

    void readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, pixelcopy_t* param) override;

  private:
    size_t _get_buffer_length(void) const override;
    void _draw_pixel(uint_fast16_t x, uint_fast16_t y, uint32_t value);
    bool _read_pixel(uint_fast16_t x, uint_fast16_t y);
    void _update_dirty_rect(uint_fast16_t &xs, uint_fast16_t &ys, uint_fast16_t &xe, uint_fast16_t &ye);
    void _send_frame(void);
  };

 }
}
