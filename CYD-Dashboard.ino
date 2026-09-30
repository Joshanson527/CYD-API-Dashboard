#include <WebServer.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <lvgl.h>

#include "FS.h"
#include "SD.h"

// ============================================================
// Display
// ============================================================

#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320

#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))

uint32_t draw_buf[DRAW_BUF_SIZE / 4];

lv_display_t *display;
lv_indev_t *touch;

// ============================================================
// Touchscreen
// ============================================================

#define XPT2046_IRQ 36
#define XPT2046_MOSI 32
#define XPT2046_MISO 39
#define XPT2046_CLK 25
#define XPT2046_CS 33

SPIClass touchscreenSPI = SPIClass(HSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);

int x, y, z;

// ============================================================
// SD
// ============================================================

#define SD_MISO 19
#define SD_MOSI 23
#define SD_SCK 18
#define SD_CS 5

SPIClass Sd_spi = SPIClass(VSPI);

#define CONFIG_FILE "/config.json"

// ============================================================
// Configuration
// ============================================================

JsonDocument config;

struct DashboardWidget {
	lv_obj_t *container = nullptr;
	lv_obj_t *titleLabel = nullptr;
	lv_obj_t *bodyLabel = nullptr;

	String id;
	String title;
	String iconFile;
	String templateText;

	String url;
	String method = "GET";
	String payload;
	String headerKey;
	String headerValue;

	String btn1Label, btn1Url;
	String btn1Method = "GET";
	String btn1Payload;
	String btn1HeaderKey;
	String btn1HeaderValue;

	String btn2Label, btn2Url;
	String btn2Method = "GET";
	String btn2Payload;
	String btn2HeaderKey;
	String btn2HeaderValue;

	unsigned long refreshRate = 0;
	unsigned long lastRefresh = 0;
	
	bool needsRefresh = false;
	bool pendingBtn1 = false;
	bool pendingBtn2 = false;

	String width = "1/2";
	int height = 110;
	int titleFontSize = 16;
	int bodyFontSize = 14;
};

// ============================================================
// Brightness
// ============================================================

const int BACKLIGHT_PIN = 21;
const int PWM_FREQ = 5000;
const int PWM_RES = 8;

void setBrightness(int percentage) {
	percentage = constrain(percentage, 0, 100);
	int dutyCycle = map(percentage, 0, 100, 0, 255);
	ledcWrite(BACKLIGHT_PIN, dutyCycle);
}

// ============================================================
// Theme
// ============================================================

struct Theme {
	uint32_t background;
	uint32_t widgetBackground;
	uint32_t text;
	uint32_t secondaryText;
	uint32_t border;
};

Theme currentTheme;

void applyTheme() {
	String theme = config["settings"]["theme"] | "dark";

	if (theme == "light") {
		currentTheme.background = 0xF2F2F2;
		currentTheme.widgetBackground = 0xFFFFFF;
		currentTheme.text = 0x111111;
		currentTheme.secondaryText = 0x444444;
		currentTheme.border = 0xCCCCCC;
	} else if (theme == "ocean") {
		currentTheme.background = 0x0F172A; 
		currentTheme.widgetBackground = 0x1E293B; 
		currentTheme.text = 0x38BDF8; 
		currentTheme.secondaryText = 0x94A3B8; 
		currentTheme.border = 0x334155; 
	} else if (theme == "cyberpunk") {
		currentTheme.background = 0x09090B;
		currentTheme.widgetBackground = 0x18181B;
		currentTheme.text = 0xF43F5E;
		currentTheme.secondaryText = 0x0EA5E9;
		currentTheme.border = 0x8B5CF6;
	} else if (theme == "terminal") {
		currentTheme.background = 0x000000;
		currentTheme.widgetBackground = 0x050505;
		currentTheme.text = 0x00FF41;
		currentTheme.secondaryText = 0x008F11;
		currentTheme.border = 0x003B00;
	} else {
		currentTheme.background = 0x101010;
		currentTheme.widgetBackground = 0x202020;
		currentTheme.text = 0xFFFFFF;
		currentTheme.secondaryText = 0xBBBBBB;
		currentTheme.border = 0x404040;
	}
}

const lv_font_t* getLVGLFont(int size) {
	switch (size) {
		case 12: return &lv_font_montserrat_12;
		case 14: return &lv_font_montserrat_14;
		case 16: return &lv_font_montserrat_16;
		case 18: return &lv_font_montserrat_18;
		case 20: return &lv_font_montserrat_20;
		default: return &lv_font_montserrat_14;
	}
}



WebServer server(80);

unsigned long lastActivityTime = 0;
unsigned long topPauseUntil = 0;
unsigned long IDLE_TIMEOUT_MS = 0;
int scrollDirection = 1;
int scrollStep = 2;
lv_timer_t *autoScrollTimerObj = nullptr;

// ============================================================
// LVGL logging
// ============================================================

void log_print(lv_log_level_t level, const char *buf) {
	LV_UNUSED(level);

	Serial.println(buf);
	Serial.flush();
}

// ============================================================
// Touch input
// ============================================================

void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data) {
	LV_UNUSED(indev);

	if (touchscreen.tirqTouched() && touchscreen.touched()) {
		TS_Point p = touchscreen.getPoint();

		x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
		y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
		z = p.z;

		data->state = LV_INDEV_STATE_PRESSED;
		data->point.x = x;
		data->point.y = y;

		lastActivityTime = millis();

	} else {
		data->state = LV_INDEV_STATE_RELEASED;
	}
}

// ============================================================
// SD
// ============================================================

bool initSD() {
	Serial.println();
	Serial.println("Initializing SD card...");

	Sd_spi.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

	if (!SD.begin(SD_CS, Sd_spi, 55000000)) {
		Serial.println("Card Mount Failed");
		return false;
	}

	Serial.println("SD card initialized.");

	return true;
}

bool loadConfig() {
	Serial.printf("Loading %s\n", CONFIG_FILE);

	File file = SD.open(CONFIG_FILE, FILE_READ);

	if (!file) {
		Serial.println("Config file does not exist.");

		return false;
	}

	DeserializationError error = deserializeJson(config, file);

	file.close();

	if (error) {
		Serial.print("Failed to parse config: ");

		Serial.println(error.c_str());

		return false;
	}

	Serial.println("Config loaded successfully.");

	return true;
}

bool saveConfig() {
	Serial.printf("Saving %s\n", CONFIG_FILE);

	if (SD.exists(CONFIG_FILE)) {
		SD.remove(CONFIG_FILE);
	}

	File file = SD.open(CONFIG_FILE, FILE_WRITE);

	if (!file) {
		Serial.println("Failed to open config for writing.");
		return false;
	}

	size_t bytesWritten = serializeJsonPretty(config, file);
	file.flush();
	file.close();

	if (bytesWritten == 0) {
		Serial.println("Failed to write data to file.");
		return false;
	}

	Serial.println("Config saved.");
	return true;
}

// ============================================================
// Default configuration
// ============================================================

void createDefaultConfig() {
	Serial.println("Creating default configuration.");

	config.clear();

	// --------------------------------------------------------
	// Settings
	// --------------------------------------------------------

	JsonObject settings = config["settings"].to<JsonObject>();

	settings["rotation"] = 1;
	settings["brightness"] = 100;
	settings["theme"] = "dark";
	settings["wifi_ssid"] = "";
	settings["wifi_password"] = "";
	settings["hostname"] = "espdash";
	settings["scroll_idle"] = 0;
	settings["scroll_speed"] = 2;

	// --------------------------------------------------------
	// Widgets
	// --------------------------------------------------------

	JsonArray widgets = config["widgets"].to<JsonArray>();

	saveConfig();
}

// ============================================================
// WiFi
// ============================================================

bool initWiFi() {
	String ssid = config["settings"]["wifi_ssid"] | "";
	String password = config["settings"]["wifi_password"] | "";
	String host = config["settings"]["hostname"] | "esp32-dashboard";

	if (ssid.length() == 0) {
		Serial.println("No WiFi credentials. Starting AP Mode.");
		WiFi.mode(WIFI_AP);
		WiFi.softAP(host.c_str());
		Serial.print("AP IP Address: ");
		Serial.println(WiFi.softAPIP());
		return true;
	}

	Serial.println("Connecting to WiFi: " + ssid);
	WiFi.mode(WIFI_STA);
	WiFi.begin(ssid.c_str(), password.c_str());

	unsigned long start = millis();
	while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
		delay(250);
		Serial.print(".");
	}
	Serial.println();

	if (WiFi.status() != WL_CONNECTED) {
		Serial.println("WiFi connection failed. Starting AP Mode.");
		WiFi.mode(WIFI_AP);
		WiFi.softAP(host.c_str());
		Serial.print("AP IP Address: ");
		Serial.println(WiFi.softAPIP());
		return true;
	}

	Serial.println("WiFi connected.");
	Serial.print("IP address: ");
	Serial.println(WiFi.localIP());
	return true;
}

// ============================================================
// Web interface
// ============================================================

void handleRoot() {
	File file = SD.open("/index.html", FILE_READ);
	if (!file) {
		server.send(404, "text/plain", "Error: index.html not found on SD card. Please upload it.");
		return;
	}
	server.streamFile(file, "text/html");
	file.close();
}

void handleGetConfig() {
	File file = SD.open(CONFIG_FILE, FILE_READ);
	if (!file) {
		server.send(200, "application/json", "{}");
		return;
	}
	server.streamFile(file, "application/json");
	file.close();
}

void handleSave() {
	if (!server.hasArg("plain")) {
		server.send(400, "text/plain", "Missing JSON body");
		return;
	}

	String jsonText = server.arg("plain");
	JsonDocument newConfig;
	DeserializationError error = deserializeJson(newConfig, jsonText);

	if (error) {
		server.send(400, "text/plain", "Invalid JSON");
		return;
	}

	config.clear();
	config.set(newConfig);

	if (!saveConfig()) {
		server.send(500, "text/plain", "Failed to save to SD card");
		return;
	}

	server.send(200, "application/json", "{\"status\":\"ok\"}");
	
	Serial.println("Config saved. Rebooting in 1 second...");
	
	delay(1000);
	ESP.restart();
}

void initWebServer() {
	server.on("/", HTTP_GET, handleRoot);
	server.on("/api/config", HTTP_GET, handleGetConfig);
	server.on("/save", HTTP_POST, handleSave);

	server.onNotFound([]() {
		server.send(404, "text/plain", "Not found");
	});

	server.begin();
	Serial.println("Web server started.");
}

String getSymbolMacro(const String& iconName) {
	if (iconName == "wifi") return LV_SYMBOL_WIFI;
	if (iconName == "bluetooth") return LV_SYMBOL_BLUETOOTH;
	if (iconName == "usb") return LV_SYMBOL_USB;
	if (iconName == "sd_card") return LV_SYMBOL_SD_CARD;
	if (iconName == "battery") return LV_SYMBOL_BATTERY_FULL;
	if (iconName == "power") return LV_SYMBOL_POWER;
	if (iconName == "charge") return LV_SYMBOL_CHARGE;
	if (iconName == "settings") return LV_SYMBOL_SETTINGS;
	if (iconName == "home") return LV_SYMBOL_HOME;
	if (iconName == "warning") return LV_SYMBOL_WARNING;
	if (iconName == "bell") return LV_SYMBOL_BELL;
	if (iconName == "list") return LV_SYMBOL_LIST;
	if (iconName == "save" || iconName == "drive") return LV_SYMBOL_SAVE;
	if (iconName == "trash") return LV_SYMBOL_TRASH;
	if (iconName == "refresh") return LV_SYMBOL_REFRESH;
	if (iconName == "upload") return LV_SYMBOL_UPLOAD;
	if (iconName == "download") return LV_SYMBOL_DOWNLOAD;
	if (iconName == "directory") return LV_SYMBOL_DIRECTORY;
	if (iconName == "audio") return LV_SYMBOL_AUDIO;
	if (iconName == "video") return LV_SYMBOL_VIDEO;
	if (iconName == "image") return LV_SYMBOL_IMAGE;
	if (iconName == "eye_open") return LV_SYMBOL_EYE_OPEN;
	if (iconName == "call") return LV_SYMBOL_CALL;
	return "";
}

// ============================================================
// Widget
// ============================================================

DashboardWidget *dashboardWidgets = nullptr;
size_t widgetCount = 0;

lv_obj_t *dashboard = nullptr;
lv_timer_t *dashboardTimer = nullptr;

// ============================================================
// JSON template renderer
// ============================================================

String renderTemplate(const String &templateText, JsonVariantConst json) {
	String output;
	int position = 0;

	while (position < templateText.length()) {
		int open = templateText.indexOf('{', position);

		if (open == -1) {
			output += templateText.substring(position);
			break;
		}

		output += templateText.substring(position, open);

		int close = templateText.indexOf('}', open);

		if (close == -1) {
			output += templateText.substring(open);
			break;
		}

		String expression = templateText.substring(open + 1, close);
		expression.trim();

		// 1. Extract Null Coalescing (??)
		String path = expression;
		String defaultValue = "";
		bool hasDefault = false;
		
		int coalesceIdx = expression.indexOf("??");
		if (coalesceIdx != -1) {
			path = expression.substring(0, coalesceIdx);
			path.trim();
			defaultValue = expression.substring(coalesceIdx + 2);
			defaultValue.trim(); 
			hasDefault = true;
		}

		// 2. Extract Filters (|)
		String filter = "";
		int filterIdx = path.indexOf("|");
		if (filterIdx != -1) {
			filter = path.substring(filterIdx + 1);
			filter.trim();
			path = path.substring(0, filterIdx);
			path.trim();
		}

		// 3. Traverse JSON Path
		JsonVariantConst value = json;
		bool found = true;
		int start = 0;

		while (start < path.length()) {
			int dot = path.indexOf('.', start);
			String part;
			
			if (dot == -1) part = path.substring(start);
			else part = path.substring(start, dot);

			part.trim();

			if (part.length() == 0) {
				found = false;
				break;
			}

			bool numeric = true;
			if (part.length() == 1 && part[0] == '-') {
				numeric = false;
			} else {
				for (size_t i = 0; i < part.length(); i++) {
					if (i == 0 && part[i] == '-') continue;
					if (!isDigit(part[i])) {
						numeric = false;
						break;
					}
				}
			}

			if (numeric) {
				int index = part.toInt();
				if (value.is<JsonArrayConst>()) {
					JsonArrayConst arr = value.as<JsonArrayConst>();
					int arrSize = arr.size();

					if (index < 0) {
						index = arrSize + index;
					}

					if (index >= 0 && index < arrSize) {
						value = arr[index];
					} else {
						found = false;
						break;
					}
				} else {
					found = false;
					break;
				}
			} else {
				if (value.is<JsonObjectConst>()) {
					value = value[part];
				} else {
					found = false;
					break;
				}
			}

			if (dot == -1) break;
			start = dot + 1;
		}

		// 4. Output Evaluation
		if (!found || value.isNull()) {
			if (hasDefault) {
				output += defaultValue;
			} else {
				output += "{";
				output += expression;
				output += "}";
			}
		} else {
			String valStr = "";
			
			if (value.is<const char *>()) {
				valStr = value.as<const char *>();
			} else if (value.is<bool>()) {
				valStr = value.as<bool>() ? "true" : "false";
			} else if (value.is<int>() || value.is<long>()) {
				valStr = String(value.as<long>());
			} else if (value.is<float>() || value.is<double>()) {
				if (filter.length() > 0 && isDigit(filter[0])) {
					valStr = String(value.as<double>(), filter.toInt());
				} else {
					valStr = String(value.as<double>());
				}
			} else if (value.is<JsonArrayConst>()) {
				if (filter == "join") {
					JsonArrayConst arr = value.as<JsonArrayConst>();
					for (size_t i = 0; i < arr.size(); i++) {
						valStr += arr[i].as<String>();
						if (i < arr.size() - 1) valStr += ", ";
					}
				} else if (filter == "count") {
					valStr = String(value.as<JsonArrayConst>().size());
				} else {
					serializeJson(value, valStr);
				}
			} else {
				serializeJson(value, valStr);
			}

			if (filter == "upper") valStr.toUpperCase();
			else if (filter == "lower") valStr.toLowerCase();

			output += valStr;
		}

		position = close + 1;
	}

	return output;
}

// ============================================================
// Update widget from API
// ============================================================

bool updateWidget(DashboardWidget &widget) {
	if (WiFi.status() != WL_CONNECTED) {
		lv_label_set_text(widget.bodyLabel, "WiFi disconnected");
		return false;
	}

	Serial.println();
	Serial.print("Updating widget: ");
	Serial.println(widget.id);
	Serial.print("URL: ");
	Serial.println(widget.url);

	int status = 0;
	String response = "";

	auto performRequest = [&](HTTPClient &http) {
		http.addHeader("Connection", "close");
		
		if (widget.headerKey.length() > 0 && widget.headerValue.length() > 0) {
			http.addHeader(widget.headerKey, widget.headerValue);
		}
		if (widget.payload.length() > 0 && !widget.headerKey.equalsIgnoreCase("Content-Type")) {
			http.addHeader("Content-Type", "application/json");
		}
		
		if (widget.method == "POST") return http.POST(widget.payload);
		if (widget.method == "PUT") return http.PUT(widget.payload);
		return http.GET();
	};

	if (widget.url.startsWith("https://")) {
		WiFiClientSecure secureClient;
		secureClient.setInsecure(); 
		HTTPClient http;
		
		http.begin(secureClient, widget.url);
		http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"); 
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(10000);

		status = performRequest(http);
		if (status > 0) response = http.getString();
		http.end(); 
	} 
	else {
		WiFiClient plainClient;
		HTTPClient http;
		
		http.begin(plainClient, widget.url);
		http.setUserAgent("Mozilla/5.0 (Windows NT 10.0; Win64; x64)"); 
		http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
		http.setTimeout(10000);

		status = performRequest(http);
		if (status > 0) response = http.getString();
		http.end(); 
	}

	if (status <= 0) {
		lv_label_set_text(widget.bodyLabel, "Request failed");
		Serial.print("HTTP request failed, code: ");
		Serial.println(status);
		return false;
	}

	if (status < 200 || status >= 300) {
		String error = "HTTP " + String(status);
		lv_label_set_text(widget.bodyLabel, error.c_str());
		Serial.println(error);
		return false;
	}

	JsonDocument json;
	DeserializationError error = deserializeJson(json, response);

	if (error) {
		lv_label_set_text(widget.bodyLabel, "Invalid JSON");
		Serial.print("JSON error: ");
		Serial.println(error.c_str());
		return false;
	}

	String rendered = renderTemplate(widget.templateText, json);
	lv_label_set_text(widget.bodyLabel, rendered.c_str());
	
	widget.lastRefresh = millis();
	
	Serial.print("Updated: ");
	Serial.println(widget.id);

	return true;
}

void applyRotation() {
	int rotation = config["settings"]["rotation"] | 270;

	lv_display_rotation_t lvRotation;
	int touchRotation;

	switch (rotation) {
		case 0:
			lvRotation = LV_DISPLAY_ROTATION_0;
			touchRotation = 0;
			break;

		case 90:
			lvRotation = LV_DISPLAY_ROTATION_90;
			touchRotation = 2;
			break;

		case 180:
			lvRotation = LV_DISPLAY_ROTATION_180;
			touchRotation = 0;
			break;

		case 270:
			lvRotation = LV_DISPLAY_ROTATION_270;
			touchRotation = 2;
			break;

		default:
			lvRotation = LV_DISPLAY_ROTATION_0;
			touchRotation = 0;
			break;
	}

	touchscreen.setRotation(touchRotation);
	lv_display_set_rotation(display, lvRotation);
}

// ============================================================
// Create LVGL widget
// ============================================================

void createWidget(DashboardWidget &widget, lv_obj_t *parent, DashboardWidget *widgetPtr) {
	widget.container = lv_obj_create(parent);

	if (widget.width == "1/3") lv_obj_set_width(widget.container, LV_PCT(31));
	else if (widget.width == "1/2") lv_obj_set_width(widget.container, LV_PCT(48));
	else if (widget.width == "2/3") lv_obj_set_width(widget.container, LV_PCT(64));
	else lv_obj_set_width(widget.container, LV_PCT(98));

	if (widget.height == 0) {
		lv_obj_set_height(widget.container, LV_SIZE_CONTENT);
	} else {
		lv_obj_set_height(widget.container, widget.height);
	}

	lv_obj_set_style_radius(widget.container, 12, 0);
	
	lv_obj_set_style_pad_all(widget.container, 8, 0); 
	lv_obj_set_style_bg_color(widget.container, lv_color_hex(currentTheme.widgetBackground), 0);
	lv_obj_set_style_border_color(widget.container, lv_color_hex(currentTheme.border), 0);
	lv_obj_set_flex_flow(widget.container, LV_FLEX_FLOW_COLUMN);
	
	lv_obj_remove_flag(widget.container, LV_OBJ_FLAG_SCROLLABLE);

	// --- Header Row ---
	String fullTitle = "";
	String sym = getSymbolMacro(widget.iconFile); 
	if (sym.length() > 0) {
		fullTitle += sym + "  "; 
	}
	fullTitle += widget.title;

	if (widget.iconFile.length() > 0 or widget.title.length() > 0) {
		lv_obj_t *headerRow = lv_obj_create(widget.container);
		lv_obj_set_width(headerRow, LV_PCT(100));
		lv_obj_set_height(headerRow, LV_SIZE_CONTENT);
		lv_obj_set_style_bg_opa(headerRow, LV_OPA_TRANSP, 0);
		lv_obj_set_style_border_width(headerRow, 0, 0);
		lv_obj_set_style_pad_all(headerRow, 0, 0);
		lv_obj_remove_flag(headerRow, LV_OBJ_FLAG_SCROLLABLE); 
		
		lv_obj_set_flex_flow(headerRow, LV_FLEX_FLOW_ROW);
		lv_obj_set_flex_align(headerRow, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

		widget.titleLabel = lv_label_create(headerRow);
	

		lv_label_set_text(widget.titleLabel, fullTitle.c_str());
		lv_obj_set_style_text_color(widget.titleLabel, lv_color_hex(currentTheme.text), 0);
		lv_obj_set_style_text_font(widget.titleLabel, getLVGLFont(widget.titleFontSize), 0);
		lv_obj_set_flex_grow(widget.titleLabel, 1);
		lv_label_set_long_mode(widget.titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
	}

	// --- Body ---
	if (widget.templateText.length() > 0) {
		widget.bodyLabel = lv_label_create(widget.container);
		lv_label_set_text(widget.bodyLabel, "Loading...");
		lv_obj_set_width(widget.bodyLabel, LV_PCT(100));
		lv_label_set_long_mode(widget.bodyLabel, LV_LABEL_LONG_WRAP);
		lv_obj_set_style_text_color(widget.bodyLabel, lv_color_hex(currentTheme.secondaryText), 0);
		lv_obj_set_style_text_font(widget.bodyLabel, getLVGLFont(widget.bodyFontSize), 0);
		
		if (widget.height > 0) {
			lv_obj_set_flex_grow(widget.bodyLabel, 1);
		}

			// Click to refresh body
			lv_obj_add_flag(widget.bodyLabel, LV_OBJ_FLAG_CLICKABLE);
			lv_obj_add_event_cb(widget.bodyLabel, [](lv_event_t *event) {
				DashboardWidget *w = static_cast<DashboardWidget *>(lv_event_get_user_data(event));
				w->needsRefresh = true;
			}, LV_EVENT_CLICKED, widgetPtr);
	}

	// --- Buttons Row ---
	if (widget.btn1Label.length() > 0 || widget.btn2Label.length() > 0) {
		lv_obj_t *btnRow = lv_obj_create(widget.container);
		lv_obj_remove_style_all(btnRow);
		lv_obj_set_width(btnRow, LV_PCT(100));
		lv_obj_set_height(btnRow, LV_SIZE_CONTENT);
		
		lv_obj_remove_flag(btnRow, LV_OBJ_FLAG_SCROLLABLE); 
		
		lv_obj_set_flex_flow(btnRow, LV_FLEX_FLOW_ROW);
		lv_obj_set_style_pad_column(btnRow, 6, 0);
		lv_obj_set_style_margin_top(btnRow, 4, 0);

		if (widget.btn1Label.length() > 0) {
			lv_obj_t *btn1 = lv_button_create(btnRow);
			lv_obj_set_style_bg_color(btn1, lv_color_hex(currentTheme.border), 0);

			lv_obj_set_flex_grow(btn1, 1); 

			lv_obj_t *lbl1 = lv_label_create(btn1);
			lv_label_set_text(lbl1, widget.btn1Label.c_str());
			lv_obj_set_style_text_font(lbl1, getLVGLFont(12), 0);
			lv_obj_center(lbl1);

			lv_obj_add_event_cb(btn1, [](lv_event_t *e){
				static_cast<DashboardWidget*>(lv_event_get_user_data(e))->pendingBtn1 = true;
			}, LV_EVENT_CLICKED, widgetPtr);
		}

		if (widget.btn2Label.length() > 0) {
			lv_obj_t *btn2 = lv_button_create(btnRow);
			lv_obj_set_style_bg_color(btn2, lv_color_hex(currentTheme.border), 0);
			
			lv_obj_set_flex_grow(btn2, 1); 
			
			lv_obj_t *lbl2 = lv_label_create(btn2);
			lv_label_set_text(lbl2, widget.btn2Label.c_str());
			lv_obj_set_style_text_font(lbl2, getLVGLFont(12), 0);
			lv_obj_center(lbl2);
			
			lv_obj_add_event_cb(btn2, [](lv_event_t *e){
				static_cast<DashboardWidget*>(lv_event_get_user_data(e))->pendingBtn2 = true;
			}, LV_EVENT_CLICKED, widgetPtr);
		}
	}
}

// ============================================================
// Dashboard refresh timer
// ============================================================

void dashboardRefreshTimer(lv_timer_t *timer) {
	LV_UNUSED(timer);

	unsigned long now = millis();

	for (size_t i = 0; i < widgetCount; i++) {
		DashboardWidget &widget = dashboardWidgets[i];

		if (widget.refreshRate == 0) continue;

		if (now - widget.lastRefresh >= widget.refreshRate * 1000UL) {
			widget.needsRefresh = true;
			widget.lastRefresh = now;
		}
	}
}

// ============================================================
// Dashboard
// ============================================================

void autoScrollTimer(lv_timer_t *timer) {
	LV_UNUSED(timer);

	if (millis() - lastActivityTime < IDLE_TIMEOUT_MS || millis() < topPauseUntil) {
		return;
	}

	if (scrollDirection > 0) {
		if (lv_obj_get_scroll_bottom(dashboard) <= 0) {
			scrollDirection = -1;
			return;
		}
	} else {
		if (lv_obj_get_scroll_top(dashboard) <= 0) {
			scrollDirection = 1;
			topPauseUntil = millis() + 1000; // Hang at top for 1 second
			return;
		}
	}

	lv_obj_scroll_by(dashboard, 0, -scrollStep * scrollDirection, LV_ANIM_OFF);
}

void createDashboard() {
	// --------------------------------------------------------
	// Dashboard container
	// --------------------------------------------------------
	dashboard = lv_obj_create(lv_screen_active());
	
	lv_obj_set_size(dashboard, LV_PCT(100), LV_PCT(100));
	lv_obj_set_style_bg_color(dashboard, lv_color_hex(currentTheme.background), 0);
	lv_obj_set_style_border_width(dashboard, 0, 0);
	lv_obj_set_style_radius(dashboard, 0, 0); 
	
	lv_obj_set_style_pad_all(dashboard, 8, 0);
	lv_obj_set_style_pad_row(dashboard, 6, 0);
	lv_obj_set_style_pad_column(dashboard, 6, 0);

	lv_obj_set_scroll_dir(dashboard, LV_DIR_VER);
	lv_obj_set_flex_flow(dashboard, LV_FLEX_FLOW_ROW_WRAP);
	lv_obj_set_flex_align(dashboard, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

	// --------------------------------------------------------
	// Determine number of widgets
	// --------------------------------------------------------
	JsonArray configWidgets = config["widgets"].as<JsonArray>();
	widgetCount = 0;

	for (JsonObject obj : configWidgets) {
		widgetCount++;
	}

	Serial.print("Creating ");
	Serial.print(widgetCount);
	Serial.println(" widgets.");

	if (widgetCount > 0) {
		dashboardWidgets = new DashboardWidget[widgetCount];
		size_t index = 0;

		for (JsonObject obj : configWidgets) {
			DashboardWidget &widget = dashboardWidgets[index];

			widget.id = obj["id"] | "";
			widget.title = obj["title"] | "Widget";
			widget.iconFile = obj["icon"] | "";
			widget.templateText = obj["body"] | "";

			widget.url = obj["url"] | "";
			widget.method = obj["method"] | "GET";
			widget.payload = obj["payload"] | "";
			widget.headerKey = obj["headerKey"] | "";
			widget.headerValue = obj["headerValue"] | "";
			
			widget.btn1Label = obj["btn1_label"] | "";
			widget.btn1Url = obj["btn1_url"] | "";
			widget.btn1Method = obj["btn1_method"] | "GET";
			widget.btn1Payload = obj["btn1_payload"] | "";
			widget.btn1HeaderKey = obj["btn1_headerKey"] | "";
			widget.btn1HeaderValue = obj["btn1_headerValue"] | "";

			widget.btn2Label = obj["btn2_label"] | "";
			widget.btn2Url = obj["btn2_url"] | "";
			widget.btn2Method = obj["btn2_method"] | "GET";
			widget.btn2Payload = obj["btn2_payload"] | "";
			widget.btn2HeaderKey = obj["btn2_headerKey"] | "";
			widget.btn2HeaderValue = obj["btn2_headerValue"] | "";

			widget.refreshRate = obj["refresh"] | 0;
			widget.width = obj["width"] | "1/2";
			widget.height = obj["height"] | 110;
			widget.titleFontSize = obj["title_font_size"] | 16;
			widget.bodyFontSize = obj["font_size"] | 14;

			widget.lastRefresh = 0;

			createWidget(widget, dashboard, &dashboardWidgets[index]);

			index++;
		}

		// Initial API updates
		for (size_t i = 0; i < widgetCount; i++) {
			dashboardWidgets[i].needsRefresh = true;
		}
	}

	// --------------------------------------------------------
	// IP Address Label
	// --------------------------------------------------------
	lv_obj_t * ipLabel = lv_label_create(dashboard);
	String ipStr = "IP: ";
	
	if (WiFi.getMode() == WIFI_AP) {
		ipStr += WiFi.softAPIP().toString() + " (AP mode)";
	} else {
		ipStr += WiFi.localIP().toString();
	}
	
	lv_label_set_text(ipLabel, ipStr.c_str());
	lv_obj_set_width(ipLabel, LV_PCT(100));
	lv_obj_set_style_text_align(ipLabel, LV_TEXT_ALIGN_CENTER, 0);
	lv_obj_set_style_text_color(ipLabel, lv_color_hex(currentTheme.secondaryText), 0);
	lv_obj_set_style_margin_top(ipLabel, 20, 0);
	lv_obj_set_style_margin_bottom(ipLabel, 20, 0);

	lastActivityTime = millis();

	dashboardTimer = lv_timer_create(dashboardRefreshTimer, 1000, nullptr);

	if (IDLE_TIMEOUT_MS != 0) {
		autoScrollTimerObj = lv_timer_create(autoScrollTimer, 40, nullptr);
	}
}

// ============================================================
// LVGL initialization
// ============================================================

void initLVGL() {
	Serial.println("Initializing LVGL...");

	lv_init();

	lv_log_register_print_cb(log_print);

	// --------------------------------------------------------
	// Display
	// --------------------------------------------------------

	display = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, sizeof(draw_buf));

	// --------------------------------------------------------
	// Touchscreen
	// --------------------------------------------------------

	touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);

	touchscreen.begin(touchscreenSPI);

	applyRotation();

	// --------------------------------------------------------
	// LVGL input device
	// --------------------------------------------------------

	touch = lv_indev_create();

	lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);

	lv_indev_set_read_cb(touch, touchscreen_read);

	Serial.println("LVGL initialized.");
}

// ============================================================
// Setup
// ============================================================

void setup() {
	Serial.begin(115200);

	delay(500);

	Serial.println();
	Serial.println("==============================");
	Serial.println("			Dashboard Starting");
	Serial.println("==============================");

	// --------------------------------------------------------
	// SD
	// --------------------------------------------------------

	if (!initSD()) {
		Serial.println("WARNING: SD unavailable.");

	} else {
		// ----------------------------------------------------
		// Configuration
		// ----------------------------------------------------

		if (!loadConfig()) {
			Serial.println("No valid config found.");

			createDefaultConfig();
		}
	}

	// --------------------------------------------------------
	// Print settings
	// --------------------------------------------------------

	Serial.print("Rotation: ");

	Serial.println(config["settings"]["rotation"] | 0);

	Serial.print("Brightness: ");

	Serial.println(config["settings"]["brightness"] | 100);

	IDLE_TIMEOUT_MS = config["settings"]["scroll_idle"] | 0;
	scrollStep = config["settings"]["scroll_speed"] | 2;


	initWiFi();

	initWebServer();

	// --------------------------------------------------------
	// LVGL
	// --------------------------------------------------------

	initLVGL();

	applyTheme();

	// --------------------------------------------------------
	// Dashboard
	// --------------------------------------------------------

	createDashboard();

	ledcAttach(BACKLIGHT_PIN, PWM_FREQ, PWM_RES);
	setBrightness(config["settings"]["brightness"] | 100);
	
	Serial.println("Dashboard ready.");
}

// ============================================================
// Loop
// ============================================================

void fireWebhook(String url, String method, String payload, String headerKey, String headerValue) {
	if (url.length() == 0) return;
	Serial.println("Firing Webhook: " + url + " [" + method + "]");
	
	auto performRequest = [&](HTTPClient &http) {
		http.addHeader("Connection", "close");
		
		if (headerKey.length() > 0 && headerValue.length() > 0) {
			http.addHeader(headerKey, headerValue);
		}
		if (payload.length() > 0 && !headerKey.equalsIgnoreCase("Content-Type")) {
			http.addHeader("Content-Type", "application/json");
		}
		if (method == "POST") return http.POST(payload);
		if (method == "PUT") return http.PUT(payload);
		return http.GET();
	};

	if (url.startsWith("https://")) {
		WiFiClientSecure secureClient;
		secureClient.setInsecure();
		HTTPClient http;
		http.begin(secureClient, url);
		http.setTimeout(5000);
		performRequest(http);
		http.end();
	} else {
		WiFiClient plainClient;
		HTTPClient http;
		http.begin(plainClient, url);
		http.setTimeout(5000);
		performRequest(http);
		http.end();
	}
}

void loop() {
	server.handleClient();
	lv_tick_inc(5);
	lv_timer_handler();

	for (size_t i = 0; i < widgetCount; i++) {
		if (dashboardWidgets[i].pendingBtn1) {
			dashboardWidgets[i].pendingBtn1 = false;
			fireWebhook(dashboardWidgets[i].btn1Url, dashboardWidgets[i].btn1Method, dashboardWidgets[i].btn1Payload, dashboardWidgets[i].btn1HeaderKey, dashboardWidgets[i].btn1HeaderValue);
			dashboardWidgets[i].needsRefresh = true;
			break;
		}

		if (dashboardWidgets[i].pendingBtn2) {
			dashboardWidgets[i].pendingBtn2 = false;
			fireWebhook(dashboardWidgets[i].btn2Url, dashboardWidgets[i].btn2Method, dashboardWidgets[i].btn2Payload, dashboardWidgets[i].btn2HeaderKey, dashboardWidgets[i].btn2HeaderValue);
			dashboardWidgets[i].needsRefresh = true;
			break;
		}

		if (dashboardWidgets[i].needsRefresh and dashboardWidgets[i].bodyLabel) {
			dashboardWidgets[i].needsRefresh = false;
			lv_label_set_text(dashboardWidgets[i].bodyLabel, "Refreshing...");
			lv_refr_now(display);
			updateWidget(dashboardWidgets[i]);
			break;
		}
	}

	delay(5);
}
