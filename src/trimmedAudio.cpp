#include "trimmedAudio.h"

trimmedAudio::trimmedAudio(bool internalDAC, uint8_t channelEnabled, uint8_t i2sPort)
    : m_f_internalDAC(internalDAC), m_f_channelEnabled(channelEnabled), m_i2s_num(i2sPort), m_vol(21), m_curve(0) {
    // Constructor implementation
}

trimmedAudio::~trimmedAudio() {
    // Destructor implementation
}

bool trimmedAudio::setPinout(uint8_t BCLK, uint8_t LRC, uint8_t DOUT, int8_t MCLK) {
    m_f_psramFound = psramInit();
    x_ps_free(m_chbuf);
    x_ps_free(m_ibuff);
    x_ps_free(m_outBuff);
    x_ps_free(m_lastHost);
    if (m_f_psramFound) m_chbufSize = 4096; else m_chbufSize = 512 + 64;
    if (m_f_psramFound) m_ibuffSize = 4096; else m_ibuffSize = 512 + 64;
    m_lastHost = (char*)x_ps_malloc(2048);
    m_outBuff = (int16_t*)x_ps_malloc(m_outbuffSize * sizeof(int16_t));
    m_chbuf = (char*)x_ps_malloc(m_chbufSize);
    m_ibuff = (char*)x_ps_malloc(m_ibuffSize);
    if (!m_chbuf || !m_lastHost || !m_outBuff || !m_ibuff) log_e("oom");

    esp_err_t result = ESP_OK;

    if (m_f_internalDAC) {
#if (ESP_IDF_VERSION_MAJOR != 5)
        i2s_set_pin((i2s_port_t)m_i2s_num, NULL);
#endif
        return true;
    }

#if (ESP_ARDUINO_VERSION_MAJOR < 2)
    log_e("Arduino Version too old!");
#endif
#if (ESP_ARDUINO_VERSION_MAJOR == 2 && ESP_ARDUINO_VERSION_PATCH < 8)
    log_e("Arduino Version must be 2.0.8 or higher!");
#endif

#if (ESP_IDF_VERSION_MAJOR == 5)
    i2s_std_gpio_config_t gpio_cfg = {};
    gpio_cfg.bclk = (gpio_num_t)BCLK;
    gpio_cfg.din = (gpio_num_t)I2S_GPIO_UNUSED;
    gpio_cfg.dout = (gpio_num_t)DOUT;
    gpio_cfg.mclk = (gpio_num_t)MCLK;
    gpio_cfg.ws = (gpio_num_t)LRC;
    I2Sstop(m_i2s_num);
    result = i2s_channel_reconfig_std_gpio(m_i2s_tx_handle, &gpio_cfg);
    I2Sstart(m_i2s_num);
#else
    m_pin_config.bck_io_num = BCLK;
    m_pin_config.ws_io_num = LRC; // wclk = lrc
    m_pin_config.data_out_num = DOUT;
    m_pin_config.data_in_num = I2S_GPIO_UNUSED;
    m_pin_config.mck_io_num = MCLK;
    result = i2s_set_pin((i2s_port_t)m_i2s_num, &m_pin_config);
#endif
    return (result == ESP_OK);
}

void trimmedAudio::setVolume(uint8_t vol, uint8_t curve) { // curve 0: default, curve 1: flat at the beginning
    uint16_t v = ESP_ARDUINO_VERSION_MAJOR * 100 + ESP_ARDUINO_VERSION_MINOR * 10 + ESP_ARDUINO_VERSION_PATCH;
    if (v < 207) {
        audio_info("Do not use this ancient Arduino version V%d.%d.%d", ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH);
    }

    if (vol > m_vol_steps) m_vol = m_vol_steps;
    else m_vol = vol;

    if (curve > 1) m_curve = 1;
    else m_curve = curve;

    computeLimit();
}

void trimmedAudio::computeLimit() {    // is calculated when the volume or balance changes
    double l = 1, r = 1, v = 1; // assume 100%

    /* balance is left -16...+16 right */
    /* TODO: logarithmic scaling of balance, too? */
    if (m_balance > 0) {
        r -= (double)abs(m_balance) / 16;
    } else if (m_balance < 0) {
        l -= (double)abs(m_balance) / 16;
    }

    switch (m_curve) {
        case 0:
            v = (double)pow(m_vol, 2) / pow(m_vol_steps, 2); // square (default)
            break;
        case 1: // logarithmic
            double log1 = log(1);
            if (m_vol > 0) {
                v = m_vol * ((std::exp(log1 + (m_vol - 1) * (std::log(m_vol_steps) - log1) / (m_vol_steps - 1))) / m_vol_steps) / m_vol_steps;
            } else {
                v = 0;
            }
            break;
    }

    m_limit_left = l * v;
    m_limit_right = r * v;

    // log_i("m_limit_left %f,  m_limit_right %f ", m_limit_left, m_limit_right);
}

bool trimmedAudio::connecttospeech(const char* speech, const char* lang) {
    xSemaphoreTakeRecursive(mutex_playAudioData, 0.3 * configTICK_RATE_HZ);

    setDefaults();
    const char* host = "translate.google.com.vn";
    const char* path = "/translate_tts";

    uint16_t speechLen = strlen(speech);
    uint16_t speechBuffLen = speechLen + 300;
    char* speechBuff = (char*)malloc(speechBuffLen);
    if (!speechBuff) {
        log_e("out of memory");
        xSemaphoreGiveRecursive(mutex_playAudioData);
        return false;
    }
    memcpy(speechBuff, speech, speechLen);
    speechBuff[speechLen] = '\0';
    char* urlStr = urlencode(speechBuff, false); // percent encoding
    if (!urlStr) {
        log_e("out of memory");
        xSemaphoreGiveRecursive(mutex_playAudioData);
        return false;
    }

    char resp[strlen(urlStr) + 200] = "";
    strcat(resp, "GET ");
    strcat(resp, path);
    strcat(resp, "?ie=UTF-8&tl=");
    strcat(resp, lang);
    strcat(resp, "&client=tw-ob&q=");
    strcat(resp, urlStr);
    strcat(resp, " HTTP/1.1\r\n");
    strcat(resp, "Host: ");
    strcat(resp, host);
    strcat(resp, "\r\n");
    strcat(resp, "User-Agent: Mozilla/5.0 \r\n");
    strcat(resp, "Accept-Encoding: identity\r\n");
    strcat(resp, "Accept: text/html\r\n");
    strcat(resp, "Connection: close\r\n\r\n");

    free(speechBuff);
    free(urlStr);

    _client = static_cast<WiFiClient*>(&client);
    audio_info("connect to \"%s\"", host);
    if (!_client->connect(host, 80)) {
        log_e("Connection failed");
        xSemaphoreGiveRecursive(mutex_playAudioData);
        return false;
    }
    _client->print(resp);

    m_streamType = ST_WEBFILE;
    m_f_running = true;
    m_f_ssl = false;
    m_f_tts = true;
    m_dataMode = HTTP_RESPONSE_HEADER;
    xSemaphoreGiveRecursive(mutex_playAudioData);
    return true;
}

void trimmedAudio::loop() {
    if (!m_f_running) return;

    switch (m_dataMode) {
        case AUDIO_LOCALFILE:
            // Placeholder for processing local file
            // processLocalFile();
            break;
        case HTTP_RESPONSE_HEADER:
            static uint8_t count = 0;
            if (!parseHttpResponseHeader()) {
                if (m_f_timeout && count < 3) {
                    m_f_timeout = false;
                    count++;
                    connecttohost(m_lastHost);
                }
            } else {
                count = 0;
            }
            break;
        case AUDIO_PLAYLISTINIT:
            // Placeholder for reading playlist data
            // readPlayListData();
            break;
        case AUDIO_PLAYLISTDATA:
            // Placeholder for parsing playlist
            // if (m_playlistFormat == FORMAT_M3U) connecttohost(parsePlaylist_M3U());
            // if (m_playlistFormat == FORMAT_PLS) connecttohost(parsePlaylist_PLS());
            // if (m_playlistFormat == FORMAT_ASX) connecttohost(parsePlaylist_ASX());
            break;
        case AUDIO_DATA:
            if (m_streamType == ST_WEBSTREAM) {
                // Placeholder for processing web stream
                // processWebStream();
            }
            if (m_streamType == ST_WEBFILE) {
                // Placeholder for processing web file
                // processWebFile();
            }
            break;
    }
}

void trimmedAudio::audio_info(const char* msg) {
    Serial.println(msg);
}
