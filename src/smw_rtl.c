#include "smw_rtl.h"
#include "variables.h"
#include <time.h>
#include "common_cpu_infra.h"
#include "snes/snes.h"
#include "src/funcs.h"

const uint8 *ptr_layer1_data;
const uint8 *ptr_layer2_data;
uint8 ptr_layer2_is_bg;

uint8 *ptr_lo_map16_data;
uint8 *ptr_lo_map16_data_bak;

bool g_lunar_magic;


void AddSprXPos(uint8 k, uint16 x) {
  AddHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
}

void AddSprYPos(uint8 k, uint16 y) {
  AddHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

void AddSprXYPos(uint8 k, uint16 x, uint16 y) {
  AddHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
  AddHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

uint16 GetSprXPos(uint8 k) {
  return PAIR16(spr_xpos_hi[k], spr_xpos_lo[k]);
}

uint16 GetSprYPos(uint8 k) {
  return PAIR16(spr_ypos_hi[k], spr_ypos_lo[k]);
}

void SetSprXPos(uint8 k, uint16 x) {
  spr_xpos_hi[k] = x >> 8;
  spr_xpos_lo[k] = x;
}

void SetSprYPos(uint8 k, uint16 y) {
  spr_ypos_hi[k] = y >> 8;
  spr_ypos_lo[k] = y;
}

void SetSprXYPos(uint8 k, uint16 x, uint16 y) {
  SetHiLo(&spr_xpos_hi[k], &spr_xpos_lo[k], x);
  SetHiLo(&spr_ypos_hi[k], &spr_ypos_lo[k], y);
}

void SmwSavePlaythroughSnapshot() {
  char buf[128];
  snprintf(buf, sizeof(buf), "playthrough/%d_%d_%d.sav", ow_level_number_lo, misc_exit_level_action, (int)time(NULL));
  RtlSaveSnapshot(buf, false);
}

void UploadOAMBuffer() {  // 008449
  memcpy(g_ppu->oam, g_ram + 0x200, 0x220);
  RtlPpuWrite(OAMADDH, 0x80);
  RtlPpuWrite(OAMADDL, mirror_oamaddress_lo);
}

static void ConfigurePpuSideSpace() {
  // Let PPU impl know about the maximum allowed extra space on the sides and bottom
  int extra_right = 0, extra_left = 0, extra_bottom = 0;
//  printf("main %d, sub %d  (%d, %d, %d)\n", main_module_index, submodule_index, BG2HOFS_copy2, room_bounds_x.v[2 | (quadrant_fullsize_x >> 1)], quadrant_fullsize_x >> 1);
  int mod = misc_game_mode;
  if (mod >= 19 && mod <= 21) {
    // In-level
    // mirror_current_layer1_xpos is the current screen scroll
    // camera_last_screen_horiz is the number of screens in the current level
    extra_left = IntMax(mirror_current_layer1_xpos, 0);
    extra_right = IntMax((camera_last_screen_horiz-1) * 256 - mirror_current_layer1_xpos, 0);
    extra_bottom = 16;
    //printf("mirror_current_layer1_xpos=%d left=%d right=%d\n", mirror_current_layer1_xpos, extra_left, extra_right);
  }
  PpuSetExtraSideSpace(g_ppu, extra_left, extra_right, extra_bottom);
}


void SmwDrawPpuFrame() {
  SimpleHdma hdma_chans[3];

  Dma *dma = g_dma;

  dma_startDma(dma, mirror_hdmaenable, true);

  SimpleHdma_Init(&hdma_chans[0], &dma->channel[5]);
  SimpleHdma_Init(&hdma_chans[1], &dma->channel[6]);
  SimpleHdma_Init(&hdma_chans[2], &dma->channel[7]);

  int trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;

  uint32 render_flags = 0xd;  // FIXME from main

  if (g_ppu->extraLeftRight != 0 || render_flags & kPpuRenderFlags_Height240)
    ConfigurePpuSideSpace();

  int height = render_flags & kPpuRenderFlags_Height240 ? 240 : 224;

  for (int i = 0; i <= height; i++) {
    ppu_runLine(g_ppu, i);
    SimpleHdma_DoLine(&hdma_chans[0]);
    SimpleHdma_DoLine(&hdma_chans[1]);
    SimpleHdma_DoLine(&hdma_chans[2]);
    //    dma_doHdma(snes->dma);
    if (i == trigger) {
      SmwVectorIRQ();
      trigger = g_snes->vIrqEnabled ? g_snes->vTimer + 1 : -1;
    }
  }
}

void SmwDrawPpuDebuggerFrame(uint8 *pixel_buffer, size_t pitch, uint32 render_flags) {
  // TODO Render TileMaps, Backgrounds, Window, Tiles, Sprites, Palettes, ...

  if (g_ppu->extraLeftRight != 0 || render_flags & kPpuRenderFlags_Height240)
    ConfigurePpuSideSpace();

  ppu_renderDebugger(g_ppu, pitch, pixel_buffer, 0, 0);
  ppu_renderDebugger(g_ppu, pitch, pixel_buffer, 1, 64*8*4);
  ppu_renderDebugger(g_ppu, pitch, pixel_buffer, 2, 64*8*pitch);
  ppu_renderDebugger(g_ppu, pitch, pixel_buffer, -1, 64*8*pitch + 64*8*4);
}

void SmwRunOneFrameOfGame(void) {
  if (*(uint16 *)reset_sprites_y_function_in_ram == 0)
    SmwVectorReset();
  SmwRunOneFrameOfGame_Internal();
  SmwVectorNMI();
}


void LoadStripeImage_UploadToVRAM(const uint8 *pp) {  // 00871e
  while (1) {
    if ((*pp & 0x80) != 0)
      break;
    uint16 vram_addr = pp[0] << 8 | pp[1];
    if (g_lunar_magic)
      vram_addr = LmHook_LoadStripeImage(vram_addr);

    uint8 vmain = __CFSHL__(pp[2], 1);
    uint8 fixed_addr = (uint8)(pp[2] & 0x40) >> 3;
    uint16 num = (swap16(WORD(pp[2])) & 0x3FFF) + 1;
    pp += 4;

    if (fixed_addr) {
      if (vram_addr != 0xffff) {
        uint16 *dst = g_ppu->vram + vram_addr;
        uint16 src_data = WORD(*pp);
        int ctr = (num + 1) >> 1;
        if (vmain) {
          for (int i = 0; i < ctr; i++)
            dst[i * 32] = src_data;
        } else {
          // uhm...?
          uint8 *dst_b = (uint8 *)dst;
          for (int i = 0; i < num; i++)
            dst_b[i + ((i & 1) << 1)] = src_data;
          for (int i = 0; i < num; i += 2)
            dst_b[i + 1] = src_data >> 8;
        }
      }
      pp += 2;
    } else {
      if (vram_addr != 0xffff) {
        uint16 *dst = g_ppu->vram + vram_addr;
        uint16 *src = (uint16 *)pp;
        if (vmain) {
          for (int i = 0; i < (num >> 1); i++)
            dst[i * 32] = src[i];
        } else {
          for (int i = 0; i < (num >> 1); i++)
            dst[i] = src[i];
        }
      }
      pp += num;
    }
  }
}

