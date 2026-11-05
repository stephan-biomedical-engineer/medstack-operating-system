// SPDX-License-Identifier: MIT
//
// infusion-sim - the model of a syringe pump's mechanism.
//
// A MODEL, and every number in it is a model parameter chosen to be testable
// (docs/REQUISITOS_INFUSAO.md section 2), not a property of any real pump. What
// it exists for is to give the control service a mechanism that behaves like
// one in the ways the software has to handle: it moves in steps, it can be
// measured independently of what it was told to do, the line pressure builds
// up when the line is blocked, and it can jam.
//
//   motor + lead screw   the plunger advances in whole steps of `stepMm`
//   encoder              the plunger's position as MEASURED, quantised to a
//                        step, with an optional drift - independent of the
//                        command, which is what lets a delivery fault be seen
//   line pressure        first order: C dP/dt = Q_pump - P/R. An occlusion is
//                        R -> infinity, and the pressure rises at Q/C
//   air detector         one bit
//
// Nothing here knows about time passing on its own, sockets, or the protocol:
// step(dt) is called by whoever owns the clock, so a test can run an hour of
// infusion in milliseconds.

#pragma once

#include <string>

namespace infusion {

struct Syringe {
    const char* name;
    double volumeMl;
    /// Inner cross-section of the barrel, mm^2.
    double areaMm2;
};

/// The syringes the model knows, by name ("10ml", "20ml", "50ml"); nullptr
/// for any other. Areas are typical inner diameters (14.5, 19.1, 26.6 mm),
/// model values.
const Syringe* findSyringe(const std::string& name);

struct ModelParameters {
    /// Compliance of the line and the syringe, mL per mmHg.
    double complianceMlPerMmHg = 0.001;
    /// Resistance downstream (line, catheter, vein), mmHg s per mL. At
    /// 100 mL/h this gives a resting pressure of 20 mmHg.
    double resistanceMmHgSPerMl = 720.0;
    /// Plunger travel per motor step, mm.
    double stepMm = 0.001;
};

class SyringeModel {
public:
    explicit SyringeModel(const Syringe& syringe, ModelParameters parameters = {});

    /// The commanded rate, mL/h. Zero or negative stops the motor.
    void setRate(double mlPerHour);
    double commandedRate() const { return rateMlH_; }

    /// Advance the model by `dtSeconds`, in internal sub-steps of at most a
    /// millisecond so that a large dt stays stable.
    void step(double dtSeconds);

    // Faults, injected by the simulator's fault channel only.
    void setOcclusion(bool on) { occluded_ = on; }
    void setAir(bool on) { air_ = on; }
    /// The motor is jammed: it is driven, the plunger does not move.
    void setStall(bool on) { stalled_ = on; }
    /// The encoder reads (1 + fraction) times the real travel.
    void setEncoderDrift(double fraction) { drift_ = fraction; }

    /// Measurements, as the mechanism's sensors report them.
    double encoderMm() const;
    double pressureMmHg() const { return pressure_; }
    bool air() const { return air_; }
    /// The motor is being driven: a rate is commanded and the syringe is not
    /// at the end of its travel. A jammed motor is still being driven.
    bool motorRunning() const;

    /// The truth, for tests only: what the plunger actually pushed out.
    double plungerMm() const { return static_cast<double>(steps_) * parameters_.stepMm; }
    double deliveredMl() const { return plungerMm() * syringe_.areaMm2 / 1000.0; }
    double strokeMm() const { return syringe_.volumeMl * 1000.0 / syringe_.areaMm2; }
    /// At the last whole step of the stroke: the plunger moves in steps, so
    /// it stops up to one step short of the nominal stroke, never past it.
    bool empty() const { return steps_ >= maxSteps(); }
    long long maxSteps() const {
        return static_cast<long long>(strokeMm() / parameters_.stepMm + 1e-9);
    }
    const Syringe& syringe() const { return syringe_; }

private:
    void subStep(double dt);

    Syringe syringe_;
    ModelParameters parameters_;
    double rateMlH_ = 0.0;
    /// Travel the motor has been driven for but not yet stepped, mm.
    double pendingMm_ = 0.0;
    long long steps_ = 0;
    double pressure_ = 0.0;
    bool occluded_ = false;
    bool air_ = false;
    bool stalled_ = false;
    double drift_ = 0.0;
};

}  // namespace infusion
