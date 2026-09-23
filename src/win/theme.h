#pragma once
//
// One place for the look. A dark palette is not decoration here: this window
// sits open next to a game or a render, and a white panel in the corner of the
// screen is the thing that gets closed.
//
#include <windows.h>

namespace fanforge {
namespace theme {

constexpr COLORREF kBackground = RGB(22, 24, 28);
constexpr COLORREF kPanel = RGB(33, 36, 42);
constexpr COLORREF kPanelRaised = RGB(42, 46, 54);
constexpr COLORREF kPanelBorder = RGB(54, 59, 68);
constexpr COLORREF kText = RGB(228, 232, 238);
constexpr COLORREF kTextDim = RGB(139, 147, 159);
constexpr COLORREF kTextFaint = RGB(94, 101, 112);
constexpr COLORREF kAccent = RGB(88, 166, 255);
constexpr COLORREF kAccentDim = RGB(48, 86, 133);
constexpr COLORREF kHot = RGB(240, 118, 88);
constexpr COLORREF kWarm = RGB(238, 176, 88);
constexpr COLORREF kCool = RGB(112, 196, 148);
constexpr COLORREF kCurve = RGB(139, 180, 255);
constexpr COLORREF kCurveInactive = RGB(84, 92, 106);

// Colour for a temperature, so the whole window reads at a glance.
inline COLORREF forTemperature(double celsius) {
    if (celsius >= 85.0) return kHot;
    if (celsius >= 70.0) return kWarm;
    if (celsius >= 50.0) return kText;
    return kCool;
}

// Layout metrics, in device-independent units before DPI scaling.
constexpr int kMargin = 14;
constexpr int kRowHeight = 20;
constexpr int kPanelPadding = 12;
constexpr int kGraphHeight = 116;

}  // namespace theme
}  // namespace fanforge
