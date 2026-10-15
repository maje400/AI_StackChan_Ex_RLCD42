/*----------------------------------------------------------------------------/
  Panel_ST7305 - LovyanGFX panel driver for ST7305 reflective LCD (RLCD)
  See Panel_ST7305.hpp for details.
/----------------------------------------------------------------------------*/
#if defined(ARDUINO_WS_RLCD42)

#include "Panel_ST7305.hpp"

#include <lgfx/v1/Bus.hpp>
#include <lgfx/v1/platforms/common.hpp>
#include <lgfx/v1/misc/pixelcopy.hpp>
#include <lgfx/v1/misc/colortype.hpp>

#ifdef min
#undef min
#endif

namespace lgfx
{
 inline namespace v1
 {
//----------------------------------------------------------------------------

  // 4x4 Bayer matrix for ordered dithering (same values as Panel_GDEW0154M09)
  static constexpr uint8_t Bayer[16] = { 8, 200, 40, 232, 72, 136, 104, 168, 56, 248, 24, 216, 120, 184, 88, 152 };

  // ST7305 (Waveshare 4.2" 400x300) address window
  static constexpr uint8_t COL_START = 0x12;
  static constexpr uint8_t COL_END   = 0x2A;
  static constexpr uint8_t ROW_START = 0x00;
  static constexpr uint8_t ROW_END   = 0xC7;

  Panel_ST7305::Panel_ST7305(void)
  {
    _cfg.panel_width  = _cfg.memory_width  = 400;
    _cfg.panel_height = _cfg.memory_height = 300;
    _cfg.dummy_read_bits = 0;
    _auto_display = true;
  }

  color_depth_t Panel_ST7305::setColorDepth(color_depth_t depth)
  {
    _write_depth = color_depth_t::rgb565_2Byte;
    _read_depth  = color_depth_t::rgb565_2Byte;
    return color_depth_t::rgb565_2Byte;
  }

  size_t Panel_ST7305::_get_buffer_length(void) const
  {
    // 400 x 300 / 8 = 15000 bytes (1bpp, 2x4 block layout)
    return (size_t)_cfg.panel_width * _cfg.panel_height >> 3;
  }

  bool Panel_ST7305::init(bool use_reset)
  {
    if (!Panel_HasBuffer::init(use_reset))
    {
      return false;
    }

    // buffer white (bit set = white)
    memset(_buf, 0xFF, _get_buffer_length());

    startWrite(true);

    // --- init sequence (Waveshare reference driver) ---
    _bus->writeCommand(0xD6, 8);  // NVM load control
    _bus->writeData(0x17, 8); _bus->writeData(0x02, 8);
    _bus->writeCommand(0xD1, 8);  // Booster enable
    _bus->writeData(0x01, 8);
    _bus->writeCommand(0xC0, 8);  // Gate voltage
    _bus->writeData(0x11, 8); _bus->writeData(0x04, 8);
    _bus->writeCommand(0xC1, 8);  // VSHP
    for (int i = 0; i < 4; ++i) _bus->writeData(0x69, 8);
    _bus->writeCommand(0xC2, 8);  // VSLP
    for (int i = 0; i < 4; ++i) _bus->writeData(0x19, 8);
    _bus->writeCommand(0xC4, 8);  // VSHN
    for (int i = 0; i < 4; ++i) _bus->writeData(0x4B, 8);
    _bus->writeCommand(0xC5, 8);  // VSLN
    for (int i = 0; i < 4; ++i) _bus->writeData(0x19, 8);
    _bus->writeCommand(0xD8, 8);  // OSC
    _bus->writeData(0x80, 8); _bus->writeData(0xE9, 8);
    _bus->writeCommand(0xB2, 8);  // Frame rate
    _bus->writeData(0x02, 8);
    {
      _bus->writeCommand(0xB3, 8);  // Gate EQ (high power)
      static constexpr uint8_t b3[] = {0xE5,0xF6,0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
      for (auto v : b3) _bus->writeData(v, 8);
      _bus->writeCommand(0xB4, 8);  // Gate EQ (low power)
      static constexpr uint8_t b4[] = {0x05,0x46,0x77,0x77,0x77,0x77,0x76,0x45};
      for (auto v : b4) _bus->writeData(v, 8);
    }
    _bus->writeCommand(0x62, 8);  // Gate timing
    _bus->writeData(0x32, 8); _bus->writeData(0x03, 8); _bus->writeData(0x1F, 8);
    _bus->writeCommand(0xB7, 8);  // Source EQ enable
    _bus->writeData(0x13, 8);
    _bus->writeCommand(0xB0, 8);  // Gate line setting: 100 * 3 = 300 lines
    _bus->writeData(0x64, 8);
    _bus->writeCommand(0x11, 8);  // Sleep out
    _bus->wait();
    delay(200);
    _bus->writeCommand(0xC9, 8);  // Source voltage select
    _bus->writeData(0x00, 8);
    _bus->writeCommand(0x36, 8);  // MADCTL: MX=1, DO=1
    _bus->writeData(0x48, 8);
    _bus->writeCommand(0x3A, 8);  // Data format: 1bit monochrome
    _bus->writeData(0x11, 8);
    _bus->writeCommand(0xB9, 8);  // Gamma mode: mono
    _bus->writeData(0x20, 8);
    _bus->writeCommand(0xB8, 8);  // Panel setting
    _bus->writeData(0x29, 8);
    _bus->writeCommand(0x21, 8);  // Display inversion on
    _bus->writeCommand(0x2A, 8);  // Column address
    _bus->writeData(COL_START, 8); _bus->writeData(COL_END, 8);
    _bus->writeCommand(0x2B, 8);  // Row address
    _bus->writeData(ROW_START, 8); _bus->writeData(ROW_END, 8);
    _bus->writeCommand(0x35, 8);  // Tearing effect line on
    _bus->writeData(0x00, 8);
    _bus->writeCommand(0xD0, 8);  // Auto power down
    _bus->writeData(0xFF, 8);
    _bus->writeCommand(0x38, 8);  // High power mode
    _bus->writeCommand(0x29, 8);  // Display on
    _bus->wait();

    setInvert(_invert);
    setRotation(_rotation);

    // push initial (white) frame
    _range_mod.top = 0;
    _range_mod.left = 0;
    _range_mod.right = _cfg.panel_width - 1;
    _range_mod.bottom = _cfg.panel_height - 1;
    _send_frame();
    _range_mod.top    = INT16_MAX;
    _range_mod.left   = INT16_MAX;
    _range_mod.right  = 0;
    _range_mod.bottom = 0;

    endWrite();

    return true;
  }

  // 注意: displayはendWrite内の自動display機構からトランザクションが開いた状態で
  // 呼ばれる。ここでstartWrite/endWriteを呼ぶとendWrite→display→endWrite→…の
  // 無限再帰になる(スタックオーバーフローでパニック)ため、バス操作は
  // beginTransaction/endTransactionを直接使い、dirty rectは送信前にクリアする。
  void Panel_ST7305::display(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h)
  {
    if (0 < w && 0 < h)
    {
      _range_mod.left   = std::min<int16_t>(_range_mod.left  , x        );
      _range_mod.right  = std::max<int16_t>(_range_mod.right , x + w - 1);
      _range_mod.top    = std::min<int16_t>(_range_mod.top   , y        );
      _range_mod.bottom = std::max<int16_t>(_range_mod.bottom, y + h - 1);
    }
    if (_range_mod.empty()) { return; }

    _range_mod.top    = INT16_MAX;
    _range_mod.left   = INT16_MAX;
    _range_mod.right  = 0;
    _range_mod.bottom = 0;

    bool tr = _in_transaction;
    if (!tr) { beginTransaction(); }
    _send_frame();
    if (!tr) { endTransaction(); }
  }

  // Always sends the full frame: the ST7305 address window granularity does
  // not map 1:1 to pixels, so partial updates are not worth the complexity.
  // 15000 bytes @ 20MHz SPI is < 7 msec.
  void Panel_ST7305::_send_frame(void)
  {
    _bus->writeCommand(0x2A, 8);
    _bus->writeData(COL_START, 8); _bus->writeData(COL_END, 8);
    _bus->writeCommand(0x2B, 8);
    _bus->writeData(ROW_START, 8); _bus->writeData(ROW_END, 8);
    _bus->writeCommand(0x2C, 8);
    _bus->writeBytes(_buf, _get_buffer_length(), true, true);
    _bus->wait();
  }

  void Panel_ST7305::setInvert(bool invert)
  {
    _invert = invert;
    startWrite();
    // Reference driver uses INVON (0x21) as the normal state.
    _bus->writeCommand(((invert ^ _cfg.invert) ? 0x20 : 0x21), 8);
    endWrite();
  }

  void Panel_ST7305::setSleep(bool flg)
  {
    startWrite();
    if (flg)
    {
      _bus->writeCommand(0x28, 8);  // Display off
      _bus->writeCommand(0x10, 8);  // Sleep in
    }
    else
    {
      _bus->writeCommand(0x11, 8);  // Sleep out
      _bus->wait();
      delay(120);
      _bus->writeCommand(0x38, 8);  // High power mode
      _bus->writeCommand(0x29, 8);  // Display on
    }
    endWrite();
  }

  void Panel_ST7305::setPowerSave(bool flg)
  {
    startWrite();
    // Low power mode (~1Hz refresh) <-> high power mode (~32Hz refresh)
    _bus->writeCommand(flg ? 0x39 : 0x38, 8);
    endWrite();
  }

  void Panel_ST7305::writeFillRectPreclipped(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, uint32_t rawcolor)
  {
    uint_fast16_t xs = x, xe = x + w - 1;
    uint_fast16_t ys = y, ye = y + h - 1;
    _xs = xs;
    _ys = ys;
    _xe = xe;
    _ye = ye;
    {
      // dirty rect更新用に回転済み座標のコピーを渡す
      // (ループ側は未回転座標を使う。_draw_pixel内で回転されるため)
      uint_fast16_t rxs = xs, rys = ys, rxe = xe, rye = ye;
      _update_dirty_rect(rxs, rys, rxe, rye);
    }

    swap565_t color;
    color.raw = rawcolor;
    uint32_t value = (color.R8() + (color.G8() << 1) + color.B8()) >> 2;

    y = ys;
    do
    {
      x = xs;
      do
      {
        _draw_pixel(x, y, value);
      } while (++x <= xe);
    } while (++y <= ye);
  }

  void Panel_ST7305::writeImage(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, pixelcopy_t* param, bool use_dma)
  {
    uint_fast16_t xs = x, xe = x + w - 1;
    uint_fast16_t ys = y, ye = y + h - 1;
    _update_dirty_rect(xs, ys, xe, ye);

    auto readbuf = (swap565_t*)alloca(w * sizeof(swap565_t));
    auto sx = param->src_x32;
    h += y;
    do
    {
      uint32_t prev_pos = 0, new_pos = 0;
      do
      {
        new_pos = param->fp_copy(readbuf, prev_pos, w, param);
        if (new_pos != prev_pos)
        {
          do
          {
            auto color = readbuf[prev_pos];
            _draw_pixel(x + prev_pos, y, (color.R8() + (color.G8() << 1) + color.B8()) >> 2);
          } while (new_pos != ++prev_pos);
        }
      } while (w != new_pos && w != (prev_pos = param->fp_skip(new_pos, w, param)));
      param->src_x32 = sx;
      param->src_y++;
    } while (++y < h);
  }

  void Panel_ST7305::writePixels(pixelcopy_t* param, uint32_t length, bool use_dma)
  {
    {
      uint_fast16_t xs = _xs;
      uint_fast16_t xe = _xe;
      uint_fast16_t ys = _ys;
      uint_fast16_t ye = _ye;
      _update_dirty_rect(xs, ys, xe, ye);
    }
    uint_fast16_t xs   = _xs  ;
    uint_fast16_t ys   = _ys  ;
    uint_fast16_t xe   = _xe  ;
    uint_fast16_t ye   = _ye  ;
    uint_fast16_t xpos = _xpos;
    uint_fast16_t ypos = _ypos;

    static constexpr uint32_t buflen = 16;
    swap565_t colors[buflen];
    int bufpos = buflen;
    do
    {
      if (bufpos == buflen) {
        param->fp_copy(colors, 0, std::min(length, buflen), param);
        bufpos = 0;
      }
      auto color = colors[bufpos++];
      _draw_pixel(xpos, ypos, (color.R8() + (color.G8() << 1) + color.B8()) >> 2);
      if (++xpos > xe)
      {
        xpos = xs;
        if (++ypos > ye)
        {
          ypos = ys;
        }
      }
    } while (--length);
    _xpos = xpos;
    _ypos = ypos;
  }

  void Panel_ST7305::readRect(uint_fast16_t x, uint_fast16_t y, uint_fast16_t w, uint_fast16_t h, void* dst, pixelcopy_t* param)
  {
    auto readbuf = (swap565_t*)alloca(w * sizeof(swap565_t));
    param->src_data = readbuf;
    int32_t readpos = 0;
    h += y;
    do
    {
      uint32_t idx = 0;
      do
      {
        readbuf[idx] = _read_pixel(x + idx, y) ? -1 : 0;
      } while (++idx != w);
      param->src_x32 = 0;
      readpos = param->fp_copy(dst, readpos, readpos + w, param);
    } while (++y < h);
  }

  // ST7305 landscape (400x300) memory layout: 2x4 pixel blocks.
  // Each byte holds 8 pixels (2 columns x 4 rows), column-major block order
  // with vertical inversion. See kylehase/ESPHome-ST7305-RLCD.
  static inline uint32_t st7305_index(uint_fast16_t x, uint_fast16_t y, uint_fast8_t &bit)
  {
    uint_fast16_t inv_y   = 299 - y;
    uint_fast16_t block_y = inv_y >> 2;         // 0..74
    uint_fast8_t  local_y = inv_y & 3;
    uint_fast16_t byte_x  = x >> 1;             // 0..199
    uint_fast8_t  local_x = x & 1;
    bit = 7 - ((local_y << 1) | local_x);
    return (uint32_t)byte_x * 75 + block_y;     // 75 = 300 / 4
  }

  void Panel_ST7305::_draw_pixel(uint_fast16_t x, uint_fast16_t y, uint32_t value)
  {
    _rotate_pos(x, y);
    uint_fast8_t bit;
    uint32_t idx = st7305_index(x, y, bit);
    bool flg = 256 <= value + Bayer[(x & 3) | (y & 3) << 2];
    if (flg) _buf[idx] |=   1 << bit;   // white
    else     _buf[idx] &= ~(1 << bit);  // black
  }

  bool Panel_ST7305::_read_pixel(uint_fast16_t x, uint_fast16_t y)
  {
    _rotate_pos(x, y);
    uint_fast8_t bit;
    uint32_t idx = st7305_index(x, y, bit);
    return _buf[idx] & (1 << bit);
  }

  void Panel_ST7305::_update_dirty_rect(uint_fast16_t &xs, uint_fast16_t &ys, uint_fast16_t &xe, uint_fast16_t &ye)
  {
    _rotate_pos(xs, ys, xe, ye);
    _range_mod.top    = std::min<int32_t>(ys, _range_mod.top);
    _range_mod.left   = std::min<int32_t>(xs, _range_mod.left);
    _range_mod.right  = std::max<int32_t>(xe, _range_mod.right);
    _range_mod.bottom = std::max<int32_t>(ye, _range_mod.bottom);
  }

//----------------------------------------------------------------------------
 }
}

#endif // ARDUINO_WS_RLCD42
