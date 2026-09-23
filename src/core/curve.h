#pragma once
//
// Temperature -> fan speed curve, plus the rate limiting that keeps a fan from
// hunting audibly.
//
#include <string>
#include <vector>

namespace fanforge {

struct CurvePoint {
    double temperature = 0.0;  // degrees Celsius
    double rpm = 0.0;

    bool operator<(const CurvePoint& o) const { return temperature < o.temperature; }
};

// A piecewise-linear curve. Between points it interpolates; outside the end
// points it holds the end value, so a curve can never extrapolate to a speed
// outside the fan's own limits.
class FanCurve {
public:
    std::vector<CurvePoint> points;

    FanCurve() = default;
    FanCurve(std::initializer_list<CurvePoint> list) : points(list) { normalise(); }

    double evaluate(double celsius) const;

    // Sorts by temperature, drops duplicate temperatures and non-finite
    // values. Harmless to call repeatedly.
    void normalise();

    // Two points minimum, and speed must not fall as temperature rises.
    bool valid() const;
    std::string invalidReason() const;

    // A sane starting curve spanning a fan's own range: quiet when cool,
    // full speed well before the silicon is in trouble.
    static FanCurve sensible(double minimumRpm, double maximumRpm);

    double minTemperature() const;
    double maxTemperature() const;
    std::string describe() const;
};

// Rate limiter and deadband. Fans must spin up promptly when the machine heats
// (thermal safety) but must not fall instantly, which is what causes the
// audible up-down hunting.
class CurveOutput {
public:
    CurveOutput(double maximumRisePerSecond, double maximumFallPerSecond, double deadbandRpm);

    // Feeds one target in and returns the limited output. The first call seeds
    // the output directly so a fan never ramps from zero on startup.
    double update(double target, double seconds);

    double value() const { return value_; }
    void reset(double value);
    bool primed() const { return primed_; }
    void setDeadband(double rpm) { deadband_ = rpm < 0 ? 0 : rpm; }
    void setLimits(double maximumRisePerSecond, double maximumFallPerSecond);

private:
    double maxRisePerSecond_;
    double maxFallPerSecond_;
    double deadband_;
    double value_ = 0.0;
    bool primed_ = false;
};

}  // namespace fanforge
