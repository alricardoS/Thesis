#!/usr/bin/env python3
"""
Generate link traffic distribution table by priority class.

Analyzes link_activity and correlates with queue_occupancy to show
how traffic is distributed across links (2.4GHz, 5GHz, 6GHz) per AC.
"""

import pandas as pd
import sys
from pathlib import Path


def _normalize_traffic_type(token):
    token = str(token).strip().lower()
    if token in {"voice", "vo", "ef", "3"}:
        return "VO"
    if token in {"video", "vi", "af41", "2"}:
        return "VI"
    if token in {"besteffort", "best-effort", "be", "1"}:
        return "BE"
    if token in {"background", "bk", "0"}:
        return "BK"
    return "BE"


def _parse_sta_traffic_types(sta_traffic_types_csv):
    if not sta_traffic_types_csv:
        return []
    return [_normalize_traffic_type(token) for token in str(sta_traffic_types_csv).split(",") if token.strip()]


def _detect_switch_info(run_df):
    """Deteta switches de AC a meio da simulação, distinguindo-os de apps concorrentes.

    Para cada STA, calcula a janela ativa [first, last] (buckets com frame_count>0) de cada
    AC. Há **switch** sse dois ACs tiverem janelas ~disjuntas no tempo (um sai quando o outro
    entra); se as janelas se sobrepõem (ex.: 'vo+be' concorrente), NÃO é switch — evita o
    falso split causado pelo cruzamento do AC dominante durante o ramp-up.

    Devolve dict: sta_id -> {"switch_t", "before_ac", "after_ac"}."""
    ts = sorted(run_df["time_bucket_s"].dropna().unique())
    dt = 1.0
    if len(ts) >= 2:
        diffs = sorted(ts[i + 1] - ts[i] for i in range(len(ts) - 1))
        dt = float(diffs[len(diffs) // 2])
    info = {}
    for sta_id, sdf in run_df.groupby("sta_id"):
        if sdf["ac"].nunique() < 2:
            continue
        windows = {}  # ac -> (first, last) dos buckets ativos
        for ac, g in sdf.groupby("ac"):
            act = g[g["frame_count"] > 0]["time_bucket_s"]
            if not act.empty:
                windows[str(ac)] = (float(act.min()), float(act.max()))
        if len(windows) < 2:
            continue
        ordered = sorted(windows.items(), key=lambda kv: kv[1][0])
        found = None
        for i in range(len(ordered)):
            for j in range(len(ordered)):
                if i == j:
                    continue
                a_ac, (a_first, a_last) = ordered[i]
                b_ac, (b_first, b_last) = ordered[j]
                if a_first < b_first and a_last <= b_first + dt:  # janelas ~disjuntas
                    found = (a_ac, b_ac, b_first)
                    break
            if found:
                break
        if not found:
            continue  # janelas sobrepostas → apps concorrentes, não switch
        before_ac, after_ac, after_first = found
        info[sta_id] = {
            "switch_t": after_first - dt / 2.0,
            "before_ac": before_ac,
            "after_ac": after_ac,
        }
    return info


def _write_dist_table(handle, sub_df, title):
    """Escreve uma tabela de distribuição (STA×AC×Link) para o sub-dataframe dado."""
    if title:
        handle.write(f"[{title}]\n")
    if sub_df.empty:
        handle.write("(no data)\n\n")
        return
    rows = []
    run_total_packets = sub_df["frame_count"].sum()
    for (sta_id, ac, link_name), group in sub_df.groupby(["sta_id", "ac", "link_name"]):
        packets = int(group["frame_count"].sum())
        bytes_count = int(group["bytes"].sum())
        ac_total_packets = int(sub_df[sub_df["ac"] == ac]["frame_count"].sum())
        sta_total_packets = int(sub_df[sub_df["sta_id"] == sta_id]["frame_count"].sum())
        pct_sta_ac = (100.0 * packets / ac_total_packets) if ac_total_packets > 0 else 0.0
        pct_sta_total = (100.0 * packets / sta_total_packets) if sta_total_packets > 0 else 0.0
        pct_run = (100.0 * packets / run_total_packets) if run_total_packets > 0 else 0.0
        rows.append({
            "STA": int(sta_id), "AC": ac, "Link": link_name,
            "FrameType": ",".join(sorted(group["frame_type"].astype(str).unique())),
            "Packets": packets, "Bytes": bytes_count,
            "Pct_Sta_AC": f"{pct_sta_ac:.1f}%",
            "Pct_Sta_Total": f"{pct_sta_total:.1f}%",
            "Pct_Run": f"{pct_run:.1f}%",
            "Avg_Bytes_Per_Pkt": f"{(bytes_count / packets) if packets > 0 else 0.0:.0f}",
        })
    if rows:
        out_df = pd.DataFrame(rows).sort_values(["STA", "AC", "Link"])
        handle.write(out_df.to_string(index=False))
        handle.write("\n\n")
    else:
        handle.write("(no data)\n\n")


def generate_link_traffic_distribution_table(outputs_dir, scenario_name, table_output_file, sta_traffic_types_csv=""):
    """Generate an AP-to-STA traffic table per link and access category.

    Prefer the new MLO CSV written by the priority test:
    - mlo_multi_sta_link_traffic_*.csv

    Fallback to the legacy link_activity/queue_occupancy heuristic if the new CSV
    is unavailable.
    """

    outputs_path = Path(outputs_dir)
    traffic_files = sorted(outputs_path.glob("mlo_multi_sta_link_traffic_*.csv"))

    if traffic_files:
        try:
            traffic_df = pd.concat([pd.read_csv(path) for path in traffic_files], ignore_index=True)
        except Exception as exc:
            print(f"Error loading traffic CSVs: {exc}")
            return False

        required = {"run_label", "pair", "protocol", "link_name", "sta_id", "ac", "frame_type", "frame_count", "bytes"}
        missing = required - set(traffic_df.columns)
        if missing:
            print(f"Error: missing columns in traffic CSV: {', '.join(sorted(missing))}")
            return False

        traffic_df["frame_count"] = pd.to_numeric(traffic_df["frame_count"], errors="coerce").fillna(0)
        traffic_df["bytes"] = pd.to_numeric(traffic_df["bytes"], errors="coerce").fillna(0)
        traffic_df["sta_id"] = pd.to_numeric(traffic_df["sta_id"], errors="coerce").fillna(-1).astype(int)

        with open(table_output_file, "w") as handle:
            handle.write("=" * 120 + "\n")
            handle.write(f"MLO Link Traffic Distribution - {scenario_name}\n")
            handle.write("=" * 120 + "\n\n")
            handle.write(f"Scenario: {scenario_name}\n")
            handle.write("Metric: AP-transmitted data frames grouped by STA, AC and link\n\n")

            has_bucket = "time_bucket_s" in traffic_df.columns
            if has_bucket:
                traffic_df["time_bucket_s"] = pd.to_numeric(
                    traffic_df["time_bucket_s"], errors="coerce").fillna(0.0)

            for run_label in sorted(traffic_df["run_label"].unique()):
                run_df = traffic_df[traffic_df["run_label"] == run_label].copy()
                if run_df.empty:
                    continue

                handle.write(f"Run: {run_label}\n")
                handle.write("-" * 120 + "\n")

                # Cenários de switch: dividir em fases (antes/depois da mudança de AC).
                switch_info = _detect_switch_info(run_df) if has_bucket else {}
                if switch_info:
                    switch_t = min(v["switch_t"] for v in switch_info.values())
                    # Em cada fase, esconder o AC que não pertence a essa fase (para os
                    # STAs que trocam): FASE 2 sem o AC "antes" (ex.: VO residual), FASE 1
                    # sem o AC "depois". STAs sem switch não são filtrados.
                    def _phase(df, drop_key):  # drop_key: "before_ac" ou "after_ac"
                        mask = pd.Series(True, index=df.index)
                        for sid, si in switch_info.items():
                            mask &= ~((df["sta_id"] == sid) & (df["ac"].astype(str) == si[drop_key]))
                        return df[mask]
                    f1 = _phase(run_df[run_df["time_bucket_s"] < switch_t], "after_ac")
                    f2 = _phase(run_df[run_df["time_bucket_s"] >= switch_t], "before_ac")
                    _write_dist_table(handle, f1,
                                      f"FASE 1 (antes do switch, t < {switch_t:g}s)")
                    _write_dist_table(handle, f2,
                                      f"FASE 2 (após o switch, t >= {switch_t:g}s)")
                else:
                    _write_dist_table(handle, run_df, None)

            handle.write("Legend:\n")
            handle.write("- STA: destination station index\n")
            handle.write("- AC: access category of the transmitted frame\n")
            handle.write("- Link: physical link used by the AP for the transmission\n")
            handle.write("- FrameType: unique frame types seen in the bucket\n")
            handle.write("- Pct_Sta_AC: share of this row within all traffic of the same AC in the run\n")
            handle.write("- Pct_Sta_Total: share of this row within all traffic destined to this STA in the run\n")
            handle.write("- Pct_Run: share of all packets in the run carried by this row\n")
            handle.write("- Avg_Bytes_Per_Pkt: average packet size for the row\n")

        print(f"✓ Generated: {table_output_file}")
        return True

    link_activity_files = list(outputs_path.glob("mlo_multi_sta_link_activity_*.csv"))
    queue_occupancy_file = outputs_path / "mlo_multi_sta_queue_occupancy.csv"
    if not link_activity_files or not queue_occupancy_file.exists():
        print(f"Warning: No link traffic CSV found in {outputs_dir}")
        return False

    try:
        link_dfs = [pd.read_csv(path) for path in sorted(link_activity_files)]
        link_df_all = pd.concat(link_dfs, ignore_index=True)
        queue_df = pd.read_csv(queue_occupancy_file)
    except Exception as exc:
        print(f"Error loading CSVs: {exc}")
        return False

    if "role" in queue_df.columns:
        queue_df_ap = queue_df[queue_df["role"] == "AP"].copy()
    else:
        queue_df_ap = queue_df.copy()

    if queue_df_ap.empty:
        print(f"Warning: No AP data in queue_occupancy for {outputs_dir}")
        return False

    queue_df_ap["packets"] = pd.to_numeric(queue_df_ap["packets"], errors="coerce").fillna(0)
    queue_df_ap["bytes"] = pd.to_numeric(queue_df_ap["bytes"], errors="coerce").fillna(0)

    output_sections = {}

    def map_run_pair_to_link_pair(run_pair):
        parts = run_pair.split("+")
        mapped = []
        for part in parts:
            mapped.append("2.4" if part == "2" else part)
        return "+".join(mapped)

    ac_to_sta = {}
    for sta_idx, traffic_type in enumerate(_parse_sta_traffic_types(sta_traffic_types_csv)):
        ac_to_sta.setdefault(traffic_type, sta_idx)

    for run_label in sorted(queue_df_ap["run_label"].unique()):
        run_queue = queue_df_ap[queue_df_ap["run_label"] == run_label]
        if run_queue.empty:
            continue

        if "|" in run_label:
            run_pair, _ = run_label.split("|", 1)
        else:
            run_pair = run_label

        link_pair = map_run_pair_to_link_pair(run_pair)
        link_rows = link_df_all[link_df_all["pair"] == link_pair]
        if link_rows.empty:
            link_rows = link_df_all[link_df_all["pair"].astype(str).str.contains(run_pair.replace("+", "\\+"), regex=True)]

        ac_dist = run_queue.groupby("ac", as_index=False).agg({"packets": "sum", "bytes": "sum"}).sort_values("ac")
        ac_total_packets = ac_dist["packets"].sum()
        ac_total_bytes = ac_dist["bytes"].sum()

        if ac_total_packets == 0:
            output_sections[run_label] = []
            continue

        rows = []
        for _, link_row in link_rows.iterrows():
            link_id = int(link_row["link_id"])
            freqs = link_row.get("pair", "").split("+") if "pair" in link_row else []
            if len(freqs) > link_id and freqs[link_id] is not None:
                link_name = f"{freqs[link_id]}GHz"
            else:
                link_name = f"Link{link_id}"
            link_packets = int(link_row.get("su_tx_count", 0))
            avg_pkt_size = ac_total_bytes / ac_total_packets if ac_total_packets > 0 else 50
            link_bytes = int(link_packets * avg_pkt_size)

            for _, ac_row in ac_dist.iterrows():
                ac = ac_row["ac"]
                ac_packets = int(ac_row["packets"])
                ac_bytes = int(ac_row["bytes"])
                pct_packets = ac_packets / ac_total_packets if ac_total_packets > 0 else 0
                pct_bytes = ac_bytes / ac_total_bytes if ac_total_bytes > 0 else 0
                link_pkt_for_ac = int(link_packets * pct_packets)
                link_bytes_for_ac = int(link_bytes * pct_bytes) if pct_bytes > 0 else 0
                rows.append(
                    {
                        "STA": ac_to_sta.get(ac, "All"),
                        "AC": ac,
                        "Link": link_name,
                        "Packets": link_pkt_for_ac,
                        "Pct_Packets": f"{pct_packets * 100:.1f}%",
                        "Bytes": link_bytes_for_ac,
                        "Pct_Bytes": f"{pct_bytes * 100:.1f}%",
                        "Avg_Bytes_Per_Pkt": f"{link_bytes_for_ac / max(link_pkt_for_ac, 1):.0f}",
                    }
                )

        output_sections[run_label] = rows

    try:
        with open(table_output_file, "w") as handle:
            handle.write("=" * 120 + "\n")
            handle.write(f"MLO Link Traffic Distribution - {scenario_name}\n")
            handle.write("=" * 120 + "\n\n")
            handle.write(f"Scenario: {scenario_name}\n")
            handle.write("Role filtered: AP only\n\n")
            handle.write("Traffic Distribution by Priority Class and Link (per experiment):\n")
            handle.write("-" * 120 + "\n")

            first = True
            for run_label in sorted(output_sections.keys()):
                rows = output_sections[run_label]
                if not first:
                    handle.write("\n")
                first = False
                handle.write(f"Experience: {run_label}\n")
                handle.write("-" * 120 + "\n")
                if rows:
                    out_df = pd.DataFrame(rows)
                    handle.write(out_df.to_string(index=False))
                    handle.write("\n")
                else:
                    handle.write("(no data)\n")

            handle.write("\nLegend:\n")
            handle.write("- STA: Source station index inferred from the traffic type mapping\n")
            handle.write("- AC: Access Category (VO=Voice, VI=Video, BE=Best Effort, BK=Background)\n")
            handle.write("- Link: Frequency band\n")
            handle.write("- Packets: Estimated number of packets transmitted on this link for this AC\n")
            handle.write("- Pct_Packets: Percentage of total packets for this AC\n")
            handle.write("- Bytes: Total bytes transmitted\n")
            handle.write("- Pct_Bytes: Percentage of total bytes for this AC\n")
            handle.write("- Avg_Bytes_Per_Pkt: Average packet size for this AC on this link\n")

        print(f"✓ Generated: {table_output_file}")
        return True

    except Exception as exc:
        print(f"Error writing table: {exc}")
        return False


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 generate_link_traffic_distribution_table.py <outputs_dir> [scenario_name] [output_file] [sta_traffic_types_csv]")
        sys.exit(1)
    
    outputs_dir = sys.argv[1]
    scenario_name = sys.argv[2] if len(sys.argv) > 2 else Path(outputs_dir).parent.name
    output_file = sys.argv[3] if len(sys.argv) > 3 else "link_traffic_distribution_table.txt"
    sta_traffic_types_csv = sys.argv[4] if len(sys.argv) > 4 else ""
    
    success = generate_link_traffic_distribution_table(outputs_dir, scenario_name, output_file, sta_traffic_types_csv)
    sys.exit(0 if success else 1)


if __name__ == "__main__":
    main()
