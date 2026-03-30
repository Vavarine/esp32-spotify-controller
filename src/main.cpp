#include <Arduino.h>
#include <SPI.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <img_converters.h>
#include "SpotifyEsp32.h"
#include "secrets.h"

// ===================== PINS =====================
constexpr uint8_t PREVIOUS_BUTTON_PIN = 13;
constexpr uint8_t PLAY_PAUSE_BUTTON_PIN = 12;
constexpr uint8_t NEXT_BUTTON_PIN = 14;
constexpr uint8_t LED_PIN = 2;

constexpr uint8_t TFT_SCLK_PIN = 18;
constexpr uint8_t TFT_MOSI_PIN = 23;
constexpr uint8_t TFT_CS_PIN = 19;
constexpr uint8_t TFT_DC_PIN = 22;
constexpr uint8_t TFT_RST_PIN = 21;

// ===================== CONFIG =====================
constexpr unsigned long BUTTON_DEBOUNCE_MS = 25;
constexpr unsigned long BUTTON_STARTUP_IGNORE_MS = 750;
constexpr unsigned long PLAYER_QUEUE_WAIT_MS = 75;
constexpr unsigned long SPOTIFY_ACTION_LOCK_WAIT_MS = 1500;
constexpr unsigned long SPOTIFY_FETCH_LOCK_WAIT_MS = 25;
constexpr unsigned long FETCH_INTERVAL_MS = 1000;
constexpr unsigned long UI_IDLE_REFRESH_MS = 250;
constexpr unsigned long UI_FEEDBACK_DURATION_MS = 1200;
constexpr unsigned long OPTIMISTIC_PLAYBACK_WINDOW_MS = 1500;
constexpr unsigned long PLAY_PAUSE_GUARD_MS = 350;
constexpr unsigned long ALBUM_ART_REFRESH_INTERVAL_MS = 10000;
constexpr uint32_t TFT_SPI_FREQUENCY = 40000000;
constexpr bool INVERT_DISPLAY_COLORS = false;

constexpr uint16_t SCREEN_BG_COLOR = ST77XX_BLACK;
constexpr uint16_t STATUS_TEXT_COLOR = ST77XX_GREEN;
constexpr uint16_t TRACK_TEXT_COLOR = ST77XX_WHITE;
constexpr uint16_t ARTIST_TEXT_COLOR = ST77XX_YELLOW;
constexpr uint16_t ALBUM_BORDER_COLOR = ST77XX_WHITE;

constexpr BaseType_t UI_TASK_CORE = 0;
constexpr BaseType_t APP_TASK_CORE = 1;

constexpr int ALBUM_ART_SIZE = 64;
constexpr int ALBUM_ART_SCALE = 2;
constexpr int ALBUM_ART_DRAW_SIZE = ALBUM_ART_SIZE * ALBUM_ART_SCALE;
constexpr int STATUS_Y = 10;
constexpr int ALBUM_ART_Y = 44;
constexpr int TRACK_Y = 186;
constexpr int ARTIST_Y = 214;

// ===================== STRUCTS =====================
struct ButtonState {
  uint8_t pin;
  bool stablePressed;
  bool lastRawPressed;
  unsigned long lastChangeMs;
  bool initialized;
};

enum class PlayerAction : uint8_t {
  Previous,
  PlayPause,
  Next,
};

struct SpotifyState {
  String track;
  String artist;
  String albumImageUrl;
  bool playing;
  bool valid;
};

struct UiFeedback {
  String message;
  bool active;
  unsigned long expiresAtMs;
};

struct OptimisticPlaybackState {
  bool active;
  bool playing;
  unsigned long expiresAtMs;
};

struct AlbumArtState {
  String currentUrl;
  String pendingUrl;
  bool hasImage;
  bool loading;
  bool dirty;
};

// ===================== GLOBAL =====================
Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
Spotify sp(SPOTIFY_CLIENT_ID, SPOTIFY_CLIENT_SECRET, SPOTIFY_REFRESH_TOKEN, 1);

QueueHandle_t playerQueue;
SemaphoreHandle_t spotifyMutex;
SemaphoreHandle_t displayMutex;
SemaphoreHandle_t stateMutex;
TaskHandle_t spotifyFetchTaskHandle = nullptr;
TaskHandle_t albumArtTaskHandle = nullptr;
TaskHandle_t uiTaskHandle = nullptr;

SpotifyState globalState;
UiFeedback globalUiFeedback;
OptimisticPlaybackState globalOptimisticPlayback;
AlbumArtState globalAlbumArt;
uint16_t globalAlbumArtPixels[ALBUM_ART_SIZE * ALBUM_ART_SIZE];
unsigned long buttonInputReadyMs = 0;
bool playPauseInFlight = false;
unsigned long playPauseGuardUntilMs = 0;

// ===================== HELPERS =====================
void lock_spotify() { xSemaphoreTake(spotifyMutex, portMAX_DELAY); }
void unlock_spotify() { xSemaphoreGive(spotifyMutex); }
bool try_lock_spotify(TickType_t timeoutTicks) {
  return xSemaphoreTake(spotifyMutex, timeoutTicks) == pdTRUE;
}

void lock_display() { xSemaphoreTake(displayMutex, portMAX_DELAY); }
void unlock_display() { xSemaphoreGive(displayMutex); }

void request_ui_refresh() {
  if (uiTaskHandle != nullptr) {
    xTaskNotifyGive(uiTaskHandle);
  }
}

void store_spotify_state(const SpotifyState& state) {
  bool changed = false;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  SpotifyState mergedState = state;
  const unsigned long now = millis();

  if (globalOptimisticPlayback.active) {
    if (now < globalOptimisticPlayback.expiresAtMs) {
      if (state.playing != globalOptimisticPlayback.playing) {
        mergedState.playing = globalOptimisticPlayback.playing;
      } else {
        globalOptimisticPlayback.active = false;
      }
    } else {
      globalOptimisticPlayback.active = false;
    }
  }

  changed =
      globalState.track != mergedState.track ||
      globalState.artist != mergedState.artist ||
      globalState.albumImageUrl != mergedState.albumImageUrl ||
      globalState.playing != mergedState.playing ||
      globalState.valid != mergedState.valid;

  globalState = mergedState;
  xSemaphoreGive(stateMutex);

  if (changed) {
    request_ui_refresh();
  }
}

void set_ui_feedback(const String& message) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  globalUiFeedback.message = message;
  globalUiFeedback.active = true;
  globalUiFeedback.expiresAtMs = millis() + UI_FEEDBACK_DURATION_MS;
  xSemaphoreGive(stateMutex);

  request_ui_refresh();
}

void request_album_art_refresh() {
  if (albumArtTaskHandle != nullptr) {
    xTaskNotifyGive(albumArtTaskHandle);
  }
}

SpotifyState fetch_spotify_state() {
  SpotifyState newState;
  JsonDocument filter;
  filter["is_playing"] = true;
  filter["item"]["name"] = true;
  JsonObject image = filter["item"]["album"]["images"].add<JsonObject>();
  image["url"] = true;
  JsonObject artist = filter["item"]["artists"].add<JsonObject>();
  artist["name"] = true;

  if (!try_lock_spotify(pdMS_TO_TICKS(SPOTIFY_FETCH_LOCK_WAIT_MS))) {
    return newState;
  }

  response data = sp.get_currently_playing_track(filter);
  unlock_spotify();

  if (data.status_code >= 200 && data.status_code < 300 && !data.reply.isNull()) {
    newState.playing = data.reply["is_playing"].as<bool>();
    newState.track = data.reply["item"]["name"].as<String>();

    JsonArray artists = data.reply["item"]["artists"].as<JsonArray>();
    newState.artist = "";
    for (JsonVariant artistEntry : artists) {
      if (!newState.artist.isEmpty()) {
        newState.artist += ", ";
      }
      newState.artist += artistEntry["name"].as<String>();
    }

    JsonArray images = data.reply["item"]["album"]["images"].as<JsonArray>();
    if (!images.isNull() && images.size() > 0) {
      const size_t preferredIndex = images.size() > 2 ? 2 : images.size() - 1;
      newState.albumImageUrl = images[preferredIndex]["url"].as<String>();
    }
  }

  newState.valid =
      !(newState.track.isEmpty() || newState.track == "null");

  return newState;
}

bool current_cached_playing() {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  const bool playing = globalState.playing;
  xSemaphoreGive(stateMutex);
  return playing;
}

void set_optimistic_playing(bool playing) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  globalState.playing = playing;
  globalOptimisticPlayback.active = true;
  globalOptimisticPlayback.playing = playing;
  globalOptimisticPlayback.expiresAtMs = millis() + OPTIMISTIC_PLAYBACK_WINDOW_MS;
  xSemaphoreGive(stateMutex);

  request_ui_refresh();
}

void update_pending_album_art_url(const String& url) {
  bool shouldRefreshUi = false;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (url.isEmpty()) {
    if (globalAlbumArt.hasImage || !globalAlbumArt.currentUrl.isEmpty()) {
      globalAlbumArt.currentUrl = "";
      globalAlbumArt.pendingUrl = "";
      globalAlbumArt.hasImage = false;
      globalAlbumArt.loading = false;
      globalAlbumArt.dirty = true;
      shouldRefreshUi = true;
    }
  } else if (url != globalAlbumArt.currentUrl && url != globalAlbumArt.pendingUrl) {
    globalAlbumArt.pendingUrl = url;
    globalAlbumArt.loading = !url.isEmpty();
  }
  xSemaphoreGive(stateMutex);

  if (shouldRefreshUi) {
    request_ui_refresh();
  }

  if (!url.isEmpty()) {
    request_album_art_refresh();
  }
}

void request_spotify_refresh() {
  if (spotifyFetchTaskHandle != nullptr) {
    xTaskNotifyGive(spotifyFetchTaskHandle);
  }
}

bool try_enqueue_player_action(PlayerAction action, unsigned long now) {
  bool reservedPlayPause = false;

  if (action == PlayerAction::PlayPause) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);

    if (playPauseInFlight || now < playPauseGuardUntilMs) {
      xSemaphoreGive(stateMutex);
      return false;
    }

    playPauseInFlight = true;
    playPauseGuardUntilMs = now + PLAY_PAUSE_GUARD_MS;
    reservedPlayPause = true;
    xSemaphoreGive(stateMutex);
  }

  if (xQueueSend(playerQueue, &action, pdMS_TO_TICKS(PLAYER_QUEUE_WAIT_MS)) == pdTRUE) {
    return true;
  }

  if (reservedPlayPause) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    playPauseInFlight = false;
    xSemaphoreGive(stateMutex);
  }

  return false;
}

void sync_button(ButtonState& button, unsigned long now) {
  const bool rawPressed = digitalRead(button.pin) == LOW;
  button.stablePressed = rawPressed;
  button.lastRawPressed = rawPressed;
  button.lastChangeMs = now;
  button.initialized = true;
}

bool update_button(ButtonState& button, unsigned long now) {
  if (!button.initialized) {
    sync_button(button, now);
    return false;
  }

  bool raw = digitalRead(button.pin) == LOW;

  if (raw != button.lastRawPressed) {
    button.lastRawPressed = raw;
    button.lastChangeMs = now;
  }

  if ((now - button.lastChangeMs) >= BUTTON_DEBOUNCE_MS &&
      button.stablePressed != raw) {
    button.stablePressed = raw;
    return button.stablePressed;
  }

  return false;
}

// ===================== WIFI =====================
void connect_wifi() {
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting WiFi");

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  Serial.println("\nConnected!");
  WiFi.setSleep(false);
}

// ===================== DISPLAY =====================
void draw_scaled_album_art(int x, int y) {
  for (int row = 0; row < ALBUM_ART_SIZE; ++row) {
    for (int col = 0; col < ALBUM_ART_SIZE; ++col) {
      const uint16_t color = globalAlbumArtPixels[row * ALBUM_ART_SIZE + col];
      tft.fillRect(
        x + (col * ALBUM_ART_SCALE),
        y + (row * ALBUM_ART_SCALE),
        ALBUM_ART_SCALE,
        ALBUM_ART_SCALE,
        color
      );
    }
  }
}

void draw_track_if_changed() {
  static String lastTrack;
  static String lastArtist;
  static bool lastPlaying = false;
  static String lastFeedbackMessage;
  static bool lastFeedbackActive = false;
  static bool lastAlbumHasImage = false;
  static bool initialized = false;

  SpotifyState state;
  UiFeedback feedback;
  AlbumArtState albumArt;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state = globalState;
  feedback = globalUiFeedback;
  albumArt = globalAlbumArt;
  if (feedback.active && millis() >= feedback.expiresAtMs) {
    globalUiFeedback.active = false;
    feedback.active = false;
  }
  xSemaphoreGive(stateMutex);

  if (!state.valid) return;

  bool changed =
      !initialized ||
      state.track != lastTrack ||
      state.artist != lastArtist ||
      state.playing != lastPlaying ||
      feedback.message != lastFeedbackMessage ||
      feedback.active != lastFeedbackActive ||
      albumArt.dirty ||
      albumArt.hasImage != lastAlbumHasImage;

  if (!changed) return;

  lock_display();

  const int screenWidth = tft.width();
  const int albumArtX = (screenWidth - ALBUM_ART_DRAW_SIZE) / 2;

  if (!initialized) {
    tft.fillScreen(SCREEN_BG_COLOR);
  }

  tft.setTextWrap(false);

  if (!initialized ||
      feedback.message != lastFeedbackMessage ||
      feedback.active != lastFeedbackActive ||
      state.playing != lastPlaying) {
    tft.fillRect(0, 0, screenWidth, 40, SCREEN_BG_COLOR);
    tft.setCursor(12, STATUS_Y);
    tft.setTextColor(STATUS_TEXT_COLOR, SCREEN_BG_COLOR);
    tft.setTextSize(2);
    tft.print(feedback.active ? feedback.message : (state.playing ? "Tocando" : "Pausado"));
  }

  if (!initialized || albumArt.dirty || albumArt.hasImage != lastAlbumHasImage) {
    tft.fillRect(
      albumArtX - 2,
      ALBUM_ART_Y - 2,
      ALBUM_ART_DRAW_SIZE + 4,
      ALBUM_ART_DRAW_SIZE + 4,
      SCREEN_BG_COLOR
    );
    tft.drawRect(
      albumArtX - 1,
      ALBUM_ART_Y - 1,
      ALBUM_ART_DRAW_SIZE + 2,
      ALBUM_ART_DRAW_SIZE + 2,
      ALBUM_BORDER_COLOR
    );

    if (albumArt.hasImage) {
      draw_scaled_album_art(albumArtX, ALBUM_ART_Y);
    } else {
      tft.fillRect(albumArtX, ALBUM_ART_Y, ALBUM_ART_DRAW_SIZE, ALBUM_ART_DRAW_SIZE, SCREEN_BG_COLOR);
      tft.setCursor(albumArtX + 40, ALBUM_ART_Y + 60);
      tft.setTextColor(TRACK_TEXT_COLOR, SCREEN_BG_COLOR);
      tft.setTextSize(2);
      tft.print("...");
    }
  }

  if (!initialized || state.track != lastTrack) {
    tft.fillRect(0, 176, screenWidth, 26, SCREEN_BG_COLOR);
    tft.setCursor(12, TRACK_Y);
    tft.setTextColor(TRACK_TEXT_COLOR, SCREEN_BG_COLOR);
    tft.setTextSize(2);
    tft.print(state.track);
  }

  if (!initialized || state.artist != lastArtist) {
    tft.fillRect(0, 204, screenWidth, 26, SCREEN_BG_COLOR);
    tft.setCursor(12, ARTIST_Y);
    tft.setTextColor(ARTIST_TEXT_COLOR, SCREEN_BG_COLOR);
    tft.setTextSize(2);
    tft.print(state.artist);
  }

  unlock_display();

  initialized = true;

  lastTrack = state.track;
  lastArtist = state.artist;
  lastPlaying = state.playing;
  lastFeedbackMessage = feedback.message;
  lastFeedbackActive = feedback.active;
  lastAlbumHasImage = albumArt.hasImage;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  globalAlbumArt.dirty = false;
  xSemaphoreGive(stateMutex);
}

bool download_album_art_jpeg(const String& url, uint8_t*& jpegData, size_t& jpegSize) {
  jpegData = nullptr;
  jpegSize = 0;

  if (url.isEmpty()) return false;

  WiFiClientSecure client;
  client.setInsecure();

  HTTPClient http;
  if (!http.begin(client, url)) {
    return false;
  }

  const int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  WiFiClient* stream = http.getStreamPtr();
  int contentLength = http.getSize();
  size_t capacity = contentLength > 0 ? static_cast<size_t>(contentLength) : 4096;
  jpegData = static_cast<uint8_t*>(malloc(capacity));
  if (jpegData == nullptr) {
    http.end();
    return false;
  }

  while (http.connected() && (contentLength > 0 || stream->available())) {
    const size_t available = stream->available();
    if (available == 0) {
      delay(1);
      continue;
    }

    if (jpegSize + available > capacity) {
      size_t newCapacity = capacity;
      while (jpegSize + available > newCapacity) {
        newCapacity *= 2;
      }

      uint8_t* resized = static_cast<uint8_t*>(realloc(jpegData, newCapacity));
      if (resized == nullptr) {
        free(jpegData);
        jpegData = nullptr;
        http.end();
        return false;
      }

      jpegData = resized;
      capacity = newCapacity;
    }

    const int bytesRead = stream->readBytes(jpegData + jpegSize, available);
    if (bytesRead <= 0) {
      break;
    }

    jpegSize += static_cast<size_t>(bytesRead);
    if (contentLength > 0) {
      contentLength -= bytesRead;
    }
  }

  http.end();
  return jpegSize > 0;
}

bool decode_album_art(uint8_t* jpegData, size_t jpegSize, uint16_t* outPixels) {
  return jpg2rgb565(jpegData, jpegSize, reinterpret_cast<uint8_t*>(outPixels), JPG_SCALE_NONE);
}

void load_album_art() {
  String pendingUrl;

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  pendingUrl = globalAlbumArt.pendingUrl;
  xSemaphoreGive(stateMutex);

  if (pendingUrl.isEmpty()) {
    return;
  }

  uint8_t* jpegData = nullptr;
  size_t jpegSize = 0;
  bool hasImage = false;

  if (download_album_art_jpeg(pendingUrl, jpegData, jpegSize)) {
    hasImage = decode_album_art(jpegData, jpegSize, globalAlbumArtPixels);
  }

  if (jpegData != nullptr) {
    free(jpegData);
  }

  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (pendingUrl == globalAlbumArt.pendingUrl) {
    globalAlbumArt.currentUrl = hasImage ? pendingUrl : "";
    globalAlbumArt.pendingUrl = "";
    globalAlbumArt.hasImage = hasImage;
    globalAlbumArt.loading = false;
    globalAlbumArt.dirty = true;
  }
  xSemaphoreGive(stateMutex);

  request_ui_refresh();
}

// ===================== TASKS =====================
void spotify_fetch_task(void*) {
  for (;;) {
    SpotifyState state = fetch_spotify_state();
    store_spotify_state(state);
    update_pending_album_art_url(state.albumImageUrl);
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(FETCH_INTERVAL_MS));
  }
}

void ui_task(void*) {
  for (;;) {
    draw_track_if_changed();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(UI_IDLE_REFRESH_MS));
  }
}

void album_art_task(void*) {
  for (;;) {
    load_album_art();
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ALBUM_ART_REFRESH_INTERVAL_MS));
  }
}

void player_task(void*) {
  PlayerAction action;

  for (;;) {
    if (xQueueReceive(playerQueue, &action, portMAX_DELAY)) {
      if (!try_lock_spotify(pdMS_TO_TICKS(SPOTIFY_ACTION_LOCK_WAIT_MS))) {
        if (action == PlayerAction::PlayPause) {
          xSemaphoreTake(stateMutex, portMAX_DELAY);
          playPauseInFlight = false;
          xSemaphoreGive(stateMutex);
        }

        set_ui_feedback("Tente novamente");
        continue;
      }

      switch (action) {
        case PlayerAction::Previous:
          set_ui_feedback("Voltando...");
          sp.skip_to_previous();
          break;

        case PlayerAction::Next:
          set_ui_feedback("Pulando...");
          sp.skip_to_next();
          break;

        case PlayerAction::PlayPause: {
          const bool playing = current_cached_playing();

          if (playing) {
            set_optimistic_playing(false);
            set_ui_feedback("Pausando...");
            sp.pause_playback();
          } else {
            set_optimistic_playing(true);
            set_ui_feedback("Tocando...");
            sp.start_a_users_playback();
          }
          break;
        }
      }

      unlock_spotify();

      if (action == PlayerAction::PlayPause) {
        xSemaphoreTake(stateMutex, portMAX_DELAY);
        playPauseInFlight = false;
        xSemaphoreGive(stateMutex);
      }

      request_spotify_refresh();
    }
  }
}

// ===================== SETUP =====================
void setup() {
  Serial.begin(115200);

  spotifyMutex = xSemaphoreCreateMutex();
  displayMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();

  pinMode(PREVIOUS_BUTTON_PIN, INPUT_PULLUP);
  pinMode(PLAY_PAUSE_BUTTON_PIN, INPUT_PULLUP);
  pinMode(NEXT_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_PIN, OUTPUT);

  SPI.begin(TFT_SCLK_PIN, -1, TFT_MOSI_PIN, TFT_CS_PIN);
  tft.init(240, 320);
  tft.setSPISpeed(TFT_SPI_FREQUENCY);
  tft.setRotation(1);
  tft.invertDisplay(INVERT_DISPLAY_COLORS);

  buttonInputReadyMs = millis() + BUTTON_STARTUP_IGNORE_MS;

  connect_wifi();

  lock_spotify();
  sp.set_scopes("user-read-playback-state user-read-currently-playing user-modify-playback-state");
  sp.begin();
  unlock_spotify();

  // AUTH
  while (true) {
    lock_spotify();
    bool auth = sp.is_auth();
    if (!auth) sp.handle_client();
    unlock_spotify();

    if (auth) break;

    delay(100);
  }

  playerQueue = xQueueCreate(8, sizeof(PlayerAction));

  xTaskCreatePinnedToCore(player_task, "player", 6144, nullptr, 3, nullptr, APP_TASK_CORE);
  xTaskCreatePinnedToCore(
    spotify_fetch_task,
    "fetch",
    8192,
    nullptr,
    1,
    &spotifyFetchTaskHandle,
    APP_TASK_CORE
  );
  xTaskCreatePinnedToCore(
    album_art_task,
    "album_art",
    8192,
    nullptr,
    1,
    &albumArtTaskHandle,
    APP_TASK_CORE
  );
  xTaskCreatePinnedToCore(ui_task, "ui", 6144, nullptr, 2, &uiTaskHandle, UI_TASK_CORE);

  request_spotify_refresh();
}

// ===================== LOOP =====================
void loop() {
  static ButtonState prev{PREVIOUS_BUTTON_PIN, false, false, 0, false};
  static ButtonState play{PLAY_PAUSE_BUTTON_PIN, false, false, 0, false};
  static ButtonState next{NEXT_BUTTON_PIN, false, false, 0, false};

  unsigned long now = millis();

  if (now < buttonInputReadyMs) {
    sync_button(prev, now);
    sync_button(play, now);
    sync_button(next, now);

    digitalWrite(
      LED_PIN,
      prev.stablePressed || play.stablePressed || next.stablePressed
    );

    vTaskDelay(pdMS_TO_TICKS(5));
    return;
  }

  if (update_button(prev, now)) {
    try_enqueue_player_action(PlayerAction::Previous, now);
  }

  if (update_button(play, now)) {
    try_enqueue_player_action(PlayerAction::PlayPause, now);
  }

  if (update_button(next, now)) {
    try_enqueue_player_action(PlayerAction::Next, now);
  }

  digitalWrite(
    LED_PIN,
    prev.stablePressed || play.stablePressed || next.stablePressed
  );

  vTaskDelay(pdMS_TO_TICKS(5));
}
