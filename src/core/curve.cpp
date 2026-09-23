#include "core/curve.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fanforge {

namespace {
bool finite(double v) { return v == v && v != HUGE_VAL && v != -HUGE_VAL; }
}  // namespace

double FanCurve::evaluate(double celsius) const {
    if (points.empty()) return 0.0;
    if (!finite(celsius)) return points.front().rpm;

    // Held at the ends rather than extrapolated: a curve is a clamp as well as
    // an interpolation.
    if (celsius <= points.front().temperature) return points.front().rpm;
    if (celsius >= points.back().temperature) return points.back().rpm;

    for (size_t i = 1; i < points.size(); ++i) {
        const CurvePoint& hi = points[i];
        const CurvePoint& lo = points[i - 1];
        if (celsius <= hi.temperature) {
            const double span = hi.temperature - lo.temperature;
            if (span <= 0.0) return hi.rpm;
            const double t = (celsius - lo.temperature) / span;
            return lo.rpm + t * (hi.rpm - lo.rpm);
        }
    }
    return points.back().rpm;
}

void FanCurve::normalise() {
    std::vector<CurvePoint> cleaned;
    cleaned.reserve(points.size());
    for (const CurvePoint& p : points) {
        if (!finite(p.temperature) || !finite(p.rpm)) continue;
        cleaned.push_back(p);
    }
    std::sort(cleaned.begin(), cleaned.end());

    points.clear();
    points.reserve(cleaned.size());
    for (const CurvePoint& p : cleaned) {
        // Last one wins at a duplicate temperature, so an edit never leaves the
        // vertical segment that would make the curve ambiguous.
        if (!points.empty() && points.back().temperature == p.temperature) {
            points.back() = p;
            continue;
        }
        points.push_back(p);
    }
}

bool FanCurve::valid() const { return invalidReason().empty(); }

std::string FanCurve::invalidReason() const {
    if (points.size() < 2) return "a curve needs at least two points";
    for (size_t i = 1; i < points.size(); ++i) {
        if (points[i].temperature <= points[i - 1].temperature) {
            return "curve points must rise in temperature";
        }
        if (points[i].rpm < points[i - 1].rpm) {
            return "fan speed must not fall as temperature rises";
        }
    }
    return {};
}

FanCurve FanCurve::sensible(double minimumRpm, double maximumRpm) {
    if (!(maximumRpm > minimumRpm)) {
        // Without a usable range there is no curve to give; the caller checks
        // valid() and falls back to system control.
        return FanCurve{};
    }
    const double span = maximumRpm - minimumRpm;
    // The first point matters as much as the last: an Intel MacBook Pro idles
    // somewhere around 45-55 C, so ramping from below that would make the
    // machine louder than stock while it is doing nothing. The curve holds the
    // fan at its own minimum until 50 C, then climbs hard enough to be at full
    // speed well before the silicon is uncomfortable.
    FanCurve c;
    c.points = {
        {50.0, minimumRpm},
        {62.0, minimumRpm + span * 0.20},
        {72.0, minimumRpm + span * 0.45},
        {82.0, minimumRpm + span * 0.75},
        {92.0, maximumRpm},
    };
    return c;
}

double FanCurve::minTemperature() const { return points.empty() ? 0.0 : points.front().temperature; }
double FanCurve::maxTemperature() const { return points.empty() ? 0.0 : points.back().temperature; }

std::string FanCurve::describe() const {
    std::string s;
    char b[48];
    for (size_t i = 0; i < points.size(); ++i) {
        std::snprintf(b, sizeof b, "%s%.0fC=%.0f", i ? " " : "", points[i].temperature, points[i].rpm);
        s += b;
    }
    return s;
}

CurveOutput::CurveOutput(double maximumRisePerSecond, double maximumFallPerSecond,
                         double deadbandRpm)
    : maxRisePerSecond_(maximumRisePerSecond > 0 ? maximumRisePerSecond : 0),
      maxFallPerSecond_(maximumFallPerSecond > 0 ? maximumFallPerSecond : 0),
      deadband_(deadbandRpm > 0 ? deadbandRpm : 0) {}

double CurveOutput::update(double target, double seconds) {
    if (!finite(target)) return value_;
    if (!primed_) {
        primed_ = true;
        value_ = target;
        return value_;
    }
    if (!(seconds > 0.0)) seconds = 0.0;

    const double delta = target - value_;
    const double magnitude = delta < 0 ? -delta : delta;

    // Small corrections are ignored: this is what stops the fan oscillating
    // around a steady-state temperature.
    if (magnitude <= deadband_) return value_;

    if (delta > 0.0) {
        const double allowed = maxRisePerSecond_ * seconds;
        value_ += (magnitude < allowed) ? magnitude : allowed;
    } else {
        const double allowed = maxFallPerSecond_ * seconds;
        value_ -= (magnitude < allowed) ? magnitude : allowed;
    }
    return value_;
}

void CurveOutput::reset(double value) {
    value_ = value;
    primed_ = true;
}

void CurveOutput::setLimits(double maximumRisePerSecond, double maximumFallPerSecond) {
    maxRisePerSecond_ = maximumRisePerSecond > 0 ? maximumRisePerSecond : 0;
    maxFallPerSecond_ = maximumFallPerSecond > 0 ? maximumFallPerSecond : 0;
}

}  // namespace fanforge
