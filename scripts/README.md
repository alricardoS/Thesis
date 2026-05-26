Aggregation helper for multi-STA priority suite

Usage

Run the aggregator after your suite completed to create two folders under the suite results root:

```bash
python3 scripts/aggregate_multi_sta_priority.py --results-root /home/ricardosantos/ns-3.47/results_multi_sta_priority_suite_base
```

Outputs
- `aggregated_per_ac/` — representative STA (default `STA0`) metrics per AC (slo/mlo)
- `aggregated_sum_all_acs/` — summed-throughput and averaged delay/jitter per AC

Notes
- The script expects each AC experiment folder to contain a `tables/` subfolder with
  `single_link_per_sta_detailed.csv` and/or `mlo_per_sta_detailed.csv`.
- Plotting requires `matplotlib` and `numpy` installed; otherwise only CSVs are produced.
