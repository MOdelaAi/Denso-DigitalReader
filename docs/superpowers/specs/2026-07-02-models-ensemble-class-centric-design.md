# Ensemble detection + class-centric Models page — design

Date: 2026-07-02
Status: approved (brainstorming) — awaiting spec review before planning

## Goal

Two connected problems with the per-camera detection feature
(`2026-07-01-per-camera-detection-design.md`):

1. **The Models wizard page is hard to use.** It lists every catalog model as a
   group box, and inside each an "attach" checkbox plus one (checkbox, conf spin)
   row *per class*. With multiple general models (yolov8n and yolo11n each carry
   the 80 COCO classes) the page is a wall of ~160 duplicated rows and the same
   class (`person`) must be toggled and tuned separately in each model.

2. **Models that share a class draw duplicate boxes.** `DetectionProcessor`
   runs each attached model independently and draws every kept box, so a person
   seen by both yolov8n and yolo11n gets two overlapping boxes.

The intent when a user attaches 2+ models is **ensemble for accuracy**: run them
on the same classes and merge results to catch more and miss less. The merge
rule is **highest-confidence-wins**: pool same-class boxes across models, run
cross-model NMS, keep the single highest-confidence box. Union for recall, dedup
for cleanliness.

The UI is reworked to be **class-centric**: pick the models in the ensemble,
then pick classes *once* (union by name) with one confidence each.

This slice is still **overlay only** — boxes + `"<class> NN%"` labels. No new
persisted schema, no reading assembly.

## Context

Current shape of the two touch points:

- `src/app/ui/camera/dialog/models_page.{h,cpp}` — the flat per-model×per-class
  page described above. `load_for(camera_id)` builds a group box per
  `list_models(db_)`; `selections(camera_id)` reads the widgets back into
  `std::vector<detection::CameraModel>`. The coordinator (`camera_dialog`)
  persists via `detection::set_camera_models` on Finish.
- `src/app/ui/camera/grid/frame_processor.cpp` — `DetectionProcessor::process`
  loops `models_`, and for each detection applies the per-class conf filter +
  ROI confinement, then draws immediately. No cross-model dedup.

Domain types (`src/core/detection/detection.h`) are **unchanged** by this work:
`DetectionModel` (with `class_names`, index == class_id), `ModelClassSelection`
(`class_id`, `conf`), `CameraModel` (per-model attachment + its selections),
`ResolvedModel` / `CameraDetection` (runtime bundles). Persistence tables
(`model`, `camera_model`, `camera_model_class`) and `detection/repo` are
unchanged.

## Class identity is by NAME

The ensemble merges and the UI dedups **by class name**, not class_id — indices
differ across models (yolov8n's `person`=0 need not match another model's index,
and a custom model's classes are entirely different). Names come from each
model's cached `class_names` (index == class_id). A model whose `.onnx` lacks
`names` metadata falls back to numeric names; for `denso` the classes are the
digits `0`–`9`, which are the correct labels, so no rename is needed.

## Change 1 — runtime ensemble merge (`frame_processor.cpp`)

`DetectionProcessor::process` changes from *loop-and-draw* to
**pool → group-by-name → cross-model NMS → draw**:

1. **Pool.** For every attached model, run inference; for each detection apply
   the existing per-class conf filter and ROI confinement (unchanged). Push
   survivors into one pooled list, each tagged with its **class name** (resolved
   via `run.class_names[class_id]`, falling back to `to_string(class_id)`), its
   `cv::Rect box`, and its `conf`.
2. **Group by class name.**
3. **Cross-model NMS per group** (class-aware — different classes never merge):
   sort the group by `conf` desc; greedily keep the top box and suppress any
   remaining box in the same group whose IoU with a kept box exceeds a fixed
   threshold (`kMergeIoU = 0.5`). This is a small local helper (a few lines of
   IoU + greedy suppression); no new dependency. Each model still does its own
   internal NMS first — this only removes cross-model duplicates.
4. **Draw** the survivors exactly as today (rect + `"<name> NN%"`).

Two models both finding a person → one box (the higher-confidence one). A person
only one model catches → still kept (union → recall). Overlapping boxes of
*different* classes are both kept.

## Change 2 — class-centric Models page (`models_page.{h,cpp}`)

Same public surface (`load_for`, `selections`, `back_requested`/`finish_requested`)
and the same page/coordinator pattern — only the internal widget layout and the
selection round-trip change.

**Layout:**

- **Ensemble models** (top): one checkbox per catalog model — "in the ensemble"
  == attached. This is the attach set.
- **Classes to detect** (below): a search/filter box, then **one row per unique
  class name** across the *checked* models — a select checkbox + one confidence
  `QDoubleSpinBox` (0.00–1.00, default 0.50). Toggling a model live-rebuilds this
  list (union of the checked models' `class_names`, sorted; numeric-only names
  after named ones or naturally sorted — a minor detail, not load-bearing).

**Round-trip to the existing schema (no migration):**

- `selections(camera_id)` — for each **checked** model, build a `CameraModel`;
  for each **selected class name** at conf `c`, if that model has a class with
  that name, add `ModelClassSelection{that model's class_id, c}`. So the single
  class-centric row fans out to a per-model selection keyed by the model's own
  class_id, and `set_camera_models` stores it exactly as before.
- `load_for(camera_id)` — read attachments via `models_for` and the catalog via
  `list_models`; mark a model checked if attached; build the union class list; a
  class row is checked if **any** attached model selected that name, and its conf
  is taken from the first such selection (legacy rows with differing confs for
  the same name collapse to one — acceptable; going forward they stay
  consistent).

The per-class **confidence becomes global** (one value per class name, applied
to every model that has it) rather than per-model-per-class. This matches the
ensemble mental model and is the deliberate simplification.

## Files touched

| File | Change |
|---|---|
| `src/app/ui/camera/grid/frame_processor.cpp` | pool + group-by-name + cross-model NMS before drawing; small IoU/NMS helper |
| `src/app/ui/camera/dialog/models_page.h` | new widget members (model checkboxes, class rows, search); `ModelRowWidgets` replaced by the class-centric bundle |
| `src/app/ui/camera/dialog/models_page.cpp` | class-centric build in `load_for`; name→class_id fan-out in `selections`; live rebuild on model toggle |

No changes to `src/core/` (domain, repo, schema), `camera_grid`, the coordinator
API, or CMake.

## Testing

- The pure cross-model NMS helper is a good unit-test target if extracted to a
  small pure function (input: vector of {box, conf}; output: kept indices) — a
  synthetic overlapping-boxes case asserting the higher-conf survivor is kept and
  a non-overlapping pair both survive. Prefer extracting it so it is testable
  (consistent with the existing pure `yolo_decode`/`letterbox` helpers).
- Models page is widget logic (no unit harness for widgets in this project);
  verify by manual smoke: attach yolov8n + yolo11n, confirm `person` appears once
  and a person in frame draws a single box.
- Manual on-device smoke as in the per-camera-detection spec.

## Non-goals (YAGNI)

- No weighted-box-fusion or agreement/voting merge — highest-confidence-wins only.
- No editing of class names / injecting `names` metadata through the app.
- No DB schema change or migration; the per-model tables are reused.
- No per-model confidence override (confidence is per class name).
- No side rail / grid layout work (tracked separately).

## Rough stage order (for the plan)

1. Extract the pure cross-model NMS helper (+ Catch2 test), then rewire
   `DetectionProcessor::process` to pool → group-by-name → NMS → draw.
2. Rebuild `ModelsPage` as class-centric (ensemble checkboxes + union class list
   + search), with the name↔class_id round-trip in `load_for`/`selections`.
3. Manual smoke: two general models on one camera → single merged boxes;
   confirm existing attachments still load/save.
