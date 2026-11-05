// SPDX-License-Identifier: MIT

#include "syringe_model.h"

#include <algorithm>
#include <cmath>

namespace infusion {

namespace {

constexpr Syringe kSyringes[] = {
    {"10ml", 10.0, 165.13},  // 14.5 mm
    {"20ml", 20.0, 286.52},  // 19.1 mm
    {"50ml", 50.0, 555.72},  // 26.6 mm
};

constexpr double kMaxSubStepSeconds = 0.001;

}  // namespace

const Syringe* findSyringe(const std::string& name) {
    for (const Syringe& syringe : kSyringes) {
        if (name == syringe.name) {
            return &syringe;
        }
    }
    return nullptr;
}

SyringeModel::SyringeModel(const Syringe& syringe, ModelParameters parameters)
    : syringe_(syringe), parameters_(parameters) {}

void SyringeModel::setRate(double mlPerHour) {
    rateMlH_ = mlPerHour > 0.0 ? mlPerHour : 0.0;
}

bool SyringeModel::motorRunning() const {
    return rateMlH_ > 0.0 && !empty();
}

double SyringeModel::encoderMm() const {
    // The encoder sees the plunger, not the motor: a jammed motor leaves it
    // still. Quantised to a step, as a real encoder resolves no finer.
    const double seen = plungerMm() * (1.0 + drift_);
    return std::floor(seen / parameters_.stepMm + 1e-9) * parameters_.stepMm;
}

void SyringeModel::step(double dtSeconds) {
    while (dtSeconds > 0.0) {
        const double dt = std::min(dtSeconds, kMaxSubStepSeconds);
        subStep(dt);
        dtSeconds -= dt;
    }
}

void SyringeModel::subStep(double dt) {
    // Flow the plunger pushes into the line, mL/s. Zero when it does not move.
    double pumpMlPerS = 0.0;
    if (motorRunning() && !stalled_) {
        const double mlPerS = rateMlH_ / 3600.0;
        pendingMm_ += mlPerS * 1000.0 / syringe_.areaMm2 * dt;
        const long long whole = static_cast<long long>(std::floor(pendingMm_ / parameters_.stepMm));
        const long long room = maxSteps() - steps_;
        const long long taken = std::max(0LL, std::min(whole, room));
        steps_ += taken;
        pendingMm_ -= static_cast<double>(whole) * parameters_.stepMm;
        if (taken < whole) {
            pendingMm_ = 0.0;  // end of travel
        }
        pumpMlPerS = mlPerS;
    } else {
        pendingMm_ = 0.0;
    }

    // C dP/dt = Q_pump - P/R; an occluded line lets nothing out.
    const double outMlPerS = occluded_ ? 0.0 : pressure_ / parameters_.resistanceMmHgSPerMl;
    pressure_ += (pumpMlPerS - outMlPerS) / parameters_.complianceMlPerMmHg * dt;
    if (pressure_ < 0.0) {
        pressure_ = 0.0;
    }
}

}  // namespace infusion
