"""Unit tests for the Python prototype. Run: python -m pytest experiments"""

import random

import pytest

from construct_proto import (
    MAX_SAMPLES_PER_CLASS,
    NUM_FEATURES,
    RAW_SLOT_BYTES,
    GaussianNB,
    Node,
    Slot,
    combine,
    deserialize_cbor,
    deserialize_raw,
    merge_slot,
    nearest_centroid_predict,
    serialize_cbor,
    serialize_raw,
)

NUM_NODES = 4


def random_drawing(rng):
    return [rng.randint(0, 16) for _ in range(NUM_FEATURES)]


def random_history(rng, teaches=40):
    """Every version each node's slot went through while teaching random drawings."""
    history = [[Slot(owner)] for owner in range(NUM_NODES)]
    for _ in range(teaches):
        owner = rng.randrange(NUM_NODES)
        slot = history[owner][-1].copy()
        slot.teach(random_drawing(rng), rng.randrange(10))
        history[owner].append(slot)
    return history


def random_state(rng, history):
    """A replica that has seen some random (possibly stale) version of each slot."""
    node = Node(0, NUM_NODES)
    for owner in range(NUM_NODES):
        node.receive(rng.choice(history[owner]))
    return node


def merged(*nodes):
    result = Node(0, NUM_NODES)
    for node in nodes:
        result.merge_from(node)
    return result.slots


def merged_node(*nodes):
    result = Node(0, NUM_NODES)
    result.slots = merged(*nodes)
    return result


# --- teach ------------------------------------------------------------------

def test_teach_updates_sums_and_version():
    slot = Slot(0)
    drawing = list(range(16)) * 4
    slot.teach(drawing, 3)
    assert slot.version == 1
    assert slot.count[3] == 1
    assert slot.sum[3] == drawing
    assert slot.sumsq[3] == [v * v for v in drawing]


@pytest.mark.parametrize(
    "pixels,label",
    [
        ([0] * 64, 10),          # label too large
        ([0] * 64, -1),          # negative label
        ([0] * 63, 1),           # truncated drawing
        ([0] * 65, 1),           # too long
        ([0] * 63 + [17], 1),    # pixel out of range
        ([0] * 63 + [-1], 1),    # negative pixel
        ([0] * 63 + [1.5], 1),   # not an integer
    ],
)
def test_bad_teach_is_rejected_and_changes_nothing(pixels, label):
    slot = Slot(0)
    with pytest.raises(ValueError):
        slot.teach(pixels, label)
    assert slot == Slot(0)


def test_teach_refuses_to_overflow():
    slot = Slot(0)
    slot.count[2] = MAX_SAMPLES_PER_CLASS
    with pytest.raises(OverflowError):
        slot.teach([16] * 64, 2)


# --- merge / CRDT laws ------------------------------------------------------

def test_merge_keeps_higher_version_and_ignores_stale():
    old = Slot(1)
    new = old.copy()
    new.teach([5] * 64, 7)
    assert merge_slot(old, new) is True
    assert old == new
    stale = Slot(1)
    assert merge_slot(old, stale) is False
    assert old == new


def test_merge_rejects_wrong_owner():
    with pytest.raises(ValueError):
        merge_slot(Slot(0), Slot(1))


@pytest.mark.parametrize("seed", range(20))
def test_merge_is_commutative_associative_idempotent(seed):
    rng = random.Random(seed)
    history = random_history(rng)
    a, b, c = (random_state(rng, history) for _ in range(3))
    assert merged(a, b) == merged(b, a)
    assert merged(merged_node(a, b), c) == merged(a, merged_node(b, c))
    assert merged(a, a) == merged(a)


def test_teach_and_merge_on_a_node():
    a = Node(0, 2)
    b = Node(1, 2)
    a.teach([1] * 64, 0)
    b.teach([2] * 64, 1)
    b.teach([3] * 64, 1)
    a.merge_from(b)
    assert a.version_vector() == [1, 2]
    totals = combine(a.slots)
    assert totals.count[:2] == [1, 2]
    assert totals.sum[1] == [5] * 64


# --- models -----------------------------------------------------------------

def test_models_with_nothing_taught():
    totals = combine([Slot(0)])
    assert nearest_centroid_predict(totals, [0] * 64) == -1
    assert GaussianNB(totals, 1.0).predict([0] * 64) == (-1, 0.0)


def test_models_skip_empty_classes():
    slot = Slot(0)
    slot.teach([16] * 64, 9)  # one sample: variance 0, only the floor keeps it finite
    totals = combine([slot])
    assert nearest_centroid_predict(totals, [0] * 64) == 9
    label, confidence = GaussianNB(totals, 1.0).predict([0] * 64)
    assert label == 9
    assert confidence == 1.0


def test_nearest_centroid_tie_goes_to_lower_label():
    slot = Slot(0)
    slot.teach([0] * 64, 4)
    slot.teach([2] * 64, 6)
    assert nearest_centroid_predict(combine([slot]), [1] * 64) == 4


def test_gaussian_nb_picks_closer_class_with_confidence():
    slot = Slot(0)
    for _ in range(5):
        slot.teach([0] * 64, 0)
        slot.teach([16] * 64, 1)
    label, confidence = GaussianNB(combine([slot]), 1.0).predict([15] * 64)
    assert label == 1
    assert 0.5 < confidence <= 1.0


def test_gaussian_nb_rejects_non_positive_floor():
    with pytest.raises(ValueError):
        GaussianNB(combine([Slot(0)]), 0.0)


# --- serialization ----------------------------------------------------------

def test_raw_and_cbor_round_trip():
    rng = random.Random(0)
    slot = Slot(2)
    for _ in range(30):
        slot.teach(random_drawing(rng), rng.randrange(10))
    raw = serialize_raw(slot)
    assert len(raw) == RAW_SLOT_BYTES
    assert deserialize_raw(2, raw) == slot
    assert deserialize_cbor(2, serialize_cbor(slot)) == slot


def test_raw_rejects_truncated_data():
    raw = serialize_raw(Slot(0))
    with pytest.raises(ValueError):
        deserialize_raw(0, raw[:-1])
