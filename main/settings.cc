#include "settings.h"

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#define TAG "Settings"

/* NVS 的 key 上限是 15 个字符（NVS_KEY_NAME_MAX_SIZE - 1），不含结尾 '\0'。
 *
 * 超长 key 的两种表现都很隐蔽：
 *   写 —— nvs_set_* 返回 ESP_ERR_NVS_KEY_TOO_LONG，老代码用 ESP_ERROR_CHECK 包着，
 *         直接 abort，用户看到的是「点一下设置就重启」；
 *   读 —— nvs_get_* 同样返回该错误，但 Get* 把它当成「key 不存在」吞掉返回默认值，
 *         于是设置页永远显示默认值，改过的档位从来没真正落盘。
 * 所以这里统一在入口拦掉，并且所有 NVS 调用都不再用 ESP_ERROR_CHECK —— 一个设置项
 * 写失败不该把整机带进 panic。 */
static constexpr size_t kMaxNvsKeyLen = 15;

static bool KeyUsable(const char* caller, const std::string& ns, const std::string& key) {
    if (key.size() <= kMaxNvsKeyLen) {
        return true;
    }
    ESP_LOGE(TAG, "%s: NVS key \"%s\" is %u chars (> %u) in namespace \"%s\", ignored", caller,
             key.c_str(), static_cast<unsigned>(key.size()),
             static_cast<unsigned>(kMaxNvsKeyLen), ns.c_str());
    return false;
}

Settings::Settings(const std::string& ns, bool read_write) : ns_(ns), read_write_(read_write) {
    esp_err_t err = nvs_open(ns.c_str(), read_write_ ? NVS_READWRITE : NVS_READONLY, &nvs_handle_);
    if (err != ESP_OK) {
        /* 保持 0，后续所有读写都直接走默认值 / 空操作，不会再拿无效 handle 去调用 NVS。 */
        nvs_handle_ = 0;
        ESP_LOGE(TAG, "nvs_open(\"%s\", %s) failed: %s", ns_.c_str(),
                 read_write_ ? "rw" : "ro", esp_err_to_name(err));
    }
}

Settings::~Settings() {
    if (nvs_handle_ != 0) {
        if (read_write_ && dirty_) {
            esp_err_t err = nvs_commit(nvs_handle_);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "nvs_commit(\"%s\") failed: %s", ns_.c_str(), esp_err_to_name(err));
            }
        }
        nvs_close(nvs_handle_);
    }
}

std::string Settings::GetString(const std::string& key, const std::string& default_value) {
    if (nvs_handle_ == 0 || !KeyUsable("GetString", ns_, key)) {
        return default_value;
    }

    size_t length = 0;
    if (nvs_get_str(nvs_handle_, key.c_str(), nullptr, &length) != ESP_OK) {
        return default_value;
    }

    std::string value;
    value.resize(length);
    esp_err_t err = nvs_get_str(nvs_handle_, key.c_str(), value.data(), &length);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "GetString(\"%s\") in \"%s\" failed: %s", key.c_str(), ns_.c_str(),
                 esp_err_to_name(err));
        return default_value;
    }
    while (!value.empty() && value.back() == '\0') {
        value.pop_back();
    }
    return value;
}

void Settings::SetString(const std::string& key, const std::string& value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    if (nvs_handle_ == 0 || !KeyUsable("SetString", ns_, key)) {
        return;
    }
    esp_err_t err = nvs_set_str(nvs_handle_, key.c_str(), value.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SetString(\"%s\") in \"%s\" failed: %s", key.c_str(), ns_.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

int32_t Settings::GetInt(const std::string& key, int32_t default_value) {
    if (nvs_handle_ == 0 || !KeyUsable("GetInt", ns_, key)) {
        return default_value;
    }

    int32_t value;
    if (nvs_get_i32(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value;
}

void Settings::SetInt(const std::string& key, int32_t value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    if (nvs_handle_ == 0 || !KeyUsable("SetInt", ns_, key)) {
        return;
    }
    esp_err_t err = nvs_set_i32(nvs_handle_, key.c_str(), value);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SetInt(\"%s\") in \"%s\" failed: %s", key.c_str(), ns_.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

bool Settings::GetBool(const std::string& key, bool default_value) {
    if (nvs_handle_ == 0 || !KeyUsable("GetBool", ns_, key)) {
        return default_value;
    }

    uint8_t value;
    if (nvs_get_u8(nvs_handle_, key.c_str(), &value) != ESP_OK) {
        return default_value;
    }
    return value != 0;
}

void Settings::SetBool(const std::string& key, bool value) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    if (nvs_handle_ == 0 || !KeyUsable("SetBool", ns_, key)) {
        return;
    }
    esp_err_t err = nvs_set_u8(nvs_handle_, key.c_str(), value ? 1 : 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SetBool(\"%s\") in \"%s\" failed: %s", key.c_str(), ns_.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

void Settings::EraseKey(const std::string& key) {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    if (nvs_handle_ == 0 || !KeyUsable("EraseKey", ns_, key)) {
        return;
    }
    esp_err_t err = nvs_erase_key(nvs_handle_, key.c_str());
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGE(TAG, "EraseKey(\"%s\") in \"%s\" failed: %s", key.c_str(), ns_.c_str(),
                 esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}

void Settings::EraseAll() {
    if (!read_write_) {
        ESP_LOGW(TAG, "Namespace %s is not open for writing", ns_.c_str());
        return;
    }
    if (nvs_handle_ == 0) {
        return;
    }
    esp_err_t err = nvs_erase_all(nvs_handle_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EraseAll in \"%s\" failed: %s", ns_.c_str(), esp_err_to_name(err));
        return;
    }
    dirty_ = true;
}
