#include "config_server.h"
#include "wifi_manager.h"
#include "ble_adv.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static const char *TAG = "ConfigServer";

static httpd_handle_t server_handle = NULL;
static TaskHandle_t dns_task_handle = NULL;
static TaskHandle_t scan_task_handle = NULL;
static bool dns_server_running = false;
static SemaphoreHandle_t scan_state_mutex = NULL;
static bool has_new_config = false;
static std::string saved_ssid;
static std::string saved_password;
static std::string cached_scan_response;
static std::string last_scan_error;
static int64_t last_scan_time_us = 0;
static bool scan_in_progress = false;
static bool scan_stop_requested = false;
static SemaphoreHandle_t provision_mutex = NULL;

static constexpr uint16_t DNS_SERVER_PORT = 53;
static constexpr uint8_t CAPTIVE_PORTAL_IP[4] = {192, 168, 4, 1};
static constexpr const char* CAPTIVE_PORTAL_URL = "http://192.168.4.1/";
static constexpr int64_t SCAN_CACHE_WINDOW_US = 30LL * 1000 * 1000;
static constexpr uint32_t INITIAL_SCAN_DELAY_MS = 1200;
/** 首次 /scan 触发的后台扫描延迟；扫描前会暂停 BLE 广播，可略短于纯 AP 场景 */
static constexpr uint32_t FIRST_PAGE_SCAN_DELAY_MS = 700;
/** 手动「重新扫描」时任务启动前延迟；0 尽快开始，扫描前会暂停 BLE 广播保稳定 */
static constexpr uint32_t REFRESH_SCAN_DELAY_MS = 0;
/**
 * 是否自动发起 WiFi 扫描（启动时延迟扫、打开页面自动扫）。
 * 关闭后仅响应「重新扫描」（/scan?refresh=1），减轻 AP+BLE+扫描 共存时手机掉线、强制门户闪退。
 * 关闭则仅手动「刷新」扫描（/scan?refresh=1）。
 */
static constexpr bool kProvisionAutoWifiScan = true;

struct ScanNetwork {
    std::string ssid;
    int rssi;
    wifi_auth_mode_t authmode;
};

struct ProvisionRequest {
    std::string ssid;
    std::string password;
    std::string bssid;
    std::string security_type;
};

static const char* config_html = R"HTML(
<!DOCTYPE html>
<html lang="zh-CN">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>WiFi 配网</title>
    <style>
        :root {
            color-scheme: light;
            --fg: #1a1a1a;
            --muted: #6b6b6b;
            --line: #e6e6e6;
            --bar-off: #d4d4d4;
            --bar-on: #1a1a1a;
            --accent: #2563eb;
            --err: #b91c1c;
            --ok: #15803d;
        }
        * { box-sizing: border-box; }
        body {
            margin: 0;
            min-height: 100vh;
            font-family: system-ui, -apple-system, "Segoe UI", "PingFang SC", "Microsoft YaHei", sans-serif;
            background: #f7f7f7;
            color: var(--fg);
            padding: 20px 16px 48px;
            -webkit-tap-highlight-color: transparent;
        }
        .wrap { max-width: 420px; margin: 0 auto; }
        h1 {
            font-size: 1.375rem;
            font-weight: 600;
            margin: 0 0 10px;
            letter-spacing: -0.02em;
        }
        .band {
            font-size: 0.8125rem;
            color: var(--muted);
            line-height: 1.55;
            margin: 0 0 18px;
        }
        .band strong { color: var(--fg); font-weight: 600; }
        .card {
            background: #fff;
            border: 1px solid var(--line);
            border-radius: 14px;
            padding: 16px 16px 14px;
            margin-bottom: 12px;
        }
        .row-top {
            display: flex;
            align-items: center;
            justify-content: space-between;
            margin-bottom: 10px;
        }
        .row-top h2 {
            margin: 0;
            font-size: 0.9375rem;
            font-weight: 600;
        }
        .btn-ghost {
            background: none;
            border: none;
            color: var(--accent);
            font-size: 0.875rem;
            padding: 6px 8px;
            cursor: pointer;
            font-weight: 500;
        }
        .btn-ghost:disabled { opacity: 0.35; cursor: default; }
        .msg {
            font-size: 0.8125rem;
            min-height: 1.25em;
            margin-bottom: 6px;
        }
        .msg.err { color: var(--err); }
        .msg.ok { color: var(--ok); }
        .list { display: flex; flex-direction: column; gap: 8px; }
        .load, .empty {
            text-align: center;
            color: var(--muted);
            font-size: 0.875rem;
            padding: 20px 8px;
        }
        .wifi-btn {
            width: 100%;
            display: flex;
            align-items: center;
            justify-content: space-between;
            gap: 10px;
            text-align: left;
            padding: 12px 12px;
            border: 1px solid var(--line);
            border-radius: 10px;
            background: #fff;
            cursor: pointer;
            font: inherit;
        }
        .wifi-btn:active { background: #fafafa; }
        .wifi-ssid {
            font-weight: 500;
            font-size: 0.9375rem;
            word-break: break-all;
            flex: 1;
            min-width: 0;
        }
        .sig {
            flex-shrink: 0;
            display: flex;
            flex-direction: column;
            align-items: flex-end;
            gap: 5px;
        }
        .bars {
            display: flex;
            align-items: flex-end;
            gap: 3px;
            height: 18px;
        }
        .bars i {
            display: block;
            width: 4px;
            border-radius: 1px;
            background: var(--bar-off);
        }
        .bars i:nth-child(1) { height: 5px; }
        .bars i:nth-child(2) { height: 9px; }
        .bars i:nth-child(3) { height: 13px; }
        .bars i:nth-child(4) { height: 17px; }
        .bars.n1 i:nth-child(1),
        .bars.n2 i:nth-child(1), .bars.n2 i:nth-child(2),
        .bars.n3 i:nth-child(1), .bars.n3 i:nth-child(2), .bars.n3 i:nth-child(3),
        .bars.n4 i:nth-child(1), .bars.n4 i:nth-child(2), .bars.n4 i:nth-child(3), .bars.n4 i:nth-child(4) {
            background: var(--bar-on);
        }
        .track {
            width: 72px;
            height: 4px;
            background: var(--line);
            border-radius: 2px;
            overflow: hidden;
        }
        .track > span {
            display: block;
            height: 100%;
            background: var(--bar-on);
            border-radius: 2px;
            min-width: 8%;
            transition: width 0.2s ease;
        }
        label {
            display: block;
            font-size: 0.8125rem;
            font-weight: 500;
            margin-bottom: 6px;
            color: #333;
        }
        input[type="text"], input[type="password"] {
            width: 100%;
            padding: 12px 12px;
            font-size: 1rem;
            border: 1px solid var(--line);
            border-radius: 10px;
        }
        input:focus {
            outline: none;
            border-color: #94a3b8;
        }
        input.invalid { border-color: var(--err); }
        .field { margin-bottom: 14px; }
        .btn-main {
            width: 100%;
            margin-top: 6px;
            padding: 14px;
            font-size: 1rem;
            font-weight: 600;
            color: #fff;
            background: var(--fg);
            border: none;
            border-radius: 10px;
            cursor: pointer;
        }
        .btn-main:disabled {
            background: #b0b0b0;
            cursor: not-allowed;
        }
        .link-pw {
            background: none;
            border: none;
            color: var(--muted);
            font-size: 0.8125rem;
            padding: 4px 0 0;
            cursor: pointer;
            text-decoration: underline;
        }
    </style>
</head>
<body>
    <div class="wrap">
        <h1>WiFi 配网</h1>
        <p class="band">本设备<strong>仅支持 2.4GHz</strong> WiFi，请选择路由器上的 2.4G 网络。</p>

        <div class="card">
            <div class="row-top">
                <h2>附近网络</h2>
                <button type="button" class="btn-ghost" id="refreshBtn">刷新</button>
            </div>
            <div id="scanMessage" class="msg"></div>
            <div id="networkList" class="list"><div class="load">扫描中…</div></div>
        </div>

        <div class="card">
            <div class="row-top">
                <h2>连接</h2>
            </div>
            <div id="formMessage" class="msg"></div>
            <form id="configForm" autocomplete="off">
                <div class="field">
                    <label for="ssid">名称</label>
                    <input id="ssid" name="ssid" type="text" maxlength="32" placeholder="WiFi 名称" autocomplete="off">
                </div>
                <div class="field">
                    <label for="password">密码</label>
                    <input id="password" name="password" type="password" maxlength="64" placeholder="加密网络必填" autocomplete="off">
                </div>
                <button type="button" class="link-pw" id="togglePasswordBtn">显示密码</button>
                <button type="submit" class="btn-main" id="submitBtn" disabled>保存并连接</button>
            </form>
        </div>
    </div>

    <script>
        const networkListEl = document.getElementById('networkList');
        const scanMessageEl = document.getElementById('scanMessage');
        const formMessageEl = document.getElementById('formMessage');
        const refreshBtn = document.getElementById('refreshBtn');
        const submitBtn = document.getElementById('submitBtn');
        const togglePasswordBtn = document.getElementById('togglePasswordBtn');
        const ssidInput = document.getElementById('ssid');
        const passwordInput = document.getElementById('password');
        const configForm = document.getElementById('configForm');
        let scanPollTimer = null;
        let hasRenderedNetworks = false;
        let pickedSecure = null;

        function rssiTier(rssi) {
            const n = Number(rssi);
            if (n >= -55) return 4;
            if (n >= -67) return 3;
            if (n >= -78) return 2;
            return 1;
        }

        function rssiBarPct(rssi) {
            const n = Number(rssi);
            const p = ((n + 95) / 60) * 100;
            return Math.max(8, Math.min(100, Math.round(p)));
        }

        function showFormMsg(text, ok) {
            formMessageEl.textContent = text || '';
            formMessageEl.className = 'msg' + (ok ? ' ok' : text ? ' err' : '');
        }

        function showScanMsg(text, err) {
            scanMessageEl.textContent = text || '';
            scanMessageEl.className = 'msg' + (err ? ' err' : '');
        }

        function clearScanMsg() {
            scanMessageEl.textContent = '';
            scanMessageEl.className = 'msg';
        }

        function updateSubmitState() {
            submitBtn.disabled = ssidInput.value.trim().length === 0;
        }

        function fillNetwork(ssid, secure) {
            ssidInput.value = ssid;
            pickedSecure = !!secure;
            passwordInput.value = '';
            if (secure) {
                passwordInput.focus();
            }
            showFormMsg('', true);
            updateSubmitState();
        }

        function renderNetworks(networks) {
            networkListEl.innerHTML = '';
            hasRenderedNetworks = true;

            if (!Array.isArray(networks) || networks.length === 0) {
                const empty = document.createElement('div');
                empty.className = 'empty';
                empty.textContent = '暂无网络，请点「刷新」';
                networkListEl.appendChild(empty);
                return;
            }

            networks.forEach((network) => {
                const rssi = network.rssi != null ? network.rssi : -90;
                const tier = rssiTier(rssi);
                const pct = rssiBarPct(rssi);

                const btn = document.createElement('button');
                btn.type = 'button';
                btn.className = 'wifi-btn';
                btn.addEventListener('click', () => fillNetwork(network.ssid, network.secure));

                const name = document.createElement('div');
                name.className = 'wifi-ssid';
                name.textContent = network.ssid;

                const sig = document.createElement('div');
                sig.className = 'sig';
                const bars = document.createElement('div');
                bars.className = 'bars n' + tier;
                for (let i = 0; i < 4; i++) {
                    bars.appendChild(document.createElement('i'));
                }
                const track = document.createElement('div');
                track.className = 'track';
                const fill = document.createElement('span');
                fill.style.width = pct + '%';
                track.appendChild(fill);
                sig.appendChild(bars);
                sig.appendChild(track);

                btn.appendChild(name);
                btn.appendChild(sig);
                networkListEl.appendChild(btn);
            });
        }

        function scheduleScanPoll(attempt, forceRefresh) {
            clearTimeout(scanPollTimer);
            const waitMs = forceRefresh ? 450 : 700;
            scanPollTimer = window.setTimeout(() => loadNetworks(forceRefresh, attempt), waitMs);
        }

        async function loadNetworks(forceRefresh = false, attempt = 0) {
            const requestPath = forceRefresh ? '/scan?refresh=1' : '/scan';

            if (attempt === 0) {
                clearScanMsg();
                refreshBtn.disabled = true;
                if (!hasRenderedNetworks || forceRefresh) {
                    networkListEl.innerHTML = '<div class="load">扫描中…</div>';
                }
            }

            try {
                const response = await fetch(requestPath, { cache: 'no-store' });
                const data = await response.json();
                if (!response.ok || !data.success) {
                    throw new Error(data.message || '扫描失败');
                }

                if (data.pending) {
                    networkListEl.innerHTML = '<div class="load">' + (data.message || '扫描中…') + '</div>';
                    if (attempt < 24) {
                        scheduleScanPoll(attempt + 1, forceRefresh);
                    } else {
                        showScanMsg('超时，请点「刷新」', true);
                    }
                    return;
                }

                if (data.manualScanOnly) {
                    clearTimeout(scanPollTimer);
                    const tip = data.hint || '请点「刷新」';
                    showScanMsg(tip, false);
                    networkListEl.innerHTML = '<div class="empty">' + tip + '</div>';
                    refreshBtn.disabled = false;
                    return;
                }

                clearTimeout(scanPollTimer);
                clearScanMsg();
                renderNetworks(data.networks || []);
            } catch (error) {
                networkListEl.innerHTML = '<div class="empty">加载失败，请点「刷新」</div>';
                showScanMsg(error.message || '失败', true);
            } finally {
                refreshBtn.disabled = false;
            }
        }

        refreshBtn.addEventListener('click', function() {
            loadNetworks(true, 0);
        });

        ssidInput.addEventListener('input', function() {
            pickedSecure = null;
            updateSubmitState();
        });

        togglePasswordBtn.addEventListener('click', function() {
            const showing = passwordInput.type === 'text';
            passwordInput.type = showing ? 'password' : 'text';
            togglePasswordBtn.textContent = showing ? '显示密码' : '隐藏密码';
        });

        configForm.addEventListener('submit', async function(event) {
            event.preventDefault();
            showFormMsg('', true);

            const ssid = ssidInput.value.trim();
            const password = passwordInput.value;

            if (!ssid) {
                showFormMsg('请填写 WiFi 名称', false);
                ssidInput.classList.add('invalid');
                ssidInput.focus();
                return;
            }
            ssidInput.classList.remove('invalid');

            if (pickedSecure === true && password.trim() === '') {
                showFormMsg('该网络需要密码', false);
                passwordInput.focus();
                return;
            }

            submitBtn.disabled = true;

            try {
                const response = await fetch('/wifi/config', {
                    method: 'POST',
                    headers: { 'Content-Type': 'application/json;charset=UTF-8' },
                    body: JSON.stringify({
                        ssid: ssid,
                        password: password,
                        bssid: '',
                        securityType: ''
                    })
                });
                const data = await response.json();
                if (!response.ok || !data.success) {
                    throw new Error(data.message || '失败');
                }
                showFormMsg('已保存', true);
            } catch (error) {
                showFormMsg(error.message || '失败', false);
            } finally {
                updateSubmitState();
            }
        });

        window.addEventListener('DOMContentLoaded', function() {
            networkListEl.innerHTML = '<div class="load">扫描中…</div>';
            updateSubmitState();
            loadNetworks(false, 0);
        });
    </script>
</body>
</html>
)HTML";

static void set_common_headers(httpd_req_t *req, const char *content_type) {
    httpd_resp_set_type(req, content_type);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_set_hdr(req, "Pragma", "no-cache");
    httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(req, "Content-Security-Policy", "default-src 'self' 'unsafe-inline'");
}

static esp_err_t send_redirect_response(httpd_req_t *req, const char *location) {
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Connection", "close");
    set_common_headers(req, "text/plain; charset=utf-8");
    /* 部分机型在探测完成后会立即关连接，NULL 体偶发与 httpd 交互不佳；空串更稳。对端已断开时仍可能返回非 OK，忽略即可 */
    (void)httpd_resp_send(req, "", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t send_json_response(httpd_req_t *req, const std::string& body, const char *status = "200 OK") {
    httpd_resp_set_status(req, status);
    set_common_headers(req, "application/json; charset=utf-8");
    esp_err_t ret = httpd_resp_send(req, body.c_str(), HTTPD_RESP_USE_STRLEN);
    return (ret == ESP_OK) ? ESP_OK : ESP_OK;
}

static std::string json_escape(const std::string& input) {
    std::string output;
    output.reserve(input.size() + 8);

    for (unsigned char ch : input) {
        switch (ch) {
            case '\"': output += "\\\""; break;
            case '\\': output += "\\\\"; break;
            case '\b': output += "\\b"; break;
            case '\f': output += "\\f"; break;
            case '\n': output += "\\n"; break;
            case '\r': output += "\\r"; break;
            case '\t': output += "\\t"; break;
            default:
                if (ch < 0x20) {
                    char buf[7];
                    snprintf(buf, sizeof(buf), "\\u%04x", ch);
                    output += buf;
                } else {
                    output += static_cast<char>(ch);
                }
                break;
        }
    }

    return output;
}

static std::string url_decode(const std::string& str) {
    std::string result;
    result.reserve(str.length());

    for (size_t i = 0; i < str.length(); ++i) {
        if (str[i] == '+') {
            result += ' ';
        } else if (str[i] == '%' && i + 2 < str.length()) {
            char hex[3] = {str[i + 1], str[i + 2], '\0'};
            char *end = nullptr;
            long value = strtol(hex, &end, 16);
            if (*end == '\0' && value >= 0 && value <= 255) {
                result += static_cast<char>(value);
                i += 2;
            } else {
                result += str[i];
            }
        } else {
            result += str[i];
        }
    }

    return result;
}

static std::map<std::string, std::string> parse_form_urlencoded(const std::string& body) {
    std::map<std::string, std::string> values;
    size_t start = 0;

    while (start <= body.size()) {
        size_t amp = body.find('&', start);
        std::string pair = body.substr(start, amp == std::string::npos ? std::string::npos : amp - start);
        size_t eq = pair.find('=');
        std::string key = url_decode(pair.substr(0, eq));
        std::string value = eq == std::string::npos ? "" : url_decode(pair.substr(eq + 1));
        if (!key.empty()) {
            values[key] = value;
        }
        if (amp == std::string::npos) {
            break;
        }
        start = amp + 1;
    }

    return values;
}

static size_t skip_json_whitespace(const std::string& body, size_t pos) {
    while (pos < body.size()) {
        char ch = body[pos];
        if (ch != ' ' && ch != '\t' && ch != '\r' && ch != '\n') {
            break;
        }
        ++pos;
    }
    return pos;
}

bool extract_json_string_field(const std::string& body, const char* key, std::string& value) {
    const std::string quoted_key = std::string("\"") + key + "\"";
    size_t key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return false;
    }

    size_t colon_pos = body.find(':', key_pos + quoted_key.size());
    if (colon_pos == std::string::npos) {
        return false;
    }

    size_t value_pos = skip_json_whitespace(body, colon_pos + 1);
    if (value_pos >= body.size() || body[value_pos] != '"') {
        return false;
    }

    ++value_pos;
    std::string result;
    result.reserve(32);

    while (value_pos < body.size()) {
        char ch = body[value_pos++];
        if (ch == '\\') {
            if (value_pos >= body.size()) {
                return false;
            }
            char escaped = body[value_pos++];
            switch (escaped) {
                case '"': result.push_back('"'); break;
                case '\\': result.push_back('\\'); break;
                case '/': result.push_back('/'); break;
                case 'b': result.push_back('\b'); break;
                case 'f': result.push_back('\f'); break;
                case 'n': result.push_back('\n'); break;
                case 'r': result.push_back('\r'); break;
                case 't': result.push_back('\t'); break;
                default: return false;
            }
            continue;
        }

        if (ch == '"') {
            value = result;
            return true;
        }

        result.push_back(ch);
    }

    return false;
}

static bool parse_json_config_body(const std::string& body, ProvisionRequest& request) {
    bool has_ssid = extract_json_string_field(body, "ssid", request.ssid);
    bool has_password = extract_json_string_field(body, "password", request.password);
    extract_json_string_field(body, "bssid", request.bssid);
    extract_json_string_field(body, "securityType", request.security_type);
    return has_ssid && has_password;
}

static bool parse_form_config_body(const std::string& body, ProvisionRequest& request) {
    std::map<std::string, std::string> params = parse_form_urlencoded(body);
    request.ssid = params.count("ssid") ? params["ssid"] : "";
    request.password = params.count("password") ? params["password"] : "";
    request.bssid = params.count("bssid") ? params["bssid"] : "";
    request.security_type = params.count("securityType") ? params["securityType"] : "";
    return !request.ssid.empty();
}

static esp_err_t save_wifi_config_to_nvs(const ProvisionRequest& request) {
    nvs_handle_t nvs_handle;
    esp_err_t ret = nvs_open("wifi_config", NVS_READWRITE, &nvs_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "打开NVS失败: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = nvs_set_str(nvs_handle, "ssid", request.ssid.c_str());
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, "password", request.password.c_str());
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, "bssid", request.bssid.c_str());
    }
    if (ret == ESP_OK) {
        ret = nvs_set_str(nvs_handle, "security", request.security_type.c_str());
    }
    if (ret == ESP_OK) {
        ret = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);
    return ret;
}

static bool has_invalid_wifi_chars(const std::string& value) {
    for (unsigned char ch : value) {
        if (ch == '\0' || ch == '\r' || ch == '\n') {
            return true;
        }
        if (ch < 0x20 && ch != '\t') {
            return true;
        }
    }
    return false;
}

static void ensure_provision_mutex() {
    if (provision_mutex != NULL) {
        return;
    }
    provision_mutex = xSemaphoreCreateMutex();
}

static void trim_inplace(std::string& s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.erase(s.begin());
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.pop_back();
    }
}

static int hex_nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/** 若整段为十六进制（无 '{' 前缀），则解码为二进制 JSON 文本。 */
static bool try_decode_hex_inplace(std::string& body) {
    trim_inplace(body);
    if (body.empty() || body.front() == '{') {
        return false;
    }
    std::string compact;
    compact.reserve(body.size());
    for (unsigned char uc : body) {
        if (std::isspace(uc)) {
            continue;
        }
        if (std::isxdigit(uc)) {
            compact.push_back(static_cast<char>(std::tolower(uc)));
        } else {
            return false;
        }
    }
    if (compact.size() < 4 || (compact.size() % 2) != 0) {
        return false;
    }
    std::string out;
    out.reserve(compact.size() / 2);
    for (size_t i = 0; i < compact.size(); i += 2) {
        int hi = hex_nibble(compact[i]);
        int lo = hex_nibble(compact[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<char>((hi << 4) | lo));
    }
    body = std::move(out);
    return true;
}

/** 校验并写入 NVS（JSON / 表单 / BLE 共用）。 */
static int validate_and_save_provision(const ProvisionRequest& request) {
    if (request.ssid.empty() || request.ssid.size() > 32 || has_invalid_wifi_chars(request.ssid)) {
        return -2;
    }
    if (request.password.size() > 64 || has_invalid_wifi_chars(request.password)) {
        return -2;
    }

    ESP_LOGI(TAG, "配网请求 SSID=%s 密码长度=%u BSSID=%s 安全=%s",
        request.ssid.c_str(),
        static_cast<unsigned>(request.password.size()),
        request.bssid.empty() ? "-" : request.bssid.c_str(),
        request.security_type.empty() ? "-" : request.security_type.c_str());

    esp_err_t ret = save_wifi_config_to_nvs(request);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "保存WiFi配置失败: %s", esp_err_to_name(ret));
        return -3;
    }

    saved_ssid = request.ssid;
    saved_password = request.password;
    has_new_config = true;
    return 0;
}

/** JSON 或 Hex 包裹的 JSON（调用方已持锁）。 */
static int apply_wifi_provision_unlocked(std::string& body) {
    trim_inplace(body);
    try_decode_hex_inplace(body);
    trim_inplace(body);

    std::string type_field;
    if (extract_json_string_field(body, "type", type_field) && type_field != "wifi_config") {
        return -1;
    }

    ProvisionRequest request = {};
    if (!parse_json_config_body(body, request)) {
        return -1;
    }

    return validate_and_save_provision(request);
}

static const char* auth_mode_to_text(wifi_auth_mode_t authmode) {
    switch (authmode) {
        case WIFI_AUTH_OPEN: return "开放网络";
        case WIFI_AUTH_WEP: return "WEP";
        case WIFI_AUTH_WPA_PSK: return "WPA";
        case WIFI_AUTH_WPA2_PSK: return "WPA2";
        case WIFI_AUTH_WPA_WPA2_PSK: return "WPA/WPA2";
        case WIFI_AUTH_WPA3_PSK: return "WPA3";
        case WIFI_AUTH_WPA2_WPA3_PSK: return "WPA2/WPA3";
        default: return "已加密";
    }
}

static const char* rssi_to_signal_text(int rssi) {
    if (rssi >= -55) {
        return "信号强";
    }
    if (rssi >= -70) {
        return "信号良好";
    }
    if (rssi >= -82) {
        return "信号一般";
    }
    return "信号较弱";
}

static bool ensure_scan_state_mutex() {
    if (scan_state_mutex != NULL) {
        return true;
    }

    scan_state_mutex = xSemaphoreCreateMutex();
    return scan_state_mutex != NULL;
}

static std::string build_scan_response_from_records(wifi_ap_record_t* records, int count) {
    std::vector<ScanNetwork> networks;
    networks.reserve(static_cast<size_t>(count));

    for (int i = 0; i < count; ++i) {
        std::string ssid(reinterpret_cast<const char*>(records[i].ssid));
        if (ssid.empty()) {
            continue;
        }

        auto existing = std::find_if(networks.begin(), networks.end(),
            [&](const ScanNetwork& network) { return network.ssid == ssid; });

        if (existing == networks.end()) {
            networks.push_back({ssid, records[i].rssi, records[i].authmode});
        } else if (records[i].rssi > existing->rssi) {
            existing->rssi = records[i].rssi;
            existing->authmode = records[i].authmode;
        }
    }

    std::sort(networks.begin(), networks.end(), [](const ScanNetwork& left, const ScanNetwork& right) {
        if (left.rssi != right.rssi) {
            return left.rssi > right.rssi;
        }
        return left.ssid < right.ssid;
    });

    std::ostringstream json;
    json << "{\"success\":true,\"pending\":false,\"networks\":[";
    for (size_t i = 0; i < networks.size(); ++i) {
        const ScanNetwork& network = networks[i];
        if (i > 0) {
            json << ',';
        }
        json << "{\"ssid\":\"" << json_escape(network.ssid)
             << "\",\"rssi\":" << network.rssi
             << ",\"signal\":\"" << rssi_to_signal_text(network.rssi)
             << "\",\"auth\":\"" << auth_mode_to_text(network.authmode)
             << "\",\"secure\":" << (network.authmode != WIFI_AUTH_OPEN ? "true" : "false")
             << "}";
    }
    json << "],\"count\":" << networks.size() << "}";
    return json.str();
}

static std::string build_scan_pending_response(const char* message) {
    std::ostringstream json;
    json << "{\"success\":true,\"pending\":true,\"message\":\""
         << json_escape(message != nullptr ? message : "正在加载附近WiFi")
         << "\",\"networks\":[]}";
    return json.str();
}

/** 自动扫描关闭时，普通 GET /scan 返回：不触发射频扫描，引导用户手动点「重新扫描」 */
static std::string build_scan_manual_prompt_response() {
    return std::string(
        "{\"success\":true,\"pending\":false,\"manualScanOnly\":true,"
        "\"hint\":\"请点击「刷新」加载列表\","
        "\"networks\":[],\"count\":0}");
}

static void scan_worker_task(void *param) {
    uint32_t delay_ms = 0;
    if (param != nullptr) {
        delay_ms = *static_cast<uint32_t*>(param);
        free(param);
    }

    if (delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }

    if (scan_stop_requested) {
        if (ensure_scan_state_mutex() && xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            scan_in_progress = false;
            scan_task_handle = NULL;
            xSemaphoreGive(scan_state_mutex);
        }
        vTaskDelete(NULL);
        return;
    }

    ble_pause_for_wifi_scan();
    wifi_ap_record_t records[32];
    int count = WiFiManager::scanNetworks(records, 32);
    ble_resume_advertising_after_wifi_scan();
    const int64_t now_us = esp_timer_get_time();
    std::string scan_result;
    std::string scan_error;

    if (count > 0) {
        scan_result = build_scan_response_from_records(records, count);
    } else {
        scan_error = "未扫描到可用WiFi，请稍后重试";
    }

    if (ensure_scan_state_mutex() && xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        if (!scan_stop_requested) {
            if (!scan_result.empty()) {
                cached_scan_response = scan_result;
                last_scan_time_us = now_us;
                last_scan_error.clear();
            } else {
                last_scan_error = scan_error;
            }
        }
        scan_in_progress = false;
        scan_task_handle = NULL;
        xSemaphoreGive(scan_state_mutex);
    }

    vTaskDelete(NULL);
}

static esp_err_t start_background_scan(uint32_t delay_ms, bool force_refresh) {
    if (!ensure_scan_state_mutex()) {
        return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    const int64_t now_us = esp_timer_get_time();
    if (scan_in_progress || scan_task_handle != NULL) {
        xSemaphoreGive(scan_state_mutex);
        return ESP_OK;
    }

    if (!force_refresh &&
        !cached_scan_response.empty() &&
        (now_us - last_scan_time_us) < SCAN_CACHE_WINDOW_US) {
        xSemaphoreGive(scan_state_mutex);
        return ESP_OK;
    }

    scan_stop_requested = false;
    scan_in_progress = true;
    last_scan_error.clear();
    xSemaphoreGive(scan_state_mutex);

    uint32_t* delay_arg = static_cast<uint32_t*>(malloc(sizeof(uint32_t)));
    if (delay_arg == nullptr) {
        if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            scan_in_progress = false;
            last_scan_error = "启动后台扫描失败";
            xSemaphoreGive(scan_state_mutex);
        }
        return ESP_ERR_NO_MEM;
    }

    *delay_arg = delay_ms;
    if (xTaskCreate(scan_worker_task, "config_scan", 6144, delay_arg, 4, &scan_task_handle) != pdPASS) {
        free(delay_arg);
        if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            scan_in_progress = false;
            last_scan_error = "启动后台扫描失败";
            xSemaphoreGive(scan_state_mutex);
        }
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void stop_background_scan() {
    if (!ensure_scan_state_mutex()) {
        return;
    }

    if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        scan_stop_requested = true;
        xSemaphoreGive(scan_state_mutex);
    }

    for (int i = 0; i < 120; ++i) {
        bool finished = false;
        if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            finished = (scan_task_handle == NULL && !scan_in_progress);
            xSemaphoreGive(scan_state_mutex);
        }
        if (finished) {
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    ESP_LOGW(TAG, "后台WiFi扫描未在预期时间内停止");
}

static std::string receive_request_body(httpd_req_t *req, bool& ok) {
    ok = false;

    if (req->content_len <= 0 || req->content_len > 512) {
        return "";
    }

    std::string body;
    body.resize(req->content_len);
    int received = 0;

    while (received < req->content_len) {
        int ret = httpd_req_recv(req, body.data() + received, req->content_len - received);
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (ret <= 0) {
            return "";
        }
        received += ret;
    }

    ok = true;
    return body;
}

static uint16_t read_u16_be(const uint8_t *data) {
    return static_cast<uint16_t>((static_cast<uint16_t>(data[0]) << 8) | data[1]);
}

static void write_u16_be(uint8_t *data, uint16_t value) {
    data[0] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[1] = static_cast<uint8_t>(value & 0xFF);
}

static void write_u32_be(uint8_t *data, uint32_t value) {
    data[0] = static_cast<uint8_t>((value >> 24) & 0xFF);
    data[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
    data[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
    data[3] = static_cast<uint8_t>(value & 0xFF);
}

static bool build_dns_response(const uint8_t *query,
                               size_t query_len,
                               uint8_t *response,
                               size_t response_size,
                               size_t *response_len) {
    if (query == nullptr || response == nullptr || response_len == nullptr || query_len < 12) {
        return false;
    }

    if ((query[2] & 0x80) != 0) {
        return false;
    }

    const uint16_t question_count = read_u16_be(query + 4);
    if (question_count == 0) {
        return false;
    }

    size_t offset = 12;
    while (offset < query_len) {
        const uint8_t label_len = query[offset];
        if (label_len == 0) {
            ++offset;
            break;
        }
        if ((label_len & 0xC0) != 0 || label_len > 63 || offset + 1 + label_len > query_len) {
            return false;
        }
        offset += 1 + label_len;
    }

    if (offset + 4 > query_len) {
        return false;
    }

    const uint16_t query_type = read_u16_be(query + offset);
    const uint16_t query_class = read_u16_be(query + offset + 2);
    const size_t question_len = offset + 4 - 12;
    const bool answer_with_ipv4 = (query_type == 1 && query_class == 1);
    const size_t answer_len = answer_with_ipv4 ? 16 : 0;

    *response_len = 12 + question_len + answer_len;
    if (*response_len > response_size) {
        return false;
    }

    memset(response, 0, *response_len);
    response[0] = query[0];
    response[1] = query[1];
    write_u16_be(response + 2, 0x8180);
    write_u16_be(response + 4, 1);
    write_u16_be(response + 6, answer_with_ipv4 ? 1 : 0);
    memcpy(response + 12, query + 12, question_len);

    if (!answer_with_ipv4) {
        return true;
    }

    size_t answer_offset = 12 + question_len;
    response[answer_offset++] = 0xC0;
    response[answer_offset++] = 0x0C;
    write_u16_be(response + answer_offset, 1);
    answer_offset += 2;
    write_u16_be(response + answer_offset, 1);
    answer_offset += 2;
    write_u32_be(response + answer_offset, 60);
    answer_offset += 4;
    write_u16_be(response + answer_offset, 4);
    answer_offset += 2;
    memcpy(response + answer_offset, CAPTIVE_PORTAL_IP, sizeof(CAPTIVE_PORTAL_IP));

    return true;
}

static void dns_server_task(void *param) {
    (void)param;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE(TAG, "创建DNS套接字失败: errno=%d", errno);
        dns_server_running = false;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    struct timeval timeout = {};
    timeout.tv_sec = 1;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    sockaddr_in server_addr = {};
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(DNS_SERVER_PORT);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(sock, reinterpret_cast<sockaddr*>(&server_addr), sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "绑定DNS端口失败: errno=%d", errno);
        close(sock);
        dns_server_running = false;
        dns_task_handle = NULL;
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "DNS劫持服务已启动，端口: %u", DNS_SERVER_PORT);

    uint8_t query[512];
    uint8_t response[512];

    while (dns_server_running) {
        sockaddr_in client_addr = {};
        socklen_t client_len = sizeof(client_addr);
        int recv_len = recvfrom(sock,
                                reinterpret_cast<char*>(query),
                                sizeof(query),
                                0,
                                reinterpret_cast<sockaddr*>(&client_addr),
                                &client_len);

        if (recv_len < 0) {
            if (errno == EWOULDBLOCK || errno == EAGAIN) {
                continue;
            }
            ESP_LOGW(TAG, "DNS接收失败: errno=%d", errno);
            continue;
        }

        size_t response_len = 0;
        if (!build_dns_response(query,
                                static_cast<size_t>(recv_len),
                                response,
                                sizeof(response),
                                &response_len)) {
            continue;
        }

        sendto(sock,
               reinterpret_cast<const char*>(response),
               response_len,
               0,
               reinterpret_cast<sockaddr*>(&client_addr),
               client_len);
    }

    close(sock);
    ESP_LOGI(TAG, "DNS劫持服务已停止");
    dns_task_handle = NULL;
    vTaskDelete(NULL);
}

static esp_err_t start_dns_server() {
    if (dns_task_handle != NULL) {
        return ESP_OK;
    }

    dns_server_running = true;
    if (xTaskCreate(dns_server_task, "captive_dns", 4096, NULL, 5, &dns_task_handle) != pdPASS) {
        dns_server_running = false;
        ESP_LOGE(TAG, "创建DNS任务失败");
        return ESP_FAIL;
    }

    return ESP_OK;
}

static void stop_dns_server() {
    if (dns_task_handle == NULL) {
        dns_server_running = false;
        return;
    }

    dns_server_running = false;
    for (int i = 0; i < 30 && dns_task_handle != NULL; ++i) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (dns_task_handle != NULL) {
        ESP_LOGW(TAG, "DNS任务停止超时，强制结束");
        vTaskDelete(dns_task_handle);
        dns_task_handle = NULL;
    }
}

static esp_err_t config_get_handler(httpd_req_t *req) {
    set_common_headers(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, config_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t captive_probe_handler(httpd_req_t *req) {
    ESP_LOGI(TAG, "检测到系统网络探测请求: %s", req->uri);
    return send_redirect_response(req, CAPTIVE_PORTAL_URL);
}

static esp_err_t captive_404_handler(httpd_req_t *req, httpd_err_code_t err) {
    (void)err;
    ESP_LOGI(TAG, "拦截未知请求并跳转到配网页: %s", req->uri);
    return send_redirect_response(req, CAPTIVE_PORTAL_URL);
}

static esp_err_t scan_get_handler(httpd_req_t *req) {
    bool force_refresh = false;
    const size_t query_len = httpd_req_get_url_query_len(req);
    if (query_len > 0) {
        std::string query;
        query.resize(query_len + 1);
        if (httpd_req_get_url_query_str(req, query.data(), query.size()) == ESP_OK) {
            char refresh_value[8] = {0};
            if (httpd_query_key_value(query.c_str(), "refresh", refresh_value, sizeof(refresh_value)) == ESP_OK &&
                strcmp(refresh_value, "1") == 0) {
                force_refresh = true;
            }
        }
    }

    if (force_refresh) {
        start_background_scan(REFRESH_SCAN_DELAY_MS, true);
    }

    if (!ensure_scan_state_mutex()) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"扫描服务初始化失败\"}",
            "500 Internal Server Error");
    }

    std::string response_body;
    bool should_start_scan = false;
    if (xSemaphoreTake(scan_state_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
        const int64_t now_us = esp_timer_get_time();
        const bool has_fresh_cache =
            !cached_scan_response.empty() &&
            (now_us - last_scan_time_us) < SCAN_CACHE_WINDOW_US;

        if (has_fresh_cache && !force_refresh) {
            response_body = cached_scan_response;
        } else if (scan_in_progress || scan_task_handle != NULL) {
            response_body = build_scan_pending_response(force_refresh ? "正在刷新附近WiFi列表..." : "正在加载附近WiFi列表...");
        } else if (!kProvisionAutoWifiScan && !force_refresh) {
            should_start_scan = false;
            response_body = build_scan_manual_prompt_response();
        } else {
            should_start_scan = true;
            response_body = build_scan_pending_response(force_refresh ? "正在刷新附近WiFi列表..." : "正在加载附近WiFi列表...");
            if (!last_scan_error.empty()) {
                response_body = build_scan_pending_response(last_scan_error.c_str());
            }
        }

        xSemaphoreGive(scan_state_mutex);
    } else {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"扫描服务忙，请稍后重试\"}",
            "503 Service Unavailable");
    }

    if (should_start_scan) {
        const uint32_t delay_ms = force_refresh ? REFRESH_SCAN_DELAY_MS : FIRST_PAGE_SCAN_DELAY_MS;
        if (start_background_scan(delay_ms, force_refresh) != ESP_OK) {
            return send_json_response(req,
                "{\"success\":false,\"message\":\"启动WiFi扫描失败，请稍后重试\"}",
                "500 Internal Server Error");
        }
    }

    return send_json_response(req, response_body);
}

static esp_err_t config_post_handler(httpd_req_t *req) {
    char content_type_buf[96] = {0};
    if (httpd_req_get_hdr_value_str(req, "Content-Type", content_type_buf, sizeof(content_type_buf)) != ESP_OK) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"请求类型无效\"}",
            "415 Unsupported Media Type");
    }

    bool ok = false;
    std::string body = receive_request_body(req, ok);
    if (!ok) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"请求内容无效或过长\"}",
            "400 Bad Request");
    }

    ensure_provision_mutex();
    if (xSemaphoreTake(provision_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"配网服务忙\"}",
            "503 Service Unavailable");
    }

    int prc = -1;
    if (strstr(content_type_buf, "application/json") != nullptr) {
        prc = apply_wifi_provision_unlocked(body);
    } else if (strstr(content_type_buf, "application/x-www-form-urlencoded") != nullptr) {
        ProvisionRequest request = {};
        if (!parse_form_config_body(body, request)) {
            prc = -1;
        } else {
            prc = validate_and_save_provision(request);
        }
    } else {
        xSemaphoreGive(provision_mutex);
        return send_json_response(req,
            "{\"success\":false,\"message\":\"仅支持JSON或表单请求\"}",
            "415 Unsupported Media Type");
    }

    xSemaphoreGive(provision_mutex);

    if (prc == -1) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"请求内容解析失败\"}",
            "400 Bad Request");
    }
    if (prc == -2) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"WiFi名称或密码不合法\"}",
            "400 Bad Request");
    }
    if (prc == -3) {
        return send_json_response(req,
            "{\"success\":false,\"message\":\"保存配置失败\"}",
            "500 Internal Server Error");
    }

    return send_json_response(req,
        "{\"success\":true,\"message\":\"WiFi configured successfully\"}");
}

esp_err_t start_config_server() {
    if (server_handle != NULL) {
        ESP_LOGW(TAG, "配网服务器已在运行");
        return ESP_OK;
    }

    ensure_scan_state_mutex();
    cached_scan_response.clear();
    last_scan_error.clear();
    last_scan_time_us = 0;
    scan_in_progress = false;
    scan_stop_requested = false;
    scan_task_handle = NULL;

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 16;

    ESP_LOGI(TAG, "启动配网HTTP服务器，端口: %d", config.server_port);

    if (httpd_start(&server_handle, &config) != ESP_OK) {
        ESP_LOGE(TAG, "启动配网HTTP服务器失败");
        return ESP_FAIL;
    }

    httpd_uri_t page_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = config_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &page_uri);

    httpd_uri_t scan_uri = {
        .uri = "/scan",
        .method = HTTP_GET,
        .handler = scan_get_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &scan_uri);

    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &config_uri);

    httpd_uri_t app_config_uri = {
        .uri = "/wifi/config",
        .method = HTTP_POST,
        .handler = config_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(server_handle, &app_config_uri);

    static const char* captive_probe_uris[] = {
        "/generate_204",
        "/gen_204",
        "/hotspot-detect.html",
        "/library/test/success.html",
        "/connecttest.txt",
        "/ncsi.txt",
        "/fwlink"
    };

    for (const char* uri : captive_probe_uris) {
        httpd_uri_t probe_uri = {
            .uri = uri,
            .method = HTTP_GET,
            .handler = captive_probe_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server_handle, &probe_uri);
    }

    httpd_register_err_handler(server_handle, HTTPD_404_NOT_FOUND, captive_404_handler);

    if (start_dns_server() != ESP_OK) {
        ESP_LOGE(TAG, "启动DNS劫持服务失败");
        httpd_stop(server_handle);
        server_handle = NULL;
        return ESP_FAIL;
    }

    if (kProvisionAutoWifiScan) {
        start_background_scan(INITIAL_SCAN_DELAY_MS, false);
    } else {
        ESP_LOGI(TAG, "配网页已关闭自动 WiFi 扫描，列表需用户点击「重新扫描」");
    }

    ESP_LOGI(TAG, "配网HTTP服务器启动成功，访问地址: http://192.168.4.1");
    ESP_LOGI(TAG, "已启用热点强制门户，大多数手机连接热点后会自动弹出配网页");
    return ESP_OK;
}

esp_err_t stop_config_server() {
    if (server_handle == NULL) {
        stop_background_scan();
        stop_dns_server();
        cached_scan_response.clear();
        last_scan_error.clear();
        last_scan_time_us = 0;
        return ESP_OK;
    }

    stop_background_scan();
    stop_dns_server();
    httpd_stop(server_handle);
    server_handle = NULL;
    cached_scan_response.clear();
    last_scan_error.clear();
    last_scan_time_us = 0;
    ESP_LOGI(TAG, "配网HTTP服务器已停止");
    return ESP_OK;
}

extern "C" int provision_apply_wifi_json(const char* json_body, size_t body_len) {
    if (json_body == NULL || body_len == 0) {
        return -1;
    }
    ensure_provision_mutex();
    xSemaphoreTake(provision_mutex, portMAX_DELAY);
    std::string body(json_body, body_len);
    int r = apply_wifi_provision_unlocked(body);
    xSemaphoreGive(provision_mutex);
    return r;
}

bool get_wifi_config(std::string& ssid, std::string& password) {
    if (!has_new_config) {
        return false;
    }

    ssid = saved_ssid;
    password = saved_password;
    has_new_config = false;
    return true;
}
