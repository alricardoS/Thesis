#!/usr/bin/env python3
"""
Aggregate per-AC and summed-AC metrics for multi-STA priority suite results.

Creates two folders inside the suite results root:
- aggregated_per_ac: picks a representative STA (default STA0) per test and records its metrics per AC
- aggregated_sum_all_acs: sums metrics across all STAs of the same AC per test

Usage:
  python3 scripts/aggregate_multi_sta_priority.py \
    --results-root /home/ricardosantos/ns-3.47/results_multi_sta_priority_suite_base

The script writes CSV summaries under the two folders and attempts to generate PNG barplots
if matplotlib is installed. It uses only standard-library CSV parsing; matplotlib is optional.
"""
import argparse
import csv
import glob
import os
import re
import sys
from collections import defaultdict, OrderedDict
from decimal import Decimal, ROUND_HALF_UP

try:
    import matplotlib.pyplot as plt
    MATPLOTLIB = True
except Exception:
    MATPLOTLIB = False


AC_MAP = {
    "all_voice": "VO",
    "all_video": "VI",
    "all_besteffort": "BE",
}


def read_csv(path):
    with open(path, newline="") as f:
        reader = csv.DictReader(f)
        return list(reader)


def ensure_dir(path):
    os.makedirs(path, exist_ok=True)


def gather_ac_folders(results_root):
    folders = []
    for name in sorted(os.listdir(results_root)):
        p = os.path.join(results_root, name)
        if os.path.isdir(p):
            folders.append(name)
    return folders


def map_ac(folder_name):
    return AC_MAP.get(folder_name, folder_name)


def parse_data_rate(scenario):
    m = re.search(r"(\d+(?:\.\d+)?)Mbps", str(scenario))
    return float(m.group(1)) if m else float("inf")


def freq_rank(freq):
    rank = {
        "2.4GHz": 0,
        "5GHz": 1,
        "6GHz": 2,
        "2.4+5": 0,
        "2.4+6": 1,
        "5+6": 2,
    }
    return rank.get(str(freq), 99)


def sort_key_scenario_freq(key):
    scenario, freq = key
    return (parse_data_rate(scenario), str(scenario), freq_rank(freq), str(freq))


def format_one_decimal_half_up(value):
    return str(Decimal(str(value)).quantize(Decimal("0.1"), rounding=ROUND_HALF_UP))


def format_annotation_value(value):
    decimal_value = Decimal(str(value))
    if abs(decimal_value) < Decimal("10"):
        return str(decimal_value.quantize(Decimal("0.1"), rounding=ROUND_HALF_UP))
    return str(decimal_value.quantize(Decimal("1"), rounding=ROUND_HALF_UP))


def clean_scenario_label(scenario):
    scenario = str(scenario)
    scenario = re.sub(r"_\d+stas\b", "", scenario)
    scenario = re.sub(r"__+", "_", scenario)
    return scenario.strip("_")


def aggregate_slo(results_root, ac_folders, representative_sta="STA0"):
    # Read single_link_per_sta_detailed.csv from each AC folder
    per_metric_per_key = defaultdict(lambda: defaultdict(dict))
    # key = (scenario, freq_band)
    metrics = ["throughput_mbps", "delay_ms", "jitter_ms", "loss_rate_pct"]

    for ac_folder in ac_folders:
        ac_label = map_ac(ac_folder)
        table_path = os.path.join(results_root, ac_folder, "tables", "single_link_per_sta_detailed.csv")
        if not os.path.isfile(table_path):
            continue
        rows = read_csv(table_path)
        grouped = defaultdict(list)
        for r in rows:
            key = (r["scenario"], r["freq_band"])
            grouped[key].append(r)

        for key, rows_list in grouped.items():
            # representative: find row with sta_id == representative_sta
            rep_row = next((r for r in rows_list if r.get("sta_id") == representative_sta), None)
            if rep_row is None:
                rep_row = rows_list[0]

            for m in metrics:
                # per-AC representative value
                try:
                    per_metric_per_key[("per_ac", m)][key][ac_label] = float(rep_row.get(m, "0") or 0)
                except Exception:
                    per_metric_per_key[("per_ac", m)][key][ac_label] = 0.0

    return per_metric_per_key


def aggregate_mlo(results_root, ac_folders, representative_sta="STA0"):
    # Similar to SLO but using mlo_per_sta_detailed.csv and key is (scenario, freq_pair)
    per_metric_per_key = defaultdict(lambda: defaultdict(dict))
    metrics = ["throughput_mbps", "delay_ms", "jitter_ms", "loss_rate_pct"]

    for ac_folder in ac_folders:
        ac_label = map_ac(ac_folder)
        table_path = os.path.join(results_root, ac_folder, "tables", "mlo_per_sta_detailed.csv")
        if not os.path.isfile(table_path):
            continue
        rows = read_csv(table_path)
        grouped = defaultdict(list)
        for r in rows:
            key = (r["scenario"], r["freq_pair"])
            grouped[key].append(r)

        for key, rows_list in grouped.items():
            rep_row = next((r for r in rows_list if r.get("sta_id") == representative_sta), None)
            if rep_row is None:
                rep_row = rows_list[0]

            for m in metrics:
                try:
                    per_metric_per_key[("per_ac", m)][key][ac_label] = float(rep_row.get(m, "0") or 0)
                except Exception:
                    per_metric_per_key[("per_ac", m)][key][ac_label] = 0.0

    return per_metric_per_key


def pick_preferred_protocol_rows(rows, link_col):
    by_link = defaultdict(list)
    for r in rows:
        by_link[r.get(link_col, "")].append(r)
    out = []
    for _, items in by_link.items():
        udp = [x for x in items if str(x.get("protocol", "")).upper() == "UDP"]
        out.append(udp[0] if udp else items[0])
    return out


def aggregate_sum_slo_from_results(results_root, ac_folders):
    metrics = ["throughput_mbps", "delay_ms", "jitter_ms", "loss_rate_pct"]
    out = defaultdict(lambda: defaultdict(dict))

    for ac_folder in ac_folders:
        ac_label = map_ac(ac_folder)
        outputs_dirs = sorted(glob.glob(os.path.join(results_root, ac_folder, "outputs_*")))
        for outputs_dir in outputs_dirs:
            scenario = os.path.basename(outputs_dir).replace("outputs_", "")
            csv_files = glob.glob(os.path.join(outputs_dir, "single_multi_sta_results_*.csv"))
            if not csv_files:
                continue
            csv_file = max(csv_files, key=os.path.getmtime)
            rows = read_csv(csv_file)
            rows = pick_preferred_protocol_rows(rows, "freq_band")

            for r in rows:
                key = (scenario, r.get("freq_band", ""))
                for m in metrics:
                    try:
                        out[("sum_ac", m)][key][ac_label] = float(r.get(m, "0") or 0)
                    except Exception:
                        out[("sum_ac", m)][key][ac_label] = 0.0

    return out


def aggregate_sum_mlo_from_results(results_root, ac_folders):
    metrics = ["throughput_mbps", "delay_ms", "jitter_ms", "loss_rate_pct"]
    out = defaultdict(lambda: defaultdict(dict))

    for ac_folder in ac_folders:
        ac_label = map_ac(ac_folder)
        outputs_dirs = sorted(glob.glob(os.path.join(results_root, ac_folder, "outputs_*")))
        for outputs_dir in outputs_dirs:
            scenario = os.path.basename(outputs_dir).replace("outputs_", "")
            csv_files = glob.glob(os.path.join(outputs_dir, "mlo_multi_sta_results_*.csv"))
            if not csv_files:
                continue
            csv_file = max(csv_files, key=os.path.getmtime)
            rows = read_csv(csv_file)
            rows = pick_preferred_protocol_rows(rows, "pair")

            for r in rows:
                key = (scenario, r.get("pair", ""))
                for m in metrics:
                    try:
                        out[("sum_ac", m)][key][ac_label] = float(r.get(m, "0") or 0)
                    except Exception:
                        out[("sum_ac", m)][key][ac_label] = 0.0

    return out


def write_summary_csv(out_dir, metric_dict, ac_order=None, key_names=("scenario", "freq")):
    csv_dir = os.path.join(out_dir, "csv")
    plots_dir = os.path.join(out_dir, "plots")
    ensure_dir(csv_dir)
    ensure_dir(plots_dir)

    # cleanup old files inside csv/ and plots/
    for d in (csv_dir, plots_dir):
        for fname in os.listdir(d):
            fpath = os.path.join(d, fname)
            if os.path.isfile(fpath) and (fname.endswith('.csv') or fname.endswith('.png')):
                try:
                    os.remove(fpath)
                except Exception:
                    pass

    # cleanup old top-level csv/png files in out_dir (keep csv/ and plots/ dirs)
    for fname in os.listdir(out_dir):
        fpath = os.path.join(out_dir, fname)
        if os.path.isfile(fpath) and (fname.endswith('.csv') or fname.endswith('.png')):
            try:
                os.remove(fpath)
            except Exception:
                pass
    for (mode, metric), mapdata in metric_dict.items():
        fn = f"{mode}_{metric}_summary.csv"
        path = os.path.join(csv_dir, fn)
        # collect sorted keys
        keys = sorted(mapdata.keys(), key=sort_key_scenario_freq)
        # collect all ACs
        acs = OrderedDict()
        for k in keys:
            for ac in mapdata[k].keys():
                acs[ac] = True
        if ac_order:
            ac_list = [a for a in ac_order if a in acs] + [a for a in acs if a not in (ac_order or [])]
        else:
            ac_list = list(acs.keys())

        with open(path, "w", newline="") as f:
            writer = csv.writer(f)
            header = [key_names[0], key_names[1]] + ac_list
            writer.writerow(header)
            for key in keys:
                row = [key[0], key[1]]
                for ac in ac_list:
                    v = mapdata[key].get(ac, "")
                    row.append(v)
                writer.writerow(row)

        # try plotting into plots_dir
        if MATPLOTLIB:
            try:
                out_png = os.path.join(plots_dir, fn.replace('.csv', '.png'))
                plot_csv(path, ac_list, key_names, out_png)
            except Exception as e:
                print(f"[WARN] failed plotting {path}: {e}")

        # Also generate per-data-rate CSVs and annotated plots (one plot per data rate)
        try:
            rate_groups = {}
            for key in keys:
                rate = int(parse_data_rate(key[0])) if parse_data_rate(key[0]) != float('inf') else None
                rate_groups.setdefault(rate, []).append(key)

            for rate, group_keys in rate_groups.items():
                if rate is None:
                    continue
                rate_dir_csv = os.path.join(csv_dir, f"by_rate_{rate}Mbps")
                rate_dir_plots = os.path.join(plots_dir, f"by_rate_{rate}Mbps")
                ensure_dir(rate_dir_csv)
                ensure_dir(rate_dir_plots)

                rate_csv_path = os.path.join(rate_dir_csv, fn.replace('.csv', f'_{rate}Mbps.csv'))
                with open(rate_csv_path, 'w', newline='') as rf:
                    writer = csv.writer(rf)
                    header = [key_names[0], key_names[1]] + ac_list
                    writer.writerow(header)
                    for key in sorted(group_keys, key=sort_key_scenario_freq):
                        row = [key[0], key[1]]
                        for ac in ac_list:
                            row.append(mapdata[key].get(ac, ""))
                        writer.writerow(row)

                # annotated plot for this rate
                rate_png = os.path.join(rate_dir_plots, fn.replace('.csv', f'_{rate}Mbps.png'))
                try:
                    plot_csv_annotated(rate_csv_path, ac_list, key_names, rate_png)
                except Exception as e:
                    print(f"[WARN] failed annotated plotting {rate_csv_path}: {e}")
        except Exception:
            pass


def plot_csv(csv_path, ac_list, key_names, out_png):
    # Simple grouped bar chart: each group is a key (scenario+freq), bars are ACs
    rows = []
    with open(csv_path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        for row in r:
            rows.append(row)

    if not rows:
        return

    groups = [f"{clean_scenario_label(r[0])}|{r[1]}" for r in rows]
    data = []
    for i, ac in enumerate(ac_list):
        col = []
        for r in rows:
            try:
                col.append(float(r[2 + i]))
            except Exception:
                col.append(0.0)
        data.append(col)

    import numpy as _np

    # colors/hatches similar to existing scripts
    STA_COLORS = ['#E74C3C', '#3498DB', '#2ECC71', '#F39C12']
    STA_HATCHES = ['//', 'xx', '..', '\\\\']

    # ensure AC order common (BK, BE, VI, VO) if present
    default_ac_order = ['BK', 'BE', 'VI', 'VO']
    ac_list_sorted = [a for a in default_ac_order if a in ac_list] + [a for a in ac_list if a not in default_ac_order]

    # reorder data to match ac_list_sorted
    data_sorted = []
    for ac in ac_list_sorted:
        idx = ac_list.index(ac)
        data_sorted.append(data[idx])

    x = _np.arange(len(groups))
    num_ac = len(ac_list_sorted)
    width = 0.75 / max(1, num_ac)
    fig, ax = plt.subplots(figsize=(12, 7))

    all_bars = []
    bars_by_group = defaultdict(list)
    for i, col in enumerate(data_sorted):
        bars = ax.bar(x + (i - (num_ac - 1) / 2) * width, col, width=width * 0.9,
                      label=ac_list_sorted[i],
                      color=STA_COLORS[i % len(STA_COLORS)],
                      hatch=STA_HATCHES[i % len(STA_HATCHES)],
                      edgecolor='black', linewidth=0.5)
        all_bars.extend(bars)
        for gi, bar in enumerate(bars):
            bars_by_group[gi].append((i, bar))
        for bar in bars:
            h = bar.get_height()
            if h == 0:
                continue
            ax.annotate(
                format_annotation_value(h),
                xy=(bar.get_x() + bar.get_width() / 2, h),
                xytext=((i - (num_ac - 1) / 2) * 2, 3),
                textcoords='offset points',
                ha='center',
                va='bottom',
                fontsize=10,
                fontweight='bold',
            )

    ax.tick_params(axis='y', labelsize=16)
    plt.setp(ax.get_yticklabels(), fontweight='bold')

    ax.set_xticks(x)
    ax.set_xticklabels(groups, fontsize=12, fontweight='bold', rotation=45, ha='right')

    # xlabel: frequency band vs pair
    xlabel = 'Frequency Band' if 'freq_band' in key_names[1] or 'freq_band' in key_names else 'Frequency Pair'
    ax.set_xlabel(xlabel, fontsize=12, fontweight='bold')

    # derive metric friendly name and ylabel
    fname = os.path.basename(csv_path).lower()
    if 'throughput' in fname:
        metric_label = 'Throughput (Mbps)'
    elif 'delay' in fname:
        metric_label = 'Delay (ms)'
    elif 'jitter' in fname:
        metric_label = 'Jitter (ms)'
    elif 'loss' in fname:
        metric_label = 'Packet Loss (%)'
    else:
        metric_label = os.path.basename(csv_path).replace('_', ' ').replace('.csv', '')

    ax.set_ylabel(metric_label, fontsize=12, fontweight='bold')

    # Title format to match example: 'MLO Delay - UDP (Aggregated per AC)'
    mode_label = 'MLO' if 'freq_pair' in key_names[1] or 'mlo' in fname else 'Single Link'
    protocol = 'UDP'  # default; aggregated CSVs don't include protocol column
    agg_label = 'Aggregated sum all ACs' if fname.startswith('sum_ac_') else 'Aggregated per AC'
    ax.set_title(f'{mode_label} {metric_label} - {protocol} ({agg_label})', fontsize=13, fontweight='bold')

    ax.legend(title='AC', fontsize=9)
    ax.grid(axis='y', alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)


def plot_csv_annotated(csv_path, ac_list, key_names, out_png):
    # Similar to plot_csv but adds numeric annotations above each bar.
    rows = []
    with open(csv_path, newline="") as f:
        r = csv.reader(f)
        header = next(r)
        for row in r:
            rows.append(row)

    if not rows:
        return

    groups = [f"{clean_scenario_label(r[0])}|{r[1]}" for r in rows]
    data = []
    for i, ac in enumerate(ac_list):
        col = []
        for r in rows:
            try:
                col.append(float(r[2 + i]))
            except Exception:
                col.append(0.0)
        data.append(col)

    import numpy as _np

    STA_COLORS = ['#E74C3C', '#3498DB', '#2ECC71', '#F39C12']
    STA_HATCHES = ['//', 'xx', '..', '\\\\']

    default_ac_order = ['BK', 'BE', 'VI', 'VO']
    ac_list_sorted = [a for a in default_ac_order if a in ac_list] + [a for a in ac_list if a not in default_ac_order]

    data_sorted = []
    for ac in ac_list_sorted:
        idx = ac_list.index(ac)
        data_sorted.append(data[idx])

    x = _np.arange(len(groups))
    num_ac = len(ac_list_sorted)
    width = 0.75 / max(1, num_ac)
    fig, ax = plt.subplots(figsize=(12, 7))

    for i, col in enumerate(data_sorted):
        bars = ax.bar(x + (i - (num_ac - 1) / 2) * width, col, width=width * 0.9,
                      label=ac_list_sorted[i],
                      color=STA_COLORS[i % len(STA_COLORS)],
                      hatch=STA_HATCHES[i % len(STA_HATCHES)],
                      edgecolor='black', linewidth=0.5)
        for bar in bars:
            h = bar.get_height()
            if h == 0:
                continue
            label = format_annotation_value(h)
            ax.annotate(label,
                        xy=(bar.get_x() + bar.get_width() / 2, h),
                        xytext=((i - (num_ac - 1) / 2) * 2, 3),
                        textcoords='offset points',
                        ha='center', va='bottom', fontsize=10, fontweight='bold')

    ax.tick_params(axis='y', labelsize=14)
    plt.setp(ax.get_yticklabels(), fontweight='bold')

    ax.set_xticks(x)
    ax.set_xticklabels(groups, fontsize=12, fontweight='bold', rotation=45, ha='right')

    xlabel = 'Frequency Band' if 'freq_band' in key_names[1] or 'freq_band' in key_names else 'Frequency Pair'
    ax.set_xlabel(xlabel, fontsize=12, fontweight='bold')

    fname = os.path.basename(csv_path).lower()
    if 'throughput' in fname:
        metric_label = 'Throughput (Mbps)'
    elif 'delay' in fname:
        metric_label = 'Delay (ms)'
    elif 'jitter' in fname:
        metric_label = 'Jitter (ms)'
    elif 'loss' in fname:
        metric_label = 'Packet Loss (%)'
    else:
        metric_label = os.path.basename(csv_path).replace('_', ' ').replace('.csv', '')

    ax.set_ylabel(metric_label, fontsize=12, fontweight='bold')

    mode_label = 'MLO' if 'freq_pair' in key_names[1] or 'mlo' in fname else 'Single Link'
    protocol = 'UDP'
    agg_label = 'Aggregated sum all ACs' if fname.startswith('sum_ac_') else 'Aggregated per AC'
    ax.set_title(f'{mode_label} {metric_label} - {protocol} ({agg_label})', fontsize=13, fontweight='bold')

    ax.legend(title='AC', fontsize=9)
    ax.grid(axis='y', alpha=0.3)

    fig.tight_layout()
    fig.savefig(out_png, dpi=150)
    plt.close(fig)


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--results-root", default="results_multi_sta_priority_suite_base")
    p.add_argument("--representative-sta", default="STA0")
    args = p.parse_args()

    results_root = args.results_root
    if not os.path.isabs(results_root):
        results_root = os.path.join(os.getcwd(), results_root)

    if not os.path.isdir(results_root):
        print("results root not found:", results_root)
        sys.exit(1)

    ac_folders = gather_ac_folders(results_root)
    ac_folders = [f for f in ac_folders if os.path.isdir(os.path.join(results_root, f, "tables"))]
    if not ac_folders:
        print("no AC folders with tables/ found in:", results_root)
        sys.exit(1)

    # preferred AC order
    ac_order = [map_ac(x) for x in ["all_voice", "all_video", "all_besteffort"] if x in ac_folders]

    slo_per_ac = aggregate_slo(results_root, ac_folders, representative_sta=args.representative_sta)
    mlo_per_ac = aggregate_mlo(results_root, ac_folders, representative_sta=args.representative_sta)
    slo_sum_ac = aggregate_sum_slo_from_results(results_root, ac_folders)
    mlo_sum_ac = aggregate_sum_mlo_from_results(results_root, ac_folders)

    out_base_a = os.path.join(results_root, "aggregated_per_ac")
    out_base_b = os.path.join(results_root, "aggregated_sum_all_acs")

    write_summary_csv(
        os.path.join(out_base_a, "slo"),
        {k: v for k, v in slo_per_ac.items() if k[0] == "per_ac"},
        ac_order=ac_order,
        key_names=("scenario", "freq_band"),
    )
    write_summary_csv(
        os.path.join(out_base_a, "mlo"),
        {k: v for k, v in mlo_per_ac.items() if k[0] == "per_ac"},
        ac_order=ac_order,
        key_names=("scenario", "freq_pair"),
    )

    write_summary_csv(os.path.join(out_base_b, "slo"), slo_sum_ac, ac_order=ac_order, key_names=("scenario", "freq_band"))
    write_summary_csv(os.path.join(out_base_b, "mlo"), mlo_sum_ac, ac_order=ac_order, key_names=("scenario", "freq_pair"))

    print("Aggregation complete. Outputs written to:")
    print(" -", out_base_a)
    print(" -", out_base_b)


if __name__ == "__main__":
    main()
