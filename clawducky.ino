/*
 * ╔══════════════════════════════════════════════════════════════╗
 *   CLAWD MOCHI — ESP32-C3 Super Mini + ST7789 1.54" 240×240
 *
 *   Wiring:
 *     SDA → GPIO 10  (hardware SPI MOSI)
 *     SCL → GPIO 8   (hardware SPI SCK)
 *     RST → GPIO 2
 *     DC  → GPIO 5   (XIAO D3 — GPIO1 isn't broken out on XIAO ESP32-C3)
 *     CS  → GPIO 4
 *     BL  → GPIO 3
 *     VCC → 3V3
 *     GND → GND
 *
 *   WiFi: joins home network (wifi_credentials.h) → http://clawd.local
 *         fallback hotspot "ClaWD-Mochi" pw: clawd1234 → 192.168.4.1
 * ╚══════════════════════════════════════════════════════════════╝
 */

#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <SPI.h>
#include <math.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Preferences.h>

// ── Home WiFi (station mode) ──────────────────────────────────
// Copy wifi_credentials.h.example → wifi_credentials.h and fill in.
// Missing file or empty SSID → boots straight into the hotspot.
#if __has_include("wifi_credentials.h")
  #include "wifi_credentials.h"
#endif
#ifndef WIFI_SSID
  #define WIFI_SSID ""
#endif
#ifndef WIFI_PASS
  #define WIFI_PASS ""
#endif

// ── Pins ──────────────────────────────────────────────────────
#define TFT_CS  4
#define TFT_DC  5   // XIAO ESP32-C3: GPIO1 not broken out; D3 = GPIO5
#define TFT_RST 2
#define TFT_BLK 3

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

// ── WiFi ──────────────────────────────────────────────────────
const char* AP_SSID = "ClaWD-Mochi";
const char* AP_PASS = "clawd1234";
WebServer server(80);
bool staConnected = false;

// ── Display ───────────────────────────────────────────────────
#define DISP_W 240
#define DISP_H 240

// ── Eye constants (shared by both eye views) ──────────────────
#define EYE_W   30
#define EYE_H   60
#define EYE_GAP 120
#define EYE_OX  0     // horizontal offset
#define EYE_OY  40    // vertical offset upward (subtracted from centre)

// ── Colours ───────────────────────────────────────────────────
uint16_t C_ORANGE, C_DARKBG, C_MUTED, C_GREEN, C_RED, C_AMBER;
#define C_WHITE ST77XX_WHITE
#define C_BLACK ST77XX_BLACK

// ── State ─────────────────────────────────────────────────────
#define VIEW_EYES_NORMAL 0
#define VIEW_EYES_SQUISH 1
#define VIEW_CODE        2
#define VIEW_DRAW        3
#define VIEW_EXPRESSION  4   // a static face from EXPRESSIONS[]; idle blink leaves it alone
#define VIEW_METER       5   // full-screen usage readout

uint8_t  currentView  = VIEW_EYES_NORMAL;
uint8_t  currentExpr  = 0;   // which EXPRESSIONS[] entry VIEW_EXPRESSION is holding
bool     busy         = false;
bool     backlightOn  = true;
uint8_t  animSpeed    = 1;   // 1=slow(default) 2=normal 3=fast

uint16_t animBgColor  = 0;   // background for eye/logo animations
uint16_t drawBgColor  = 0;   // background for canvas

// ── Claude Code status (driven by /claude?e=... from hooks) ───
#define CL_NONE    0
#define CL_WORKING 1
#define CL_WAITING 2
#define CL_DONE    3
#define CL_ERROR   4

uint8_t  claudeState   = CL_NONE;
uint8_t  claudeFrame   = 0;
uint32_t claudeFrameAt = 0;   // next animation frame due
uint32_t claudeExpires = 0;   // auto-return for transient states (0 = never)
uint32_t nextIdleBlink = 0;

// ── Expression types ──────────────────────────────────────────
// These live up here, above the first function, because the .ino preprocessor
// hoists auto-generated prototypes to the top of the file — a signature that
// mentions a type defined further down won't compile.

struct Expression {
  const char* id;
  uint8_t     shape;
  bool        brow;
  int8_t      tiltL;     // vertical delta of the INNER brow end, in px:
  int8_t      tiltR;     //   + drops it toward the nose (angry), - raises it
  int8_t      browHalf;  // half-width of the brow — match the eye to read as
                         //   one glyph (ಠ), overhang it to read as a brow
  int8_t      browGap;   // clearance above the eye's own top edge
  bool        pupil;     // draw a pupil inside the ring
  int8_t      pupilOX;   // pupil offset — this is what sells side-eye
  uint8_t     anim;      // flourish played on entry, and on idle where it loops
};

// Everything an animation is allowed to vary about a resting face. Keeping the
// deltas in one struct means a new motion is a new sequence of numbers rather
// than another draw function — and every face can use every motion.
struct FrameDelta {
  int16_t browDY;    // raises the brow
  int16_t pupilDX;   // slides the pupils
  int16_t eyeDX;     // shifts both eyes horizontally
  int16_t eyeDY;     // ...and vertically
  int16_t radiusD;   // grows or shrinks the eye
  bool    blink;
};
bool     uiStarted     = false;  // false while boot info screen is showing

// ── Terminal ──────────────────────────────────────────────────
#define TERM_COLS      15
#define TERM_ROWS       8
#define TERM_CHAR_W    12
#define TERM_CHAR_H    20
#define TERM_PAD_X      8
#define TERM_PAD_Y     18

bool    termMode    = false;
String  termLines[TERM_ROWS];
uint8_t termRow     = 0;
uint8_t termCol     = 0;

// ── Logo data ─────────────────────────────────────────────────
#define LOGO_CX 120
#define LOGO_CY 105

#define LOGO_TRI_COUNT 162
static const int16_t LOGO_TRIS[][6] PROGMEM = {
  {120,105,65,134,100,114},{120,105,100,114,101,113},{120,105,101,113,100,112},
  {120,105,100,112,99,112},{120,105,99,112,93,111},{120,105,93,111,73,111},
  {120,105,73,111,55,110},{120,105,55,110,38,109},{120,105,38,109,34,108},
  {120,105,34,108,30,103},{120,105,30,103,30,100},{120,105,30,100,34,98},
  {120,105,34,98,39,98},{120,105,39,98,50,99},{120,105,50,99,67,100},
  {120,105,67,100,80,101},{120,105,80,101,98,103},{120,105,98,103,101,103},
  {120,105,101,103,101,102},{120,105,101,102,100,101},{120,105,100,101,100,100},
  {120,105,100,100,82,88},{120,105,82,88,63,76},{120,105,63,76,53,69},
  {120,105,53,69,48,65},{120,105,48,65,45,61},{120,105,45,61,44,54},
  {120,105,44,54,49,49},{120,105,49,49,55,49},{120,105,55,49,57,49},
  {120,105,57,49,64,55},{120,105,64,55,78,66},{120,105,78,66,96,79},
  {120,105,96,79,99,81},{120,105,99,81,100,81},{120,105,100,81,100,80},
  {120,105,100,80,99,78},{120,105,99,78,89,60},{120,105,89,60,78,41},
  {120,105,78,41,73,34},{120,105,73,34,72,29},{120,105,72,29,72,28},
  {120,105,72,28,72,27},{120,105,72,27,71,26},{120,105,71,26,71,25},
  {120,105,71,25,71,24},{120,105,71,24,77,16},{120,105,77,16,80,15},
  {120,105,80,15,87,16},{120,105,87,16,91,19},{120,105,91,19,95,29},
  {120,105,95,29,103,46},{120,105,103,46,114,68},{120,105,114,68,118,75},
  {120,105,118,75,119,81},{120,105,119,81,120,83},{120,105,120,83,121,83},
  {120,105,121,83,121,82},{120,105,121,82,122,69},{120,105,122,69,124,54},
  {120,105,124,54,126,34},{120,105,126,34,126,28},{120,105,126,28,129,21},
  {120,105,129,21,135,18},{120,105,135,18,139,20},{120,105,139,20,143,25},
  {120,105,143,25,142,28},{120,105,142,28,140,42},{120,105,140,42,136,64},
  {120,105,136,64,133,78},{120,105,133,78,135,78},{120,105,135,78,136,76},
  {120,105,136,76,144,67},{120,105,144,67,156,51},{120,105,156,51,162,45},
  {120,105,162,45,168,38},{120,105,168,38,172,35},{120,105,172,35,180,35},
  {120,105,180,35,185,43},{120,105,185,43,183,52},{120,105,183,52,175,62},
  {120,105,175,62,168,71},{120,105,168,71,159,83},{120,105,159,83,153,94},
  {120,105,153,94,154,94},{120,105,154,94,155,94},{120,105,155,94,176,90},
  {120,105,176,90,188,88},{120,105,188,88,201,85},{120,105,201,85,208,88},
  {120,105,208,88,208,91},{120,105,208,91,206,97},{120,105,206,97,191,101},
  {120,105,191,101,174,104},{120,105,174,104,148,110},{120,105,148,110,148,111},
  {120,105,148,111,148,111},{120,105,148,111,160,112},{120,105,160,112,165,112},
  {120,105,165,112,177,112},{120,105,177,112,200,114},{120,105,200,114,205,118},
  {120,105,205,118,209,123},{120,105,209,123,208,126},{120,105,208,126,199,131},
  {120,105,199,131,187,128},{120,105,187,128,159,121},{120,105,159,121,149,119},
  {120,105,149,119,147,119},{120,105,147,119,147,120},{120,105,147,120,156,128},
  {120,105,156,128,170,141},{120,105,170,141,189,158},{120,105,189,158,190,163},
  {120,105,190,163,188,166},{120,105,188,166,185,166},{120,105,185,166,169,153},
  {120,105,169,153,162,148},{120,105,162,148,148,136},{120,105,148,136,147,136},
  {120,105,147,136,147,137},{120,105,147,137,150,142},{120,105,150,142,168,168},
  {120,105,168,168,169,176},{120,105,169,176,168,179},{120,105,168,179,163,180},
  {120,105,163,180,158,179},{120,105,158,179,148,165},{120,105,148,165,137,149},
  {120,105,137,149,129,134},{120,105,129,134,128,135},{120,105,128,135,123,189},
  {120,105,123,189,120,192},{120,105,120,192,115,194},{120,105,115,194,110,191},
  {120,105,110,191,108,185},{120,105,108,185,110,174},{120,105,110,174,113,160},
  {120,105,113,160,116,148},{120,105,116,148,118,134},{120,105,118,134,119,129},
  {120,105,119,129,119,129},{120,105,119,129,118,129},{120,105,118,129,107,144},
  {120,105,107,144,91,166},{120,105,91,166,78,180},{120,105,78,180,75,181},
  {120,105,75,181,70,178},{120,105,70,178,70,173},{120,105,70,173,73,169},
  {120,105,73,169,91,146},{120,105,91,146,102,132},{120,105,102,132,109,124},
  {120,105,109,124,109,123},{120,105,109,123,108,123},{120,105,108,123,61,153},
  {120,105,61,153,52,155},{120,105,52,155,49,151},{120,105,49,151,49,146},
  {120,105,49,146,51,144},{120,105,51,144,65,134},{120,105,65,134,65,134},
};

#define LOGO_SEG_COUNT 162
static const int16_t LOGO_SEGS[][4] PROGMEM = {
  {65,134,100,114},{100,114,101,113},{101,113,100,112},{100,112,99,112},
  {99,112,93,111},{93,111,73,111},{73,111,55,110},{55,110,38,109},
  {38,109,34,108},{34,108,30,103},{30,103,30,100},{30,100,34,98},
  {34,98,39,98},{39,98,50,99},{50,99,67,100},{67,100,80,101},
  {80,101,98,103},{98,103,101,103},{101,103,101,102},{101,102,100,101},
  {100,101,100,100},{100,100,82,88},{82,88,63,76},{63,76,53,69},
  {53,69,48,65},{48,65,45,61},{45,61,44,54},{44,54,49,49},
  {49,49,55,49},{55,49,57,49},{57,49,64,55},{64,55,78,66},
  {78,66,96,79},{96,79,99,81},{99,81,100,81},{100,81,100,80},
  {100,80,99,78},{99,78,89,60},{89,60,78,41},{78,41,73,34},
  {73,34,72,29},{72,29,72,28},{72,28,72,27},{72,27,71,26},
  {71,26,71,25},{71,25,71,24},{71,24,77,16},{77,16,80,15},
  {80,15,87,16},{87,16,91,19},{91,19,95,29},{95,29,103,46},
  {103,46,114,68},{114,68,118,75},{118,75,119,81},{119,81,120,83},
  {120,83,121,83},{121,83,121,82},{121,82,122,69},{122,69,124,54},
  {124,54,126,34},{126,34,126,28},{126,28,129,21},{129,21,135,18},
  {135,18,139,20},{139,20,143,25},{143,25,142,28},{142,28,140,42},
  {140,42,136,64},{136,64,133,78},{133,78,135,78},{135,78,136,76},
  {136,76,144,67},{144,67,156,51},{156,51,162,45},{162,45,168,38},
  {168,38,172,35},{172,35,180,35},{180,35,185,43},{185,43,183,52},
  {183,52,175,62},{175,62,168,71},{168,71,159,83},{159,83,153,94},
  {153,94,154,94},{154,94,155,94},{155,94,176,90},{176,90,188,88},
  {188,88,201,85},{201,85,208,88},{208,88,208,91},{208,91,206,97},
  {206,97,191,101},{191,101,174,104},{174,104,148,110},{148,110,148,111},
  {148,111,148,111},{148,111,160,112},{160,112,165,112},{165,112,177,112},
  {177,112,200,114},{200,114,205,118},{205,118,209,123},{209,123,208,126},
  {208,126,199,131},{199,131,187,128},{187,128,159,121},{159,121,149,119},
  {149,119,147,119},{147,119,147,120},{147,120,156,128},{156,128,170,141},
  {170,141,189,158},{189,158,190,163},{190,163,188,166},{188,166,185,166},
  {185,166,169,153},{169,153,162,148},{162,148,148,136},{148,136,147,136},
  {147,136,147,137},{147,137,150,142},{150,142,168,168},{168,168,169,176},
  {169,176,168,179},{168,179,163,180},{163,180,158,179},{158,179,148,165},
  {148,165,137,149},{137,149,129,134},{129,134,128,135},{128,135,123,189},
  {123,189,120,192},{120,192,115,194},{115,194,110,191},{110,191,108,185},
  {108,185,110,174},{110,174,113,160},{113,160,116,148},{116,148,118,134},
  {118,134,119,129},{119,129,119,129},{119,129,118,129},{118,129,107,144},
  {107,144,91,166},{91,166,78,180},{78,180,75,181},{75,181,70,178},
  {70,178,70,173},{70,173,73,169},{73,169,91,146},{91,146,102,132},
  {102,132,109,124},{109,124,109,123},{109,123,108,123},{108,123,61,153},
  {61,153,52,155},{52,155,49,151},{49,151,49,146},{49,146,51,144},
  {51,144,65,134},{65,134,65,134},
};

// ═════════════════════════════════════════════════════════════
//  HELPERS
// ═════════════════════════════════════════════════════════════

int speedMs(int ms) {
  if (animSpeed == 3) return ms / 2;
  if (animSpeed == 1) return ms * 2;
  return ms;
}

uint16_t hexToRgb565(String hex) {
  hex.replace("#", "");
  if (hex.length() != 6) return C_WHITE;
  long v = strtol(hex.c_str(), nullptr, 16);
  return tft.color565((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF);
}

void setBacklight(bool on) {
  backlightOn = on;
  digitalWrite(TFT_BLK, on ? HIGH : LOW);
}

void initColours() {
  // C_ORANGE = tft.color565(170, 72, 28);
  C_ORANGE = tft.color565(218, 17, 0);
  C_DARKBG = tft.color565(10,  12,  16);
  C_MUTED  = tft.color565(90,  88,  86);
  C_GREEN  = tft.color565(80, 220, 130);
  C_RED    = tft.color565(190, 40, 30);
  C_AMBER  = tft.color565(240, 170, 40);
  animBgColor = C_ORANGE;
  drawBgColor = C_ORANGE;
}

// ═════════════════════════════════════════════════════════════
//  LOGO
// ═════════════════════════════════════════════════════════════

void drawLogoFilled(uint16_t bg, uint16_t fg) {
  tft.fillScreen(bg);
  for (uint16_t i = 0; i < LOGO_TRI_COUNT; i++) {
    tft.fillTriangle(
      pgm_read_word(&LOGO_TRIS[i][0]), pgm_read_word(&LOGO_TRIS[i][1]),
      pgm_read_word(&LOGO_TRIS[i][2]), pgm_read_word(&LOGO_TRIS[i][3]),
      pgm_read_word(&LOGO_TRIS[i][4]), pgm_read_word(&LOGO_TRIS[i][5]),
      fg);
  }
  tft.setTextColor(fg); tft.setTextSize(2);
  tft.setCursor(LOGO_CX - 54, 210); tft.print("Anthropic");
  tft.setCursor(LOGO_CX - 53, 210); tft.print("Anthropic");
}

// ═════════════════════════════════════════════════════════════
//  VIEWS
// ═════════════════════════════════════════════════════════════

// Eye helpers — shared constants via #define EYE_*
inline int16_t eyeLX(int16_t ox) {
  return (DISP_W - (EYE_W * 2 + EYE_GAP)) / 2 + EYE_OX + ox;
}
inline int16_t eyeRX(int16_t ox) { return eyeLX(ox) + EYE_W + EYE_GAP; }
inline int16_t eyeY()            { return (DISP_H - EYE_H) / 2 - EYE_OY; }
inline int16_t eyeCY()           { return eyeY() + EYE_H / 2; }

void drawNormalEyes(int16_t ox = 0, bool blink = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(ox), rx = eyeRX(ox), ey = eyeY();
  if (!blink) {
    tft.fillRect(lx, ey, EYE_W, EYE_H, C_BLACK);
    tft.fillRect(rx, ey, EYE_W, EYE_H, C_BLACK);
  } else {
    tft.fillRect(lx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
    tft.fillRect(rx, ey + EYE_H / 2 - 3, EYE_W, 6, C_BLACK);
  }
  drawMeterOverlay();
}

void drawChevron(int16_t cx, int16_t cy, int16_t arm, int16_t reach,
                 uint8_t thk, bool rightFacing, uint16_t col) {
  for (int8_t t = -(int8_t)thk; t <= (int8_t)thk; t++) {
    if (rightFacing) {
      tft.drawLine(cx - reach/2, cy - arm + t, cx + reach/2, cy + t,      col);
      tft.drawLine(cx + reach/2, cy + t,       cx - reach/2, cy + arm + t, col);
    } else {
      tft.drawLine(cx + reach/2, cy - arm + t, cx - reach/2, cy + t,      col);
      tft.drawLine(cx - reach/2, cy + t,       cx + reach/2, cy + arm + t, col);
    }
  }
}

void drawSquishEyes(bool closed = false) {
  tft.fillScreen(animBgColor);
  const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
  const int16_t arm   = EYE_H / 2;
  const int16_t reach = EYE_W / 2;
  const int16_t lcx   = lx + EYE_W / 2;
  const int16_t rcx   = rx + EYE_W / 2;
  if (!closed) {
    drawChevron(lcx, cy, arm, reach, 10, true,  C_BLACK);
    drawChevron(rcx, cy, arm, reach, 10, false, C_BLACK);
  } else {
    tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);
    tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
  }
  drawMeterOverlay();
}

// ── Expressions ───────────────────────────────────────────────
// Every face here is "an eye shape plus an optional brow". Almost nothing
// that separates one expression from another lives in the eye — it lives in
// the brow angle. Keeping the brow as its own primitive means a new face is
// one row in EXPRESSIONS[], not another draw function.

#define ES_RECT   0   // tall filled bar — the default open eye
#define ES_RING   1   // hollow circle — the ಠ eye; the visible white is the point
#define ES_FLAT   2   // thin horizontal slit
#define ES_CROSS  3   // X
#define ES_ARC    4   // ^ chevron
#define ES_DISC   5   // filled disc

// Expression faces get their own geometry rather than reusing the EYE_*
// constants. Those describe a tall 30x60 eye sitting high on the panel, which
// suits the idle crab but leaves a round eye looking small and off-centre.
#define EX_CY    116  // vertical centre of the eye pair
#define EX_DX     52  // eye centre offset from screen centre
#define EX_R      34  // eye radius
#define EX_THK     9  // stroke weight for rings, brows and strokes

// Flourishes — the motion that carries a face's meaning.
#define AN_NONE   0
#define AN_POP    1   // snap wide with an overshoot: the double-take
#define AN_SHAKE  2   // damped horizontal rattle
#define AN_DROOP  3   // sag, catch itself, sag further: nodding off

const Expression EXPRESSIONS[] = {
  //  id             shape     brow   tiltL tiltR  bHalf bGap  pupil pOX  anim
  { "disapproval",   ES_RING,  true,      0,    0,    38,   7,  false,  0, AN_NONE  },  // ಠ_ಠ
  { "skeptical",     ES_RING,  true,    -15,    8,    38,   7,  true,   0, AN_NONE  },
  { "angry",         ES_RING,  true,     16,   16,    40,   4,  true,   0, AN_NONE  },
  { "sideeye",       ES_RING,  true,      0,    0,    38,   7,  true,  15, AN_NONE  },
  { "alert",         ES_RING,  false,     0,    0,     0,   0,  false,  0, AN_POP   },  // O_O
  { "happy",         ES_ARC,   false,     0,    0,     0,   0,  false,  0, AN_NONE  },  // ^_^
  { "sleepy",        ES_FLAT,  false,     0,    0,     0,   0,  false,  0, AN_DROOP },  // —_—
  { "dead",          ES_CROSS, false,     0,    0,     0,   0,  false,  0, AN_SHAKE },  // x_x
};
const uint8_t EXPRESSION_COUNT = sizeof(EXPRESSIONS) / sizeof(EXPRESSIONS[0]);

// How far the shape reaches above its centre — lets a brow sit tight against
// whatever it's drawn over instead of floating at one fixed height.
int16_t eyeTopExtent(uint8_t shape) {
  switch (shape) {
    case ES_RING:
    case ES_DISC:  return EX_R;
    case ES_FLAT:  return EX_THK / 2;
    case ES_CROSS: return EX_R;
    case ES_ARC:   return 22;
    default:       return EYE_H / 2;   // ES_RECT
  }
}

// One eye of the given shape, centred on (cx, cy) at radius r. Radius is a
// parameter rather than EX_R so a frame can scale the eye — that's the pop.
void drawEyeShape(uint8_t shape, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  if (r < EX_THK + 2) r = EX_THK + 2;
  switch (shape) {
    case ES_RING:
      // Punch the hole rather than stacking drawCircle() calls — Bresenham
      // leaves gaps in a thick ring built from concentric outlines.
      tft.fillCircle(cx, cy, r, col);
      tft.fillCircle(cx, cy, r - EX_THK, animBgColor);
      break;
    case ES_DISC:
      tft.fillCircle(cx, cy, r, col);
      break;
    case ES_FLAT:
      tft.fillRect(cx - r, cy - EX_THK / 2, r * 2, EX_THK, col);
      break;
    case ES_CROSS: {
      const int16_t a = r - 4;
      for (int8_t t = -(EX_THK / 2); t <= EX_THK / 2; t++) {
        tft.drawLine(cx - a, cy - a + t, cx + a, cy + a + t, col);
        tft.drawLine(cx + a, cy - a + t, cx - a, cy + a + t, col);
      }
      break;
    }
    case ES_ARC:
      for (int8_t t = -(EX_THK / 2); t <= EX_THK / 2; t++) {
        tft.drawLine(cx - r, cy + 20 + t, cx,     cy - 20 + t, col);
        tft.drawLine(cx,     cy - 20 + t, cx + r, cy + 20 + t, col);
      }
      break;
    default:  // ES_RECT
      tft.fillRect(cx - EYE_W / 2, cy - EYE_H / 2, EYE_W, EYE_H, col);
      break;
  }
}

// A brow above an eye centred on cx. `tilt` moves the INNER end only, so one
// value mirrors correctly across the face.
void drawBrow(int16_t cx, int16_t topY, int8_t tilt, int8_t half,
              bool isLeft, uint16_t col) {
  const int16_t inner = isLeft ? cx + half : cx - half;
  const int16_t outer = isLeft ? cx - half : cx + half;
  for (int8_t t = 0; t < EX_THK; t++)
    tft.drawLine(outer, topY + t, inner, topY + tilt + t, col);
}

FrameDelta restDelta(const Expression& e) {
  FrameDelta d = {0, 0, 0, 0, 0, false};
  d.pupilDX = e.pupil ? e.pupilOX : 0;
  return d;
}

// One frame of a face: the table's resting pose plus the deltas.
void drawExpressionFrame(uint8_t idx, const FrameDelta& d) {
  if (idx >= EXPRESSION_COUNT) return;
  const Expression& e = EXPRESSIONS[idx];
  tft.fillScreen(animBgColor);
  const int16_t cy  = EX_CY + d.eyeDY;
  const int16_t lcx = DISP_W / 2 - EX_DX + d.eyeDX;
  const int16_t rcx = DISP_W / 2 + EX_DX + d.eyeDX;
  const uint8_t shape = d.blink ? ES_FLAT : e.shape;
  const int16_t r     = EX_R + d.radiusD;

  drawEyeShape(shape, lcx, cy, r, C_BLACK);
  drawEyeShape(shape, rcx, cy, r, C_BLACK);
  if (e.pupil && !d.blink) {
    const int16_t pr = (r - EX_THK) / 2;
    tft.fillCircle(lcx + d.pupilDX, cy, pr, C_BLACK);
    tft.fillCircle(rcx + d.pupilDX, cy, pr, C_BLACK);
  }
  if (e.brow) {
    // Anchor to the top of the eye, then rise by the tilt so an angled brow
    // never clips into the eye it belongs to. The brow keeps its height during
    // a blink so the face doesn't lose its expression mid-blink.
    const int tilt = (e.tiltL > e.tiltR) ? e.tiltL : e.tiltR;
    const int lift = (tilt > 0) ? tilt : 0;
    const int16_t browY = cy - eyeTopExtent(e.shape) - e.browGap
                             - EX_THK - lift - d.browDY;
    drawBrow(lcx, browY, e.tiltL, e.browHalf, true,  C_BLACK);
    drawBrow(rcx, browY, e.tiltR, e.browHalf, false, C_BLACK);
  }
  drawMeterOverlay();
}

void drawExpression(uint8_t idx) {
  if (idx >= EXPRESSION_COUNT) return;
  drawExpressionFrame(idx, restDelta(EXPRESSIONS[idx]));
}

// Play a delta sequence. `field` picks which delta the numbers drive, so all
// three flourishes below are one loop with different data.
void playSeq(uint8_t idx, const FrameDelta& base, uint8_t field,
             const int16_t* seq, uint8_t n, uint16_t ms) {
  for (uint8_t i = 0; i < n; i++) {
    FrameDelta f = base;
    switch (field) {
      case 0: f.eyeDX   = seq[i]; break;
      case 1: f.eyeDY   = seq[i]; break;
      default: f.radiusD = seq[i]; break;
    }
    drawExpressionFrame(idx, f);
    delay(speedMs(ms));
    server.handleClient();   // long flourishes shouldn't stall the web UI
  }
}

// Per-face flourish — the motion that IS the expression.
void animFlourish(uint8_t idx, const FrameDelta& base) {
  static const int16_t POP[]   = { -18, -11, 7, 2, 0 };            // snap wide, overshoot, settle
  static const int16_t SHAKE[] = { -8, 8, -6, 6, -4, 4, -2, 0 };   // rattle, damped
  static const int16_t DROOP[] = { -9, -5, 0, 5, 9, 4, 8, 11, 6, 0 };  // sag, catch it, sag again

  switch (EXPRESSIONS[idx].anim) {
    case AN_POP:   playSeq(idx, base, 2, POP,   5,  55); break;
    case AN_SHAKE: playSeq(idx, base, 0, SHAKE, 8,  42); break;
    case AN_DROOP: playSeq(idx, base, 1, DROOP, 10, 145); break;
    default: break;
  }
}

// Entry animation. The brow drops into place, then the pupils slide over, then
// the face's own flourish plays, then a blink settles it.
void animExpression(uint8_t idx) {
  if (idx >= EXPRESSION_COUNT) return;
  const Expression& e = EXPRESSIONS[idx];
  const FrameDelta rest = restDelta(e);

  if (e.brow) {
    for (int16_t b = 20; b > 0; b -= 5) {
      FrameDelta f = rest;
      f.browDY = b; f.pupilDX = 0;   // pupils stay centred until the brow lands
      drawExpressionFrame(idx, f);
      delay(speedMs(45));
    }
  }
  if (rest.pupilDX != 0) {
    // Pupils start centred and slide — the glance is the whole joke.
    const int16_t step = rest.pupilDX > 0 ? 3 : -3;
    for (int16_t p = 0; abs(p) < abs(rest.pupilDX); p += step) {
      FrameDelta f = rest; f.pupilDX = p;
      drawExpressionFrame(idx, f);
      delay(speedMs(55));
    }
  }

  animFlourish(idx, rest);
  drawExpressionFrame(idx, rest);

  // A flat bar blinking to a flat bar is an invisible change but a visible
  // full-screen flicker, so faces already drawn flat skip the settle-blink.
  if (e.shape != ES_FLAT) {
    delay(speedMs(140));
    FrameDelta b = rest; b.blink = true;
    drawExpressionFrame(idx, b);
    delay(speedMs(95));
    drawExpressionFrame(idx, rest);
  }
}

int8_t expressionIndex(const String& id) {
  for (uint8_t i = 0; i < EXPRESSION_COUNT; i++)
    if (id.equals(EXPRESSIONS[i].id)) return (int8_t)i;
  return -1;
}

// ═════════════════════════════════════════════════════════════
//  USAGE METER
//  The device is a dumb readout: something upstream POSTs percentages to
//  /meter and this draws them. It deliberately knows nothing about where the
//  numbers come from, which keeps credentials off the device entirely.
// ═════════════════════════════════════════════════════════════

#define MTR_WARN  60   // % at which a bar turns amber
#define MTR_CRIT  85   // ...and red
#define MTR_STALE 900000UL   // 15 min without an update greys everything out

uint8_t  mtrCtx = 0, mtrSes = 0, mtrWk = 0;   // percentages, 0-100
uint32_t mtrSeen = 0;      // millis() of the last /meter push; 0 = never fed
bool     mtrOverlay = false;   // pin the two bars to the bottom of other views

// Auto-cycle. Off by default: an instrument you have to wait for isn't
// glanceable, so this is opt-in rather than the primary way to read usage.
bool     mtrCycle     = false;
uint16_t mtrCycleSec  = 10;
uint32_t nextCycle    = 0;
uint8_t  cycleView    = VIEW_EYES_NORMAL;   // what to return to
uint8_t  cycleExpr    = 0;
bool     mtrCycleRand = false;   // pick a fresh face each time round

// What the crab does when the feeder reports a finished turn. The hook stays
// dumb — it reports the event, the device owns the reaction.
#define SF_NONE   0
#define SF_FIXED  1
#define SF_RANDOM 2
uint8_t  stopFaceMode = SF_NONE;
uint8_t  stopFaceIdx  = 0;

// ── Persistence ───────────────────────────────────────────────
// Settings survive a power cycle. Writes are coalesced rather than immediate:
// a slider dragged across its range would otherwise be dozens of flash writes
// for one intent, and NVS has a finite erase budget.
Preferences prefs;
bool     settingsDirty = false;
uint32_t settingsSaveAt = 0;

void settingsTouch() {
  settingsDirty  = true;
  settingsSaveAt = millis() + 3000;
}

void settingsSave() {
  prefs.begin("clawd", false);
  prefs.putBool ("ovl",    mtrOverlay);
  prefs.putBool ("cyc",    mtrCycle);
  prefs.putUShort("cycsec", mtrCycleSec);
  prefs.putBool ("cycrnd", mtrCycleRand);
  prefs.putUChar("sfmode", stopFaceMode);
  prefs.putUChar("sfidx",  stopFaceIdx);
  prefs.putUChar("speed",  animSpeed);
  prefs.end();
  settingsDirty = false;
}

void settingsLoad() {
  prefs.begin("clawd", true);
  mtrOverlay   = prefs.getBool  ("ovl",    false);
  mtrCycle     = prefs.getBool  ("cyc",    false);
  mtrCycleSec  = prefs.getUShort("cycsec", 10);
  mtrCycleRand = prefs.getBool  ("cycrnd", false);
  stopFaceMode = prefs.getUChar ("sfmode", SF_NONE);
  stopFaceIdx  = prefs.getUChar ("sfidx",  0);
  animSpeed    = prefs.getUChar ("speed",  1);
  prefs.end();
  // Clamp everything: a firmware change can shrink EXPRESSIONS[] under a
  // stored index, and a corrupt read shouldn't brick the boot.
  if (stopFaceIdx >= EXPRESSION_COUNT)  stopFaceIdx = 0;
  if (stopFaceMode > SF_RANDOM)         stopFaceMode = SF_NONE;
  if (animSpeed < 1 || animSpeed > 3)   animSpeed = 1;
  if (mtrCycleSec < 2 || mtrCycleSec > 600) mtrCycleSec = 10;
}

void settingsTick() {
  if (settingsDirty && millis() > settingsSaveAt) settingsSave();
}

bool meterStale() {
  return mtrSeen == 0 || (millis() - mtrSeen) > MTR_STALE;
}

uint16_t meterColour(uint8_t pct) {
  if (meterStale())    return C_MUTED;
  if (pct >= MTR_CRIT) return C_RED;
  if (pct >= MTR_WARN) return C_AMBER;
  return C_GREEN;
}

// A labelled horizontal bar. Track always drawn so an empty bar still reads as
// a bar rather than as a missing element.
void drawMeterBar(int16_t x, int16_t y, int16_t w, int16_t h,
                  uint8_t pct, const char* label) {
  tft.setTextSize(1);
  tft.setTextColor(meterStale() ? C_MUTED : C_WHITE);
  tft.setCursor(x, y + (h - 8) / 2);
  tft.print(label);

  const int16_t bx = x + 20, bw = w - 20;
  tft.fillRect(bx, y, bw, h, C_DARKBG);
  tft.drawRect(bx, y, bw, h, C_MUTED);
  const int16_t fill = (int32_t)(bw - 2) * (pct > 100 ? 100 : pct) / 100;
  if (fill > 0) tft.fillRect(bx + 1, y + 1, fill, h - 2, meterColour(pct));
}

// The two quota bars, used both as the bottom-of-screen overlay and inside the
// full meter view.
void drawMeterBars(int16_t y) {
  drawMeterBar(6, y,      DISP_W - 12, 14, mtrSes, "5h");
  drawMeterBar(6, y + 18, DISP_W - 12, 14, mtrWk,  "7d");
}

// Pinned strip along the bottom. Faces occupy roughly y=60..150, so the
// bottom 40px is free — the overlay never covers the crab.
void drawMeterOverlay() {
  if (!mtrOverlay || termMode) return;
  if (currentView != VIEW_EYES_NORMAL && currentView != VIEW_EYES_SQUISH
      && currentView != VIEW_EXPRESSION) return;
  drawMeterBars(198);
}

void drawArc(int16_t cx, int16_t cy, int16_t r, int16_t thk,
             float startDeg, float sweepDeg, uint16_t col) {
  if (sweepDeg <= 0) return;
  for (float a = startDeg; a <= startDeg + sweepDeg; a += 0.6f) {
    const float rad = a * 0.017453293f;
    const float c = cos(rad), s = sin(rad);
    for (int16_t t = 0; t < thk; t++)
      tft.drawPixel(cx + (int16_t)((r - t) * c), cy + (int16_t)((r - t) * s), col);
  }
}

// Full-screen readout: context gets the big arc because it moves every turn and
// decides when you lose your working state; the quotas get quiet bars because
// they crawl.
void drawMeterView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);

  const int16_t cx = DISP_W / 2, cy = 88, r = 62, thk = 13;
  drawArc(cx, cy, r, thk, 135, 270, C_DARKBG);     // clear
  drawArc(cx, cy, r, thk, 135, 270, tft.color565(38, 40, 46));   // track
  drawArc(cx, cy, r, thk, 135, 270.0f * (mtrCtx > 100 ? 100 : mtrCtx) / 100.0f,
          meterColour(mtrCtx));

  char buf[8];
  snprintf(buf, sizeof(buf), "%u%%", (unsigned)mtrCtx);
  tft.setTextSize(4);
  tft.setTextColor(meterStale() ? C_MUTED : C_WHITE);
  tft.setCursor(cx - (int16_t)(strlen(buf) * 12), cy - 16);
  tft.print(buf);

  tft.setTextSize(1);
  tft.setTextColor(C_MUTED);
  tft.setCursor(cx - 21, cy + 26);
  tft.print("context");

  if (meterStale()) {
    tft.setCursor(cx - 33, 168);
    tft.print("no data yet");
  }
  drawMeterBars(190);
}

// React to a finished turn. Also becomes the cycle's home face, so a cycling
// crab alternates between the meter and whatever face the last turn produced.
void triggerStopFace() {
  if (stopFaceMode == SF_NONE || termMode || busy) return;
  const uint8_t idx = (stopFaceMode == SF_RANDOM)
                    ? (uint8_t)random(EXPRESSION_COUNT) : stopFaceIdx;
  // A finished turn is reason enough to take over the boot info screen; don't
  // gate on uiStarted the way the idle animations do.
  uiStarted   = true;
  claudeState = CL_NONE;
  currentView = VIEW_EXPRESSION;
  currentExpr = idx;
  cycleView   = VIEW_EXPRESSION;
  cycleExpr   = idx;
  animExpression(idx);
  nextIdleBlink = millis() + 3000 + random(5000);
}

// Flip between the current face and the meter. Saves whatever face was showing
// so the cycle returns to it rather than resetting to default eyes.
void meterCycleTick() {
  if (!mtrCycle || busy || termMode || !uiStarted) return;
  if (currentView != VIEW_METER && currentView != VIEW_EYES_NORMAL
      && currentView != VIEW_EYES_SQUISH && currentView != VIEW_EXPRESSION) return;
  if (millis() < nextCycle) return;
  nextCycle = millis() + (uint32_t)mtrCycleSec * 1000UL;

  if (currentView == VIEW_METER) {
    if (mtrCycleRand) {
      cycleView = VIEW_EXPRESSION;
      cycleExpr = (uint8_t)random(EXPRESSION_COUNT);
    }
    currentView = cycleView;
    currentExpr = cycleExpr;
    if      (currentView == VIEW_EXPRESSION)  drawExpression(cycleExpr);
    else if (currentView == VIEW_EYES_SQUISH) drawSquishEyes();
    else                                      drawNormalEyes();
  } else {
    cycleView = currentView;
    cycleExpr = currentExpr;
    currentView = VIEW_METER;
    drawMeterView();
  }
}

void drawCodeView() {
  termMode = false;
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0,          DISP_W, 4, C_ORANGE);
  tft.fillRect(0, DISP_H - 4, DISP_W, 4, C_ORANGE);
  tft.setTextColor(C_ORANGE); tft.setTextSize(4);
  tft.setCursor((DISP_W - 144) / 2, DISP_H / 2 - 52); tft.print("Claude");
  tft.setTextColor(C_WHITE);  tft.setTextSize(4);
  tft.setCursor((DISP_W - 96) / 2,  DISP_H / 2 + 8);  tft.print("Code");
  tft.fillRect((DISP_W - 96) / 2, DISP_H / 2 + 52, 96, 3, C_ORANGE);
}

// ═════════════════════════════════════════════════════════════
//  TERMINAL
// ═════════════════════════════════════════════════════════════

void termClear() {
  for (uint8_t i = 0; i < TERM_ROWS; i++) termLines[i] = "";
  termRow = 0; termCol = 0;
}

void termDrawHeader() {
  tft.fillRect(0, 0, DISP_W, TERM_PAD_Y + 1, C_DARKBG);
  tft.setTextColor(C_ORANGE); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, 4); tft.print("clawd@mochi terminal");
  tft.drawFastHLine(0, TERM_PAD_Y, DISP_W, C_ORANGE);
}

// Prefix "clawd:~$ " in green, drawn only when the row has content
void termDrawPrefix(int16_t yy) {
  tft.setTextColor(C_GREEN); tft.setTextSize(1);
  tft.setCursor(TERM_PAD_X, yy + 6);
  tft.print("clawd:~$ ");
}

#define PREFIX_PX 54   // 9 chars × 6px = 54px at textSize 1

void termDrawLine(uint8_t r) {
  const int16_t yy = TERM_PAD_Y + 4 + r * TERM_CHAR_H;
  tft.fillRect(0, yy, DISP_W, TERM_CHAR_H, C_DARKBG);
  // show prefix only on the currently active (cursor) line
  if (r == termRow) termDrawPrefix(yy);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(TERM_PAD_X + PREFIX_PX, yy + 1);
  tft.print(termLines[r]);
  if (r == termRow) {
    const int16_t cx = TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W;
    tft.fillRect(cx, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  }
}

void termDrawLastChar() {
  if (termCol == 0) return;
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  const uint8_t prev  = termCol - 1;
  // erase prev cell (had cursor block)
  tft.fillRect(baseX + prev * TERM_CHAR_W, yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
  tft.setTextColor(C_WHITE); tft.setTextSize(2);
  tft.setCursor(baseX + prev * TERM_CHAR_W, yy + 1);
  tft.print(termLines[termRow][prev]);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
}

void termDrawBackspace() {
  const int16_t yy    = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
  const int16_t baseX = TERM_PAD_X + PREFIX_PX;
  // erase deleted char + old cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W * 2, TERM_CHAR_H - 1, C_DARKBG);
  // new cursor
  tft.fillRect(baseX + termCol * TERM_CHAR_W, yy + 1, TERM_CHAR_W - 2, TERM_CHAR_H - 2, C_GREEN);
  // if line now empty, erase the prefix too
  if (termLines[termRow].length() == 0) {
    tft.fillRect(0, yy, TERM_PAD_X + PREFIX_PX, TERM_CHAR_H, C_DARKBG);
  }
}

void termFullRedraw() {
  tft.fillScreen(C_DARKBG);
  termDrawHeader();
  for (uint8_t r = 0; r < TERM_ROWS; r++) termDrawLine(r);
}

void termScroll() {
  for (uint8_t i = 0; i < TERM_ROWS - 1; i++) termLines[i] = termLines[i + 1];
  termLines[TERM_ROWS - 1] = "";
  termRow = TERM_ROWS - 1;
  termFullRedraw();
}

void termAddChar(char c) {
  if (c == '\n' || c == '\r') {
    const int16_t yy = TERM_PAD_Y + 4 + termRow * TERM_CHAR_H;
    // erase cursor on current row
    tft.fillRect(TERM_PAD_X + PREFIX_PX + termCol * TERM_CHAR_W,
                 yy + 1, TERM_CHAR_W, TERM_CHAR_H - 1, C_DARKBG);
    termRow++; termCol = 0;
    if (termRow >= TERM_ROWS) { termScroll(); return; }
    termDrawLine(termRow);  // draws prefix on new line
  } else if (c == '\b' || c == 127) {
    if (termCol > 0) {
      termCol--;
      termLines[termRow].remove(termLines[termRow].length() - 1);
      termDrawBackspace();
    }
  } else if (c >= 32 && c < 127) {
    if (termCol >= TERM_COLS) {
      termRow++; termCol = 0;
      if (termRow >= TERM_ROWS) { termScroll(); return; }
    }
    // draw prefix on first char of this line
    if (termCol == 0) termDrawPrefix(TERM_PAD_Y + 4 + termRow * TERM_CHAR_H);
    termLines[termRow] += c;
    termCol++;
    termDrawLastChar();
  }
}

// ═════════════════════════════════════════════════════════════
//  ANIMATIONS
// ═════════════════════════════════════════════════════════════

void animNormalEyes() {
  busy = true;
  const int16_t offs[] = {-16, 16, -16, 16, 0};
  for (uint8_t i = 0; i < 5; i++) { drawNormalEyes(offs[i]); delay(speedMs(80)); }
  drawNormalEyes(0, true);  delay(speedMs(100));
  drawNormalEyes(0, false); delay(speedMs(70));
  drawNormalEyes(0, true);  delay(speedMs(70));
  drawNormalEyes(0, false);
  busy = false;
}

void animSquishEyes() {
  busy = true;
  for (uint8_t i = 0; i < 3; i++) {
    drawSquishEyes(false); delay(speedMs(160));
    drawSquishEyes(true);  delay(speedMs(100));
  }
  drawSquishEyes(false);
  busy = false;
}

void animLogoReveal() {
  busy = true;
  tft.fillScreen(animBgColor);
  for (uint16_t i = 0; i < LOGO_SEG_COUNT; i++) {
    int16_t x1 = pgm_read_word(&LOGO_SEGS[i][0]);
    int16_t y1 = pgm_read_word(&LOGO_SEGS[i][1]);
    int16_t x2 = pgm_read_word(&LOGO_SEGS[i][2]);
    int16_t y2 = pgm_read_word(&LOGO_SEGS[i][3]);
    tft.drawLine(x1, y1, x2, y2, C_WHITE);
    tft.drawLine(x1 + 1, y1, x2 + 1, y2, C_WHITE);
    if (i % 4 == 0) { server.handleClient(); delay(speedMs(8)); }
  }
  drawLogoFilled(animBgColor, C_WHITE);
  delay(1500);
  busy = false;
}

// ═════════════════════════════════════════════════════════════
//  CLAUDE STATUS (non-blocking, ticked from loop)
// ═════════════════════════════════════════════════════════════

void claudeCaption(const char* msg, uint16_t bg, uint16_t col) {
  tft.fillRect(0, 170, DISP_W, 22, bg);
  const int16_t w = strlen(msg) * 12;   // textSize 2 → 12 px/char
  tft.setTextColor(col); tft.setTextSize(2);
  tft.setCursor((DISP_W - w) / 2, 172);
  tft.print(msg);
}

void claudeDots(uint8_t active, uint16_t bg) {
  const int16_t y = 214, r = 5, gap = 26;
  const int16_t x0 = DISP_W / 2 - gap;
  for (uint8_t i = 0; i < 3; i++) {
    if (i == active) tft.fillCircle(x0 + i * gap, y, r, C_BLACK);
    else {
      tft.fillCircle(x0 + i * gap, y, r, bg);
      tft.drawCircle(x0 + i * gap, y, r, C_BLACK);
    }
  }
}

void enterClaude(uint8_t s) {
  claudeState   = s;
  claudeFrame   = 0;
  claudeFrameAt = 0;
  claudeExpires = 0;
  termMode      = false;
  uiStarted     = true;
  if (!backlightOn) setBacklight(true);

  switch (s) {
    case CL_WORKING:
      currentView = VIEW_EYES_SQUISH;
      drawSquishEyes();
      claudeCaption("working", animBgColor, C_BLACK);
      break;
    case CL_WAITING:
      currentView = VIEW_EYES_NORMAL;
      drawNormalEyes();
      claudeCaption("your turn!", animBgColor, C_BLACK);
      break;
    case CL_DONE:
      currentView = VIEW_EYES_SQUISH;
      animSquishEyes();   // happy open/close wiggle
      claudeCaption("done!", animBgColor, C_BLACK);
      claudeExpires = millis() + 6000;
      break;
    case CL_ERROR: {
      tft.fillScreen(C_RED);
      const int16_t lx = eyeLX(0), rx = eyeRX(0), cy = eyeCY();
      tft.fillRect(lx, cy - 5, EYE_W, 10, C_BLACK);   // flat, unimpressed
      tft.fillRect(rx, cy - 5, EYE_W, 10, C_BLACK);
      claudeCaption("error", C_RED, C_WHITE);
      break;
    }
    default:  // CL_NONE — back to plain idle eyes
      currentView = VIEW_EYES_NORMAL;
      drawNormalEyes();
      break;
  }
}

void claudeTick() {
  if (claudeState == CL_NONE) {
    // occasional idle blink so the crab feels alive between commands
    if (uiStarted && currentView == VIEW_EYES_NORMAL && !busy && !termMode
        && millis() > nextIdleBlink) {
      drawNormalEyes(0, true); delay(90); drawNormalEyes(0, false);
      nextIdleBlink = millis() + 3000 + random(5000);
    }
    // A held expression keeps breathing too — a face with its own flourish
    // replays that instead of blinking, since the motion is the character.
    if (uiStarted && currentView == VIEW_EXPRESSION && !busy && !termMode
        && millis() > nextIdleBlink) {
      const Expression& e = EXPRESSIONS[currentExpr];
      const FrameDelta rest = restDelta(e);
      if (e.anim != AN_NONE) {
        animFlourish(currentExpr, rest);
        drawExpressionFrame(currentExpr, rest);
      } else if (e.shape != ES_FLAT) {
        FrameDelta b = rest; b.blink = true;
        drawExpressionFrame(currentExpr, b); delay(90);
        drawExpressionFrame(currentExpr, rest);
      }
      nextIdleBlink = millis() + 3000 + random(5000);
    }
    return;
  }
  if (claudeExpires && millis() > claudeExpires) { enterClaude(CL_NONE); return; }
  if (millis() < claudeFrameAt) return;

  switch (claudeState) {
    case CL_WORKING:
      claudeDots(claudeFrame % 3, animBgColor);
      if (claudeFrame % 10 == 9) {   // quick blink now and then
        drawSquishEyes(true); delay(90); drawSquishEyes(false);
        claudeCaption("working", animBgColor, C_BLACK);
      }
      claudeFrame++;
      claudeFrameAt = millis() + 350;
      break;
    case CL_WAITING: {
      static const int16_t look[] = {0, -14, 0, 14};
      drawNormalEyes(look[claudeFrame % 4], claudeFrame % 7 == 6);
      claudeCaption("your turn!", animBgColor, C_BLACK);
      claudeFrame++;
      claudeFrameAt = millis() + 700;
      break;
    }
    default:  // CL_DONE waits for expiry, CL_ERROR is static
      claudeFrameAt = millis() + 500;
      break;
  }
}

// ═════════════════════════════════════════════════════════════
//  WEB PAGE
// ═════════════════════════════════════════════════════════════
const char INDEX_HTML[] PROGMEM = R"rawhtml(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Clawd Mochi</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
body{background:#1c1c20;font-family:'Courier New',monospace;color:#e8e4dc;
  display:flex;flex-direction:column;align-items:center;
  padding:20px 14px 52px;gap:14px;min-height:100vh}

.hdr{text-align:center;padding:2px 0 4px}
.mascot{font-size:15px;color:#c96a3e;line-height:1.3;font-weight:bold;
  font-family:'Courier New',monospace;display:block;letter-spacing:1px}
.sitename{font-size:10px;color:#5a5048;margin-top:8px;letter-spacing:3px}

.sec{width:100%;max-width:390px;font-size:10px;color:#8a8278;
  letter-spacing:2px;font-weight:bold;padding:0 2px}

/* Busy bar */
.busy{width:100%;max-width:390px;height:2px;background:#2e2a28;
  border-radius:1px;overflow:hidden;opacity:0;transition:opacity .2s}
.busy.show{opacity:1}
.busy-i{height:100%;width:30%;background:#c96a3e;border-radius:1px;
  animation:sl 1s linear infinite}
@keyframes sl{0%{margin-left:-30%}100%{margin-left:100%}}

/* Controls */
.ctrl{display:flex;gap:8px;width:100%;max-width:390px}
.cbtn{flex:1;background:#252428;border:1.5px solid #38343a;border-radius:10px;
  color:#b8b4ac;font-family:'Courier New',monospace;font-size:11px;font-weight:bold;
  padding:12px 4px;cursor:pointer;text-align:center;transition:all .12s}
.cbtn:active:not(:disabled){transform:scale(.94)}
.cbtn:disabled{opacity:.3;cursor:default}
.cbtn.on{border-color:#c96a3e;color:#c96a3e;background:#201408}
.cbtn.dim{border-color:#2e2a28;color:#4a4540}

/* View grid */
.vgrid{display:grid;grid-template-columns:1fr 1fr;gap:8px;width:100%;max-width:390px}
.cysec{font-size:10px;color:#8a8278;letter-spacing:.5px;display:inline-flex;
       align-items:center;gap:4px}
.cysec input[type=number]{width:46px;background:#252428;border:1.5px solid #38343a;
       border-radius:8px;color:#e8e4dc;font:inherit;font-size:11px;padding:4px 6px;
       text-align:center}
.cysec input[type=checkbox]{accent-color:#c96a3e;width:14px;height:14px}
.cysec select{background:#252428;border:1.5px solid #38343a;border-radius:8px;
       color:#e8e4dc;font:inherit;font-size:11px;padding:4px 6px}
.vbtn{background:#252428;border:1.5px solid #38343a;border-radius:12px;
  color:#d8d4cc;font-family:'Courier New',monospace;
  padding:14px 6px 10px;cursor:pointer;text-align:center;
  transition:all .12s;user-select:none}
.vbtn:active:not(:disabled){transform:scale(.94)}
.vbtn:disabled{opacity:.3;cursor:default}
.vbtn .ic{font-size:20px;display:block;margin-bottom:4px;line-height:1;color:#c96a3e}
.vbtn .nm{font-size:12px;font-weight:bold;color:#e8e4dc}
.vbtn .ht{font-size:9px;color:#8a8278;margin-top:3px}
.vbtn.active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="1"].active{border-color:#c96a3e;background:#201408}
.vbtn[data-v="2"].active{border-color:#4a8acd;background:#0c1628}
.vbtn[data-v="3"].active{border-color:#38343a;background:#201c18}

/* Speed slider */
.speed-row{width:100%;max-width:390px;display:flex;align-items:center;gap:10px}
.sl{font-size:10px;color:#6a6058;white-space:nowrap;min-width:36px}
input[type=range]{flex:1;accent-color:#c96a3e;cursor:pointer;height:20px}
.sv{font-size:11px;color:#c96a3e;min-width:44px;text-align:right;font-weight:bold}

/* Terminal */
.twrap{width:100%;max-width:390px;display:none;flex-direction:column;gap:8px}
.twrap.open{display:flex}
.thdr{display:flex;justify-content:space-between;align-items:center}
.tttl{font-size:11px;color:#28b878;letter-spacing:1px;font-weight:bold}
.tx{background:#0c1e12;border:2px solid #1a4828;border-radius:9px;
  color:#28b878;font-family:'Courier New',monospace;font-size:13px;
  font-weight:bold;padding:10px 18px;cursor:pointer}
.tx:active{background:#081410}
.trow{display:flex;gap:6px}
.tin{flex:1;background:#0c1018;border:1.5px solid #1a2820;border-radius:9px;
  color:#40d880;font-family:'Courier New',monospace;font-size:15px;
  padding:11px;outline:none}
.tin::placeholder{color:#2a3828}
.tgo{background:#1a9060;border:none;border-radius:9px;color:#fff;
  font-family:'Courier New',monospace;font-size:22px;font-weight:bold;
  padding:11px 16px;cursor:pointer;min-width:52px}
.tgo:active{background:#0f6040}

/* Canvas */
.cwrap{width:100%;max-width:390px;background:#222028;border:1.5px solid #38343a;
  border-radius:12px;padding:12px;flex-direction:column;gap:10px;display:none}
.cwrap.open{display:flex}
.crow{display:flex;gap:8px}
.ci{display:flex;flex-direction:column;align-items:center;gap:4px;flex:1}
.cl{font-size:10px;color:#7a7068;letter-spacing:1px;font-weight:bold}
.cs{width:100%;height:38px;border-radius:7px;border:1.5px solid #38343a;cursor:pointer;padding:0}
.dacts{display:flex;gap:7px}
.db{flex:1;background:#1c1820;border:1.5px solid #38343a;border-radius:9px;
  color:#c0bab8;font-family:'Courier New',monospace;font-size:11px;
  font-weight:bold;padding:11px 4px;cursor:pointer;transition:all .12s}
.db:active{transform:scale(.95);background:#281838}
.db.hi{border-color:#c96a3e;color:#c96a3e}
canvas{width:100%;border-radius:8px;border:1.5px solid #38343a;
  touch-action:none;cursor:crosshair;display:block}

/* Toast */
.toast{position:fixed;bottom:18px;left:50%;transform:translateX(-50%);
  background:#252428;border:1.5px solid #38343a;border-radius:9px;
  font-size:12px;color:#d8d4cc;padding:7px 16px;opacity:0;
  transition:opacity .18s;pointer-events:none;white-space:nowrap;z-index:99}
.toast.show{opacity:1}
</style>
</head>
<body>

<div class="hdr">
  <span class="mascot">&#x2590;&#x259B;&#x2588;&#x2588;&#x2588;&#x259C;&#x258C;<br>&#x259C;&#x2588;&#x2588;&#x2588;&#x2588;&#x2588;&#x259B;<br>&#x2598;&#x2598;&nbsp;&#x259D;&#x259D;</span>
  <div class="sitename">CLAWD &middot; MOCHI &middot; CONTROLLER</div>
</div>

<div class="busy" id="busy"><div class="busy-i"></div></div>

<div class="sec">// controls</div>
<div class="ctrl">
  <button class="cbtn on" id="blBtn" onclick="toggleBL()">&#9728; display on</button>
  <button class="cbtn dim" id="ovBtn" onclick="toggleOverlay()">&#9707; usage bars off</button>
  <button class="cbtn dim" id="cyBtn" onclick="toggleCycle()">&#8635; cycle off</button>
  <span class="cysec">every
    <input type="number" id="cySec" min="2" max="600" value="10" onchange="setCycleSec(this.value)">s
  </span>
  <span class="cysec">
    <input type="checkbox" id="cyRnd" onchange="setCycleRand(this.checked)">random face
  </span>
</div>

<div class="sec">// on stop</div>
<div class="ctrl" style="justify-content:center">
  <span class="cysec">show
    <select id="sfSel" onchange="setStopFace(this.value)">
      <option value="none">nothing</option>
      <option value="random">a random face</option>
    </select>
  </span>
</div>

<div class="sec">// views</div>
<div class="vgrid" id="views">
  <button class="vbtn active" data-v="0" onclick="setView(0)">
    <span class="ic">&#9632; &#9632;</span>
    <span class="nm">Normal eyes</span>
    <span class="ht">wiggle + blink</span>
  </button>
  <button class="vbtn" data-v="1" onclick="setView(1)">
    <span class="ic">&gt; &lt;</span>
    <span class="nm">Squish eyes</span>
    <span class="ht">open / close</span>
  </button>
  <button class="vbtn" data-v="2" onclick="setView(2)">
    <span class="ic">{ }</span>
    <span class="nm">Claude Code</span>
    <span class="ht">opens terminal</span>
  </button>
  <button class="vbtn" data-v="3" onclick="toggleCanvas()">
    <span class="ic">&#11035;</span>
    <span class="nm">Canvas</span>
    <span class="ht">draw on display</span>
  </button>
  <button class="vbtn" data-v="4" onclick="setMeter()">
    <span class="ic">&#9680;</span>
    <span class="nm">Usage</span>
    <span class="ht">context + quota</span>
  </button>
</div>

<div class="sec">// speed</div>
<div class="speed-row">
  <span class="sl">slow</span>
  <input type="range" id="spd" min="1" max="3" value="1" step="1" oninput="setSpeed(this.value)">
  <span class="sv" id="spdV">slow</span>
</div>

<div class="ctrl">
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">BACKGROUND</span>
    <input type="color" class="cs" id="bgCol" value="#aa4818" oninput="onBgChange(this.value)">
  </div>
  <div class="ci" style="flex:1;display:flex;flex-direction:column;gap:4px;align-items:stretch">
    <span class="cl" style="font-size:10px;color:#8a8278;letter-spacing:1px;font-weight:bold;text-align:center">PEN COLOR</span>
    <input type="color" class="cs" id="penCol" value="#000000">
  </div>
</div>

<div class="sec">// terminal</div>
<div class="twrap" id="twrap">
  <div class="thdr">
    <span class="tttl">&#9658; clawd:~$</span>
    <button class="tx" onclick="closeTerm()">&#x2715; exit terminal</button>
  </div>
  <div class="trow">
    <input class="tin" id="tin" type="text" placeholder="type here..."
           autocomplete="off" autocorrect="off" autocapitalize="off" spellcheck="false">
    <button class="tgo" onclick="termEnter()">&#8629;</button>
  </div>
</div>

<div class="cwrap" id="cwrap">
  <div class="dacts">
    <button class="db hi" onclick="clearAll()">&#11035; clear</button>
    <button class="db" style="border-color:#28b878;color:#28b878" onclick="toggleCanvas()">&#10003; done</button>
  </div>
  <canvas id="cvs" width="240" height="240"></canvas>
</div>

<div class="toast" id="toast"></div>

<script>
let activeView  = 0;
let termOpen    = false;
let canvasOpen  = false;
let blOn        = true;
let isBusy      = false;
let drawing     = false;
let lastX = 0, lastY = 0;
let tt;

const spdLabels = ['','slow','normal','fast'];

// ── Toast ──────────────────────────────────────────────────────
function toast(msg, ok=true) {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.style.borderColor = ok ? '#28b878' : '#c96a3e';
  el.classList.add('show');
  clearTimeout(tt);
  tt = setTimeout(() => el.classList.remove('show'), 1300);
}

// ── Busy ────────────────────────────────────────────────────────
function setBusy(b) {
  isBusy = b;
  document.getElementById('busy').classList.toggle('show', b);
  const locked = b || termOpen;
  document.querySelectorAll('.vbtn').forEach(el => {
    // when canvas open, keep canvas btn (data-v=3) active so user can exit
    el.disabled = canvasOpen ? parseInt(el.dataset.v) !== 3 : locked;
  });
  document.querySelectorAll('.lbtn').forEach(el => el.disabled = locked || canvasOpen);
  document.querySelectorAll('.cbtn').forEach(el => {
    if (el.id !== 'blBtn') el.disabled = locked;
  });
}

// ── HTTP ────────────────────────────────────────────────────────
async function req(path) {
  try { const r = await fetch(path); return r.ok; }
  catch(e) { toast('no connection', false); return false; }
}

// ── Faces ───────────────────────────────────────────────────────
// Labels are UI-side decoration; the firmware owns the canonical list, so
// adding a row to EXPRESSIONS[] makes a button appear here with no HTML edit.
// Unknown ids still render, just without a glyph or hint.
const FACE_META = {
  disapproval: ['&#3232;_&#3232;', 'Disapproval', 'the stare'],
  skeptical:   ['&#8857;_&#9673;', 'Skeptical',   'one brow up'],
  angry:       ['&#9699;_&#9700;', 'Angry',       'brows down'],
  sideeye:     ['&#9673;&#9673;_', 'Side-eye',    'pupils slide'],
  alert:       ['O_O',             'Alert',       'pops wide'],
  happy:       ['^_^',             'Happy',       'squint smile'],
  sleepy:      ['&#8212;_&#8212;', 'Sleepy',      'slow droop'],
  dead:        ['x_x',             'Dead',        'shakes it off']
};

async function loadFaces() {
  let list;
  try { list = (await (await fetch('/face')).json()).faces; }
  catch(e) { return; }
  const box = document.getElementById('views');
  for (const id of list) {
    const m = FACE_META[id] || ['&#9632;&#9632;', id, ''];
    const b = document.createElement('button');
    b.className = 'vbtn';
    b.dataset.f = id;
    b.innerHTML = '<span class="ic">' + m[0] + '</span>' +
                  '<span class="nm">' + m[1] + '</span>' +
                  '<span class="ht">' + m[2] + '</span>';
    b.onclick = () => setFace(id);
    box.appendChild(b);
  }
  const sel = document.getElementById('sfSel');
  for (const id of list) {
    const o = document.createElement('option');
    o.value = id;
    o.textContent = (FACE_META[id] || [0, id])[1].toLowerCase();
    sel.appendChild(o);
  }
}

async function setMeter() {
  if (isBusy || termOpen || canvasOpen) return;
  if (!await req('/meter/view')) return;
  activeView = 4;
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === 4));
}

let cyOn = false;
async function toggleCycle() {
  cyOn = !cyOn;
  const sec = document.getElementById('cySec').value || 10;
  if (!await req('/meter/cycle?on=' + (cyOn ? 1 : 0) + '&sec=' + sec)) { cyOn = !cyOn; return; }
  const b = document.getElementById('cyBtn');
  b.innerHTML = cyOn ? '\u21BB cycle on' : '\u21BB cycle off';
  b.classList.toggle('on', cyOn);
  b.classList.toggle('dim', !cyOn);
}

async function setCycleSec(v) {
  await req('/meter/cycle?sec=' + v);
}

async function setCycleRand(on) {
  await req('/meter/cycle?random=' + (on ? 1 : 0));
}

// "none" and "random" are policies; anything else is a face id, which the
// device stores as the fixed choice.
async function setStopFace(v) {
  if (v === 'none' || v === 'random') await req('/stopface?mode=' + v);
  else await req('/stopface?mode=fixed&f=' + encodeURIComponent(v));
}

let ovOn = false;
async function toggleOverlay() {
  ovOn = !ovOn;
  if (!await req('/meter/overlay?on=' + (ovOn ? 1 : 0))) { ovOn = !ovOn; return; }
  const b = document.getElementById('ovBtn');
  b.innerHTML = ovOn ? '\u25FB usage bars on' : '\u25FB usage bars off';
  b.classList.toggle('on', ovOn);
  b.classList.toggle('dim', !ovOn);
}

async function setFace(id) {
  if (isBusy || termOpen || canvasOpen) return;
  if (!await req('/face?f=' + encodeURIComponent(id))) return;
  activeView = -1;
  document.querySelectorAll('.vbtn').forEach(
    b => b.classList.toggle('active', b.dataset.f === id));
}

async function waitNotBusy() {
  for (let i = 0; i < 100; i++) {
    try {
      const r = await fetch('/state');
      const j = await r.json();
      if (!j.busy) return;
    } catch(e) {}
    await new Promise(r => setTimeout(r, 150));
  }
}

// ── Background colour ───────────────────────────────────────────
async function onBgChange(hex) {
  if (canvasOpen) {
    await req('/draw/clear?bg=' + encodeURIComponent(hex));
  } else {
    await req('/redraw?bg=' + encodeURIComponent(hex));
  }
  redrawCanvas(hex);
}

// ── Speed ───────────────────────────────────────────────────────
async function setSpeed(v) {
  document.getElementById('spdV').textContent = spdLabels[v];
  await req('/speed?v=' + v);
}

// ── Views ───────────────────────────────────────────────────────
async function setView(v) {
  if (isBusy || termOpen || canvasOpen) return;
  if (v === 3) { toggleCanvas(); return; }  // canvas button in grid
  const keys = ['w','s','d'];
  if (!await req('/cmd?k=' + keys[v])) return;
  activeView = v;
  // Face buttons share the .vbtn class and carry no data-v, so this clears
  // them for free — NaN never matches.
  document.querySelectorAll('.vbtn').forEach(b =>
    b.classList.toggle('active', parseInt(b.dataset.v) === v));
  if (v === 2) {
    termOpen = true;
    document.getElementById('twrap').classList.add('open');
    setBusy(false);   // re-run to apply termOpen lock
    setBusy(false);
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    const cvb = document.getElementById('cvBtn'); if (cvb) cvb.disabled = true;
    document.getElementById('tin').focus();
    toast('terminal open');
    return;
  }
  setBusy(true);
  await waitNotBusy();
  setBusy(false);
}

// ── Logo animations (kept for startup, not exposed in UI) ──────

// ── Backlight ───────────────────────────────────────────────────
async function toggleBL() {
  blOn = !blOn;
  await req('/backlight?on=' + (blOn ? 1 : 0));
  const b = document.getElementById('blBtn');
  b.textContent = blOn ? '\u2600 display on' : '\u25cb display off';
  b.classList.toggle('on', blOn);
  b.classList.toggle('dim', !blOn);
}

// ── Canvas toggle ───────────────────────────────────────────────
async function toggleCanvas() {
  canvasOpen = !canvasOpen;
  document.getElementById('cwrap').classList.toggle('open', canvasOpen);
  const b = document.getElementById('cvBtn');
  if (b) { b.classList.toggle('on', canvasOpen); b.textContent = canvasOpen ? '\u2b1b canvas on' : '\u2b1b canvas'; }
  // highlight the canvas vbtn (data-v=3) in the grid
  document.querySelectorAll('.vbtn').forEach(btn =>
    btn.classList.toggle('active', canvasOpen && parseInt(btn.dataset.v) === 3));
  await req('/canvas?on=' + (canvasOpen ? 1 : 0));
  if (canvasOpen) {
    const bg = document.getElementById('bgCol').value;
    redrawCanvas(bg);
    await req('/draw/clear?bg=' + encodeURIComponent(bg));
    // lock all other buttons
    document.querySelectorAll('.vbtn,.lbtn').forEach(b => b.disabled = true);
    toast('canvas active');
  } else {
    setBusy(false);   // re-evaluate locks
    toast('canvas off');
  }
}

// ── Terminal ────────────────────────────────────────────────────
const tin = document.getElementById('tin');
let lastVal = '';
tin.addEventListener('input', async () => {
  const cur = tin.value, prev = lastVal;
  if (cur.length > prev.length) {
    await req('/char?c=' + encodeURIComponent(cur[cur.length - 1]));
  } else if (cur.length < prev.length) {
    await req('/char?c=%08');
  }
  lastVal = cur;
});
async function termEnter() {
  await req('/char?c=%0A');
  tin.value = ''; lastVal = ''; tin.focus();
}
tin.addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); termEnter(); }
});
async function closeTerm() {
  await req('/cmd?k=q');
  termOpen = false;
  document.getElementById('twrap').classList.remove('open');
  setBusy(false);
  toast('terminal closed');
}

// ── Canvas drawing — send full stroke on finger lift ────────────
const cvs = document.getElementById('cvs');
const ctx = cvs.getContext('2d');
let strokePts = [];

function getPos(e) {
  const r = cvs.getBoundingClientRect();
  const sx = cvs.width / r.width, sy = cvs.height / r.height;
  const s = e.touches ? e.touches[0] : e;
  return { x: (s.clientX - r.left) * sx, y: (s.clientY - r.top) * sy };
}

function redrawCanvas(hex) {
  ctx.fillStyle = hex;
  ctx.fillRect(0, 0, cvs.width, cvs.height);
}

function startDraw(e) {
  e.preventDefault();
  drawing = true;
  strokePts = [];
  const p = getPos(e); lastX = p.x; lastY = p.y;
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  // draw dot on canvas preview only — no display send yet
  ctx.beginPath(); ctx.arc(p.x, p.y, 2, 0, Math.PI * 2);
  ctx.fillStyle = document.getElementById('penCol').value; ctx.fill();
}
function moveDraw(e) {
  if (!drawing) return; e.preventDefault();
  const p = getPos(e);
  ctx.beginPath(); ctx.moveTo(lastX, lastY); ctx.lineTo(p.x, p.y);
  ctx.strokeStyle = document.getElementById('penCol').value;
  ctx.lineWidth = 4; ctx.lineCap = 'round'; ctx.stroke();
  strokePts.push({ x: Math.round(p.x), y: Math.round(p.y) });
  lastX = p.x; lastY = p.y;
}
async function endDraw(e) {
  if (!drawing) return; drawing = false;
  if (!canvasOpen || strokePts.length < 1) return;
  const pen = document.getElementById('penCol').value.replace('#', '');
  const pts = strokePts.map(p => p.x + ',' + p.y).join(';');
  await req('/draw/stroke?pen=' + pen + '&pts=' + encodeURIComponent(pts));
  strokePts = [];
}

cvs.addEventListener('mousedown',  startDraw);
cvs.addEventListener('mousemove',  moveDraw);
cvs.addEventListener('mouseup',    endDraw);
cvs.addEventListener('mouseleave', endDraw);
cvs.addEventListener('touchstart', startDraw, {passive:false});
cvs.addEventListener('touchmove',  moveDraw,  {passive:false});
cvs.addEventListener('touchend',   endDraw);

// Clear = clear both web canvas and display
async function clearAll() {
  const bg = document.getElementById('bgCol').value;
  redrawCanvas(bg);
  await req('/draw/clear?bg=' + encodeURIComponent(bg));
  toast('cleared');
}

// Init: sync speed and backlight from ESP32, reset bg to default
(async () => {
  try {
    const r = await fetch('/state');
    const j = await r.json();
    // Sync speed
    const spd = j.speed || 1;
    document.getElementById('spd').value = spd;
    document.getElementById('spdV').textContent = spdLabels[spd];
    // Sync backlight
    if (j.bl === false) {
      blOn = false;
      const b = document.getElementById('blBtn');
      b.textContent = '\u25cb display off';
      b.classList.remove('on'); b.classList.add('dim');
    }
    // Reflect persisted device settings rather than markup defaults.
    ovOn = !!j.ovl;
    const ob = document.getElementById('ovBtn');
    ob.innerHTML = ovOn ? '\u25FB usage bars on' : '\u25FB usage bars off';
    ob.classList.toggle('on', ovOn); ob.classList.toggle('dim', !ovOn);

    cyOn = !!j.cyc;
    const cb = document.getElementById('cyBtn');
    cb.innerHTML = cyOn ? '\u21BB cycle on' : '\u21BB cycle off';
    cb.classList.toggle('on', cyOn); cb.classList.toggle('dim', !cyOn);

    document.getElementById('cySec').value = j.cycsec || 10;
    document.getElementById('cyRnd').checked = !!j.cycrnd;

    await loadFaces();
    const sf = document.getElementById('sfSel');
    sf.value = j.sfmode === 'fixed' ? (j.sfface || 'none') : (j.sfmode || 'none');
  } catch(e) { loadFaces(); }
  // Always reset bg picker to default orange on page load
  document.getElementById('bgCol').value = '#aa4818';
  redrawCanvas('#aa4818');
})();
</script>
</body>
</html>
)rawhtml";

// ═════════════════════════════════════════════════════════════
//  WEB ROUTES
// ═════════════════════════════════════════════════════════════

void routeRoot() {
  server.sendHeader("Cache-Control", "no-store, no-cache, must-revalidate");
  server.sendHeader("Pragma", "no-cache");
  server.send_P(200, "text/html", INDEX_HTML);
}

void routeCmd() {
  if (!server.hasArg("k") || server.arg("k").isEmpty()) {
    server.send(400, "application/json", "{\"e\":1}"); return;
  }
  const char c = server.arg("k")[0];
  uiStarted = true;
  claudeState = CL_NONE;   // manual control overrides hook-driven state

  if (termMode) {
    if (c == 'q') { termMode = false; drawCodeView(); }
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }

  server.send(200, "application/json", "{\"ok\":1}");
  switch (c) {
    case 'w': currentView = VIEW_EYES_NORMAL; animNormalEyes(); break;
    case 's': currentView = VIEW_EYES_SQUISH; animSquishEyes(); break;
    case 'd':
      currentView = VIEW_CODE; drawCodeView();
      termMode = true; termClear(); termFullRedraw(); break;
    case 'a':
      currentView = VIEW_EYES_NORMAL;
      animLogoReveal();
      break;
  }
}

// /face?f=<id>  — show a static expression. /face with no arg lists them,
// so the UI can build its own buttons instead of hardcoding the set twice.
void routeFace() {
  if (!server.hasArg("f")) {
    String out = "{\"faces\":[";
    for (uint8_t i = 0; i < EXPRESSION_COUNT; i++) {
      if (i) out += ",";
      out += "\""; out += EXPRESSIONS[i].id; out += "\"";
    }
    out += "]}";
    server.send(200, "application/json", out);
    return;
  }
  const int8_t idx = expressionIndex(server.arg("f"));
  if (idx < 0) { server.send(404, "application/json", "{\"e\":1}"); return; }

  uiStarted   = true;
  claudeState = CL_NONE;   // manual control overrides hook-driven state
  termMode    = false;
  currentView = VIEW_EXPRESSION;
  currentExpr = (uint8_t)idx;
  server.send(200, "application/json", "{\"ok\":1}");
  animExpression((uint8_t)idx);
  nextIdleBlink = millis() + 3000 + random(5000);
}

// /meter?ctx=&session=&week=  — any subset; omitted values keep their last
// reading, so a feeder that only has some numbers can still push what it has.
void routeMeter() {
  bool got = false;
  if (server.hasArg("ctx"))     { mtrCtx = constrain(server.arg("ctx").toInt(),     0, 100); got = true; }
  if (server.hasArg("session")) { mtrSes = constrain(server.arg("session").toInt(), 0, 100); got = true; }
  if (server.hasArg("week"))    { mtrWk  = constrain(server.arg("week").toInt(),    0, 100); got = true; }
  if (got) mtrSeen = millis();

  const bool stop = server.hasArg("stop") && server.arg("stop").toInt() != 0;
  server.send(200, "application/json", "{\"ok\":1}");

  if (stop && stopFaceMode != SF_NONE) { triggerStopFace(); return; }
  if (!got) return;
  if (currentView == VIEW_METER)   drawMeterView();
  else if (mtrOverlay && !termMode) drawMeterOverlay();
}

// /stopface?mode=none|fixed|random&f=<id> — what a finished turn looks like.
void routeStopFace() {
  if (server.hasArg("f")) {
    const int8_t i = expressionIndex(server.arg("f"));
    if (i >= 0) stopFaceIdx = (uint8_t)i;
  }
  if (server.hasArg("mode")) {
    const String m = server.arg("mode");
    if      (m == "random") stopFaceMode = SF_RANDOM;
    else if (m == "fixed")  stopFaceMode = SF_FIXED;
    else                    stopFaceMode = SF_NONE;
  }
  settingsTouch();
  const char* mode = stopFaceMode == SF_RANDOM ? "random"
                   : stopFaceMode == SF_FIXED  ? "fixed" : "none";
  server.send(200, "application/json",
              String("{\"mode\":\"") + mode + "\",\"face\":\"" +
              EXPRESSIONS[stopFaceIdx].id + "\"}");
}

// /meter/view — switch the display to the full usage readout.
void routeMeterView() {
  uiStarted   = true;
  claudeState = CL_NONE;
  termMode    = false;
  currentView = VIEW_METER;
  server.send(200, "application/json", "{\"ok\":1}");
  drawMeterView();
}

// /meter/cycle?on=0|1&sec=N — alternate between the face and the meter.
void routeMeterCycle() {
  if (server.hasArg("sec"))
    mtrCycleSec = constrain(server.arg("sec").toInt(), 2, 600);
  if (server.hasArg("random"))
    mtrCycleRand = server.arg("random").toInt() != 0;
  if (server.hasArg("on")) {
    mtrCycle = server.arg("on").toInt() != 0;
    if (mtrCycle) {
      cycleView = (currentView == VIEW_METER) ? VIEW_EYES_NORMAL : currentView;
      cycleExpr = currentExpr;
      nextCycle = millis() + (uint32_t)mtrCycleSec * 1000UL;
    }
  }
  settingsTouch();
  server.send(200, "application/json",
              String("{\"cycle\":") + (mtrCycle ? "1" : "0") +
              ",\"sec\":" + String(mtrCycleSec) +
              ",\"random\":" + (mtrCycleRand ? "1" : "0") + "}");
}

// /meter/overlay?on=0|1 — with no arg, reports the current setting.
void routeMeterOverlay() {
  if (server.hasArg("on")) {
    mtrOverlay = server.arg("on").toInt() != 0;
    settingsTouch();
    if (currentView == VIEW_EXPRESSION)        drawExpression(currentExpr);
    else if (currentView == VIEW_EYES_NORMAL)  drawNormalEyes();
    else if (currentView == VIEW_EYES_SQUISH)  drawSquishEyes();
  }
  server.send(200, "application/json",
              String("{\"overlay\":") + (mtrOverlay ? "1" : "0") + "}");
}

void routeChar() {
  if (!termMode) { server.send(200, "application/json", "{\"ok\":1}"); return; }
  const String val = server.arg("c");
  if (val.length() > 0) termAddChar(val[0]);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeSpeed() {
  if (server.hasArg("v")) {
    animSpeed = constrain(server.arg("v").toInt(), 1, 3);
    settingsTouch();
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

// /claude?e=working|waiting|done|error|idle — Claude Code hook events
void routeClaude() {
  const String e = server.arg("e");
  server.send(200, "application/json", "{\"ok\":1}");
  if      (e == "working") enterClaude(CL_WORKING);
  else if (e == "waiting") enterClaude(CL_WAITING);
  else if (e == "done")    enterClaude(CL_DONE);
  else if (e == "error")   enterClaude(CL_ERROR);
  else if (e == "idle")    enterClaude(CL_NONE);
}

// /redraw?bg=hex — set animBg and immediately redraw current view
void routeRedraw() {
  uiStarted = true;
  if (server.hasArg("bg")) {
    animBgColor = hexToRgb565(server.arg("bg"));
    drawBgColor = animBgColor;
  }
  switch (currentView) {
    case VIEW_EYES_NORMAL: drawNormalEyes(); break;
    case VIEW_EYES_SQUISH: drawSquishEyes(); break;
    case VIEW_CODE:        drawCodeView();   break;
    case VIEW_DRAW:        tft.fillScreen(drawBgColor); break;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeCanvas() {
  uiStarted = true;
  claudeState = CL_NONE;
  const bool on = server.hasArg("on") && server.arg("on") == "1";
  if (on) { currentView = VIEW_DRAW; tft.fillScreen(drawBgColor); }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawClear() {
  const String bg = server.hasArg("bg") ? server.arg("bg") : "#aa4818";
  drawBgColor = hexToRgb565(bg);
  animBgColor = drawBgColor;  // keep in sync
  currentView = VIEW_DRAW; termMode = false;
  tft.fillScreen(drawBgColor);
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeDrawStroke() {
  if (!server.hasArg("pts") || !server.hasArg("pen")) {
    server.send(200, "application/json", "{\"ok\":1}"); return;
  }
  const uint16_t color = hexToRgb565(server.arg("pen"));
  const String   data  = server.arg("pts");
  currentView = VIEW_DRAW;

  struct Pt { int16_t x, y; };
  Pt prev = {-1, -1};
  int start = 0;
  while (start < (int)data.length()) {
    int semi = data.indexOf(';', start);
    if (semi == -1) semi = data.length();
    String entry = data.substring(start, semi);
    const int comma = entry.indexOf(',');
    if (comma > 0) {
      const int16_t x = entry.substring(0, comma).toInt();
      const int16_t y = entry.substring(comma + 1).toInt();
      if (prev.x >= 0) {
        tft.drawLine(prev.x, prev.y, x, y, color);
        tft.drawLine(prev.x + 1, prev.y, x + 1, y, color);
        tft.drawLine(prev.x, prev.y + 1, x, y + 1, color);
      } else {
        tft.fillCircle(x, y, 2, color);
      }
      prev = {x, y};
    }
    start = semi + 1;
  }
  server.send(200, "application/json", "{\"ok\":1}");
}

void routeBacklight() {
  setBacklight(server.hasArg("on") && server.arg("on") == "1");
  server.send(200, "application/json", "{\"ok\":1}");
}

// Convert RGB565 back to #RRGGBB for state endpoint
String rgb565ToHex(uint16_t c) {
  uint8_t r = ((c >> 11) & 0x1F) << 3;
  uint8_t g = ((c >> 5)  & 0x3F) << 2;
  uint8_t b = (c & 0x1F) << 3;
  char buf[8];
  snprintf(buf, sizeof(buf), "#%02x%02x%02x", r, g, b);
  return String(buf);
}

void routeState() {
  String j = "{\"view\":"; j += currentView;
  j += ",\"busy\":";   j += busy        ? "true" : "false";
  j += ",\"term\":";   j += termMode    ? "true" : "false";
  j += ",\"bl\":";     j += backlightOn ? "true" : "false";
  j += ",\"speed\":";  j += animSpeed;
  j += ",\"claude\":"; j += claudeState;
  j += ",\"ovl\":";    j += mtrOverlay   ? "true" : "false";
  j += ",\"cyc\":";    j += mtrCycle     ? "true" : "false";
  j += ",\"cycsec\":"; j += mtrCycleSec;
  j += ",\"cycrnd\":"; j += mtrCycleRand ? "true" : "false";
  j += ",\"sfmode\":\"";
  j += stopFaceMode == SF_RANDOM ? "random" : stopFaceMode == SF_FIXED ? "fixed" : "none";
  j += "\",\"sfface\":\"";  j += EXPRESSIONS[stopFaceIdx].id;  j += "\"";
  j += ",\"sta\":";    j += staConnected ? "true" : "false";
  j += ",\"ip\":\"";
  j += staConnected ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  j += "\"}";
  server.send(200, "application/json", j);
}

void routeNotFound() { server.send(404, "text/plain", "not found"); }

// ═════════════════════════════════════════════════════════════
//  SETUP
// ═════════════════════════════════════════════════════════════

void setup() {
  Serial.begin(115200);

  pinMode(TFT_BLK, OUTPUT);
  setBacklight(true);

  SPI.begin(8, -1, 10, TFT_CS);   // SCK=8, MOSI=10
  tft.init(240, 240);
  tft.setSPISpeed(40000000);
  tft.setRotation(1);
  initColours();
  settingsLoad();

  // ── Boot splash ────────────────────────────────────────────
  tft.fillScreen(animBgColor);
  tft.setTextColor(C_WHITE); tft.setTextSize(3);
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 - 22); tft.print("Clawd");
  tft.setCursor(DISP_W / 2 - 54, DISP_H / 2 + 14); tft.print("Mochi");
  delay(1200);

  // ── Logo shown once at startup ─────────────────────────────
  animLogoReveal();

  // ── Start WiFi: join home network, hotspot as fallback ─────
  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    // C3 Super Mini antenna quirk: full TX power breaks many STA connects
    WiFi.setTxPower(WIFI_POWER_8_5dBm);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    const uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(250);
    staConnected = (WiFi.status() == WL_CONNECTED);
    if (staConnected) {
      Serial.print("STA connected: ");  Serial.print(WIFI_SSID);
      Serial.print(" ip=");             Serial.println(WiFi.localIP());
    } else {
      Serial.print("STA failed (status=");
      Serial.print(WiFi.status());      Serial.println("), falling back to AP");
    }
  }
  if (!staConnected) {
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    Serial.print("AP up: ");  Serial.print(AP_SSID);
    Serial.print(" ip=");     Serial.println(WiFi.softAPIP());
  }
  if (MDNS.begin("clawd")) MDNS.addService("http", "tcp", 80);

  // ── WiFi info screen (stays until first web request) ───────
  tft.fillScreen(C_DARKBG);
  tft.fillRect(0, 0, DISP_W, 4, C_ORANGE);
  if (staConnected) {
    tft.setTextColor(C_WHITE);  tft.setTextSize(2);
    tft.setCursor(12, 16);  tft.print("WiFi: joined");
    tft.setTextColor(C_MUTED);  tft.setTextSize(1);
    tft.setCursor(12, 44);  tft.print(WIFI_SSID);
    tft.setTextColor(C_WHITE);  tft.setTextSize(2);
    tft.setCursor(12, 68);  tft.print("Open browser:");
    tft.setTextColor(C_ORANGE); tft.setTextSize(2);
    tft.setCursor(12, 94);  tft.print("clawd.local");
    tft.setTextColor(C_MUTED);  tft.setTextSize(1);
    tft.setCursor(12, 124); tft.print(WiFi.localIP().toString());
    tft.setCursor(12, 140); tft.print("press any button to start");
  } else {
    tft.setTextColor(C_WHITE);  tft.setTextSize(2);
    tft.setCursor(12, 16);  tft.print("WiFi: ClaWD-Mochi");
    tft.setTextColor(C_MUTED);  tft.setTextSize(1);
    tft.setCursor(12, 44);  tft.print("password: clawd1234");
    tft.setTextColor(C_WHITE);  tft.setTextSize(2);
    tft.setCursor(12, 68);  tft.print("Open browser:");
    tft.setTextColor(C_ORANGE); tft.setTextSize(2);
    tft.setCursor(12, 94);  tft.print("192.168.4.1");
    tft.setTextColor(C_MUTED);  tft.setTextSize(1);
    tft.setCursor(12, 124); tft.print("press any button to start");
  }

  // ── Register routes ────────────────────────────────────────
  server.on("/",            HTTP_GET, routeRoot);
  server.on("/cmd",         HTTP_GET, routeCmd);
  server.on("/char",        HTTP_GET, routeChar);
  server.on("/speed",       HTTP_GET, routeSpeed);
  server.on("/redraw",      HTTP_GET, routeRedraw);
  server.on("/canvas",      HTTP_GET, routeCanvas);
  server.on("/draw/clear",  HTTP_GET, routeDrawClear);
  server.on("/draw/stroke", HTTP_GET, routeDrawStroke);
  server.on("/backlight",   HTTP_GET, routeBacklight);
  server.on("/face",        HTTP_GET, routeFace);
  server.on("/meter",       HTTP_GET, routeMeter);
  server.on("/meter/view",  HTTP_GET, routeMeterView);
  server.on("/meter/cycle", HTTP_GET, routeMeterCycle);
  server.on("/stopface",    HTTP_GET, routeStopFace);
  server.on("/meter/overlay", HTTP_GET, routeMeterOverlay);
  server.on("/claude",      HTTP_GET, routeClaude);
  server.on("/state",       HTTP_GET, routeState);
  server.onNotFound(routeNotFound);
  server.begin();

  // WiFi info stays on screen — first button press triggers setView/cmd
  // which will replace it with the correct view
}

// ═════════════════════════════════════════════════════════════
//  LOOP
// ═════════════════════════════════════════════════════════════

void loop() {
  server.handleClient();
  claudeTick();
  meterCycleTick();
  settingsTick();
}
