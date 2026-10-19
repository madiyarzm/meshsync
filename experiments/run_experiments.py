"""Phase 1 experiments: accuracy per room, merge exactness, and slot size.

Run:  python experiments/run_experiments.py
Everything is seeded, so the printed numbers are the same on every run.
"""

import random

import numpy as np
from sklearn.datasets import load_digits
from sklearn.model_selection import train_test_split
from sklearn.naive_bayes import GaussianNB as SklearnGaussianNB
from sklearn.neighbors import NearestCentroid as SklearnNearestCentroid

from construct_proto import (
    NUM_CLASSES,
    RAW_SLOT_BYTES,
    GaussianNB,
    Node,
    Slot,
    combine,
    deserialize_cbor,
    deserialize_raw,
    nearest_centroid_predict,
    serialize_cbor,
    serialize_raw,
)

VAR_FLOOR = 1.0  # chosen by the validation sweep below (training data only)
SEED = 0


def print_table(headers, rows):
    widths = [len(h) for h in headers]
    for row in rows:
        for i, cell in enumerate(row):
            widths[i] = max(widths[i], len(str(cell)))
    line = "  ".join(h.ljust(widths[i]) for i, h in enumerate(headers))
    print(line)
    print("-" * len(line))
    for row in rows:
        print("  ".join(str(cell).ljust(widths[i]) for i, cell in enumerate(row)))
    print()


def percent(value):
    return f"{100.0 * value:.1f}%"


def load_data():
    pixels, labels = load_digits(return_X_y=True)
    pixels = pixels.astype(int)  # values are already whole numbers 0-16
    return train_test_split(pixels, labels, test_size=0.25, stratify=labels, random_state=SEED)


def as_samples(pixels, labels):
    """numpy arrays -> list of (list of Python ints, int) for the prototype."""
    return list(zip(pixels.tolist(), labels.tolist()))


def accuracy_nearest_centroid(totals, samples):
    if not samples:
        return 0.0
    correct = 0
    for pixels, label in samples:
        if nearest_centroid_predict(totals, pixels) == label:
            correct += 1
    return correct / len(samples)


def accuracy_gaussian_nb(totals, samples):
    if not samples:
        return 0.0
    model = GaussianNB(totals, VAR_FLOOR)
    correct = 0
    for pixels, label in samples:
        if model.predict(pixels)[0] == label:
            correct += 1
    return correct / len(samples)


def centralized_totals(train_x, train_y):
    """Sums computed straight from the arrays with numpy, independent of the slot code."""
    count = np.bincount(train_y, minlength=NUM_CLASSES).tolist()
    sums = []
    sumsqs = []
    for label in range(NUM_CLASSES):
        rows = train_x[train_y == label]
        sums.append(rows.sum(axis=0).tolist())
        sumsqs.append((rows * rows).sum(axis=0).tolist())
    return count, sums, sumsqs


def totals_equal_central(totals, central):
    count, sums, sumsqs = central
    return totals.count == count and totals.sum == sums and totals.sumsq == sumsqs


# ---------------------------------------------------------------------------

def check_against_sklearn(train_x, train_y, test_x):
    """Sanity check: our models should agree with scikit-learn's versions."""
    slot = Slot(0)
    for pixels, label in as_samples(train_x, train_y):
        slot.teach(pixels, label)
    totals = combine([slot])
    test_samples = as_samples(test_x, np.zeros(len(test_x), dtype=int))

    sk_centroid = SklearnNearestCentroid().fit(train_x, train_y).predict(test_x)
    ours_centroid = [nearest_centroid_predict(totals, pixels) for pixels, _ in test_samples]

    # sklearn adds var_smoothing * (largest per-pixel variance) to every variance.
    sk_nb = SklearnGaussianNB().fit(train_x, train_y)
    epsilon = 1e-9 * np.var(train_x, axis=0).max()
    ours_nb_model = GaussianNB(totals, epsilon)
    ours_nb = [ours_nb_model.predict(pixels)[0] for pixels, _ in test_samples]

    rows = [
        ["nearest centroid", percent(np.mean(np.array(ours_centroid) == sk_centroid))],
        ["gaussian NB (sklearn's smoothing)", percent(np.mean(np.array(ours_nb) == sk_nb.predict(test_x)))],
    ]
    print("== Check: agreement with scikit-learn on the test set ==")
    print_table(["model", "same prediction"], rows)


def sweep_var_floor(train_x, train_y):
    """Pick VAR_FLOOR on a validation split carved out of the training data."""
    fit_x, val_x, fit_y, val_y = train_test_split(
        train_x, train_y, test_size=0.25, stratify=train_y, random_state=SEED + 1
    )
    slot = Slot(0)
    for pixels, label in as_samples(fit_x, fit_y):
        slot.teach(pixels, label)
    totals = combine([slot])
    val_samples = as_samples(val_x, val_y)

    rows = []
    for var_floor in [0.01, 0.1, 0.5, 1.0, 2.0, 4.0, 8.0]:
        model = GaussianNB(totals, var_floor)
        correct = sum(1 for pixels, label in val_samples if model.predict(pixels)[0] == label)
        rows.append([var_floor, percent(correct / len(val_samples))])
    print(f"== Gaussian NB variance floor (fit {len(fit_x)}, validate {len(val_x)}; using {VAR_FLOOR}) ==")
    print_table(["var_floor", "validation accuracy"], rows)


def run_room_experiment(name, room_a_mask, train_x, train_y, test_x, test_y, own_digits=None):
    room_a = Node(0, 2)
    room_b = Node(1, 2)
    for pixels, label in as_samples(train_x[room_a_mask], train_y[room_a_mask]):
        room_a.teach(pixels, label)
    for pixels, label in as_samples(train_x[~room_a_mask], train_y[~room_a_mask]):
        room_b.teach(pixels, label)

    test_all = as_samples(test_x, test_y)
    states = []

    def add_row(state, totals, test_name, samples):
        states.append([
            state,
            test_name,
            len(samples),
            percent(accuracy_nearest_centroid(totals, samples)),
            percent(accuracy_gaussian_nb(totals, samples)),
        ])

    before_a = combine(room_a.slots)
    before_b = combine(room_b.slots)
    add_row("room A before merge", before_a, "all digits", test_all)
    if own_digits:
        digits_a, digits_b = own_digits
        own_a = [s for s in test_all if s[1] in digits_a]
        own_b = [s for s in test_all if s[1] in digits_b]
        add_row("room A before merge", before_a, f"digits {digits_a[0]}-{digits_a[-1]}", own_a)
    add_row("room B before merge", before_b, "all digits", test_all)
    if own_digits:
        add_row("room B before merge", before_b, f"digits {digits_b[0]}-{digits_b[-1]}", own_b)

    # Both directions of a sync: each room merges the other's slots.
    room_a.merge_from(room_b)
    room_b.merge_from(room_a)
    after_a = combine(room_a.slots)
    after_b = combine(room_b.slots)
    add_row("room A after merge", after_a, "all digits", test_all)
    add_row("room B after merge", after_b, "all digits", test_all)

    central = centralized_totals(train_x, train_y)
    central_slot = Slot(0)
    for pixels, label in as_samples(train_x, train_y):
        central_slot.teach(pixels, label)
    central_totals = combine([central_slot])
    add_row("centralized", central_totals, "all digits", test_all)

    print(f"== Accuracy, split: {name} ==")
    print(f"train: room A {int(room_a_mask.sum())}, room B {int((~room_a_mask).sum())}; test {len(test_x)}")
    print_table(["state", "test set", "n", "nearest centroid", "gaussian NB"], states)

    nb_central = GaussianNB(central_totals, VAR_FLOOR)
    nb_merged = GaussianNB(after_a, VAR_FLOOR)
    same_predictions = all(
        nb_central.log_scores(pixels) == nb_merged.log_scores(pixels)
        and nearest_centroid_predict(central_totals, pixels) == nearest_centroid_predict(after_a, pixels)
        for pixels, _ in test_all
    )
    print(f"rooms A and B hold identical slots after merge: {room_a.slots == room_b.slots}")
    print(f"merged sums == numpy centralized sums:          {totals_equal_central(after_a, central)}")
    print(f"merged scores == centralized scores (bit-exact): {same_predictions}")
    print()


def run_exactness_test(train_x, train_y, num_nodes=5, num_orders=300):
    """Merge every slot snapshot (stale copies and duplicates too) in many random orders."""
    rng = random.Random(SEED)
    nodes = [Node(i, num_nodes) for i in range(num_nodes)]
    snapshots = []  # every slot version a node ever sent, like gossip in flight

    for pixels, label in as_samples(train_x, train_y):
        owner = rng.randrange(num_nodes)
        nodes[owner].teach(pixels, label)
        if rng.random() < 0.1:
            snapshots.append(nodes[owner].slots[owner].copy())
    expected_slots = []
    for node in nodes:
        latest = node.slots[node.node_id].copy()
        snapshots.append(latest)
        expected_slots.append(latest)

    central = centralized_totals(train_x, train_y)
    sequential_ok = 0
    tree_ok = 0
    for trial in range(num_orders):
        messages = snapshots + rng.sample(snapshots, len(snapshots) // 2)  # duplicates
        rng.shuffle(messages)

        # 1) One receiver gets every message in a random order.
        receiver = Node(trial % num_nodes, num_nodes)
        for slot in messages:
            receiver.receive(slot)
        if receiver.slots == expected_slots and totals_equal_central(combine(receiver.slots), central):
            sequential_ok += 1

        # 2) Messages split across several replicas, which then merge with each other.
        replicas = [Node(i % num_nodes, num_nodes) for i in range(rng.randint(2, 6))]
        for slot in messages:
            rng.choice(replicas).receive(slot)
        rng.shuffle(replicas)
        for i in range(1, len(replicas)):
            replicas[0].merge_from(replicas[i])
        if replicas[0].slots == expected_slots and totals_equal_central(combine(replicas[0].slots), central):
            tree_ok += 1

    print(f"== Exactness: {num_nodes} nodes, {len(snapshots)} snapshots (incl. stale), "
          f"+50% duplicates per trial ==")
    print_table(
        ["merge pattern", "orders tried", "equal to centralized"],
        [
            ["one receiver, random order", num_orders, sequential_ok],
            ["random replicas merged pairwise", num_orders, tree_ok],
        ],
    )


def run_size_test(train_x, train_y):
    rng = random.Random(SEED)
    samples = as_samples(train_x, train_y)
    rng.shuffle(samples)

    rows = []
    for size in [0, 1, 10, 100, 500, 1000, len(samples)]:
        slot = Slot(0)
        for pixels, label in samples[:size]:
            slot.teach(pixels, label)
        raw = serialize_raw(slot)
        cbor = serialize_cbor(slot)
        round_trip = deserialize_raw(0, raw) == slot and deserialize_cbor(0, cbor) == slot
        largest = max(max(row) for row in slot.sumsq)
        rows.append([size, largest, len(raw), len(cbor), f"{len(cbor) / len(raw):.2f}", round_trip])

    print(f"== Size of one slot: raw uint32 vs CBOR (cbor2) ==")
    print_table(["samples taught", "largest sumsq", "raw bytes", "cbor bytes", "cbor/raw", "round-trip ok"], rows)
    assert all(row[2] == RAW_SLOT_BYTES for row in rows)


def main():
    train_x, test_x, train_y, test_y = load_data()
    print(f"sklearn digits: {len(train_x)} train, {len(test_x)} test, 64 pixels (0-16), seed {SEED}\n")

    check_against_sklearn(train_x, train_y, test_x)
    sweep_var_floor(train_x, train_y)

    by_digit = train_y <= 4
    run_room_experiment(
        "by digit (A: 0-4, B: 5-9)", by_digit, train_x, train_y, test_x, test_y,
        own_digits=([0, 1, 2, 3, 4], [5, 6, 7, 8, 9]),
    )

    order = np.random.default_rng(SEED).permutation(len(train_x))
    random_half = np.zeros(len(train_x), dtype=bool)
    random_half[order[: len(train_x) // 2]] = True
    run_room_experiment("random halves", random_half, train_x, train_y, test_x, test_y)

    run_exactness_test(train_x, train_y)
    run_size_test(train_x, train_y)


if __name__ == "__main__":
    main()
