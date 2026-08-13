/**
 * Faraday board widget — the MCP App.
 *
 * A list of findings in text is usable; a board with the findings ON it is what makes an FAE
 * trust the tool (ABT #665). So this is a packaging exercise, not new rendering code: it
 * mounts the web app's own BoardView over the same report JSON the CLI writes, and adds the
 * one thing a chat needs that the browser app does not — reporting the engineer's chosen
 * finding back to the model, so the next question can be about that finding.
 *
 * Importing BoardView rather than redrawing means the board in a chat and the board in the
 * browser cannot disagree about what the copper looks like.
 */
import { createApp, defineComponent, h, ref, computed } from "vue";
import { App } from "@modelcontextprotocol/ext-apps";
import BoardView from "../../web/src/components/BoardView.vue";

const app = new App({ name: "Faraday board", version: "0.1.0" });

const SEVERITY_ORDER = { high: 0, medium: 1, low: 2, info: 3 };

const Widget = defineComponent({
  setup() {
    const report = ref(null);
    const selectedId = ref("");
    const error = ref("");
    const counts = ref({});
    const review = ref("");

    // BoardView wants the findings it should draw; the server has already applied whatever
    // filter the tool call implied, so the board shows exactly what the answer listed.
    const findings = computed(() => report.value?.findings ?? []);
    const ordered = computed(() => [...findings.value].sort(
      (a, b) => (SEVERITY_ORDER[a.severityLabel] ?? 9) - (SEVERITY_ORDER[b.severityLabel] ?? 9)
      || (b.severity ?? 0) - (a.severity ?? 0)));

    /** Report the engineer's choice to the model. */
    async function choose(id) {
      selectedId.value = id;
      const f = findings.value.find((x) => x.id === id);
      if (!f) return;
      const where = [f.netA, f.netB].filter(Boolean).join(" <-> ");
      // updateModelContext OVERWRITES, so the message restates what was being looked at —
      // otherwise the model gets a finding id with no board and no question behind it.
      await app.updateModelContext({
        content: [{
          type: "text",
          text: [
            `[user selected] ${f.id} — ${f.rule}, severity ${f.severityLabel}.`,
            `[what] ${f.title}`,
            where ? `[nets] ${where}` : null,
            f.remediation ? `[remediation as given] ${f.remediation}` : null,
            `[review] ${review.value}`,
          ].filter(Boolean).join("\n"),
        }],
        structuredContent: JSON.parse(JSON.stringify({
          selected: {
            id: f.id, rule: f.rule, severity: f.severityLabel, title: f.title,
            netA: f.netA ?? null, netB: f.netB ?? null, confidence: f.confidence ?? null,
          },
          context: { review: review.value },
        })),
      });
    }

    app.ontoolresult = async (result) => {
      const sc = result?.structuredContent;
      if (!sc?.report) {
        error.value = "The tool returned no board for this widget.";
        return;
      }
      report.value = sc.report;
      counts.value = sc.counts ?? {};
      review.value = sc.review ?? "";
      selectedId.value = sc.finding?.id ?? "";
    };

    return () => {
      if (error.value) return h("div", { class: "err" }, error.value);
      if (!report.value) return h("div", { class: "muted pad" }, "Waiting for a board…");

      const tally = Object.entries(counts.value)
        .filter(([, n]) => n)
        .map(([s, n]) => h("span", { class: `chip ${s}` }, `${n} ${s}`));

      return h("div", { class: "wrap" }, [
        h("div", { class: "head" }, [
          h("h1", {}, `${findings.value.length} finding${findings.value.length === 1 ? "" : "s"}`),
          h("div", { class: "chips" }, tally),
          h("div", { class: "sub" },
            "Click a finding on the board or in the list — your choice goes back to the assistant."),
        ]),
        h("div", { class: "split" }, [
          h("div", { class: "boardpane" }, [
            h(BoardView, {
              report: report.value,
              findings: ordered.value,
              selectedId: selectedId.value,
              onSelect: (id) => choose(id),
            }),
          ]),
          h("ul", { class: "list" }, ordered.value.slice(0, 60).map((f) =>
            h("li", {
              class: `item ${f.severityLabel}${f.id === selectedId.value ? " chosen" : ""}`,
              onClick: () => choose(f.id),
            }, [
              h("div", { class: "itemhead" }, [
                h("span", { class: "fid" }, f.id),
                h("span", { class: `sev ${f.severityLabel}` }, f.severityLabel),
                h("span", { class: "rule" }, f.rule),
              ]),
              h("div", { class: "title" }, f.title),
              [f.netA, f.netB].filter(Boolean).length
                ? h("div", { class: "nets" }, [f.netA, f.netB].filter(Boolean).join(" ↔ "))
                : null,
            ]))),
        ]),
      ]);
    };
  },
});

createApp(Widget).mount("#app");
await app.connect();
