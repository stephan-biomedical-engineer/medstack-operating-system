"""Fault injection of the infusion pump's acceptance suite, on the target
(implementation_plan_infusion_pump.md, phase 7).

One image per assertion would cost hours, so defects go in batches, and the
claim is not "each fails alone" but "the set that fails is exactly the set
predicted before the run". Each batch applies its defects to the sources,
builds the image, puts the sources back - before the suite even starts, so an
interrupted run never leaves a defect in the tree - and runs
med-check.py infusion against the injected image.

Batches 1, 2a and 2b are the ones of record. Batch 2 is kept because it was
wrong in an instructive way: it combined the controller's supervision off with
the mechanism's dead-man off, a double fault in which nothing stops the motor,
and its failing set differed from the prediction (the phase 7 record says why).

After any batch, rebuild the clean image (make infusion) before trusting it.

Usage: python3 tests/infusion-target/inject.py 1|2|2a|2b   (from the repository)"""
import os, re, shutil, subprocess, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
APP = REPO + "/meta-custom/meta-med-app/recipes-apps/"
CTL = APP + "infusion-control-service/files/src/controller.cpp"
MAIN = APP + "infusion-control-service/files/src/main.cpp"
MECH = APP + "infusion-sim/files/src/mechanism.cpp"
OUT = os.path.join(REPO, "build", "infusion-inject") + os.sep

BATCHES = {
    "1": {
        "edits": [
            (CTL, "    if (rateMlH > d->hardMaxMlH) {", "    if (false) {"),
            (CTL, "    if (rateMlH > d->softMaxMlH) {", "    if (false) {"),
            (CTL, "    if (readback.pressureMmHg >= parameters_.occlusionMmHg) {", "    if (false) {"),
            (CTL, "        if (expected > 0.0 &&", "        if (false &&"),
            (CTL, "    if (deliveredMl() >= program_.vtbiMl) {", "    if (false) {"),
            (CTL, "    if (silencedUntil_ != Clock::time_point{} && now >= silencedUntil_) {", "    if (false) {"),
            (CTL, '    audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm acknowledged",',
                  '    if (false) audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm acknowledged",'),
            (CTL, '    audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm silenced",',
                  '    if (false) audit_.audit(med::AuditEvent::AlarmAcknowledged, "alarm silenced",'),
            (MAIN, "    if (::access(cfg.therapyMarker.c_str(), F_OK) == 0) {", "    if (false) {"),
        ],
        "predicted": {"ders-hard-limit", "ders-soft-limit", "occlusion-alarm", "delivery-fault",
                      "vtbi-complete", "alarm-silence", "no-auto-resume", "audit-trail"},
    },
    "2": {
        "edits": [
            (CTL, "    if (readback.air) {", "    if (false) {"),
            (MECH, "    if (now - lastHeard_ > window_) {", "    if (false) {"),
            (CTL, "    return syringe_ == nullptr ? 0.0 : mm * syringe_->areaMm2 / 1000.0;",
                  "    return syringe_ == nullptr ? 0.0 : mm * syringe_->areaMm2 / 1000.0 * 1.04;"),
            (CTL, "        if (actuator_.heartbeat() != med::Status::Ok) {",
                  "        if (actuator_.heartbeat() != med::Status::Ok && false) {"),
            (CTL, "    if (supervising_ && now - lastReadbackAt_ > parameters_.producerTimeout) {",
                  "    if (false) {"),
        ],
        "predicted": {"air-alarm", "alarm-silence", "delivery-volume", "producer-lost", "deadman"},
    },
    "2a": {
        "edits": [
            (CTL, "    if (readback.air) {", "    if (false) {"),
            (MECH, "    if (now - lastHeard_ > window_) {", "    if (false) {"),
            (CTL, "    return syringe_ == nullptr ? 0.0 : mm * syringe_->areaMm2 / 1000.0;",
                  "    return syringe_ == nullptr ? 0.0 : mm * syringe_->areaMm2 / 1000.0 * 1.08;"),
        ],
        "predicted": {"air-alarm", "alarm-silence", "deadman", "delivery-volume",
                      "vtbi-complete", "occlusion-alarm"},
    },
    "2b": {
        "edits": [
            (CTL, "        if (actuator_.heartbeat() != med::Status::Ok) {",
                  "        if (actuator_.heartbeat() != med::Status::Ok && false) {"),
            (CTL, "    if (supervising_ && now - lastReadbackAt_ > parameters_.producerTimeout) {",
                  "    if (false) {"),
        ],
        "predicted": {"producer-lost"},
    },
}


def main():
    os.makedirs(OUT, exist_ok=True)
    batch = BATCHES[sys.argv[1]]
    files = sorted({e[0] for e in batch["edits"]})
    saved = {f: open(f).read() for f in files}
    try:
        for path, old, new in batch["edits"]:
            text = open(path).read()
            assert text.count(old) == 1, (path, old)
            open(path, "w").write(text.replace(old, new))
        os.chdir(REPO)
        build = subprocess.run(["make", "infusion"], stdout=open(OUT + "inj%s-build.log" % sys.argv[1], "w"),
                               stderr=subprocess.STDOUT)
        if build.returncode != 0:
            print("build failed"); return 2
    finally:
        for f, text in saved.items():
            open(f, "w").write(text)
    env = dict(os.environ, MEDCHECK_VERBOSE="1")
    log = OUT + "inj%s-check.log" % sys.argv[1]
    subprocess.run(["timeout", "1500", "python3", "scripts/med-check.py", "infusion"], env=env,
                   stdout=open(log, "w"), stderr=subprocess.STDOUT)
    text = open(log).read()
    failed = set(re.findall(r"^\s+FAIL\s+(\S+)", text, re.M))
    passed = set(re.findall(r"^\s+PASS\s+(\S+)", text, re.M))
    print("batch", sys.argv[1], "passed", len(passed), "failed", len(failed))
    print("failed   :", sorted(failed))
    print("predicted:", sorted(batch["predicted"]))
    print("EQUAL" if failed == batch["predicted"] else "DIFFERENT: extra=%s missing=%s" % (
        sorted(failed - batch["predicted"]), sorted(batch["predicted"] - failed)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
