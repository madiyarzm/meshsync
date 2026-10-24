"""Python prototype of the meshsync core: slots, teach, merge, and the two models.

The model code uses plain loops in a fixed order on purpose: the C++ port
(Phase 3) repeats the same operations in the same order, so both produce
bit-identical floating point results.
"""

import math
import struct

NUM_CLASSES = 10
NUM_FEATURES = 64  # 8x8 grid
MAX_PIXEL = 16
UINT32_MAX = 2**32 - 1
VAR_FLOOR = 1.0  # chosen by the validation sweep in run_experiments.py (training data only)

# Per slot, per digit. Keeps uint32 fields far from overflow, and with up to 32 nodes keeps the
# mesh-wide count per digit under 2^25, so every C++ 64-bit intermediate in the models fits.
MAX_SAMPLES_PER_CLASS = 1_000_000


def check_pixels(pixels):
    if len(pixels) != NUM_FEATURES:
        raise ValueError(f"expected {NUM_FEATURES} pixels, got {len(pixels)}")
    for value in pixels:
        if not isinstance(value, int) or value < 0 or value > MAX_PIXEL:
            raise ValueError(f"pixel values must be ints 0-{MAX_PIXEL}, got {value!r}")


class Slot:
    """Everything one node has learned. Only the owning node ever modifies it."""

    def __init__(self, owner_id):
        self.owner_id = owner_id
        self.version = 0  # version 0 means "nothing learned yet"
        self.count = [0] * NUM_CLASSES
        self.sum = [[0] * NUM_FEATURES for _ in range(NUM_CLASSES)]
        self.sumsq = [[0] * NUM_FEATURES for _ in range(NUM_CLASSES)]

    def copy(self):
        other = Slot(self.owner_id)
        other.version = self.version
        other.count = list(self.count)
        other.sum = [list(row) for row in self.sum]
        other.sumsq = [list(row) for row in self.sumsq]
        return other

    def __eq__(self, other):
        return (
            isinstance(other, Slot)
            and self.owner_id == other.owner_id
            and self.version == other.version
            and self.count == other.count
            and self.sum == other.sum
            and self.sumsq == other.sumsq
        )

    def teach(self, pixels, label):
        """Add one labelled drawing. Validates everything before changing any field."""
        if not isinstance(label, int) or label < 0 or label >= NUM_CLASSES:
            raise ValueError(f"label must be an int 0-{NUM_CLASSES - 1}, got {label!r}")
        check_pixels(pixels)
        if self.count[label] >= MAX_SAMPLES_PER_CLASS or self.version >= UINT32_MAX:
            raise OverflowError("slot is full: another teach would overflow uint32")

        self.count[label] += 1
        for j in range(NUM_FEATURES):
            self.sum[label][j] += pixels[j]
            self.sumsq[label][j] += pixels[j] * pixels[j]
        self.version += 1


# ---------------------------------------------------------------------------
# Merge (the CRDT)
# ---------------------------------------------------------------------------

def merge_slot(local, incoming):
    """Keep whichever copy has the higher version. Returns True if local changed."""
    if incoming.owner_id != local.owner_id:
        raise ValueError("cannot merge slots of different owners")
    if incoming.version <= local.version:
        return False  # stale or duplicate: ignoring it is what makes merge idempotent
    local.version = incoming.version
    local.count = list(incoming.count)
    local.sum = [list(row) for row in incoming.sum]
    local.sumsq = [list(row) for row in incoming.sumsq]
    return True


class Node:
    """One hub. Holds a copy of every node's slot; only slots[node_id] is its own."""

    def __init__(self, node_id, num_nodes):
        if node_id < 0 or node_id >= num_nodes:
            raise ValueError("node_id out of range")
        self.node_id = node_id
        self.slots = [Slot(owner) for owner in range(num_nodes)]

    def teach(self, pixels, label):
        self.slots[self.node_id].teach(pixels, label)

    def receive(self, incoming_slot):
        return merge_slot(self.slots[incoming_slot.owner_id], incoming_slot)

    def merge_from(self, other_node):
        for slot in other_node.slots:
            self.receive(slot)

    def version_vector(self):
        return [slot.version for slot in self.slots]


class Totals:
    """Sum of all slots. The model is computed from this and never stored."""

    def __init__(self):
        self.count = [0] * NUM_CLASSES
        self.sum = [[0] * NUM_FEATURES for _ in range(NUM_CLASSES)]
        self.sumsq = [[0] * NUM_FEATURES for _ in range(NUM_CLASSES)]

    def total_count(self):
        return sum(self.count)


def combine(slots):
    totals = Totals()
    for slot in slots:
        for label in range(NUM_CLASSES):
            totals.count[label] += slot.count[label]
            for j in range(NUM_FEATURES):
                totals.sum[label][j] += slot.sum[label][j]
                totals.sumsq[label][j] += slot.sumsq[label][j]
    return totals


# ---------------------------------------------------------------------------
# Models
# ---------------------------------------------------------------------------

def nearest_centroid_predict(totals, pixels):
    """Return the label whose mean drawing is closest, or -1 if nothing was taught.

    Uses n*(x - mean) = x*n - sum, which is an exact integer, so the only
    rounding is the final division. Ties go to the lower label.
    """
    check_pixels(pixels)
    best_label = -1
    best_distance = math.inf
    for label in range(NUM_CLASSES):
        n = totals.count[label]
        if n == 0:
            continue  # never taught this digit: it has no centroid
        squared_numerator = 0
        for j in range(NUM_FEATURES):
            diff = pixels[j] * n - totals.sum[label][j]
            squared_numerator += diff * diff
        distance = float(squared_numerator) / float(n * n)
        if distance < best_distance:
            best_distance = distance
            best_label = label
    return best_label


class GaussianNB:
    """Gaussian Naive Bayes built from integer totals.

    var_floor is added to every variance. Without it, pixels that are always 0
    for a digit (like the corners) would have variance 0 and divide by zero.
    """

    def __init__(self, totals, var_floor):
        if not var_floor > 0:  # also rejects NaN
            raise ValueError("var_floor must be positive")
        self.labels = []  # only classes that have at least one sample
        self.log_prior = {}
        self.mean = {}
        self.var = {}
        self.log_norm = {}  # per class: sum of -0.5*log(2*pi*var) over all pixels

        total = totals.total_count()
        for label in range(NUM_CLASSES):
            n = totals.count[label]
            if n == 0:
                continue
            means = [0.0] * NUM_FEATURES
            variances = [0.0] * NUM_FEATURES
            log_norm = 0.0
            for j in range(NUM_FEATURES):
                s = totals.sum[label][j]
                sq = totals.sumsq[label][j]
                means[j] = float(s) / float(n)
                # n*sumsq - sum^2 is an exact non-negative integer (n^2 times the variance).
                spread = n * sq - s * s
                if spread < 0:
                    spread = 0  # impossible for real data; guards against a corrupted slot
                variances[j] = float(spread) / float(n * n) + var_floor
                log_norm += -0.5 * math.log(2.0 * math.pi * variances[j])
            self.labels.append(label)
            self.log_prior[label] = math.log(float(n) / float(total))
            self.mean[label] = means
            self.var[label] = variances
            self.log_norm[label] = log_norm

    def log_scores(self, pixels):
        check_pixels(pixels)
        scores = {}
        for label in self.labels:
            score = self.log_prior[label] + self.log_norm[label]
            means = self.mean[label]
            variances = self.var[label]
            for j in range(NUM_FEATURES):
                diff = pixels[j] - means[j]
                score -= diff * diff / (2.0 * variances[j])
            scores[label] = score
        return scores

    def predict(self, pixels):
        """Return (label, confidence). (-1, 0.0) if nothing was taught yet."""
        if not self.labels:
            return -1, 0.0
        scores = self.log_scores(pixels)
        best_label = self.labels[0]
        for label in self.labels:
            if scores[label] > scores[best_label]:
                best_label = label
        # Softmax relative to the best score so exp() never overflows.
        denominator = 0.0
        for label in self.labels:
            denominator += math.exp(scores[label] - scores[best_label])
        return best_label, 1.0 / denominator


# ---------------------------------------------------------------------------
# Serialization (used by the size test)
# ---------------------------------------------------------------------------

SLOT_UINT32_FIELDS = 1 + NUM_CLASSES + 2 * NUM_CLASSES * NUM_FEATURES
RAW_SLOT_BYTES = 4 * SLOT_UINT32_FIELDS


def slot_fields(slot):
    """Flatten a slot into one list: version, count[10], sum[10][64], sumsq[10][64]."""
    fields = [slot.version]
    fields.extend(slot.count)
    for row in slot.sum:
        fields.extend(row)
    for row in slot.sumsq:
        fields.extend(row)
    return fields


def slot_from_fields(owner_id, fields):
    if len(fields) != SLOT_UINT32_FIELDS:
        raise ValueError(f"expected {SLOT_UINT32_FIELDS} fields, got {len(fields)}")
    slot = Slot(owner_id)
    slot.version = fields[0]
    slot.count = list(fields[1:1 + NUM_CLASSES])
    position = 1 + NUM_CLASSES
    for label in range(NUM_CLASSES):
        slot.sum[label] = list(fields[position:position + NUM_FEATURES])
        position += NUM_FEATURES
    for label in range(NUM_CLASSES):
        slot.sumsq[label] = list(fields[position:position + NUM_FEATURES])
        position += NUM_FEATURES
    return slot


def serialize_raw(slot):
    """Fixed-size little-endian uint32 encoding (what the ESP32 will send)."""
    return struct.pack(f"<{SLOT_UINT32_FIELDS}I", *slot_fields(slot))


def deserialize_raw(owner_id, data):
    if len(data) != RAW_SLOT_BYTES:
        raise ValueError(f"raw slot must be {RAW_SLOT_BYTES} bytes, got {len(data)}")
    return slot_from_fields(owner_id, list(struct.unpack(f"<{SLOT_UINT32_FIELDS}I", data)))


def serialize_cbor(slot):
    import cbor2  # only needed for the size comparison

    return cbor2.dumps(slot_fields(slot))


def deserialize_cbor(owner_id, data):
    import cbor2

    return slot_from_fields(owner_id, cbor2.loads(data))
