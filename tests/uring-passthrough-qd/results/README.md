# Result-directory contract

Nothing is recorded in this repository. A run writes everything under the
`--result-dir` the operator chooses; this file only says what a completed one
has to contain.

A completed run should be retained in a uniquely named directory only after
the runner prints `CAMPAIGN_OK` and the outer transaction has restored the
target.

Each retained campaign contains:

- `config.txt` and `prepare.txt` for fixed geometry and untimed preparation;
- `results.tsv` for the 32-cell default matrix (or the selected static subset);
- `cells/` with one directory per exact `rep/QD/arm` measurement;
- `summary.txt` and `summary.json` with samples, medians, and passthrough versus
  ext4 percentages;
- `final-readback.txt` from the campaign's one complete correctness pass;
- `MANIFEST.sha256` only when the runner owns `--manifest-mode self`.

Do not publish a partial directory as a benchmark result.  The important
per-cell proof is not the directory name: it is the validated JSON showing the
requested QD, full submitted batch, exact command/completion counts, IOPOLL,
and the correct opcode/backend pair.
