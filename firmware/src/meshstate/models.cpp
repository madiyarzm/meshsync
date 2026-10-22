#include "models.h"

#include <cmath>
#include <limits>

namespace meshstate {

namespace {
constexpr double PI = 3.141592653589793;  // same double as Python's math.pi
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();

bool pixels_valid(const uint8_t pixels[DIGIT_FEATURES]) {
    if (pixels == nullptr) {
        return false;
    }
    for (int j = 0; j < DIGIT_FEATURES; j++) {
        if (pixels[j] > DIGIT_MAX_PIXEL) {
            return false;
        }
    }
    return true;
}

bool features_valid(const int32_t features[FACE_FEATURES]) {
    if (features == nullptr) {
        return false;
    }
    for (int f = 0; f < FACE_FEATURES; f++) {
        if (features[f] > MAX_FEATURE_VALUE || features[f] < -MAX_FEATURE_VALUE) {
            return false;
        }
    }
    return true;
}

// Softmax confidence of the best label, relative to the best score so exp() never overflows.
Prediction pick_best(const double* scores, const bool* has_class, int classes) {
    Prediction result = {-1, 0.0};
    for (int label = 0; label < classes; label++) {
        if (has_class[label] && (result.label == -1 || scores[label] > scores[result.label])) {
            result.label = label;
        }
    }
    if (result.label == -1) {
        return result;  // nothing taught yet
    }
    double denominator = 0.0;
    for (int label = 0; label < classes; label++) {
        if (has_class[label]) {
            denominator += std::exp(scores[label] - scores[result.label]);
        }
    }
    result.confidence = 1.0 / denominator;
    return result;
}

// lambda * (all - own) + own, for one integer statistic.
double mix(int64_t all, int64_t own, double lambda) {
    return lambda * double(all - own) + double(own);
}
}  // namespace

// ======================================================================================
// Digits
// ======================================================================================

int digit_nearest_centroid(const DigitsTotals& totals, const uint8_t pixels[DIGIT_FEATURES]) {
    if (!pixels_valid(pixels)) {
        return -1;
    }
    int best_label = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int label = 0; label < DIGIT_CLASSES; label++) {
        uint64_t n = totals.count(label);
        if (n == 0) {
            continue;  // never taught this digit: it has no centroid
        }
        // n*(x - mean) = x*n - sum is an exact integer, so the only rounding is the final division.
        uint64_t squared_numerator = 0;
        for (int j = 0; j < DIGIT_FEATURES; j++) {
            int64_t diff = int64_t(pixels[j]) * int64_t(n) - int64_t(totals.sum(label, j));
            squared_numerator += uint64_t(diff * diff);
        }
        double distance = double(squared_numerator) / double(n * n);
        if (distance < best_distance) {
            best_distance = distance;
            best_label = label;
        }
    }
    return best_label;
}

bool DigitModel::fit(const DigitsTotals& totals, double var_floor) {
    for (int label = 0; label < DIGIT_CLASSES; label++) {
        has_class[label] = false;
    }
    if (!(var_floor > 0.0)) {
        return false;  // also rejects NaN
    }
    uint64_t total = totals.total_count();
    for (int label = 0; label < DIGIT_CLASSES; label++) {
        int64_t n = int64_t(totals.count(label));
        if (n == 0) {
            continue;
        }
        double norm = 0.0;
        for (int j = 0; j < DIGIT_FEATURES; j++) {
            int64_t s = int64_t(totals.sum(label, j));
            int64_t sq = int64_t(totals.sumsq(label, j));
            mean[label][j] = double(s) / double(n);
            // n*sumsq - sum^2 is n^2 times the variance, as an exact integer.
            int64_t spread = n * sq - s * s;
            if (spread < 0) {
                spread = 0;  // impossible for real data; guards against a corrupted slot
            }
            var[label][j] = double(spread) / double(n * n) + var_floor;
            norm += -0.5 * std::log(2.0 * PI * var[label][j]);
        }
        log_norm[label] = norm;
        log_prior[label] = std::log(double(n) / double(total));
        has_class[label] = true;
    }
    return true;
}

void DigitModel::log_scores(const uint8_t pixels[DIGIT_FEATURES], double scores[DIGIT_CLASSES]) const {
    bool valid = pixels_valid(pixels);
    for (int label = 0; label < DIGIT_CLASSES; label++) {
        if (!valid || !has_class[label]) {
            scores[label] = NEGATIVE_INFINITY;
            continue;
        }
        double score = log_prior[label] + log_norm[label];
        for (int j = 0; j < DIGIT_FEATURES; j++) {
            double diff = pixels[j] - mean[label][j];
            score -= diff * diff / (2.0 * var[label][j]);
        }
        scores[label] = score;
    }
}

Prediction DigitModel::predict(const uint8_t pixels[DIGIT_FEATURES]) const {
    if (!pixels_valid(pixels)) {
        return {-1, 0.0};
    }
    double scores[DIGIT_CLASSES];
    log_scores(pixels, scores);
    return pick_best(scores, has_class, DIGIT_CLASSES);
}

// ======================================================================================
// Gestures
// ======================================================================================

bool GestureModel::fit(const GestureTotals& all, const GestureTotals& own, double lambda, double var_floor) {
    own_samples = 0;
    other_samples = 0;
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        has_class[c] = false;
    }
    if (!(var_floor > 0.0) || !(lambda >= 0.0)) {
        return false;  // also rejects NaN
    }
    double weighted[GESTURE_CLASSES];
    double total = 0;
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        own_samples += double(own.count(c));
        other_samples += double(all.count(c) - own.count(c));
        weighted[c] = mix(all.count(c), own.count(c), lambda);
        total += weighted[c];
    }
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        double n = weighted[c];
        if (n <= 0.0) {
            continue;  // no data for this gesture (or lambda = 0 and no own samples)
        }
        double norm = 0.0;
        for (int f = 0; f < FACE_FEATURES; f++) {
            double s = mix(all.sum(c, f), own.sum(c, f), lambda);
            double sq = mix(all.sumsq(c, f), own.sumsq(c, f), lambda);
            mean[c][f] = s / n;
            double variance = sq / n - mean[c][f] * mean[c][f];
            if (variance < 0.0) {
                variance = 0.0;  // rounding can push a tiny variance below zero
            }
            var[c][f] = variance + var_floor;
            norm += -0.5 * std::log(2.0 * PI * var[c][f]);
        }
        log_norm[c] = norm;
        log_prior[c] = std::log(n / total);
        has_class[c] = true;
    }
    return true;
}

void GestureModel::log_scores(const int32_t features[FACE_FEATURES], double scores[GESTURE_CLASSES]) const {
    bool valid = features_valid(features);
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        if (!valid || !has_class[c]) {
            scores[c] = NEGATIVE_INFINITY;
            continue;
        }
        double score = log_prior[c] + log_norm[c];
        for (int f = 0; f < FACE_FEATURES; f++) {
            double diff = features[f] - mean[c][f];
            score -= diff * diff / (2.0 * var[c][f]);
        }
        scores[c] = score;
    }
}

Prediction GestureModel::predict(const int32_t features[FACE_FEATURES]) const {
    if (!features_valid(features)) {
        return {-1, 0.0};
    }
    double scores[GESTURE_CLASSES];
    log_scores(features, scores);
    return pick_best(scores, has_class, GESTURE_CLASSES);
}

int GestureModel::nearest_centroid(const int32_t features[FACE_FEATURES]) const {
    if (!features_valid(features)) {
        return -1;
    }
    int best = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int c = 0; c < GESTURE_CLASSES; c++) {
        if (!has_class[c]) {
            continue;
        }
        double distance = 0;
        for (int f = 0; f < FACE_FEATURES; f++) {
            double diff = features[f] - mean[c][f];
            distance += diff * diff;
        }
        if (distance < best_distance) {
            best_distance = distance;
            best = c;
        }
    }
    return best;
}

// ======================================================================================
// Gaze: ridge regression solved with a Cholesky decomposition
// ======================================================================================

bool GazeModel::fit(const GazeTotals& all, const GazeTotals& own, double lambda, double alpha) {
    ready = false;
    own_samples = double(own.n());
    other_samples = double(all.n() - own.n());
    if (!(lambda >= 0.0) || !(alpha >= 0.0)) {
        return false;
    }
    if (mix(all.n(), own.n(), lambda) <= 0.0) {
        return false;  // no calibration data yet
    }

    // A = mixed X'X + alpha*I (not on the bias), b = mixed X'Y.
    double a[GAZE_DIM][GAZE_DIM];
    double b[GAZE_DIM][GAZE_TARGETS];
    for (int i = 0; i < GAZE_DIM; i++) {
        for (int j = 0; j < GAZE_DIM; j++) {
            a[i][j] = mix(all.xtx(i, j), own.xtx(i, j), lambda);
        }
        if (i > 0) {
            a[i][i] += alpha;
        }
        for (int k = 0; k < GAZE_TARGETS; k++) {
            b[i][k] = mix(all.xty(i, k), own.xty(i, k), lambda);
        }
    }

    // Cholesky: A = L L^T. A pivot near zero means the calibration points don't pin down a
    // solution (too few, or all the same), so we refuse instead of returning garbage.
    double l[GAZE_DIM][GAZE_DIM] = {};
    double largest_diagonal = 0;
    for (int i = 0; i < GAZE_DIM; i++) {
        if (a[i][i] > largest_diagonal) {
            largest_diagonal = a[i][i];
        }
    }
    for (int i = 0; i < GAZE_DIM; i++) {
        for (int j = 0; j <= i; j++) {
            double value = a[i][j];
            for (int m = 0; m < j; m++) {
                value -= l[i][m] * l[j][m];
            }
            if (i == j) {
                if (!(value > 1e-12 * largest_diagonal)) {
                    return false;
                }
                l[i][i] = std::sqrt(value);
            } else {
                l[i][j] = value / l[j][j];
            }
        }
    }

    // Solve L z = b, then L^T w = z, for each target.
    for (int k = 0; k < GAZE_TARGETS; k++) {
        double z[GAZE_DIM];
        for (int i = 0; i < GAZE_DIM; i++) {
            double value = b[i][k];
            for (int m = 0; m < i; m++) {
                value -= l[i][m] * z[m];
            }
            z[i] = value / l[i][i];
        }
        for (int i = GAZE_DIM - 1; i >= 0; i--) {
            double value = z[i];
            for (int m = i + 1; m < GAZE_DIM; m++) {
                value -= l[m][i] * weights[m][k];
            }
            weights[i][k] = value / l[i][i];
        }
    }
    ready = true;
    return true;
}

bool GazeModel::predict(const int32_t features[FACE_FEATURES], double& x, double& y) const {
    if (!ready || !features_valid(features)) {
        return false;
    }
    double output[GAZE_TARGETS] = {0, 0};
    for (int k = 0; k < GAZE_TARGETS; k++) {
        output[k] = weights[0][k] * BIAS_VALUE;
        for (int f = 0; f < FACE_FEATURES; f++) {
            output[k] += weights[f + 1][k] * features[f];
        }
    }
    x = output[0] / MAX_TARGET_VALUE;  // back from x1000 units to 0-1
    y = output[1] / MAX_TARGET_VALUE;
    return true;
}

int gaze_cell(double x, double y) {
    int column = int(std::floor(x * 3.0));
    int row = int(std::floor(y * 3.0));
    column = column < 0 ? 0 : (column > 2 ? 2 : column);
    row = row < 0 ? 0 : (row > 2 ? 2 : row);
    return row * 3 + column;
}

}  // namespace meshstate
