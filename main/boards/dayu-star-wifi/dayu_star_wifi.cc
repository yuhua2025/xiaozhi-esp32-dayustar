#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "display/lcd_display.h"
#include "system_reset.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "mcp_server.h"
#include "led/single_led.h"
#include "lamp_G.h"
#include "power_save_timer.h"
#include "power_manager.h"
#include "sd_wav_player.h"

#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <driver/spi_common.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include "assets/lang_config.h"

#define TAG "dayu_star_wifi"

// 电源管理引脚（从 config.h 之外额外定义，避免与显示/音频宏冲突）
#define CHARGING_DETECT_PIN GPIO_NUM_38
#define POWER_HOLD_PIN      GPIO_NUM_21

class dayu_star_wifi : public WifiBoard {
private:
    Button boot_button_;
    Button volume_up_button_;
    Button volume_down_button_;
    LcdDisplay* display_;
    PowerSaveTimer* power_save_timer_;
    PowerManager* power_manager_;
    esp_lcd_panel_io_handle_t panel_io_ = nullptr;
    esp_lcd_panel_handle_t panel_ = nullptr;

    void InitializePowerManager() {
        power_manager_ = new PowerManager(CHARGING_DETECT_PIN);
        power_manager_->OnChargingStatusChanged([this](bool is_charging) {
            if (is_charging) {
                power_save_timer_->SetEnabled(false);
            } else {
                power_save_timer_->SetEnabled(true);
            }
        });
    }

    void InitializePowerSaveTimer() {
        // 初始化电源保持引脚（外部 LDO EN，高电平=保持供电）
        rtc_gpio_init(POWER_HOLD_PIN);
        rtc_gpio_set_direction(POWER_HOLD_PIN, RTC_GPIO_MODE_OUTPUT_ONLY);
        rtc_gpio_set_level(POWER_HOLD_PIN, 1);

        // 构造参数：cpu_max_freq=-1 不限制，空闲 60 秒黑屏，300 秒关机
        power_save_timer_ = new PowerSaveTimer(-1, 60, 300);
        power_save_timer_->OnEnterSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(true);
            GetBacklight()->SetBrightness(1);
        });
        power_save_timer_->OnExitSleepMode([this]() {
            GetDisplay()->SetPowerSaveMode(false);
            GetBacklight()->RestoreBrightness();
        });
        power_save_timer_->OnShutdownRequest([this]() {
            ESP_LOGI(TAG, "Shutting down");
            rtc_gpio_set_level(POWER_HOLD_PIN, 0); // 拉低外部电源保持，物理断电
            rtc_gpio_hold_en(POWER_HOLD_PIN);
            if (panel_ != nullptr) {
                esp_lcd_panel_disp_on_off(panel_, false);
            }
            esp_deep_sleep_start();
        });
        power_save_timer_->SetEnabled(true);
    }

    void InitializeSpi() {
        spi_bus_config_t buscfg = {};
        buscfg.mosi_io_num = DISPLAY_MOSI_PIN;
        buscfg.miso_io_num = GPIO_NUM_NC;
        buscfg.sclk_io_num = DISPLAY_CLK_PIN;
        buscfg.quadwp_io_num = GPIO_NUM_NC;
        buscfg.quadhd_io_num = GPIO_NUM_NC;
        buscfg.max_transfer_sz = DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t);
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    void InitializeLcdDisplay() {
        ESP_LOGD(TAG, "Install panel IO");
        esp_lcd_panel_io_spi_config_t io_config = {};
        io_config.cs_gpio_num = DISPLAY_CS_PIN;
        io_config.dc_gpio_num = DISPLAY_DC_PIN;
        io_config.spi_mode = DISPLAY_SPI_MODE;
        io_config.pclk_hz = 40 * 1000 * 1000;
        io_config.trans_queue_depth = 10;
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &panel_io_));

        ESP_LOGD(TAG, "Install LCD driver");
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_RST_PIN;
        panel_config.rgb_ele_order = DISPLAY_RGB_ORDER;
        panel_config.bits_per_pixel = 16;
#if defined(LCD_TYPE_ILI9341_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(panel_io_, &panel_config, &panel_));
#elif defined(LCD_TYPE_GC9A01_SERIAL)
        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(panel_io_, &panel_config, &panel_));
        gc9a01_vendor_config_t gc9107_vendor_config = {
            .init_cmds = gc9107_lcd_init_cmds,
            .init_cmds_size = sizeof(gc9107_lcd_init_cmds) / sizeof(gc9a01_lcd_init_cmd_t),
        };
#else
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(panel_io_, &panel_config, &panel_));
#endif

        esp_lcd_panel_reset(panel_);
        esp_lcd_panel_init(panel_);
        esp_lcd_panel_invert_color(panel_, DISPLAY_INVERT_COLOR);
        esp_lcd_panel_swap_xy(panel_, DISPLAY_SWAP_XY);
        esp_lcd_panel_mirror(panel_, DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y);
#ifdef LCD_TYPE_GC9A01_SERIAL
        panel_config.vendor_config = &gc9107_vendor_config;
#endif
        display_ = new SpiLcdDisplay(panel_io_, panel_,
                                    DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                    DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y,
                                    DISPLAY_MIRROR_X, DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });

        volume_up_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() + 10;
            if (volume > 100) {
                volume = 100;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });

        volume_down_button_.OnClick([this]() {
            power_save_timer_->WakeUp();
            auto codec = GetAudioCodec();
            auto volume = codec->output_volume() - 10;
            if (volume < 0) {
                volume = 0;
            }
            codec->SetOutputVolume(volume);
            GetDisplay()->ShowNotification(Lang::Strings::VOLUME + std::to_string(volume));
        });
    }

    void InitializeTools() {
        static LED_Lamp lamp_G(GPIO_NUM_47);

        // SD card playback is controlled by the AI through MCP tools (no buttons).
        auto& mcp_server = McpServer::GetInstance();
        mcp_server.AddTool("self.sd_player.list",
                           "List every MP3 song on the SD card, with its number and title.",
                           PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               auto& player = SdWavPlayer::GetInstance();
                               if (!player.IsMounted()) {
                                   return std::string("SD card is not mounted");
                               }
                               const auto& tracks = player.BuildPlaylist(TF_MOUNT_POINT);
                               if (tracks.empty()) {
                                   return std::string("No MP3 files found on the SD card");
                               }
                               std::string out;
                               for (size_t i = 0; i < tracks.size(); i++) {
                                   std::string title = player.TrackTitle(i);
                                   if (title.empty()) {
                                       title = "Unknown";
                                   }
                                   out += std::to_string(i + 1) + ". " + title + "\n";
                               }
                               return out;
                           });

        mcp_server.AddTool("self.sd_player.play",
                           "Play a song from the SD card. 'track' can be the song number "
                           "returned by self.sd_player.list, a part of the song title, or "
                           "empty to play the next song.",
                           PropertyList({Property("track", kPropertyTypeString, std::string())}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               auto& player = SdWavPlayer::GetInstance();
                               if (!player.IsMounted()) {
                                   return std::string("SD card is not mounted");
                               }
                               auto codec = GetAudioCodec();
                               std::string track = properties["track"].value<std::string>();
                               while (!track.empty() && track.front() == ' ') {
                                   track.erase(track.begin());
                               }
                               while (!track.empty() && track.back() == ' ') {
                                   track.pop_back();
                               }

                               bool ok = false;
                               const auto& tracks = player.BuildPlaylist(TF_MOUNT_POINT);
                               if (track.empty()) {
                                   ok = player.PlayNext(TF_MOUNT_POINT, codec);
                               } else if (!track.empty() &&
                                          track.find_first_not_of("0123456789") == std::string::npos) {
                                   size_t index = (size_t)std::stoi(track);
                                   ok = index > 0 && player.PlayIndex(index - 1, TF_MOUNT_POINT, codec);
                               } else {
                                   for (size_t i = 0; i < tracks.size() && !ok; i++) {
                                       std::string title = player.TrackTitle(i);
                                       if (!title.empty() && title.find(track) != std::string::npos) {
                                           ok = player.PlayIndex(i, TF_MOUNT_POINT, codec);
                                       }
                                   }
                               }
                               if (!ok) {
                                   return std::string("Song not found or cannot be played");
                               }
                               vTaskDelay(pdMS_TO_TICKS(150));  // let the task parse the title
                               return std::string("Playing: ") + player.CurrentTitle();
                           });

        mcp_server.AddTool("self.sd_player.next", "Play the next song on the SD card.",
                           PropertyList(),
                           [this](const PropertyList&) -> ReturnValue {
                               auto& player = SdWavPlayer::GetInstance();
                               if (!player.IsMounted()) {
                                   return std::string("SD card is not mounted");
                               }
                               if (!player.PlayNext(TF_MOUNT_POINT, GetAudioCodec())) {
                                   return std::string("No song to play");
                               }
                               vTaskDelay(pdMS_TO_TICKS(150));
                               return std::string("Playing: ") + player.CurrentTitle();
                           });

        mcp_server.AddTool("self.sd_player.stop", "Stop SD card playback.", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               SdWavPlayer::GetInstance().Stop();
                               return true;
                           });

        mcp_server.AddTool("self.sd_player.status",
                           "Get the current SD card playback status.", PropertyList(),
                           [](const PropertyList&) -> ReturnValue {
                               auto& player = SdWavPlayer::GetInstance();
                               if (!player.IsPlaying()) {
                                   return std::string("Not playing");
                               }
                               int elapsed = player.ElapsedSeconds();
                               int duration = player.DurationSeconds();
                               char buf[192];
                               if (duration > 0) {
                                   snprintf(buf, sizeof(buf), "Playing %d/%d %s: %d:%02d/%d:%02d",
                                            player.CurrentTrack(), player.TrackCount(),
                                            player.CurrentTitle().c_str(), elapsed / 60,
                                            elapsed % 60, duration / 60, duration % 60);
                               } else {
                                   snprintf(buf, sizeof(buf), "Playing %d/%d %s: %d:%02d",
                                            player.CurrentTrack(), player.TrackCount(),
                                            player.CurrentTitle().c_str(), elapsed / 60,
                                            elapsed % 60);
                               }
                               return std::string(buf);
                           });
    }

public:
    dayu_star_wifi() :
        boot_button_(BOOT_BUTTON_GPIO, false, 2000),
        volume_up_button_(VOLUME_UP_BUTTON_GPIO, false, 2000),
        volume_down_button_(VOLUME_DOWN_BUTTON_GPIO, false, 2000) {
        InitializePowerManager();
        InitializePowerSaveTimer();
        InitializeSpi();
        InitializeButtons();
        InitializeLcdDisplay();
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }
        InitializeTools();
        // Mount SD card
        SdWavPlayer::GetInstance().Mount();

        // 1 Hz playback progress updater for the bottom bar
        // (a dedicated task: the esp_timer task stack is too small for LVGL text)
        xTaskCreate([](void* arg) {
            auto* self = static_cast<dayu_star_wifi*>(arg);
            auto& app = Application::GetInstance();
            auto& player = SdWavPlayer::GetInstance();
            int paused_seconds = 0;
            while (true) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                // Music has been waiting for a conversation that is not going
                // anywhere: end the idle listening session so playback resumes.
                if (player.IsPaused() && app.GetDeviceState() == kDeviceStateListening &&
                    !app.IsVoiceDetected()) {
                    if (++paused_seconds >= 2) {
                        paused_seconds = 0;
                        ESP_LOGI(TAG, "Music waiting, leaving listening state");
                        app.Schedule([]() { Application::GetInstance().SetDeviceState(kDeviceStateIdle); });
                    }
                } else {
                    paused_seconds = 0;
                }
                self->UpdatePlaybackStatus();
            }
        }, "mp3_status", 4096, this, 2, nullptr);
    }

    virtual Led* GetLed() override {
        static SingleLed led(BUILTIN_LED_GPIO);
        return &led;
    }

    virtual AudioCodec* GetAudioCodec() override {
#ifdef AUDIO_I2S_METHOD_SIMPLEX
        static NoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK, AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK, AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
#else
        static NoAudioCodecDuplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS, AUDIO_I2S_GPIO_DOUT,
            AUDIO_I2S_GPIO_DIN);
#endif
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }

    virtual Backlight* GetBacklight() override {
        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
            return &backlight;
        }
        return nullptr;
    }

    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        static bool last_discharging = false;
        charging = power_manager_->IsCharging();
        discharging = power_manager_->IsDischarging();
        if (discharging != last_discharging) {
            power_save_timer_->SetEnabled(discharging);
            last_discharging = discharging;
        }
        level = power_manager_->GetBatteryLevel();
        return true;
    }

    virtual void SetPowerSaveLevel(PowerSaveLevel level) override {
        if (level != PowerSaveLevel::LOW_POWER) {
            power_save_timer_->WakeUp();
        }
        WifiBoard::SetPowerSaveLevel(level);
    }

    static std::string FormatMmss(int seconds) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%d:%02d", seconds / 60, seconds % 60);
        return buf;
    }

    // Two-line playback info: "1:23/4:45" then "1/8  title"
    std::string BuildPlaybackText() {
        auto& player = SdWavPlayer::GetInstance();
        std::string name = player.CurrentTitle();
        if (name.empty()) {
            name = "曲目";
        }
        std::string text = FormatMmss(player.ElapsedSeconds());
        int duration = player.DurationSeconds();
        if (duration > 0) {
            text += "/" + FormatMmss(duration);
        }
        text += "\n" + std::to_string(player.CurrentTrack()) + "/" +
                std::to_string(player.TrackCount()) + "  " + name;
        return text;
    }

    // Refresh the bottom bar while playing
    void UpdatePlaybackStatus() {
        auto& player = SdWavPlayer::GetInstance();
        if (!player.IsPlaying()) {
            return;
        }
        GetDisplay()->SetChatMessage("assistant", BuildPlaybackText().c_str());
    }
};

DECLARE_BOARD(dayu_star_wifi);
