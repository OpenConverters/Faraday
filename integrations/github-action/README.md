# Faraday CI action

Screens a board on every push and fails the job when findings reach a chosen
severity. Uses `faraday_cli --fail-on`, exit code 3 on violation.

```yaml
jobs:
  emc:
    runs-on: ubuntu-latest
    steps:
      - uses: actions/checkout@v4
      - uses: OpenConverters/Faraday/integrations/github-action@main
        with:
          board: hardware/main.kicad_pcb
          stackup: default-4layer     # only used if the board has no stackup
          fail-on: high
      - uses: actions/upload-artifact@v4
        if: always()
        with: { name: faraday-report, path: faraday-report.json }
```

The action builds the CLI from source (cached), so the first run takes a few
minutes and later runs seconds. Note the action path must be able to see
`cpp/` — use it from this repository, or vendor the repo.
