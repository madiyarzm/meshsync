#pragma once

#include <cstdint>

#include "meshstate.h"

namespace meshstate {

// Models are never stored: they are computed from running totals whenever those change.
// Exactness claims apply to the integer totals; fitting and predicting use double.

struct Prediction {
    int label;          // -1 if no prediction
    double confidence;  // 0-1, softmax of the log scores
};

// ======================================================================================
// Digits: same math, in the same order, as experiments/prototype.py, so the results are
// bit-identical on the same machine (checked by test/test_golden).
// ======================================================================================

constexpr double DIGIT_VAR_FLOOR = 1.0;

// Label of the digit whose mean drawing is closest, or -1 if nothing was taught.
int digit_nearest_centroid(const DigitsTotals& totals, const uint8_t pixels[DIGIT_FEATURES]);

// Gaussian Naive Bayes from exact integer totals. About 10 KB: keep it static.
struct DigitModel {
    bool has_class[DIGIT_CLASSES];
    double log_prior[DIGIT_CLASSES];
    double log_norm[DIGIT_CLASSES];  // sum of -0.5*log(2*pi*var) over all pixels
    double mean[DIGIT_CLASSES][DIGIT_FEATURES];
    double var[DIGIT_CLASSES][DIGIT_FEATURES];

    bool fit(const DigitsTotals& totals, double var_floor);
    void log_scores(const uint8_t pixels[DIGIT_FEATURES], double scores[DIGIT_CLASSES]) const;
    Prediction predict(const uint8_t pixels[DIGIT_FEATURES]) const;
};

// ======================================================================================
// Personal mix: lambda * (everyone else) + own.
// lambda = 0 uses only your own calibration; 1 treats everyone's data like yours.
// ======================================================================================

// Variance floor in scaled units (features are x1000): 400 = a standard deviation of 0.02.
constexpr double GESTURE_VAR_FLOOR = 400.0;

struct GestureModel {
    bool has_class[GESTURE_CLASSES];
    double log_prior[GESTURE_CLASSES];
    double log_norm[GESTURE_CLASSES];
    double mean[GESTURE_CLASSES][FACE_FEATURES];
    double var[GESTURE_CLASSES][FACE_FEATURES];
    double own_samples;    // how much of the model is the user's own data
    double other_samples;  // ... and how much comes from everyone else (before lambda)

    bool fit(const GestureTotals& all, const GestureTotals& own, double lambda, double var_floor);
    void log_scores(const int32_t features[FACE_FEATURES], double scores[GESTURE_CLASSES]) const;
    Prediction predict(const int32_t features[FACE_FEATURES]) const;
    int nearest_centroid(const int32_t features[FACE_FEATURES]) const;
};

// Ridge strength in scaled units: 1e5 = 0.1 in unscaled feature units.
constexpr double GAZE_RIDGE_ALPHA = 1e5;

struct GazeModel {
    bool ready;  // false until there is enough calibration to solve
    double weights[GAZE_DIM][GAZE_TARGETS];
    double own_samples;
    double other_samples;

    // Solves (A + alpha*I) w = b with A = mixed X'X, b = mixed X'Y (no penalty on the bias).
    // Returns false (ready = false) if there's no data or the system can't be solved.
    bool fit(const GazeTotals& all, const GazeTotals& own, double lambda, double alpha);

    // Predicted point in screen coordinates (0-1, may fall slightly outside).
    bool predict(const int32_t features[FACE_FEATURES], double& x, double& y) const;
};

// 3x3 grid cell (row * 3 + column) for a point in 0-1 screen coordinates.
int gaze_cell(double x, double y);

}  // namespace meshstate
