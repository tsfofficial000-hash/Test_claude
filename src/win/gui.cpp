//
// The window. Everything it draws, it draws itself: there is not one native
// child control anywhere, which is why the whole surface is consistently dark
// instead of half-themed.
//
// Painting computes the layout into a list of hit regions, and painting is the
// only thing that computes it. Mouse handling reads that list. There is exactly
// one description of where every drawn thing is, so what you see and what you
// can click cannot drift apart.
//
#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/controller.h"
#include "core/csv.h"
#include "core/fan.h"
#include "core/selftest.h"
#include "core/sensordb.h"
#include "core/units.h"
#include "platform/platform.h"
#include "smc/device.h"
#include "win/theme.h"

using namespace fanforge;

namespace {

constexpr wchar_t kWindowClass[] = L"FanForgeWindow";
constexpr wchar_t kWindowTitle[] = L"FanForge";
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT_PTR kPollTimer = 1;
constexpr int kTrayId = 1;

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

std::wstring widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                         nullptr, 0);
    if (size <= 0) return {};
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), &out[0], size);
    return out;
}

std::string formatNumber(double value, int decimals) {
    char buffer[64];
    const char* pattern = decimals == 0 ? "%.0f" : (decimals == 1 ? "%.1f" : "%.2f");
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

// Temperatures are held in Celsius everywhere, because that is what the SMC
// speaks. This is the single place the display unit is applied, so the window,
// the tray and the log cannot disagree about it.
std::wstring temperatureText(double celsius, bool fahrenheit, int decimals = 1) {
    if (!(celsius == celsius)) return L"--";
    const double value = fahrenheit ? celsiusToFahrenheit(celsius) : celsius;
    return widen(formatNumber(value, decimals) + " " + temperatureUnit(fahrenheit));
}

RECT inset(const RECT& rect, int left, int top, int right, int bottom) {
    RECT out = rect;
    out.left += left;
    out.top += top;
    out.right -= right;
    out.bottom -= bottom;
    return out;
}

bool contains(const RECT& rect, POINT point) {
    return point.x >= rect.left && point.x < rect.right && point.y >= rect.top &&
           point.y < rect.bottom;
}

void fillRect(HDC dc, const RECT& rect, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}

void strokeRect(HDC dc, const RECT& rect, COLORREF colour, int width = 1) {
    HPEN pen = CreatePen(PS_SOLID, width, colour);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
    Rectangle(dc, rect.left, rect.top, rect.right, rect.bottom);
    SelectObject(dc, oldBrush);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

void fillEllipse(HDC dc, int centreX, int centreY, int radius, COLORREF colour) {
    HBRUSH brush = CreateSolidBrush(colour);
    HGDIOBJ oldBrush = SelectObject(dc, brush);
    HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, centreX - radius, centreY - radius, centreX + radius, centreY + radius);
    SelectObject(dc, oldPen);
    SelectObject(dc, oldBrush);
    DeleteObject(brush);
}

void drawLine(HDC dc, int x1, int y1, int x2, int y2, COLORREF colour, int width) {
    HPEN pen = CreatePen(PS_SOLID, width, colour);
    HGDIOBJ oldPen = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr);
    LineTo(dc, x2, y2);
    SelectObject(dc, oldPen);
    DeleteObject(pen);
}

enum class Align { Left, Center, Right };

void drawText(HDC dc, const RECT& rect, const std::wstring& text, COLORREF colour, HFONT font,
              Align align = Align::Left, bool singleLine = true) {
    SetTextColor(dc, colour);
    SetBkMode(dc, TRANSPARENT);
    HGDIOBJ oldFont = SelectObject(dc, font);
    UINT flags = singleLine ? (DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS) : DT_WORDBREAK;
    if (align == Align::Center) flags |= DT_CENTER;
    if (align == Align::Right) flags |= DT_RIGHT;
    RECT target = rect;
    DrawTextW(dc, text.c_str(), -1, &target, flags);
    SelectObject(dc, oldFont);
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif

// GetProcAddress returns a generic FARPROC; casting it to the real signature is
// the documented way to use it.
template <typename Signature>
Signature resolveSymbol(HMODULE module, const char* name) {
    return reinterpret_cast<Signature>(GetProcAddress(module, name));
}

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// The tray structure holds a fixed-size buffer, so this cannot overrun it.
void setTooltip(WCHAR* destination, size_t capacity, const std::wstring& text) {
    const size_t count = std::min(text.size(), capacity - 1);
    std::wmemcpy(destination, text.c_str(), count);
    destination[count] = L'\0';
}

// ---------------------------------------------------------------------------
// Fonts
// ---------------------------------------------------------------------------

struct Fonts {
    HFONT title = nullptr;
    HFONT heading = nullptr;
    HFONT body = nullptr;
    HFONT small = nullptr;
    HFONT numeric = nullptr;

    void create(int scale) {
        destroy();
        auto make = [scale](int tenths, int weight, const wchar_t* face) {
            return CreateFontW(-MulDiv(scale, tenths, 10), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                               DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                               CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
        };
        title = make(20, FW_SEMIBOLD, L"Segoe UI");
        heading = make(15, FW_SEMIBOLD, L"Segoe UI");
        body = make(14, FW_NORMAL, L"Segoe UI");
        small = make(12, FW_NORMAL, L"Segoe UI");
        numeric = make(14, FW_SEMIBOLD, L"Consolas");
    }

    void destroy() {
        HFONT* all[] = {&title, &heading, &body, &small, &numeric};
        for (HFONT* font : all) {
            if (*font) DeleteObject(*font);
            *font = nullptr;
        }
    }
};

// ---------------------------------------------------------------------------
// Hit regions
// ---------------------------------------------------------------------------

enum class Action {
    None,
    ModeSystem,
    ModeManual,
    ModeCurve,
    Slider,
    SensorNext,
    SensorPrevious,
    CurvePoint,
    RestoreAll,
};

struct Hit {
    RECT rect{};
    Action action = Action::None;
    int fan = -1;
    int vertex = -1;
};

// Maps a fan's curve onto its graph. Kept per fan because the mouse handlers
// need the same mapping the painter used.
struct CurveGeometry {
    RECT plot{};
    double minimumTemperature = 30.0;
    double maximumTemperature = 100.0;
    double minimumRpm = 0.0;
    double maximumRpm = 1.0;
    bool valid = false;

    int xFor(double temperature) const {
        const double span = maximumTemperature - minimumTemperature;
        const double where = span > 0 ? (temperature - minimumTemperature) / span : 0.0;
        return plot.left + static_cast<int>(where * (plot.right - plot.left));
    }
    int yFor(double rpm) const {
        const double span = maximumRpm - minimumRpm;
        const double where = span > 0 ? (rpm - minimumRpm) / span : 0.0;
        return plot.bottom - static_cast<int>(where * (plot.bottom - plot.top));
    }
    double temperatureAt(int x) const {
        const double width = plot.right - plot.left;
        if (width <= 0) return minimumTemperature;
        return minimumTemperature +
               (maximumTemperature - minimumTemperature) * (x - plot.left) / width;
    }
    double rpmAt(int y) const {
        const double height = plot.bottom - plot.top;
        if (height <= 0) return minimumRpm;
        return minimumRpm + (maximumRpm - minimumRpm) * (plot.bottom - y) / height;
    }
};

// ---------------------------------------------------------------------------
// Application state
// ---------------------------------------------------------------------------

struct App {
    SmcSession session;
    std::unique_ptr<SmcDevice> device;
    FanBank bank;
    std::unique_ptr<Controller> controller;
    Config config;
    std::string configPath;
    std::vector<SensorReading> readings;
    Controller::TickResult lastTick;
    std::vector<CurveGeometry> geometry;

    HWND window = nullptr;
    Fonts fonts;
    int scale = 1;
    std::vector<Hit> hits;
    RECT sensorListRect{};
    int sensorScroll = 0;

    NOTIFYICONDATAW tray{};
    bool trayAdded = false;

    bool draggingSlider = false;
    int draggingFan = -1;
    int draggingVertex = -1;
    DWORD lastTickAt = 0;

    CsvRecorder log;

    void save() { config.write(configPath); }
};

App* g_app = nullptr;

// ---------------------------------------------------------------------------
// Drawing
// ---------------------------------------------------------------------------

void drawSensorList(App& app, HDC dc) {
    const RECT panel = app.sensorListRect;
    const int pad = theme::kPanelPadding * app.scale;
    const int rowHeight = theme::kRowHeight * app.scale;

    fillRect(dc, panel, theme::kPanel);
    strokeRect(dc, panel, theme::kPanelBorder);

    RECT header = inset(panel, pad, pad, pad, 0);
    header.bottom = header.top + rowHeight;
    drawText(dc, header, L"TEMPERATURES", theme::kTextDim, app.fonts.small);

    if (app.readings.empty()) {
        RECT message = inset(panel, pad, pad * 3, pad, 0);
        drawText(dc, message, L"No sensor is reporting a plausible value.",
                 theme::kTextFaint, app.fonts.small, Align::Left, false);
        return;
    }

    const int top = header.bottom + rowHeight / 2;
    const int bottom = panel.bottom - pad;
    const int visibleRows = std::max(1, (bottom - top) / rowHeight);

    const int maximumScroll =
        std::max(0, static_cast<int>(app.readings.size()) - visibleRows);
    app.sensorScroll = std::max(0, std::min(app.sensorScroll, maximumScroll));

    HRGN clip = CreateRectRgn(panel.left, top, panel.right, bottom);
    SelectClipRgn(dc, clip);

    for (int row = 0; row < visibleRows; ++row) {
        const size_t index = static_cast<size_t>(row + app.sensorScroll);
        if (index >= app.readings.size()) break;
        const SensorReading& reading = app.readings[index];

        RECT line = panel;
        line.left += pad;
        line.right -= pad + 4 * app.scale;
        line.top = top + row * rowHeight;
        line.bottom = line.top + rowHeight;

        if (row % 2 == 1) {
            RECT stripe = line;
            stripe.left -= pad / 2;
            stripe.right += pad / 2;
            fillRect(dc, stripe, theme::kPanelRaised);
        }

        RECT nameRect = line;
        nameRect.right = nameRect.left + (panel.right - panel.left) * 52 / 100;
        drawText(dc, nameRect, widen(reading.name), theme::kText, app.fonts.body);

        drawText(dc, line, temperatureText(reading.celsius, app.config.fahrenheit),
                 theme::forTemperature(reading.celsius), app.fonts.numeric, Align::Right);
    }

    SelectClipRgn(dc, nullptr);
    DeleteObject(clip);

    if (maximumScroll > 0) {
        const int trackHeight = bottom - top;
        const int thumbHeight = std::max(
            rowHeight / 2, trackHeight * visibleRows / static_cast<int>(app.readings.size()));
        const int thumbTop =
            top + (trackHeight - thumbHeight) * app.sensorScroll / maximumScroll;
        RECT thumb{panel.right - 5 * app.scale, thumbTop, panel.right - 2 * app.scale,
                   thumbTop + thumbHeight};
        fillRect(dc, thumb, theme::kTextFaint);
    }
}

void drawCurveGraph(App& app, HDC dc, const RECT& plot, const FanInfo& fan, const FanPolicy& policy,
                    CurveGeometry& geometry) {
    geometry.plot = plot;
    geometry.minimumRpm = fan.minRpm;
    geometry.maximumRpm = fan.maxRpm > fan.minRpm ? fan.maxRpm : fan.minRpm + 1.0;

    if (policy.curve.points.size() >= 2) {
        geometry.minimumTemperature = std::floor(policy.curve.minTemperature() / 10.0) * 10.0;
        geometry.maximumTemperature = std::ceil(policy.curve.maxTemperature() / 10.0) * 10.0;
    }
    if (geometry.maximumTemperature - geometry.minimumTemperature < 40.0) {
        geometry.minimumTemperature -= 10.0;
        geometry.maximumTemperature += 30.0;
    }
    geometry.valid = true;

    fillRect(dc, plot, theme::kBackground);
    strokeRect(dc, plot, theme::kPanelBorder);

    const COLORREF grid = RGB(38, 42, 49);
    for (double temperature = std::ceil(geometry.minimumTemperature / 20.0) * 20.0;
         temperature <= geometry.maximumTemperature; temperature += 20.0) {
        const int x = geometry.xFor(temperature);
        drawLine(dc, x, plot.top + 1, x, plot.bottom - 1, grid, 1);

        RECT label{plot.left, plot.bottom + 1, plot.right, plot.bottom + 1 + 16 * app.scale};
        RECT tick{x - 20 * app.scale, label.top, x + 20 * app.scale, label.bottom};
        drawText(dc, tick, widen(formatNumber(temperature, 0) + " C"), theme::kTextFaint,
                 app.fonts.small, Align::Center);
    }

    const bool active = policy.mode == FanControlMode::Curve;

    if (!policy.curve.valid()) {
        drawText(dc, plot, L"No usable curve for this fan", theme::kTextFaint, app.fonts.small,
                 Align::Center);
        return;
    }

    const COLORREF curveColour = active ? theme::kCurve : theme::kCurveInactive;

    // Sample the curve across the plot so the drawn line and the evaluated
    // curve are the same function, not two approximations of each other.
    bool first = true;
    int previousX = 0;
    int previousY = 0;
    for (int x = plot.left + 1; x < plot.right - 1; ++x) {
        const int y = geometry.yFor(policy.curve.evaluate(geometry.temperatureAt(x)));
        if (first) {
            first = false;
        } else {
            drawLine(dc, previousX, previousY, x, y, curveColour, 2 * app.scale);
        }
        previousX = x;
        previousY = y;
    }

    // Where the fan is actually running. The gap between this dot and the curve
    // is the fan's own lag, made visible.
    if (fan.actualRpm > 0.0) {
        double sourceTemperature = app.lastTick.hasTemperatures ? app.lastTick.hottestCelsius : 0.0;
        for (const SensorReading& reading : app.readings) {
            if (reading.key == policy.curveSensorKey) {
                sourceTemperature = reading.celsius;
                break;
            }
        }
        const int x = std::max(static_cast<int>(plot.left) + 1,
                               std::min(static_cast<int>(plot.right) - 2,
                                        geometry.xFor(sourceTemperature)));
        const int y = std::max(static_cast<int>(plot.top) + 1,
                               std::min(static_cast<int>(plot.bottom) - 2,
                                        geometry.yFor(fan.actualRpm)));
        fillEllipse(dc, x, y, 4 * app.scale, theme::kHot);
    }

    for (size_t i = 0; i < policy.curve.points.size(); ++i) {
        const CurvePoint& point = policy.curve.points[i];
        const int x = geometry.xFor(point.temperature);
        const int y = geometry.yFor(point.rpm);
        const int radius = 5 * app.scale;

        fillEllipse(dc, x, y, radius + app.scale, theme::kBackground);
        fillEllipse(dc, x, y, radius, active ? theme::kAccent : theme::kPanelBorder);

        Hit hit;
        hit.rect = RECT{x - radius - 4 * app.scale, y - radius - 4 * app.scale,
                        x + radius + 4 * app.scale, y + radius + 4 * app.scale};
        hit.action = Action::CurvePoint;
        hit.fan = fan.index;
        hit.vertex = static_cast<int>(i);
        app.hits.push_back(hit);
    }
}

void drawFanPanel(App& app, HDC dc, const FanInfo& fan, const RECT& panel) {
    const int pad = theme::kPanelPadding * app.scale;
    const int rowHeight = theme::kRowHeight * app.scale;
    const FanPolicy policy = app.config.policyFor(static_cast<size_t>(fan.index));

    fillRect(dc, panel, theme::kPanel);
    strokeRect(dc, panel, theme::kPanelBorder);

    RECT row = inset(panel, pad, pad, pad, pad);

    // Title: name on the left, live speed on the right.
    RECT titleRow = row;
    titleRow.bottom = titleRow.top + rowHeight * 2;
    drawText(dc, titleRow, widen(fan.label), theme::kText, app.fonts.heading);
    drawText(dc, titleRow, widen(formatNumber(fan.actualRpm, 0) + " RPM"), theme::kText,
             app.fonts.numeric, Align::Right);

    if (!fan.controllable()) {
        RECT note = row;
        note.top = titleRow.bottom + rowHeight / 2;
        note.bottom = note.top + rowHeight * 3;
        drawText(dc, note,
                 L"This fan does not report a speed range, so it is never driven. Its speed is "
                 L"shown above.",
                 theme::kTextFaint, app.fonts.small, Align::Left, false);
        return;
    }

    // Mode selection.
    RECT modeRow = row;
    modeRow.top = titleRow.bottom + rowHeight / 4;
    modeRow.bottom = modeRow.top + rowHeight * 2;

    struct ModeButton {
        const wchar_t* label;
        FanControlMode mode;
        Action action;
    };
    const ModeButton buttons[] = {
        {L"System", FanControlMode::System, Action::ModeSystem},
        {L"Manual", FanControlMode::Manual, Action::ModeManual},
        {L"Curve", FanControlMode::Curve, Action::ModeCurve},
    };
    const int buttonWidth = (modeRow.right - modeRow.left - 2 * pad) / 3;

    for (int i = 0; i < 3; ++i) {
        RECT button = modeRow;
        button.left = modeRow.left + i * (buttonWidth + pad);
        button.right = button.left + buttonWidth;
        const bool selected = policy.mode == buttons[i].mode;

        fillRect(dc, button, selected ? theme::kAccentDim : theme::kPanelRaised);
        strokeRect(dc, button, selected ? theme::kAccent : theme::kPanelBorder);
        drawText(dc, button, buttons[i].label, selected ? theme::kText : theme::kTextDim,
                 app.fonts.body, Align::Center);

        Hit hit;
        hit.rect = button;
        hit.action = buttons[i].action;
        hit.fan = fan.index;
        app.hits.push_back(hit);
    }

    // Speed slider.
    RECT sliderRow = modeRow;
    sliderRow.top = modeRow.bottom + rowHeight / 2;
    sliderRow.bottom = sliderRow.top + rowHeight * 2;

    const int centreY = sliderRow.top + rowHeight / 2;
    RECT track{sliderRow.left, centreY - 2 * app.scale, sliderRow.right, centreY + 2 * app.scale};
    fillRect(dc, track, theme::kPanelBorder);

    const double fraction = (fan.maxRpm > fan.minRpm)
                                ? (fan.targetRpm - fan.minRpm) / (fan.maxRpm - fan.minRpm)
                                : 0.0;
    const int thumbX =
        track.left +
        static_cast<int>(std::max(0.0, std::min(1.0, fraction)) * (track.right - track.left));
    fillEllipse(dc, thumbX, centreY, 7 * app.scale,
                policy.mode == FanControlMode::Manual ? theme::kAccent : theme::kTextFaint);

    Hit sliderHit;
    sliderHit.rect = sliderRow;
    sliderHit.rect.top -= rowHeight / 2;
    sliderHit.action = Action::Slider;
    sliderHit.fan = fan.index;
    app.hits.push_back(sliderHit);

    RECT rangeText = sliderRow;
    rangeText.bottom = rangeText.top + rowHeight;
    drawText(dc, rangeText,
             widen(formatNumber(fan.minRpm, 0) + " - " + formatNumber(fan.maxRpm, 0) + " RPM"),
             theme::kTextFaint, app.fonts.small, Align::Center);

    // Which sensor the curve follows.
    RECT sensorRow = sliderRow;
    sensorRow.top = sliderRow.bottom + rowHeight / 2;
    sensorRow.bottom = sensorRow.top + rowHeight * 2;

    RECT previous = sensorRow;
    previous.right = previous.left + rowHeight * 2;
    RECT next = sensorRow;
    next.left = next.right - rowHeight * 2;

    fillRect(dc, previous, theme::kPanelRaised);
    fillRect(dc, next, theme::kPanelRaised);
    drawText(dc, previous, L"<", theme::kTextDim, app.fonts.body, Align::Center);
    drawText(dc, next, L">", theme::kTextDim, app.fonts.body, Align::Center);

    std::string sensorLabel = "hottest component";
    if (!policy.curveSensorKey.empty()) sensorLabel = sensorName(policy.curveSensorKey);

    RECT sensorText = sensorRow;
    sensorText.left = previous.right + pad / 2;
    sensorText.right = next.left - pad / 2;
    drawText(dc, sensorText, widen("follows " + sensorLabel), theme::kTextDim, app.fonts.small);

    Hit previousHit;
    previousHit.rect = previous;
    previousHit.action = Action::SensorPrevious;
    previousHit.fan = fan.index;
    app.hits.push_back(previousHit);

    Hit nextHit;
    nextHit.rect = next;
    nextHit.action = Action::SensorNext;
    nextHit.fan = fan.index;
    app.hits.push_back(nextHit);

    // The graph.
    RECT plot = inset(panel, pad, 0, pad, pad);
    plot.top = sensorRow.bottom + rowHeight / 2;
    plot.bottom = panel.bottom - pad - (18 * app.scale);
    plot.bottom = std::min(plot.bottom, plot.top + theme::kGraphHeight * app.scale);
    plot.bottom = std::max(plot.bottom, plot.top + 30 * app.scale);

    if (static_cast<size_t>(fan.index) < app.geometry.size()) {
        drawCurveGraph(app, dc, plot, fan, policy, app.geometry[static_cast<size_t>(fan.index)]);
    }
}

void paint(App& app, HDC target, const RECT& client) {
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap =
        CreateCompatibleBitmap(target, client.right - client.left, client.bottom - client.top);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);

    fillRect(dc, client, theme::kBackground);

    // Painting is the single source of layout.
    app.hits.clear();
    for (CurveGeometry& geometry : app.geometry) geometry.valid = false;

    const int margin = theme::kMargin * app.scale;
    const int rowHeight = theme::kRowHeight * app.scale;

    // Header.
    RECT header = inset(client, margin, margin, margin, 0);
    header.bottom = header.top + rowHeight * 2;

    RECT titleRect = header;
    titleRect.right = titleRect.left + 150 * app.scale;
    drawText(dc, titleRect, kWindowTitle, theme::kText, app.fonts.title);

    RECT restoreButton = header;
    restoreButton.left = restoreButton.right - 190 * app.scale;
    fillRect(dc, restoreButton, theme::kPanelRaised);
    strokeRect(dc, restoreButton, theme::kPanelBorder);
    drawText(dc, restoreButton, L"Return all to system", theme::kTextDim, app.fonts.body,
             Align::Center);

    Hit restoreHit;
    restoreHit.rect = restoreButton;
    restoreHit.action = Action::RestoreAll;
    app.hits.push_back(restoreHit);

    RECT statusText = header;
    statusText.left = titleRect.right + margin;
    statusText.right = restoreButton.left - margin;
    statusText.top = header.top + rowHeight;
    drawText(dc, statusText,
             widen("transport " + std::string(app.session.activeName()) + "   fans " +
                   std::to_string(app.bank.count()) + "   control " +
                   manualModeName(app.bank.manualMode())),
             theme::kTextFaint, app.fonts.small);

    RECT hottestRect = header;
    hottestRect.left = titleRect.right + margin;
    hottestRect.right = restoreButton.left - margin;
    drawText(dc, hottestRect,
             widen(app.lastTick.hasTemperatures
                       ? "hottest " + formatTemperatureWithUnit(app.lastTick.hottestCelsius,
                                                               app.config.fahrenheit)
                       : "no temperatures"),
             app.lastTick.hasTemperatures ? theme::forTemperature(app.lastTick.hottestCelsius)
                                          : theme::kTextFaint,
             app.fonts.heading);

    // Emergency cooling is not a detail to be discovered in a log afterwards:
    // it gets a banner, in the colour reserved for real heat.
    int contentTop = header.bottom + margin;
    if (app.lastTick.emergency) {
        RECT banner{header.left, contentTop, header.right, contentTop + rowHeight};
        fillRect(dc, banner, theme::kHot);
        drawText(dc, banner,
                 widen("EMERGENCY COOLING  -  every fan at maximum, above " +
                       formatTemperatureWithUnit(app.config.emergencyCelsius,
                                                 app.config.fahrenheit)),
                 theme::kBackground, app.fonts.small, Align::Center);
        contentTop = banner.bottom + margin;
    }

    // Body.
    const int contentBottom = client.bottom - margin;
    const int available = client.right - client.left - margin * 2;
    const int sensorWidth = std::max(220 * app.scale, available * 34 / 100);

    app.sensorListRect =
        RECT{client.left + margin, contentTop, client.left + margin + sensorWidth, contentBottom};
    drawSensorList(app, dc);

    const int fanLeft = app.sensorListRect.right + margin;
    const int fanWidth = client.right - margin - fanLeft;
    const int fanCount = static_cast<int>(app.bank.count());

    if (fanCount == 0) {
        RECT panel{fanLeft, contentTop, fanLeft + fanWidth, contentBottom};
        fillRect(dc, panel, theme::kPanel);
        strokeRect(dc, panel, theme::kPanelBorder);
        RECT message = inset(panel, margin * 2, rowHeight * 4, margin * 2, 0);
        drawText(dc, message,
                 L"This Mac reports no fans, so there is nothing to control. The temperatures on "
                 L"the left are live.",
                 theme::kTextDim, app.fonts.body, Align::Center, false);
    } else {
        const int gap = margin;
        const int panelHeight = std::max(
            240 * app.scale, (contentBottom - contentTop - gap * (fanCount - 1)) / fanCount);
        for (int i = 0; i < fanCount; ++i) {
            RECT panel;
            panel.left = fanLeft;
            panel.right = fanLeft + fanWidth;
            panel.top = contentTop + i * (panelHeight + gap);
            panel.bottom = panel.top + panelHeight;
            if (panel.bottom > contentBottom) panel.bottom = contentBottom;
            drawFanPanel(app, dc, app.bank.at(static_cast<size_t>(i)), panel);
        }
    }

    BitBlt(target, 0, 0, client.right - client.left, client.bottom - client.top, dc, 0, 0, SRCCOPY);

    SelectObject(dc, oldBitmap);
    DeleteObject(bitmap);
    DeleteDC(dc);
}

// ---------------------------------------------------------------------------
// Actions
// ---------------------------------------------------------------------------

void cycleSensor(App& app, int fanIndex, int direction) {
    if (!app.controller || fanIndex < 0 || static_cast<size_t>(fanIndex) >= app.bank.count()) {
        return;
    }
    std::vector<std::string> options;
    options.emplace_back();  // "hottest component"
    for (const std::string& key : app.controller->sensorKeys()) options.push_back(key);

    FanPolicy policy = app.config.policyFor(static_cast<size_t>(fanIndex));
    int current = 0;
    for (size_t i = 0; i < options.size(); ++i) {
        if (options[i] == policy.curveSensorKey) {
            current = static_cast<int>(i);
            break;
        }
    }
    int next = current + direction;
    if (next < 0) next = static_cast<int>(options.size()) - 1;
    if (next >= static_cast<int>(options.size())) next = 0;

    policy.curveSensorKey = options[static_cast<size_t>(next)];
    app.config.setPolicy(static_cast<size_t>(fanIndex), policy);
    app.controller->setConfig(app.config);
    app.save();
    InvalidateRect(app.window, nullptr, FALSE);
}

void setMode(App& app, int fanIndex, FanControlMode mode) {
    if (fanIndex < 0 || static_cast<size_t>(fanIndex) >= app.bank.count()) return;
    const FanInfo& fan = app.bank.at(static_cast<size_t>(fanIndex));

    FanPolicy policy = app.config.policyFor(static_cast<size_t>(fanIndex));
    policy.mode = mode;

    // Turning manual mode on should start from where the fan already is, not
    // from a stale number the user set an hour ago.
    if (mode == FanControlMode::Manual && !(policy.manualRpm > 0.0)) {
        policy.manualRpm = fan.actualRpm;
    }
    // Turning the curve on for the first time gives a working one, so the mode
    // is never silently inert.
    if (mode == FanControlMode::Curve && !policy.curve.valid() && fan.controllable()) {
        policy.curve = FanCurve::sensible(fan.minRpm, fan.maxRpm);
    }

    app.config.setPolicy(static_cast<size_t>(fanIndex), policy);
    app.controller->setConfig(app.config);
    app.save();
    InvalidateRect(app.window, nullptr, FALSE);
}

void setManualRpm(App& app, int fanIndex, double rpm) {
    if (fanIndex < 0 || static_cast<size_t>(fanIndex) >= app.bank.count()) return;
    if (!app.bank.at(static_cast<size_t>(fanIndex)).controllable()) return;

    FanPolicy policy = app.config.policyFor(static_cast<size_t>(fanIndex));
    policy.manualRpm = app.bank.clamp(static_cast<size_t>(fanIndex), rpm);
    app.config.setPolicy(static_cast<size_t>(fanIndex), policy);
}

void restoreAllToSystem(App& app) {
    if (!app.controller) return;
    app.controller->restoreAllToSystem();
    for (size_t i = 0; i < app.bank.count(); ++i) {
        FanPolicy policy = app.config.policyFor(i);
        policy.mode = FanControlMode::System;
        app.config.setPolicy(i, policy);
    }
    app.controller->setConfig(app.config);
    app.controller->invalidateCommandCache();
    app.save();
    InvalidateRect(app.window, nullptr, FALSE);
}

// Opens the CSV log if the user asked for one. Best effort by design: a disk
// that will not take the file must not take the control loop down with it.
void startLog(App& app) {
    if (app.log.isOpen()) return;
    if (!app.log.open(defaultLogPath())) return;

    std::vector<std::string> header;
    header.push_back("seconds");
    header.push_back(std::string("hottest_") + temperatureUnit(app.config.fahrenheit));
    for (size_t i = 0; i < app.bank.count(); ++i) {
        header.push_back("F" + std::to_string(i) + "_rpm");
    }
    app.log.setHeader(header);
}

void pollOnce(App& app) {
    if (!app.controller) return;

    const DWORD now = GetTickCount();
    double elapsed = app.lastTickAt == 0 ? 1.0 : (now - app.lastTickAt) / 1000.0;
    app.lastTickAt = now;
    // A long stall - a suspend, a stalled driver - must not turn into a single
    // enormous rate-limit step.
    elapsed = std::max(0.0, std::min(elapsed, 5.0));

    app.lastTick = app.controller->tick(elapsed);
    app.readings = app.controller->readTemperatures();

    if (app.config.recordCsv) {
        startLog(app);
        if (app.log.isOpen()) {
            std::vector<double> row;
            row.push_back(static_cast<double>(now) / 1000.0);
            row.push_back(app.lastTick.hottestCelsius);
            for (size_t i = 0; i < app.bank.count(); ++i) {
                row.push_back(app.bank.at(i).actualRpm);
            }
            app.log.writeRow(row);
        }
    }

    InvalidateRect(app.window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Curve editing
//
// The editor can only produce valid curves. Dragging a point is bounded by its
// neighbours in both temperature and speed, so a curve can never fold back on
// itself or run backwards - which matters because invalid curves make the
// controller hand the fan back to the firmware, and the editor would look like
// it was doing nothing.
// ---------------------------------------------------------------------------

void editCurvePoint(App& app, int fanIndex, int vertex, POINT point) {
    const size_t fan = static_cast<size_t>(fanIndex);
    if (fan >= app.geometry.size() || !app.geometry[fan].valid) return;

    FanPolicy policy = app.config.policyFor(fan);
    const size_t index = static_cast<size_t>(vertex);
    if (index >= policy.curve.points.size()) return;

    const CurveGeometry& geometry = app.geometry[fan];
    const FanInfo& info = app.bank.at(fan);

    double temperature = geometry.temperatureAt(point.x);
    double rpm = geometry.rpmAt(point.y);

    const size_t count = policy.curve.points.size();
    const double lowerTemperature =
        index > 0 ? policy.curve.points[index - 1].temperature + 0.5 : geometry.minimumTemperature;
    const double upperTemperature = index + 1 < count
                                        ? policy.curve.points[index + 1].temperature - 0.5
                                        : geometry.maximumTemperature;
    if (upperTemperature > lowerTemperature) {
        temperature = std::max(lowerTemperature, std::min(upperTemperature, temperature));
    }

    const double lowerRpm = index > 0 ? policy.curve.points[index - 1].rpm : info.minRpm;
    const double upperRpm = index + 1 < count ? policy.curve.points[index + 1].rpm : info.maxRpm;
    rpm = std::max(lowerRpm, std::min(upperRpm, rpm));
    rpm = std::max(info.minRpm, std::min(info.maxRpm, rpm));

    policy.curve.points[index] = CurvePoint{temperature, rpm};
    app.config.setPolicy(fan, policy);
    app.controller->setConfig(app.config);
}

void addCurvePoint(App& app, int fanIndex, POINT point) {
    const size_t fan = static_cast<size_t>(fanIndex);
    if (fan >= app.geometry.size() || !app.geometry[fan].valid) return;

    const FanInfo& info = app.bank.at(fan);
    if (!info.controllable()) return;

    const CurveGeometry& geometry = app.geometry[fan];
    FanPolicy policy = app.config.policyFor(fan);
    if (policy.curve.points.empty()) policy.curve = FanCurve::sensible(info.minRpm, info.maxRpm);

    const double temperature = geometry.temperatureAt(point.x);
    double rpm = geometry.rpmAt(point.y);

    // Insert in temperature order, then clamp the speed between the neighbours
    // so the new point cannot break the curve's monotonicity.
    size_t position = 0;
    while (position < policy.curve.points.size() &&
           policy.curve.points[position].temperature < temperature) {
        ++position;
    }
    const double lowerRpm = position > 0 ? policy.curve.points[position - 1].rpm : info.minRpm;
    const double upperRpm =
        position < policy.curve.points.size() ? policy.curve.points[position].rpm : info.maxRpm;
    rpm = std::max(lowerRpm, std::min(upperRpm, rpm));

    policy.curve.points.insert(policy.curve.points.begin() + static_cast<long>(position),
                               CurvePoint{temperature, rpm});
    policy.curve.normalise();
    policy.mode = FanControlMode::Curve;  // editing a curve means wanting to use it

    app.config.setPolicy(fan, policy);
    app.controller->setConfig(app.config);
    app.save();
    InvalidateRect(app.window, nullptr, FALSE);
}

void removeCurvePoint(App& app, int fanIndex, int vertex) {
    const size_t fan = static_cast<size_t>(fanIndex);
    FanPolicy policy = app.config.policyFor(fan);
    const size_t index = static_cast<size_t>(vertex);
    // Two points are the minimum that describes a curve at all.
    if (policy.curve.points.size() <= 2 || index >= policy.curve.points.size()) return;

    policy.curve.points.erase(policy.curve.points.begin() + static_cast<long>(index));
    policy.curve.normalise();
    app.config.setPolicy(fan, policy);
    app.controller->setConfig(app.config);
    app.save();
    InvalidateRect(app.window, nullptr, FALSE);
}

// ---------------------------------------------------------------------------
// Tray
// ---------------------------------------------------------------------------

void addTrayIcon(App& app) {
    app.tray.cbSize = sizeof(app.tray);
    app.tray.hWnd = app.window;
    app.tray.uID = kTrayId;
    app.tray.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    app.tray.uCallbackMessage = kTrayMessage;
    app.tray.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    setTooltip(app.tray.szTip, 128, L"FanForge");
    app.trayAdded = Shell_NotifyIconW(NIM_ADD, &app.tray) != FALSE;
}

void updateTrayTooltip(App& app) {
    if (!app.trayAdded) return;
    std::wstring tip = L"FanForge";
    if (app.lastTick.hasTemperatures) {
        tip += L" - " + temperatureText(app.lastTick.hottestCelsius, app.config.fahrenheit);
    }
    if (app.bank.count() > 0) {
        tip += L" - fan " + widen(formatNumber(app.bank.at(0).actualRpm, 0)) + L" RPM";
    }
    app.tray.uFlags = NIF_TIP;
    setTooltip(app.tray.szTip, 128, tip);
    Shell_NotifyIconW(NIM_MODIFY, &app.tray);
}

void removeTrayIcon(App& app) {
    if (!app.trayAdded) return;
    Shell_NotifyIconW(NIM_DELETE, &app.tray);
    app.trayAdded = false;
}

// The same non-destructive write-path test the CLI exposes, from the tray. It
// answers the one question a machine cannot be trusted with until it is
// answered: will this Mac actually accept fan writes?
void runWritePathTest(App& app) {
    if (!app.device) return;

    const SelfTestReport report = runWritePathSelfTest(*app.device, app.bank);
    std::string body = report.summary();
    for (const SelfTestStep& step : report.steps) {
        body += "\n";
        body += step.ok ? "ok    " : "FAIL  ";
        body += step.label + " - " + step.detail;
    }
    MessageBoxW(app.window, widen(body).c_str(), L"FanForge write-path test",
                MB_OK | (report.ok() ? MB_ICONINFORMATION : MB_ICONWARNING));

    // The test put every fan back; make sure the control loop agrees.
    if (app.controller) app.controller->invalidateCommandCache();
}

void showTrayMenu(App& app) {
    POINT cursor;
    GetCursorPos(&cursor);

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Show FanForge");
    AppendMenuW(menu, MF_STRING, 2, L"Return all fans to system control");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 4, L"Test fan write path (non-destructive)");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, 3, L"Exit");

    SetForegroundWindow(app.window);
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0,
                                       app.window, nullptr);
    // Required so the menu dismisses properly the next time.
    PostMessageW(app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);

    if (choice == 1) {
        ShowWindow(app.window, SW_SHOW);
        SetForegroundWindow(app.window);
    } else if (choice == 2) {
        restoreAllToSystem(app);
    } else if (choice == 3) {
        DestroyWindow(app.window);
    } else if (choice == 4) {
        runWritePathTest(app);
    }
}

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------

void handleMouseDown(App& app, POINT point, bool rightButton) {
    // Back to front, so the innermost region drawn wins the click.
    for (auto it = app.hits.rbegin(); it != app.hits.rend(); ++it) {
        const Hit& hit = *it;
        if (!contains(hit.rect, point)) continue;

        switch (hit.action) {
            case Action::RestoreAll:
                restoreAllToSystem(app);
                return;
            case Action::ModeSystem:
                setMode(app, hit.fan, FanControlMode::System);
                return;
            case Action::ModeManual:
                setMode(app, hit.fan, FanControlMode::Manual);
                return;
            case Action::ModeCurve:
                setMode(app, hit.fan, FanControlMode::Curve);
                return;
            case Action::SensorNext:
                cycleSensor(app, hit.fan, 1);
                return;
            case Action::SensorPrevious:
                cycleSensor(app, hit.fan, -1);
                return;

            case Action::Slider: {
                if (hit.fan < 0 ||
                    !app.bank.at(static_cast<size_t>(hit.fan)).controllable()) {
                    return;
                }
                // Touching the slider is an instruction to take the fan.
                if (app.config.policyFor(static_cast<size_t>(hit.fan)).mode !=
                    FanControlMode::Manual) {
                    setMode(app, hit.fan, FanControlMode::Manual);
                }
                app.draggingSlider = true;
                app.draggingFan = hit.fan;
                SetCapture(app.window);

                const int width = hit.rect.right - hit.rect.left;
                const FanInfo& fan = app.bank.at(static_cast<size_t>(hit.fan));
                if (width > 0) {
                    const double where = std::max(
                        0.0, std::min(1.0, static_cast<double>(point.x - hit.rect.left) / width));
                    setManualRpm(app, hit.fan, fan.minRpm + where * (fan.maxRpm - fan.minRpm));
                    app.controller->setConfig(app.config);
                }
                return;
            }

            case Action::CurvePoint:
                if (rightButton) {
                    removeCurvePoint(app, hit.fan, hit.vertex);
                    return;
                }
                // Dragging a point implies wanting the curve, so switch to it.
                if (app.config.policyFor(static_cast<size_t>(hit.fan)).mode !=
                    FanControlMode::Curve) {
                    setMode(app, hit.fan, FanControlMode::Curve);
                }
                app.draggingVertex = hit.vertex;
                app.draggingFan = hit.fan;
                SetCapture(app.window);
                return;

            default:
                return;
        }
    }
}

void handleMouseMove(App& app, POINT point) {
    if (app.draggingSlider && app.draggingFan >= 0) {
        for (const Hit& hit : app.hits) {
            if (hit.action != Action::Slider || hit.fan != app.draggingFan) continue;
            const FanInfo& fan = app.bank.at(static_cast<size_t>(hit.fan));
            const int width = hit.rect.right - hit.rect.left;
            if (width > 0) {
                const double where = std::max(
                    0.0, std::min(1.0, static_cast<double>(point.x - hit.rect.left) / width));
                setManualRpm(app, hit.fan, fan.minRpm + where * (fan.maxRpm - fan.minRpm));
                app.controller->setConfig(app.config);
                InvalidateRect(app.window, nullptr, FALSE);
            }
            break;
        }
        return;
    }

    if (app.draggingVertex >= 0 && app.draggingFan >= 0) {
        editCurvePoint(app, app.draggingFan, app.draggingVertex, point);
        InvalidateRect(app.window, nullptr, FALSE);
    }
}

void handleMouseUp(App& app) {
    if (app.draggingSlider) {
        app.draggingSlider = false;
        app.draggingFan = -1;
        ReleaseCapture();
        app.controller->setConfig(app.config);
        app.save();
        InvalidateRect(app.window, nullptr, FALSE);
        return;
    }
    if (app.draggingVertex >= 0) {
        app.draggingVertex = -1;
        app.draggingFan = -1;
        ReleaseCapture();
        app.save();
        InvalidateRect(app.window, nullptr, FALSE);
    }
}

void handleDoubleClick(App& app, POINT point) {
    // A double-click that is not on an existing point adds one.
    for (const Hit& hit : app.hits) {
        if (hit.action == Action::CurvePoint && contains(hit.rect, point)) return;
    }
    for (size_t fan = 0; fan < app.geometry.size(); ++fan) {
        if (!app.geometry[fan].valid) continue;
        if (!contains(app.geometry[fan].plot, point)) continue;
        addCurvePoint(app, static_cast<int>(fan), point);
        return;
    }
}

// ---------------------------------------------------------------------------
// Window procedure
// ---------------------------------------------------------------------------

LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    App* app = g_app;
    if (!app) return DefWindowProcW(window, message, wparam, lparam);

    switch (message) {
        case WM_PAINT: {
            PAINTSTRUCT paintStruct;
            HDC dc = BeginPaint(window, &paintStruct);
            RECT client;
            GetClientRect(window, &client);
            paint(*app, dc, client);
            EndPaint(window, &paintStruct);
            return 0;
        }

        case WM_ERASEBKGND:
            return 1;  // the whole surface is repainted in WM_PAINT

        case WM_SIZE:
            InvalidateRect(window, nullptr, FALSE);
            return 0;

        case WM_TIMER:
            if (wparam == kPollTimer) {
                pollOnce(*app);
                updateTrayTooltip(*app);
            }
            return 0;

        case WM_LBUTTONDOWN:
            handleMouseDown(*app, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, false);
            return 0;

        case WM_RBUTTONDOWN:
            handleMouseDown(*app, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}, true);
            return 0;

        case WM_LBUTTONDBLCLK:
            handleDoubleClick(*app, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;

        case WM_MOUSEMOVE:
            handleMouseMove(*app, POINT{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)});
            return 0;

        case WM_LBUTTONUP:
            handleMouseUp(*app);
            return 0;

        case WM_MOUSEWHEEL: {
            // Wheel coordinates arrive in screen space, unlike the other mouse
            // messages.
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(window, &point);
            if (contains(app->sensorListRect, point)) {
                const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
                app->sensorScroll -= notches * 3;
                if (app->sensorScroll < 0) app->sensorScroll = 0;
                InvalidateRect(window, nullptr, FALSE);
            }
            return 0;
        }

        case kTrayMessage:
            if (LOWORD(lparam) == WM_RBUTTONUP || LOWORD(lparam) == WM_CONTEXTMENU) {
                showTrayMenu(*app);
            } else if (LOWORD(lparam) == WM_LBUTTONDBLCLK) {
                ShowWindow(window, SW_SHOW);
                SetForegroundWindow(window);
            }
            return 0;

        case WM_CLOSE:
            // Closing the window leaves the app running in the tray. It exists
            // to hold a fan speed while something else is running; an
            // accidental close should not hand the fans back mid-game, and
            // "Exit" in the tray menu is right there.
            ShowWindow(window, SW_HIDE);
            return 0;

        // After a resume the firmware has usually taken the fans back, so what
        // this program last commanded is no longer what the hardware is doing.
        // Forgetting the command cache and ticking immediately re-asserts every
        // policy at once, instead of waiting for the next poll and hoping the
        // difference is noticed.
        case WM_POWERBROADCAST:
            if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
                if (app->controller) {
                    app->controller->invalidateCommandCache();
                    app->lastTickAt = 0;  // do not count the sleep as elapsed time
                    pollOnce(*app);
                    updateTrayTooltip(*app);
                }
            }
            return TRUE;

        // Handing the machine back is the default, and the setting that turns
        // it off is deliberately documented as a way to leave a fan pinned.
        case WM_QUERYENDSESSION:
            if (app->controller && app->config.restoreOnExit) {
                app->controller->restoreAllToSystem();
            }
            return TRUE;

        case WM_ENDSESSION:
            if (app->controller && app->config.restoreOnExit) {
                app->controller->restoreAllToSystem();
            }
            return 0;

        case WM_DESTROY:
            if (app->controller && app->config.restoreOnExit) {
                app->controller->restoreAllToSystem();
            }
            app->save();
            KillTimer(window, kPollTimer);
            removeTrayIcon(*app);
            PostQuitMessage(0);
            return 0;

        default:
            break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// ---------------------------------------------------------------------------
// Diagnostics and entry point
// ---------------------------------------------------------------------------

void showFatalError(const std::string& body) {
    MessageBoxW(nullptr, widen(body).c_str(), L"FanForge", MB_OK | MB_ICONERROR);
}

// --check opens the SMC, reports which access path worked and what the machine
// looks like, then exits without opening a window. The GUI cannot be driven
// from a script, so this is how a machine can be checked non-interactively.
// Exits before any window exists, so it works over a remote shell too.
void writeStdout(const std::string& text) { std::fwrite(text.data(), 1, text.size(), stdout); }

int runCheck() {
    SmcSession session;
    const bool opened = session.open();

    std::string report = session.summary();
    if (!opened) {
        report += "\nNo access path worked. See README.md for what each one needs.\n";
        writeStdout(report);
        return 2;
    }

    SmcDevice device(*session.transport());
    FanBank bank;
    bank.discover(device);

    report += "transport : " + std::string(session.activeName()) + "\n";
    report += "keys      : " + std::to_string(device.keyCount()) + "\n";
    report += "fans      : " + std::to_string(bank.count()) + "\n";
    report += "control   : " + std::string(manualModeName(bank.manualMode())) + "\n";
    if (!bank.notes().empty()) report += "notes     : " + bank.notes() + "\n";

    for (size_t i = 0; i < bank.count(); ++i) {
        const FanInfo& fan = bank.at(i);
        report += "fan " + std::to_string(i) + "     : " + fan.label + ", " +
                  formatNumber(fan.actualRpm, 0) + " RPM";
        if (fan.hasLimits) {
            report += " (" + formatNumber(fan.minRpm, 0) + "-" + formatNumber(fan.maxRpm, 0) + ")";
        } else {
            report += " (no range reported)";
        }
        report += "\n";
    }

    writeStdout(report);
    return 0;
}

}  // namespace

int WINAPI WinMain(HINSTANCE instance, HINSTANCE, LPSTR, int showCommand) {
    // Per-monitor DPI, resolved dynamically so the program still starts on a
    // system that does not have it.
    using SetContextFn = BOOL(WINAPI*)(void*);
    using LegacyFn = BOOL(WINAPI*)();
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = resolveSymbol<SetContextFn>(user32, "SetProcessDpiAwarenessContext")) {
            fn(reinterpret_cast<void*>(-4));  // PER_MONITOR_AWARE_V2
        } else if (auto legacy = resolveSymbol<LegacyFn>(user32, "SetProcessDPIAware")) {
            legacy();
        }
    }

    // The command line is read wide so a path with spaces or non-ASCII in it
    // cannot confuse the check.
    if (std::wstring(GetCommandLineW()).find(L"--check") != std::wstring::npos) return runCheck();

    // The SMC device allows one program at a time, so a second copy could only
    // ever fight the first.
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"FanForge.SingleInstance");
    if (singleInstance && GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"FanForge is already running - look for it in the system tray.",
                    L"FanForge", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    App app;
    app.configPath = defaultConfigPath();
    app.config = Config::read(app.configPath);

    if (!app.session.open()) {
        showFatalError(
            "FanForge could not reach the SMC on this machine.\n\n" + app.session.summary() +
            "\nThe SMC is only reachable from Windows running natively on an Intel Mac. See "
            "README.md for what each access path needs.");
        if (singleInstance) CloseHandle(singleInstance);
        return 2;
    }

    app.device = std::make_unique<SmcDevice>(*app.session.transport());
    app.bank.discover(*app.device);
    app.geometry.resize(app.bank.count());

    // Drop policies for fans this machine does not have rather than carrying
    // them around forever.
    if (app.config.fans.size() > app.bank.count()) app.config.fans.resize(app.bank.count());

    app.controller = std::make_unique<Controller>(*app.device, app.bank);
    app.controller->initialise(app.config);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = nullptr;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) {
        showFatalError("The window class could not be registered.");
        if (singleInstance) CloseHandle(singleInstance);
        return 1;
    }

    app.window = CreateWindowExW(0, kWindowClass, kWindowTitle, WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                 CW_USEDEFAULT, 940, 680, nullptr, nullptr, instance, nullptr);
    if (!app.window) {
        showFatalError("The window could not be created.");
        if (singleInstance) CloseHandle(singleInstance);
        return 1;
    }

    {
        HDC screen = GetDC(nullptr);
        const int dpi = GetDeviceCaps(screen, LOGPIXELSX);
        ReleaseDC(nullptr, screen);
        app.scale = std::max(1, (dpi + 48) / 96);
    }
    app.fonts.create(app.scale);

    g_app = &app;
    addTrayIcon(app);
    SetTimer(app.window, kPollTimer,
             static_cast<UINT>(std::max(200, app.config.pollIntervalMs)), nullptr);

    ShowWindow(app.window, app.config.startMinimized ? SW_HIDE : showCommand);
    UpdateWindow(app.window);
    pollOnce(app);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    app.fonts.destroy();
    if (singleInstance) CloseHandle(singleInstance);
    return static_cast<int>(message.wParam);
}

#endif  // _WIN32
