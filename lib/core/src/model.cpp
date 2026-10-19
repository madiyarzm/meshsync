#include "model.h"

#include <cmath>
#include <limits>

namespace construct {

namespace {
constexpr double PI = 3.141592653589793;  // same double as Python's math.pi
constexpr double NEGATIVE_INFINITY = -std::numeric_limits<double>::infinity();
}  // namespace

int nearest_centroid_predict(const Totals& totals, const uint8_t pixels[NUM_FEATURES]) {
    if (!pixels_valid(pixels)) {
        return -1;
    }
    int best_label = -1;
    double best_distance = std::numeric_limits<double>::infinity();
    for (int label = 0; label < NUM_CLASSES; label++) {
        uint64_t n = totals.count[label];
        if (n == 0) {
            continue;  // never taught this digit: it has no centroid
        }
        // n*(x - mean) = x*n - sum is an exact integer, so the only rounding is the final division.
        uint64_t squared_numerator = 0;
        for (int j = 0; j < NUM_FEATURES; j++) {
            int64_t diff = int64_t(pixels[j]) * int64_t(n) - int64_t(totals.sum[label][j]);
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

bool GaussianNB::fit(const Totals& totals, double var_floor) {
    for (int label = 0; label < NUM_CLASSES; label++) {
        has_class[label] = false;
    }
    if (!(var_floor > 0.0)) {
        return false;  // also rejects NaN
    }

    uint64_t total = totals.total_count();
    for (int label = 0; label < NUM_CLASSES; label++) {
        int64_t n = int64_t(totals.count[label]);
        if (n == 0) {
            continue;
        }
        double norm = 0.0;
        for (int j = 0; j < NUM_FEATURES; j++) {
            int64_t s = int64_t(totals.sum[label][j]);
            int64_t sq = int64_t(totals.sumsq[label][j]);
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

void GaussianNB::log_scores(const uint8_t pixels[NUM_FEATURES], double scores[NUM_CLASSES]) const {
    bool valid = pixels_valid(pixels);
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (!valid || !has_class[label]) {
            scores[label] = NEGATIVE_INFINITY;
            continue;
        }
        double score = log_prior[label] + log_norm[label];
        for (int j = 0; j < NUM_FEATURES; j++) {
            double diff = pixels[j] - mean[label][j];
            score -= diff * diff / (2.0 * var[label][j]);
        }
        scores[label] = score;
    }
}

Prediction GaussianNB::predict(const uint8_t pixels[NUM_FEATURES]) const {
    Prediction result = {-1, 0.0};
    if (!pixels_valid(pixels)) {
        return result;
    }
    double scores[NUM_CLASSES];
    log_scores(pixels, scores);

    int best_label = -1;
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (!has_class[label]) {
            continue;
        }
        if (best_label == -1 || scores[label] > scores[best_label]) {
            best_label = label;
        }
    }
    if (best_label == -1) {
        return result;  // nothing taught yet
    }

    // Softmax relative to the best score so exp() never overflows.
    double denominator = 0.0;
    for (int label = 0; label < NUM_CLASSES; label++) {
        if (has_class[label]) {
            denominator += std::exp(scores[label] - scores[best_label]);
        }
    }
    result.label = best_label;
    result.confidence = 1.0 / denominator;
    return result;
}

}  // namespace construct
