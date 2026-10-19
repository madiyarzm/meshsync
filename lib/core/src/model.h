#pragma once

#include <cstdint>

#include "core_constants.h"
#include "node.h"

namespace construct {

// Same math, in the same order, as experiments/construct_proto.py, so the results are
// bit-identical on the same machine (checked by test/test_golden).

// Label of the digit whose mean drawing is closest, or -1 if nothing was taught
// or the drawing is invalid. Ties go to the lower label.
int nearest_centroid_predict(const Totals& totals, const uint8_t pixels[NUM_FEATURES]);

struct Prediction {
    int label;          // -1 if no prediction
    double confidence;  // 0-1, softmax of the log scores
};

// Gaussian Naive Bayes built from integer totals. About 10 KB: keep it static.
struct GaussianNB {
    bool has_class[NUM_CLASSES];  // false for digits never taught
    double log_prior[NUM_CLASSES];
    double log_norm[NUM_CLASSES];  // sum of -0.5*log(2*pi*var) over all pixels
    double mean[NUM_CLASSES][NUM_FEATURES];
    double var[NUM_CLASSES][NUM_FEATURES];

    // var_floor is added to every variance so pixels that never change (corners) don't
    // divide by zero. Returns false (and an empty model) if var_floor is not positive.
    bool fit(const Totals& totals, double var_floor);

    // Log score per digit; -infinity for digits never taught or if the drawing is invalid.
    void log_scores(const uint8_t pixels[NUM_FEATURES], double scores[NUM_CLASSES]) const;

    Prediction predict(const uint8_t pixels[NUM_FEATURES]) const;
};

}  // namespace construct
