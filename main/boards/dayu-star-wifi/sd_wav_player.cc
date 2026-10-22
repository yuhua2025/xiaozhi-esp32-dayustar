#include "sd_wav_player.h"
#include "config.h"
#include "application.h"
#include "audio/audio_codec.h"
#include "audio/audio_service.h"

#include <esp_log.h>
#include <esp_vfs_fat.h>
#include <driver/sdspi_host.h>
#include <driver/sdmmc_types.h>
#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "simple_dec/esp_audio_simple_dec.h"
#include "simple_dec/esp_audio_simple_dec_reg.h"
#include "decoder/esp_audio_dec.h"
#include "decoder/impl/esp_mp3_dec.h"
#include "esp_ae_rate_cvt.h"
#include "esp_audio_types.h"

#include <dirent.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstring>
#include <cstdio>
#include <algorithm>
#include <vector>

#define TAG "SdWavPlayer"
#define SD_SPI_HOST SPI2_HOST  // LCD uses SPI3_HOST — no conflict

static constexpr uint32_t kMp3ChunkBytes = 16 * 1024;   // larger read-ahead from SD
static constexpr uint32_t kPcmBufSamples  = 8192;
static constexpr uint32_t kMonoBlockSamples = 4096;     // I2S write granularity (~170 ms @24k)

// ———————————————————— MP3 frame parser ————————————————————
// The simple decoder has no MP3 parser registered, so it must be fed whole frames.
// Splitting frames across read chunks makes the decoder log "Not supported format"
// and drop audio (audible stutter).
static const int kBitrateV1L3[16] = {0, 32, 40, 48, 56, 64, 80, 96,
                                     112, 128, 160, 192, 224, 256, 320, 0};
static const int kBitrateV2L3[16] = {0, 8, 16, 24, 32, 40, 48, 56,
                                     64, 80, 96, 112, 128, 144, 160, 0};
static const int kSampleRateV1[3]  = {44100, 48000, 32000};
static const int kSampleRateV2[3]  = {22050, 24000, 16000};
static const int kSampleRateV25[3] = {11025, 12000, 8000};

// Returns the byte length of the MPEG Layer-3 frame at p, or 0 if it is not a valid header
static int Mp3FrameLength(const uint8_t* p, size_t avail) {
    if (avail < 4) {
        return 0;
    }
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) {
        return 0;
    }
    int version = (p[1] >> 3) & 0x3;   // 0 = MPEG2.5, 2 = MPEG2, 3 = MPEG1
    int layer   = (p[1] >> 1) & 0x3;   // 1 = Layer III
    if (version == 1 || layer != 1) {
        return 0;
    }
    int bitrate_idx = (p[2] >> 4) & 0xF;
    int sr_idx      = (p[2] >> 2) & 0x3;
    int padding     = (p[2] >> 1) & 0x1;
    if (bitrate_idx == 0 || bitrate_idx == 15 || sr_idx == 3) {
        return 0;
    }
    int sample_rate = (version == 3) ? kSampleRateV1[sr_idx]
                      : (version == 2 ? kSampleRateV2[sr_idx] : kSampleRateV25[sr_idx]);
    int bitrate = (version == 3 ? kBitrateV1L3[bitrate_idx] : kBitrateV2L3[bitrate_idx]) * 1000;
    if (sample_rate == 0 || bitrate == 0) {
        return 0;
    }
    int len = (version == 3) ? (144 * bitrate) / sample_rate + padding
                             : (72 * bitrate) / sample_rate + padding;
    return (len > 4) ? len : 0;
}

// ———————————————————— Singleton ————————————————————
SdWavPlayer& SdWavPlayer::GetInstance() {
    static SdWavPlayer inst;
    return inst;
}

// ———————————————————— Mount ————————————————————
esp_err_t SdWavPlayer::Mount() {
    if (mounted_) {
        ESP_LOGI(TAG, "SD already mounted");
        return ESP_OK;
    }

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = TF_MOSI_PIN,
        .miso_io_num = TF_MISO_PIN,
        .sclk_io_num = TF_CLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4096,
    };
    esp_err_t err = spi_bus_initialize(SD_SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPI bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 20000;

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.host_id = SD_SPI_HOST;
    slot_cfg.gpio_cs = TF_CS_PIN;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024,
    };

    sdmmc_card_t* card = nullptr;
    err = esp_vfs_fat_sdspi_mount(TF_MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD mount failed: %s", esp_err_to_name(err));
        spi_bus_free(SD_SPI_HOST);
        return err;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "SD mounted at %s", TF_MOUNT_POINT);
    return ESP_OK;
}

// ———————————————————— Playlist (recursive scan) ————————————————————
// Some FAT 8.3 names cannot be resolved back through the UTF-8 VFS layer, so only
// keep files we can actually open.
static bool FilePlayable(const std::string& path, long* size) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) {
        return false;
    }
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    fclose(f);
    if (size) {
        *size = (long)st.st_size;
    }
    return true;
}

static void CollectAudio(const std::string& dir, std::vector<std::string>& out) {
    DIR* d = opendir(dir.c_str());
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        if (ent->d_name[0] == '.') continue;
        std::string full = dir + "/" + ent->d_name;
        if (ent->d_type == DT_DIR) {
            CollectAudio(full, out);
        } else if (ent->d_type == DT_REG) {
            const char* ext = strrchr(ent->d_name, '.');
            if (ext && strcasecmp(ext, ".mp3") == 0) {
                long size = 0;
                if (FilePlayable(full, &size) && size > 0) {
                    out.push_back(full);
                } else {
                    ESP_LOGW(TAG, "Skip unplayable file: %s (errno=%d)", full.c_str(), errno);
                }
            }
        }
    }
    closedir(d);
}

// GB2312/GBK -> Unicode table generated into gbk_table.cc
extern const uint16_t kGbkToUnicode[8178];

static void AppendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

// Many Chinese MP3 tags are GBK encoded
static std::string GbkToUtf8(const uint8_t* p, size_t len) {
    std::string out;
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == 0) {
            break;
        }
        if (c < 0x80) {
            out.push_back((char)c);
            i++;
            continue;
        }
        if (i + 1 >= len) {
            break;
        }
        uint32_t cp = 0;
        uint8_t hi = c, lo = p[i + 1];
        if (hi >= 0xA1 && hi <= 0xF7 && lo >= 0xA1 && lo <= 0xFE) {
            cp = kGbkToUnicode[(hi - 0xA1) * 94 + (lo - 0xA1)];
        }
        i += 2;
        if (cp == 0) {
            out.push_back('?');
        } else {
            AppendUtf8(out, cp);
        }
    }
    return out;
}

// Convert UTF-16 (with or without BOM) to UTF-8, BMP only
static std::string Utf16ToUtf8(const uint8_t* p, size_t len, bool big_endian) {
    std::string out;
    size_t i = 0;
    bool be = big_endian;
    if (len >= 2) {
        if (p[0] == 0xFF && p[1] == 0xFE) {
            be = false;
            i = 2;
        } else if (p[0] == 0xFE && p[1] == 0xFF) {
            be = true;
            i = 2;
        }
    }
    for (; i + 1 < len; i += 2) {
        uint32_t cp = be ? ((uint32_t)p[i] << 8 | p[i + 1]) : ((uint32_t)p[i + 1] << 8 | p[i]);
        if (cp == 0) {
            break;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

// Read the ID3v2 TIT2 (song title) frame
std::string SdWavPlayer::ReadId3Title(const std::string& path) {
    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        return "";
    }

    uint8_t hdr[10];
    if (fread(hdr, 1, 10, fp) != 10 || memcmp(hdr, "ID3", 3) != 0) {
        fclose(fp);
        return "";
    }
    int version = hdr[3];
    uint32_t tag_size = ((uint32_t)(hdr[6] & 0x7F) << 21) | ((uint32_t)(hdr[7] & 0x7F) << 14) |
                        ((uint32_t)(hdr[8] & 0x7F) << 7) | (uint32_t)(hdr[9] & 0x7F);

    std::string title;
    uint32_t consumed = 0;
    while (consumed + 10 <= tag_size) {
        uint8_t fh[10];
        if (fread(fh, 1, 10, fp) != 10) {
            break;
        }
        consumed += 10;
        if (fh[0] == 0) {
            break;  // padding reached
        }
        char id[5] = {(char)fh[0], (char)fh[1], (char)fh[2], (char)fh[3], 0};
        uint32_t fsize;
        if (version >= 4) {
            fsize = ((uint32_t)(fh[4] & 0x7F) << 21) | ((uint32_t)(fh[5] & 0x7F) << 14) |
                    ((uint32_t)(fh[6] & 0x7F) << 7) | (uint32_t)(fh[7] & 0x7F);
        } else {
            fsize = ((uint32_t)fh[4] << 24) | ((uint32_t)fh[5] << 16) | ((uint32_t)fh[6] << 8) |
                    (uint32_t)fh[7];
        }
        if (fsize == 0 || fsize > 8192) {
            fseek(fp, (long)fsize, SEEK_CUR);
            consumed += fsize;
            continue;
        }

        std::vector<uint8_t> buf(fsize);
        if (fread(buf.data(), 1, fsize, fp) != fsize) {
            break;
        }
        consumed += fsize;

        if (strcmp(id, "TIT2") == 0 && fsize > 1) {
            uint8_t enc = buf[0];
            const uint8_t* p = buf.data() + 1;
            size_t n = fsize - 1;
            if (enc == 1 || enc == 2) {
                title = Utf16ToUtf8(p, n, enc == 2);
            } else {  // 0 = ISO-8859-1 (Chinese tags usually store GBK), 3 = UTF-8
                while (n > 0 && p[n - 1] == 0) {
                    n--;
                }
                bool has_high = false;
                for (size_t k = 0; k < n; k++) {
                    if (p[k] >= 0x80) {
                        has_high = true;
                        break;
                    }
                }
                if (enc == 0 && has_high) {
                    title = GbkToUtf8(p, n);
                } else {
                    title.assign((const char*)p, n);
                }
            }
            break;
        }
    }
    fclose(fp);

    // Fallback: ID3v1 tag in the last 128 bytes
    if (title.empty()) {
        FILE* f1 = fopen(path.c_str(), "rb");
        if (f1) {
            uint8_t tag[128];
            if (fseek(f1, -128, SEEK_END) == 0 && fread(tag, 1, 128, f1) == 128 &&
                memcmp(tag, "TAG", 3) == 0) {
                size_t n = 30;
                while (n > 0 && (tag[3 + n - 1] == 0 || tag[3 + n - 1] == ' ')) {
                    n--;
                }
                title = GbkToUtf8(tag + 3, n);
            }
            fclose(f1);
        }
    }
    return title;
}

bool SdWavPlayer::PlayNext(const std::string& dir, AudioCodec* codec) {
    if (!mounted_ || !codec) {
        return false;
    }
    if (audio_files_.empty()) {
        CollectAudio(dir, audio_files_);
        std::sort(audio_files_.begin(), audio_files_.end());
        ESP_LOGI(TAG, "Playlist built: %d track(s)", (int)audio_files_.size());
    }
    if (audio_files_.empty()) {
        return false;
    }
    if (audio_index_ >= audio_files_.size()) {
        audio_index_ = 0;
    }
    std::string path = audio_files_[audio_index_++];
    ESP_LOGI(TAG, "Track %d/%d: %s", (int)audio_index_, (int)audio_files_.size(), path.c_str());
    return PlayAudio(path, codec);
}

// ———————————————————— PlayAudio ————————————————————
bool SdWavPlayer::PlayAudio(const std::string& path, AudioCodec* codec) {
    if (!mounted_) {
        ESP_LOGE(TAG, "SD not mounted");
        return false;
    }
    if (!codec) {
        ESP_LOGE(TAG, "codec is null");
        return false;
    }
    ESP_LOGI(TAG, "PlayAudio request: %s", path.c_str());

    // Stop any previous playback and wait for the old task to leave
    want_stop_ = true;
    for (int i = 0; i < 100 && playing_; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (playing_) {
        ESP_LOGW(TAG, "Previous playback still running, forcing new task");
        task_handle_ = nullptr;
    }

    current_path_ = path;
    current_title_ = ReadId3Title(path);
    ESP_LOGI(TAG, "Title: %s", current_title_.empty() ? "(none)" : current_title_.c_str());
    current_codec_ = codec;
    want_stop_ = false;
    playing_ = true;

    BaseType_t ret = xTaskCreate(PlaybackTaskTrampoline, "sd_mp3", 10240, this, 6, &task_handle_);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "Failed to create playback task");
        playing_ = false;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void SdWavPlayer::Stop() {
    want_stop_ = true;
}

void SdWavPlayer::PlaybackTaskTrampoline(void* arg) {
    SdWavPlayer* self = static_cast<SdWavPlayer*>(arg);
    self->PlaybackTask();
    self->playing_ = false;
    self->task_handle_ = nullptr;
    vTaskDelete(nullptr);
}

// ———————————————————— PlaybackTask ————————————————————
void SdWavPlayer::PlaybackTask() {
    const std::string path = current_path_;
    AudioCodec* codec = current_codec_;
    ESP_LOGI(TAG, "PlaybackTask start: %s", path.c_str());

    FILE* fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "Cannot open %s (errno=%d)", path.c_str(), errno);
        return;
    }

    // Register MP3 decoder + simple_decoder wrapper
    static bool mp3_registered = false;
    if (!mp3_registered) {
        esp_audio_err_t r1 = esp_mp3_dec_register();
        ESP_LOGI(TAG, "esp_mp3_dec_register() ret=%d", r1);
        // Also register to simple_decoder
        esp_audio_dec_ops_t mp3_ops = ESP_MP3_DEC_DEFAULT_OPS();
        esp_audio_simple_dec_reg_info_t reg_info = {
            .decoder_ops = mp3_ops,
            .parser = nullptr,
            .free = nullptr,
        };
        esp_audio_err_t r2 = esp_audio_simple_dec_register(ESP_AUDIO_SIMPLE_DEC_TYPE_MP3, &reg_info);
        ESP_LOGI(TAG, "esp_audio_simple_dec_register(MP3) ret=%d", r2);
        mp3_registered = true;
    }

    esp_audio_simple_dec_cfg_t dec_cfg = {
        .dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3,
        .dec_cfg = nullptr,
        .cfg_size = 0,
        .use_frame_dec = false,
    };
    esp_audio_simple_dec_handle_t dec_handle = nullptr;
    esp_audio_err_t derr = esp_audio_simple_dec_open(&dec_cfg, &dec_handle);
    if (derr != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Simple decoder open failed: %d", derr);
        fclose(fp);
        return;
    }
    // Skip ID3v2 tag so frame search starts on real audio data
    {
        uint8_t id3[10];
        if (fread(id3, 1, 10, fp) == 10 && memcmp(id3, "ID3", 3) == 0) {
            size_t tag_size = ((size_t)(id3[6] & 0x7F) << 21) | ((size_t)(id3[7] & 0x7F) << 14) |
                              ((size_t)(id3[8] & 0x7F) << 7) | (size_t)(id3[9] & 0x7F);
            fseek(fp, (long)(10 + tag_size), SEEK_SET);
            ESP_LOGI(TAG, "Skipped ID3v2 tag (%u bytes)", (unsigned)(10 + tag_size));
        } else {
            fseek(fp, 0, SEEK_SET);
        }
    }

    // Heap buffers (NOT on FreeRTOS task stack!)
    std::vector<uint8_t> pcm_buf(kPcmBufSamples * 2);  // interleaved 16-bit
    std::vector<int16_t> mono_acc;                     // accumulate mono PCM
    mono_acc.reserve(kMonoBlockSamples * 2);
    std::vector<int16_t> resample_buf;

    bool info_ready = false;
    int decoded_sr = 0;
    int decoded_ch = 0;
    int output_sr = codec->output_sample_rate();
    esp_ae_rate_cvt_handle_t resampler = nullptr;

    int total_frames = 0;

    // Resample accumulated block and write it to I2S in one shot
    auto write_block = [&]() {
        if (mono_acc.empty()) {
            return;
        }
        // AudioService powers down the TX path when it sees no output activity for a while
        // (it only tracks writes it makes itself), so keep it awake while we write directly.
        Application::GetInstance().GetAudioService().NotifyOutputActive();
        if (resampler != nullptr) {
            uint32_t max_out = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler, mono_acc.size(), &max_out);
            if (resample_buf.size() < max_out) {
                resample_buf.resize(max_out);
            }
            uint32_t actual_out = max_out;
            auto ret = esp_ae_rate_cvt_process(resampler, (esp_ae_sample_t)mono_acc.data(),
                                               mono_acc.size(),
                                               (esp_ae_sample_t)resample_buf.data(), &actual_out);
            if (ret == ESP_AE_ERR_OK && actual_out > 0) {
                resample_buf.resize(actual_out);
                codec->OutputData(resample_buf);
            } else if (ret != ESP_AE_ERR_OK) {
                ESP_LOGW(TAG, "rate_cvt_process err=%d", ret);
            }
        } else {
            codec->OutputData(mono_acc);
        }
        mono_acc.clear();
    };

    // Stream buffer keeps a whole frame buffered, so read-chunk boundaries never split a frame
    std::vector<uint8_t> sbuf(kMp3ChunkBytes + 4096);
    size_t fill = 0;   // valid bytes in sbuf
    size_t pos = 0;    // consumed position
    bool eof = false;
    int bad_frames = 0;
    int skipped_bytes = 0;

    auto refill = [&]() -> bool {
        if (pos > 0) {
            if (fill > pos) {
                memmove(sbuf.data(), sbuf.data() + pos, fill - pos);
                fill -= pos;
            } else {
                fill = 0;
            }
            pos = 0;
        }
        if (fill >= sbuf.size()) {
            return false;
        }
        size_t n = fread(sbuf.data() + fill, 1, sbuf.size() - fill, fp);
        if (n == 0) {
            eof = true;
            return false;
        }
        fill += n;
        return true;
    };

    while (!want_stop_) {
        // 1. Make sure a header can be parsed
        while ((fill - pos) < 4 && !eof && !want_stop_) {
            if (!refill()) {
                break;
            }
        }
        if ((fill - pos) < 4) {
            break;  // end of file
        }

        // 2. Locate the next frame sync
        size_t off = pos;
        int flen = 0;
        while ((off + 4) <= fill) {
            flen = Mp3FrameLength(sbuf.data() + off, fill - off);
            if (flen > 0) {
                break;
            }
            off++;
        }
        if (flen <= 0) {
            skipped_bytes += (int)(fill - pos);
            pos = fill;
            if (eof) {
                break;
            }
            continue;
        }
        if (off != pos) {
            skipped_bytes += (int)(off - pos);
            pos = off;
        }

        // 3. Buffer the complete frame before handing it to the decoder
        if ((fill - pos) < (size_t)flen) {
            if (!eof) {
                refill();
            }
            if ((fill - pos) < (size_t)flen) {
                break;  // truncated tail
            }
        }

        esp_audio_simple_dec_raw_t raw = {
            .buffer = sbuf.data() + pos,
            .len = (uint32_t)flen,
            .eos = (eof && (fill - pos == (size_t)flen)),
            .consumed = 0,
        };
        esp_audio_simple_dec_out_t out = {
            .buffer = pcm_buf.data(),
            .len = (uint32_t)pcm_buf.size(),
            .needed_size = 0,
            .decoded_size = 0,
        };

        derr = esp_audio_simple_dec_process(dec_handle, &raw, &out);

        if (derr == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            // Output buffer too small: enlarge and retry the same frame
            pcm_buf.resize(out.needed_size);
            out.buffer = pcm_buf.data();
            out.len = (uint32_t)pcm_buf.size();
            out.decoded_size = 0;
            continue;
        }
        if (derr != ESP_AUDIO_ERR_OK) {
            // Skip this frame and resync instead of aborting the whole chunk
            bad_frames++;
            if (bad_frames <= 3 || (bad_frames % 50) == 0) {
                ESP_LOGW(TAG, "frame decode err=%d (bad=%d)", derr, bad_frames);
            }
            pos += flen;
            continue;
        }
        pos += flen;

        if (out.decoded_size > 0) {
            total_frames++;

            // 4. Get info once, then create resampler for codec output rate
            if (!info_ready) {
                esp_audio_simple_dec_info_t info = {};
                if (esp_audio_simple_dec_get_info(dec_handle, &info) == ESP_AUDIO_ERR_OK) {
                    decoded_sr = info.sample_rate;
                    decoded_ch = info.channel;
                    info_ready = true;
                    ESP_LOGI(TAG, "MP3 info: sr=%d ch=%d bits=%d -> out sr=%d",
                             info.sample_rate, info.channel, info.bits_per_sample, output_sr);

                    if (decoded_sr > 0 && decoded_sr != output_sr) {
                        esp_ae_rate_cvt_cfg_t cvt_cfg = {
                            .src_rate = (uint32_t)decoded_sr,
                            .dest_rate = (uint32_t)output_sr,
                            .channel = 1,
                            .bits_per_sample = ESP_AUDIO_BIT16,
                            .complexity = 3,
                            .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                        };
                        auto ret = esp_ae_rate_cvt_open(&cvt_cfg, &resampler);
                        if (ret != ESP_AE_ERR_OK || resampler == nullptr) {
                            ESP_LOGE(TAG, "Rate cvt open failed: %d", ret);
                            resampler = nullptr;
                        } else {
                            ESP_LOGI(TAG, "Resampler %d -> %d ready", decoded_sr, output_sr);
                        }
                    }
                }
            }

            // 5. Downmix this frame to mono and append to the block
            uint32_t ch = (decoded_ch > 1) ? (uint32_t)decoded_ch : 1;
            uint32_t mono_samples = out.decoded_size / (2 * ch);
            if (mono_samples > 0) {
                size_t old_size = mono_acc.size();
                mono_acc.resize(old_size + mono_samples);
                int16_t* dst = mono_acc.data() + old_size;
                const int16_t* src = (const int16_t*)pcm_buf.data();
                if (ch > 1) {
                    for (uint32_t i = 0; i < mono_samples; i++) {
                        int32_t sum = 0;
                        for (uint32_t c = 0; c < ch; c++) {
                            sum += src[i * ch + c];
                        }
                        dst[i] = (int16_t)(sum / (int32_t)ch);
                    }
                } else {
                    memcpy(dst, src, mono_samples * sizeof(int16_t));
                }

                // 6. Write in large blocks to avoid I2S underrun
                if (mono_acc.size() >= kMonoBlockSamples) {
                    write_block();
                }
            }

            if (total_frames % 100 == 0) {
                ESP_LOGI(TAG, "decoded frames=%d", total_frames);
            }
        }
    }

    // Flush tail when playback ended naturally
    if (!want_stop_) {
        write_block();
    }

    ESP_LOGI(TAG, "Playback done (frames=%d, stop=%d, bad=%d, skipped=%d)",
             total_frames, want_stop_ ? 1 : 0, bad_frames, skipped_bytes);
    if (resampler != nullptr) {
        esp_ae_rate_cvt_close(resampler);
    }
    esp_audio_simple_dec_close(dec_handle);
    fclose(fp);
}
